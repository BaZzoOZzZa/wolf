#pragma once
#include "consumer_pipeline.hpp"
#include <gst/app/gstappsink.h>

namespace streaming::detail {
// Internal adapter used by both real consumers. Functions own their captured
// UDP/context resources; Gst holders copy that ownership until retirement.
struct ConsumerMediaCallbacks {
  std::function<GstFlowReturn(GstAppSink *)> sample;
  std::function<GstBusSyncReply(GstBus *, GstMessage *)> context;
};
enum class MediaAllocation {
  SinkHolder,
  ContextHolder,
  ProducerTarget
};
enum class MediaCallback {
  Sink,
  Context
};
enum class MediaPhase {
  BeforeEntry,
  AfterEntry,
  Retiring
};
// Narrow deterministic probes, absent in production. Observe borrowed objects;
// only before_allocation may throw. Other hooks must not throw and must be
// released by the fixture even if an assertion fails. Never replace Gst types.
struct ConsumerMediaProbe {
  std::function<void(MediaAllocation, GstObject *)> before_allocation;
  std::function<void(MediaCallback, MediaPhase, GstMessage *)> checkpoint;
  std::function<void(GstElement *)> ready;
  std::function<void()> draining;
};
bool run_media_consumer(const std::string &description,
                        ConsumerJob &job,
                        std::size_t session_id,
                        const RunScope &producer,
                        const ConsumerMediaCallbacks &callbacks,
                        const std::shared_ptr<const ConsumerMediaProbe> &probe = {});
} // namespace streaming::detail
