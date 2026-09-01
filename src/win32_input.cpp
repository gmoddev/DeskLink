#ifdef _WIN32

#include "desklink/win32_input.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <type_traits>

namespace desklink {
namespace {

bool send_one(INPUT input) {
    return SendInput(1, &input, static_cast<int>(sizeof(INPUT))) == 1;
}

DWORD button_flag(MouseButtonId button, bool down) {
    switch (button) {
        case MouseButtonId::Left: return down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        case MouseButtonId::Right: return down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        case MouseButtonId::Middle: return down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        case MouseButtonId::X1:
        case MouseButtonId::X2: return down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        default: return 0;
    }
}

DWORD xbutton_data(MouseButtonId button) {
    return button == MouseButtonId::X2 ? XBUTTON2 : XBUTTON1;
}

} // namespace

Win32InputInjector::Win32InputInjector() {
    (void)DisplayTopology_.Refresh();
}

bool Win32InputInjector::inject_key(const KeyEventMessage& event) {
    if (event.scan_code == 0 || event.scan_code > 255) return false;
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = event.scan_code;
    input.ki.dwFlags = KEYEVENTF_SCANCODE;
    if (event.extended) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (!event.down) input.ki.dwFlags |= KEYEVENTF_KEYUP;

    if (!send_one(input)) return false;

    return SetInputSnapshotKey(OwnedState_, event.scan_code, event.extended, event.down);
}

bool Win32InputInjector::inject_button(const MouseButtonMessage& event) {
    const DWORD flag = button_flag(event.button, event.down);
    if (flag == 0) return false;

    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flag;
    if (event.button == MouseButtonId::X1 || event.button == MouseButtonId::X2) {
        input.mi.mouseData = xbutton_data(event.button);
    }
    if (!send_one(input)) return false;

    return SetInputSnapshotButton(OwnedState_, event.button, event.down);
}

bool Win32InputInjector::inject_pointer(const PointerPositionMessage& event) {
    auto NormalizedX = event.normalized_x;
    auto NormalizedY = event.normalized_y;
    if (event.display_id != kLegacyVirtualDesktopDisplayId) {
        if (!DisplayTopology_.RefreshIfDue()) return false;
        if (!DisplayGeneration_) {
            const auto Generation = DisplayTopology_.Current().Generation;
            if (Generation == 0) return false;
            DisplayGeneration_ = Generation;
        }
        const auto Mapped = DisplayTopology_.MapToVirtualDesktop(
            event.display_id, *DisplayGeneration_, event.normalized_x, event.normalized_y);
        if (!Mapped) return false;
        NormalizedX = Mapped->X;
        NormalizedY = Mapped->Y;
    }

    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = static_cast<LONG>(NormalizedX);
    input.mi.dy = static_cast<LONG>(NormalizedY);
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    return send_one(input);
}

bool Win32InputInjector::InjectPointerMotion(
    const PointerMotionMessage& Message) {
    if (!IsValidPointerMotionMessage(Message)) return false;
    INPUT Input{};
    Input.type = INPUT_MOUSE;
    Input.mi.dx = static_cast<LONG>(Message.DeltaX);
    Input.mi.dy = static_cast<LONG>(Message.DeltaY);
    Input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_MOVE_NOCOALESCE;
    return send_one(Input);
}

bool Win32InputInjector::InjectWheel(const MouseWheelMessage& Message) {
    if (!IsValidMouseWheelMessage(Message)) return false;

    INPUT Input{};
    Input.type = INPUT_MOUSE;
    Input.mi.mouseData = static_cast<DWORD>(static_cast<LONG>(Message.Delta));
    Input.mi.dwFlags = Message.Axis == MouseWheelAxis::Vertical
        ? MOUSEEVENTF_WHEEL : MOUSEEVENTF_HWHEEL;
    return send_one(Input);
}

std::optional<PointerPositionMessage>
Win32InputInjector::CurrentPointerPosition() {
    if (!DisplayTopology_.RefreshIfDue()) return std::nullopt;
    POINT Cursor{};
    if (!GetCursorPos(&Cursor)) return std::nullopt;
    for (const auto& Display : DisplayTopology_.Current().Displays) {
        if (Cursor.x < Display.Bounds.Left ||
            Cursor.x >= Display.Bounds.Right ||
            Cursor.y < Display.Bounds.Top ||
            Cursor.y >= Display.Bounds.Bottom) {
            continue;
        }
        const auto Width = Display.Bounds.Right - Display.Bounds.Left;
        const auto Height = Display.Bounds.Bottom - Display.Bounds.Top;
        if (Width <= 1 || Height <= 1) return std::nullopt;
        const auto Normalize = [](std::int32_t Value,
                                  std::int32_t Length) {
            return static_cast<std::uint16_t>(
                static_cast<std::int64_t>(Value) * 65'535 /
                (Length - 1));
        };
        return PointerPositionMessage{
            Display.Id,
            Normalize(Cursor.x - Display.Bounds.Left, Width),
            Normalize(Cursor.y - Display.Bounds.Top, Height)};
    }
    return std::nullopt;
}

bool Win32InputInjector::RefreshDisplayTopology() {
    return DisplayTopology_.Refresh();
}

const DisplayTopologySnapshot& Win32InputInjector::CurrentDisplayTopology() const noexcept {
    return DisplayTopology_.Current();
}

bool Win32InputInjector::ReconcileState(const InputStateSnapshotMessage& Snapshot) {
    try {
        for (const auto& Transition : BuildInputStateTransitions(OwnedState_, Snapshot)) {
            const bool Applied = std::visit([this](const auto& Event) {
                using EventType = std::decay_t<decltype(Event)>;
                if constexpr (std::is_same_v<EventType, KeyEventMessage>) {
                    return inject_key(Event);
                } else {
                    return inject_button(Event);
                }
            }, Transition);
            if (!Applied) return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool Win32InputInjector::release_owned_state() noexcept {
    bool ReleasedAll = true;
    for (std::uint16_t ScanCode = 1; ScanCode <= 255; ++ScanCode) {
        for (const bool Extended : {false, true}) {
            if (!InputSnapshotKeyDown(OwnedState_, ScanCode, Extended)) continue;
            INPUT Input{};
            Input.type = INPUT_KEYBOARD;
            Input.ki.wScan = ScanCode;
            Input.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
            if (Extended) Input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
            if (send_one(Input)) {
                (void)SetInputSnapshotKey(
                    OwnedState_, ScanCode, Extended, false);
            } else {
                ReleasedAll = false;
            }
        }
    }

    for (std::uint8_t Raw = static_cast<std::uint8_t>(MouseButtonId::Left);
         Raw <= static_cast<std::uint8_t>(MouseButtonId::X2); ++Raw) {
        const auto Button = static_cast<MouseButtonId>(Raw);
        if (!InputSnapshotButtonDown(OwnedState_, Button)) continue;
        INPUT Input{};
        Input.type = INPUT_MOUSE;
        Input.mi.dwFlags = button_flag(Button, false);
        if (Button == MouseButtonId::X1 || Button == MouseButtonId::X2) {
            Input.mi.mouseData = xbutton_data(Button);
        }
        if (send_one(Input)) {
            (void)SetInputSnapshotButton(OwnedState_, Button, false);
        } else {
            ReleasedAll = false;
        }
    }
    DisplayGeneration_.reset();
    return ReleasedAll;
}

} // namespace desklink

#endif
