#pragma once
#ifdef __linux__
#include <rest/rest.hpp>
#include <sessions/protected_access.hpp>
#include <sessions/protected_authority.hpp>

namespace sessions::protected_identity {
namespace access = protected_access;
using PairingRevision = protected_authority::PairingRevision;
using Request = SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request;
using RequestPtr = std::shared_ptr<Request>;
enum class Error {
  None,
  Invalid,
  Capacity,
  Ambiguous,
  Stale,
  Unpaired,
  ProviderFailure,
  AllocationFailure
};
template <typename T> struct Result {
  Error error;
  std::optional<T> value;
};
// Function pointers only, synchronous trusted seams; never retained by sealed data.
// Defaults preserve the existing client numeric index and real SHA256 digest.
struct Indexers {
  std::uint64_t (*numeric)(const std::string &);
  access::Digest (*digest)(const access::Leaf &);
  static Indexers defaults();
};
struct PeerCertificateProvider {
  void *context = nullptr; // borrowed for capture call only; no escaping owner graph
  x509::x509_ptr (*get)(void *, const RequestPtr &) = nullptr;
  static PeerCertificateProvider defaults(); // actual HTTPS request getter, no socket creation
};
class VerifiedHttpsIdentity;
class PairedSnapshot {
  friend class VerifiedHttpsIdentity;
  struct Data;
  std::shared_ptr<const Data> data_;
  explicit PairedSnapshot(std::shared_ptr<const Data> data) : data_(std::move(data)) {}

public:
  PairedSnapshot() = default;
  // Trusted caller supplies its owned immutable paired snapshot AND existing revision.
  // No load-then-mint epoch and no current-authority claim. Parsing occurs off all gates.
  static Result<PairedSnapshot>
  seal(const state::PairedClientList &, PairingRevision, Indexers = Indexers::defaults()) noexcept;
  PairingRevision revision() const;
  std::size_t size() const;
  // Data-only sealed membership: full DER and matching numeric metadata.
  // Historical snapshot membership is not current authority or TLS possession.
  bool contains(const access::Principal &) const;
};
class VerifiedHttpsIdentity {
  access::Leaf presented_, paired_;
  access::Principal principal_;
  PairingRevision revision_;
  VerifiedHttpsIdentity(access::Leaf presented,
                        access::Leaf paired,
                        access::Principal principal,
                        PairingRevision revision)
      : presented_(std::move(presented)), paired_(std::move(paired)), principal_(std::move(principal)),
        revision_(std::move(revision)) {}

public:
  // Internal trusted C++ boundary. Injection changes retrieval only, not DER/verification.
  // Provider must not mutate its certificate concurrently during synchronous capture.
  // Caller must not invoke SSL getter concurrently with that connection's SSL operations.
  static Result<VerifiedHttpsIdentity> capture(const RequestPtr &,
                                               const PairedSnapshot &,
                                               const PairingRevision &expected,
                                               PeerCertificateProvider = PeerCertificateProvider::defaults()) noexcept;
  const access::Leaf &presented() const {
    return presented_;
  }
  const access::Leaf &paired() const {
    return paired_;
  }
  const access::Principal &principal() const {
    return principal_;
  }
  const PairingRevision &revision() const {
    return revision_;
  }
};
} // namespace sessions::protected_identity
#endif
