#include "desklink/roaming.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <tuple>

namespace desklink {
namespace {

constexpr std::uint32_t kRoamingSettingsMagic = 0x444C5247u; // DLRG
constexpr std::uint16_t kRoamingSettingsVersion = 1;
constexpr std::uint16_t kMaximumPushDistancePixels = 64;
constexpr std::uint16_t kMaximumCrossingTimeMilliseconds = 1'000;
constexpr std::uint16_t kMaximumLandingInsetPixels = 64;
constexpr std::uint16_t kMaximumCornerClearancePixels = 256;
constexpr std::uint16_t kMaximumReentryDistancePixels = 128;

[[nodiscard]] bool IsNonzeroMachine(const MachineId& Machine) noexcept {
    return std::any_of(Machine.begin(), Machine.end(), [](std::uint8_t Byte) {
        return Byte != 0;
    });
}

[[nodiscard]] bool IsValidIdentity(std::string_view Identity) noexcept {
    return !Identity.empty() && Identity.size() <= kMaxDisplayIdentityLength &&
           Identity.find('\0') == std::string_view::npos;
}

[[nodiscard]] bool IsValidSide(DisplayEdgeSide Side) noexcept {
    return Side == DisplayEdgeSide::Left || Side == DisplayEdgeSide::Top ||
           Side == DisplayEdgeSide::Right || Side == DisplayEdgeSide::Bottom;
}

[[nodiscard]] bool IsValidPolicy(CrossingPolicy Policy) noexcept {
    return Policy == CrossingPolicy::Push ||
           Policy == CrossingPolicy::DwellAndPush ||
           Policy == CrossingPolicy::DoublePush;
}

[[nodiscard]] bool IsValidDirection(RoamingDirectionMode Direction) noexcept {
    return Direction == RoamingDirectionMode::Bidirectional ||
           Direction == RoamingDirectionMode::AToB ||
           Direction == RoamingDirectionMode::BToA;
}

[[nodiscard]] bool SameDisplay(const RoamingEndpoint& Left,
                               const RoamingEndpoint& Right) noexcept {
    return Left.Machine == Right.Machine &&
           Left.StableDisplayIdentity == Right.StableDisplayIdentity;
}

[[nodiscard]] bool SameEndpoint(const RoamingEndpoint& Left,
                                const RoamingEndpoint& Right) noexcept {
    return SameDisplay(Left, Right) && Left.Side == Right.Side &&
           Left.SegmentStartPermyriad == Right.SegmentStartPermyriad &&
           Left.SegmentEndPermyriad == Right.SegmentEndPermyriad;
}

[[nodiscard]] bool SameSourceEdge(const RoamingEndpoint& Left,
                                  const RoamingEndpoint& Right) noexcept {
    return SameDisplay(Left, Right) && Left.Side == Right.Side;
}

[[nodiscard]] bool SegmentsOverlap(const RoamingEndpoint& Left,
                                   const RoamingEndpoint& Right) noexcept {
    return std::max(Left.SegmentStartPermyriad,
                    Right.SegmentStartPermyriad) <
           std::min(Left.SegmentEndPermyriad,
                    Right.SegmentEndPermyriad);
}

[[nodiscard]] bool HasAToB(RoamingDirectionMode Direction) noexcept {
    return Direction == RoamingDirectionMode::Bidirectional ||
           Direction == RoamingDirectionMode::AToB;
}

[[nodiscard]] bool HasBToA(RoamingDirectionMode Direction) noexcept {
    return Direction == RoamingDirectionMode::Bidirectional ||
           Direction == RoamingDirectionMode::BToA;
}

[[nodiscard]] bool IsValidLink(const RoamingLink& Link) noexcept {
    return IsValidRoamingEndpoint(Link.EndpointA) &&
           IsValidRoamingEndpoint(Link.EndpointB) &&
           IsValidDirection(Link.Direction) &&
           IsValidCrossingConfiguration(Link.AToB) &&
           IsValidCrossingConfiguration(Link.BToA) &&
           Link.EndpointA.Machine != Link.EndpointB.Machine &&
           Link.LandingInsetPixels >= 1 &&
           Link.LandingInsetPixels <= kMaximumLandingInsetPixels &&
           Link.CornerClearancePixels >= 1 &&
           Link.CornerClearancePixels <= kMaximumCornerClearancePixels &&
           Link.ReentryDistancePixels >= 1 &&
           Link.ReentryDistancePixels <= kMaximumReentryDistancePixels;
}

void AppendU8(ByteBuffer& Output, std::uint8_t Value) {
    Output.push_back(Value);
}

void AppendU16(ByteBuffer& Output, std::uint16_t Value) {
    Output.push_back(static_cast<std::uint8_t>(Value >> 8u));
    Output.push_back(static_cast<std::uint8_t>(Value));
}

void AppendU32(ByteBuffer& Output, std::uint32_t Value) {
    Output.push_back(static_cast<std::uint8_t>(Value >> 24u));
    Output.push_back(static_cast<std::uint8_t>(Value >> 16u));
    Output.push_back(static_cast<std::uint8_t>(Value >> 8u));
    Output.push_back(static_cast<std::uint8_t>(Value));
}

void AppendI32(ByteBuffer& Output, std::int32_t Value) {
    std::uint32_t Bits{};
    static_assert(sizeof(Bits) == sizeof(Value));
    std::memcpy(&Bits, &Value, sizeof(Bits));
    AppendU32(Output, Bits);
}

void AppendMachine(ByteBuffer& Output, const MachineId& Machine) {
    Output.insert(Output.end(), Machine.begin(), Machine.end());
}

void AppendString(ByteBuffer& Output, std::string_view Value) {
    AppendU16(Output, static_cast<std::uint16_t>(Value.size()));
    Output.insert(Output.end(), Value.begin(), Value.end());
}

void AppendCrossing(ByteBuffer& Output,
                    const CrossingConfiguration& Crossing) {
    AppendU8(Output, static_cast<std::uint8_t>(Crossing.Policy));
    AppendU16(Output, Crossing.PushDistancePixels);
    AppendU16(Output, Crossing.DwellMilliseconds);
    AppendU16(Output, Crossing.DoublePushWindowMilliseconds);
}

void AppendEndpoint(ByteBuffer& Output, const RoamingEndpoint& Endpoint) {
    AppendMachine(Output, Endpoint.Machine);
    AppendString(Output, Endpoint.StableDisplayIdentity);
    AppendU8(Output, static_cast<std::uint8_t>(Endpoint.Side));
    AppendU16(Output, Endpoint.SegmentStartPermyriad);
    AppendU16(Output, Endpoint.SegmentEndPermyriad);
}

[[nodiscard]] bool ReadU8(ByteSpan Input, std::size_t& Offset,
                          std::uint8_t& Value) noexcept {
    if (Offset >= Input.size()) return false;
    Value = Input[Offset++];
    return true;
}

[[nodiscard]] bool ReadU16(ByteSpan Input, std::size_t& Offset,
                           std::uint16_t& Value) noexcept {
    if (Offset > Input.size() || Input.size() - Offset < 2) return false;
    Value = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(Input[Offset]) << 8u) |
        Input[Offset + 1]);
    Offset += 2;
    return true;
}

[[nodiscard]] bool ReadU32(ByteSpan Input, std::size_t& Offset,
                           std::uint32_t& Value) noexcept {
    if (Offset > Input.size() || Input.size() - Offset < 4) return false;
    Value = (static_cast<std::uint32_t>(Input[Offset]) << 24u) |
            (static_cast<std::uint32_t>(Input[Offset + 1]) << 16u) |
            (static_cast<std::uint32_t>(Input[Offset + 2]) << 8u) |
            static_cast<std::uint32_t>(Input[Offset + 3]);
    Offset += 4;
    return true;
}

[[nodiscard]] bool ReadI32(ByteSpan Input, std::size_t& Offset,
                           std::int32_t& Value) noexcept {
    std::uint32_t Bits{};
    if (!ReadU32(Input, Offset, Bits)) return false;
    static_assert(sizeof(Bits) == sizeof(Value));
    std::memcpy(&Value, &Bits, sizeof(Value));
    return true;
}

[[nodiscard]] bool ReadMachine(ByteSpan Input, std::size_t& Offset,
                               MachineId& Machine) noexcept {
    if (Offset > Input.size() || Input.size() - Offset < Machine.size()) {
        return false;
    }
    std::copy_n(Input.begin() + static_cast<std::ptrdiff_t>(Offset),
                Machine.size(), Machine.begin());
    Offset += Machine.size();
    return true;
}

[[nodiscard]] bool ReadString(ByteSpan Input, std::size_t& Offset,
                              std::string& Value) {
    std::uint16_t Length{};
    if (!ReadU16(Input, Offset, Length) ||
        Length == 0 || Length > kMaxDisplayIdentityLength ||
        Offset > Input.size() || Input.size() - Offset < Length) {
        return false;
    }
    Value.assign(reinterpret_cast<const char*>(Input.data() + Offset), Length);
    Offset += Length;
    return true;
}

[[nodiscard]] bool ReadCrossing(ByteSpan Input, std::size_t& Offset,
                                CrossingConfiguration& Crossing) noexcept {
    std::uint8_t Policy{};
    if (!ReadU8(Input, Offset, Policy) ||
        !ReadU16(Input, Offset, Crossing.PushDistancePixels) ||
        !ReadU16(Input, Offset, Crossing.DwellMilliseconds) ||
        !ReadU16(Input, Offset,
                 Crossing.DoublePushWindowMilliseconds)) {
        return false;
    }
    Crossing.Policy = static_cast<CrossingPolicy>(Policy);
    return IsValidCrossingConfiguration(Crossing);
}

[[nodiscard]] bool ReadEndpoint(ByteSpan Input, std::size_t& Offset,
                                RoamingEndpoint& Endpoint) {
    std::uint8_t Side{};
    if (!ReadMachine(Input, Offset, Endpoint.Machine) ||
        !ReadString(Input, Offset, Endpoint.StableDisplayIdentity) ||
        !ReadU8(Input, Offset, Side) ||
        !ReadU16(Input, Offset, Endpoint.SegmentStartPermyriad) ||
        !ReadU16(Input, Offset, Endpoint.SegmentEndPermyriad)) {
        return false;
    }
    Endpoint.Side = static_cast<DisplayEdgeSide>(Side);
    return IsValidRoamingEndpoint(Endpoint);
}

} // namespace

bool IsValidRoamingEndpoint(const RoamingEndpoint& Endpoint) noexcept {
    return IsNonzeroMachine(Endpoint.Machine) &&
           IsValidIdentity(Endpoint.StableDisplayIdentity) &&
           IsValidSide(Endpoint.Side) &&
           Endpoint.SegmentStartPermyriad <
               Endpoint.SegmentEndPermyriad &&
           Endpoint.SegmentEndPermyriad <= 10'000;
}

bool IsValidCrossingConfiguration(
    const CrossingConfiguration& Configuration) noexcept {
    return IsValidPolicy(Configuration.Policy) &&
           Configuration.PushDistancePixels >= 1 &&
           Configuration.PushDistancePixels <= kMaximumPushDistancePixels &&
           Configuration.DwellMilliseconds <=
               kMaximumCrossingTimeMilliseconds &&
           Configuration.DoublePushWindowMilliseconds >= 1 &&
           Configuration.DoublePushWindowMilliseconds <=
               kMaximumCrossingTimeMilliseconds;
}

bool IsValidRoamingConfiguration(
    const RoamingConfiguration& Configuration) noexcept {
    if (!IsValidCrossingConfiguration(Configuration.CrossingDefaults) ||
        Configuration.Links.size() > kMaximumRoamingLinks ||
        Configuration.CanvasLayout.size() > kMaximumCanvasPlacements) {
        return false;
    }

    std::vector<const RoamingEndpoint*> Sources;
    Sources.reserve(Configuration.Links.size() * 2);
    for (std::size_t Index = 0; Index < Configuration.Links.size(); ++Index) {
        const auto& Link = Configuration.Links[Index];
        if (!IsValidLink(Link)) {
            return false;
        }
        for (std::size_t Previous = 0; Previous < Index; ++Previous) {
            const auto& Existing = Configuration.Links[Previous];
            if ((SameEndpoint(Link.EndpointA, Existing.EndpointA) &&
                 SameEndpoint(Link.EndpointB, Existing.EndpointB)) ||
                (SameEndpoint(Link.EndpointA, Existing.EndpointB) &&
                 SameEndpoint(Link.EndpointB, Existing.EndpointA))) {
                return false;
            }
        }
        if (!Link.Enabled) continue;
        if (HasAToB(Link.Direction)) Sources.push_back(&Link.EndpointA);
        if (HasBToA(Link.Direction)) Sources.push_back(&Link.EndpointB);
    }

    for (std::size_t Index = 0; Index < Sources.size(); ++Index) {
        for (std::size_t Other = Index + 1; Other < Sources.size(); ++Other) {
            if (SameSourceEdge(*Sources[Index], *Sources[Other]) &&
                SegmentsOverlap(*Sources[Index], *Sources[Other])) {
                return false;
            }
        }
    }

    for (std::size_t Index = 0;
         Index < Configuration.CanvasLayout.size(); ++Index) {
        const auto& Placement = Configuration.CanvasLayout[Index];
        if (!IsNonzeroMachine(Placement.Machine) ||
            !IsValidIdentity(Placement.StableDisplayIdentity) ||
            Placement.X < -kMaximumCanvasCoordinate ||
            Placement.X > kMaximumCanvasCoordinate ||
            Placement.Y < -kMaximumCanvasCoordinate ||
            Placement.Y > kMaximumCanvasCoordinate) {
            return false;
        }
        for (std::size_t Previous = 0; Previous < Index; ++Previous) {
            const auto& Existing = Configuration.CanvasLayout[Previous];
            if (Placement.Machine == Existing.Machine &&
                Placement.StableDisplayIdentity ==
                    Existing.StableDisplayIdentity) {
                return false;
            }
        }
    }
    return true;
}

std::optional<ByteBuffer> EncodeRoamingConfiguration(
    const RoamingConfiguration& Configuration) {
    if (!IsValidRoamingConfiguration(Configuration)) return std::nullopt;
    ByteBuffer Output;
    Output.reserve(64 + Configuration.Links.size() * 128 +
                   Configuration.CanvasLayout.size() * 64);
    AppendU32(Output, kRoamingSettingsMagic);
    AppendU16(Output, kRoamingSettingsVersion);
    AppendU16(Output,
              static_cast<std::uint16_t>(Configuration.Links.size()));
    AppendU16(Output, static_cast<std::uint16_t>(
        Configuration.CanvasLayout.size()));
    AppendCrossing(Output, Configuration.CrossingDefaults);
    for (const auto& Link : Configuration.Links) {
        AppendEndpoint(Output, Link.EndpointA);
        AppendEndpoint(Output, Link.EndpointB);
        AppendU8(Output, static_cast<std::uint8_t>(Link.Direction));
        AppendCrossing(Output, Link.AToB);
        AppendCrossing(Output, Link.BToA);
        AppendU16(Output, Link.LandingInsetPixels);
        AppendU16(Output, Link.CornerClearancePixels);
        AppendU16(Output, Link.ReentryDistancePixels);
        AppendU8(Output, Link.Enabled ? 1u : 0u);
    }
    for (const auto& Placement : Configuration.CanvasLayout) {
        AppendMachine(Output, Placement.Machine);
        AppendString(Output, Placement.StableDisplayIdentity);
        AppendI32(Output, Placement.X);
        AppendI32(Output, Placement.Y);
    }
    if (Output.size() > kMaximumRoamingSettingsBytes) return std::nullopt;
    return Output;
}

std::optional<RoamingConfiguration> DecodeRoamingConfiguration(
    ByteSpan Bytes) {
    if (Bytes.empty() || Bytes.size() > kMaximumRoamingSettingsBytes) {
        return std::nullopt;
    }
    std::size_t Offset{};
    std::uint32_t Magic{};
    std::uint16_t Version{};
    std::uint16_t LinkCount{};
    std::uint16_t PlacementCount{};
    RoamingConfiguration Result;
    if (!ReadU32(Bytes, Offset, Magic) ||
        !ReadU16(Bytes, Offset, Version) ||
        !ReadU16(Bytes, Offset, LinkCount) ||
        !ReadU16(Bytes, Offset, PlacementCount) ||
        Magic != kRoamingSettingsMagic ||
        Version != kRoamingSettingsVersion ||
        LinkCount > kMaximumRoamingLinks ||
        PlacementCount > kMaximumCanvasPlacements ||
        !ReadCrossing(Bytes, Offset, Result.CrossingDefaults)) {
        return std::nullopt;
    }
    Result.Links.reserve(LinkCount);
    for (std::size_t Index = 0; Index < LinkCount; ++Index) {
        RoamingLink Link;
        std::uint8_t Direction{};
        std::uint8_t Enabled{};
        if (!ReadEndpoint(Bytes, Offset, Link.EndpointA) ||
            !ReadEndpoint(Bytes, Offset, Link.EndpointB) ||
            !ReadU8(Bytes, Offset, Direction) ||
            !ReadCrossing(Bytes, Offset, Link.AToB) ||
            !ReadCrossing(Bytes, Offset, Link.BToA) ||
            !ReadU16(Bytes, Offset, Link.LandingInsetPixels) ||
            !ReadU16(Bytes, Offset, Link.CornerClearancePixels) ||
            !ReadU16(Bytes, Offset, Link.ReentryDistancePixels) ||
            !ReadU8(Bytes, Offset, Enabled) || Enabled > 1) {
            return std::nullopt;
        }
        Link.Direction = static_cast<RoamingDirectionMode>(Direction);
        Link.Enabled = Enabled != 0;
        Result.Links.push_back(std::move(Link));
    }
    Result.CanvasLayout.reserve(PlacementCount);
    for (std::size_t Index = 0; Index < PlacementCount; ++Index) {
        CanvasDisplayPlacement Placement;
        if (!ReadMachine(Bytes, Offset, Placement.Machine) ||
            !ReadString(Bytes, Offset, Placement.StableDisplayIdentity) ||
            !ReadI32(Bytes, Offset, Placement.X) ||
            !ReadI32(Bytes, Offset, Placement.Y)) {
            return std::nullopt;
        }
        Result.CanvasLayout.push_back(std::move(Placement));
    }
    if (Offset != Bytes.size() || !IsValidRoamingConfiguration(Result)) {
        return std::nullopt;
    }
    return Result;
}

RoamingEndpointResolutionResult ResolveRoamingEndpoint(
    const RoamingEndpoint& Endpoint,
    std::span<const MachineDisplayTopology> Topologies) noexcept {
    if (!IsValidRoamingEndpoint(Endpoint)) {
        return {RoamingEndpointResolution::InvalidEndpoint, std::nullopt};
    }
    const MachineDisplayTopology* Match{};
    for (const auto& Candidate : Topologies) {
        if (Candidate.Machine != Endpoint.Machine) continue;
        if (Match) {
            return {RoamingEndpointResolution::AmbiguousMachine, std::nullopt};
        }
        Match = &Candidate;
    }
    if (!Match || !Match->Topology || Match->Topology->Generation == 0) {
        return {RoamingEndpointResolution::MachineUnavailable, std::nullopt};
    }
    const auto* Display = Match->Topology->FindStableIdentity(
        Endpoint.StableDisplayIdentity);
    if (!Display) {
        return {RoamingEndpointResolution::DisplayMissing, std::nullopt};
    }
    return {
        RoamingEndpointResolution::Ready,
        ResolvedRoamingEndpoint{
            Endpoint.Machine,
            Display->Id,
            Match->Topology->Generation,
            Endpoint.Side,
            Endpoint.SegmentStartPermyriad,
            Endpoint.SegmentEndPermyriad,
        },
    };
}

RoamingLinkResolutionResult ResolveRoamingLink(
    const RoamingLink& Link,
    std::span<const MachineDisplayTopology> Topologies) noexcept {
    if (!IsValidLink(Link)) {
        return {
            {RoamingEndpointResolution::InvalidEndpoint, std::nullopt},
            {RoamingEndpointResolution::InvalidEndpoint, std::nullopt},
        };
    }
    return {
        ResolveRoamingEndpoint(Link.EndpointA, Topologies),
        ResolveRoamingEndpoint(Link.EndpointB, Topologies),
    };
}

} // namespace desklink
