#include "desklink/monitor_configurator.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace desklink {
namespace {

using DisplayKey = std::pair<MachineId, std::string>;

[[nodiscard]] bool IsNonzeroMachine(const MachineId& Machine) noexcept {
    return std::any_of(Machine.begin(), Machine.end(), [](std::uint8_t Byte) {
        return Byte != 0;
    });
}

[[nodiscard]] std::int32_t ClampDimension(
    std::uint64_t Value, std::int32_t Minimum,
    std::int32_t Maximum) noexcept {
    return static_cast<std::int32_t>(std::clamp<std::uint64_t>(
        Value, static_cast<std::uint64_t>(Minimum),
        static_cast<std::uint64_t>(Maximum)));
}

[[nodiscard]] std::pair<std::int32_t, std::int32_t> DisplaySize(
    const DisplayDescriptor& Display) noexcept {
    if (Display.PhysicalWidthMillimeters != 0 &&
        Display.PhysicalHeightMillimeters != 0) {
        return {
            ClampDimension(
                (static_cast<std::uint64_t>(
                     Display.PhysicalWidthMillimeters) * 45u + 50u) / 100u,
                kMonitorCanvasMinimumWidth, kMonitorCanvasMaximumWidth),
            ClampDimension(
                (static_cast<std::uint64_t>(
                     Display.PhysicalHeightMillimeters) * 45u + 50u) / 100u,
                kMonitorCanvasMinimumHeight, kMonitorCanvasMaximumHeight),
        };
    }
    const auto Width = std::max<std::uint32_t>(Display.PixelWidth, 1);
    const auto Height = std::max<std::uint32_t>(Display.PixelHeight, 1);
    constexpr std::int32_t DefaultLongEdge = 260;
    if (Width >= Height) {
        return {
            DefaultLongEdge,
            ClampDimension(
                (static_cast<std::uint64_t>(DefaultLongEdge) * Height +
                 Width / 2u) / Width,
                kMonitorCanvasMinimumHeight, kMonitorCanvasMaximumHeight),
        };
    }
    return {
        ClampDimension(
            (static_cast<std::uint64_t>(DefaultLongEdge) * Width +
             Height / 2u) / Height,
            kMonitorCanvasMinimumWidth, kMonitorCanvasMaximumWidth),
        DefaultLongEdge,
    };
}

[[nodiscard]] std::optional<CanvasDisplayPlacement> FindPlacement(
    const RoamingConfiguration& Configuration,
    const MachineId& Machine,
    std::string_view Identity) {
    const auto Match = std::find_if(
        Configuration.CanvasLayout.begin(), Configuration.CanvasLayout.end(),
        [&](const CanvasDisplayPlacement& Placement) {
            return Placement.Machine == Machine &&
                   Placement.StableDisplayIdentity == Identity;
        });
    return Match == Configuration.CanvasLayout.end()
        ? std::nullopt
        : std::optional<CanvasDisplayPlacement>(*Match);
}

[[nodiscard]] std::int32_t Overlap(
    std::int32_t FirstStart, std::int32_t FirstLength,
    std::int32_t SecondStart, std::int32_t SecondLength) noexcept {
    const auto Start = std::max(FirstStart, SecondStart);
    const auto End = std::min(
        FirstStart + FirstLength, SecondStart + SecondLength);
    return std::max<std::int32_t>(0, End - Start);
}

} // namespace

std::optional<MonitorCanvasModel> BuildMonitorCanvasModel(
    std::span<const MonitorCanvasMachine> Machines,
    const RoamingConfiguration& Configuration) {
    if (!IsValidRoamingConfiguration(Configuration) ||
        Machines.size() > kMaximumMonitorCanvasMachines) {
        return std::nullopt;
    }

    std::set<MachineId> MachineIds;
    std::set<DisplayKey> SeenDisplays;
    MonitorCanvasModel Result;
    std::int32_t NextMachineX = 30;
    for (const auto& Machine : Machines) {
        if (!IsNonzeroMachine(Machine.Machine) ||
            !MachineIds.insert(Machine.Machine).second ||
            (Machine.Status == DisplayTopologyExchangeStatus::Ready) !=
                Machine.Topology.has_value() ||
            (Machine.Topology &&
             !IsValidDisplayTopologySnapshot(*Machine.Topology))) {
            return std::nullopt;
        }
        std::int32_t NextDisplayX = NextMachineX;
        std::int32_t MachineWidth = 0;
        if (Machine.Topology) {
            for (const auto& Display : Machine.Topology->Displays) {
                const DisplayKey Key{Machine.Machine, Display.StableIdentity};
                if (!SeenDisplays.insert(Key).second) return std::nullopt;
                const auto [Width, Height] = DisplaySize(Display);
                const auto Placement = FindPlacement(
                    Configuration, Machine.Machine, Display.StableIdentity);
                MonitorCanvasTile Tile;
                Tile.Machine = Machine.Machine;
                Tile.MachineName = Machine.DisplayName;
                Tile.StableDisplayIdentity = Display.StableIdentity;
                Tile.FriendlyName = Display.FriendlyName;
                Tile.Rect = {
                    Placement ? Placement->X : NextDisplayX,
                    Placement ? Placement->Y : 70,
                    Width,
                    Height,
                };
                Tile.PixelWidth = Display.PixelWidth;
                Tile.PixelHeight = Display.PixelHeight;
                Tile.RefreshMilliHertz = Display.RefreshMilliHertz;
                Tile.PhysicalWidthMillimeters =
                    Display.PhysicalWidthMillimeters;
                Tile.PhysicalHeightMillimeters =
                    Display.PhysicalHeightMillimeters;
                Tile.Primary = Display.Primary;
                Tile.Local = Machine.Local;
                Tile.Online = true;
                Tile.SizeEstimated =
                    Display.PhysicalSize != PhysicalSizeSource::Edid;
                Tile.PeerInputAllowed = Machine.PeerInputAllowed;
                Result.Tiles.push_back(std::move(Tile));
                NextDisplayX += Width + 16;
                MachineWidth += Width + 16;
            }
        }
        NextMachineX += std::max<std::int32_t>(MachineWidth, 280) + 70;
    }

    std::map<MachineId, std::string> MachineNames;
    for (const auto& Machine : Machines) {
        MachineNames.emplace(Machine.Machine, Machine.DisplayName);
    }
    const auto AddOffline = [&](const MachineId& Machine,
                                std::string_view Identity,
                                std::optional<CanvasDisplayPlacement> Placement) {
        const DisplayKey Key{Machine, std::string(Identity)};
        if (!SeenDisplays.insert(Key).second) return;
        MonitorCanvasTile Tile;
        Tile.Machine = Machine;
        const auto Name = MachineNames.find(Machine);
        Tile.MachineName = Name == MachineNames.end()
            ? "Offline PC"
            : Name->second;
        Tile.StableDisplayIdentity = std::string(Identity);
        Tile.FriendlyName = "Offline display";
        Tile.Rect = {
            Placement ? Placement->X : NextMachineX,
            Placement ? Placement->Y : 70,
            260,
            146,
        };
        Tile.SizeEstimated = true;
        Result.Tiles.push_back(std::move(Tile));
        NextMachineX += 300;
    };
    for (const auto& Placement : Configuration.CanvasLayout) {
        AddOffline(
            Placement.Machine, Placement.StableDisplayIdentity, Placement);
    }
    for (const auto& Link : Configuration.Links) {
        AddOffline(
            Link.EndpointA.Machine, Link.EndpointA.StableDisplayIdentity,
            FindPlacement(Configuration, Link.EndpointA.Machine,
                          Link.EndpointA.StableDisplayIdentity));
        AddOffline(
            Link.EndpointB.Machine, Link.EndpointB.StableDisplayIdentity,
            FindPlacement(Configuration, Link.EndpointB.Machine,
                          Link.EndpointB.StableDisplayIdentity));
    }
    if (Result.Tiles.size() > kMaximumCanvasPlacements) return std::nullopt;
    return Result;
}

std::optional<RoamingLinkSuggestion> BuildRoamingLinkSuggestion(
    std::span<const MonitorCanvasTile> Tiles,
    std::size_t TileA,
    std::size_t TileB,
    std::int32_t Tolerance) noexcept {
    if (TileA >= Tiles.size() || TileB >= Tiles.size() || TileA == TileB ||
        Tolerance < 0 || Tolerance > 1'000) {
        return std::nullopt;
    }
    const auto& A = Tiles[TileA];
    const auto& B = Tiles[TileB];
    if (!A.Online || !B.Online || A.Machine == B.Machine ||
        A.StableDisplayIdentity.empty() || B.StableDisplayIdentity.empty()) {
        return std::nullopt;
    }

    struct Candidate {
        DisplayEdgeSide SideA;
        DisplayEdgeSide SideB;
        std::int32_t Gap;
        std::int32_t EdgeOverlap;
    };
    const auto HorizontalOverlap = Overlap(
        A.Rect.X, A.Rect.Width, B.Rect.X, B.Rect.Width);
    const auto VerticalOverlap = Overlap(
        A.Rect.Y, A.Rect.Height, B.Rect.Y, B.Rect.Height);
    const std::array Candidates{
        Candidate{DisplayEdgeSide::Right, DisplayEdgeSide::Left,
                  std::abs(A.Rect.X + A.Rect.Width - B.Rect.X),
                  VerticalOverlap},
        Candidate{DisplayEdgeSide::Left, DisplayEdgeSide::Right,
                  std::abs(A.Rect.X - (B.Rect.X + B.Rect.Width)),
                  VerticalOverlap},
        Candidate{DisplayEdgeSide::Bottom, DisplayEdgeSide::Top,
                  std::abs(A.Rect.Y + A.Rect.Height - B.Rect.Y),
                  HorizontalOverlap},
        Candidate{DisplayEdgeSide::Top, DisplayEdgeSide::Bottom,
                  std::abs(A.Rect.Y - (B.Rect.Y + B.Rect.Height)),
                  HorizontalOverlap},
    };
    const Candidate* Best{};
    for (const auto& Candidate : Candidates) {
        if (Candidate.Gap > Tolerance || Candidate.EdgeOverlap < 24) continue;
        if (!Best || std::tie(Candidate.Gap, Candidate.SideA) <
                         std::tie(Best->Gap, Best->SideA)) {
            Best = &Candidate;
        }
    }
    if (!Best) return std::nullopt;

    RoamingLink Link;
    Link.EndpointA = {
        A.Machine, A.StableDisplayIdentity, Best->SideA, 0, 10'000};
    Link.EndpointB = {
        B.Machine, B.StableDisplayIdentity, Best->SideB, 0, 10'000};
    Link.Direction = RoamingDirectionMode::Bidirectional;
    if (!IsValidRoamingConfiguration(RoamingConfiguration{
            {}, {Link}, {}})) {
        return std::nullopt;
    }
    return RoamingLinkSuggestion{TileA, TileB, std::move(Link), Best->Gap};
}

} // namespace desklink
