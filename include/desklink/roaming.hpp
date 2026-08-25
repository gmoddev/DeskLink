#pragma once

#include "desklink/display_topology.hpp"
#include "desklink/types.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace desklink {

inline constexpr std::size_t kMaximumRoamingLinks = 128;
inline constexpr std::size_t kMaximumCanvasPlacements = 128;
inline constexpr std::size_t kMaximumRoamingSettingsBytes = 512u * 1024u;
inline constexpr std::int32_t kMaximumCanvasCoordinate = 1'000'000;

enum class DisplayEdgeSide : std::uint8_t {
    Left = 1,
    Top = 2,
    Right = 3,
    Bottom = 4,
};

enum class CrossingPolicy : std::uint8_t {
    Push = 1,
    DwellAndPush = 2,
    DoublePush = 3,
};

enum class RoamingDirectionMode : std::uint8_t {
    Bidirectional = 1,
    AToB = 2,
    BToA = 3,
};

struct RoamingEndpoint {
    MachineId Machine{};
    std::string StableDisplayIdentity;
    DisplayEdgeSide Side{DisplayEdgeSide::Right};
    std::uint16_t SegmentStartPermyriad{};
    std::uint16_t SegmentEndPermyriad{10'000};

    [[nodiscard]] bool operator==(const RoamingEndpoint&) const noexcept = default;
};

struct CrossingConfiguration {
    CrossingPolicy Policy{CrossingPolicy::Push};
    std::uint16_t PushDistancePixels{8};
    std::uint16_t DwellMilliseconds{120};
    std::uint16_t DoublePushWindowMilliseconds{500};

    [[nodiscard]] bool operator==(const CrossingConfiguration&) const noexcept = default;
};

struct RoamingLink {
    RoamingEndpoint EndpointA;
    RoamingEndpoint EndpointB;
    RoamingDirectionMode Direction{RoamingDirectionMode::Bidirectional};
    CrossingConfiguration AToB;
    CrossingConfiguration BToA;
    std::uint16_t LandingInsetPixels{12};
    std::uint16_t CornerClearancePixels{24};
    std::uint16_t ReentryDistancePixels{24};
    bool Enabled{true};

    [[nodiscard]] bool operator==(const RoamingLink&) const noexcept = default;
};

struct CanvasDisplayPlacement {
    MachineId Machine{};
    std::string StableDisplayIdentity;
    std::int32_t X{};
    std::int32_t Y{};

    [[nodiscard]] bool operator==(const CanvasDisplayPlacement&) const noexcept = default;
};

struct RoamingConfiguration {
    CrossingConfiguration CrossingDefaults;
    std::vector<RoamingLink> Links;
    std::vector<CanvasDisplayPlacement> CanvasLayout;

    [[nodiscard]] bool operator==(const RoamingConfiguration&) const noexcept = default;
};

[[nodiscard]] bool IsValidRoamingEndpoint(
    const RoamingEndpoint& Endpoint) noexcept;
[[nodiscard]] bool IsValidCrossingConfiguration(
    const CrossingConfiguration& Configuration) noexcept;
[[nodiscard]] bool IsValidRoamingConfiguration(
    const RoamingConfiguration& Configuration) noexcept;
[[nodiscard]] std::optional<ByteBuffer> EncodeRoamingConfiguration(
    const RoamingConfiguration& Configuration);
[[nodiscard]] std::optional<RoamingConfiguration> DecodeRoamingConfiguration(
    ByteSpan Bytes);

struct MachineDisplayTopology {
    MachineId Machine{};
    const DisplayTopologySnapshot* Topology{};
};

enum class RoamingEndpointResolution {
    Ready,
    InvalidEndpoint,
    MachineUnavailable,
    AmbiguousMachine,
    DisplayMissing,
};

struct ResolvedRoamingEndpoint {
    MachineId Machine{};
    DisplayId Display{};
    std::uint64_t TopologyGeneration{};
    DisplayEdgeSide Side{DisplayEdgeSide::Right};
    std::uint16_t SegmentStartPermyriad{};
    std::uint16_t SegmentEndPermyriad{};

    [[nodiscard]] bool operator==(
        const ResolvedRoamingEndpoint&) const noexcept = default;
};

struct RoamingEndpointResolutionResult {
    RoamingEndpointResolution Status{RoamingEndpointResolution::InvalidEndpoint};
    std::optional<ResolvedRoamingEndpoint> Endpoint;
};

struct RoamingLinkResolutionResult {
    RoamingEndpointResolutionResult EndpointA;
    RoamingEndpointResolutionResult EndpointB;

    [[nodiscard]] bool Ready() const noexcept {
        return EndpointA.Status == RoamingEndpointResolution::Ready &&
               EndpointB.Status == RoamingEndpointResolution::Ready;
    }
};

[[nodiscard]] RoamingEndpointResolutionResult ResolveRoamingEndpoint(
    const RoamingEndpoint& Endpoint,
    std::span<const MachineDisplayTopology> Topologies) noexcept;
[[nodiscard]] RoamingLinkResolutionResult ResolveRoamingLink(
    const RoamingLink& Link,
    std::span<const MachineDisplayTopology> Topologies) noexcept;

} // namespace desklink
