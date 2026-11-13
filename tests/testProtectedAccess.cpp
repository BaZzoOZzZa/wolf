#include <array>
#include <atomic>
#include <barrier>
#include <catch2/catch_all.hpp>
#include <openssl/pem.h>
#include <sessions/protected_access.hpp>
#include <thread>
#include <type_traits>
#include <vector>

namespace pa = sessions::protected_access;
namespace {
// Software-only trust adapter. Tests establish exact pairing facts, NOT TLS
// key-possession proof. The future live adapter is deliberately not implemented.
struct Source final : pa::VerifiedIdentitySource {
  pa::VerifiedLeafSnapshot value;
  Source(x509::x509_ptr leaf, std::uint64_t index) : value{leaf, leaf, index} {}
  pa::VerifiedLeafSnapshot verified_leaf() const override {
    return value;
  }
};
struct Certificates {
  x509::pkey_ptr key = x509::generate_key();
  x509::x509_ptr first = x509::generate_x509(key);
  x509::x509_ptr second{X509_dup(first.get()), X509_free};
  Certificates() {
    REQUIRE(key);
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(ASN1_INTEGER_set(X509_get_serialNumber(second.get()), 4321) == 1);
    REQUIRE(X509_sign(second.get(), key.get(), EVP_sha256()) > 0);
  }
};
const Certificates &certs() {
  static Certificates c;
  return c;
}
pa::Leaf leaf(const x509::x509_ptr &cert) {
  auto result = pa::Leaf::from_x509(cert);
  REQUIRE(result);
  return *result;
}
pa::Principal principal(const x509::x509_ptr &cert, std::uint64_t index = 7) {
  auto result = pa::Principal::capture(Source(cert, index));
  REQUIRE(result);
  return *result;
}
struct Policy {
  std::array<pa::PinView, 1> pins{{{leaf(certs().first), 7}}};
  std::array<pa::RuleView, 1> rules{{{"wow", "42", pins}}};
};
pa::Digest collision(const pa::Leaf &) {
  return {};
}
pa::Digest fail_on_reissued(const pa::Leaf &value) {
  if (value.matches(leaf(certs().second)))
    throw std::runtime_error("injected index failure");
  return value.digest();
}
} // namespace

TEST_CASE("Protected leaf parsing is bounded and exact", "[protected-access]") {
  auto pinned = leaf(certs().first);
  auto der = pa::Leaf::from_der(pinned.der());
  REQUIRE(der);
  REQUIRE(der->matches(pinned));
  auto pem = x509::get_cert_pem(certs().first);
  auto roundtrip = pa::Leaf::from_pem(pem);
  REQUIRE(roundtrip);
  REQUIRE(roundtrip->matches(pinned));
  REQUIRE(roundtrip->digest() == pinned.digest());
  // A substring need not end at NUL. The following junk must not be read.
  auto storage = pem + "DO NOT PARSE THIS";
  REQUIRE(pa::Leaf::from_pem(std::string_view(storage.data(), pem.size()))->matches(pinned));
  REQUIRE(pa::Leaf::from_pem(pem + " \r\n\t"));
  REQUIRE_FALSE(pa::Leaf::from_pem(pem + pem));
  REQUIRE_FALSE(pa::Leaf::from_pem(pem + "x"));
  REQUIRE_FALSE(pa::Leaf::from_pem("junk" + pem));
  REQUIRE_FALSE(pa::Leaf::from_pem({}));
  REQUIRE_FALSE(pa::Leaf::from_x509({}));
  REQUIRE_FALSE(pa::Leaf{}.matches(pa::Leaf{}));
  auto bytes = std::vector<unsigned char>(pinned.der().begin(), pinned.der().end());
  bytes.push_back(0);
  REQUIRE_FALSE(pa::Leaf::from_der(bytes));
  bytes.resize(bytes.size() / 2);
  REQUIRE_FALSE(pa::Leaf::from_der(bytes));
  REQUIRE_FALSE(pa::Leaf::from_der({}));
  REQUIRE_FALSE(pa::Leaf::from_der(std::vector<unsigned char>(pa::max_der_bytes + 1, 0)));
  REQUIRE_FALSE(pa::Leaf::from_pem(std::string(pa::max_pem_bytes + 1, 'x')));
}

TEST_CASE("Protected principal requires exact trusted paired leaf", "[protected-access]") {
  Source trusted(certs().first, 7);
  auto accepted = pa::Principal::capture(trusted);
  REQUIRE(accepted);
  trusted.value.presented = certs().second; // same key, different signed leaf
  REQUIRE_FALSE(pa::Principal::capture(trusted));
  trusted.value.presented.reset();
  REQUIRE_FALSE(pa::Principal::capture(trusted));
  trusted.value.presented = certs().first;
  trusted.value.paired.reset();
  REQUIRE_FALSE(pa::Principal::capture(trusted));
  struct Throwing final : pa::VerifiedIdentitySource {
    pa::VerifiedLeafSnapshot verified_leaf() const override {
      throw std::runtime_error("source unavailable");
    }
  } throwing;
  REQUIRE_THROWS_AS(pa::Principal::capture(throwing), std::runtime_error);
  STATIC_REQUIRE_FALSE(std::is_constructible_v<pa::Principal, pa::Leaf, std::uint64_t>);
  STATIC_REQUIRE_FALSE(std::is_constructible_v<pa::Principal, bool>);
  // Capture is immutable even if the trusted source later changes its X509.
  auto mutable_cert = x509::x509_ptr(X509_dup(certs().first.get()), X509_free);
  auto captured = principal(mutable_cert);
  REQUIRE(ASN1_INTEGER_set(X509_get_serialNumber(mutable_cert.get()), 999) == 1);
  REQUIRE(X509_sign(mutable_cert.get(), certs().key.get(), EVP_sha256()) > 0);
  REQUIRE(captured.leaf().matches(accepted->leaf()));
  REQUIRE_FALSE(captured.leaf().matches(leaf(mutable_cert)));
}

TEST_CASE("Pairing and routing metadata alone never allow a protected game", "[protected-access]") {
  pa::AccessStore store;
  Policy policy;
  auto allowed = principal(certs().first);
  auto reissued = principal(certs().second, 8);
  REQUIRE(store.evaluate(allowed, "wow", "42").result == pa::Result::Denied);
  REQUIRE(store.replace_policy(policy.rules) == pa::Result::Allowed);
  REQUIRE(store.evaluate(allowed, "wow", "42").result == pa::Result::Allowed);
  REQUIRE(store.evaluate({}, "wow", "42").result == pa::Result::Denied);
  REQUIRE(store.evaluate(reissued, "wow", "42").result == pa::Result::Denied);
  REQUIRE(store.evaluate(allowed, "unconfigured", "42").result == pa::Result::Denied);
  REQUIRE(store.evaluate(allowed, "wow", "43").result == pa::Result::Denied);
  REQUIRE(store.evaluate(allowed, "PIN-1234", "42").result == pa::Result::Denied);
  REQUIRE(store.evaluate(allowed, "first-input", "42").result == pa::Result::Denied);
  REQUIRE(store.evaluate(principal(certs().first, 900), "wow", "42").result == pa::Result::Denied);
  auto previous = store.evaluate(allowed, "wow", "42").evidence;
  policy.pins[0] = {leaf(certs().second), 8};
  REQUIRE(store.replace_policy(policy.rules) == pa::Result::Allowed);
  REQUIRE_FALSE(store.is_current(previous));
  REQUIRE(store.evaluate(allowed, "wow", "42").result == pa::Result::Denied);
  REQUIRE(store.evaluate(reissued, "wow", "42").result == pa::Result::Allowed);
}

TEST_CASE("Missing wildcard malformed and ambiguous policies fail closed", "[protected-access]") {
  pa::AccessStore store;
  Policy policy;
  auto p = principal(certs().first);
  REQUIRE(store.replace_policy(policy.rules) == pa::Result::Allowed);
  auto old = store.evaluate(p, "wow", "42").evidence;
  REQUIRE(store.replace_policy({}) == pa::Result::Denied);
  REQUIRE_FALSE(store.is_current(old));
  for (auto game : {"", "*", "../wow", "/wow", "My WoW", ".", ".."}) {
    policy.rules[0].game_key = game;
    REQUIRE(store.replace_policy(policy.rules) == pa::Result::Invalid);
  }
  policy.rules[0].game_key = "wow";
  for (auto app : {"", "*", "042", "0", "-1", "4294967296", "42/x"}) {
    policy.rules[0].catalog_appid = app;
    REQUIRE(store.replace_policy(policy.rules) == pa::Result::Invalid);
  }
  policy.rules[0].catalog_appid = "42";
  policy.rules[0].allow = {};
  REQUIRE(store.replace_policy(policy.rules) == pa::Result::Invalid);
  policy.rules[0].allow = policy.pins;
  policy.pins[0].leaf = {};
  REQUIRE(store.replace_policy(policy.rules) == pa::Result::Invalid);
  policy.pins[0].leaf = leaf(certs().first);
  std::array<pa::RuleView, 2> duplicates{{{"wow", "42", policy.pins}, {"other", "42", policy.pins}}};
  REQUIRE(store.replace_policy(duplicates) == pa::Result::Ambiguous);
  duplicates[1] = {"wow", "43", policy.pins};
  REQUIRE(store.replace_policy(duplicates) == pa::Result::Ambiguous);
  auto long_name = std::string(pa::max_identifier_bytes + 1, 'a');
  policy.rules[0].game_key = long_name;
  REQUIRE(store.replace_policy(policy.rules) == pa::Result::Invalid);
}

TEST_CASE("Numeric and digest collisions cannot substitute a leaf", "[protected-access]") {
  Policy policy;
  std::array<pa::PinView, 2> pins{{{leaf(certs().first), 7}, {leaf(certs().second), 7}}};
  policy.rules[0].allow = pins;
  pa::AccessStore normal;
  REQUIRE(normal.replace_policy(policy.rules) == pa::Result::Ambiguous);
  pins[1].numeric_index = 8;
  pa::AccessStore collision_store(collision);
  REQUIRE(collision_store.replace_policy(policy.rules) == pa::Result::Ambiguous);
  REQUIRE(normal.replace_policy(policy.rules) == pa::Result::Allowed);
  // Duplicate identical pins also deny ambiguous operator configuration.
  pins[1] = pins[0];
  REQUIRE(normal.replace_policy(policy.rules) == pa::Result::Ambiguous);
  pins[1].numeric_index = 8;
  REQUIRE(normal.replace_policy(policy.rules) == pa::Result::Ambiguous);
  policy.rules[0].allow = policy.pins;
  REQUIRE(normal.replace_policy(policy.rules) == pa::Result::Allowed);
  REQUIRE(collision_store.replace_policy(policy.rules) == pa::Result::Allowed);
  auto exact = principal(certs().first);
  REQUIRE(collision_store.evaluate(exact, "wow", "42").result == pa::Result::Allowed);
  REQUIRE(normal.evaluate(principal(certs().second, 7), "wow", "42").result == pa::Result::Ambiguous);
  REQUIRE(collision_store.evaluate(principal(certs().second, 8), "wow", "42").result == pa::Result::Ambiguous);
  std::array<pa::PinView, 1> other{{{leaf(certs().second), 7}}};
  std::array<pa::RuleView, 2> cross_game{{{"wow", "42", policy.pins}, {"local", "43", other}}};
  REQUIRE(normal.replace_policy(cross_game) == pa::Result::Ambiguous);
}

TEST_CASE("Only owning store current revisions can revalidate evidence", "[protected-access]") {
  pa::AccessStore store, other;
  Policy policy;
  auto p = principal(certs().first);
  REQUIRE_FALSE(store.is_current({}));
  REQUIRE(store.replace_policy(policy.rules) == pa::Result::Allowed);
  REQUIRE(other.replace_policy(policy.rules) == pa::Result::Allowed);
  auto first = store.evaluate(p, "wow", "42").evidence;
  REQUIRE(first.historical_allowed());
  REQUIRE(first.game_key() == "wow");
  REQUIRE(first.catalog_appid() == "42");
  REQUIRE(first.principal().leaf().matches(p.leaf()));
  REQUIRE_FALSE(first.target()); // historical ACL-only evidence, NOT a run grant
  REQUIRE(store.is_current(first));
  REQUIRE_FALSE(other.is_current(first));
  REQUIRE(store.replace_policy(policy.rules) == pa::Result::Allowed);
  auto second = store.evaluate(p, "wow", "42").evidence;
  REQUIRE_FALSE(first.config_revision().matches(second.config_revision()));
  REQUIRE_FALSE(first.auth_revision().matches(second.auth_revision()));
  REQUIRE_FALSE(store.is_current(first));
  REQUIRE(first.historical_allowed());
  REQUIRE(store.is_current(second));
  store.invalidate_policy();
  REQUIRE_FALSE(store.is_current(second));
  REQUIRE(store.replace_policy(policy.rules) == pa::Result::Allowed);
  REQUIRE_FALSE(store.is_current(first));
  REQUIRE_FALSE(store.is_current(second));
  REQUIRE(store.is_current(store.evaluate(p, "wow", "42").evidence));
  REQUIRE_FALSE(pa::ConfigRevision{}.matches(pa::ConfigRevision{}));
  REQUIRE_FALSE(pa::Evidence{}.historical_allowed());
}

TEST_CASE("Running binding is retained and cannot be reassigned by IDs", "[protected-access]") {
  pa::AccessStore store, foreign;
  Policy policy;
  auto p = principal(certs().first);
  auto run = pa::RunScope::fresh("L");
  REQUIRE_FALSE(store.bind_target("wow", "42", run));
  REQUIRE(store.replace_policy(policy.rules) == pa::Result::Allowed);
  REQUIRE_FALSE(store.bind_target("wow", "42", {}));
  REQUIRE_FALSE(store.bind_target("wow", "42", pa::RunScope::fresh("")));
  auto without_target = store.evaluate(p, "wow", "42").evidence;
  auto first = store.bind_target("wow", "42", run);
  REQUIRE(first);
  REQUIRE(first->run().matches(run));
  REQUIRE_FALSE(store.is_current(without_target));
  auto first_e = store.evaluate(p, "wow", "42").evidence;
  REQUIRE(store.is_current(first_e));
  REQUIRE_FALSE(store.bind_target("wow", "42", pa::RunScope::fresh("L")));
  REQUIRE_FALSE(foreign.set_lifecycle(*first, pa::Lifecycle::Ended));
  auto rebound = store.bind_target("wow", "42", run);
  REQUIRE(rebound);
  REQUIRE_FALSE(first->binding_revision().matches(rebound->binding_revision()));
  REQUIRE_FALSE(store.is_current(first_e));
  REQUIRE_FALSE(store.set_lifecycle(*first, pa::Lifecycle::Ended));
  policy.rules[0].catalog_appid = "43";
  auto current = store.evaluate(p, "wow", "42").evidence;
  REQUIRE(store.replace_policy(policy.rules) == pa::Result::BindingConflict);
  REQUIRE_FALSE(store.is_current(current));
  REQUIRE_FALSE(store.bind_target("wow", "43", run));
  policy.rules[0].catalog_appid = "42";
  REQUIRE(store.replace_policy(policy.rules) == pa::Result::Allowed);
  REQUIRE_FALSE(store.is_current(current));
  REQUIRE(store.evaluate(p, "wow", "42").evidence.target().run().matches(run));
}

TEST_CASE("Unknown lifecycle occupies target slot without implicit recovery", "[protected-access]") {
  pa::AccessStore store;
  Policy policy;
  auto p = principal(certs().first);
  auto run = pa::RunScope::fresh("L");
  REQUIRE(store.replace_policy(policy.rules) == pa::Result::Allowed);
  auto initial = store.bind_target("wow", "42", run);
  REQUIRE(initial);
  auto old = store.evaluate(p, "wow", "42").evidence;
  auto running = store.set_lifecycle(*initial, pa::Lifecycle::Running);
  REQUIRE(running);
  REQUIRE(running->binding_revision().matches(initial->binding_revision()));
  REQUIRE_FALSE(running->lifecycle_revision().matches(initial->lifecycle_revision()));
  REQUIRE_FALSE(store.is_current(old));
  auto unknown = store.set_lifecycle(*running, pa::Lifecycle::Unknown);
  REQUIRE(unknown);
  REQUIRE(store.evaluate(p, "wow", "42").result == pa::Result::Denied);
  REQUIRE_FALSE(store.bind_target("wow", "42", run));
  REQUIRE_FALSE(store.bind_target("wow", "42", pa::RunScope::fresh("L")));
  REQUIRE_FALSE(store.set_lifecycle(*unknown, pa::Lifecycle::Running));
  REQUIRE_FALSE(store.set_lifecycle({}, pa::Lifecycle::Ended));
  REQUIRE_FALSE(store.set_lifecycle(*unknown, static_cast<pa::Lifecycle>(999)));
  REQUIRE(store.replace_policy(policy.rules) == pa::Result::Allowed);
  REQUIRE(store.evaluate(p, "wow", "42").result == pa::Result::Denied);
  // Trusted terminal FACT, not proof that a process/device actually stopped.
  auto ended = store.set_lifecycle(*unknown, pa::Lifecycle::Ended);
  REQUIRE(ended);
  REQUIRE_FALSE(store.set_lifecycle(*ended, pa::Lifecycle::Running));
  auto replacement_run = pa::RunScope::fresh("L");
  auto replacement = store.bind_target("wow", "42", replacement_run);
  REQUIRE(replacement);
  REQUIRE_FALSE(replacement->run().matches(run));
  REQUIRE(store.evaluate(p, "wow", "42").result == pa::Result::Allowed);
  REQUIRE_FALSE(store.is_current(old));
  REQUIRE_FALSE(store.set_lifecycle(*unknown, pa::Lifecycle::Ended));
}

TEST_CASE("Record and byte bounds apply before policy copies", "[protected-access]") {
  Policy policy;
  pa::AccessStore store;
  std::vector<pa::RuleView> excess_games(pa::max_games + 1, policy.rules[0]);
  REQUIRE(store.replace_policy(excess_games) == pa::Result::Capacity);
  std::vector<pa::PinView> excess_pins(pa::max_pin_entries + 1, policy.pins[0]);
  policy.rules[0].allow = excess_pins;
  REQUIRE(store.replace_policy(policy.rules) == pa::Result::Capacity);
  // Real large certificate: the aggregate bound wins before duplicate-pin
  // processing; all parse/copy extents are still within per-leaf limits.
  auto large = x509::x509_ptr(X509_dup(certs().first.get()), X509_free);
  std::unique_ptr<ASN1_OCTET_STRING, decltype(&ASN1_OCTET_STRING_free)> data(ASN1_OCTET_STRING_new(),
                                                                             ASN1_OCTET_STRING_free);
  REQUIRE(data);
  std::vector<unsigned char> padding(5000, 'a');
  REQUIRE(ASN1_OCTET_STRING_set(data.get(), padding.data(), padding.size()) == 1);
  std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> extension(
      X509_EXTENSION_create_by_NID(nullptr, NID_netscape_comment, 0, data.get()),
      X509_EXTENSION_free);
  REQUIRE(extension);
  REQUIRE(X509_add_ext(large.get(), extension.get(), -1) == 1);
  REQUIRE(X509_sign(large.get(), certs().key.get(), EVP_sha256()) > 0);
  auto large_leaf = leaf(large);
  REQUIRE(large_leaf.der().size() < pa::max_der_bytes);
  REQUIRE(large_leaf.der().size() * pa::max_pin_entries > pa::max_policy_der_bytes);
  std::vector<pa::PinView> large_pins(pa::max_pin_entries, {large_leaf, 7});
  policy.rules[0].allow = large_pins;
  REQUIRE(store.replace_policy(policy.rules) == pa::Result::Capacity);
  // Both count limits are attainable with unambiguous exact pins.
  std::array<pa::PinView, 2> two{{{leaf(certs().first), 7}, {leaf(certs().second), 8}}};
  std::vector<std::string> games, apps;
  games.reserve(pa::max_games);
  apps.reserve(pa::max_games);
  for (std::size_t i = 0; i < pa::max_games; ++i) {
    games.push_back("game-" + std::to_string(i));
    apps.push_back(std::to_string(i + 1));
  }
  std::vector<pa::RuleView> full;
  for (std::size_t i = 0; i < pa::max_games; ++i)
    full.push_back({games[i], apps[i], two});
  REQUIRE(full.size() == pa::max_games);
  REQUIRE(full.size() * two.size() == pa::max_pin_entries);
  REQUIRE(store.replace_policy(full) == pa::Result::Allowed);
  for (std::size_t i = 0; i < pa::max_games; ++i)
    REQUIRE(store.evaluate(principal(certs().first), games[i], apps[i]).result == pa::Result::Allowed);
}

TEST_CASE("Failed policy preparation invalidates old evidence", "[protected-access]") {
  pa::AccessStore store(fail_on_reissued);
  Policy policy;
  REQUIRE(store.replace_policy(policy.rules) == pa::Result::Allowed);
  auto old = store.evaluate(principal(certs().first), "wow", "42").evidence;
  REQUIRE(store.is_current(old));
  policy.pins[0] = {leaf(certs().second), 8};
  REQUIRE_THROWS_AS(store.replace_policy(policy.rules), std::runtime_error);
  REQUIRE_FALSE(store.is_current(old));
  REQUIRE(store.evaluate(principal(certs().first), "wow", "42").result == pa::Result::Denied);
}

TEST_CASE("Completed store revocation invalidates concurrent retained evidence", "[protected-access]") {
  pa::AccessStore store;
  Policy policy;
  REQUIRE(store.replace_policy(policy.rules) == pa::Result::Allowed);
  const auto p = principal(certs().first);
  auto evidence = store.evaluate(p, "wow", "42").evidence;
  std::barrier gate(2);
  bool before = false, after = true;
  std::jthread reader([&] {
    before = store.is_current(evidence);
    gate.arrive_and_wait();
    gate.arrive_and_wait();
    after = store.is_current(evidence);
  });
  gate.arrive_and_wait();
  store.invalidate_policy();
  gate.arrive_and_wait();
  reader.join();
  REQUIRE(before);
  REQUIRE_FALSE(after);
  REQUIRE(evidence.historical_allowed());
}
