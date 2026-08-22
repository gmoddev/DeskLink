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
constexpr std::size_t kMaximumQueuedEvents = 1024;
constexpr wchar_t kCaptureWindowClass[] = L"DeskLink.RawInputCapture.v1";

using CapturedEvent = std::variant<
    KeyEventMessage, MouseButtonMessage, PointerPositionMessage>;

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
    explicit State(Win32CaptureHandlers OwnedHandlers)
        : Handlers(std::move(OwnedHandlers)) {}

    Win32CaptureHandlers Handlers;
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

    bool Enqueue(CapturedEvent Event) {
        std::scoped_lock Lock(QueueMutex);
        if (std::holds_alternative<PointerPositionMessage>(Event) && !Queue.empty() &&
            std::holds_alternative<PointerPositionMessage>(Queue.back())) {
            Queue.back() = std::move(Event);
            QueueChanged.notify_one();
            return true;
        }
        if (Queue.size() >= kMaximumQueuedEvents) return false;
        Queue.push_back(std::move(Event));
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
        if (!Gate.RemoteRouting()) return;
        const auto* Input = reinterpret_cast<const RAWINPUT*>(Storage.data());
        if (Input->header.dwType == RIM_TYPEKEYBOARD && Read >= sizeof(RAWINPUT)) {
            const auto& Keyboard = Input->data.keyboard;
            if (Keyboard.VKey == 255u || Keyboard.MakeCode == 0u) return;
            const bool Extended =
                (Keyboard.Flags & (RI_KEY_E0 | RI_KEY_E1)) != 0;
            const bool Down = (Keyboard.Flags & RI_KEY_BREAK) == 0;
            if (!Enqueue(KeyEventMessage{
                    Keyboard.MakeCode, Extended, Down})) {
                Gate.SetRemoteRouting(false);
                PostThreadMessageW(CaptureThreadId, kQueueOverflowMessage, 0, 0);
            }
            return;
        }
        if (Input->header.dwType != RIM_TYPEMOUSE || Read < sizeof(RAWINPUT)) return;
        const auto& Mouse = Input->data.mouse;
        const std::array Buttons{
            GetButton(Mouse, RI_MOUSE_LEFT_BUTTON_DOWN, MouseButtonId::Left, true),
            GetButton(Mouse, RI_MOUSE_LEFT_BUTTON_UP, MouseButtonId::Left, false),
            GetButton(Mouse, RI_MOUSE_RIGHT_BUTTON_DOWN, MouseButtonId::Right, true),
            GetButton(Mouse, RI_MOUSE_RIGHT_BUTTON_UP, MouseButtonId::Right, false),
            GetButton(Mouse, RI_MOUSE_MIDDLE_BUTTON_DOWN, MouseButtonId::Middle, true),
            GetButton(Mouse, RI_MOUSE_MIDDLE_BUTTON_UP, MouseButtonId::Middle, false),
            GetButton(Mouse, RI_MOUSE_BUTTON_4_DOWN, MouseButtonId::X1, true),
            GetButton(Mouse, RI_MOUSE_BUTTON_4_UP, MouseButtonId::X1, false),
            GetButton(Mouse, RI_MOUSE_BUTTON_5_DOWN, MouseButtonId::X2, true),
            GetButton(Mouse, RI_MOUSE_BUTTON_5_UP, MouseButtonId::X2, false)};
        for (const auto& Button : Buttons) {
            if (Button && !Enqueue(*Button)) {
                Gate.SetRemoteRouting(false);
                PostThreadMessageW(CaptureThreadId, kQueueOverflowMessage, 0, 0);
                return;
            }
        }

        if ((Mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0) {
            PointerX = std::clamp<std::int64_t>(Mouse.lLastX, 0, 65'535);
            PointerY = std::clamp<std::int64_t>(Mouse.lLastY, 0, 65'535);
        } else if (Mouse.lLastX != 0 || Mouse.lLastY != 0) {
            const auto Width = std::max(GetSystemMetrics(SM_CXVIRTUALSCREEN), 1);
            const auto Height = std::max(GetSystemMetrics(SM_CYVIRTUALSCREEN), 1);
            PointerX = std::clamp<std::int64_t>(
                PointerX + static_cast<std::int64_t>(Mouse.lLastX) * 65'535 / Width,
                0, 65'535);
            PointerY = std::clamp<std::int64_t>(
                PointerY + static_cast<std::int64_t>(Mouse.lLastY) * 65'535 / Height,
                0, 65'535);
        } else {
            return;
        }
        if (!Enqueue(PointerPositionMessage{
                0, static_cast<std::uint16_t>(PointerX),
                static_cast<std::uint16_t>(PointerY)})) {
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
    if (Decision == Win32HookDecision::Suppress) return 1;
    return CallNextHookEx(nullptr, Code, Message, Data);
}

LRESULT CALLBACK MouseHook(int Code, WPARAM Message, LPARAM Data) {
    auto* State = ActiveState.load(std::memory_order_acquire);
    if (Code != HC_ACTION || !State) {
        return CallNextHookEx(nullptr, Code, Message, Data);
    }
    const auto* Event = reinterpret_cast<const MSLLHOOKSTRUCT*>(Data);
    if (Message == WM_MOUSEWHEEL || Message == WM_MOUSEHWHEEL) {
        return CallNextHookEx(nullptr, Code, Message, Data);
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
    }
}

bool Win32SuppressionGate::RemoteRouting() const noexcept {
    return RemoteRouting_.load(std::memory_order_acquire);
}

Win32HookDecision Win32SuppressionGate::HandleKeyboard(
    std::uint32_t VirtualKey, bool Down, bool Injected) noexcept {
    if (Injected) return Win32HookDecision::Pass;
    UpdateModifier(ControlMask_, ModifierBit(VirtualKey, true), Down);
    UpdateModifier(AltMask_, ModifierBit(VirtualKey, false), Down);
    if (Down && VirtualKey == VK_PAUSE && RemoteRouting() &&
        ControlMask_.load(std::memory_order_relaxed) != 0 &&
        AltMask_.load(std::memory_order_relaxed) != 0) {
        SetRemoteRouting(false);
        return Win32HookDecision::Emergency;
    }
    return RemoteRouting()
        ? Win32HookDecision::Suppress : Win32HookDecision::Pass;
}

Win32HookDecision Win32SuppressionGate::HandleMouse(bool Injected) const noexcept {
    return !Injected && RemoteRouting()
        ? Win32HookDecision::Suppress : Win32HookDecision::Pass;
}

Win32InputCapture::Win32InputCapture(Win32CaptureHandlers Handlers)
    : State_(std::make_unique<State>(std::move(Handlers))) {}

Win32InputCapture::~Win32InputCapture() { Stop(); }

bool Win32InputCapture::Start() {
    if (State_->CaptureThread.joinable()) return State_->StartSucceeded;
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
                               State->Window},
                RAWINPUTDEVICE{0x01, 0x06, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY,
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
                if (Message.message == kQueueOverflowMessage) {
                    State->Fail("bounded input queue overflowed");
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
