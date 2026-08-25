#pragma once

#include "desklink/roaming.hpp"
#include "desklink/topology_exchange.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace desklink {

inline constexpr std::int32_t kMonitorCanvasMinimumWidth = 120;
inline constexpr std::int32_t kMonitorCanvasMaximumWidth = 420;
inline constexpr std::int32_t kMonitorCanvasMinimumHeight = 80;
inline constexpr std::int32_t kMonitorCanvasMaximumHeight = 300;
inline constexpr std::int32_t kMonitorCanvasAdjacencyTolerance = 24;
inline constexpr std::size_t kMaximumMonitorCanvasMachines = 8;

struct MonitorCanvasMachine {
    MachineId Machine{};
    std::string DisplayName;
    std::optional<DisplayTopologySnapshot> Topology;
    DisplayTopologyExchangeStatus Status{
        DisplayTopologyExchangeStatus::Offline};
    bool Local{};
    bool PeerInputAllowed{};
};

struct MonitorCanvasRect {
    std::int32_t X{};
    std::int32_t Y{};
    std::int32_t Width{};
    std::int32_t Height{};

    [[nodiscard]] bool operator==(
        const MonitorCanvasRect&) const noexcept = default;
};

struct MonitorCanvasTile {
    MachineId Machine{};
    std::string MachineName;
    std::string StableDisplayIdentity;
    std::string FriendlyName;
    MonitorCanvasRect Rect;
    std::uint32_t PixelWidth{};
    std::uint32_t PixelHeight{};
    std::uint32_t RefreshMilliHertz{};
    std::uint16_t PhysicalWidthMillimeters{};
    std::uint16_t PhysicalHeightMillimeters{};
    bool Primary{};
    bool Local{};
    bool Online{};
    bool SizeEstimated{true};
    bool PeerInputAllowed{};
};

struct MonitorCanvasModel {
    std::vector<MonitorCanvasTile> Tiles;
};

struct RoamingLinkSuggestion {
    std::size_t TileA{};
    std::size_t TileB{};
    RoamingLink Link;
    std::int32_t EdgeGapPixels{};
};

[[nodiscard]] std::optional<MonitorCanvasModel> BuildMonitorCanvasModel(
    std::span<const MonitorCanvasMachine> Machines,
    const RoamingConfiguration& Configuration);
[[nodiscard]] std::optional<RoamingLinkSuggestion>
BuildRoamingLinkSuggestion(
    std::span<const MonitorCanvasTile> Tiles,
    std::size_t TileA,
    std::size_t TileB,
    std::int32_t Tolerance = kMonitorCanvasAdjacencyTolerance) noexcept;

} // namespace desklink
