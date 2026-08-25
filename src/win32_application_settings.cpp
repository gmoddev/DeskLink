#ifdef _WIN32

#include "desklink/win32_application_settings.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

namespace desklink {
namespace {

constexpr std::array<std::uint8_t, 4> kSettingsMagic{'D', 'L', 'A', 'S'};
constexpr std::uint16_t kSettingsVersion = 1;
constexpr std::size_t kSettingsSize = 12;
constexpr wchar_t kRunKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"DeskLink";

[[nodiscard]] std::array<std::uint8_t, kSettingsSize> Encode(
    const Win32ApplicationSettings& Settings) noexcept {
    std::array<std::uint8_t, kSettingsSize> Result{};
    std::copy(kSettingsMagic.begin(), kSettingsMagic.end(), Result.begin());
    Result[4] = static_cast<std::uint8_t>(kSettingsVersion >> 8u);
    Result[5] = static_cast<std::uint8_t>(kSettingsVersion);
    std::uint16_t Flags = 0;
    if (Settings.CloseToTray) Flags |= 0x0001u;
    if (Settings.RunAtLogin) Flags |= 0x0002u;
    if (Settings.FirstRunComplete) Flags |= 0x0004u;
    Result[6] = static_cast<std::uint8_t>(Flags >> 8u);
    Result[7] = static_cast<std::uint8_t>(Flags);
    return Result;
}

[[nodiscard]] std::optional<Win32ApplicationSettings> Decode(
    const std::array<std::uint8_t, kSettingsSize>& Bytes) noexcept {
    if (!std::equal(
            kSettingsMagic.begin(), kSettingsMagic.end(), Bytes.begin())) {
        return std::nullopt;
    }
    const auto Version = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(Bytes[4]) << 8u) | Bytes[5]);
    const auto Flags = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(Bytes[6]) << 8u) | Bytes[7]);
    if (Version != kSettingsVersion || (Flags & 0xfff8u) != 0 ||
        std::any_of(Bytes.begin() + 8, Bytes.end(),
                    [](std::uint8_t Value) { return Value != 0; })) {
        return std::nullopt;
    }
    return Win32ApplicationSettings{
        (Flags & 0x0001u) != 0,
        (Flags & 0x0002u) != 0,
        (Flags & 0x0004u) != 0,
    };
}

[[nodiscard]] bool WriteAtomic(
    const std::filesystem::path& Path,
    const std::array<std::uint8_t, kSettingsSize>& Bytes) {
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

Win32ApplicationSettingsStore::Win32ApplicationSettingsStore(
    std::filesystem::path Path)
    : Path_(std::move(Path)) {}

bool Win32ApplicationSettingsStore::Load() {
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
    std::array<std::uint8_t, kSettingsSize> Bytes{};
    DWORD Read{};
    const auto ReadOk = ReadFile(
        File, Bytes.data(), static_cast<DWORD>(Bytes.size()), &Read,
        nullptr) != FALSE && Read == static_cast<DWORD>(Bytes.size());
    std::uint8_t Trailing{};
    DWORD TrailingRead{};
    const auto Exact = ReadOk && ReadFile(
        File, &Trailing, 1, &TrailingRead, nullptr) != FALSE &&
        TrailingRead == 0;
    CloseHandle(File);
    if (!Exact) return false;
    const auto Parsed = Decode(Bytes);
    if (!Parsed) return false;
    Current_ = *Parsed;
    Loaded_ = true;
    return true;
}

bool Win32ApplicationSettingsStore::IsLoaded() const noexcept {
    std::scoped_lock Lock(Mutex_);
    return Loaded_;
}

std::optional<Win32ApplicationSettings>
Win32ApplicationSettingsStore::Current() const {
    std::scoped_lock Lock(Mutex_);
    return Loaded_
        ? std::optional<Win32ApplicationSettings>(Current_)
        : std::nullopt;
}

bool Win32ApplicationSettingsStore::Save(
    Win32ApplicationSettings Settings) {
    std::scoped_lock Lock(Mutex_);
    if (!Loaded_ || !SaveLocked(Settings)) return false;
    Current_ = Settings;
    return true;
}

bool Win32ApplicationSettingsStore::SaveLocked(
    const Win32ApplicationSettings& Settings) const {
    return WriteAtomic(Path_, Encode(Settings));
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
