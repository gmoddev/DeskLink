#pragma once

#include "desklink/pairing.hpp"
#include "desklink/product.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

namespace desklink {

inline constexpr auto kBrokerReconnectMinimumDelay =
    std::chrono::milliseconds{800};
inline constexpr auto kBrokerReconnectMaximumDelay =
    std::chrono::seconds{30};
inline constexpr std::uint32_t kBrokerManagedRetryableProcessExit = 64;
inline constexpr std::uint32_t kBrokerManagedActionRequiredProcessExit = 65;
inline constexpr std::uint32_t kBrokerManagedProtocolProcessExit = 66;

enum class BrokerRuntimePhase : std::uint8_t {
    Stopped = 0,
    Paused = 1,
    Listening = 2,
    Discovering = 3,
    Connecting = 4,
    ConnectedLocal = 5,
    RetryWaiting = 6,
    ActionRequired = 7,
};

// The child owns live peer/focus/input state only after the supervisor has
// observed an admitted connection. During discovery, launch, and retry the
// runtime mutex can exist before the child's control pipe is ready; querying
// it then must not block the broker's always-available state endpoint.
[[nodiscard]] constexpr bool ShouldQueryManagedRuntimeState(
    BrokerRuntimePhase Phase) noexcept {
    return Phase == BrokerRuntimePhase::ConnectedLocal;
}

enum class BrokerRuntimeFailure : std::uint8_t {
    None = 0,
    OrdinaryUnavailable = 1,
    Security = 2,
    Identity = 3,
    Credential = 4,
    Signing = 5,
    Authentication = 6,
    Capability = 7,
    Protocol = 8,
    Unknown = 9,
};

struct BrokerRuntimeSnapshot {
    BrokerRuntimePhase Phase{BrokerRuntimePhase::Stopped};
    BrokerRuntimeFailure Failure{BrokerRuntimeFailure::None};
    std::uint16_t RetryAttempt{};
    IClock::time_point RetryAt{};
    bool Paused{};
    bool SystemSuspended{};
};

[[nodiscard]] constexpr bool IsRetryableBrokerRuntimeFailure(
    BrokerRuntimeFailure Failure) noexcept {
    return Failure == BrokerRuntimeFailure::OrdinaryUnavailable;
}

[[nodiscard]] std::chrono::milliseconds BrokerReconnectDelay(
    std::uint16_t Attempt, std::uint64_t JitterSeed) noexcept;

// Exit 64 is reserved for a child that positively classified an ordinary
// availability failure. Typed terminal exits preserve enough bounded context
// for the product shell to explain why retry stopped without exposing session
// data. Every unknown non-success exit remains terminal.
[[nodiscard]] constexpr BrokerRuntimeFailure
ClassifyBrokerManagedProcessExit(std::uint32_t ExitCode) noexcept {
    switch (ExitCode) {
        case kBrokerManagedRetryableProcessExit:
            return BrokerRuntimeFailure::OrdinaryUnavailable;
        case kBrokerManagedProtocolProcessExit:
            return BrokerRuntimeFailure::Protocol;
        default:
            return BrokerRuntimeFailure::Unknown;
    }
}

// Pure lifecycle controller for one broker-owned transport process. Only
// ordinary availability/network failures may schedule another attempt.
// Security, identity, credential, signing, authentication, capability,
// protocol, and unknown failures remain Local in ActionRequired until an
// explicit Resume or configuration change resets the state.
class BrokerReconnectController final {
public:
    explicit BrokerReconnectController(std::uint64_t JitterSeed) noexcept;

    void ResetForConfiguration(IClock::time_point Now) noexcept;
    void Pause(IClock::time_point Now) noexcept;
    void Resume(IClock::time_point Now) noexcept;
    void SystemSuspend(IClock::time_point Now) noexcept;
    void SystemResume(IClock::time_point Now) noexcept;
    [[nodiscard]] bool Begin(BrokerRuntimePhase Phase) noexcept;
    void ConnectedLocal() noexcept;
    void ProcessStopped(BrokerRuntimeFailure Failure,
                        IClock::time_point Now) noexcept;
    void NetworkChanged(IClock::time_point Now) noexcept;
    [[nodiscard]] bool AttemptDue(IClock::time_point Now) const noexcept;
    [[nodiscard]] BrokerRuntimeSnapshot Snapshot() const noexcept;

private:
    std::uint64_t JitterSeed_{};
    BrokerRuntimeSnapshot State_;
    std::optional<BrokerRuntimeSnapshot> StateBeforeSystemSuspend_;
};

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

    // Permission changes first return both directions Local, then reload the
    // exact persisted grant over the already-authenticated session. A failed
    // reload must fall back to the stop path below.
    [[nodiscard]] virtual bool ReturnLocalForPeer(
        const MachineId& Machine) noexcept = 0;
    [[nodiscard]] virtual bool RefreshPeerCapabilities(
        const MachineId& Machine) noexcept = 0;

    // Forget and failed renegotiation still require full fail-local cleanup
    // and closure of the affected authenticated session.
    [[nodiscard]] virtual bool ReturnLocalAndStopPeer(
        const MachineId& Machine) noexcept = 0;
};

class RuntimeTrustAuthority final {
public:
    RuntimeTrustAuthority(ITrustStore& TrustStore,
                          IRuntimeSafetyController& SafetyController,
                          std::function<bool()> Reload = {}) noexcept;

    [[nodiscard]] std::optional<std::vector<TrustedPeer>> ListTrustedPeers() const;
    [[nodiscard]] TrustMutationStatus RequestPermissionChange(
        const MachineId& Machine, CapabilitySet DesiredCapabilities);
    // This is intentionally separate from the generic reduction API. Callers
    // must first complete a broker-owned, reject-default local authorization
    // ceremony and provide the exact identity/capability snapshot that was
    // reviewed. The stored identity is never replaced by this operation.
    [[nodiscard]] TrustMutationStatus ApplyReauthorizedPermissionChange(
        const PeerIdentity& ExpectedIdentity,
        CapabilitySet ExpectedCapabilities,
        CapabilitySet DesiredCapabilities);
    [[nodiscard]] TrustMutationStatus ForgetPeer(const MachineId& Machine);
    [[nodiscard]] bool ReloadAfterExternalPairing();

private:
    ITrustStore& TrustStore_;
    IRuntimeSafetyController& SafetyController_;
    std::function<bool()> Reload_;
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

using BrokerPermissionRequestId = std::uint64_t;

struct BrokerPermissionCandidate {
    BrokerPermissionRequestId RequestId{};
    PeerIdentity Identity;
    CapabilitySet CurrentCapabilities;
    CapabilitySet DesiredCapabilities;
    IClock::time_point ExpiresAt{};
};

// One short-lived permission candidate may be reviewed at a time. The lease
// carries the immutable peer identity snapshot so approval cannot be replayed
// after trust replacement or another permission mutation.
class BrokerPermissionCandidateLease final {
public:
    [[nodiscard]] bool Present(BrokerPermissionCandidate Candidate,
                               IClock::time_point Now);
    [[nodiscard]] std::optional<BrokerPermissionCandidate> Current(
        IClock::time_point Now);
    [[nodiscard]] std::optional<BrokerPermissionCandidate> ResolveLocally(
        BrokerPermissionRequestId RequestId, bool Approved,
        IClock::time_point Now);
    void Reject(BrokerPermissionRequestId RequestId) noexcept;
    void RejectAll() noexcept;

private:
    void ExpireLocked(IClock::time_point Now) noexcept;

    std::mutex Mutex_;
    std::optional<BrokerPermissionCandidate> Candidate_;
};

} // namespace desklink
