#pragma once

#include "desklink/pairing.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace desklink {

enum class TrustMutationStatus : std::uint8_t {
    Applied = 0,
    NoChange = 1,
    ReauthorizationRequired = 2,
    InvalidRequest = 3,
    PeerNotFound = 4,
    CleanupFailed = 5,
    StoreFailed = 6,
};

// Only a definitive not-found result proves that no runtime owner exists.
// Every other lookup outcome is active or unknown and must fail closed.
[[nodiscard]] constexpr bool RuntimeOwnerMayBeActive(
    bool HandleOpened, bool WasNotFound) noexcept {
    return HandleOpened || !WasNotFound;
}

class IRuntimeSafetyController {
public:
    virtual ~IRuntimeSafetyController() = default;

    // The implementation must return input Local, release owned input state,
    // and close the affected authenticated session before returning true.
    [[nodiscard]] virtual bool ReturnLocalAndStopPeer(
        const MachineId& Machine) noexcept = 0;
};

class RuntimeTrustAuthority final {
public:
    RuntimeTrustAuthority(ITrustStore& TrustStore,
                          IRuntimeSafetyController& SafetyController) noexcept;

    [[nodiscard]] std::optional<std::vector<TrustedPeer>> ListTrustedPeers() const;
    [[nodiscard]] TrustMutationStatus RequestPermissionChange(
        const MachineId& Machine, CapabilitySet DesiredCapabilities);
    [[nodiscard]] TrustMutationStatus ForgetPeer(const MachineId& Machine);

private:
    ITrustStore& TrustStore_;
    IRuntimeSafetyController& SafetyController_;
    mutable std::mutex Mutex_;
};

using BrokerClientId = std::uint64_t;
using BrokerPairingRequestId = std::uint64_t;

struct BrokerPairingCandidate {
    BrokerPairingRequestId RequestId{};
    BrokerClientId OwnerClient{};
    PairingCandidate Candidate;
    CapabilitySet RequestedCapabilities;
    IClock::time_point ExpiresAt{};
};

class BrokerPairingCandidateLease final {
public:
    [[nodiscard]] bool Present(BrokerPairingCandidate Candidate,
                               IClock::time_point Now);
    [[nodiscard]] std::optional<BrokerPairingCandidate> Current(
        IClock::time_point Now);
    [[nodiscard]] std::optional<BrokerPairingCandidate> ResolveLocally(
        BrokerPairingRequestId RequestId, bool Approved,
        IClock::time_point Now);
    void Reject(BrokerPairingRequestId RequestId) noexcept;
    void ClientDisconnected(BrokerClientId Client) noexcept;

private:
    void ExpireLocked(IClock::time_point Now) noexcept;

    std::mutex Mutex_;
    std::optional<BrokerPairingCandidate> Candidate_;
};

} // namespace desklink
