#ifdef _WIN32

#include "desklink/win32_capture.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

namespace desklink {
namespace {

constexpr UINT kEmergencyMessage = WM_APP + 0x44u;
constexpr UINT kStopMessage = WM_APP + 0x45u;
constexpr UINT kQueueOverflowMessage = WM_APP + 0x46u;
constexpr UINT kKeyboardCaptureFailureMessage = WM_APP + 0x47u;
constexpr UINT kMouseWheelCaptureFailureMessage = WM_APP + 0x48u;
constexpr UINT kReturnLocalMessage = WM_APP + 0x49u;
constexpr std::size_t kMaximumQueuedEvents = 1024;
constexpr wchar_t kCaptureWindowClass[] = L"DeskLink.RawInputCapture.v1";

using CapturedEvent = std::variant<
    KeyEventMessage, MouseButtonMessage, PointerPositionMessage,
    PointerMotionMessage, Win32LocalPointerObservation,
    MouseWheelMessage>;

std::atomic<Win32InputCapture::State*> ActiveState{};

std::uint32_t ModifierBit(std::uint32_t VirtualKey, bool Control) noexcept {
    if (Control) {
        if (VirtualKey == VK_LCONTROL) return 1u;
        if (VirtualKey == VK_RCONTROL) return 2u;
        if (VirtualKey == VK_CONTROL) return 4u;
    } else {
        if (VirtualKey == VK_LMENU) return 1u;
        if (VirtualKey == VK_RMENU) return 2u;
        if (VirtualKey == VK_MENU) return 4u;
    }
    return 0;
}

std::uint32_t ShiftModifierBit(std::uint32_t VirtualKey) noexcept {
    if (VirtualKey == VK_LSHIFT) return 1u;
    if (VirtualKey == VK_RSHIFT) return 2u;
    if (VirtualKey == VK_SHIFT) return 4u;
    return 0;
}

void UpdateModifier(std::atomic_uint32_t& Mask,
                    std::uint32_t Bit,
                    bool Down) noexcept {
    if (Bit == 0) return;
    if (Down) Mask.fetch_or(Bit, std::memory_order_relaxed);
    else Mask.fetch_and(~Bit, std::memory_order_relaxed);
}

std::optional<MouseButtonMessage> GetButton(const RAWMOUSE& Mouse,
                                             USHORT Flag,
                                             MouseButtonId Button,
                                             bool Down) {
    if ((Mouse.usButtonFlags & Flag) == 0) return std::nullopt;
    return MouseButtonMessage{Button, Down};
}

} // namespace

struct Win32InputCapture::State {
    State(Win32CaptureHandlers OwnedHandlers,
          Win32PointerCalibration Calibration)
        : Handlers(std::move(OwnedHandlers)), MotionScaler(Calibration) {}

    Win32CaptureHandlers Handlers;
    PointerMotionScaler MotionScaler;
    Win32SuppressionGate Gate;
    std::thread CaptureThread;
    std::thread WorkerThread;
    std::mutex StartMutex;
    std::condition_variable Started;
    std::mutex QueueMutex;
    std::condition_variable QueueChanged;
    std::deque<CapturedEvent> Queue;
    DWORD CaptureThreadId{};
    HWND Window{};
    HHOOK KeyboardHook{};
    HHOOK MouseHook{};
    std::int64_t PointerX{};
    std::int64_t PointerY{};
    std::int32_t AbsolutePointerX{};
    std::int32_t AbsolutePointerY{};
    bool AbsolutePointerInitialized{};
    bool StartComplete{};
    bool StartSucceeded{};
    bool StopWorker{};

    void ClearQueue() {
        std::scoped_lock Lock(QueueMutex);
        Queue.clear();
    }

    void Fail(std::string Message) {
        Gate.SetRemoteRouting(false);
        ClearQueue();
        if (Handlers.Failed) Handlers.Failed(std::move(Message));
    }

    void Emergency() {
        Gate.SetRemoteRouting(false);
        ClearQueue();
        if (Handlers.Emergency) Handlers.Emergency();
    }

    void ReturnLocal() {
        Gate.SetRemoteRouting(false);
        ClearQueue();
        if (Handlers.ReturnLocal) Handlers.ReturnLocal();
    }

    bool Enqueue(CapturedEvent Event) {
        std::scoped_lock Lock(QueueMutex);
        if (std::holds_alternative<PointerPositionMessage>(Event) && !Queue.empty() &&
            std::holds_alternative<PointerPositionMessage>(Queue.back())) {
            Queue.back() = std::move(Event);
            QueueChanged.notify_one();
            return true;
        }
        if (std::holds_alternative<PointerMotionMessage>(Event) && !Queue.empty() &&
            std::holds_alternative<PointerMotionMessage>(Queue.back())) {
            const auto& Incoming = std::get<PointerMotionMessage>(Event);
            const auto& Existing = std::get<PointerMotionMessage>(Queue.back());
            const auto DeltaX = static_cast<std::int64_t>(Existing.DeltaX) +
                                Incoming.DeltaX;
            const auto DeltaY = static_cast<std::int64_t>(Existing.DeltaY) +
                                Incoming.DeltaY;
            if (DeltaX >= std::numeric_limits<std::int32_t>::min() &&
                DeltaX <= std::numeric_limits<std::int32_t>::max() &&
                DeltaY >= std::numeric_limits<std::int32_t>::min() &&
                DeltaY <= std::numeric_limits<std::int32_t>::max()) {
                const PointerMotionMessage Combined{
                    static_cast<std::int32_t>(DeltaX),
                    static_cast<std::int32_t>(DeltaY)};
                if (IsValidPointerMotionMessage(Combined)) {
                    Queue.back() = Combined;
                    QueueChanged.notify_one();
                    return true;
                }
            }
        }
        if (std::holds_alternative<Win32LocalPointerObservation>(Event) &&
            !Queue.empty() &&
            std::holds_alternative<Win32LocalPointerObservation>(
                Queue.back())) {
            const auto& Incoming =
                std::get<Win32LocalPointerObservation>(Event);
            const auto& Existing =
                std::get<Win32LocalPointerObservation>(Queue.back());
            const auto DeltaX = static_cast<std::int64_t>(Existing.DeltaX) +
                                Incoming.DeltaX;
            const auto DeltaY = static_cast<std::int64_t>(Existing.DeltaY) +
                                Incoming.DeltaY;
            if (DeltaX >= std::numeric_limits<std::int32_t>::min() &&
                DeltaX <= std::numeric_limits<std::int32_t>::max() &&
                DeltaY >= std::numeric_limits<std::int32_t>::min() &&
                DeltaY <= std::numeric_limits<std::int32_t>::max()) {
                Queue.back() = Win32LocalPointerObservation{
                    Incoming.ScreenX,
                    Incoming.ScreenY,
                    static_cast<std::int32_t>(DeltaX),
                    static_cast<std::int32_t>(DeltaY)};
                QueueChanged.notify_one();
                return true;
            }
        }
        if (Queue.size() >= kMaximumQueuedEvents) return false;
        Queue.push_back(std::move(Event));
        QueueChanged.notify_one();
        return true;
    }

    bool TryEnqueueHookEvent(CapturedEvent Event) {
        std::unique_lock Lock(QueueMutex, std::try_to_lock);
        if (!Lock.owns_lock() || Queue.size() >= kMaximumQueuedEvents) {
            return false;
        }
        Queue.push_back(std::move(Event));
        Lock.unlock();
        QueueChanged.notify_one();
        return true;
    }

    void HandleRawInput(HRAWINPUT Handle) {
        alignas(DWORD) std::array<std::byte, sizeof(RAWINPUT)> Storage{};
        UINT Size = static_cast<UINT>(Storage.size());
        const auto Read = GetRawInputData(
            Handle, RID_INPUT, Storage.data(), &Size, sizeof(RAWINPUTHEADER));
        if (Read == static_cast<UINT>(-1) || Read > Storage.size() ||
            Read < sizeof(RAWINPUTHEADER)) {
            Fail("could not read bounded Raw Input data");
            return;
        }
        const auto* Input = reinterpret_cast<const RAWINPUT*>(Storage.data());
        if (Input->header.dwType != RIM_TYPEMOUSE || Read < sizeof(RAWINPUT)) return;
        const auto& Mouse = Input->data.mouse;
        const auto RemoteRouting = Gate.RemoteRouting();
        if (RemoteRouting) {
            const std::array Buttons{
                GetButton(Mouse, RI_MOUSE_LEFT_BUTTON_DOWN,
                          MouseButtonId::Left, true),
                GetButton(Mouse, RI_MOUSE_LEFT_BUTTON_UP,
                          MouseButtonId::Left, false),
                GetButton(Mouse, RI_MOUSE_RIGHT_BUTTON_DOWN,
                          MouseButtonId::Right, true),
                GetButton(Mouse, RI_MOUSE_RIGHT_BUTTON_UP,
                          MouseButtonId::Right, false),
                GetButton(Mouse, RI_MOUSE_MIDDLE_BUTTON_DOWN,
                          MouseButtonId::Middle, true),
                GetButton(Mouse, RI_MOUSE_MIDDLE_BUTTON_UP,
                          MouseButtonId::Middle, false),
                GetButton(Mouse, RI_MOUSE_BUTTON_4_DOWN,
                          MouseButtonId::X1, true),
                GetButton(Mouse, RI_MOUSE_BUTTON_4_UP,
                          MouseButtonId::X1, false),
                GetButton(Mouse, RI_MOUSE_BUTTON_5_DOWN,
                          MouseButtonId::X2, true),
                GetButton(Mouse, RI_MOUSE_BUTTON_5_UP,
                          MouseButtonId::X2, false)};
            for (const auto& Button : Buttons) {
                if (Button && !Enqueue(*Button)) {
                    Gate.SetRemoteRouting(false);
                    PostThreadMessageW(
                        CaptureThreadId, kQueueOverflowMessage, 0, 0);
                    return;
                }
            }
        }

        std::int32_t RawX{};
        std::int32_t RawY{};
        if ((Mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0) {
            const auto CurrentX = std::clamp<std::int32_t>(Mouse.lLastX, 0, 65'535);
            const auto CurrentY = std::clamp<std::int32_t>(Mouse.lLastY, 0, 65'535);
            if (!AbsolutePointerInitialized) {
                AbsolutePointerX = CurrentX;
                AbsolutePointerY = CurrentY;
                AbsolutePointerInitialized = true;
                return;
            }
            const auto Width = std::max(GetSystemMetrics(SM_CXVIRTUALSCREEN), 1);
            const auto Height = std::max(GetSystemMetrics(SM_CYVIRTUALSCREEN), 1);
            RawX = static_cast<std::int32_t>(
                static_cast<std::int64_t>(CurrentX - AbsolutePointerX) * Width /
                65'535);
            RawY = static_cast<std::int32_t>(
                static_cast<std::int64_t>(CurrentY - AbsolutePointerY) * Height /
                65'535);
            AbsolutePointerX = CurrentX;
            AbsolutePointerY = CurrentY;
        } else if (Mouse.lLastX != 0 || Mouse.lLastY != 0) {
            RawX = Mouse.lLastX;
            RawY = Mouse.lLastY;
        } else {
            return;
        }
        if (!RemoteRouting) {
            if (!Handlers.LocalPointerMotion) return;
            POINT Cursor{};
            if (!GetCursorPos(&Cursor)) {
                Fail("could not observe the local pointer position");
                return;
            }
            if (!Enqueue(Win32LocalPointerObservation{
                    Cursor.x, Cursor.y, RawX, RawY})) {
                Gate.SetRemoteRouting(false);
                PostThreadMessageW(
                    CaptureThreadId, kQueueOverflowMessage, 0, 0);
            }
            return;
        }
        std::optional<PointerMotionMessage> Motion;
        if (!MotionScaler.Scale(RawX, RawY, Motion)) {
            Fail("pointer calibration produced an invalid relative delta");
            return;
        }
        if (!Motion) return;
        if (!Enqueue(*Motion)) {
            Gate.SetRemoteRouting(false);
            PostThreadMessageW(CaptureThreadId, kQueueOverflowMessage, 0, 0);
        }
    }
};

namespace {

LRESULT CALLBACK KeyboardHook(int Code, WPARAM Message, LPARAM Data) {
    auto* State = ActiveState.load(std::memory_order_acquire);
    if (Code != HC_ACTION || !State) {
        return CallNextHookEx(nullptr, Code, Message, Data);
    }
    const auto* Event = reinterpret_cast<const KBDLLHOOKSTRUCT*>(Data);
    const bool Down = Message == WM_KEYDOWN || Message == WM_SYSKEYDOWN;
    const bool Up = Message == WM_KEYUP || Message == WM_SYSKEYUP;
    if (!Down && !Up) return CallNextHookEx(nullptr, Code, Message, Data);
    const auto Decision = State->Gate.HandleKeyboard(
        Event->vkCode, Down, (Event->flags & LLKHF_INJECTED) != 0);
    if (Decision == Win32HookDecision::Emergency) {
        PostThreadMessageW(State->CaptureThreadId, kEmergencyMessage, 0, 0);
        return CallNextHookEx(nullptr, Code, Message, Data);
    }
    if (Decision == Win32HookDecision::ReturnLocal) {
        PostThreadMessageW(State->CaptureThreadId, kReturnLocalMessage, 0, 0);
        return 1;
    }
    if (Decision == Win32HookDecision::Suppress) {
        if (Event->scanCode == 0 || Event->scanCode > 255u) {
            State->Gate.SetRemoteRouting(false);
            PostThreadMessageW(
                State->CaptureThreadId, kKeyboardCaptureFailureMessage, 0, 0);
            return CallNextHookEx(nullptr, Code, Message, Data);
        }
        const bool Extended = (Event->flags & LLKHF_EXTENDED) != 0;
        if (!State->TryEnqueueHookEvent(KeyEventMessage{
                static_cast<std::uint16_t>(Event->scanCode), Extended, Down})) {
            State->Gate.SetRemoteRouting(false);
            PostThreadMessageW(
                State->CaptureThreadId, kKeyboardCaptureFailureMessage, 0, 0);
            return CallNextHookEx(nullptr, Code, Message, Data);
        }
        return 1;
    }
    return CallNextHookEx(nullptr, Code, Message, Data);
}

LRESULT CALLBACK MouseHook(int Code, WPARAM Message, LPARAM Data) {
    auto* State = ActiveState.load(std::memory_order_acquire);
    if (Code != HC_ACTION || !State) {
        return CallNextHookEx(nullptr, Code, Message, Data);
    }
    const auto* Event = reinterpret_cast<const MSLLHOOKSTRUCT*>(Data);
    if (Message == WM_MOUSEWHEEL || Message == WM_MOUSEHWHEEL) {
        if (State->Gate.HandleMouse((Event->flags & LLMHF_INJECTED) != 0) !=
            Win32HookDecision::Suppress) {
            return CallNextHookEx(nullptr, Code, Message, Data);
        }
        const auto Delta = static_cast<std::int16_t>(
            static_cast<SHORT>(HIWORD(Event->mouseData)));
        const MouseWheelMessage Wheel{
            Message == WM_MOUSEWHEEL
                ? MouseWheelAxis::Vertical : MouseWheelAxis::Horizontal,
            Delta};
        if (!IsValidMouseWheelMessage(Wheel) ||
            !State->TryEnqueueHookEvent(Wheel)) {
            State->Gate.SetRemoteRouting(false);
            PostThreadMessageW(
                State->CaptureThreadId, kMouseWheelCaptureFailureMessage, 0, 0);
            return CallNextHookEx(nullptr, Code, Message, Data);
        }
        return 1;
    }
    if (State->Gate.HandleMouse((Event->flags & LLMHF_INJECTED) != 0) ==
        Win32HookDecision::Suppress) {
        return 1;
    }
    return CallNextHookEx(nullptr, Code, Message, Data);
}

LRESULT CALLBACK CaptureWindowProc(HWND Window,
                                   UINT Message,
                                   WPARAM WParam,
                                   LPARAM LParam) {
    auto* State = reinterpret_cast<Win32InputCapture::State*>(
        GetWindowLongPtrW(Window, GWLP_USERDATA));
    if (Message == WM_NCCREATE) {
        const auto* Create = reinterpret_cast<const CREATESTRUCTW*>(LParam);
        State = static_cast<Win32InputCapture::State*>(Create->lpCreateParams);
        SetWindowLongPtrW(Window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(State));
    }
    if (Message == WM_INPUT && State) {
        State->HandleRawInput(reinterpret_cast<HRAWINPUT>(LParam));
        return DefWindowProcW(Window, Message, WParam, LParam);
    }
    return DefWindowProcW(Window, Message, WParam, LParam);
}

} // namespace

void Win32SuppressionGate::SetRemoteRouting(bool Enabled) noexcept {
    RemoteRouting_.store(Enabled, std::memory_order_release);
    if (!Enabled) {
        ControlMask_.store(0, std::memory_order_relaxed);
        AltMask_.store(0, std::memory_order_relaxed);
        ShiftMask_.store(0, std::memory_order_relaxed);
    }
}

void Win32SuppressionGate::SetReturnLocalHotkey(
    ProductHotkey Hotkey) noexcept {
    ReturnLocalHotkey_.store(
        IsValidProductHotkey(Hotkey) ? Hotkey : ProductHotkey::Off,
        std::memory_order_release);
}

bool Win32SuppressionGate::RemoteRouting() const noexcept {
    return RemoteRouting_.load(std::memory_order_acquire);
}

Win32HookDecision Win32SuppressionGate::HandleKeyboard(
    std::uint32_t VirtualKey, bool Down, bool Injected) noexcept {
    if (Injected) return Win32HookDecision::Pass;
    UpdateModifier(ControlMask_, ModifierBit(VirtualKey, true), Down);
    UpdateModifier(AltMask_, ModifierBit(VirtualKey, false), Down);
    UpdateModifier(ShiftMask_, ShiftModifierBit(VirtualKey), Down);
    if (Down && (VirtualKey == VK_PAUSE || VirtualKey == VK_CANCEL) &&
        RemoteRouting() &&
        ControlMask_.load(std::memory_order_relaxed) != 0 &&
        AltMask_.load(std::memory_order_relaxed) != 0) {
        SetRemoteRouting(false);
        return Win32HookDecision::Emergency;
    }
    const auto Hotkey = ReturnLocalHotkey_.load(std::memory_order_acquire);
    const bool FunctionKeyMatches =
        ((Hotkey == ProductHotkey::CtrlAltF11 ||
          Hotkey == ProductHotkey::CtrlShiftF11) && VirtualKey == VK_F11) ||
        ((Hotkey == ProductHotkey::CtrlAltF12 ||
          Hotkey == ProductHotkey::CtrlShiftF12) && VirtualKey == VK_F12);
    const bool ControlDown =
        ControlMask_.load(std::memory_order_relaxed) != 0;
    const bool AltDown = AltMask_.load(std::memory_order_relaxed) != 0;
    const bool ShiftDown = ShiftMask_.load(std::memory_order_relaxed) != 0;
    const bool ModifiersMatch =
        (Hotkey == ProductHotkey::CtrlAltF11 ||
         Hotkey == ProductHotkey::CtrlAltF12)
            ? ControlDown && AltDown && !ShiftDown
            : (Hotkey == ProductHotkey::CtrlShiftF11 ||
               Hotkey == ProductHotkey::CtrlShiftF12)
                ? ControlDown && ShiftDown && !AltDown
                : false;
    if (Down && RemoteRouting() && FunctionKeyMatches && ModifiersMatch) {
        SetRemoteRouting(false);
        return Win32HookDecision::ReturnLocal;
    }
    return RemoteRouting()
        ? Win32HookDecision::Suppress : Win32HookDecision::Pass;
}

Win32HookDecision Win32SuppressionGate::HandleMouse(bool Injected) const noexcept {
    return !Injected && RemoteRouting()
        ? Win32HookDecision::Suppress : Win32HookDecision::Pass;
}

bool IsValidWin32PointerCalibration(
    const Win32PointerCalibration& Calibration) noexcept {
    return Calibration.GainPercent >= kMinimumPointerGainPercent &&
           Calibration.GainPercent <= kMaximumPointerGainPercent &&
           (Calibration.SourceDpi == 0 ||
            (Calibration.SourceDpi >= kMinimumPointerDpi &&
             Calibration.SourceDpi <= kMaximumPointerDpi));
}

PointerMotionScaler::PointerMotionScaler(
    Win32PointerCalibration Calibration) noexcept
    : Calibration_(Calibration) {}

bool PointerMotionScaler::Scale(
    std::int32_t RawX,
    std::int32_t RawY,
    std::optional<PointerMotionMessage>& Motion) noexcept {
    Motion.reset();
    if (!IsValidWin32PointerCalibration(Calibration_)) return false;
    const auto DpiFactor = Calibration_.SourceDpi == 0
        ? 1ll : static_cast<std::int64_t>(kReferencePointerDpi);
    const auto Divisor = static_cast<std::int64_t>(100) *
        (Calibration_.SourceDpi == 0 ? 1 : Calibration_.SourceDpi);
    const auto ScaleAxis = [&](std::int32_t Raw, std::int64_t& Residual) {
        const auto Numerator = static_cast<std::int64_t>(Raw) *
            Calibration_.GainPercent * DpiFactor + Residual;
        const auto Scaled = Numerator / Divisor;
        Residual = Numerator - Scaled * Divisor;
        return Scaled;
    };
    const auto DeltaX = ScaleAxis(RawX, ResidualX_);
    const auto DeltaY = ScaleAxis(RawY, ResidualY_);
    if (DeltaX < std::numeric_limits<std::int32_t>::min() ||
        DeltaX > std::numeric_limits<std::int32_t>::max() ||
        DeltaY < std::numeric_limits<std::int32_t>::min() ||
        DeltaY > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }
    PointerMotionMessage Result{
        static_cast<std::int32_t>(DeltaX),
        static_cast<std::int32_t>(DeltaY)};
    if (Result.DeltaX == 0 && Result.DeltaY == 0) return true;
    if (!IsValidPointerMotionMessage(Result)) return false;
    Motion = Result;
    return true;
}

void PointerMotionScaler::Reset() noexcept {
    ResidualX_ = 0;
    ResidualY_ = 0;
}

Win32InputCapture::Win32InputCapture(
    Win32CaptureHandlers Handlers,
    Win32PointerCalibration Calibration)
    : State_(std::make_unique<State>(
          std::move(Handlers), Calibration)) {}

Win32InputCapture::~Win32InputCapture() { Stop(); }

bool Win32InputCapture::Start() {
    if (State_->CaptureThread.joinable()) return State_->StartSucceeded;
    State_->Gate.SetRemoteRouting(false);
    {
        std::scoped_lock Lock(State_->QueueMutex);
        State_->Queue.clear();
        State_->StopWorker = false;
    }
    State_->CaptureThreadId = 0;
    State_->Window = nullptr;
    State_->KeyboardHook = nullptr;
    State_->MouseHook = nullptr;
    State_->PointerX = 0;
    State_->PointerY = 0;
    State_->AbsolutePointerX = 0;
    State_->AbsolutePointerY = 0;
    State_->AbsolutePointerInitialized = false;
    State_->MotionScaler.Reset();
    State_->StartComplete = false;
    State_->StartSucceeded = false;
    State_->WorkerThread = std::thread([State = State_.get()] {
        for (;;) {
            CapturedEvent Event;
            {
                std::unique_lock Lock(State->QueueMutex);
                State->QueueChanged.wait(Lock, [&] {
                    return State->StopWorker || !State->Queue.empty();
                });
                if (State->StopWorker && State->Queue.empty()) return;
                Event = std::move(State->Queue.front());
                State->Queue.pop_front();
            }
            try {
                std::visit([&](auto&& Value) {
                    using ValueType = std::decay_t<decltype(Value)>;
                    if constexpr (std::is_same_v<ValueType, KeyEventMessage>) {
                        if (State->Handlers.Key) State->Handlers.Key(Value);
                    } else if constexpr (std::is_same_v<ValueType, MouseButtonMessage>) {
                        if (State->Handlers.Button) State->Handlers.Button(Value);
                    } else if constexpr (std::is_same_v<ValueType, PointerPositionMessage>) {
                        if (State->Handlers.Pointer) State->Handlers.Pointer(Value);
                    } else if constexpr (std::is_same_v<ValueType, PointerMotionMessage>) {
                        if (State->Handlers.PointerMotion) {
                            State->Handlers.PointerMotion(Value);
                        }
                    } else if constexpr (std::is_same_v<
                                             ValueType,
                                             Win32LocalPointerObservation>) {
                        if (State->Handlers.LocalPointerMotion) {
                            State->Handlers.LocalPointerMotion(Value);
                        }
                    } else if constexpr (std::is_same_v<ValueType, MouseWheelMessage>) {
                        if (State->Handlers.Wheel) State->Handlers.Wheel(Value);
                    }
                }, std::move(Event));
            } catch (...) {
                State->Fail("captured input handler failed");
            }
        }
    });
    State_->CaptureThread = std::thread([State = State_.get()] {
        State->CaptureThreadId = GetCurrentThreadId();
        Win32InputCapture::State* Expected = nullptr;
        bool Success = ActiveState.compare_exchange_strong(Expected, State);
        const auto Instance = GetModuleHandleW(nullptr);
        WNDCLASSW WindowClass{};
        WindowClass.lpfnWndProc = CaptureWindowProc;
        WindowClass.hInstance = Instance;
        WindowClass.lpszClassName = kCaptureWindowClass;
        if (Success && RegisterClassW(&WindowClass) == 0 &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            Success = false;
        }
        if (Success) {
            State->Window = CreateWindowExW(
                0, kCaptureWindowClass, L"", 0, 0, 0, 0, 0,
                HWND_MESSAGE, nullptr, Instance, State);
            Success = State->Window != nullptr;
        }
        if (Success) {
            const std::array Devices{
                RAWINPUTDEVICE{0x01, 0x02, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY,
                               State->Window}};
            Success = RegisterRawInputDevices(
                Devices.data(), static_cast<UINT>(Devices.size()),
                sizeof(RAWINPUTDEVICE)) != FALSE;
        }
        if (Success) {
            State->KeyboardHook = SetWindowsHookExW(
                WH_KEYBOARD_LL, KeyboardHook, Instance, 0);
            State->MouseHook = SetWindowsHookExW(
                WH_MOUSE_LL, MouseHook, Instance, 0);
            Success = State->KeyboardHook && State->MouseHook;
        }
        POINT Cursor{};
        if (Success && GetCursorPos(&Cursor)) {
            const auto Left = GetSystemMetrics(SM_XVIRTUALSCREEN);
            const auto Top = GetSystemMetrics(SM_YVIRTUALSCREEN);
            const auto Width = std::max(GetSystemMetrics(SM_CXVIRTUALSCREEN), 1);
            const auto Height = std::max(GetSystemMetrics(SM_CYVIRTUALSCREEN), 1);
            State->PointerX = std::clamp<std::int64_t>(
                static_cast<std::int64_t>(Cursor.x - Left) * 65'535 / Width,
                0, 65'535);
            State->PointerY = std::clamp<std::int64_t>(
                static_cast<std::int64_t>(Cursor.y - Top) * 65'535 / Height,
                0, 65'535);
        }
        {
            std::scoped_lock Lock(State->StartMutex);
            State->StartSucceeded = Success;
            State->StartComplete = true;
        }
        State->Started.notify_all();
        if (!Success) {
            State->Gate.SetRemoteRouting(false);
        } else {
            MSG Message{};
            while (GetMessageW(&Message, nullptr, 0, 0) > 0) {
                if (Message.message == kStopMessage) break;
                if (Message.message == kEmergencyMessage) State->Emergency();
                if (Message.message == kReturnLocalMessage) {
                    State->ReturnLocal();
                }
                if (Message.message == kQueueOverflowMessage) {
                    State->Fail("bounded input queue overflowed");
                }
                if (Message.message == kKeyboardCaptureFailureMessage) {
                    State->Fail(
                        "keyboard hook scan code or capture queue was invalid");
                }
                if (Message.message == kMouseWheelCaptureFailureMessage) {
                    State->Fail(
                        "mouse-wheel delta or capture queue was invalid");
                }
                TranslateMessage(&Message);
                DispatchMessageW(&Message);
            }
        }
        State->Gate.SetRemoteRouting(false);
        if (State->KeyboardHook) UnhookWindowsHookEx(State->KeyboardHook);
        if (State->MouseHook) UnhookWindowsHookEx(State->MouseHook);
        if (State->Window) DestroyWindow(State->Window);
        auto* ExpectedState = State;
        ActiveState.compare_exchange_strong(ExpectedState, nullptr);
    });
    std::unique_lock Lock(State_->StartMutex);
    State_->Started.wait(Lock, [&] { return State_->StartComplete; });
    if (!State_->StartSucceeded) {
        Lock.unlock();
        Stop();
        return false;
    }
    return true;
}

void Win32InputCapture::SetRemoteRouting(bool Enabled) noexcept {
    State_->Gate.SetRemoteRouting(Enabled);
    if (!Enabled) State_->ClearQueue();
}

void Win32InputCapture::SetReturnLocalHotkey(
    ProductHotkey Hotkey) noexcept {
    State_->Gate.SetReturnLocalHotkey(Hotkey);
}

bool Win32InputCapture::RemoteRouting() const noexcept {
    return State_->Gate.RemoteRouting();
}

void Win32InputCapture::Stop() noexcept {
    State_->Gate.SetRemoteRouting(false);
    if (State_->CaptureThread.joinable()) {
        PostThreadMessageW(State_->CaptureThreadId, kStopMessage, 0, 0);
        State_->CaptureThread.join();
    }
    {
        std::scoped_lock Lock(State_->QueueMutex);
        State_->StopWorker = true;
        State_->Queue.clear();
    }
    State_->QueueChanged.notify_all();
    if (State_->WorkerThread.joinable()) State_->WorkerThread.join();
}

} // namespace desklink

#endif
