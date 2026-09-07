#pragma once

#ifdef _WIN32

#include "desklink/protocol.hpp"
#include "desklink/product.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace desklink {

enum class Win32HookDecision {
    Pass,
    Suppress,
    ReturnLocal,
    Emergency,
};

inline constexpr std::uint16_t kMinimumPointerGainPercent = 25;
inline constexpr std::uint16_t kMaximumPointerGainPercent = 400;
inline constexpr std::uint16_t kMinimumPointerDpi = 100;
inline constexpr std::uint16_t kMaximumPointerDpi = 32'000;
inline constexpr std::uint16_t kReferencePointerDpi = 800;

struct Win32PointerCalibration {
    std::uint16_t GainPercent{100};
    // Zero preserves raw device counts. A non-zero value normalizes physical
    // motion to the 800-DPI reference before applying GainPercent.
    std::uint16_t SourceDpi{};
};

struct Win32LocalPointerObservation {
    std::int32_t ScreenX{};
    std::int32_t ScreenY{};
    std::int32_t DeltaX{};
    std::int32_t DeltaY{};
};

[[nodiscard]] bool IsValidWin32PointerCalibration(
    const Win32PointerCalibration& Calibration) noexcept;

class PointerMotionScaler final {
public:
    explicit PointerMotionScaler(Win32PointerCalibration Calibration) noexcept;

    // A successful call may intentionally produce no packet while a fractional
    // count is retained for the next sample.
    [[nodiscard]] bool Scale(
        std::int32_t RawX,
        std::int32_t RawY,
        std::optional<PointerMotionMessage>& Motion) noexcept;
    void Reset() noexcept;

private:
    Win32PointerCalibration Calibration_;
    std::int64_t ResidualX_{};
    std::int64_t ResidualY_{};
};

class Win32SuppressionGate final {
public:
    void SetRemoteRouting(bool Enabled) noexcept;
    void SetReturnLocalHotkey(ProductHotkey Hotkey) noexcept;
    [[nodiscard]] bool RemoteRouting() const noexcept;
    [[nodiscard]] Win32HookDecision HandleKeyboard(
        std::uint32_t VirtualKey, bool Down, bool Injected) noexcept;
    [[nodiscard]] Win32HookDecision HandleMouse(bool Injected) const noexcept;

private:
    std::atomic_bool RemoteRouting_{};
    std::atomic_uint32_t ControlMask_{};
    std::atomic_uint32_t AltMask_{};
    std::atomic_uint32_t ShiftMask_{};
    std::atomic<ProductHotkey> ReturnLocalHotkey_{ProductHotkey::Off};
};

struct Win32CaptureHandlers {
    std::function<void(KeyEventMessage)> Key;
    std::function<void(MouseButtonMessage)> Button;
    std::function<void(PointerPositionMessage)> Pointer;
    std::function<void(PointerMotionMessage)> PointerMotion;
    std::function<void(Win32LocalPointerObservation)> LocalPointerMotion;
    std::function<void(MouseWheelMessage)> Wheel;
    std::function<void()> ReturnLocal;
    std::function<void()> Emergency;
    // Called after suppression has synchronously failed local whenever the
    // active Windows input desktop changes between Default and unavailable.
    std::function<void(bool Available)> InputDesktopChanged;
    std::function<void(std::string)> Failed;
};

class Win32InputCapture final {
public:
    struct State;

    explicit Win32InputCapture(
        Win32CaptureHandlers Handlers,
        Win32PointerCalibration Calibration = {});
    ~Win32InputCapture();

    Win32InputCapture(const Win32InputCapture&) = delete;
    Win32InputCapture& operator=(const Win32InputCapture&) = delete;

    [[nodiscard]] bool Start();
    void SetReturnLocalHotkey(ProductHotkey Hotkey) noexcept;
    void SetRemoteRouting(bool Enabled) noexcept;
    [[nodiscard]] bool RemoteRouting() const noexcept;
    void Stop() noexcept;

private:
    std::unique_ptr<State> State_;
};

} // namespace desklink

#endif
