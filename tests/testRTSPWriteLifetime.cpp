#include <boost/weak_ptr.hpp>
#include <catch2/catch_test_macros.hpp>
#include <rtsp/net.hpp>

namespace {
class Connection : public rtsp::tcp_connection {
public:
  explicit Connection(boost::asio::io_context &io) : tcp_connection(io, {}) {}
  using tcp_connection::send_message_with_writer;
};

// Only the async provider is replaced: borrow the buffer during initiation and
// retain the real production completion. Weak probes never extend its lifetime.
struct DeferredWrite {
  using Completion = std::function<void(boost::system::error_code, std::size_t)>;
  std::weak_ptr<const std::string> storage;
  boost::asio::const_buffer buffer;
  Completion completion;
  bool throw_before = false;
  bool throw_after = false;
  bool unopened = false;

  void operator()(rtsp::tcp::socket &socket, const std::shared_ptr<const std::string> &bytes, Completion done) {
    unopened = !socket.is_open();
    storage = bytes;
    buffer = boost::asio::buffer(*bytes);
    if (throw_before)
      throw std::runtime_error("initiation failed before ownership transfer");
    completion = std::move(done);
    if (throw_after)
      throw std::runtime_error("provider retained operation before throwing");
  }

  void finish(boost::system::error_code error, std::size_t bytes) {
    auto done = std::move(completion);
    done(error, bytes);
  }

  void check_bytes(const std::string &expected) {
    auto live = storage.lock();
    CHECK(bool(live));
    // RED must fail by expired ownership, never read freed memory.
    if (live) {
      CHECK(buffer.data() == live->data());
      CHECK(buffer.size() == live->size());
      CHECK(std::string(static_cast<const char *>(buffer.data()), buffer.size()) == expected);
    }
  }
};
} // namespace

TEST_CASE("RTSP deferred write owns exact serialized bytes and connection", "[rtsp-write]") {
  boost::asio::io_context io;
  DeferredWrite provider;
  auto connection = boost::shared_ptr<Connection>(new Connection(io));
  boost::weak_ptr<Connection> weak = connection;
  int calls = 0;
  int sent = -1;
  std::string expected;
  {
    auto response = rtsp::commands::ok_msg(73, {{"Session", "transport"}}, {{"a", std::string(4096, 'z')}});
    expected = rtsp::to_string(response);
    connection->send_message_with_writer(
        response,
        [&](int n) {
          ++calls;
          sent = n;
        },
        provider);
    response.response.msg = "changed";
    response.payloads.clear();
  }
  connection.reset();
  CHECK(provider.unopened);
  CHECK_FALSE(weak.expired());
  CHECK(calls == 0);
  provider.check_bytes(expected);
  provider.finish({}, expected.size());
  CHECK(calls == 1);
  CHECK(sent == static_cast<int>(expected.size()));
  CHECK(provider.storage.expired());
  CHECK(weak.expired());
  CHECK_FALSE(bool(provider.completion));
}

TEST_CASE("RTSP close cancellation and errors retain owners through callback retirement", "[rtsp-write]") {
  for (auto error : {boost::system::error_code(boost::asio::error::operation_aborted),
                     boost::system::error_code(boost::asio::error::connection_reset)}) {
    boost::asio::io_context io;
    DeferredWrite provider;
    auto connection = boost::shared_ptr<Connection>(new Connection(io));
    boost::weak_ptr<Connection> weak = connection;
    auto capture = std::make_shared<int>(17);
    std::weak_ptr<int> weak_capture = capture;
    bool callback_alive = false;
    int calls = 0;
    int sent = -1;
    auto response = rtsp::commands::error_msg(400, "BAD REQUEST", 19);
    const auto expected = rtsp::to_string(response);
    connection->send_message_with_writer(
        response,
        [capture, &callback_alive, &calls, &sent, &weak, &provider](int n) {
          callback_alive = !weak.expired() && !provider.storage.expired();
          ++calls;
          sent = n;
        },
        provider);
    capture.reset();
    connection->close(); // unopened socket; no network activity
    connection.reset();
    CHECK_FALSE(weak.expired());
    CHECK_FALSE(weak_capture.expired());
    provider.check_bytes(expected);
    provider.finish(error, 3);
    CHECK(callback_alive);
    CHECK(calls == 1); // Existing callback semantics include errors/partial bytes.
    CHECK(sent == 3);
    CHECK(weak.expired());
    CHECK(weak_capture.expired());
    CHECK(provider.storage.expired());
  }
}

TEST_CASE("RTSP scheduling rollback and abandoned provider release all owners", "[rtsp-write]") {
  for (bool retained : {false, true}) {
    boost::asio::io_context io;
    DeferredWrite provider;
    provider.throw_before = !retained;
    provider.throw_after = retained;
    auto connection = boost::shared_ptr<Connection>(new Connection(io));
    boost::weak_ptr<Connection> weak = connection;
    auto capture = std::make_shared<int>(1);
    std::weak_ptr<int> weak_capture = capture;
    int calls = 0;
    REQUIRE_THROWS_AS(connection->send_message_with_writer(
                          rtsp::commands::ok_msg(1),
                          [capture, &calls](int) { ++calls; },
                          provider),
                      std::runtime_error);
    capture.reset();
    connection.reset();
    CHECK(calls == 0);
    CHECK(weak.expired() == !retained);
    CHECK(provider.storage.expired() == !retained);
    CHECK(weak_capture.expired() == !retained);
    provider.completion = {}; // provider abandonment, like execution-context teardown
    CHECK(weak.expired());
    CHECK(provider.storage.expired());
    CHECK(weak_capture.expired());
    CHECK(calls == 0);
  }
}

TEST_CASE("RTSP throwing callback releases completion owners on unwind", "[rtsp-write]") {
  boost::asio::io_context io;
  DeferredWrite provider;
  auto connection = boost::shared_ptr<Connection>(new Connection(io));
  boost::weak_ptr<Connection> weak = connection;
  bool alive = false;
  connection->send_message_with_writer(
      rtsp::commands::ok_msg(2),
      [&](int) {
        alive = !weak.expired() && !provider.storage.expired();
        throw std::runtime_error("callback failure");
      },
      provider);
  connection.reset();
  REQUIRE_THROWS_AS(provider.finish({}, 0), std::runtime_error);
  CHECK(alive);
  CHECK(weak.expired());
  CHECK(provider.storage.expired());
  CHECK_FALSE(bool(provider.completion));
}
