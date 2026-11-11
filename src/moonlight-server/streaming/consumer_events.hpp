#pragma once
#include "consumer_runtime.hpp"
#include <events/events.hpp>

namespace streaming {
// Own these registrations for the server lifetime. Destroy them on the owner
// shutdown thread, not from a synchronous event callback (Runtime joins there).
immer::vector<immer::box<wolf::core::events::EventBusHandlers>>
consumer_event_handlers(const std::shared_ptr<wolf::core::events::EventBusType> &bus,
                        const std::shared_ptr<ConsumerRuntime> &runtime);

template <class Session>
ConsumerAdmission
admit_consumer(ConsumerRuntime &runtime, const Session &request, ConsumerKind kind, ConsumerRuntime::Work work) {
  if (!request.connection_scope.get() || request.connection_scope.get().id() != request.session_id)
    return ConsumerAdmission::Stale;
  return runtime.admit(request.connection_scope.get(),
                       request.run_scope.get(),
                       kind,
                       ConsumerCorrelation{request.client_ip, request.port, request.rtp_secret_payload},
                       std::move(work));
}
} // namespace streaming
