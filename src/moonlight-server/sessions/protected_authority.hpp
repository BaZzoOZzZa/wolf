#pragma once
#ifdef __linux__
#include <control/input_seat.hpp>
#include <limits>
#include <span>
namespace sessions::protected_access {
class Principal;
class Evidence;
struct Evaluation;
struct RuleView;
} // namespace sessions::protected_access

namespace sessions::protected_authority {
namespace access = protected_access;
struct DomainTag;
struct OperatorTag;
struct PairingTag;
struct UpdateTag;
using DomainIdentity = wolf::core::sessions::Scope<unsigned, DomainTag>;
using OperatorCapability = wolf::core::sessions::Scope<unsigned, OperatorTag>;
using PairingRevision = wolf::core::sessions::Scope<unsigned, PairingTag>;
using UpdateIdentity = wolf::core::sessions::Scope<unsigned, UpdateTag>;

// Sealed 3A data only: diagnostic handles never retain arbitrary callbacks.
class Facts {
  struct Data;
  const std::unique_ptr<const Data> data_;
  Facts(std::unique_ptr<const Data>, control::RunScope, PairingRevision);

public:
  ~Facts();
  const control::RunScope target_run;
  const PairingRevision pairing;
  static std::shared_ptr<const Facts> prepare(std::span<const access::RuleView>,
                                              std::string_view game,
                                              std::string_view app,
                                              control::RunScope,
                                              PairingRevision);
  access::Evaluation evaluate(const access::Principal &) const; // outside Seat gate
  bool locally_current(const access::Evidence &) const;         // diagnostic, NOT authority
};
using FactHandle = std::shared_ptr<const Facts>;
// Optional trusted provider/resource retirement. Invocation and capture destruction
// are lifecycle work, never performed under gate. Throw means unproved release.
struct RetirementResource {
  virtual ~RetirementResource() = default;
  virtual void retire() = 0;
};
struct PreparedFacts {
  FactHandle facts;
  std::unique_ptr<RetirementResource> resource;
};
class ProtectedAuthorityDomain;
class Permit {
  friend class ProtectedAuthorityDomain;
  friend class Cell;
  const DomainIdentity domain_;
  const FactHandle facts_;
  const uint64_t sequence_;
  const std::shared_ptr<const access::Evidence> evidence_;
  Permit(DomainIdentity domain,
         FactHandle facts,
         uint64_t sequence,
         std::shared_ptr<const access::Evidence> evidence,
         control::SeatCandidate candidate,
         control::DeviceGeneration device)
      : domain_(std::move(domain)), facts_(std::move(facts)), sequence_(sequence), evidence_(std::move(evidence)),
        candidate(std::move(candidate)), device(std::move(device)) {}

public:
  const control::SeatCandidate candidate;
  const control::DeviceGeneration device;
};
using PermitHandle = std::shared_ptr<const Permit>;
class UpdateToken {
  friend class ProtectedAuthorityDomain;
  DomainIdentity domain_;
  FactHandle expected_;
  uint64_t sequence_ = 0;
  UpdateIdentity identity_;

public:
  UpdateToken() = default;
};
enum class Result {
  Accepted,
  Stale,
  Busy,
  Denied,
  Unknown
};
struct UpdateResult {
  Result result;
  std::shared_ptr<const UpdateToken> token;
};
struct Status {
  bool installed, closed, quarantined, cleanup_pending, update_pending;
  uint64_t sequence;
};

// Internal gate data. No owner/worker/Seat back-reference. The pure *_locked
// methods are used by actual Seat/Handoff, not by ingress or public inspectors.
class Cell {
  friend class ProtectedAuthorityDomain;
  friend class Retirement;
  DomainIdentity domain_ = DomainIdentity::fresh(0);
  OperatorCapability operator_;
  FactHandle facts_;
  std::unique_ptr<RetirementResource> resource_, failed_resource_;
  std::shared_ptr<const UpdateToken> update_;
  uint64_t sequence_ = 0, sequence_limit_;
  bool closed_ = false, cleanup_ = false, cleanup_failed_ = false, sequence_exhausted_ = false;
  PermitHandle bootstrap_, route_, pending_;

public:
  Cell(PreparedFacts prepared, OperatorCapability cap, uint64_t limit)
      : operator_(std::move(cap)), facts_(std::move(prepared.facts)), resource_(std::move(prepared.resource)),
        sequence_limit_(limit) {}
  static bool same(const control::SeatCandidate &a, const control::SeatCandidate &b) {
    return a.source.matches(b.source) && a.destination.matches(b.destination) && a.connection.matches(b.connection) &&
           a.peer.matches(b.peer) && a.binding.matches(b.binding) && a.slot == b.slot;
  }
  bool permits_locked(const PermitHandle &p,
                      const control::SeatCandidate &candidate,
                      const control::DeviceGeneration &device) const {
    return !closed_ && p && p->domain_.matches(domain_) && facts_ && p->facts_ == facts_ && p->sequence_ == sequence_ &&
           same(p->candidate, candidate) && p->device.matches(device);
  }
  bool cleanup_locked() const {
    return cleanup_ || cleanup_failed_;
  }
  PermitHandle prepare_bootstrap(const control::DeviceGeneration &device) const {
    // bootstrap_ immutable after pre-factory publication; no store/callback work.
    const auto &p = bootstrap_;
    return p ? PermitHandle(new Permit(p->domain_, p->facts_, p->sequence_, p->evidence_, p->candidate, device))
             : nullptr;
  }
  bool bootstrap_locked(const PermitHandle &p, const control::SeatCandidate &c, const control::DeviceGeneration &d) {
    if (cleanup_locked() || !permits_locked(p, c, d))
      return false;
    return true;
  }
  void pending_locked(const PermitHandle &p) {
    pending_ = p;
  }
  void commit_locked(const PermitHandle &p) {
    route_ = p;
    pending_.reset();
  }
};

// Declare BEFORE every owned temporary payload. Ack follows real destruction,
// not intent-to-release. A failed retirement permanently retains the occupied cap.
class Retirement {
  std::shared_ptr<control::seat_detail::Gate> gate_;
  bool reserved_ = false, failed_ = false;

public:
  explicit Retirement(std::shared_ptr<control::seat_detail::Gate> gate) : gate_(std::move(gate)) {}
  Retirement(const Retirement &) = delete;
  bool reserve_locked() {
    if (!gate_->authority)
      return true;
    auto &c = *gate_->authority;
    if (c.cleanup_ || c.cleanup_failed_) {
      gate_->close(true);
      return false;
    }
    c.cleanup_ = reserved_ = true;
    return true;
  }
  void failed() {
    failed_ = true;
  }
  ~Retirement() {
    if (!reserved_)
      return;
    std::lock_guard lock(gate_->mutex);
    auto &c = *gate_->authority;
    if (!c.cleanup_ || failed_) {
      c.cleanup_failed_ = true;
      gate_->close(true);
    } else
      c.cleanup_ = false;
    gate_->wake.notify_all();
  }
};

// Trusted C++ capability owner, not a TLS implementation or wire permission.
// create_seat/start/destruction are single lifecycle-owner operations. All other
// calls may race; slow preparation/publication retirement must stay off ingress.
// Optional synchronous internal scheduling probe; no retained capture/permission value.
struct CaptureProbe {
  void *context = nullptr;
  void (*after_evaluation)(void *) = nullptr;
};
class ProtectedAuthorityDomain {
  const std::shared_ptr<control::seat_detail::Gate> gate_;
  std::shared_ptr<control::InputSeat> seat_;
  bool reserved_ = false;

public:
  ProtectedAuthorityDomain(PreparedFacts,
                           OperatorCapability,
                           uint64_t sequence_limit = std::numeric_limits<uint64_t>::max());
  ~ProtectedAuthorityDomain(); // lifecycle owner only; close before releasing owned Seat
  std::shared_ptr<control::InputSeat> create_seat(control::SeatCandidate,
                                                  const access::Principal &,
                                                  control::ProtectedInventory,
                                                  control::InputSeat::Factory,
                                                  control::SeatValidation additional = {},
                                                  control::SeatPolicy = {},
                                                  control::SeatThreadFactory = control::start_seat_thread);
  FactHandle facts() const;
  PermitHandle route() const; // historical binding inspection, never permission
  Status status() const;
  PermitHandle capture_selection(const control::SeatCandidate &, const access::Principal &) const;
  PermitHandle capture_selection(const control::SeatCandidate &,
                                 const access::Principal &,
                                 const FactHandle &expected,
                                 const PairingRevision &,
                                 CaptureProbe = {}) const;
  // Typed facts-only execution prerequisite, not permission or a recovery proof.
  bool publication_ready(const OperatorCapability &, const std::shared_ptr<const UpdateToken> &) const;
  UpdateResult begin_update(const OperatorCapability &, const FactHandle &expected);
  bool close_exact(const OperatorCapability &, const FactHandle &expected);
  bool close_exact(const control::ConnectionScope &, const control::PeerToken &);
  Result publish_prepared(const std::shared_ptr<const UpdateToken> &, PreparedFacts);
};
} // namespace sessions::protected_authority
#endif
