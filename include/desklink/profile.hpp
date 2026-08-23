#pragma once

#include "desklink/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace desklink {

inline constexpr std::size_t kMaximumForegroundProfileRules = 32;
inline constexpr std::size_t kMaximumExecutableNameBytes = 260;

struct ForegroundWindowSnapshot {
    std::uint64_t WindowId{};
    std::string ExecutableName;
    bool Fullscreen{};
    bool Inspectable{};
};

struct ForegroundProfileRule {
    std::string ExecutableName;
    DeskMode Mode{DeskMode::Game};
    bool FullscreenOnly{};
};

enum class ProfileModeSource : std::uint8_t {
    SystemDefault = 0,
    ProfileRule = 1,
    ManualOverride = 2,
    ForegroundUnavailable = 3,
    Emergency = 4,
};

struct ProfileModeDecision {
    DeskMode Mode{DeskMode::Roam};
    ProfileModeSource Source{ProfileModeSource::SystemDefault};
    std::optional<std::size_t> RuleIndex;

    [[nodiscard]] bool operator==(const ProfileModeDecision&) const = default;
};

[[nodiscard]] bool IsValidForegroundWindowSnapshot(
    const ForegroundWindowSnapshot& Snapshot) noexcept;
[[nodiscard]] bool IsValidForegroundProfileRule(
    const ForegroundProfileRule& Rule) noexcept;
[[nodiscard]] std::string NormalizeExecutableName(std::string_view Name);

class ForegroundProfileEngine final {
public:
    explicit ForegroundProfileEngine(
        DeskMode SystemDefault = DeskMode::Roam) noexcept;

    [[nodiscard]] bool SetRules(std::vector<ForegroundProfileRule> Rules);
    [[nodiscard]] const std::vector<ForegroundProfileRule>& Rules() const noexcept {
        return Rules_;
    }

    [[nodiscard]] bool SetSystemDefault(DeskMode Mode) noexcept;
    void SetForeground(ForegroundWindowSnapshot Snapshot);
    void ClearForeground() noexcept;
    [[nodiscard]] bool SetManualOverride(DeskMode Mode) noexcept;
    void ClearManualOverride() noexcept;
    void EmergencyFailLocal() noexcept;
    void ClearEmergency() noexcept;

    [[nodiscard]] ProfileModeDecision Decision() const noexcept;

private:
    DeskMode SystemDefault_{DeskMode::Roam};
    std::vector<ForegroundProfileRule> Rules_;
    std::optional<ForegroundWindowSnapshot> Foreground_;
    std::optional<DeskMode> ManualOverride_;
    bool Emergency_{};
};

} // namespace desklink
