#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wtsapi32.h>

#include <array>
#include <chrono>
#include <cwchar>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace {

enum class ProbeOperation {
    DefaultRelease,
    SecureCancel,
};

enum class ProbeExitCode : int {
    Success = 0,
    ReleaseFailed = 11,
    NotLocalSystem = 12,
    SessionLookupFailed = 13,
    ConsoleSessionMismatch = 14,
    SessionLockedOrUnknown = 15,
    ThreadDesktopMismatch = 16,
    InputDesktopMismatch = 17,
    ForegroundUnavailable = 20,
    ForegroundProcessUnavailable = 21,
    ForegroundImageUnavailable = 22,
    ForegroundIsNotConsent = 23,
    InputDesktopChanged = 24,
    EscapeScanCodeUnavailable = 25,
    SendInputFailed = 26,
    DesktopTransitionTimedOut = 27,
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

ProbeExitCode ValidateExecutionContext(ProbeOperation Operation) {
    if (!IsLocalSystem()) {
        Log(L"probe refused because helper is not LocalSystem");
        return ProbeExitCode::NotLocalSystem;
    }
    DWORD SessionId{};
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &SessionId)) {
        Log(L"probe refused because the helper session could not be read");
        return ProbeExitCode::SessionLookupFailed;
    }
    if (SessionId != WTSGetActiveConsoleSessionId()) {
        Log(L"probe refused because helper is not in the console session");
        return ProbeExitCode::ConsoleSessionMismatch;
    }
    if (!IsSessionUnlocked(SessionId)) {
        Log(L"probe refused because the console session is locked or unknown");
        return ProbeExitCode::SessionLockedOrUnknown;
    }
    const auto DesktopName = CurrentDesktopName();
    const auto InputDesktopName = ActiveInputDesktopName();
    const wchar_t* Expected = Operation == ProbeOperation::SecureCancel
        ? L"Winlogon" : L"Default";
    if (_wcsicmp(DesktopName.c_str(), Expected) != 0) {
        Log(L"probe refused because the helper thread desktop is unexpected");
        return ProbeExitCode::ThreadDesktopMismatch;
    }
    if (_wcsicmp(InputDesktopName.c_str(), Expected) != 0) {
        Log(L"probe refused because the active input desktop is unexpected");
        return ProbeExitCode::InputDesktopMismatch;
    }
    return ProbeExitCode::Success;
}

INPUT KeyInput(WORD VirtualKey, DWORD Flags) noexcept {
    INPUT Input{};
    Input.type = INPUT_KEYBOARD;
    Input.ki.wVk = VirtualKey;
    Input.ki.dwFlags = Flags;
    return Input;
}

INPUT ScanCodeInput(WORD ScanCode, DWORD Flags) noexcept {
    INPUT Input{};
    Input.type = INPUT_KEYBOARD;
    Input.ki.wScan = ScanCode;
    Input.ki.dwFlags = KEYEVENTF_SCANCODE | Flags;
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

ProbeExitCode CancelSecureDesktopPrompt() noexcept {
    const HWND Foreground = GetForegroundWindow();
    DWORD ProcessId{};
    if (!Foreground ||
        GetWindowThreadProcessId(Foreground, &ProcessId) == 0 ||
        ProcessId == 0) {
        return ProbeExitCode::ForegroundUnavailable;
    }
    HANDLE Process = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ProcessId);
    if (!Process) return ProbeExitCode::ForegroundProcessUnavailable;
    std::array<wchar_t, 32'768> ImagePath{};
    DWORD ImageLength = static_cast<DWORD>(ImagePath.size());
    const BOOL ReadImage = QueryFullProcessImageNameW(
        Process, 0, ImagePath.data(), &ImageLength);
    CloseHandle(Process);
    if (!ReadImage || ImageLength == 0 ||
        ImageLength >= static_cast<DWORD>(ImagePath.size())) {
        return ProbeExitCode::ForegroundImageUnavailable;
    }
    const std::wstring_view Image(ImagePath.data(), ImageLength);
    const auto Separator = Image.find_last_of(L"\\/");
    const auto BaseName = Separator == std::wstring_view::npos
        ? Image : Image.substr(Separator + 1);
    if (_wcsicmp(std::wstring(BaseName).c_str(), L"consent.exe") != 0) {
        return ProbeExitCode::ForegroundIsNotConsent;
    }
    if (_wcsicmp(ActiveInputDesktopName().c_str(), L"Winlogon") != 0) {
        return ProbeExitCode::InputDesktopChanged;
    }

    const auto ScanCode = static_cast<WORD>(
        MapVirtualKeyW(VK_ESCAPE, MAPVK_VK_TO_VSC));
    if (ScanCode == 0) return ProbeExitCode::EscapeScanCodeUnavailable;
    std::array<INPUT, 2> Escape = {
        ScanCodeInput(ScanCode, 0),
        ScanCodeInput(ScanCode, KEYEVENTF_KEYUP),
    };
    const auto Expected = static_cast<UINT>(Escape.size());
    if (SendInput(Expected, Escape.data(), sizeof(INPUT)) != Expected) {
        return ProbeExitCode::SendInputFailed;
    }
    for (std::size_t Attempt = 0; Attempt < 20; ++Attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (_wcsicmp(
                ActiveInputDesktopName().c_str(), L"Default") == 0) {
            return ProbeExitCode::Success;
        }
    }
    return ProbeExitCode::DesktopTransitionTimedOut;
}

int RunProbe(ProbeOperation Operation) {
    const auto ContextResult = ValidateExecutionContext(Operation);
    if (ContextResult != ProbeExitCode::Success) {
        return static_cast<int>(ContextResult);
    }
    if (!ReleaseOwnedInput()) {
        Log(L"release-only probe failed closed");
        return static_cast<int>(ProbeExitCode::ReleaseFailed);
    }
    if (Operation == ProbeOperation::SecureCancel) {
        const auto Result = CancelSecureDesktopPrompt();
        if (Result != ProbeExitCode::Success) {
            Log(L"secure-desktop cancel probe failed closed at stage " +
                std::to_wstring(static_cast<int>(Result)));
            return static_cast<int>(Result);
        }
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
