#include "desklink/focus.hpp"

namespace desklink {

InputFocusStateMachine::InputFocusStateMachine(const IClock& clock) noexcept : clock_(clock) {}

bool InputFocusStateMachine::lease_active() const noexcept {
    return focus_ == FocusLocation::Remote && clock_.now() < lease_expiry_;
}

void InputFocusStateMachine::invalidate_epoch() noexcept {
    ++epoch_;
    if (epoch_ == 0) ++epoch_;
}

void InputFocusStateMachine::set_mode(DeskMode mode) noexcept {
    mode_ = mode;
    if (mode == DeskMode::Game || mode == DeskMode::LockPc1) {
        release_remote_focus();
    }
}

std::uint64_t InputFocusStateMachine::begin_remote_focus(std::chrono::milliseconds lease_duration) noexcept {
    if (mode_ == DeskMode::Game || mode_ == DeskMode::LockPc1 || lease_duration.count() <= 0) {
        return 0;
    }
    invalidate_epoch();
    focus_ = FocusLocation::Remote;
    lease_expiry_ = clock_.now() + lease_duration;
    return epoch_;
}

bool InputFocusStateMachine::renew(std::uint64_t epoch, std::chrono::milliseconds lease_duration) noexcept {
    if (!accepts_remote_input(epoch) || lease_duration.count() <= 0) return false;
    lease_expiry_ = clock_.now() + lease_duration;
    return true;
}

bool InputFocusStateMachine::accepts_remote_input(std::uint64_t epoch) const noexcept {
    return epoch != 0 && epoch == epoch_ && lease_active() &&
           mode_ != DeskMode::Game && mode_ != DeskMode::LockPc1;
}

void InputFocusStateMachine::release_remote_focus() noexcept {
    const bool was_remote = focus_ == FocusLocation::Remote;
    focus_ = FocusLocation::Local;
    lease_expiry_ = {};
    if (was_remote) invalidate_epoch();
}

void InputFocusStateMachine::emergency_fail_local() noexcept {
    mode_ = DeskMode::LockPc1;
    focus_ = FocusLocation::Local;
    lease_expiry_ = {};
    invalidate_epoch();
}

bool InputFocusStateMachine::poll_expiry() noexcept {
    if (focus_ == FocusLocation::Remote && clock_.now() >= lease_expiry_) {
        focus_ = FocusLocation::Local;
        lease_expiry_ = {};
        invalidate_epoch();
        return true;
    }
    return false;
}

} // namespace desklink
