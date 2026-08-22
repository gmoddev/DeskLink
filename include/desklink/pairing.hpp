#pragma once

#include "desklink/capabilities.hpp"
#include "desklink/types.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace desklink {

inline constexpr std::size_t kSha256DigestSize = 32;
inline constexpr std::size_t kPairingNonceSize = 32;
inline constexpr std::size_t kMaxPairingDisplayName = 64;
inline constexpr std::size_t kMaxTrustedPeers = 64;

using Sha256Digest = std::array<std::uint8_t, kSha256DigestSize>;
using PairingNonce = std::array<std::uint8_t, kPairingNonceSize>;

struct PairingOffer {
    MachineId Machine{};
    std::string DisplayName;
    Sha256Digest CertificatePin{};
    PairingNonce Nonce{};
};

[[nodiscard]] bool IsValidPairingOffer(const PairingOffer& Offer) noexcept;

struct TrustedPeer {
    PeerIdentity Identity;
    CapabilitySet Capabilities;
};

class IPairingCrypto {
public:
    virtual ~IPairingCrypto() = default;
    [[nodiscard]] virtual bool FillRandom(std::span<std::uint8_t> Bytes) = 0;
    [[nodiscard]] virtual std::optional<Sha256Digest> HashSha256(ByteSpan Bytes) const = 0;
};

class ITrustStore {
public:
    virtual ~ITrustStore() = default;
    [[nodiscard]] virtual std::optional<TrustedPeer> GetPeer(const MachineId& Machine) const = 0;
    [[nodiscard]] virtual std::optional<TrustedPeer> FindPeerByFingerprint(
        std::string_view Fingerprint) const = 0;
    [[nodiscard]] virtual bool SavePeer(TrustedPeer Peer) = 0;
    [[nodiscard]] virtual bool RemovePeer(const MachineId& Machine) = 0;
};

class InMemoryTrustStore final : public ITrustStore {
public:
    [[nodiscard]] std::optional<TrustedPeer> GetPeer(const MachineId& Machine) const override;
    [[nodiscard]] std::optional<TrustedPeer> FindPeerByFingerprint(
        std::string_view Fingerprint) const override;
    [[nodiscard]] bool SavePeer(TrustedPeer Peer) override;
    [[nodiscard]] bool RemovePeer(const MachineId& Machine) override;

private:
    mutable std::mutex Mutex_;
    std::vector<TrustedPeer> Peers_;
};

enum class PairingStatus {
    Ready,
    WindowClosed,
    InvalidOffer,
    CryptoFailure,
};

struct PairingCandidate {
    PairingStatus Status{PairingStatus::InvalidOffer};
    PeerIdentity Identity;
    std::string VerificationCode;
};

class PairingCoordinator final {
public:
    PairingCoordinator(PeerIdentity LocalIdentity,
                       Sha256Digest LocalCertificatePin,
                       IClock& Clock,
                       IPairingCrypto& Crypto,
                       ITrustStore& TrustStore);

    [[nodiscard]] bool BeginPairing(std::chrono::seconds Duration);
    void ClosePairing();
    [[nodiscard]] bool IsPairingOpen() const;
    [[nodiscard]] std::optional<PairingOffer> CreateOffer() const;
    [[nodiscard]] PairingCandidate InspectOffer(const PairingOffer& RemoteOffer) const;
    [[nodiscard]] bool ConfirmOffer(const PairingOffer& RemoteOffer,
                                    std::string_view VerificationCode,
                                    CapabilitySet Capabilities);

private:
    PeerIdentity LocalIdentity_;
    Sha256Digest LocalCertificatePin_{};
    PairingNonce LocalNonce_{};
    IClock& Clock_;
    IPairingCrypto& Crypto_;
    ITrustStore& TrustStore_;
    IClock::time_point PairingDeadline_{};
    bool PairingOpen_{};
    mutable std::recursive_mutex Mutex_;
};

[[nodiscard]] std::string FormatFingerprint(const Sha256Digest& Digest);
[[nodiscard]] std::optional<Sha256Digest> ParseFingerprint(std::string_view Fingerprint);
[[nodiscard]] std::optional<TrustedPeer> MatchPeerCertificate(
    const ITrustStore& TrustStore,
    const IPairingCrypto& Crypto,
    ByteSpan CertificateDer,
    const MachineId* ExpectedMachine = nullptr);
[[nodiscard]] bool IsTrustedPeer(const ITrustStore& TrustStore,
                                 const PeerIdentity& Identity) noexcept;

} // namespace desklink
