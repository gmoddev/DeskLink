#ifdef _WIN32

#include "desklink/win32_display_topology.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <shellscalingapi.h>

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
    std::uint32_t PixelWidth{};
    std::uint32_t PixelHeight{};
    std::uint32_t RefreshMilliHertz{};
    PhysicalDisplaySize PhysicalSize;
    PhysicalSizeSource PhysicalSizeKind{PhysicalSizeSource::Unknown};
    DisplayOrientation Orientation{DisplayOrientation::Landscape};
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

[[nodiscard]] std::optional<DisplayOrientation> ToOrientation(
    DISPLAYCONFIG_ROTATION Rotation) noexcept {
    switch (Rotation) {
        case DISPLAYCONFIG_ROTATION_ROTATE90:
            return DisplayOrientation::Portrait;
        case DISPLAYCONFIG_ROTATION_ROTATE180:
            return DisplayOrientation::LandscapeFlipped;
        case DISPLAYCONFIG_ROTATION_ROTATE270:
            return DisplayOrientation::PortraitFlipped;
        case DISPLAYCONFIG_ROTATION_IDENTITY:
            return DisplayOrientation::Landscape;
        default:
            return std::nullopt;
    }
}

[[nodiscard]] std::optional<std::uint32_t> RefreshMilliHertz(
    const DISPLAYCONFIG_RATIONAL& Refresh) noexcept {
    if (Refresh.Numerator == 0 || Refresh.Denominator == 0) {
        return std::nullopt;
    }
    const auto Result =
        (static_cast<std::uint64_t>(Refresh.Numerator) * 1'000u +
         Refresh.Denominator / 2u) /
        Refresh.Denominator;
    if (Result < kMinimumDisplayRefreshMilliHertz ||
        Result > kMaximumDisplayRefreshMilliHertz) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(Result);
}

[[nodiscard]] std::optional<PhysicalDisplaySize> ReadMonitorEdid(
    std::wstring_view TargetPath) {
    constexpr GUID MonitorInterface{
        0xe6f07b5f, 0xee97, 0x4a90,
        {0xb0, 0x76, 0x33, 0xf5, 0x7b, 0xf4, 0xea, 0xa7}};
    const auto Devices = SetupDiGetClassDevsW(
        &MonitorInterface, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (Devices == INVALID_HANDLE_VALUE) return std::nullopt;

    std::optional<PhysicalDisplaySize> Result;
    for (DWORD Index = 0; !Result; ++Index) {
        SP_DEVICE_INTERFACE_DATA Interface{};
        Interface.cbSize = sizeof(Interface);
        if (SetupDiEnumDeviceInterfaces(
                Devices, nullptr, &MonitorInterface, Index, &Interface) ==
            FALSE) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) break;
            SetupDiDestroyDeviceInfoList(Devices);
            return std::nullopt;
        }
        DWORD Required{};
        SetupDiGetDeviceInterfaceDetailW(
            Devices, &Interface, nullptr, 0, &Required, nullptr);
        if (Required < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        std::vector<std::uint64_t> Storage(
            (static_cast<std::size_t>(Required) + sizeof(std::uint64_t) - 1) /
            sizeof(std::uint64_t));
        auto* Detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(
            Storage.data());
        Detail->cbSize = sizeof(*Detail);
        SP_DEVINFO_DATA Device{};
        Device.cbSize = sizeof(Device);
        if (SetupDiGetDeviceInterfaceDetailW(
                Devices, &Interface, Detail, Required, nullptr, &Device) ==
                FALSE ||
            NormalizeWide(Detail->DevicePath) != NormalizeWide(TargetPath)) {
            continue;
        }

        const auto Key = SetupDiOpenDevRegKey(
            Devices, &Device, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_QUERY_VALUE);
        if (Key == INVALID_HANDLE_VALUE) break;
        DWORD Type{};
        DWORD Size{};
        auto Status = RegQueryValueExW(
            Key, L"EDID", nullptr, &Type, nullptr, &Size);
        if (Status == ERROR_SUCCESS && Type == REG_BINARY && Size >= 128 &&
            Size <= 1024) {
            ByteBuffer Edid(Size);
            Status = RegQueryValueExW(
                Key, L"EDID", nullptr, &Type, Edid.data(), &Size);
            if (Status == ERROR_SUCCESS) {
                Edid.resize(Size);
                Result = ParseEdidPhysicalSize(Edid);
            }
        }
        RegCloseKey(Key);
        break;
    }
    SetupDiDestroyDeviceInfoList(Devices);
    return Result;
}

[[nodiscard]] std::optional<PhysicalDisplaySize> EstimatePhysicalSize(
    HMONITOR Monitor, std::uint32_t PixelWidth,
    std::uint32_t PixelHeight) noexcept {
    UINT DpiX{};
    UINT DpiY{};
    if (GetDpiForMonitor(Monitor, MDT_RAW_DPI, &DpiX, &DpiY) != S_OK ||
        DpiX == 0 || DpiY == 0) {
        return std::nullopt;
    }
    const auto Width =
        (static_cast<std::uint64_t>(PixelWidth) * 254u + DpiX * 5u) /
        (DpiX * 10u);
    const auto Height =
        (static_cast<std::uint64_t>(PixelHeight) * 254u + DpiY * 5u) /
        (DpiY * 10u);
    if (Width == 0 || Height == 0 ||
        Width > kMaximumPhysicalDisplayMillimeters ||
        Height > kMaximumPhysicalDisplayMillimeters) {
        return std::nullopt;
    }
    return PhysicalDisplaySize{
        static_cast<std::uint16_t>(Width),
        static_cast<std::uint16_t>(Height)};
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

    const auto Bounds = DisplayRect{
        Info.rcMonitor.left, Info.rcMonitor.top,
        Info.rcMonitor.right, Info.rcMonitor.bottom};
    auto PixelWidth = Target->second.PixelWidth;
    auto PixelHeight = Target->second.PixelHeight;
    if (PixelWidth == 0) {
        PixelWidth = static_cast<std::uint32_t>(
            static_cast<std::int64_t>(Bounds.Right) - Bounds.Left);
    }
    if (PixelHeight == 0) {
        PixelHeight = static_cast<std::uint32_t>(
            static_cast<std::int64_t>(Bounds.Bottom) - Bounds.Top);
    }
    auto Physical = Target->second.PhysicalSize;
    auto PhysicalKind = Target->second.PhysicalSizeKind;
    if (PhysicalKind == PhysicalSizeSource::Unknown) {
        const auto Estimate = EstimatePhysicalSize(
            Monitor, PixelWidth, PixelHeight);
        if (Estimate) {
            Physical = *Estimate;
            PhysicalKind = PhysicalSizeSource::RawDpiEstimate;
        }
    }
    if (PhysicalKind == PhysicalSizeSource::Edid) {
        const auto Oriented = OrientPhysicalDisplaySize(
            Physical, Target->second.Orientation);
        if (!Oriented) {
            Context.Failed = true;
            return FALSE;
        }
        Physical = *Oriented;
    }

    Context.Displays.push_back(DiscoveredDisplay{
        "win32-displayconfig:" + *StableIdentity,
        FriendlyName->empty() ? *SourceName : *FriendlyName,
        Bounds,
        (Info.dwFlags & MONITORINFOF_PRIMARY) != 0,
        PixelWidth,
        PixelHeight,
        Target->second.RefreshMilliHertz,
        Physical.WidthMillimeters,
        Physical.HeightMillimeters,
        PhysicalKind,
        Target->second.Orientation,
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
        std::uint32_t PixelWidth{};
        std::uint32_t PixelHeight{};
        if (Path.sourceInfo.modeInfoIdx !=
                DISPLAYCONFIG_PATH_MODE_IDX_INVALID &&
            Path.sourceInfo.modeInfoIdx < ModeCount) {
            const auto& Mode = Modes[Path.sourceInfo.modeInfoIdx];
            if (Mode.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
                PixelWidth = Mode.sourceMode.width;
                PixelHeight = Mode.sourceMode.height;
            }
        }
        const auto Refresh = RefreshMilliHertz(Path.targetInfo.refreshRate);
        const auto Orientation = ToOrientation(Path.targetInfo.rotation);
        if (!Refresh || !Orientation) return std::nullopt;
        auto Physical = ReadMonitorEdid(Target.monitorDevicePath);
        const auto PhysicalKind = Physical
            ? PhysicalSizeSource::Edid
            : PhysicalSizeSource::Unknown;
        if (SourceName.empty() || StableIdentity.empty() ||
            !Result.emplace(
                SourceName,
                TargetIdentity{
                    StableIdentity,
                    FriendlyName,
                    PixelWidth,
                    PixelHeight,
                    *Refresh,
                    Physical.value_or(PhysicalDisplaySize{}),
                    PhysicalKind,
                    *Orientation,
                }).second) {
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
