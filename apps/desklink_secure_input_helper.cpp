#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wtsapi32.h>

#include <array>
#include <cwchar>
#include <iostream>
#include <string>
#include <string_view>

namespace {

enum class ProbeOperation {
    DefaultRelease,
    SecureCancel,
};

void Log(std::wstring_view Message) {
    std::wstring Line = L"[SecureInput:Helper] ";
    Line.append(Message);
    Line.push_back(L'\n');
    OutputDebugStringW(Line.c_str());
    std::wcout << Line;
}

bool IsLocalSystem() noexcept {
    SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
    PSID LocalSystemSid{};
    if (!AllocateAndInitializeSid(&NtAuthority, 1,
            SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0,
            &LocalSystemSid)) {
        return false;
    }
    BOOL Member = FALSE;
    const bool Result = CheckTokenMembership(
        nullptr, LocalSystemSid, &Member) && Member;
    FreeSid(LocalSystemSid);
    return Result;
}

std::wstring CurrentDesktopName() {
    const HDESK Desktop = GetThreadDesktop(GetCurrentThreadId());
    if (!Desktop) return {};
    DWORD Required{};
    GetUserObjectInformationW(Desktop, UOI_NAME, nullptr, 0, &Required);
    if (Required < sizeof(wchar_t) || Required > 1024) return {};
    std::wstring Name(Required / sizeof(wchar_t), L'\0');
    if (!GetUserObjectInformationW(
            Desktop, UOI_NAME, Name.data(), Required, &Required)) {
        return {};
    }
    Name.resize(std::wcslen(Name.c_str()));
    return Name;
}

std::wstring ActiveInputDesktopName() {
    const HDESK Desktop = OpenInputDesktop(
        0, FALSE, DESKTOP_READOBJECTS | DESKTOP_SWITCHDESKTOP);
    if (!Desktop) return {};
    DWORD Required{};
    GetUserObjectInformationW(Desktop, UOI_NAME, nullptr, 0, &Required);
    if (Required < sizeof(wchar_t) || Required > 1024) {
        CloseDesktop(Desktop);
        return {};
    }
    std::wstring Name(Required / sizeof(wchar_t), L'\0');
    const BOOL Read = GetUserObjectInformationW(
        Desktop, UOI_NAME, Name.data(), Required, &Required);
    CloseDesktop(Desktop);
    if (!Read) return {};
    Name.resize(std::wcslen(Name.c_str()));
    return Name;
}

bool IsSessionUnlocked(DWORD SessionId) noexcept {
    LPWSTR Buffer{};
    DWORD Bytes{};
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, SessionId,
            WTSSessionInfoEx, &Buffer, &Bytes) || !Buffer ||
        Bytes < sizeof(WTSINFOEXW)) {
        if (Buffer) WTSFreeMemory(Buffer);
        return false;
    }
    const auto* Info = reinterpret_cast<const WTSINFOEXW*>(Buffer);
    const bool Unlocked = Info->Level == 1 &&
        Info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_UNLOCK;
    WTSFreeMemory(Buffer);
    return Unlocked;
}

bool ValidateExecutionContext(ProbeOperation Operation) {
    if (!IsLocalSystem()) {
        Log(L"probe refused because helper is not LocalSystem");
        return false;
    }
    DWORD SessionId{};
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &SessionId) ||
        SessionId != WTSGetActiveConsoleSessionId() ||
        !IsSessionUnlocked(SessionId)) {
        Log(L"probe refused because helper is not in the active console session");
        return false;
    }
    const auto DesktopName = CurrentDesktopName();
    const auto InputDesktopName = ActiveInputDesktopName();
    const wchar_t* Expected = Operation == ProbeOperation::SecureCancel
        ? L"Winlogon" : L"Default";
    if (_wcsicmp(DesktopName.c_str(), Expected) != 0 ||
        _wcsicmp(InputDesktopName.c_str(), Expected) != 0) {
        Log(L"probe refused because helper is not on the active input desktop");
        return false;
    }
    return true;
}

INPUT KeyInput(WORD VirtualKey, DWORD Flags) noexcept {
    INPUT Input{};
    Input.type = INPUT_KEYBOARD;
    Input.ki.wVk = VirtualKey;
    Input.ki.dwFlags = Flags;
    return Input;
}

INPUT MouseInput(DWORD Flags, DWORD Data = 0) noexcept {
    INPUT Input{};
    Input.type = INPUT_MOUSE;
    Input.mi.dwFlags = Flags;
    Input.mi.mouseData = Data;
    return Input;
}

bool ReleaseOwnedInput() noexcept {
    std::array<INPUT, 11> Releases = {
        KeyInput(VK_LCONTROL, KEYEVENTF_KEYUP),
        KeyInput(VK_RCONTROL, KEYEVENTF_KEYUP),
        KeyInput(VK_LSHIFT, KEYEVENTF_KEYUP),
        KeyInput(VK_RSHIFT, KEYEVENTF_KEYUP),
        KeyInput(VK_LMENU, KEYEVENTF_KEYUP),
        KeyInput(VK_RMENU, KEYEVENTF_KEYUP),
        KeyInput(VK_LWIN, KEYEVENTF_KEYUP),
        KeyInput(VK_RWIN, KEYEVENTF_KEYUP),
        MouseInput(MOUSEEVENTF_LEFTUP),
        MouseInput(MOUSEEVENTF_RIGHTUP),
        MouseInput(MOUSEEVENTF_MIDDLEUP),
    };
    const auto Expected = static_cast<UINT>(Releases.size());
    return SendInput(Expected, Releases.data(), sizeof(INPUT)) == Expected;
}

bool CancelSecureDesktopPrompt() noexcept {
    std::array<INPUT, 2> Escape = {
        KeyInput(VK_ESCAPE, 0),
        KeyInput(VK_ESCAPE, KEYEVENTF_KEYUP),
    };
    const auto Expected = static_cast<UINT>(Escape.size());
    return SendInput(Expected, Escape.data(), sizeof(INPUT)) == Expected;
}

int RunProbe(ProbeOperation Operation) {
    if (!ValidateExecutionContext(Operation)) return 1;
    if (!ReleaseOwnedInput()) {
        Log(L"release-only probe failed closed");
        return 1;
    }
    if (Operation == ProbeOperation::SecureCancel &&
        !CancelSecureDesktopPrompt()) {
        Log(L"secure-desktop cancel probe failed closed");
        return 1;
    }
    Log(Operation == ProbeOperation::SecureCancel
        ? L"secure-desktop cancel probe completed"
        : L"Default-desktop release probe completed");
    return 0;
}

} // namespace

int wmain(int ArgumentCount, wchar_t** Arguments) {
    if (ArgumentCount == 2 &&
        std::wstring_view(Arguments[1]) == L"--self-test") {
        Log(L"self-test passed; no input was generated");
        return 0;
    }
    if (ArgumentCount != 3 ||
        std::wstring_view(Arguments[1]) != L"--service-probe") {
        std::wcerr << L"[SecureInput:Helper] service-only invocation required\n";
        return 2;
    }
    const std::wstring_view Operation(Arguments[2]);
    if (Operation == L"default-release") {
        return RunProbe(ProbeOperation::DefaultRelease);
    }
    if (Operation == L"secure-cancel") {
        return RunProbe(ProbeOperation::SecureCancel);
    }
    std::wcerr << L"[SecureInput:Helper] unsupported operation\n";
    return 2;
}
