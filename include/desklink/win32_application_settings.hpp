#pragma once

#ifdef _WIN32

#include <filesystem>
#include <mutex>
#include <optional>

namespace desklink {

struct Win32ApplicationSettings {
    bool CloseToTray{true};
    bool RunAtLogin{};
    bool FirstRunComplete{};

    [[nodiscard]] bool operator==(
        const Win32ApplicationSettings&) const noexcept = default;
};

class Win32ApplicationSettingsStore final {
public:
    explicit Win32ApplicationSettingsStore(std::filesystem::path Path);

    [[nodiscard]] bool Load();
    [[nodiscard]] bool IsLoaded() const noexcept;
    [[nodiscard]] std::optional<Win32ApplicationSettings> Current() const;
    [[nodiscard]] bool Save(Win32ApplicationSettings Settings);

private:
    [[nodiscard]] bool SaveLocked(
        const Win32ApplicationSettings& Settings) const;

    std::filesystem::path Path_;
    mutable std::mutex Mutex_;
    Win32ApplicationSettings Current_;
    bool Loaded_{};
};

[[nodiscard]] bool SetWin32RunAtLogin(
    bool Enabled, const std::filesystem::path& Executable);

} // namespace desklink

#endif
