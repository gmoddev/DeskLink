#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wtsapi32.h>

#include "desklink/protocol.hpp"
#include "desklink/secure_input_wire.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace {

enum class ProbeOperation {
    DefaultRelease,
    DefaultMinimizeElevatedForeground,
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
    ForegroundIsNotTopLevel = 28,
    ForegroundIsNotVisible = 29,
    ForegroundTokenUnavailable = 30,
    ForegroundIsNotElevated = 31,
    ForegroundChanged = 32,
    MinimizeRequestFailed = 33,
    MinimizeTimedOut = 34,
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

ProbeExitCode ValidateSecureConsentForeground() noexcept {
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
    return ProbeExitCode::Success;
}

ProbeExitCode MinimizeElevatedForeground() noexcept {
    const HWND Foreground = GetForegroundWindow();
    DWORD ProcessId{};
    if (!Foreground ||
        GetWindowThreadProcessId(Foreground, &ProcessId) == 0 ||
        ProcessId == 0) {
        return ProbeExitCode::ForegroundUnavailable;
    }
    if (GetAncestor(Foreground, GA_ROOT) != Foreground) {
        return ProbeExitCode::ForegroundIsNotTopLevel;
    }
    if (!IsWindowVisible(Foreground)) {
        return ProbeExitCode::ForegroundIsNotVisible;
    }

    HANDLE Process = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ProcessId);
    if (!Process) return ProbeExitCode::ForegroundProcessUnavailable;
    HANDLE Token{};
    if (!OpenProcessToken(Process, TOKEN_QUERY, &Token)) {
        CloseHandle(Process);
        return ProbeExitCode::ForegroundTokenUnavailable;
    }
    TOKEN_ELEVATION Elevation{};
    DWORD Returned{};
    const BOOL ReadElevation = GetTokenInformation(
        Token, TokenElevation, &Elevation, sizeof(Elevation), &Returned);
    CloseHandle(Token);
    CloseHandle(Process);
    if (!ReadElevation || Returned != sizeof(Elevation)) {
        return ProbeExitCode::ForegroundTokenUnavailable;
    }
    if (!Elevation.TokenIsElevated) {
        return ProbeExitCode::ForegroundIsNotElevated;
    }

    // Do not accept an HWND, PID, image name, or show command from outside
    // this helper. Recheck the exact local foreground immediately before the
    // one permitted cross-integrity operation.
    if (GetForegroundWindow() != Foreground) {
        return ProbeExitCode::ForegroundChanged;
    }
    if (!ShowWindowAsync(Foreground, SW_MINIMIZE)) {
        return ProbeExitCode::MinimizeRequestFailed;
    }
    for (std::size_t Attempt = 0; Attempt < 10; ++Attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (IsIconic(Foreground)) return ProbeExitCode::Success;
    }
    return ProbeExitCode::MinimizeTimedOut;
}

ProbeExitCode CancelSecureDesktopPrompt() noexcept {
    auto Result = ValidateSecureConsentForeground();
    if (Result != ProbeExitCode::Success) return Result;
    if (!ReleaseOwnedInput()) return ProbeExitCode::ReleaseFailed;

    // Recheck after releasing owned state so a foreground transition cannot
    // turn the following Escape into input for another secure surface.
    Result = ValidateSecureConsentForeground();
    if (Result != ProbeExitCode::Success) return Result;

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

std::uint16_t LoadU16(
    const std::array<std::uint8_t, 72>& Payload,
    std::size_t Offset) noexcept {
    return static_cast<std::uint16_t>(Payload[Offset]) |
        static_cast<std::uint16_t>(Payload[Offset + 1] << 8u);
}

std::uint32_t LoadU32(
    const std::array<std::uint8_t, 72>& Payload,
    std::size_t Offset) noexcept {
    std::uint32_t Result{};
    for (std::size_t Index = 0; Index < 4; ++Index) {
        Result |= static_cast<std::uint32_t>(Payload[Offset + Index])
            << (Index * 8u);
    }
    return Result;
}

bool SendSingleInput(INPUT Input) noexcept {
    return SendInput(1, &Input, sizeof(Input)) == 1;
}

struct BrokerInputState {
    std::array<std::uint8_t, 32> Keys{};
    std::array<std::uint8_t, 32> ExtendedKeys{};
    std::uint8_t MouseButtons{};
};

bool BitSet(
    const std::array<std::uint8_t, 32>& Bitmap,
    std::uint16_t ScanCode) noexcept {
    return (Bitmap[ScanCode / 8u] &
        static_cast<std::uint8_t>(1u << (ScanCode % 8u))) != 0;
}

void SetBit(
    std::array<std::uint8_t, 32>& Bitmap,
    std::uint16_t ScanCode, bool Down) noexcept {
    const auto Mask = static_cast<std::uint8_t>(1u << (ScanCode % 8u));
    if (Down) Bitmap[ScanCode / 8u] |= Mask;
    else Bitmap[ScanCode / 8u] &= static_cast<std::uint8_t>(~Mask);
}

std::optional<DWORD> MouseFlags(
    desklink::MouseButtonId Button, bool Down) noexcept {
    switch (Button) {
        case desklink::MouseButtonId::Left:
            return Down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        case desklink::MouseButtonId::Right:
            return Down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        case desklink::MouseButtonId::Middle:
            return Down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        case desklink::MouseButtonId::X1:
        case desklink::MouseButtonId::X2:
            return Down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
    }
    return std::nullopt;
}

std::uint8_t MouseMask(desklink::MouseButtonId Button) noexcept {
    const auto Raw = static_cast<std::uint8_t>(Button);
    return Raw >= 1 && Raw <= 5
        ? static_cast<std::uint8_t>(1u << (Raw - 1u)) : 0;
}

bool ReleaseBrokerInput(BrokerInputState& State) noexcept {
    bool Result = true;
    for (std::uint16_t ScanCode = 1; ScanCode < 256; ++ScanCode) {
        for (const bool Extended : {false, true}) {
            auto& Bitmap = Extended ? State.ExtendedKeys : State.Keys;
            if (!BitSet(Bitmap, ScanCode)) continue;
            Result = SendSingleInput(ScanCodeInput(
                ScanCode, KEYEVENTF_KEYUP |
                    (Extended ? KEYEVENTF_EXTENDEDKEY : 0))) && Result;
            SetBit(Bitmap, ScanCode, false);
        }
    }
    for (std::uint8_t Raw = 1; Raw <= 5; ++Raw) {
        const auto Button = static_cast<desklink::MouseButtonId>(Raw);
        const auto Mask = MouseMask(Button);
        if ((State.MouseButtons & Mask) == 0) continue;
        const auto Flags = MouseFlags(Button, false);
        INPUT Input = MouseInput(*Flags,
            Raw == static_cast<std::uint8_t>(desklink::MouseButtonId::X2)
                ? XBUTTON2 : Raw == static_cast<std::uint8_t>(
                    desklink::MouseButtonId::X1) ? XBUTTON1 : 0);
        Result = SendSingleInput(Input) && Result;
        State.MouseButtons &= static_cast<std::uint8_t>(~Mask);
    }
    return Result;
}

desklink::secure_input_wire::Status ApplyBrokerOperation(
    desklink::secure_input_wire::Operation Operation,
    const std::array<std::uint8_t, 72>& Payload,
    bool SecureDesktop, BrokerInputState& State) noexcept {
    using desklink::secure_input_wire::Status;
    const auto Context = ValidateExecutionContext(
        SecureDesktop ? ProbeOperation::SecureCancel
                      : ProbeOperation::DefaultRelease);
    if (Context != ProbeExitCode::Success) return Status::DesktopUnavailable;
    if (SecureDesktop && Operation !=
            desklink::secure_input_wire::Operation::ReleaseOwnedState &&
        ValidateSecureConsentForeground() != ProbeExitCode::Success) {
        return Status::DesktopUnavailable;
    }
    // DeskLink's privileged development path is limited to pointer-based
    // approval or cancellation of an already visible consent prompt. It must
    // never type authentication secrets into an over-the-shoulder UAC prompt.
    if (SecureDesktop &&
        (Operation == desklink::secure_input_wire::Operation::Key ||
         Operation == desklink::secure_input_wire::Operation::ReconcileState)) {
        return Status::SecureOperationBlocked;
    }

    switch (Operation) {
        case desklink::secure_input_wire::Operation::ReleaseOwnedState:
            return ReleaseBrokerInput(State)
                ? Status::Ok : Status::InjectionFailed;
        case desklink::secure_input_wire::Operation::Key: {
            const auto ScanCode = LoadU16(Payload, 0);
            if (ScanCode == 0 || ScanCode > 255 || Payload[2] > 1 ||
                Payload[3] > 1) {
                return Status::InvalidRequest;
            }
            const bool Extended = Payload[2] == 1;
            const bool Down = Payload[3] == 1;
            auto& Bitmap = Extended ? State.ExtendedKeys : State.Keys;
            INPUT Input = ScanCodeInput(
                ScanCode, (Extended ? KEYEVENTF_EXTENDEDKEY : 0) |
                    (Down ? 0 : KEYEVENTF_KEYUP));
            if (!SendSingleInput(Input)) return Status::InjectionFailed;
            SetBit(Bitmap, ScanCode, Down);
            return Status::Ok;
        }
        case desklink::secure_input_wire::Operation::MouseButton: {
            if (Payload[0] < 1 || Payload[0] > 5 || Payload[1] > 1) {
                return Status::InvalidRequest;
            }
            const auto Button = static_cast<desklink::MouseButtonId>(Payload[0]);
            const bool Down = Payload[1] == 1;
            const auto Flags = MouseFlags(Button, Down);
            if (!Flags) return Status::InvalidRequest;
            const DWORD Data = Button == desklink::MouseButtonId::X1
                ? XBUTTON1 : Button == desklink::MouseButtonId::X2
                    ? XBUTTON2 : 0;
            if (!SendSingleInput(MouseInput(*Flags, Data))) {
                return Status::InjectionFailed;
            }
            const auto Mask = MouseMask(Button);
            if (Down) State.MouseButtons |= Mask;
            else State.MouseButtons &= static_cast<std::uint8_t>(~Mask);
            return Status::Ok;
        }
        case desklink::secure_input_wire::Operation::PointerMotion: {
            const auto DeltaX = static_cast<std::int32_t>(LoadU32(Payload, 0));
            const auto DeltaY = static_cast<std::int32_t>(LoadU32(Payload, 4));
            if (DeltaX < -desklink::kMaximumPointerMotionDelta ||
                DeltaX > desklink::kMaximumPointerMotionDelta ||
                DeltaY < -desklink::kMaximumPointerMotionDelta ||
                DeltaY > desklink::kMaximumPointerMotionDelta) {
                return Status::InvalidRequest;
            }
            INPUT Input = MouseInput(MOUSEEVENTF_MOVE);
            Input.mi.dx = DeltaX;
            Input.mi.dy = DeltaY;
            return SendSingleInput(Input) ? Status::Ok : Status::InjectionFailed;
        }
        case desklink::secure_input_wire::Operation::PointerPosition: {
            INPUT Input = MouseInput(
                MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE |
                MOUSEEVENTF_VIRTUALDESK);
            Input.mi.dx = LoadU16(Payload, 0);
            Input.mi.dy = LoadU16(Payload, 2);
            return SendSingleInput(Input) ? Status::Ok : Status::InjectionFailed;
        }
        case desklink::secure_input_wire::Operation::Wheel: {
            if (Payload[0] < 1 || Payload[0] > 2) {
                return Status::InvalidRequest;
            }
            const auto Delta = static_cast<std::int16_t>(LoadU16(Payload, 1));
            if (Delta == 0 || Delta < -desklink::kMaximumMouseWheelDelta ||
                Delta > desklink::kMaximumMouseWheelDelta) {
                return Status::InvalidRequest;
            }
            return SendSingleInput(MouseInput(
                Payload[0] == static_cast<std::uint8_t>(
                    desklink::MouseWheelAxis::Vertical)
                    ? MOUSEEVENTF_WHEEL : MOUSEEVENTF_HWHEEL,
                static_cast<DWORD>(static_cast<std::int32_t>(Delta))))
                ? Status::Ok : Status::InjectionFailed;
        }
        case desklink::secure_input_wire::Operation::ReconcileState: {
            BrokerInputState Desired;
            std::copy_n(Payload.begin(), 32, Desired.Keys.begin());
            std::copy_n(Payload.begin() + 32, 32,
                Desired.ExtendedKeys.begin());
            Desired.MouseButtons = Payload[64];
            if ((Desired.MouseButtons & ~0x1fu) != 0) {
                return Status::InvalidRequest;
            }
            for (std::uint16_t ScanCode = 1; ScanCode < 256; ++ScanCode) {
                for (const bool Extended : {false, true}) {
                    auto& CurrentBitmap = Extended
                        ? State.ExtendedKeys : State.Keys;
                    const auto& DesiredBitmap = Extended
                        ? Desired.ExtendedKeys : Desired.Keys;
                    const bool Current = BitSet(CurrentBitmap, ScanCode);
                    const bool Wanted = BitSet(DesiredBitmap, ScanCode);
                    if (Current == Wanted) continue;
                    if (!SendSingleInput(ScanCodeInput(
                            ScanCode, (Extended ? KEYEVENTF_EXTENDEDKEY : 0) |
                                (Wanted ? 0 : KEYEVENTF_KEYUP)))) {
                        return Status::InjectionFailed;
                    }
                    SetBit(CurrentBitmap, ScanCode, Wanted);
                }
            }
            for (std::uint8_t Raw = 1; Raw <= 5; ++Raw) {
                const auto Button = static_cast<desklink::MouseButtonId>(Raw);
                const auto Mask = MouseMask(Button);
                const bool Current = (State.MouseButtons & Mask) != 0;
                const bool Wanted = (Desired.MouseButtons & Mask) != 0;
                if (Current == Wanted) continue;
                const auto Flags = MouseFlags(Button, Wanted);
                const DWORD Data = Button == desklink::MouseButtonId::X1
                    ? XBUTTON1 : Button == desklink::MouseButtonId::X2
                        ? XBUTTON2 : 0;
                if (!SendSingleInput(MouseInput(*Flags, Data))) {
                    return Status::InjectionFailed;
                }
                if (Wanted) State.MouseButtons |= Mask;
                else State.MouseButtons &= static_cast<std::uint8_t>(~Mask);
            }
            return Status::Ok;
        }
        default:
            return Status::InvalidRequest;
    }
}

std::optional<HANDLE> ParseInheritedHandle(const wchar_t* Text) noexcept {
    if (!Text || *Text == L'\0') return std::nullopt;
    wchar_t* End{};
    errno = 0;
    const auto Raw = _wcstoui64(Text, &End, 10);
    if (errno != 0 || !End || *End != L'\0' || Raw == 0 ||
        Raw > static_cast<unsigned long long>(
            std::numeric_limits<std::uintptr_t>::max())) {
        return std::nullopt;
    }
    return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(Raw));
}

int RunBroker(
    HANDLE ReadPipe, HANDLE WritePipe, bool SecureDesktop) noexcept {
    if (!IsLocalSystem()) return 12;
    const auto Context = ValidateExecutionContext(
        SecureDesktop ? ProbeOperation::SecureCancel
                      : ProbeOperation::DefaultRelease);
    desklink::secure_input_wire::HelperResponse Ready;
    Ready.Result = Context == ProbeExitCode::Success
        ? desklink::secure_input_wire::Status::Ok
        : desklink::secure_input_wire::Status::DesktopUnavailable;
    DWORD ReadyWritten{};
    if (!WriteFile(
            WritePipe, &Ready, sizeof(Ready), &ReadyWritten, nullptr) ||
        ReadyWritten != sizeof(Ready)) {
        return 1;
    }
    if (Ready.Result != desklink::secure_input_wire::Status::Ok) {
        return static_cast<int>(Context);
    }
    BrokerInputState State;
    for (;;) {
        desklink::secure_input_wire::HelperRequest Request;
        DWORD Read{};
        if (!ReadFile(ReadPipe, &Request, sizeof(Request), &Read, nullptr) ||
            Read != sizeof(Request)) {
            (void)ReleaseBrokerInput(State);
            return 0;
        }
        desklink::secure_input_wire::HelperResponse Response;
        if (Request.Magic != desklink::secure_input_wire::kMagic ||
            Request.Version != desklink::secure_input_wire::kVersion ||
            Request.Size != sizeof(Request) || Request.Reserved != 0) {
            Response.Result =
                desklink::secure_input_wire::Status::InvalidRequest;
        } else {
            Response.Result = ApplyBrokerOperation(
                Request.RequestedOperation, Request.Payload,
                SecureDesktop, State);
        }
        DWORD Written{};
        if (!WriteFile(
                WritePipe, &Response, sizeof(Response), &Written, nullptr) ||
            Written != sizeof(Response)) {
            (void)ReleaseBrokerInput(State);
            return 1;
        }
    }
}

int RunProbe(ProbeOperation Operation) {
    const auto ContextResult = ValidateExecutionContext(Operation);
    if (ContextResult != ProbeExitCode::Success) {
        return static_cast<int>(ContextResult);
    }
    if (Operation == ProbeOperation::DefaultRelease) {
        if (!ReleaseOwnedInput()) {
            Log(L"release-only probe failed closed");
            return static_cast<int>(ProbeExitCode::ReleaseFailed);
        }
    } else if (Operation ==
            ProbeOperation::DefaultMinimizeElevatedForeground) {
        const auto Result = MinimizeElevatedForeground();
        if (Result != ProbeExitCode::Success) {
            Log(L"elevated-foreground minimize probe failed closed at stage " +
                std::to_wstring(static_cast<int>(Result)));
            return static_cast<int>(Result);
        }
    } else {
        const auto Result = CancelSecureDesktopPrompt();
        if (Result != ProbeExitCode::Success) {
            Log(L"secure-desktop cancel probe failed closed at stage " +
                std::to_wstring(static_cast<int>(Result)));
            return static_cast<int>(Result);
        }
    }
    if (Operation == ProbeOperation::SecureCancel) {
        Log(L"secure-desktop cancel probe completed");
    } else if (Operation ==
            ProbeOperation::DefaultMinimizeElevatedForeground) {
        Log(L"elevated foreground was minimized");
    } else {
        Log(L"Default-desktop release probe completed");
    }
    return 0;
}

} // namespace

int wmain(int ArgumentCount, wchar_t** Arguments) {
    if (ArgumentCount == 2 &&
        std::wstring_view(Arguments[1]) == L"--self-test") {
        Log(L"self-test passed; no input was generated");
        return 0;
    }
    if (ArgumentCount == 5 &&
        std::wstring_view(Arguments[1]) == L"--broker") {
        const auto ReadPipe = ParseInheritedHandle(Arguments[2]);
        const auto WritePipe = ParseInheritedHandle(Arguments[3]);
        const std::wstring_view Desktop(Arguments[4]);
        if (!ReadPipe || !WritePipe ||
            (Desktop != L"default" && Desktop != L"winlogon")) {
            return 2;
        }
        return RunBroker(*ReadPipe, *WritePipe, Desktop == L"winlogon");
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
    if (Operation == L"default-minimize-elevated-foreground") {
        return RunProbe(
            ProbeOperation::DefaultMinimizeElevatedForeground);
    }
    if (Operation == L"secure-cancel") {
        return RunProbe(ProbeOperation::SecureCancel);
    }
    std::wcerr << L"[SecureInput:Helper] unsupported operation\n";
    return 2;
}
