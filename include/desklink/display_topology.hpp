#pragma once

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
};

struct DisplayDescriptor {
    DisplayId Id{};
    std::string StableIdentity;
    std::string FriendlyName;
    DisplayRect Bounds;
    bool Primary{};
};

struct DisplayTopologySnapshot {
    std::uint64_t Generation{};
    DisplayRect VirtualBounds;
    std::vector<DisplayDescriptor> Displays;

    [[nodiscard]] const DisplayDescriptor* Find(DisplayId Id) const noexcept;
};

enum class DisplayTopologyUpdate {
    Unchanged,
    Changed,
    Invalid,
};

[[nodiscard]] DisplayId DeriveStableDisplayId(std::string_view StableIdentity) noexcept;
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
