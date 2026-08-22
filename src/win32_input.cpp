#ifdef _WIN32

#include "desklink/win32_input.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>

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

bool Win32InputInjector::inject_key(const KeyEventMessage& event) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = event.scan_code;
    input.ki.dwFlags = KEYEVENTF_SCANCODE;
    if (event.extended) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (!event.down) input.ki.dwFlags |= KEYEVENTF_KEYUP;

    if (!send_one(input)) return false;

    KeyState state{event.scan_code, event.extended};
    if (event.down) owned_keys_.insert(state);
    else owned_keys_.erase(state);
    return true;
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

    if (event.down) owned_buttons_.insert(event.button);
    else owned_buttons_.erase(event.button);
    return true;
}

bool Win32InputInjector::inject_pointer(const PointerPositionMessage& event) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = static_cast<LONG>(event.normalized_x);
    input.mi.dy = static_cast<LONG>(event.normalized_y);
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    return send_one(input);
}

void Win32InputInjector::release_owned_state() noexcept {
    for (const auto& key : owned_keys_) {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wScan = key.scan_code;
        input.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        if (key.extended) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        (void)send_one(input);
    }
    owned_keys_.clear();

    for (const auto button : owned_buttons_) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = button_flag(button, false);
        if (button == MouseButtonId::X1 || button == MouseButtonId::X2) {
            input.mi.mouseData = xbutton_data(button);
        }
        (void)send_one(input);
    }
    owned_buttons_.clear();
}

} // namespace desklink

#endif
