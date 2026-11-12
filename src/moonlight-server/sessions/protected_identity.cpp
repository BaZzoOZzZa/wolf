#include "protected_identity.hpp"
#ifdef __linux__
#include <vector>

namespace sessions::protected_identity {
struct PairedSnapshot::Data {
  struct Record {
    access::Leaf leaf;
    std::uint64_t numeric;
    access::Digest digest;
  };
  PairingRevision revision;
  std::vector<Record> records;
};
Indexers Indexers::defaults() {
  return {+[](const std::string &pem) -> std::uint64_t { return std::hash<std::string>{}(pem); },
          +[](const access::Leaf &leaf) { return leaf.digest(); }};
}
PeerCertificateProvider PeerCertificateProvider::defaults() {
  return {nullptr, +[](void *, const RequestPtr &request) { return HttpsServer::get_client_cert(request); }};
}
Result<PairedSnapshot>
PairedSnapshot::seal(const state::PairedClientList &clients, PairingRevision revision, Indexers indexes) noexcept {
  try {
    if (!revision || !indexes.numeric || !indexes.digest || clients.empty())
      return {Error::Invalid, {}};
    if (clients.size() > access::max_pin_entries)
      return {Error::Capacity, {}};
    std::size_t pem_total = 0;
    for (const auto &client : clients) {
      const auto size = client->client_cert.size();
      if (size > access::max_pem_bytes || size > access::max_policy_der_bytes - pem_total)
        return {Error::Capacity, {}};
      pem_total += size;
    }
    auto data = std::make_shared<Data>();
    data->revision = std::move(revision);
    data->records.reserve(clients.size());
    std::size_t der_total = 0;
    for (const auto &client : clients) {
      auto leaf = access::Leaf::from_pem(client->client_cert);
      if (!leaf)
        return {Error::Invalid, {}};
      if (leaf->der().size() > access::max_policy_der_bytes - der_total)
        return {Error::Capacity, {}};
      der_total += leaf->der().size();
      const auto numeric = indexes.numeric(client->client_cert);
      const auto digest = indexes.digest(*leaf);
      for (const auto &old : data->records)
        if (old.numeric == numeric || old.digest == digest || old.leaf.matches(*leaf))
          return {Error::Ambiguous, {}};
      data->records.push_back({std::move(*leaf), numeric, digest});
    }
    return {Error::None, PairedSnapshot(std::move(data))};
  } catch (const std::bad_alloc &) {
    return {Error::AllocationFailure, {}};
  } catch (...) {
    return {Error::Invalid, {}};
  }
}
PairingRevision PairedSnapshot::revision() const {
  return data_ ? data_->revision : PairingRevision{};
}
std::size_t PairedSnapshot::size() const {
  return data_ ? data_->records.size() : 0;
}
namespace {
// Reconstitute ephemeral verifier objects ONLY from already bounded canonical DER.
// No mutable X509 aliases survive capture; reuse the public verifier via Principal.
x509::x509_ptr certificate(const access::Leaf &leaf) {
  auto bytes = leaf.der();
  const auto *cursor = bytes.data();
  x509::x509_ptr result(d2i_X509(nullptr, &cursor, static_cast<long>(bytes.size())), X509_free);
  return result && cursor == bytes.data() + bytes.size() ? result : x509::x509_ptr{};
}
struct Source final : access::VerifiedIdentitySource {
  access::VerifiedLeafSnapshot snapshot;
  explicit Source(access::VerifiedLeafSnapshot snapshot) : snapshot(std::move(snapshot)) {}
  access::VerifiedLeafSnapshot verified_leaf() const override {
    return snapshot;
  }
};
} // namespace
Result<VerifiedHttpsIdentity> VerifiedHttpsIdentity::capture(const RequestPtr &request,
                                                             const PairedSnapshot &paired,
                                                             const PairingRevision &expected,
                                                             PeerCertificateProvider provider) noexcept {
  try {
    if (!expected || !paired.data_ || !paired.data_->revision.matches(expected))
      return {Error::Stale, {}};
    if (!request || !provider.get)
      return {Error::Invalid, {}};
    x509::x509_ptr peer;
    try {
      peer = provider.get(provider.context, request);
    } catch (const std::bad_alloc &) {
      return {Error::AllocationFailure, {}};
    } catch (...) {
      return {Error::ProviderFailure, {}};
    }
    const auto presented = access::Leaf::from_x509(peer);
    if (!presented)
      return {Error::Unpaired, {}};
    const PairedSnapshot::Data::Record *match = nullptr;
    for (const auto &record : paired.data_->records) {
      // Indexes never authorize; inspect full DER for every bounded record.
      if (record.leaf.matches(*presented)) {
        if (match)
          return {Error::Ambiguous, {}};
        match = &record;
      }
    }
    if (!match)
      return {Error::Unpaired, {}};
    Source source({certificate(*presented), certificate(match->leaf), match->numeric});
    auto principal = access::Principal::capture(source); // actual public x509 verifier + DER primitives
    if (!principal)
      return {Error::Unpaired, {}};
    return {Error::None, VerifiedHttpsIdentity(*presented, match->leaf, std::move(*principal), expected)};
  } catch (const std::bad_alloc &) {
    return {Error::AllocationFailure, {}};
  } catch (...) {
    return {Error::Invalid, {}};
  }
}
} // namespace sessions::protected_identity
#endif
