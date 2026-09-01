#include "desklink/roaming_runtime.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <utility>

namespace desklink {
namespace {

[[nodiscard]] bool IsNonzeroMachine(const MachineId& Machine) noexcept {
    return std::any_of(Machine.begin(), Machine.end(), [](std::uint8_t Byte) {
        return Byte != 0;
    });
}

[[nodiscard]] bool DirectionConfigured(
    const RoamingLink& Link, RoamingDirection Direction) noexcept {
    return Direction == RoamingDirection::AToB
        ? Link.Direction == RoamingDirectionMode::AToB ||
              Link.Direction == RoamingDirectionMode::Bidirectional
        : Link.Direction == RoamingDirectionMode::BToA ||
              Link.Direction == RoamingDirectionMode::Bidirectional;
}

[[nodiscard]] const RoamingEndpoint& SourceEndpoint(
    const RoamingLink& Link, RoamingDirection Direction) noexcept {
    return Direction == RoamingDirection::AToB
        ? Link.EndpointA : Link.EndpointB;
}

[[nodiscard]] const RoamingEndpoint& TargetEndpoint(
    const RoamingLink& Link, RoamingDirection Direction) noexcept {
    return Direction == RoamingDirection::AToB
        ? Link.EndpointB : Link.EndpointA;
}

[[nodiscard]] const CrossingConfiguration& CrossingFor(
    const RoamingLink& Link, RoamingDirection Direction) noexcept {
    return Direction == RoamingDirection::AToB ? Link.AToB : Link.BToA;
}

[[nodiscard]] std::int32_t BoundedDisplayDimension(
    std::int32_t Start, std::int32_t End) noexcept {
    const auto Dimension = static_cast<std::int64_t>(End) - Start;
    if (Dimension <= 0 ||
        Dimension > static_cast<std::int64_t>(
            kMaximumDisplayPixelDimension)) {
        return 0;
    }
    return static_cast<std::int32_t>(Dimension);
}

[[nodiscard]] std::int32_t DisplayWidth(
    const DisplayDescriptor& Display) noexcept {
    return BoundedDisplayDimension(
        Display.Bounds.Left, Display.Bounds.Right);
}

[[nodiscard]] std::int32_t DisplayHeight(
    const DisplayDescriptor& Display) noexcept {
    return BoundedDisplayDimension(
        Display.Bounds.Top, Display.Bounds.Bottom);
}

[[nodiscard]] bool Contains(
    const DisplayDescriptor& Display, std::int32_t X,
    std::int32_t Y) noexcept {
    return X >= Display.Bounds.Left && X < Display.Bounds.Right &&
           Y >= Display.Bounds.Top && Y < Display.Bounds.Bottom;
}

[[nodiscard]] std::int32_t AlongCoordinate(
    const DisplayDescriptor& Display, DisplayEdgeSide Side,
    const LocalPointerObservation& Observation) noexcept {
    return Side == DisplayEdgeSide::Left || Side == DisplayEdgeSide::Right
        ? Observation.ScreenY - Display.Bounds.Top
        : Observation.ScreenX - Display.Bounds.Left;
}

[[nodiscard]] std::int32_t AlongLength(
    const DisplayDescriptor& Display, DisplayEdgeSide Side) noexcept {
    return Side == DisplayEdgeSide::Left || Side == DisplayEdgeSide::Right
        ? DisplayHeight(Display) : DisplayWidth(Display);
}

[[nodiscard]] std::int64_t OutwardDelta(
    DisplayEdgeSide Side, const LocalPointerObservation& Observation) noexcept {
    switch (Side) {
        case DisplayEdgeSide::Left:
            return -static_cast<std::int64_t>(Observation.DeltaX);
        case DisplayEdgeSide::Top:
            return -static_cast<std::int64_t>(Observation.DeltaY);
        case DisplayEdgeSide::Right: return Observation.DeltaX;
        case DisplayEdgeSide::Bottom: return Observation.DeltaY;
    }
    return 0;
}

[[nodiscard]] std::int64_t AbsoluteDelta(std::int32_t Value) noexcept {
    const auto Wide = static_cast<std::int64_t>(Value);
    return Wide < 0 ? -Wide : Wide;
}

[[nodiscard]] std::int64_t LateralDelta(
    DisplayEdgeSide Side, const LocalPointerObservation& Observation) noexcept {
    return Side == DisplayEdgeSide::Left || Side == DisplayEdgeSide::Right
        ? AbsoluteDelta(Observation.DeltaY)
        : AbsoluteDelta(Observation.DeltaX);
}

[[nodiscard]] bool HasOutwardIntent(
    std::int64_t Outward, std::int64_t Lateral) noexcept {
    if (Outward <= 0 || Lateral < 0) return false;
    const auto Total = Outward + Lateral;
    return Outward * 10'000 >=
        Total * kRoamingMinimumOutwardIntentPermyriad;
}

[[nodiscard]] bool AtEdge(
    const DisplayDescriptor& Display, DisplayEdgeSide Side,
    const LocalPointerObservation& Observation) noexcept {
    switch (Side) {
        case DisplayEdgeSide::Left:
            return Observation.ScreenX <= Display.Bounds.Left;
        case DisplayEdgeSide::Top:
            return Observation.ScreenY <= Display.Bounds.Top;
        case DisplayEdgeSide::Right:
            return Observation.ScreenX >= Display.Bounds.Right - 1;
        case DisplayEdgeSide::Bottom:
            return Observation.ScreenY >= Display.Bounds.Bottom - 1;
    }
    return false;
}

[[nodiscard]] std::uint16_t NormalizeAlong(
    std::int32_t Coordinate, std::int32_t Length) noexcept {
    if (Length <= 1) return 0;
    const auto Clamped = std::clamp(Coordinate, 0, Length - 1);
    return static_cast<std::uint16_t>(
        static_cast<std::int64_t>(Clamped) * 10'000 / (Length - 1));
}

[[nodiscard]] bool InSegment(
    std::uint16_t Along, const RoamingEndpoint& Endpoint) noexcept {
    return Along >= Endpoint.SegmentStartPermyriad &&
           (Along < Endpoint.SegmentEndPermyriad ||
            Endpoint.SegmentEndPermyriad == 10'000);
}

[[nodiscard]] std::int32_t PermyriadToPixel(
    std::uint16_t Value, std::int32_t Length) noexcept {
    if (Length <= 1) return 0;
    return static_cast<std::int32_t>(
        (static_cast<std::int64_t>(Value) * (Length - 1) + 5'000) /
        10'000);
}

[[nodiscard]] std::optional<std::uint16_t> PhysicalAlongMillimeters(
    const DisplayDescriptor& Display, DisplayEdgeSide Side) noexcept {
    if (Display.PhysicalSize != PhysicalSizeSource::Edid ||
        Display.Orientation != DisplayOrientation::Landscape) {
        return std::nullopt;
    }
    const auto Millimeters =
        Side == DisplayEdgeSide::Left || Side == DisplayEdgeSide::Right
        ? Display.PhysicalHeightMillimeters
        : Display.PhysicalWidthMillimeters;
    if (Millimeters == 0 ||
        Millimeters > kMaximumPhysicalDisplayMillimeters) {
        return std::nullopt;
    }
    return Millimeters;
}

[[nodiscard]] std::optional<std::uint16_t> BuildPhysicalLandingHint(
    const RoamingEndpoint& Source,
    const RoamingEndpoint& Target,
    const DisplayDescriptor& SourceDisplay,
    const DisplayDescriptor& TargetDisplay,
    std::uint16_t SourceAlong) noexcept {
    const auto SourceMillimeters = PhysicalAlongMillimeters(
        SourceDisplay, Source.Side);
    const auto TargetMillimeters = PhysicalAlongMillimeters(
        TargetDisplay, Target.Side);
    if (!SourceMillimeters || !TargetMillimeters) return std::nullopt;

    const auto SourceSpan = static_cast<std::uint64_t>(
        Source.SegmentEndPermyriad - Source.SegmentStartPermyriad) *
        *SourceMillimeters;
    const auto TargetSpan = static_cast<std::uint64_t>(
        Target.SegmentEndPermyriad - Target.SegmentStartPermyriad) *
        *TargetMillimeters;
    constexpr std::uint64_t PermyriadPerMillimeter = 10'000;
    const auto MinimumSpan = static_cast<std::uint64_t>(
        kRoamingMinimumPhysicalSegmentMillimeters) *
        PermyriadPerMillimeter;
    if (SourceSpan < MinimumSpan || TargetSpan < MinimumSpan) {
        return std::nullopt;
    }
    const auto MaximumSpan = std::max(SourceSpan, TargetSpan);
    const auto Difference = SourceSpan > TargetSpan
        ? SourceSpan - TargetSpan : TargetSpan - SourceSpan;
    const auto Tolerance = std::max(
        static_cast<std::uint64_t>(
            kRoamingPhysicalSpanToleranceMillimeters) *
            PermyriadPerMillimeter,
        MaximumSpan * kRoamingPhysicalSpanTolerancePermyriad / 10'000u);
    if (Difference > Tolerance) return std::nullopt;

    const auto SourcePosition = static_cast<std::uint64_t>(SourceAlong) *
        *SourceMillimeters;
    const auto SourceStart = static_cast<std::uint64_t>(
        Source.SegmentStartPermyriad) * *SourceMillimeters;
    if (SourcePosition < SourceStart) return std::nullopt;
    const auto TargetStart = static_cast<std::uint64_t>(
        Target.SegmentStartPermyriad) * *TargetMillimeters;
    const auto TargetEnd = static_cast<std::uint64_t>(
        Target.SegmentEndPermyriad) * *TargetMillimeters;
    const auto TargetPosition = TargetStart +
        (SourcePosition - SourceStart);
    if (TargetPosition < TargetStart || TargetPosition > TargetEnd) {
        return std::nullopt;
    }
    const auto TargetAlong =
        (TargetPosition + *TargetMillimeters / 2u) / *TargetMillimeters;
    if (TargetAlong > 10'000u) return std::nullopt;
    return static_cast<std::uint16_t>(TargetAlong);
}

[[nodiscard]] std::uint16_t PixelToNormalized(
    std::int32_t Pixel, std::int32_t Length) noexcept {
    if (Length <= 1) return 0;
    const auto Clamped = std::clamp(Pixel, 0, Length - 1);
    return static_cast<std::uint16_t>(
        (static_cast<std::int64_t>(Clamped) * 65'535 +
         (Length - 1) / 2) / (Length - 1));
}

[[nodiscard]] std::optional<PointerPositionMessage> BuildLanding(
    const RoamingEndpoint& Source,
    const RoamingEndpoint& Target,
    const DisplayDescriptor& SourceDisplay,
    const DisplayDescriptor& TargetDisplay,
    const RoamingLink& Link,
    const LocalPointerObservation& Observation) noexcept {
    const auto SourceLength = AlongLength(SourceDisplay, Source.Side);
    const auto TargetLength = AlongLength(TargetDisplay, Target.Side);
    const auto TargetWidth = DisplayWidth(TargetDisplay);
    const auto TargetHeight = DisplayHeight(TargetDisplay);
    if (SourceLength <= 1 || TargetLength <= 1 ||
        TargetWidth <= 1 || TargetHeight <= 1) {
        return std::nullopt;
    }
    const auto SourceAlong = NormalizeAlong(
        AlongCoordinate(SourceDisplay, Source.Side, Observation),
        SourceLength);
    if (!InSegment(SourceAlong, Source)) return std::nullopt;

    const auto SourceSpan = static_cast<std::uint32_t>(
        Source.SegmentEndPermyriad - Source.SegmentStartPermyriad);
    const auto Relative = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(
             SourceAlong - Source.SegmentStartPermyriad) * 10'000u +
         SourceSpan / 2u) / SourceSpan);
    const auto TargetStart = PermyriadToPixel(
        Target.SegmentStartPermyriad, TargetLength);
    const auto TargetEnd = PermyriadToPixel(
        Target.SegmentEndPermyriad, TargetLength);
    const auto Clearance = static_cast<std::int32_t>(
        Link.CornerClearancePixels);
    if (TargetEnd - TargetStart <= Clearance * 2) {
        return std::nullopt;
    }
    const auto PhysicalHint = BuildPhysicalLandingHint(
        Source, Target, SourceDisplay, TargetDisplay, SourceAlong);
    auto TargetAlong = PhysicalHint
        ? PermyriadToPixel(*PhysicalHint, TargetLength)
        : TargetStart + static_cast<std::int32_t>(
              (static_cast<std::int64_t>(TargetEnd - TargetStart) * Relative +
               5'000) / 10'000);
    TargetAlong = std::clamp(
        TargetAlong, TargetStart + Clearance, TargetEnd - Clearance);

    const auto Inset = static_cast<std::int32_t>(Link.LandingInsetPixels);
    if (((Target.Side == DisplayEdgeSide::Left ||
          Target.Side == DisplayEdgeSide::Right) && TargetWidth <= Inset) ||
        ((Target.Side == DisplayEdgeSide::Top ||
          Target.Side == DisplayEdgeSide::Bottom) && TargetHeight <= Inset)) {
        return std::nullopt;
    }
    std::int32_t TargetX{};
    std::int32_t TargetY{};
    switch (Target.Side) {
        case DisplayEdgeSide::Left:
            TargetX = Inset;
            TargetY = TargetAlong;
            break;
        case DisplayEdgeSide::Top:
            TargetX = TargetAlong;
            TargetY = Inset;
            break;
        case DisplayEdgeSide::Right:
            TargetX = TargetWidth - 1 - Inset;
            TargetY = TargetAlong;
            break;
        case DisplayEdgeSide::Bottom:
            TargetX = TargetAlong;
            TargetY = TargetHeight - 1 - Inset;
            break;
    }
    return PointerPositionMessage{
        TargetDisplay.Id,
        PixelToNormalized(TargetX, TargetWidth),
        PixelToNormalized(TargetY, TargetHeight),
    };
}

} // namespace

struct RoamingRuntime::ReadyRoute {
    RoamingRouteKey Key;
    RoamingLink Link;
    ResolvedRoamingEndpoint Source;
    ResolvedRoamingEndpoint Target;
    DisplayDescriptor SourceDisplay;
    DisplayDescriptor TargetDisplay;
};

struct RoamingRuntime::Candidate {
    ReadyRoute Route;
    std::uint64_t SessionNonce{};
    IClock::time_point Started{};
    IClock::time_point FirstPush{};
    std::int64_t OutwardDistance{};
    std::int64_t LateralDistance{};
    bool FirstPushComplete{};
    bool SawInward{};
};

struct RoamingRuntime::Cooldown {
    RoamingEndpoint Source;
    DisplayDescriptor Display;
    std::uint16_t RequiredDistance{};
    std::int64_t InwardDistance{};
};

RoamingRuntime::RoamingRuntime(const IClock& Clock) noexcept : Clock_(Clock) {}

RoamingRuntime::~RoamingRuntime() = default;

RoamingContextUpdate RoamingRuntime::UpdateContext(
    RoamingRuntimeContext Context) {
    RoamingContextUpdate Result;
    const auto PreviouslyActive = ActiveRequest_.has_value();
    Context_ = std::move(Context);
    ReadyRoutes_.clear();
    ContextValid_ =
        IsNonzeroMachine(Context_.LocalMachine) &&
        IsNonzeroMachine(Context_.PeerMachine) &&
        Context_.LocalMachine != Context_.PeerMachine &&
        Context_.PeerValidated && Context_.SessionNonce != 0 &&
        IsValidRoamingConfiguration(Context_.Configuration) &&
        Context_.LocalTopology && Context_.PeerTopology &&
        IsValidDisplayTopologySnapshot(*Context_.LocalTopology) &&
        IsValidDisplayTopologySnapshot(*Context_.PeerTopology);
    if (ContextValid_) {
        const std::array Topologies{
            MachineDisplayTopology{
                Context_.LocalMachine, &*Context_.LocalTopology},
            MachineDisplayTopology{
                Context_.PeerMachine, &*Context_.PeerTopology},
        };
        for (std::size_t Index = 0;
             Index < Context_.Configuration.Links.size(); ++Index) {
            const auto& Link = Context_.Configuration.Links[Index];
            for (const auto Direction :
                 {RoamingDirection::AToB, RoamingDirection::BToA}) {
                if (!DirectionConfigured(Link, Direction)) continue;
                const auto& Source = SourceEndpoint(Link, Direction);
                const auto& Target = TargetEndpoint(Link, Direction);
                if (Source.Machine != Context_.LocalMachine ||
                    Target.Machine != Context_.PeerMachine ||
                    EvaluateRoamingRoute(
                        Link, Direction, Context_.PeerStatus,
                        Context_.TopologyStatus,
                        Context_.InputCapabilityGranted,
                        Context_.DirectionSupported, Topologies) !=
                        RoamingRouteStatus::Ready) {
                    continue;
                }
                const auto Resolved = ResolveRoamingLink(Link, Topologies);
                const auto& ResolvedSource = Direction == RoamingDirection::AToB
                    ? Resolved.EndpointA : Resolved.EndpointB;
                const auto& ResolvedTarget = Direction == RoamingDirection::AToB
                    ? Resolved.EndpointB : Resolved.EndpointA;
                if (!ResolvedSource.Endpoint || !ResolvedTarget.Endpoint) {
                    continue;
                }
                const auto* SourceDisplay = Context_.LocalTopology->Find(
                    ResolvedSource.Endpoint->Display);
                const auto* TargetDisplay = Context_.PeerTopology->Find(
                    ResolvedTarget.Endpoint->Display);
                if (!SourceDisplay || !TargetDisplay) continue;
                const auto TargetLength = AlongLength(
                    *TargetDisplay, Target.Side);
                const auto TargetStart = PermyriadToPixel(
                    Target.SegmentStartPermyriad, TargetLength);
                const auto TargetEnd = PermyriadToPixel(
                    Target.SegmentEndPermyriad, TargetLength);
                const auto TargetWidth = DisplayWidth(*TargetDisplay);
                const auto TargetHeight = DisplayHeight(*TargetDisplay);
                const auto Clearance = static_cast<std::int32_t>(
                    Link.CornerClearancePixels);
                const auto Inset = static_cast<std::int32_t>(
                    Link.LandingInsetPixels);
                const auto TargetHasInset =
                    ((Target.Side == DisplayEdgeSide::Left ||
                      Target.Side == DisplayEdgeSide::Right) &&
                     TargetWidth > Inset) ||
                    ((Target.Side == DisplayEdgeSide::Top ||
                      Target.Side == DisplayEdgeSide::Bottom) &&
                     TargetHeight > Inset);
                if (TargetLength <= 1 || TargetWidth <= 1 ||
                    TargetHeight <= 1 ||
                    TargetEnd - TargetStart <= Clearance * 2 ||
                    !TargetHasInset) {
                    continue;
                }
                ReadyRoutes_.push_back(ReadyRoute{
                    {Index, Direction}, Link,
                    *ResolvedSource.Endpoint, *ResolvedTarget.Endpoint,
                    *SourceDisplay, *TargetDisplay});
            }
        }
    }
    Result.Valid = ContextValid_;
    Result.ReadyRouteCount = ReadyRoutes_.size();
    if (PreviouslyActive && !ActiveRequestRemainsReady()) {
        EnterCooldown();
        Result.MustFailLocal = true;
    } else if (State_ == RoamingRuntimeState::EdgeCandidate &&
               Candidate_ && std::none_of(
                   ReadyRoutes_.begin(), ReadyRoutes_.end(),
                   [&](const ReadyRoute& Route) {
                       return Route.Key == Candidate_->Route.Key &&
                              Route.Link == Candidate_->Route.Link &&
                              Route.Source == Candidate_->Route.Source &&
                              Route.Target == Candidate_->Route.Target &&
                              Candidate_->SessionNonce ==
                                  Context_.SessionNonce;
                   })) {
        CancelCandidate();
    }
    return Result;
}

std::optional<RoamingFocusRequest> RoamingRuntime::Observe(
    LocalPointerObservation Observation) noexcept {
    if (State_ == RoamingRuntimeState::LocalCooldown) {
        if (!Cooldown_) {
            State_ = RoamingRuntimeState::Local;
        } else {
            const auto& Display = Cooldown_->Display;
            const auto Outward = OutwardDelta(
                Cooldown_->Source.Side, Observation);
            Cooldown_->InwardDistance +=
                std::max<std::int64_t>(0, -Outward);
            std::int64_t EdgeDistance{};
            switch (Cooldown_->Source.Side) {
                case DisplayEdgeSide::Left:
                    EdgeDistance =
                        static_cast<std::int64_t>(Observation.ScreenX) -
                        Display.Bounds.Left;
                    break;
                case DisplayEdgeSide::Top:
                    EdgeDistance =
                        static_cast<std::int64_t>(Observation.ScreenY) -
                        Display.Bounds.Top;
                    break;
                case DisplayEdgeSide::Right:
                    EdgeDistance =
                        static_cast<std::int64_t>(Display.Bounds.Right) - 1 -
                        Observation.ScreenX;
                    break;
                case DisplayEdgeSide::Bottom:
                    EdgeDistance =
                        static_cast<std::int64_t>(Display.Bounds.Bottom) - 1 -
                        Observation.ScreenY;
                    break;
            }
            if (Cooldown_->InwardDistance >= Cooldown_->RequiredDistance ||
                EdgeDistance >= Cooldown_->RequiredDistance) {
                Cooldown_.reset();
                State_ = RoamingRuntimeState::Local;
            }
        }
        return std::nullopt;
    }
    if (!ContextValid_ ||
        State_ == RoamingRuntimeState::FocusPending ||
        State_ == RoamingRuntimeState::RemoteReady ||
        State_ == RoamingRuntimeState::Remote ||
        State_ == RoamingRuntimeState::ReturnPending) {
        return std::nullopt;
    }

    const ReadyRoute* Matched{};
    for (const auto& Route : ReadyRoutes_) {
        if (!Contains(Route.SourceDisplay, Observation.ScreenX,
                      Observation.ScreenY) ||
            !AtEdge(Route.SourceDisplay, Route.Source.Side, Observation)) {
            continue;
        }
        const auto Along = NormalizeAlong(
            AlongCoordinate(
                Route.SourceDisplay, Route.Source.Side, Observation),
            AlongLength(Route.SourceDisplay, Route.Source.Side));
        if (!InSegment(Along, SourceEndpoint(
                Route.Link, Route.Key.Direction))) {
            continue;
        }
        Matched = &Route;
        break;
    }
    if (!Matched) {
        CancelCandidate();
        return std::nullopt;
    }
    if (!Candidate_ || Candidate_->Route.Key != Matched->Key ||
        Candidate_->Route.Link != Matched->Link) {
        Candidate_ = std::make_unique<Candidate>(Candidate{
            *Matched, Context_.SessionNonce, Clock_.now(), {}, 0,
            0, false, false});
        State_ = RoamingRuntimeState::EdgeCandidate;
    }
    auto& Candidate = *Candidate_;
    const auto Delta = OutwardDelta(Candidate.Route.Source.Side, Observation);
    Candidate.LateralDistance = std::min<std::int64_t>(
        Candidate.LateralDistance +
            LateralDelta(Candidate.Route.Source.Side, Observation),
        std::numeric_limits<std::uint16_t>::max());
    if (Delta < 0) {
        Candidate.SawInward = true;
        if (!Candidate.FirstPushComplete) {
            CancelCandidate();
            return std::nullopt;
        }
    } else if (Delta > 0) {
        Candidate.OutwardDistance = std::min<std::int64_t>(
            Candidate.OutwardDistance + Delta,
            std::numeric_limits<std::uint16_t>::max());
    }
    const auto& Crossing = CrossingFor(
        Candidate.Route.Link, Candidate.Route.Key.Direction);
    const auto IntentReady = HasOutwardIntent(
        Candidate.OutwardDistance, Candidate.LateralDistance);
    if (Candidate.LateralDistance >= Crossing.PushDistancePixels &&
        !IntentReady) {
        CancelCandidate();
        return std::nullopt;
    }
    const auto Now = Clock_.now();
    bool Triggered = false;
    switch (Crossing.Policy) {
        case CrossingPolicy::Push:
            Triggered = Candidate.OutwardDistance >=
                    Crossing.PushDistancePixels && IntentReady;
            break;
        case CrossingPolicy::DwellAndPush:
            Triggered = Candidate.OutwardDistance >=
                    Crossing.PushDistancePixels &&
                IntentReady &&
                Now - Candidate.Started >= std::chrono::milliseconds(
                    Crossing.DwellMilliseconds);
            break;
        case CrossingPolicy::DoublePush:
            if (!Candidate.FirstPushComplete &&
                Candidate.OutwardDistance >= Crossing.PushDistancePixels &&
                IntentReady) {
                Candidate.FirstPushComplete = true;
                Candidate.FirstPush = Now;
                Candidate.OutwardDistance = 0;
                Candidate.LateralDistance = 0;
                Candidate.SawInward = false;
            } else if (Candidate.FirstPushComplete &&
                       Now - Candidate.FirstPush > std::chrono::milliseconds(
                           Crossing.DoublePushWindowMilliseconds)) {
                CancelCandidate();
                return std::nullopt;
            } else if (Candidate.FirstPushComplete && Candidate.SawInward &&
                       Candidate.OutwardDistance >=
                           Crossing.PushDistancePixels && IntentReady) {
                Triggered = true;
            }
            break;
    }
    if (!Triggered) return std::nullopt;

    const auto& Source = SourceEndpoint(
        Candidate.Route.Link, Candidate.Route.Key.Direction);
    const auto& Target = TargetEndpoint(
        Candidate.Route.Link, Candidate.Route.Key.Direction);
    const auto Landing = BuildLanding(
        Source, Target, Candidate.Route.SourceDisplay,
        Candidate.Route.TargetDisplay, Candidate.Route.Link, Observation);
    if (!Landing) {
        CancelCandidate();
        return std::nullopt;
    }
    ActiveRequest_ = RoamingFocusRequest{
        Candidate.Route.Key,
        Candidate.Route.Link,
        Context_.PeerMachine,
        Context_.SessionNonce,
        Candidate.Route.Source.TopologyGeneration,
        Candidate.Route.Target.TopologyGeneration,
        *Landing,
    };
    FocusRequestedAt_ = Now;
    Candidate_.reset();
    State_ = RoamingRuntimeState::FocusPending;
    return ActiveRequest_;
}

bool RoamingRuntime::AdmitFocusReady(
    const RoamingFocusRequest& Request) noexcept {
    if (State_ != RoamingRuntimeState::FocusPending ||
        !ActiveRequest_ || *ActiveRequest_ != Request ||
        !ActiveRequestRemainsReady()) {
        EnterCooldown();
        return false;
    }
    State_ = RoamingRuntimeState::RemoteReady;
    return true;
}

bool RoamingRuntime::AdmitRemoteInput(
    const RoamingFocusRequest& Request) noexcept {
    if (State_ != RoamingRuntimeState::RemoteReady ||
        !ActiveRequest_ || *ActiveRequest_ != Request ||
        !ActiveRequestRemainsReady()) {
        EnterCooldown();
        return false;
    }
    State_ = RoamingRuntimeState::Remote;
    return true;
}

bool RoamingRuntime::ObserveRemotePointer(
    PointerPositionFeedbackMessage Position) noexcept {
    if (State_ != RoamingRuntimeState::Remote || !ActiveRequest_ ||
        !ActiveRequestRemainsReady() ||
        Position.DisplayId != ActiveRequest_->Landing.display_id) {
        return false;
    }
    const auto& Link = ActiveRequest_->ConfiguredLink;
    const auto& Target = TargetEndpoint(
        Link, ActiveRequest_->Route.Direction);
    const auto AtReturnEdge = [&] {
        switch (Target.Side) {
            case DisplayEdgeSide::Left:
                return Position.NormalizedX == 0;
            case DisplayEdgeSide::Top:
                return Position.NormalizedY == 0;
            case DisplayEdgeSide::Right:
                return Position.NormalizedX == 65'535;
            case DisplayEdgeSide::Bottom:
                return Position.NormalizedY == 65'535;
        }
        return false;
    }();
    const auto Along = Target.Side == DisplayEdgeSide::Left ||
            Target.Side == DisplayEdgeSide::Right
        ? static_cast<std::uint16_t>(
              static_cast<std::uint32_t>(Position.NormalizedY) *
              10'000u / 65'535u)
        : static_cast<std::uint16_t>(
              static_cast<std::uint32_t>(Position.NormalizedX) *
              10'000u / 65'535u);
    if (!AtReturnEdge || !InSegment(Along, Target)) {
        return false;
    }
    return true;
}

bool RoamingRuntime::ExpireFocusPending() noexcept {
    if (State_ != RoamingRuntimeState::FocusPending ||
        !ActiveRequest_ ||
        Clock_.now() - FocusRequestedAt_ < kRoamingFocusTimeout) {
        return false;
    }
    EnterCooldown();
    return true;
}

void RoamingRuntime::BeginReturn() noexcept {
    if (State_ == RoamingRuntimeState::Remote ||
        State_ == RoamingRuntimeState::RemoteReady ||
        State_ == RoamingRuntimeState::FocusPending) {
        State_ = RoamingRuntimeState::ReturnPending;
    }
}

void RoamingRuntime::ReturnLocal() noexcept { EnterCooldown(); }

void RoamingRuntime::FailLocal() noexcept { EnterCooldown(); }

RoamingRuntimeState RoamingRuntime::State() const noexcept { return State_; }

std::optional<RoamingFocusRequest>
RoamingRuntime::ActiveRequest() const noexcept {
    return ActiveRequest_;
}

bool RoamingRuntime::ActiveRequestRemainsReady() const noexcept {
    if (!ContextValid_ || !ActiveRequest_ ||
        ActiveRequest_->PeerMachine != Context_.PeerMachine ||
        ActiveRequest_->SessionNonce != Context_.SessionNonce) {
        return false;
    }
    return std::any_of(
        ReadyRoutes_.begin(), ReadyRoutes_.end(),
        [&](const ReadyRoute& Route) {
            return Route.Key == ActiveRequest_->Route &&
                   Route.Link == ActiveRequest_->ConfiguredLink &&
                   Route.Source.TopologyGeneration ==
                       ActiveRequest_->SourceTopologyGeneration &&
                   Route.Target.TopologyGeneration ==
                       ActiveRequest_->TargetTopologyGeneration;
        });
}

void RoamingRuntime::CancelCandidate() noexcept {
    Candidate_.reset();
    if (State_ == RoamingRuntimeState::EdgeCandidate) {
        State_ = RoamingRuntimeState::Local;
    }
}

void RoamingRuntime::EnterCooldown() noexcept {
    Cooldown_.reset();
    if (ActiveRequest_ && ActiveRequest_->Route.LinkIndex <
            Context_.Configuration.Links.size()) {
        const auto& Link = Context_.Configuration.Links[
            ActiveRequest_->Route.LinkIndex];
        const auto& Source = SourceEndpoint(
            Link, ActiveRequest_->Route.Direction);
        const auto Display = Context_.LocalTopology
            ? Context_.LocalTopology->FindStableIdentity(
                  Source.StableDisplayIdentity)
            : nullptr;
        if (Display) {
            Cooldown_ = std::make_unique<Cooldown>(Cooldown{
                Source, *Display, Link.ReentryDistancePixels, 0});
        }
    }
    Candidate_.reset();
    ActiveRequest_.reset();
    FocusRequestedAt_ = {};
    State_ = Cooldown_ ? RoamingRuntimeState::LocalCooldown
                       : RoamingRuntimeState::Local;
}

PeerDirectionToken PeerDirectionArbiter::NewToken(bool Outgoing) noexcept {
    ++Generation_;
    if (Generation_ == 0) ++Generation_;
    return {PeerMachine_, SessionNonce_, Generation_, Outgoing};
}

bool PeerDirectionArbiter::BindSession(
    MachineId PeerMachine, std::uint64_t SessionNonce) noexcept {
    if (!IsNonzeroMachine(PeerMachine) || SessionNonce == 0 ||
        State_ != PeerDirectionState::Local || ActiveToken_) {
        return false;
    }
    FailLocal();
    PeerMachine_ = PeerMachine;
    SessionNonce_ = SessionNonce;
    return true;
}

void PeerDirectionArbiter::ResetSession() noexcept {
    FailLocal();
    PeerMachine_ = {};
    SessionNonce_ = 0;
}

PeerDirectionDecision PeerDirectionArbiter::BeginOutgoing() noexcept {
    if (!IsNonzeroMachine(PeerMachine_) || SessionNonce_ == 0) {
        return {PeerDirectionOutcome::RejectedUnbound, std::nullopt};
    }
    if (State_ != PeerDirectionState::Local) {
        return {PeerDirectionOutcome::RejectedBusy, std::nullopt};
    }
    ActiveToken_ = NewToken(true);
    State_ = PeerDirectionState::OutgoingPending;
    return {PeerDirectionOutcome::Admitted, ActiveToken_};
}

PeerDirectionDecision PeerDirectionArbiter::BeginIncoming() noexcept {
    if (!IsNonzeroMachine(PeerMachine_) || SessionNonce_ == 0) {
        return {PeerDirectionOutcome::RejectedUnbound, std::nullopt};
    }
    if (State_ == PeerDirectionState::OutgoingPending ||
        State_ == PeerDirectionState::OutgoingActive) {
        FailLocal();
        return {PeerDirectionOutcome::CollisionFailLocal, std::nullopt};
    }
    if (State_ != PeerDirectionState::Local) {
        return {PeerDirectionOutcome::RejectedBusy, std::nullopt};
    }
    ActiveToken_ = NewToken(false);
    State_ = PeerDirectionState::IncomingActive;
    return {PeerDirectionOutcome::Admitted, ActiveToken_};
}

bool PeerDirectionArbiter::AdmitOutgoing(
    PeerDirectionToken Token) noexcept {
    if (State_ != PeerDirectionState::OutgoingPending ||
        !Token.Outgoing || !ActiveToken_ || *ActiveToken_ != Token) {
        return false;
    }
    State_ = PeerDirectionState::OutgoingActive;
    return true;
}

bool PeerDirectionArbiter::Release(PeerDirectionToken Token) noexcept {
    if (!ActiveToken_ || *ActiveToken_ != Token) return false;
    FailLocal();
    return true;
}

void PeerDirectionArbiter::FailLocal() noexcept {
    State_ = PeerDirectionState::Local;
    ActiveToken_.reset();
    ++Generation_;
    if (Generation_ == 0) ++Generation_;
}

PeerDirectionState PeerDirectionArbiter::State() const noexcept {
    return State_;
}

bool PeerDirectionArbiter::BoundTo(
    const MachineId& PeerMachine,
    std::uint64_t SessionNonce) const noexcept {
    return SessionNonce != 0 && PeerMachine_ == PeerMachine &&
        SessionNonce_ == SessionNonce;
}

} // namespace desklink
