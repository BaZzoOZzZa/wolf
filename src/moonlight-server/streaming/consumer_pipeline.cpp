#include "consumer_pipeline.hpp"
#include <memory>

namespace streaming {
namespace {
struct Context {
  GMainContext *context = g_main_context_new();
  Context() {
    g_main_context_push_thread_default(context);
  }
  ~Context() {
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
  }
};
struct LoopState {
  GMainLoop *loop;
  GstElement *pipeline;
  ConsumerJob &job;
  const ConsumerPipelineHooks &hooks;
  bool failed = false;
};
gboolean on_bus(GstBus *, GstMessage *message, gpointer data) {
  auto &state = *static_cast<LoopState *>(data);
  if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR || GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
    state.failed |= GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR;
    state.job.begin_drain();
    g_main_loop_quit(state.loop);
  }
  return G_SOURCE_CONTINUE;
}
gboolean on_tick(gpointer data) {
  auto &state = *static_cast<LoopState *>(data);
  if (state.job.cancelled()) {
    g_main_loop_quit(state.loop);
    return G_SOURCE_REMOVE;
  }
  try {
    auto commands = state.job.take_commands();
    if (state.hooks.commands)
      state.hooks.commands(state.pipeline, std::move(commands));
  } catch (...) {
    state.failed = true;
    state.job.begin_drain();
    g_main_loop_quit(state.loop);
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}
} // namespace
bool run_consumer_pipeline(const std::string &description, ConsumerJob &job, const ConsumerPipelineHooks &hooks) {
  if (job.cancelled())
    return true;
  Context context;
  GError *error = nullptr;
  std::unique_ptr<GstElement, decltype(&gst_object_unref)> pipeline(gst_parse_launch(description.c_str(), &error),
                                                                    gst_object_unref);
  const bool parse_failed = error != nullptr || !pipeline;
  if (error)
    g_error_free(error);
  if (!pipeline || !GST_IS_PIPELINE(pipeline.get()))
    return false;
  std::unique_ptr<GMainLoop, decltype(&g_main_loop_unref)> loop(g_main_loop_new(context.context, FALSE),
                                                                g_main_loop_unref);
  std::unique_ptr<GstBus, decltype(&gst_object_unref)> bus(gst_element_get_bus(pipeline.get()), gst_object_unref);
  std::unique_ptr<GSource, decltype(&g_source_unref)> watch(gst_bus_create_watch(bus.get()), g_source_unref);
  std::unique_ptr<GSource, decltype(&g_source_unref)> tick(g_timeout_source_new(10), g_source_unref);
  LoopState state{loop.get(), pipeline.get(), job, hooks, parse_failed};
  g_source_set_callback(watch.get(), G_SOURCE_FUNC(on_bus), &state, nullptr);
  g_source_attach(watch.get(), context.context);
  g_source_set_callback(tick.get(), on_tick, &state, nullptr);
  g_source_attach(tick.get(), context.context);
  try {
    if (!parse_failed && !job.cancelled()) {
      if (hooks.setup)
        hooks.setup(pipeline.get());
      if (!job.cancelled()) {
        if (gst_element_set_state(pipeline.get(), GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
          state.failed = true;
        else
          g_main_loop_run(loop.get());
      }
    }
  } catch (...) {
    state.failed = true;
  }
  job.begin_drain();
  g_source_destroy(tick.get());
  g_source_destroy(watch.get());
  // NULL normally completes synchronously, but failure/ASYNC is not proof of
  // release. Keep the entire worker/resources quarantinable until confirmed.
  for (;;) {
    auto result = gst_element_set_state(pipeline.get(), GST_STATE_NULL);
    GstState actual = GST_STATE_VOID_PENDING;
    GstState pending = GST_STATE_VOID_PENDING;
    auto observed = gst_element_get_state(pipeline.get(), &actual, &pending, 0);
    if (result != GST_STATE_CHANGE_FAILURE && observed != GST_STATE_CHANGE_FAILURE && actual == GST_STATE_NULL &&
        pending == GST_STATE_VOID_PENDING)
      break;
    state.failed = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  gst_bus_set_sync_handler(bus.get(), nullptr, nullptr, nullptr);
  if (hooks.drain) {
    // A broken cleanup contract cannot escape to Runtime's exception catcher
    // and falsely publish Failed-with-release. Retain on failure as Unknown.
    for (;;) {
      try {
        hooks.drain(pipeline.get());
        break;
      } catch (...) {
        state.failed = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
  }
  // Local destructors release bus sources, pipeline and context before return;
  // the runtime additionally joins the worker before it publishes Stopped.
  return !state.failed;
}
} // namespace streaming
