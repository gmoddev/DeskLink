#pragma once

#include "desklink/protocol.hpp"
#include "desklink/roaming.hpp"

#include <chrono>
#include <optional>
#include <span>

namespace desklink {

inline constexpr std::chrono::milliseconds kDisplayTopologyExchangeTimeout{
    5'000};
inline constexpr std::chrono::milliseconds kDisplayTopologyPublishInterval{
    2'000};

enum class PeerConnectionStatus {
    Offline,
    Discovering,
    Connecting,
    Authenticating,
    SynchronizingTopology,
    Connected,
    Reconnecting,
    UserActionRequired,
};

enum class DisplayTopologyExchangeStatus {
    Offline,
    Disabled,
    CapabilityMissing,
    Synchronizing,
    Ready,
    TimedOut,
    Rejected,
};

enum class RoamingRouteStatus {
    Ready,
    SynchronizingTopology,
    DisplayMissing,
    CapabilityMissing,
    DirectionUnsupported,
    Disabled,
    Invalid,
};

enum class RoamingDirection {
    AToB,
    BToA,
};

class DisplayTopologyExchangeTracker final {
public:
    explicit DisplayTopologyExchangeTracker(
        const IClock* Clock = nullptr) noexcept;

    void Begin(const MachineId& ExpectedPeer,
               std::uint64_t SessionNonce,
               bool Enabled,
               bool CapabilityGranted) noexcept;
    void Stop() noexcept;
    [[nodiscard]] bool Admit(
        const EnvelopeHeader& Header,
        const DisplayTopologySnapshotMessage& Message);
    void RejectMalformed() noexcept;
    [[nodiscard]] DisplayTopologyExchangeStatus Status() const noexcept;
    [[nodiscard]] bool Authorized() const noexcept;
    [[nodiscard]] std::optional<DisplayTopologySnapshot> Snapshot() const;

private:
    [[nodiscard]] IClock::time_point Now() const noexcept;
    void Reject() noexcept;

    const IClock* Clock_{};
    MachineId ExpectedPeer_{};
    std::uint64_t SessionNonce_{};
    IClock::time_point Deadline_{};
    std::optional<DisplayTopologySnapshot> Snapshot_;
    DisplayTopologyExchangeStatus Status_{
        DisplayTopologyExchangeStatus::Offline};
};

[[nodiscard]] RoamingRouteStatus EvaluateRoamingRoute(
    const RoamingLink& Link,
    RoamingDirection Direction,
    PeerConnectionStatus PeerStatus,
    DisplayTopologyExchangeStatus TopologyStatus,
    bool CapabilityGranted,
    bool DirectionSupported,
    std::span<const MachineDisplayTopology> Topologies);

} // namespace desklink
