#pragma once

#include "desklink/roaming.hpp"
#include "desklink/topology_exchange.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace desklink {

inline constexpr std::chrono::milliseconds kRoamingFocusTimeout{1'500};
inline constexpr std::uint16_t kRoamingMinimumOutwardIntentPermyriad{6'000};
inline constexpr std::uint16_t kRoamingPhysicalSpanTolerancePermyriad{500};
inline constexpr std::uint16_t kRoamingPhysicalSpanToleranceMillimeters{5};
inline constexpr std::uint16_t kRoamingMinimumPhysicalSegmentMillimeters{25};

enum class RoamingRuntimeState : std::uint8_t {
    Local,
    EdgeCandidate,
    FocusPending,
    RemoteReady,
    Remote,
    ReturnPending,
    LocalCooldown,
};

struct LocalPointerObservation {
    std::int32_t ScreenX{};
    std::int32_t ScreenY{};
    std::int32_t DeltaX{};
    std::int32_t DeltaY{};
};

struct RoamingRouteKey {
    std::size_t LinkIndex{};
    RoamingDirection Direction{RoamingDirection::AToB};

    [[nodiscard]] bool operator==(
        const RoamingRouteKey&) const noexcept = default;
};

struct RoamingLocalLanding {
    std::int32_t ScreenX{};
    std::int32_t ScreenY{};

    [[nodiscard]] bool operator==(
        const RoamingLocalLanding&) const noexcept = default;
};

struct RoamingFocusRequest {
    RoamingRouteKey Route;
    RoamingLink ConfiguredLink;
    MachineId PeerMachine{};
    std::uint64_t SessionNonce{};
    std::uint64_t SourceTopologyGeneration{};
    std::uint64_t TargetTopologyGeneration{};
    PointerPositionMessage Landing;
    RoamingLocalLanding LocalReturnLanding;

    [[nodiscard]] bool operator==(
        const RoamingFocusRequest& Other) const noexcept {
        return Route == Other.Route &&
               ConfiguredLink == Other.ConfiguredLink &&
               PeerMachine == Other.PeerMachine &&
               SessionNonce == Other.SessionNonce &&
               SourceTopologyGeneration ==
                   Other.SourceTopologyGeneration &&
               TargetTopologyGeneration ==
                   Other.TargetTopologyGeneration &&
               Landing.display_id == Other.Landing.display_id &&
               Landing.normalized_x == Other.Landing.normalized_x &&
               Landing.normalized_y == Other.Landing.normalized_y &&
               LocalReturnLanding == Other.LocalReturnLanding;
    }
};

struct RoamingRuntimeContext {
    MachineId LocalMachine{};
    MachineId PeerMachine{};
    RoamingConfiguration Configuration;
    std::optional<DisplayTopologySnapshot> LocalTopology;
    std::optional<DisplayTopologySnapshot> PeerTopology;
    PeerConnectionStatus PeerStatus{PeerConnectionStatus::Offline};
    DisplayTopologyExchangeStatus TopologyStatus{
        DisplayTopologyExchangeStatus::Offline};
    std::uint64_t SessionNonce{};
    bool PeerValidated{};
    bool InputCapabilityGranted{};
    bool DirectionSupported{};
};

struct RoamingContextUpdate {
    bool Valid{};
    bool MustFailLocal{};
    std::size_t ReadyRouteCount{};
};

class RoamingRuntime final {
public:
    explicit RoamingRuntime(const IClock& Clock) noexcept;
    ~RoamingRuntime();

    [[nodiscard]] RoamingContextUpdate UpdateContext(
        RoamingRuntimeContext Context);
    [[nodiscard]] std::optional<RoamingFocusRequest> Observe(
        LocalPointerObservation Observation) noexcept;
    [[nodiscard]] bool AdmitFocusReady(
        const RoamingFocusRequest& Request) noexcept;
    [[nodiscard]] bool AdmitRemoteInput(
        const RoamingFocusRequest& Request) noexcept;
    // Accepts the first authenticated, epoch-bound observation on the exact
    // configured remote edge. Landing inset and the local re-entry latch keep
    // this immediate return from bouncing between displays.
    [[nodiscard]] bool ObserveRemotePointer(
        PointerPositionFeedbackMessage Position) noexcept;
    [[nodiscard]] bool ExpireFocusPending() noexcept;
    void BeginReturn() noexcept;
    void ReturnLocal() noexcept;
    void FailLocal() noexcept;
    // Clears the spatial re-entry latch only after the platform confirms that
    // the returned local pointer is safely inside the original source display.
    [[nodiscard]] bool ConfirmLocalReturnLanding(
        RoamingLocalLanding Landing) noexcept;

    [[nodiscard]] RoamingRuntimeState State() const noexcept;
    [[nodiscard]] std::optional<RoamingFocusRequest>
    ActiveRequest() const noexcept;

private:
    struct ReadyRoute;
    struct Candidate;
    struct Cooldown;

    [[nodiscard]] bool ActiveRequestRemainsReady() const noexcept;
    void CancelCandidate() noexcept;
    void EnterCooldown() noexcept;

    const IClock& Clock_;
    RoamingRuntimeContext Context_;
    std::vector<ReadyRoute> ReadyRoutes_;
    std::unique_ptr<Candidate> Candidate_;
    std::unique_ptr<Cooldown> Cooldown_;
    std::optional<RoamingFocusRequest> ActiveRequest_;
    IClock::time_point FocusRequestedAt_{};
    RoamingRuntimeState State_{RoamingRuntimeState::Local};
    bool ContextValid_{};
};

enum class PeerDirectionState : std::uint8_t {
    Local,
    OutgoingPending,
    OutgoingActive,
    IncomingActive,
};

enum class PeerDirectionOutcome : std::uint8_t {
    Admitted,
    RejectedBusy,
    RejectedUnbound,
    CollisionFailLocal,
};

struct PeerDirectionToken {
    MachineId PeerMachine{};
    std::uint64_t SessionNonce{};
    std::uint64_t Generation{};
    bool Outgoing{};

    [[nodiscard]] bool operator==(
        const PeerDirectionToken&) const noexcept = default;
};

struct PeerDirectionDecision {
    PeerDirectionOutcome Outcome{PeerDirectionOutcome::RejectedBusy};
    std::optional<PeerDirectionToken> Token;
};

class PeerDirectionArbiter final {
public:
    [[nodiscard]] bool BindSession(
        MachineId PeerMachine, std::uint64_t SessionNonce) noexcept;
    void ResetSession() noexcept;

    [[nodiscard]] PeerDirectionDecision BeginOutgoing() noexcept;
    [[nodiscard]] PeerDirectionDecision BeginIncoming() noexcept;
    [[nodiscard]] bool AdmitOutgoing(PeerDirectionToken Token) noexcept;
    [[nodiscard]] bool Release(PeerDirectionToken Token) noexcept;
    void FailLocal() noexcept;

    [[nodiscard]] PeerDirectionState State() const noexcept;
    [[nodiscard]] bool BoundTo(
        const MachineId& PeerMachine,
        std::uint64_t SessionNonce) const noexcept;

private:
    [[nodiscard]] PeerDirectionToken NewToken(bool Outgoing) noexcept;

    MachineId PeerMachine_{};
    std::uint64_t SessionNonce_{};
    PeerDirectionState State_{PeerDirectionState::Local};
    std::uint64_t Generation_{1};
    std::optional<PeerDirectionToken> ActiveToken_;
};

} // namespace desklink
