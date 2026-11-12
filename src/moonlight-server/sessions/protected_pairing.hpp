#pragma once
#ifdef __linux__
#include <sessions/protected_identity.hpp>
#include <sessions/protected_runtime.hpp>

namespace sessions::protected_pairing {
namespace authority = protected_authority;
namespace identity = protected_identity;
namespace access = protected_access;
struct OwnerTag;
struct PublicationTag;
using OwnerIdentity = wolf::core::sessions::Scope<unsigned, OwnerTag>;
using PublicationIdentity = wolf::core::sessions::Scope<unsigned, PublicationTag>;
class PairingMutationOwner;
class Publication {
  friend class PairingMutationOwner;
  Publication(OwnerIdentity,
              state::PairedClientList,
              authority::PairingRevision,
              identity::PairedSnapshot,
              authority::FactHandle);

public:
  const OwnerIdentity owner;
  const PublicationIdentity identity;
  const state::PairedClientList source; // exact immutable persistent snapshot, never a mutable atom
  const authority::PairingRevision revision;
  const identity::PairedSnapshot sealed; // absent for ClosedEmpty
  const authority::FactHandle facts;
};
using PublicationHandle = std::shared_ptr<const Publication>;
class Update {
  friend class PairingMutationOwner;
  Update() = default;
  OwnerIdentity owner_;
  authority::UpdateIdentity identity_;
  uint64_t sequence_ = 0;
  PublicationHandle source_, prepared_;
  authority::PairingRevision revision_;
  std::shared_ptr<const authority::UpdateToken> token_;
  bool preparation_claimed_ = false;
};
using UpdateHandle = std::shared_ptr<const Update>;
enum class Result {
  Accepted,
  Busy,
  Stale,
  Invalid,
  Unknown
};
enum class Phase {
  Current,
  Closing,
  Preparing,
  Retiring,
  ClosedEmpty,
  Unknown
};
struct BeginResult {
  Result result;
  UpdateHandle update;
};
struct Status {
  Phase phase;
  uint64_t sequence;
  bool occupied;
};
class Transaction;
enum class PersistenceResult {
  Checked,
  Failed,
  Unknown
};
class PersistenceOutcome {
  friend class Transaction;
  const OwnerIdentity owner_;
  const authority::UpdateIdentity update_;
  const uint64_t sequence_;
  const PublicationHandle source_, result_;
  PersistenceOutcome(
      OwnerIdentity, authority::UpdateIdentity, uint64_t, PublicationHandle, PublicationHandle, PersistenceResult);

public:
  const PersistenceResult result;
};
class Transaction {
  friend class PairingMutationOwner;
  Transaction(OwnerIdentity, authority::UpdateIdentity, uint64_t, PublicationHandle, PublicationHandle);
  const OwnerIdentity owner_;
  const authority::UpdateIdentity update_;
  const uint64_t sequence_;

public:
  const PublicationHandle source, result;
  // Trusted B1 SOFTWARE operation outcome. This is NOT a disk durability proof.
  PersistenceOutcome outcome(PersistenceResult) const;
  bool matches(const PersistenceOutcome &) const;
};
// Trusted provider may own persistence resources, NEVER this owner/Runtime or a callback back to them.
// Destruction must be noexcept; arbitrary external capture graphs are outside the logical byte cap.
struct PersistenceOperation {
  virtual ~PersistenceOperation() = default;
  virtual PersistenceOutcome execute(const Transaction &) = 0;
};
struct PublicationReceipt {
  std::atomic<RuntimeJobState> state{RuntimeJobState::Queued};
  std::atomic<authority::Result> authority_result{authority::Result::Denied};
};
// Structured private payload. No arbitrary callable or owner back-reference.
// Provider is uniquely owned until full command retirement; references in tx are data only.
class PublicationCommand {
  friend class PairingMutationOwner;
  friend class ::sessions::ProtectedRuntimeOwner;
  PublicationCommand() = default;
  authority::OperatorCapability op_;
  std::shared_ptr<const authority::UpdateToken> token_;
  authority::PreparedFacts facts_;
  std::shared_ptr<const Transaction> transaction_;
  std::unique_ptr<PersistenceOperation> operation_;
  std::shared_ptr<PublicationReceipt> receipt_;
};
class PairingMutationOwner {
  const OwnerIdentity identity_ = OwnerIdentity::fresh(0);
  const authority::OperatorCapability op_;
  const std::shared_ptr<ProtectedRuntimeOwner> runtime_;
  const std::string game_, app_;
  const uint64_t sequence_limit_;
  mutable std::mutex mutex_;
  uint64_t sequence_ = 0;
  Phase phase_ = Phase::Current;
  PublicationHandle current_;
  std::shared_ptr<Update> pending_;
  std::shared_ptr<PublicationReceipt> receipt_;
  static bool bounded(const state::PairedClientList &);
  PublicationHandle prepare_publication(const state::PairedClientList &,
                                        std::span<const access::RuleView>,
                                        const control::RunScope &,
                                        authority::PairingRevision) const;

public:
  // Lifecycle only. Builds revision/facts before SAME Runtime/Seat worker bootstrap.
  // No retry after start failure; Runtime's reserved cap is preserved.
  PairingMutationOwner(std::shared_ptr<ProtectedRuntimeOwner>,
                       const state::PairedClientList &,
                       std::span<const access::RuleView>,
                       std::string_view game,
                       std::string_view app,
                       RuntimeBootstrap,
                       uint64_t sequence_limit = std::numeric_limits<uint64_t>::max());
  ~PairingMutationOwner(); // lifecycle only; retained Runtime may join, never an ingress callback
  PublicationHandle capture_publication() const;
  Status status();                        // drives existing deadline decision, never provider cleanup
  bool ready(const UpdateHandle &) const; // observation, not permission or reservation
  void poll();                            // data-only receipt -> tuple exchange, no grant/recovery
  BeginResult begin_update(const authority::OperatorCapability &, const PublicationHandle &);
  bool close_exact(const authority::OperatorCapability &, const PublicationHandle &);
  PublicationHandle prepare(const UpdateHandle &, const state::PairedClientList &, std::span<const access::RuleView>);
  Result submit(const UpdateHandle &,
                const PublicationHandle &,
                std::unique_ptr<PersistenceOperation>,
                std::unique_ptr<authority::RetirementResource> = {});
  identity::Result<identity::VerifiedHttpsIdentity>
  capture_https(const identity::RequestPtr &,
                const PublicationHandle &,
                identity::PeerCertificateProvider = identity::PeerCertificateProvider::defaults()) const;
  authority::PermitHandle capture_selection(const control::SeatCandidate &,
                                            const identity::VerifiedHttpsIdentity &,
                                            const PublicationHandle &,
                                            authority::CaptureProbe = {}) const;
};
} // namespace sessions::protected_pairing
#endif
