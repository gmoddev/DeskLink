#pragma once

#ifdef _WIN32

#include "desklink/roaming.hpp"

#include <filesystem>
#include <mutex>
#include <optional>

namespace desklink {

// This store contains presentation and roaming preferences only. Trust records,
// certificates, pins, capabilities, and device identity never belong here.
class Win32RoamingSettingsStore final {
public:
    explicit Win32RoamingSettingsStore(std::filesystem::path Path);

    [[nodiscard]] bool Load();
    [[nodiscard]] bool IsLoaded() const noexcept;
    [[nodiscard]] std::optional<RoamingConfiguration> Current() const;
    [[nodiscard]] bool Save(RoamingConfiguration Configuration);

private:
    [[nodiscard]] bool SaveLocked(
        const RoamingConfiguration& Configuration) const;

    std::filesystem::path Path_;
    mutable std::mutex Mutex_;
    RoamingConfiguration Current_;
    bool Loaded_{};
};

} // namespace desklink

#endif
