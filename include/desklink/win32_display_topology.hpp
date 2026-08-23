#pragma once

#ifdef _WIN32

#include "desklink/display_topology.hpp"

#include <chrono>
#include <optional>
#include <vector>

namespace desklink {

[[nodiscard]] std::optional<std::vector<DiscoveredDisplay>> EnumerateWin32Displays();

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
    std::chrono::steady_clock::time_point LastRefresh_{};
    bool HasRefresh_{};
};

} // namespace desklink

#endif
