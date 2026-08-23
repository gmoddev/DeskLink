#ifdef _WIN32

#include "desklink/win32_display_topology.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace desklink {
namespace {

struct TargetIdentity {
    std::wstring StableIdentity;
    std::wstring FriendlyName;
};

struct EnumerationContext {
    const std::map<std::wstring, TargetIdentity>* Targets{};
    std::vector<DiscoveredDisplay> Displays;
    bool Failed{};
};

[[nodiscard]] std::wstring NormalizeWide(std::wstring_view Value) {
    std::wstring Result(Value);
    std::transform(Result.begin(), Result.end(), Result.begin(), [](wchar_t Character) {
        return static_cast<wchar_t>(std::towlower(Character));
    });
    return Result;
}

[[nodiscard]] std::optional<std::string> ToUtf8(std::wstring_view Value) {
    if (Value.empty()) return std::string{};
    const auto Length = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, Value.data(), static_cast<int>(Value.size()),
        nullptr, 0, nullptr, nullptr);
    if (Length <= 0) return std::nullopt;
    std::string Result(static_cast<std::size_t>(Length), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, Value.data(), static_cast<int>(Value.size()),
            Result.data(), Length, nullptr, nullptr) != Length) {
        return std::nullopt;
    }
    return Result;
}

BOOL CALLBACK CollectMonitor(HMONITOR Monitor, HDC, LPRECT, LPARAM Parameter) {
    auto& Context = *reinterpret_cast<EnumerationContext*>(Parameter);
    MONITORINFOEXW Info{};
    Info.cbSize = sizeof(Info);
    if (!GetMonitorInfoW(Monitor, &Info)) {
        Context.Failed = true;
        return FALSE;
    }

    const auto Target = Context.Targets->find(NormalizeWide(Info.szDevice));
    if (Target == Context.Targets->end()) {
        Context.Failed = true;
        return FALSE;
    }
    const auto StableIdentity = ToUtf8(Target->second.StableIdentity);
    const auto FriendlyName = ToUtf8(Target->second.FriendlyName);
    const auto SourceName = ToUtf8(Info.szDevice);
    if (!StableIdentity || !FriendlyName || !SourceName || StableIdentity->empty()) {
        Context.Failed = true;
        return FALSE;
    }

    Context.Displays.push_back(DiscoveredDisplay{
        "win32-displayconfig:" + *StableIdentity,
        FriendlyName->empty() ? *SourceName : *FriendlyName,
        DisplayRect{Info.rcMonitor.left, Info.rcMonitor.top,
                    Info.rcMonitor.right, Info.rcMonitor.bottom},
        (Info.dwFlags & MONITORINFOF_PRIMARY) != 0,
    });
    return TRUE;
}

[[nodiscard]] std::optional<std::map<std::wstring, TargetIdentity>> GetActiveTargets() {
    UINT32 PathCount = 0;
    UINT32 ModeCount = 0;
    if (GetDisplayConfigBufferSizes(
            QDC_ONLY_ACTIVE_PATHS, &PathCount, &ModeCount) != ERROR_SUCCESS || PathCount == 0) {
        return std::nullopt;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> Paths(PathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> Modes(ModeCount);
    const auto QueryResult = QueryDisplayConfig(
        QDC_ONLY_ACTIVE_PATHS, &PathCount, Paths.data(), &ModeCount, Modes.data(), nullptr);
    if (QueryResult != ERROR_SUCCESS) return std::nullopt;
    Paths.resize(PathCount);

    std::map<std::wstring, TargetIdentity> Result;
    for (const auto& Path : Paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME Source{};
        Source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        Source.header.size = sizeof(Source);
        Source.header.adapterId = Path.sourceInfo.adapterId;
        Source.header.id = Path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&Source.header) != ERROR_SUCCESS) {
            return std::nullopt;
        }

        DISPLAYCONFIG_TARGET_DEVICE_NAME Target{};
        Target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        Target.header.size = sizeof(Target);
        Target.header.adapterId = Path.targetInfo.adapterId;
        Target.header.id = Path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&Target.header) != ERROR_SUCCESS ||
            Target.monitorDevicePath[0] == L'\0') {
            return std::nullopt;
        }

        const auto SourceName = NormalizeWide(Source.viewGdiDeviceName);
        const auto StableIdentity = NormalizeWide(Target.monitorDevicePath);
        const std::wstring FriendlyName = Target.monitorFriendlyDeviceName[0] != L'\0'
            ? Target.monitorFriendlyDeviceName
            : Source.viewGdiDeviceName;
        if (SourceName.empty() || StableIdentity.empty() ||
            !Result.emplace(SourceName, TargetIdentity{StableIdentity, FriendlyName}).second) {
            // Clone paths share a source rectangle but identify multiple targets. Refuse an
            // ambiguous mapping until clone-aware routing is explicitly designed.
            return std::nullopt;
        }
    }
    return Result;
}

} // namespace

std::optional<std::vector<DiscoveredDisplay>> EnumerateWin32Displays() {
    const auto Targets = GetActiveTargets();
    if (!Targets) return std::nullopt;

    EnumerationContext Context;
    Context.Targets = &*Targets;
    if (!EnumDisplayMonitors(
            nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&Context)) ||
        Context.Failed || Context.Displays.size() != Targets->size()) {
        return std::nullopt;
    }
    return Context.Displays;
}

bool Win32DisplayTopology::Refresh() {
    try {
        const auto Displays = EnumerateWin32Displays();
        if (!Displays || Topology_.Update(*Displays) == DisplayTopologyUpdate::Invalid) {
            return false;
        }
        LastRefresh_ = std::chrono::steady_clock::now();
        HasRefresh_ = true;
        return true;
    } catch (...) {
        return false;
    }
}

bool Win32DisplayTopology::RefreshIfDue(std::chrono::milliseconds MaximumAge) {
    if (MaximumAge < std::chrono::milliseconds::zero()) return false;
    const auto Now = std::chrono::steady_clock::now();
    if (HasRefresh_ && Now - LastRefresh_ < MaximumAge) return true;
    return Refresh();
}

const DisplayTopologySnapshot& Win32DisplayTopology::Current() const noexcept {
    return Topology_.Current();
}

std::optional<NormalizedDisplayPoint> Win32DisplayTopology::MapToVirtualDesktop(
    DisplayId Id,
    std::uint64_t ExpectedGeneration,
    std::uint16_t NormalizedX,
    std::uint16_t NormalizedY) const noexcept {
    return Topology_.MapToVirtualDesktop(
        Id, ExpectedGeneration, NormalizedX, NormalizedY);
}

} // namespace desklink

#endif
