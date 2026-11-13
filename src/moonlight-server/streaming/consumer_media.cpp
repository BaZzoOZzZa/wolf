#include "consumer_media.hpp"
#include <core/gstreamer.hpp>
#include <fmt/format.h>

namespace streaming::detail {
namespace {
// Adopt every transfer-full reference before any C++ allocation/callback/string
// work. C functions below receive borrowed pointers; these guards own the refs.
template <class T> using GstOwned = std::unique_ptr<T, decltype(&gst_object_unref)>;
void checkpoint(const std::shared_ptr<const ConsumerMediaProbe> &probe,
                MediaCallback kind,
                MediaPhase phase,
                GstMessage *message = nullptr) noexcept {
  if (probe && probe->checkpoint)
    probe->checkpoint(kind, phase, message);
}
void before_allocation(const std::shared_ptr<const ConsumerMediaProbe> &probe,
                       MediaAllocation point,
                       GstObject *object) {
  if (probe && probe->before_allocation)
    probe->before_allocation(point, object);
}
template <class Callback> struct CallbackData {
  Callback callback;
  std::shared_ptr<ConsumerCallbackBarrier> barrier;
  std::shared_ptr<const ConsumerMediaProbe> probe;
  MediaCallback kind;
};
using SinkData = CallbackData<std::function<GstFlowReturn(GstAppSink *)>>;
using ContextData = CallbackData<std::function<GstBusSyncReply(GstBus *, GstMessage *)>>;
template <class Data> void retire(gpointer raw) noexcept {
  auto data = static_cast<Data *>(raw);
  auto barrier = data->barrier;
  checkpoint(data->probe, data->kind, MediaPhase::Retiring);
  delete data; // release callback captures and free the owned holder FIRST
  barrier->retire_holder();
}
void configure_appsink(GstElement *sink,
                       const ConsumerMediaCallbacks &callbacks,
                       const std::shared_ptr<ConsumerCallbackBarrier> &barrier,
                       const std::shared_ptr<const ConsumerMediaProbe> &probe) {
  g_object_set(sink, "emit-signals", FALSE, "buffer-list", TRUE, nullptr);
  GstAppSinkCallbacks gst_callbacks = {};
  gst_callbacks.new_sample = [](GstAppSink *sink, gpointer raw) -> GstFlowReturn {
    auto &data = *static_cast<SinkData *>(raw);
    checkpoint(data.probe, MediaCallback::Sink, MediaPhase::BeforeEntry);
    GstFlowReturn result = GST_FLOW_FLUSHING;
    {
      auto lease = data.barrier->enter();
      if (lease) {
        try {
          result = data.callback(sink);
        } catch (...) {
          result = GST_FLOW_ERROR;
        }
      }
    }
    checkpoint(data.probe, MediaCallback::Sink, MediaPhase::AfterEntry);
    return result;
  };
  before_allocation(probe, MediaAllocation::SinkHolder, GST_OBJECT(sink));
  auto data = std::make_unique<SinkData>(SinkData{callbacks.sample, barrier, probe, MediaCallback::Sink});
  barrier->retain_holder();
  gst_app_sink_set_callbacks(GST_APP_SINK(sink), &gst_callbacks, data.release(), retire<SinkData>);
}
void configure_context(GstBus *bus,
                       const ConsumerMediaCallbacks &callbacks,
                       const std::shared_ptr<ConsumerCallbackBarrier> &barrier,
                       const std::shared_ptr<const ConsumerMediaProbe> &probe) {
  before_allocation(probe, MediaAllocation::ContextHolder, GST_OBJECT(bus));
  auto data = std::make_unique<ContextData>(ContextData{callbacks.context, barrier, probe, MediaCallback::Context});
  barrier->retain_holder();
  gst_bus_set_sync_handler(
      bus,
      [](GstBus *bus, GstMessage *message, gpointer raw) -> GstBusSyncReply {
        auto &data = *static_cast<ContextData *>(raw);
        checkpoint(data.probe, MediaCallback::Context, MediaPhase::BeforeEntry, message);
        GstBusSyncReply result = GST_BUS_PASS;
        {
          auto lease = data.barrier->enter();
          if (lease) {
            try {
              result = data.callback(bus, message);
            } catch (...) {
              result = GST_BUS_PASS;
            }
          }
        }
        checkpoint(data.probe, MediaCallback::Context, MediaPhase::AfterEntry, message);
        return result;
      },
      data.release(),
      retire<ContextData>);
}
} // namespace

bool run_media_consumer(const std::string &description,
                        ConsumerJob &job,
                        std::size_t session_id,
                        const RunScope &producer,
                        const ConsumerMediaCallbacks &callbacks,
                        const std::shared_ptr<const ConsumerMediaProbe> &probe) {
  const std::string kind = job.kind == ConsumerKind::Video ? "video" : "audio";
  const auto src_name = fmt::format("interpipesrc_{}_{}", session_id, kind);
  auto barrier = std::make_shared<ConsumerCallbackBarrier>();
  wolf::core::sessions::ProducerBinding binding(producer);
  auto select = [&](GstElement *pipeline, const RunScope &selected) {
    binding.select(selected, [&](const std::string &key) {
      if (GstOwned<GstElement> src{gst_bin_get_by_name(GST_BIN(pipeline), src_name.c_str()), gst_object_unref}) {
        before_allocation(probe, MediaAllocation::ProducerTarget, GST_OBJECT(src.get()));
        auto target = key + "_" + kind;
        g_object_set(src.get(), "listen-to", target.c_str(), nullptr);
        return true;
      }
      return false;
    });
  };
  return run_consumer_pipeline(
      description,
      job,
      {.setup =
           [&](GstElement *pipeline) {
             if (GstOwned<GstElement> sink{gst_bin_get_by_name(GST_BIN(pipeline), "wolf_udp_sink"), gst_object_unref}) {
               if (!GST_IS_APP_SINK(sink.get())) {
                 throw std::runtime_error("wolf_udp_sink is not an appsink");
               }
               configure_appsink(sink.get(), callbacks, barrier, probe);
             }
             if (callbacks.context) {
               GstOwned<GstBus> bus{gst_element_get_bus(pipeline), gst_object_unref};
               configure_context(bus.get(), callbacks, barrier, probe);
             }
             select(pipeline, producer);
             if (probe && probe->ready)
               probe->ready(pipeline);
           },
       .commands =
           [&](GstElement *pipeline, ConsumerCommands commands) {
             if (commands.producer)
               select(pipeline, *commands.producer);
             if (commands.idr && job.kind == ConsumerKind::Video)
               wolf::core::gstreamer::send_message(
                   pipeline,
                   gst_structure_new("GstForceKeyUnit", "all-headers", G_TYPE_BOOLEAN, TRUE, nullptr));
           },
       .drain =
           [&](GstElement *pipeline) {
             if (GstOwned<GstElement> sink{gst_bin_get_by_name(GST_BIN(pipeline), "wolf_udp_sink"), gst_object_unref}) {
               if (GST_IS_APP_SINK(sink.get())) {
                 GstAppSinkCallbacks empty = {};
                 gst_app_sink_set_callbacks(GST_APP_SINK(sink.get()), &empty, nullptr, nullptr);
               }
             }
             if (probe && probe->draining)
               probe->draining();
             barrier->close_and_wait();
           }});
}
} // namespace streaming::detail
