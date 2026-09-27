#pragma once

#ifdef _WIN32

#include "desklink/display_topology.hpp"

#include <chrono>
#include <optional>
#include <vector>

namespace desklink {

struct Win32DisplayInventory {
    std::vector<DiscoveredDisplay> ActiveDisplays;
    std::vector<ConnectedDisplayDescriptor> ConnectedDisplays;
};

[[nodiscard]] std::optional<Win32DisplayInventory>
EnumerateWin32DisplayInventory();
[[nodiscard]] std::optional<std::vector<DiscoveredDisplay>>
EnumerateWin32Displays();
[[nodiscard]] std::optional<std::vector<ConnectedDisplayDescriptor>>
EnumerateWin32ConnectedDisplays();

class Win32DisplayTopology final {
public:
    [[nodiscard]] bool Refresh();
    [[nodiscard]] bool RefreshIfDue(
        std::chrono::milliseconds MaximumAge = std::chrono::milliseconds(250));
    [[nodiscard]] const DisplayTopologySnapshot& Current() const noexcept;
    [[nodiscard]] std::optional<NormalizedDisplayPoint> MapToVirtualDesktop(
        DisplayId Id,
        std::uint64_t ExpectedGeneration,
        std::uint16_t NormalizedX,
        std::uint16_t NormalizedY) const noexcept;

private:
    DisplayTopologyMap Topology_;
    std::vector<ConnectedDisplayDescriptor> ConnectedDisplays_;
    std::chrono::steady_clock::time_point LastRefresh_{};
    std::chrono::steady_clock::time_point LastConnectedRefresh_{};
    bool HasRefresh_{};
    bool HasConnectedRefresh_{};
};

} // namespace desklink

#endif
