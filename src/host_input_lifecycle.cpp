#include "desklink/host_input_lifecycle.hpp"

namespace desklink {
namespace {

bool IsValidDeskMode(DeskMode Mode) noexcept {
    return static_cast<std::uint8_t>(Mode) <=
           static_cast<std::uint8_t>(DeskMode::Game);
}

bool IsRestrictedMode(DeskMode Mode) noexcept {
    return Mode == DeskMode::LockPc1 || Mode == DeskMode::Game;
}

} // namespace

HostInputLifecycle::HostInputLifecycle(
    IHostInputLifecycleBackend& Backend,
    bool CaptureRequested) noexcept
    : Backend_(Backend), CaptureRequested_(CaptureRequested) {}

bool HostInputLifecycle::Start(DeskMode InitialMode) {
    if (Started_ || !IsValidDeskMode(InitialMode)) return false;
    Started_ = true;
    return IsRestrictedMode(InitialMode)
        ? ApplyRestrictedMode(InitialMode)
        : ApplyPermissiveMode(InitialMode);
}

bool HostInputLifecycle::ApplyMode(DeskMode Mode) {
    if (!Started_ || !IsValidDeskMode(Mode)) return false;
    if (Mode == Mode_) return true;
    return IsRestrictedMode(Mode)
        ? ApplyRestrictedMode(Mode)
        : ApplyPermissiveMode(Mode);
}

bool HostInputLifecycle::ApplyRestrictedMode(DeskMode Mode) {
    Backend_.DisableCapture();
    CaptureInstalled_ = false;

    if (State_ != HostInputLifecycleState::Local) {
        (void)Backend_.ReleaseFocus();
    }
    State_ = HostInputLifecycleState::Local;
    Backend_.StopCapture();

    Mode_ = Mode;
    return Backend_.SetDesiredMode(Mode);
}

bool HostInputLifecycle::ApplyPermissiveMode(DeskMode Mode) {
    if (!Backend_.SetDesiredMode(Mode)) {
        FailLocal();
        return false;
    }
    Mode_ = Mode;
    if (State_ == HostInputLifecycleState::Remote ||
        State_ == HostInputLifecycleState::AwaitingFocus) {
        return true;
    }
    if (!Backend_.RequestFocus()) {
        FailLocal();
        return false;
    }
    State_ = HostInputLifecycleState::AwaitingFocus;
    return true;
}

bool HostInputLifecycle::FocusReady() {
    if (!Started_ || State_ != HostInputLifecycleState::AwaitingFocus ||
        IsRestrictedMode(Mode_)) {
        return false;
    }
    if (!Backend_.SendInputStateSnapshot()) {
        FailLocal();
        return false;
    }
    if (CaptureRequested_) {
        if (!Backend_.StartCapture()) {
            FailLocal();
            return false;
        }
        CaptureInstalled_ = true;
        Backend_.EnableCapture();
    }
    State_ = HostInputLifecycleState::Remote;
    return true;
}

void HostInputLifecycle::FailLocal() noexcept {
    Backend_.DisableCapture();
    CaptureInstalled_ = false;
    (void)Backend_.ReleaseFocus();
    State_ = HostInputLifecycleState::Local;
    Backend_.StopCapture();
    Mode_ = DeskMode::LockPc1;
    (void)Backend_.SetDesiredMode(Mode_);
}

HostInputLifecycleStatus HostInputLifecycle::Status() const noexcept {
    return {Mode_, State_, CaptureRequested_, CaptureInstalled_, Started_};
}

} // namespace desklink
