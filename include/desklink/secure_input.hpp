#pragma once

#include "desklink/types.hpp"

#include <array>
#include <chrono>
#include <cstdint>

namespace desklink {

using CertificateDerHash = std::array<std::uint8_t, 32>;

enum class SecureInputOperation : std::uint8_t {
    ReleaseOwnedState = 0,
    Key = 1,
    MouseButton = 2,
    PointerMotion = 3,
    Wheel = 4,
};

struct SecureInputGrant {
    MachineId PeerMachine{};
    CertificateDerHash PeerCertificateDerHash{};
    std::uint64_t SessionNonce{};
    std::uint64_t Epoch{};
    std::uint64_t GrantRevision{};
    bool AllowSecureDesktopInput{};
};

struct SecureInputEnvelope {
    MachineId PeerMachine{};
    CertificateDerHash PeerCertificateDerHash{};
    std::uint64_t SessionNonce{};
    std::uint64_t Epoch{};
    std::uint64_t GrantRevision{};
    std::uint64_t Sequence{};
    SecureInputOperation Operation{SecureInputOperation::ReleaseOwnedState};
};

enum class SecureInputDecision : std::uint8_t {
    Accepted = 0,
    RejectedNoGrant,
    RejectedGrant,
    RejectedIdentity,
    RejectedSession,
    RejectedEpoch,
    RejectedSequence,
    RejectedExpired,
    RejectedOperation,
};

// This gate is deliberately independent of the transport session. A future
// privileged broker must populate it only after independently authenticating
// the fixed, signed DeskLink runtime and reading its own protected grant.
class SecureInputAuthorizationGate final {
public:
    explicit SecureInputAuthorizationGate(const IClock& Clock) noexcept;

    [[nodiscard]] bool Authorize(
        const SecureInputGrant& Grant,
        std::chrono::milliseconds Lease) noexcept;
    [[nodiscard]] SecureInputDecision Admit(
        const SecureInputEnvelope& Envelope) noexcept;
    void Revoke() noexcept;

    [[nodiscard]] bool Authorized() const noexcept;
    [[nodiscard]] std::uint64_t LastAcceptedSequence() const noexcept {
        return LastAcceptedSequence_;
    }

private:
    [[nodiscard]] bool Expired() const noexcept;

    const IClock& Clock_;
    SecureInputGrant Grant_{};
    IClock::time_point ExpiresAt_{};
    std::uint64_t LastAcceptedSequence_{};
    std::uint64_t HighestGrantRevision_{};
    bool Active_{};
};

[[nodiscard]] bool IsValidSecureInputGrant(
    const SecureInputGrant& Grant) noexcept;
[[nodiscard]] bool IsValidSecureInputOperation(
    SecureInputOperation Operation) noexcept;

} // namespace desklink
