#pragma once

#ifdef _WIN32

#include "desklink/product.hpp"

#include <filesystem>
#include <mutex>
#include <optional>

namespace desklink {

class Win32ProductPreferencesStore final {
public:
    explicit Win32ProductPreferencesStore(std::filesystem::path Path);

    [[nodiscard]] bool Load();
    [[nodiscard]] bool IsLoaded() const noexcept;
    [[nodiscard]] std::optional<ProductPreferences> Current() const;
    [[nodiscard]] bool Save(ProductPreferences Preferences);

private:
    [[nodiscard]] bool SaveLocked(
        const ProductPreferences& Preferences) const;

    std::filesystem::path Path_;
    mutable std::mutex Mutex_;
    ProductPreferences Current_;
    bool Loaded_{};
};

[[nodiscard]] bool SetWin32RunAtLogin(
    bool Enabled, const std::filesystem::path& Executable);

} // namespace desklink

#endif
