#include <catch2/catch_test_macros.hpp>
#include <openssl/x509v3.h>
#include <sessions/protected_identity.hpp>
#include <type_traits>

namespace pi = sessions::protected_identity;
namespace pa = sessions::protected_access;
namespace {
// Access only the existing protected SimpleWeb transport types; no server instance.
struct Types : SimpleWeb::ServerBase<SimpleWeb::HTTPS> {
  using SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Connection;
  using SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Session;
};
struct RequestFixture {
  boost::asio::io_context io;
  boost::asio::ssl::context tls{boost::asio::ssl::context::tls};
  std::shared_ptr<Types::Connection> connection = std::make_shared<Types::Connection>(
      std::make_shared<SimpleWeb::ScopeRunner>(), io, tls);
  std::shared_ptr<Types::Session> session = std::make_shared<Types::Session>(2048, connection);
  pi::RequestPtr request = session->request;
};
struct Certificates {
  x509::pkey_ptr key = x509::generate_key();
  x509::x509_ptr first = x509::generate_x509(key);
  x509::x509_ptr copy(long serial) const {
    x509::x509_ptr result(X509_dup(first.get()), X509_free);
    REQUIRE(result);
    REQUIRE(ASN1_INTEGER_set(X509_get_serialNumber(result.get()), serial) == 1);
    REQUIRE(X509_sign(result.get(), key.get(), EVP_sha256()) > 0);
    return result;
  }
};
const Certificates &certs() {
  static Certificates c;
  return c;
}
state::PairedClientList list(const std::vector<std::string> &pems) {
  state::PairedClientList result;
  for (const auto &pem : pems)
    result = result.push_back(immer::box<wolf::config::PairedClient>(wolf::config::PairedClient{.client_cert = pem}));
  return result;
}
struct Provider {
  x509::x509_ptr certificate;
  unsigned calls = 0;
  int fault = 0;
  pi::PeerCertificateProvider operations() {
    return {this, +[](void *raw, const pi::RequestPtr &) -> x509::x509_ptr {
              auto &self = *static_cast<Provider *>(raw);
              ++self.calls;
              if (self.fault == 1)
                throw std::runtime_error("provider fault");
              if (self.fault == 2)
                throw std::bad_alloc();
              return self.certificate;
            }};
  }
};
pi::PairedSnapshot snapshot(const pi::PairingRevision &revision) {
  auto result = pi::PairedSnapshot::seal(list({x509::get_cert_pem(certs().first)}), revision);
  REQUIRE(result.error == pi::Error::None);
  REQUIRE(result.value);
  return *result.value;
}
} // namespace

TEST_CASE("HTTPS actual peer getter denies absent and expired transport without opening sockets",
          "[protected-identity]") {
  CHECK_FALSE(HttpsServer::get_client_cert({}));
  RequestFixture f;
  REQUIRE_FALSE(f.connection->socket->lowest_layer().is_open());
  CHECK_FALSE(HttpsServer::get_client_cert(f.request)); // real SSL object, no handshake/peer cert
  auto request = f.request;
  std::weak_ptr<Types::Connection> weak = f.connection;
  f.session.reset();
  f.connection.reset();
  REQUIRE(weak.expired());
  CHECK_FALSE(HttpsServer::get_client_cert(request));
  auto no_connection = std::make_shared<Types::Session>(2048, nullptr);
  CHECK_FALSE(HttpsServer::get_client_cert(no_connection->request));
  RequestFixture no_socket;
  no_socket.connection->socket.reset();
  CHECK_FALSE(HttpsServer::get_client_cert(no_socket.request));
}

TEST_CASE("Sealed HTTPS identity uses actual exact DER and real paired verifier", "[protected-identity]") {
  static_assert(!std::is_default_constructible_v<pi::VerifiedHttpsIdentity>);
  RequestFixture f;
  auto revision = pi::PairingRevision::fresh(4);
  auto paired = snapshot(revision);
  Provider provider{certs().first};
  auto identity = pi::VerifiedHttpsIdentity::capture(f.request, paired, revision, provider.operations());
  REQUIRE(identity.value);
  CHECK(identity.error == pi::Error::None);
  CHECK(provider.calls == 1);
  CHECK(identity.value->presented().matches(identity.value->paired()));
  CHECK(identity.value->principal().leaf().matches(identity.value->presented()));
  CHECK(identity.value->revision().matches(revision));
  CHECK(identity.value->principal().numeric_index() == std::hash<std::string>{}(x509::get_cert_pem(certs().first)));
  CHECK_FALSE(x509::verification_error(certs().first, certs().first));
  provider.certificate = certs().copy(3456); // same key, another signed DER leaf, not paired
  auto reissued = pi::VerifiedHttpsIdentity::capture(f.request, paired, revision, provider.operations());
  CHECK_FALSE(reissued.value);
  CHECK(reissued.error == pi::Error::Unpaired);
  auto foreign_key = x509::generate_key();
  provider.certificate = x509::generate_x509(foreign_key);
  CHECK_FALSE(pi::VerifiedHttpsIdentity::capture(f.request, paired, revision, provider.operations()).value);
  // Existing pairing facts are not permission for any game.
  pa::AccessStore unconfigured;
  CHECK(unconfigured.evaluate(identity.value->principal(), "game", "42").result != pa::Result::Allowed);
}

TEST_CASE("HTTPS capture rejects null foreign and ABA revisions before retrieval", "[protected-identity]") {
  RequestFixture f;
  auto revision = pi::PairingRevision::fresh(7);
  auto paired = snapshot(revision);
  Provider provider{certs().first};
  for (const auto &wrong : {pi::PairingRevision{}, pi::PairingRevision::fresh(7), pi::PairingRevision::fresh(8)}) {
    auto result = pi::VerifiedHttpsIdentity::capture(f.request, paired, wrong, provider.operations());
    CHECK(result.error == pi::Error::Stale);
    CHECK_FALSE(result.value);
  }
  CHECK(provider.calls == 0);
  CHECK_FALSE(pi::PairedSnapshot::seal(list({x509::get_cert_pem(certs().first)}), {}).value);
  CHECK_FALSE(pi::VerifiedHttpsIdentity::capture(f.request, {}, revision, provider.operations()).value);
  auto replacement_revision = pi::PairingRevision::fresh(7);
  auto replacement = snapshot(replacement_revision);
  CHECK_FALSE(pi::VerifiedHttpsIdentity::capture(f.request, replacement, revision, provider.operations()).value);
  auto retained = paired.revision();
  revision = {};
  CHECK(pi::VerifiedHttpsIdentity::capture(f.request, paired, retained, provider.operations()).value);
  // Old retained evidence remains historically valid; no claim of current authority.
  CHECK_FALSE(pi::VerifiedHttpsIdentity::capture(f.request, paired, replacement_revision, provider.operations()).value);
}

TEST_CASE("Pairing snapshot rejects duplicate and colliding indexes instead of first match", "[protected-identity]") {
  auto revision = pi::PairingRevision::fresh(1);
  auto pem = x509::get_cert_pem(certs().first);
  auto second = x509::get_cert_pem(certs().copy(4321));
  for (const auto &entries : {list({pem, pem}), list({pem, pem + "\n"})}) {
    auto result = pi::PairedSnapshot::seal(entries, revision);
    CHECK(result.error == pi::Error::Ambiguous);
    CHECK_FALSE(result.value);
  }
  auto numeric = pi::Indexers::defaults();
  numeric.numeric = +[](const std::string &) -> std::uint64_t { return 0; };
  auto result = pi::PairedSnapshot::seal(list({pem, second}), revision, numeric);
  CHECK(result.error == pi::Error::Ambiguous);
  CHECK_FALSE(result.value);
  auto digest = pi::Indexers::defaults();
  digest.digest = +[](const pa::Leaf &) { return pa::Digest{}; };
  result = pi::PairedSnapshot::seal(list({pem, second}), revision, digest);
  CHECK(result.error == pi::Error::Ambiguous);
  CHECK_FALSE(result.value);
  // All bounded records are inspected, even a duplicate far beyond the matching first entry.
  result = pi::PairedSnapshot::seal(list({pem, second, pem}), revision);
  CHECK(result.error == pi::Error::Ambiguous);
  CHECK_FALSE(result.value);
}

TEST_CASE("Pairing preflight count PEM and aggregate bounds precede parsing", "[protected-identity]") {
  auto revision = pi::PairingRevision::fresh(2);
  CHECK(pi::PairedSnapshot::seal({}, revision).error == pi::Error::Invalid);
  CHECK(pi::PairedSnapshot::seal(list(std::vector<std::string>(65, "invalid")), revision).error == pi::Error::Capacity);
  CHECK(pi::PairedSnapshot::seal(list(std::vector<std::string>(64, "invalid")), revision).error == pi::Error::Invalid);
  CHECK(pi::PairedSnapshot::seal(list({"invalid", std::string(pa::max_pem_bytes + 1, 'x')}), revision).error ==
        pi::Error::Capacity);
  CHECK(pi::PairedSnapshot::seal(list(std::vector<std::string>(11, std::string(pa::max_pem_bytes, 'x'))), revision)
            .error == pi::Error::Capacity);
  auto padded = x509::get_cert_pem(certs().first);
  padded.resize(pa::max_pem_bytes, ' ');
  CHECK(pi::PairedSnapshot::seal(list({padded}), revision).value);
  padded.push_back(' ');
  CHECK(pi::PairedSnapshot::seal(list({padded}), revision).error == pi::Error::Capacity);
  std::vector<std::string> exact_total;
  for (int i = 0; i < 11; ++i) {
    auto pem = x509::get_cert_pem(certs().copy(10000 + i));
    pem.resize(i == 10 ? 16 * 1024 : 24 * 1024, ' ');
    exact_total.push_back(std::move(pem));
  }
  CHECK(pi::PairedSnapshot::seal(list(exact_total), revision).value); // exactly256KiB input
  exact_total.back().push_back(' ');
  CHECK(pi::PairedSnapshot::seal(list(exact_total), revision).error == pi::Error::Capacity);
  std::vector<std::string> max_count;
  for (int i = 0; i < 64; ++i)
    max_count.push_back(x509::get_cert_pem(certs().copy(20000 + i)));
  auto full = pi::PairedSnapshot::seal(list(max_count), revision);
  REQUIRE(full.value);
  CHECK(full.value->size() == 64);
  max_count.push_back(x509::get_cert_pem(certs().copy(30000)));
  CHECK(pi::PairedSnapshot::seal(list(max_count), revision).error == pi::Error::Capacity);
}

TEST_CASE("HTTPS identity freezes DER and owns no request provider or mutable certificate", "[protected-identity]") {
  RequestFixture f;
  auto revision = pi::PairingRevision::fresh(3);
  auto mutable_cert = certs().copy(4567);
  std::weak_ptr<X509> weak_cert = mutable_cert;
  auto paired_input = list({x509::get_cert_pem(mutable_cert)});
  auto sealed = pi::PairedSnapshot::seal(paired_input, revision);
  REQUIRE(sealed.value);
  Provider provider{mutable_cert};
  auto result = pi::VerifiedHttpsIdentity::capture(f.request, *sealed.value, revision, provider.operations());
  REQUIRE(result.value);
  auto old_der =
      std::vector<unsigned char>(result.value->presented().der().begin(), result.value->presented().der().end());
  REQUIRE(ASN1_INTEGER_set(X509_get_serialNumber(mutable_cert.get()), 9876) == 1);
  REQUIRE(X509_sign(mutable_cert.get(), certs().key.get(), EVP_sha256()) > 0);
  CHECK_FALSE(pi::VerifiedHttpsIdentity::capture(f.request, *sealed.value, revision, provider.operations()).value);
  paired_input = {};
  sealed.value.reset();
  provider.certificate.reset();
  mutable_cert.reset();
  CHECK(weak_cert.expired());
  std::weak_ptr<pi::Request> weak_request = f.request;
  f.request.reset();
  f.session.reset();
  f.connection.reset();
  CHECK(weak_request.expired());
  CHECK(std::vector<unsigned char>(result.value->presented().der().begin(), result.value->presented().der().end()) ==
        old_der);
  CHECK(result.value->paired().matches(result.value->presented()));
  CHECK(result.value->revision().matches(revision));
}

TEST_CASE("HTTPS adapter rejects getter and parser failures without escaping", "[protected-identity]") {
  RequestFixture f;
  auto revision = pi::PairingRevision::fresh(5);
  auto paired = snapshot(revision);
  CHECK_FALSE(pi::VerifiedHttpsIdentity::capture(f.request, paired, revision).value); // real no-peer getter
  Provider provider{certs().first};
  CHECK(pi::VerifiedHttpsIdentity::capture({}, paired, revision, provider.operations()).error == pi::Error::Invalid);
  CHECK(provider.calls == 0);
  CHECK(pi::VerifiedHttpsIdentity::capture(f.request, paired, revision, {}).error == pi::Error::Invalid);
  provider.certificate.reset();
  CHECK(pi::VerifiedHttpsIdentity::capture(f.request, paired, revision, provider.operations()).error ==
        pi::Error::Unpaired);
  provider.fault = 1;
  CHECK(pi::VerifiedHttpsIdentity::capture(f.request, paired, revision, provider.operations()).error ==
        pi::Error::ProviderFailure);
  provider.fault = 2; // boundary handling, not global allocator-failure evidence
  CHECK(pi::VerifiedHttpsIdentity::capture(f.request, paired, revision, provider.operations()).error ==
        pi::Error::AllocationFailure);
  auto pem = x509::get_cert_pem(certs().first);
  for (const auto &bad : {std::string{}, std::string("bad"), pem + pem, pem + "junk"})
    CHECK_FALSE(pi::PairedSnapshot::seal(list({bad}), revision).value);
  auto indexes = pi::Indexers::defaults();
  indexes.numeric = +[](const std::string &) -> std::uint64_t { throw std::runtime_error("index fault"); };
  CHECK(pi::PairedSnapshot::seal(list({pem}), revision, indexes).error == pi::Error::Invalid);
  indexes.numeric = +[](const std::string &) -> std::uint64_t { throw std::bad_alloc(); };
  CHECK(pi::PairedSnapshot::seal(list({pem}), revision, indexes).error == pi::Error::AllocationFailure);
  CHECK_FALSE(pi::PairedSnapshot::seal(list({pem}), revision, {}).value);
}

TEST_CASE("Oversized presented DER is refused by actual bounded Leaf primitive", "[protected-identity]") {
  RequestFixture f;
  auto revision = pi::PairingRevision::fresh(6);
  auto paired = snapshot(revision);
  auto large = certs().copy(8888);
  std::unique_ptr<ASN1_OCTET_STRING, decltype(&ASN1_OCTET_STRING_free)> value(ASN1_OCTET_STRING_new(),
                                                                              ASN1_OCTET_STRING_free);
  REQUIRE(value);
  std::string bytes(pa::max_der_bytes, 'a');
  REQUIRE(ASN1_OCTET_STRING_set(value.get(), reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size()) == 1);
  std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> extension(
      X509_EXTENSION_create_by_NID(nullptr, NID_netscape_comment, 0, value.get()),
      X509_EXTENSION_free);
  REQUIRE(extension);
  REQUIRE(X509_add_ext(large.get(), extension.get(), -1) == 1);
  REQUIRE(X509_sign(large.get(), certs().key.get(), EVP_sha256()) > 0);
  REQUIRE(i2d_X509(large.get(), nullptr) > pa::max_der_bytes);
  Provider provider{large};
  CHECK(pi::VerifiedHttpsIdentity::capture(f.request, paired, revision, provider.operations()).error ==
        pi::Error::Unpaired);
}
