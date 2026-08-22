#pragma once

#include "desklink/protocol.hpp"
#include "desklink/types.hpp"

#include <chrono>
#include <cstdint>

namespace desklink {

enum class FocusLocation : std::uint8_t {
    Local = 0,
    Remote = 1,
};

class InputFocusStateMachine {
public:
    explicit InputFocusStateMachine(const IClock& clock) noexcept;

    [[nodiscard]] DeskMode mode() const noexcept { return mode_; }
    [[nodiscard]] FocusLocation focus() const noexcept { return focus_; }
    [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_; }
    [[nodiscard]] bool lease_active() const noexcept;

    void set_mode(DeskMode mode) noexcept;
    [[nodiscard]] std::uint64_t begin_remote_focus(std::chrono::milliseconds lease_duration) noexcept;
    [[nodiscard]] bool renew(std::uint64_t epoch, std::chrono::milliseconds lease_duration) noexcept;
    [[nodiscard]] bool accepts_remote_input(std::uint64_t epoch) const noexcept;
    void release_remote_focus() noexcept;
    void emergency_fail_local() noexcept;
    [[nodiscard]] bool poll_expiry() noexcept;

private:
    void invalidate_epoch() noexcept;

    const IClock& clock_;
    DeskMode mode_{DeskMode::Roam};
    FocusLocation focus_{FocusLocation::Local};
    std::uint64_t epoch_{1};
    IClock::time_point lease_expiry_{};
};

} // namespace desklink
