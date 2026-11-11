#include "consumer_events.hpp"
#include <immer/vector_transient.hpp>

namespace streaming {
using namespace wolf::core;
immer::vector<immer::box<events::EventBusHandlers>>
consumer_event_handlers(const std::shared_ptr<events::EventBusType> &bus,
                        const std::shared_ptr<ConsumerRuntime> &runtime) {
  immer::vector_transient<immer::box<events::EventBusHandlers>> handlers;
  handlers.push_back(bus->register_handler<immer::box<events::PauseStreamEvent>>([runtime](const auto &event) {
    if (sessions::targets(event->connection_scope.get(), event->connection_scope.get(), event->session_id))
      runtime->revoke(event->connection_scope.get());
  }));
  handlers.push_back(bus->register_handler<immer::box<events::StopStreamEvent>>([runtime](const auto &event) {
    if (sessions::targets(event->connection_scope.get(), event->connection_scope.get(), event->session_id))
      runtime->revoke(event->connection_scope.get());
  }));
  handlers.push_back(bus->register_handler<immer::box<events::RTPVideoPingEvent>>([runtime](const auto &event) {
    runtime->ping(ConsumerKind::Video,
                  {event->client_ip, event->client_port, event->payload, event->video_socket.get()});
  }));
  handlers.push_back(bus->register_handler<immer::box<events::RTPAudioPingEvent>>([runtime](const auto &event) {
    runtime->ping(ConsumerKind::Audio,
                  {event->client_ip, event->client_port, event->payload, event->audio_socket.get()});
  }));
  handlers.push_back(bus->register_handler<immer::box<events::IDRRequestEvent>>([runtime](const auto &event) {
    if (sessions::targets(event->connection_scope.get(), event->connection_scope.get(), event->session_id))
      runtime->command(event->connection_scope.get(), ConsumerKind::Video, {.idr = true});
  }));
  handlers.push_back(bus->register_handler<immer::box<events::SwitchStreamProducerEvents>>([runtime](
                                                                                               const auto &event) {
    if (sessions::resolve_producer(event->connection_scope.get(),
                                   event->connection_scope.get(),
                                   event->session_id,
                                   event->producer_scope.get(),
                                   event->interpipe_src_id)) {
      runtime->command(event->connection_scope.get(), ConsumerKind::Video, {.producer = event->producer_scope.get()});
      runtime->command(event->connection_scope.get(), ConsumerKind::Audio, {.producer = event->producer_scope.get()});
    }
  }));
  return handlers.persistent();
}
} // namespace streaming
