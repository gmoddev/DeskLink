#ifdef _WIN32

#include "desklink/win32_roaming_settings.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <utility>

namespace desklink {
namespace {

[[nodiscard]] bool ReadSettingsFile(const std::filesystem::path& Path,
                                    ByteBuffer& Bytes) {
    const auto File = CreateFileW(
        Path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (File == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER Size{};
    const auto ValidSize = GetFileSizeEx(File, &Size) != FALSE &&
        Size.QuadPart > 0 &&
        static_cast<unsigned long long>(Size.QuadPart) <=
            kMaximumRoamingSettingsBytes;
    if (!ValidSize) {
        CloseHandle(File);
        return false;
    }
    Bytes.resize(static_cast<std::size_t>(Size.QuadPart));
    DWORD Read{};
    const auto ReadOk = ReadFile(
        File, Bytes.data(), static_cast<DWORD>(Bytes.size()), &Read, nullptr) !=
            FALSE &&
        Read == Bytes.size();
    CloseHandle(File);
    return ReadOk;
}

[[nodiscard]] bool WriteSettingsFileAtomic(
    const std::filesystem::path& Path, ByteSpan Bytes) {
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
    const auto WriteOk = WriteFile(
        File, Bytes.data(), static_cast<DWORD>(Bytes.size()), &Written,
        nullptr) != FALSE &&
        Written == Bytes.size();
    const auto FlushOk = FlushFileBuffers(File) != FALSE;
    CloseHandle(File);
    if (!WriteOk || !FlushOk ||
        MoveFileExW(
            Temporary.c_str(), Path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        DeleteFileW(Temporary.c_str());
        return false;
    }
    return true;
}

} // namespace

Win32RoamingSettingsStore::Win32RoamingSettingsStore(
    std::filesystem::path Path)
    : Path_(std::move(Path)) {}

bool Win32RoamingSettingsStore::Load() {
    std::scoped_lock Lock(Mutex_);
    Loaded_ = false;
    Current_ = {};
    const auto Attributes = GetFileAttributesW(Path_.c_str());
    if (Attributes == INVALID_FILE_ATTRIBUTES) {
        const auto Error = GetLastError();
        if (Error != ERROR_FILE_NOT_FOUND && Error != ERROR_PATH_NOT_FOUND) {
            return false;
        }
        Current_ = {};
        Loaded_ = true;
        return true;
    }
    if ((Attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return false;
    ByteBuffer Bytes;
    if (!ReadSettingsFile(Path_, Bytes)) return false;
    auto Parsed = DecodeRoamingConfiguration(Bytes);
    if (!Parsed) return false;
    Current_ = std::move(*Parsed);
    Loaded_ = true;
    return true;
}

bool Win32RoamingSettingsStore::IsLoaded() const noexcept {
    std::scoped_lock Lock(Mutex_);
    return Loaded_;
}

std::optional<RoamingConfiguration> Win32RoamingSettingsStore::Current() const {
    std::scoped_lock Lock(Mutex_);
    if (!Loaded_) return std::nullopt;
    return Current_;
}

bool Win32RoamingSettingsStore::Save(
    RoamingConfiguration Configuration) {
    if (!IsValidRoamingConfiguration(Configuration)) return false;
    std::scoped_lock Lock(Mutex_);
    if (!Loaded_ || !SaveLocked(Configuration)) return false;
    Current_ = std::move(Configuration);
    return true;
}

bool Win32RoamingSettingsStore::SaveLocked(
    const RoamingConfiguration& Configuration) const {
    const auto Bytes = EncodeRoamingConfiguration(Configuration);
    return Bytes && WriteSettingsFileAtomic(Path_, *Bytes);
}

} // namespace desklink

#endif
