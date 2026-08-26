#ifdef _WIN32

#include "desklink/win32_application_settings.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <utility>

namespace desklink {
namespace {

constexpr std::array<std::uint8_t, 4> kLegacyMagic{'D', 'L', 'A', 'S'};
constexpr std::array<std::uint8_t, 4> kPreferencesMagic{'D', 'L', 'P', 'P'};
constexpr std::uint16_t kLegacyVersion = 1;
constexpr std::size_t kLegacySize = 12;
constexpr std::size_t kPreferencesSize = 64;
constexpr std::uint16_t kKnownPreferenceFlags = 0x00ffu;
constexpr wchar_t kRunKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"DeskLink";

[[nodiscard]] std::uint16_t ReadU16(
    std::span<const std::uint8_t> Bytes, std::size_t Offset) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(Bytes[Offset]) << 8u) |
        Bytes[Offset + 1]);
}

void WriteU16(std::span<std::uint8_t> Bytes, std::size_t Offset,
              std::uint16_t Value) noexcept {
    Bytes[Offset] = static_cast<std::uint8_t>(Value >> 8u);
    Bytes[Offset + 1] = static_cast<std::uint8_t>(Value);
}

[[nodiscard]] std::array<std::uint8_t, kPreferencesSize> Encode(
    const ProductPreferences& Preferences) noexcept {
    std::array<std::uint8_t, kPreferencesSize> Result{};
    std::copy(
        kPreferencesMagic.begin(), kPreferencesMagic.end(), Result.begin());
    WriteU16(Result, 4, kProductPreferencesSchemaVersion);
    Result[6] = static_cast<std::uint8_t>(Preferences.Role);
    Result[7] = static_cast<std::uint8_t>(Preferences.AudioRoute);
    Result[8] = static_cast<std::uint8_t>(Preferences.Gaming);
    std::uint16_t Flags = 0;
    if (Preferences.CloseToTray) Flags |= 0x0001u;
    if (Preferences.RunAtLogin) Flags |= 0x0002u;
    if (Preferences.FirstRunComplete) Flags |= 0x0004u;
    if (Preferences.AutoStartRuntime) Flags |= 0x0008u;
    if (Preferences.AutoConnect) Flags |= 0x0010u;
    if (Preferences.InputRoamingDesired) Flags |= 0x0020u;
    if (Preferences.ClipboardDesired) Flags |= 0x0040u;
    if (Preferences.AdvancedModeEnabled) Flags |= 0x0080u;
    WriteU16(Result, 10, Flags);
    WriteU16(Result, 12, Preferences.AudioGainPermyriad);
    if (Preferences.PreferredPeerMachine) {
        std::copy(
            Preferences.PreferredPeerMachine->begin(),
            Preferences.PreferredPeerMachine->end(), Result.begin() + 16);
    }
    return Result;
}

[[nodiscard]] std::optional<ProductPreferences> DecodePreferences(
    std::span<const std::uint8_t> Bytes) noexcept {
    if (Bytes.size() != kPreferencesSize ||
        !std::equal(
            kPreferencesMagic.begin(), kPreferencesMagic.end(),
            Bytes.begin()) ||
        ReadU16(Bytes, 4) != kProductPreferencesSchemaVersion ||
        Bytes[9] != 0 || Bytes[14] != 0 || Bytes[15] != 0 ||
        std::any_of(Bytes.begin() + 32, Bytes.end(),
                    [](std::uint8_t Value) { return Value != 0; })) {
        return std::nullopt;
    }
    const auto Flags = ReadU16(Bytes, 10);
    if ((Flags & ~kKnownPreferenceFlags) != 0) return std::nullopt;

    ProductPreferences Result;
    Result.Role = static_cast<DeskRole>(Bytes[6]);
    Result.AudioRoute = static_cast<AudioRoutePreference>(Bytes[7]);
    Result.Gaming = static_cast<GamingBehavior>(Bytes[8]);
    Result.CloseToTray = (Flags & 0x0001u) != 0;
    Result.RunAtLogin = (Flags & 0x0002u) != 0;
    Result.FirstRunComplete = (Flags & 0x0004u) != 0;
    Result.AutoStartRuntime = (Flags & 0x0008u) != 0;
    Result.AutoConnect = (Flags & 0x0010u) != 0;
    Result.InputRoamingDesired = (Flags & 0x0020u) != 0;
    Result.ClipboardDesired = (Flags & 0x0040u) != 0;
    Result.AdvancedModeEnabled = (Flags & 0x0080u) != 0;
    Result.AudioGainPermyriad = ReadU16(Bytes, 12);
    MachineId Preferred{};
    std::copy(Bytes.begin() + 16, Bytes.begin() + 32, Preferred.begin());
    if (std::any_of(
            Preferred.begin(), Preferred.end(),
            [](std::uint8_t Value) { return Value != 0; })) {
        Result.PreferredPeerMachine = Preferred;
    }
    return IsValidProductPreferences(Result)
        ? std::optional<ProductPreferences>(Result)
        : std::nullopt;
}

[[nodiscard]] std::optional<ProductPreferences> DecodeLegacy(
    std::span<const std::uint8_t> Bytes) noexcept {
    if (Bytes.size() != kLegacySize ||
        !std::equal(kLegacyMagic.begin(), kLegacyMagic.end(), Bytes.begin()) ||
        ReadU16(Bytes, 4) != kLegacyVersion ||
        std::any_of(Bytes.begin() + 8, Bytes.end(),
                    [](std::uint8_t Value) { return Value != 0; })) {
        return std::nullopt;
    }
    const auto Flags = ReadU16(Bytes, 6);
    if ((Flags & 0xfff8u) != 0) return std::nullopt;
    ProductPreferences Result;
    Result.CloseToTray = (Flags & 0x0001u) != 0;
    Result.RunAtLogin = (Flags & 0x0002u) != 0;
    Result.FirstRunComplete = (Flags & 0x0004u) != 0;
    return Result;
}

[[nodiscard]] bool WriteAtomic(
    const std::filesystem::path& Path,
    std::span<const std::uint8_t> Bytes) {
    std::error_code Error;
    const auto Parent = Path.parent_path();
    if (!Parent.empty()) {
        std::filesystem::create_directories(Parent, Error);
        if (Error) return false;
    }
    auto Temporary = Path;
    Temporary += L".tmp";
    const auto File = CreateFileW(
        Temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (File == INVALID_HANDLE_VALUE) return false;
    DWORD Written{};
    const auto WrittenOk = WriteFile(
        File, Bytes.data(), static_cast<DWORD>(Bytes.size()), &Written,
        nullptr) != FALSE && Written == static_cast<DWORD>(Bytes.size());
    const auto Flushed = FlushFileBuffers(File) != FALSE;
    CloseHandle(File);
    if (!WrittenOk || !Flushed || MoveFileExW(
            Temporary.c_str(), Path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        DeleteFileW(Temporary.c_str());
        return false;
    }
    return true;
}

} // namespace

Win32ProductPreferencesStore::Win32ProductPreferencesStore(
    std::filesystem::path Path)
    : Path_(std::move(Path)) {}

bool Win32ProductPreferencesStore::Load() {
    std::scoped_lock Lock(Mutex_);
    Loaded_ = false;
    Current_ = {};
    const auto File = CreateFileW(
        Path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (File == INVALID_HANDLE_VALUE) {
        const auto Error = GetLastError();
        if (Error != ERROR_FILE_NOT_FOUND && Error != ERROR_PATH_NOT_FOUND) {
            return false;
        }
        Loaded_ = true;
        return true;
    }

    LARGE_INTEGER Size{};
    if (GetFileSizeEx(File, &Size) == FALSE ||
        (Size.QuadPart != static_cast<LONGLONG>(kLegacySize) &&
         Size.QuadPart != static_cast<LONGLONG>(kPreferencesSize))) {
        CloseHandle(File);
        return false;
    }
    std::array<std::uint8_t, kPreferencesSize> Bytes{};
    const auto ByteCount = static_cast<DWORD>(Size.QuadPart);
    DWORD Read{};
    const auto Exact = ReadFile(
        File, Bytes.data(), ByteCount, &Read, nullptr) != FALSE &&
        Read == ByteCount;
    CloseHandle(File);
    if (!Exact) return false;

    const auto View = std::span<const std::uint8_t>(Bytes.data(), ByteCount);
    const bool Legacy = ByteCount == kLegacySize;
    const auto Parsed = Legacy ? DecodeLegacy(View) : DecodePreferences(View);
    if (!Parsed || (Legacy && !WriteAtomic(Path_, Encode(*Parsed)))) {
        return false;
    }
    Current_ = *Parsed;
    Loaded_ = true;
    return true;
}

bool Win32ProductPreferencesStore::IsLoaded() const noexcept {
    std::scoped_lock Lock(Mutex_);
    return Loaded_;
}

std::optional<ProductPreferences>
Win32ProductPreferencesStore::Current() const {
    std::scoped_lock Lock(Mutex_);
    return Loaded_
        ? std::optional<ProductPreferences>(Current_)
        : std::nullopt;
}

bool Win32ProductPreferencesStore::Save(ProductPreferences Preferences) {
    std::scoped_lock Lock(Mutex_);
    if (!Loaded_ || !IsValidProductPreferences(Preferences) ||
        !SaveLocked(Preferences)) {
        return false;
    }
    Current_ = std::move(Preferences);
    return true;
}

bool Win32ProductPreferencesStore::SaveLocked(
    const ProductPreferences& Preferences) const {
    return WriteAtomic(Path_, Encode(Preferences));
}

bool SetWin32RunAtLogin(
    bool Enabled, const std::filesystem::path& Executable) {
    if (Executable.empty() || !Executable.is_absolute()) return false;
    HKEY Key{};
    if (RegCreateKeyExW(
            HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE,
            nullptr, &Key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    LSTATUS Status{};
    if (Enabled) {
        const auto Native = Executable.native();
        if (Native.find(L'"') != std::wstring::npos) {
            RegCloseKey(Key);
            return false;
        }
        const std::wstring Command = L"\"" + Native + L"\" --background";
        Status = RegSetValueExW(
            Key, kRunValue, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(Command.c_str()),
            static_cast<DWORD>((Command.size() + 1) * sizeof(wchar_t)));
    } else {
        Status = RegDeleteValueW(Key, kRunValue);
        if (Status == ERROR_FILE_NOT_FOUND) Status = ERROR_SUCCESS;
    }
    RegCloseKey(Key);
    return Status == ERROR_SUCCESS;
}

} // namespace desklink

#endif
