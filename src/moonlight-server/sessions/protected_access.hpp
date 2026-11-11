#pragma once

#include <array>
#include <crypto/crypto.hpp>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <sessions/connection_scope.hpp>
#include <span>
#include <string_view>

namespace sessions::protected_access {
// Internal model limits, not global Moonlight wire limits or process RSS caps.
inline constexpr std::size_t max_der_bytes = 16 * 1024;
inline constexpr std::size_t max_pem_bytes = 24 * 1024;
inline constexpr std::size_t max_games = 32;
inline constexpr std::size_t max_pin_entries = 64;
inline constexpr std::size_t max_policy_der_bytes = 256 * 1024;
inline constexpr std::size_t max_identifier_bytes = 64;
using Digest = std::array<unsigned char, 32>;
using RunScope = wolf::core::sessions::RunScope;
struct ConfigTag;
struct AuthTag;
struct BindingTag;
struct LifecycleTag;
using ConfigRevision = wolf::core::sessions::Scope<unsigned, ConfigTag>;
using AuthRevision = wolf::core::sessions::Scope<unsigned, AuthTag>;
using BindingRevision = wolf::core::sessions::Scope<unsigned, BindingTag>;
using LifecycleRevision = wolf::core::sessions::Scope<unsigned, LifecycleTag>;

class Leaf {
  struct Data;
  std::shared_ptr<const Data> data_;
  explicit Leaf(std::shared_ptr<const Data>);

public:
  Leaf() = default;
  static std::optional<Leaf> from_pem(std::string_view);
  static std::optional<Leaf> from_der(std::span<const unsigned char>);
  static std::optional<Leaf> from_x509(const x509::x509_ptr &);
  explicit operator bool() const;
  std::span<const unsigned char> der() const;
  Digest digest() const;
  bool matches(const Leaf &) const; // complete DER, null never matches
};

// Trusted, stable snapshot from a future TLS/pairing adapter. NOT a wire identity
// or bool "approved". The adapter must already have verified TLS key possession
// and resolved the paired leaf; this facts-only module performs neither step.
struct VerifiedLeafSnapshot {
  x509::x509_ptr presented, paired;
  std::uint64_t numeric_index = 0; // lookup metadata only; collisions fail closed
};
class VerifiedIdentitySource {
public:
  virtual ~VerifiedIdentitySource() = default;
  virtual VerifiedLeafSnapshot verified_leaf() const = 0;
};
class Principal {
  Leaf leaf_;
  std::uint64_t numeric_index_ = 0;
  Principal(Leaf, std::uint64_t);

public:
  Principal() = default;
  static std::optional<Principal> capture(const VerifiedIdentitySource &);
  explicit operator bool() const;
  const Leaf &leaf() const;
  std::uint64_t numeric_index() const;
};

// Operator-owned inputs. No parser, config writes, wildcard or paired-all mode.
struct PinView {
  Leaf leaf;
  std::uint64_t numeric_index;
};
struct RuleView {
  std::string_view game_key, catalog_appid;
  std::span<const PinView> allow;
};
enum class Result {
  Allowed,
  Denied,
  Invalid,
  Capacity,
  Ambiguous,
  BindingConflict,
  Stale,
  AllocationFailure
};
enum class Lifecycle {
  Running,
  Unknown,
  Ended
};
namespace detail {
struct Policy;
struct Target;
struct StoreIdentity {};
} // namespace detail
class TargetFact {
  friend class AccessStore;
  friend class Evidence;
  std::shared_ptr<const detail::Target> value_;
  explicit TargetFact(std::shared_ptr<const detail::Target>);

public:
  TargetFact() = default;
  explicit operator bool() const;
  RunScope run() const;
  BindingRevision binding_revision() const;
  LifecycleRevision lifecycle_revision() const;
};
class Evidence {
  friend class AccessStore;
  std::shared_ptr<const detail::StoreIdentity> store_;
  std::shared_ptr<const detail::Policy> policy_;
  TargetFact target_;
  Principal principal_;
  std::size_t rule_ = 0;

public:
  Evidence() = default;
  bool historical_allowed() const;
  std::string_view game_key() const;
  std::string_view catalog_appid() const;
  ConfigRevision config_revision() const;
  AuthRevision auth_revision() const;
  const Principal &principal() const;
  const TargetFact &target() const;
};
struct Evaluation {
  Result result = Result::Denied;
  Evidence evidence;
};

class AccessStore {
public:
  // Narrow deterministic collision seam, no captured callable ownership. Default
  // uses real SHA256. Even a colliding index cannot bypass complete DER checks.
  using DigestIndex = Digest (*)(const Leaf &);
  static Digest sha256_index(const Leaf &);
  explicit AccessStore(DigestIndex index = sha256_index);
  Result replace_policy(std::span<const RuleView>);
  void invalidate_policy(); // trusted notification; no implicit grant/reconciliation
  std::optional<TargetFact> bind_target(std::string_view game_key, std::string_view catalog_appid, const RunScope &);
  std::optional<TargetFact> set_lifecycle(const TargetFact &expected, Lifecycle);
  Evaluation evaluate(const Principal &, std::string_view game_key, std::string_view catalog_appid) const;
  // This store's current state only. Caller cannot supply a "current snapshot".
  bool is_current(const Evidence &) const;

private:
  const DigestIndex index_;
  const std::shared_ptr<const detail::StoreIdentity> identity_;
  mutable std::mutex mutex_;
  std::shared_ptr<const detail::Policy> policy_;
  std::shared_ptr<const detail::Target> target_; // max1; Unknown retains this slot
};
} // namespace sessions::protected_access
