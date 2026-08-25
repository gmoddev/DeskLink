#pragma once

#ifdef _WIN32

#include "desklink/control.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <functional>
#include <optional>

namespace desklink {

struct Win32MonitorConfiguratorCallbacks {
    std::function<std::optional<ControlTopologyState>()> GetTopologies;
    std::function<bool()> EnsureLocal;
};

[[nodiscard]] bool ShowWin32MonitorConfigurator(
    HWND Owner,
    std::filesystem::path SettingsPath,
    Win32MonitorConfiguratorCallbacks Callbacks);
[[nodiscard]] bool ShowWin32DisplayIdentification(HWND Owner);

} // namespace desklink

#endif
