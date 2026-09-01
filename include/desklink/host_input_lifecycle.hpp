#pragma once

#include "desklink/protocol.hpp"

#include <cstdint>

namespace desklink {

enum class HostInputLifecycleState : std::uint8_t {
    Local = 0,
    AwaitingFocus = 1,
    Remote = 2,
};

struct HostInputLifecycleStatus {
    DeskMode Mode{DeskMode::LockPc1};
    HostInputLifecycleState State{HostInputLifecycleState::Local};
    bool CaptureRequested{};
    bool CaptureInstalled{};
    bool Started{};
};

class IHostInputLifecycleBackend {
public:
    virtual ~IHostInputLifecycleBackend() = default;

    virtual void DisableCapture() noexcept = 0;
    virtual void StopCapture() noexcept = 0;
    [[nodiscard]] virtual bool ReleaseFocus() noexcept = 0;
    [[nodiscard]] virtual bool SetDesiredMode(DeskMode Mode) noexcept = 0;
    [[nodiscard]] virtual bool RequestFocus() noexcept = 0;
    [[nodiscard]] virtual bool SendInputStateSnapshot() noexcept = 0;
    [[nodiscard]] virtual bool StartCapture() noexcept = 0;
    virtual void EnableCapture() noexcept = 0;
};

class HostInputLifecycle final {
public:
    HostInputLifecycle(IHostInputLifecycleBackend& Backend,
                       bool CaptureRequested) noexcept;

    [[nodiscard]] bool Start(DeskMode InitialMode = DeskMode::Roam);
    [[nodiscard]] bool ApplyMode(DeskMode Mode);
    [[nodiscard]] bool FocusReady();
    // Returns input Local without changing the selected policy. This is used
    // by ordinary roaming returns so Roam can re-arm after its cooldown.
    [[nodiscard]] bool ReturnLocal() noexcept;
    void FailLocal() noexcept;

    [[nodiscard]] HostInputLifecycleStatus Status() const noexcept;

private:
    [[nodiscard]] bool ApplyRestrictedMode(DeskMode Mode);
    [[nodiscard]] bool ApplyPermissiveMode(DeskMode Mode);

    IHostInputLifecycleBackend& Backend_;
    DeskMode Mode_{DeskMode::LockPc1};
    HostInputLifecycleState State_{HostInputLifecycleState::Local};
    bool CaptureRequested_{};
    bool CaptureInstalled_{};
    bool Started_{};
};

} // namespace desklink
