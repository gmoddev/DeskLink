#ifdef _WIN32

#include "desklink/win32_display_topology.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace desklink {
namespace {

struct TargetIdentity {
    std::string StableIdentity;
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

[[nodiscard]] std::optional<ByteBuffer> ReadMonitorEdid(
    std::wstring_view TargetPath) {
    constexpr GUID MonitorInterface{
        0xe6f07b5f, 0xee97, 0x4a90,
        {0xb0, 0x76, 0x33, 0xf5, 0x7b, 0xf4, 0xea, 0xa7}};
    const auto Devices = SetupDiGetClassDevsW(
        &MonitorInterface, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (Devices == INVALID_HANDLE_VALUE) return std::nullopt;

    std::optional<ByteBuffer> Result;
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
                Result = std::move(Edid);
            }
        }
        RegCloseKey(Key);
        break;
    }
    SetupDiDestroyDeviceInfoList(Devices);
    return Result;
}

[[nodiscard]] std::string NormalizeSerial(std::string Value) {
    Value.erase(
        std::remove_if(Value.begin(), Value.end(), [](unsigned char Character) {
            return Character == '\0' || std::isspace(Character);
        }),
        Value.end());
    std::transform(
        Value.begin(), Value.end(), Value.begin(), [](unsigned char Character) {
            return static_cast<char>(std::tolower(Character));
        });
    if (Value.empty()) return {};
    const auto AllDigits = std::all_of(
        Value.begin(), Value.end(), [](unsigned char Character) {
            return std::isdigit(Character) != 0;
        });
    if (AllDigits) {
        const auto Nonzero = Value.find_first_not_of('0');
        if (Nonzero == std::string::npos || Value.substr(Nonzero) == "1") {
            return {};
        }
    }
    return Value;
}

[[nodiscard]] std::string EdidSerial(ByteSpan Edid) {
    constexpr std::size_t BaseBlockSize = 128;
    if (Edid.size() < BaseBlockSize) return {};
    for (std::size_t Offset = 54; Offset + 18 <= BaseBlockSize;
         Offset += 18) {
        if (Edid[Offset] != 0 || Edid[Offset + 1] != 0 ||
            Edid[Offset + 2] != 0 || Edid[Offset + 3] != 0xff) {
            continue;
        }
        std::string Serial;
        for (std::size_t Index = Offset + 5; Index < Offset + 18; ++Index) {
            const auto Character = Edid[Index];
            if (Character == 0x0a) break;
            if (Character >= 0x20 && Character <= 0x7e) {
                Serial.push_back(static_cast<char>(Character));
            }
        }
        Serial = NormalizeSerial(std::move(Serial));
        if (!Serial.empty()) return Serial;
    }
    const auto Numeric = static_cast<std::uint32_t>(Edid[12]) |
        (static_cast<std::uint32_t>(Edid[13]) << 8u) |
        (static_cast<std::uint32_t>(Edid[14]) << 16u) |
        (static_cast<std::uint32_t>(Edid[15]) << 24u);
    if (Numeric <= 1 || Numeric == 0xffffffffu) return {};
    std::ostringstream Stream;
    Stream << std::hex << std::setfill('0') << std::setw(8) << Numeric;
    return Stream.str();
}

[[nodiscard]] std::uint64_t Fingerprint(ByteSpan Bytes) noexcept {
    std::uint64_t Result = 14695981039346656037ull;
    for (const auto Byte : Bytes) {
        Result ^= Byte;
        Result *= 1099511628211ull;
    }
    return Result;
}

[[nodiscard]] std::string StableDisplayIdentity(
    const DISPLAYCONFIG_TARGET_DEVICE_NAME& Target,
    const DISPLAYCONFIG_PATH_TARGET_INFO& PathTarget,
    ByteSpan Edid) {
    std::ostringstream Stream;
    Stream << "win32-edid:" << std::hex << std::setfill('0')
           << std::setw(4) << Target.edidManufactureId << '-'
           << std::setw(4) << Target.edidProductCodeId;
    const auto Serial = EdidSerial(Edid);
    if (!Serial.empty()) {
        Stream << ":serial:" << Serial;
    } else if (!Edid.empty()) {
        Stream << ":hash:" << std::setw(16) << Fingerprint(Edid)
               << ":connector:" << std::setw(8)
               << static_cast<std::uint32_t>(Target.outputTechnology)
               << '-' << std::setw(8) << Target.connectorInstance
               << '-' << std::setw(8) << PathTarget.id;
    } else {
        const auto Path = ToUtf8(NormalizeWide(Target.monitorDevicePath));
        Stream << ":path:" << (Path ? *Path : "unavailable");
    }
    return Stream.str();
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
    const auto FriendlyName = ToUtf8(Target->second.FriendlyName);
    const auto SourceName = ToUtf8(Info.szDevice);
    if (!FriendlyName || !SourceName ||
        Target->second.StableIdentity.empty()) {
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
        Target->second.StableIdentity,
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
        const auto Edid = ReadMonitorEdid(Target.monitorDevicePath);
        const auto Physical = Edid
            ? ParseEdidPhysicalSize(*Edid)
            : std::nullopt;
        const auto PhysicalKind = Physical
            ? PhysicalSizeSource::Edid
            : PhysicalSizeSource::Unknown;
        const auto StableIdentity = StableDisplayIdentity(
            Target, Path.targetInfo,
            Edid ? ByteSpan{*Edid} : ByteSpan{});
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

[[nodiscard]] std::optional<std::vector<ConnectedDisplayDescriptor>>
GetConnectedDisplays() {
    constexpr UINT32 QueryFlags = QDC_ALL_PATHS | QDC_VIRTUAL_MODE_AWARE;
    UINT32 PathCount{};
    UINT32 ModeCount{};
    if (GetDisplayConfigBufferSizes(
            QueryFlags, &PathCount, &ModeCount) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    if (PathCount == 0) return std::vector<ConnectedDisplayDescriptor>{};
    std::vector<DISPLAYCONFIG_PATH_INFO> Paths(PathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> Modes(ModeCount);
    if (QueryDisplayConfig(
            QueryFlags, &PathCount, Paths.data(), &ModeCount, Modes.data(),
            nullptr) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    Paths.resize(PathCount);

    std::set<std::string> Identities;
    std::vector<ConnectedDisplayDescriptor> Result;
    for (const auto& Path : Paths) {
        if (!Path.targetInfo.targetAvailable) continue;
        DISPLAYCONFIG_TARGET_DEVICE_NAME Target{};
        Target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        Target.header.size = sizeof(Target);
        Target.header.adapterId = Path.targetInfo.adapterId;
        Target.header.id = Path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&Target.header) != ERROR_SUCCESS ||
            Target.monitorDevicePath[0] == L'\0') {
            continue;
        }
        const auto Edid = ReadMonitorEdid(Target.monitorDevicePath);
        const auto Identity = StableDisplayIdentity(
            Target, Path.targetInfo,
            Edid ? ByteSpan{*Edid} : ByteSpan{});
        if (Identity.empty() || !Identities.insert(Identity).second) continue;
        const auto FriendlyName = ToUtf8(Target.monitorFriendlyDeviceName);
        if (!FriendlyName) return std::nullopt;
        Result.push_back({Identity, *FriendlyName});
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

std::optional<std::vector<ConnectedDisplayDescriptor>>
EnumerateWin32ConnectedDisplays() {
    return GetConnectedDisplays();
}

std::optional<Win32DisplayInventory> EnumerateWin32DisplayInventory() {
    auto Active = EnumerateWin32Displays();
    auto Connected = EnumerateWin32ConnectedDisplays();
    if (!Active || !Connected) return std::nullopt;
    for (const auto& Display : *Active) {
        const auto Match = std::find_if(
            Connected->begin(), Connected->end(), [&](const auto& Candidate) {
                return Candidate.StableIdentity == Display.StableIdentity;
            });
        if (Match == Connected->end()) {
            Connected->push_back({
                Display.StableIdentity, Display.FriendlyName});
        }
    }
    return Win32DisplayInventory{
        std::move(*Active), std::move(*Connected)};
}

bool Win32DisplayTopology::Refresh() {
    try {
        const auto Displays = EnumerateWin32Displays();
        if (!Displays) return false;
        const auto Now = std::chrono::steady_clock::now();
        constexpr auto ConnectedRefreshInterval = std::chrono::seconds(2);
        if (!HasConnectedRefresh_ ||
            Now - LastConnectedRefresh_ >= ConnectedRefreshInterval) {
            auto Connected = EnumerateWin32ConnectedDisplays();
            if (!Connected) return false;
            ConnectedDisplays_ = std::move(*Connected);
            LastConnectedRefresh_ = Now;
            HasConnectedRefresh_ = true;
        }
        for (const auto& Display : *Displays) {
            const auto Match = std::find_if(
                ConnectedDisplays_.begin(), ConnectedDisplays_.end(),
                [&](const auto& Candidate) {
                    return Candidate.StableIdentity == Display.StableIdentity;
                });
            if (Match == ConnectedDisplays_.end()) {
                ConnectedDisplays_.push_back({
                    Display.StableIdentity, Display.FriendlyName});
            }
        }
        if (Topology_.Update(*Displays, ConnectedDisplays_) ==
            DisplayTopologyUpdate::Invalid) {
            return false;
        }
        LastRefresh_ = Now;
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
