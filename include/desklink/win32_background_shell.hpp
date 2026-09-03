#pragma once

#ifdef _WIN32

#include "desklink/win32_application_settings.hpp"

#include <filesystem>
#include <functional>
#include <memory>

namespace desklink {

// Owns the lightweight, durable tray and global-hotkey surface. The WinUI
// settings process can exit independently while this shell keeps the broker
// reachable and the emergency/product shortcuts registered.
class Win32BackgroundShell final {
public:
    Win32BackgroundShell(
        std::filesystem::path ProductShellExecutable,
        Win32ProductPreferencesStore& PreferencesStore,
        std::function<void()> RequestExit);
    ~Win32BackgroundShell();

    Win32BackgroundShell(const Win32BackgroundShell&) = delete;
    Win32BackgroundShell& operator=(const Win32BackgroundShell&) = delete;

    [[nodiscard]] bool Start();
    void Stop() noexcept;
    [[nodiscard]] bool ApplyPreferences(
        const ProductPreferences& Preferences) noexcept;
    void PreferencesChanged() noexcept;

private:
    class Implementation;
    std::unique_ptr<Implementation> Implementation_;
};

} // namespace desklink

#endif
