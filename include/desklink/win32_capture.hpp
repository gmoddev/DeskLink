#pragma once

#ifdef _WIN32

#include "desklink/protocol.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace desklink {

enum class Win32HookDecision {
    Pass,
    Suppress,
    Emergency,
};

class Win32SuppressionGate final {
public:
    void SetRemoteRouting(bool Enabled) noexcept;
    [[nodiscard]] bool RemoteRouting() const noexcept;
    [[nodiscard]] Win32HookDecision HandleKeyboard(
        std::uint32_t VirtualKey, bool Down, bool Injected) noexcept;
    [[nodiscard]] Win32HookDecision HandleMouse(bool Injected) const noexcept;

private:
    std::atomic_bool RemoteRouting_{};
    std::atomic_uint32_t ControlMask_{};
    std::atomic_uint32_t AltMask_{};
};

struct Win32CaptureHandlers {
    std::function<void(KeyEventMessage)> Key;
    std::function<void(MouseButtonMessage)> Button;
    std::function<void(PointerPositionMessage)> Pointer;
    std::function<void()> Emergency;
    std::function<void(std::string)> Failed;
};

class Win32InputCapture final {
public:
    struct State;

    explicit Win32InputCapture(Win32CaptureHandlers Handlers);
    ~Win32InputCapture();

    Win32InputCapture(const Win32InputCapture&) = delete;
    Win32InputCapture& operator=(const Win32InputCapture&) = delete;

    [[nodiscard]] bool Start();
    void SetRemoteRouting(bool Enabled) noexcept;
    [[nodiscard]] bool RemoteRouting() const noexcept;
    void Stop() noexcept;

private:
    std::unique_ptr<State> State_;
};

} // namespace desklink

#endif
