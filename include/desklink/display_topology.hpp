#pragma once

#include "desklink/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace desklink {

using DisplayId = std::uint16_t;

inline constexpr DisplayId kLegacyVirtualDesktopDisplayId = 0;
inline constexpr std::size_t kMaxDisplayCount = 64;
inline constexpr std::size_t kMaxDisplayIdentityLength = 1024;
inline constexpr std::size_t kMaxDisplayFriendlyNameLength = 256;
inline constexpr std::uint32_t kMaximumDisplayPixelDimension = 65'535;
inline constexpr std::uint32_t kMinimumDisplayRefreshMilliHertz = 1'000;
inline constexpr std::uint32_t kMaximumDisplayRefreshMilliHertz = 1'000'000;
inline constexpr std::uint16_t kMaximumPhysicalDisplayMillimeters = 10'000;

enum class PhysicalSizeSource : std::uint8_t {
    Unknown,
    RawDpiEstimate,
    Edid,
};

enum class DisplayOrientation : std::uint8_t {
    Landscape,
    Portrait,
    LandscapeFlipped,
    PortraitFlipped,
};

struct PhysicalDisplaySize {
    std::uint16_t WidthMillimeters{};
    std::uint16_t HeightMillimeters{};

    [[nodiscard]] bool operator==(const PhysicalDisplaySize&) const noexcept = default;
};

struct DisplayRect {
    std::int32_t Left{};
    std::int32_t Top{};
    std::int32_t Right{};
    std::int32_t Bottom{};

    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] bool operator==(const DisplayRect&) const noexcept = default;
};

struct NormalizedDisplayPoint {
    std::uint16_t X{};
    std::uint16_t Y{};

    [[nodiscard]] bool operator==(const NormalizedDisplayPoint&) const noexcept = default;
};

struct DiscoveredDisplay {
    std::string StableIdentity;
    std::string FriendlyName;
    DisplayRect Bounds;
    bool Primary{};
    std::uint32_t PixelWidth{};
    std::uint32_t PixelHeight{};
    std::uint32_t RefreshMilliHertz{};
    std::uint16_t PhysicalWidthMillimeters{};
    std::uint16_t PhysicalHeightMillimeters{};
    PhysicalSizeSource PhysicalSize{PhysicalSizeSource::Unknown};
    DisplayOrientation Orientation{DisplayOrientation::Landscape};

    [[nodiscard]] bool operator==(const DiscoveredDisplay&) const noexcept = default;
};

struct DisplayDescriptor {
    DisplayId Id{};
    std::string StableIdentity;
    std::string FriendlyName;
    DisplayRect Bounds;
    bool Primary{};
    std::uint32_t PixelWidth{};
    std::uint32_t PixelHeight{};
    std::uint32_t RefreshMilliHertz{};
    std::uint16_t PhysicalWidthMillimeters{};
    std::uint16_t PhysicalHeightMillimeters{};
    PhysicalSizeSource PhysicalSize{PhysicalSizeSource::Unknown};
    DisplayOrientation Orientation{DisplayOrientation::Landscape};

    [[nodiscard]] bool operator==(const DisplayDescriptor&) const noexcept = default;
};

struct DisplayTopologySnapshot {
    std::uint64_t Generation{};
    DisplayRect VirtualBounds;
    std::vector<DisplayDescriptor> Displays;

    [[nodiscard]] const DisplayDescriptor* Find(DisplayId Id) const noexcept;
    [[nodiscard]] const DisplayDescriptor* FindStableIdentity(
        std::string_view StableIdentity) const noexcept;
    [[nodiscard]] bool operator==(
        const DisplayTopologySnapshot&) const noexcept = default;
};

enum class DisplayTopologyUpdate {
    Unchanged,
    Changed,
    Invalid,
};

[[nodiscard]] DisplayId DeriveStableDisplayId(std::string_view StableIdentity) noexcept;
[[nodiscard]] std::optional<PhysicalDisplaySize> ParseEdidPhysicalSize(
    ByteSpan Edid) noexcept;
[[nodiscard]] std::optional<PhysicalDisplaySize> OrientPhysicalDisplaySize(
    PhysicalDisplaySize Size, DisplayOrientation Orientation) noexcept;
[[nodiscard]] bool IsValidDisplayTopologySnapshot(
    const DisplayTopologySnapshot& Snapshot);
[[nodiscard]] std::optional<NormalizedDisplayPoint> MapDisplayPointToVirtualDesktop(
    const DisplayRect& DisplayBounds,
    const DisplayRect& VirtualBounds,
    std::uint16_t NormalizedX,
    std::uint16_t NormalizedY) noexcept;

class DisplayTopologyMap final {
public:
    [[nodiscard]] DisplayTopologyUpdate Update(std::vector<DiscoveredDisplay> Displays);
    [[nodiscard]] const DisplayTopologySnapshot& Current() const noexcept;
    [[nodiscard]] std::optional<NormalizedDisplayPoint> MapToVirtualDesktop(
        DisplayId Id,
        std::uint64_t ExpectedGeneration,
        std::uint16_t NormalizedX,
        std::uint16_t NormalizedY) const noexcept;

private:
    DisplayTopologySnapshot Current_;
};

} // namespace desklink
