#include "protected_access.hpp"
#include <algorithm>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace sessions::protected_access {
struct Leaf::Data {
  std::vector<unsigned char> der;
  Digest digest;
};
Leaf::Leaf(std::shared_ptr<const Data> data) : data_(std::move(data)) {}
std::optional<Leaf> Leaf::from_x509(const x509::x509_ptr &cert) {
  if (!cert)
    return {};
  const int size = i2d_X509(cert.get(), nullptr);
  if (size <= 0 || static_cast<std::size_t>(size) > max_der_bytes)
    return {};
  auto data = std::make_shared<Data>();
  data->der.resize(size); // extent checked before allocation/copy
  auto cursor = data->der.data();
  if (i2d_X509(cert.get(), &cursor) != size || cursor != data->der.data() + size ||
      !SHA256(data->der.data(), data->der.size(), data->digest.data()))
    return {};
  return Leaf(std::move(data));
}
std::optional<Leaf> Leaf::from_der(std::span<const unsigned char> bytes) {
  if (bytes.empty() || bytes.size() > max_der_bytes)
    return {};
  auto cursor = bytes.data();
  x509::x509_ptr cert(d2i_X509(nullptr, &cursor, static_cast<long>(bytes.size())), X509_free);
  if (!cert || cursor != bytes.data() + bytes.size())
    return {};
  auto leaf = from_x509(cert);
  if (!leaf || !std::ranges::equal(leaf->der(), bytes))
    return {}; // reject noncanonical/trailing encoding
  return leaf;
}
std::optional<Leaf> Leaf::from_pem(std::string_view bytes) {
  if (bytes.empty() || bytes.size() > max_pem_bytes || !bytes.starts_with("-----BEGIN CERTIFICATE-----"))
    return {};
  // cert_from_string uses BIO_puts on string_view::data(); use the bounded public
  // OpenSSL memory-BIO interface here, without changing existing MWOW6 helpers.
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new_mem_buf(bytes.data(), static_cast<int>(bytes.size())),
                                                BIO_free);
  if (!bio)
    return {};
  x509::x509_ptr cert(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr), X509_free);
  char *tail = nullptr;
  const auto remaining = BIO_get_mem_data(bio.get(), &tail);
  for (long i = 0; i < remaining; ++i)
    if (tail[i] != ' ' && tail[i] != '\t' && tail[i] != '\r' && tail[i] != '\n')
      return {}; // no second certificate/chain/ignored junk
  return from_x509(cert);
}
Leaf::operator bool() const {
  return bool(data_);
}
std::span<const unsigned char> Leaf::der() const {
  return data_ ? std::span<const unsigned char>(data_->der) : std::span<const unsigned char>{};
}
Digest Leaf::digest() const {
  return data_ ? data_->digest : Digest{};
}
bool Leaf::matches(const Leaf &other) const {
  return data_ && other.data_ && data_->der == other.data_->der;
}
Principal::Principal(Leaf leaf, std::uint64_t index) : leaf_(std::move(leaf)), numeric_index_(index) {}
std::optional<Principal> Principal::capture(const VerifiedIdentitySource &source) {
  auto verified = source.verified_leaf(); // trusted stable source; exceptions propagate, no store mutation
  const auto presented = Leaf::from_x509(verified.presented);
  const auto paired = Leaf::from_x509(verified.paired);
  if (!presented || !paired || x509::verification_error(verified.paired, verified.presented))
    return {};
  return Principal(*presented, verified.numeric_index);
}
Principal::operator bool() const {
  return bool(leaf_);
}
const Leaf &Principal::leaf() const {
  return leaf_;
}
std::uint64_t Principal::numeric_index() const {
  return numeric_index_;
}
namespace detail {
struct Pin {
  Leaf leaf;
  std::uint64_t numeric;
  Digest digest;
};
struct Rule {
  std::string game, app;
  std::vector<Pin> pins;
};
struct Policy {
  ConfigRevision config = ConfigRevision::fresh(0);
  AuthRevision auth = AuthRevision::fresh(0);
  std::vector<Rule> rules;
};
struct Target {
  std::shared_ptr<const StoreIdentity> store;
  std::string game, app;
  RunScope run;
  BindingRevision binding = BindingRevision::fresh(0);
  LifecycleRevision lifecycle = LifecycleRevision::fresh(0);
  Lifecycle state = Lifecycle::Running;
};
} // namespace detail
namespace {
bool game_id(std::string_view s) {
  return !s.empty() && s.size() <= max_identifier_bytes && s != "." && s != ".." &&
         std::ranges::all_of(s, [](unsigned char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                  c == '.';
         });
}
bool app_id(std::string_view s) {
  if (s.empty() || s.size() > 10 || s.front() == '0')
    return false;
  std::uint64_t n = 0;
  for (unsigned char c : s) {
    if (c < '0' || c > '9')
      return false;
    n = n * 10 + c - '0';
  }
  return n <= UINT32_MAX;
}
Result preflight(std::span<const RuleView> rules) {
  if (rules.empty())
    return Result::Denied;
  if (rules.size() > max_games)
    return Result::Capacity;
  std::size_t pins = 0, bytes = 0;
  for (const auto &r : rules) {
    if (!game_id(r.game_key) || !app_id(r.catalog_appid) || r.allow.empty())
      return Result::Invalid;
    if (r.allow.size() > max_pin_entries - pins)
      return Result::Capacity;
    pins += r.allow.size();
    for (const auto &p : r.allow) {
      if (!p.leaf)
        return Result::Invalid;
      if (p.leaf.der().size() > max_policy_der_bytes - bytes)
        return Result::Capacity;
      bytes += p.leaf.der().size();
    }
  }
  return Result::Allowed;
}
} // namespace
TargetFact::TargetFact(std::shared_ptr<const detail::Target> value) : value_(std::move(value)) {}
TargetFact::operator bool() const {
  return bool(value_);
}
RunScope TargetFact::run() const {
  return value_ ? value_->run : RunScope{};
}
BindingRevision TargetFact::binding_revision() const {
  return value_ ? value_->binding : BindingRevision{};
}
LifecycleRevision TargetFact::lifecycle_revision() const {
  return value_ ? value_->lifecycle : LifecycleRevision{};
}
bool Evidence::historical_allowed() const {
  return bool(store_ && policy_);
}
std::string_view Evidence::game_key() const {
  return policy_ ? policy_->rules[rule_].game : std::string_view{};
}
std::string_view Evidence::catalog_appid() const {
  return policy_ ? policy_->rules[rule_].app : std::string_view{};
}
ConfigRevision Evidence::config_revision() const {
  return policy_ ? policy_->config : ConfigRevision{};
}
AuthRevision Evidence::auth_revision() const {
  return policy_ ? policy_->auth : AuthRevision{};
}
const Principal &Evidence::principal() const {
  return principal_;
}
const TargetFact &Evidence::target() const {
  return target_;
}
Digest AccessStore::sha256_index(const Leaf &leaf) {
  return leaf.digest();
}
AccessStore::AccessStore(DigestIndex index)
    : index_(index), identity_(std::make_shared<const detail::StoreIdentity>()) {
  if (!index_)
    throw std::invalid_argument("Missing digest index");
}
void AccessStore::invalidate_policy() {
  std::shared_ptr<const detail::Policy> retired;
  {
    std::lock_guard lock(mutex_);
    retired.swap(policy_);
  }
}
Result AccessStore::replace_policy(std::span<const RuleView> rules) {
  auto status = preflight(rules);
  if (status != Result::Allowed) {
    invalidate_policy();
    return status;
  }
  std::shared_ptr<detail::Policy> prepared;
  try {
    prepared = std::make_shared<detail::Policy>();
    prepared->rules.reserve(rules.size());
    for (const auto &input : rules) {
      for (const auto &r : prepared->rules)
        if (r.game == input.game_key || r.app == input.catalog_appid) {
          invalidate_policy();
          return Result::Ambiguous;
        }
      detail::Rule rule{std::string(input.game_key), std::string(input.catalog_appid), {}};
      rule.pins.reserve(input.allow.size());
      for (const auto &p : input.allow) {
        const auto digest = index_(p.leaf); // outside store lock; no effectful callbacks
        auto conflict = [&](const detail::Pin &existing) {
          const bool same = existing.leaf.matches(p.leaf);
          return (same && existing.numeric != p.numeric_index) ||
                 (!same && (existing.numeric == p.numeric_index || existing.digest == digest));
        };
        for (const auto &r : prepared->rules)
          for (const auto &pin : r.pins)
            if (conflict(pin)) {
              invalidate_policy();
              return Result::Ambiguous;
            }
        for (const auto &pin : rule.pins)
          if (conflict(pin) || pin.leaf.matches(p.leaf)) {
            invalidate_policy();
            return Result::Ambiguous;
          }
        rule.pins.push_back({p.leaf, p.numeric_index, digest});
      }
      prepared->rules.push_back(std::move(rule));
    }
  } catch (const std::bad_alloc &) {
    invalidate_policy();
    return Result::AllocationFailure;
  } catch (...) {
    invalidate_policy();
    throw;
  }
  std::shared_ptr<const detail::Policy> retired;
  {
    std::lock_guard lock(mutex_);
    if (target_ && target_->state != Lifecycle::Ended) {
      const auto found = std::ranges::find_if(prepared->rules, [&](const auto &r) {
        return r.game == target_->game && r.app == target_->app;
      });
      if (found == prepared->rules.end())
        status = Result::BindingConflict;
    }
    retired.swap(policy_); // failed replacement denies old evidence, retains target/Unknown slot
    if (status == Result::Allowed)
      policy_ = std::move(prepared);
  }
  return status;
}
std::optional<TargetFact> AccessStore::bind_target(std::string_view game, std::string_view app, const RunScope &run) {
  if (!game_id(game) || !app_id(app) || !run || run.id().empty() || run.id().size() > max_identifier_bytes)
    return {};
  std::shared_ptr<const detail::Target> retired;
  std::lock_guard lock(mutex_);
  if (!policy_ || !std::ranges::any_of(policy_->rules, [&](const auto &r) { return r.game == game && r.app == app; }))
    return {};
  if (target_ && target_->state != Lifecycle::Ended &&
      (!target_->run.matches(run) || target_->game != game || target_->app != app ||
       target_->state == Lifecycle::Unknown))
    return {};
  auto target = std::make_shared<detail::Target>();
  target->store = identity_;
  target->game = game;
  target->app = app;
  target->run = run;
  retired = std::move(target_);
  target_ = std::move(target);
  return TargetFact(target_);
}
std::optional<TargetFact> AccessStore::set_lifecycle(const TargetFact &expected, Lifecycle state) {
  if (state != Lifecycle::Running && state != Lifecycle::Unknown && state != Lifecycle::Ended)
    return {};
  std::shared_ptr<const detail::Target> retired;
  std::lock_guard lock(mutex_);
  if (!expected.value_ || expected.value_ != target_ || expected.value_->store != identity_)
    return {};
  // No implicit recovery from Unknown or Ended; replacement after explicit Ended
  // is a new target fact, never revival of this lifecycle snapshot.
  if (target_->state != Lifecycle::Running && state != Lifecycle::Ended)
    return {};
  auto next = std::make_shared<detail::Target>(*target_);
  next->lifecycle = LifecycleRevision::fresh(0);
  next->state = state;
  retired = std::move(target_);
  target_ = std::move(next);
  return TargetFact(target_);
}
Evaluation AccessStore::evaluate(const Principal &principal, std::string_view game, std::string_view app) const {
  if (!principal || !game_id(game) || !app_id(app))
    return {};
  const auto digest = index_(principal.leaf());
  std::lock_guard lock(mutex_);
  if (!policy_)
    return {};
  if (target_ && (target_->state != Lifecycle::Running || target_->game != game || target_->app != app))
    return {};
  // Reject collisions even with a pin in another configured game. Neither the
  // digest index nor legacy numeric routing metadata grants identity.
  for (const auto &r : policy_->rules)
    for (const auto &pin : r.pins)
      if ((pin.digest == digest || pin.numeric == principal.numeric_index()) && !pin.leaf.matches(principal.leaf()))
        return {Result::Ambiguous, {}};
  for (std::size_t i = 0; i < policy_->rules.size(); ++i) {
    const auto &r = policy_->rules[i];
    if (r.game != game || r.app != app)
      continue;
    for (const auto &pin : r.pins)
      if (pin.digest == digest && pin.numeric == principal.numeric_index() && pin.leaf.matches(principal.leaf())) {
        Evidence evidence;
        evidence.store_ = identity_;
        evidence.policy_ = policy_;
        evidence.target_ = TargetFact(target_);
        evidence.principal_ = principal;
        evidence.rule_ = i;
        return {Result::Allowed, std::move(evidence)};
      }
  }
  return {};
}
bool AccessStore::is_current(const Evidence &evidence) const {
  std::lock_guard lock(mutex_);
  if (!policy_ || evidence.store_ != identity_ || evidence.policy_ != policy_ || evidence.target_.value_ != target_ ||
      !evidence.principal_ || evidence.rule_ >= policy_->rules.size())
    return false;
  const auto &r = policy_->rules[evidence.rule_];
  if (target_ && (target_->state != Lifecycle::Running || target_->game != r.game || target_->app != r.app))
    return false;
  return std::ranges::any_of(r.pins, [&](const auto &pin) {
    return pin.numeric == evidence.principal_.numeric_index() && pin.leaf.matches(evidence.principal_.leaf());
  });
}
} // namespace sessions::protected_access
