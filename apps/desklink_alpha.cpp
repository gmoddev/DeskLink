#include "desklink/control.hpp"
#include "desklink/pairing.hpp"
#include "desklink/profile.hpp"
#include "desklink/win32_application_settings.hpp"
#include "desklink/win32_control.hpp"
#include "desklink/win32_device_certificate.hpp"
#include "desklink/win32_display_topology.hpp"
#include "desklink/win32_launcher.hpp"
#include "desklink/win32_monitor_configurator.hpp"
#include "desklink/win32_pairing.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kWindowClass[] = L"DeskLinkAlphaWindow.v1";
constexpr UINT kStatusTimer = 1;
constexpr UINT kProcessLogMessage = WM_APP + 1;
constexpr UINT kProcessExitedMessage = WM_APP + 2;
constexpr UINT kTrayMessage = WM_APP + 3;
constexpr UINT kActivateMessage = WM_APP + 4;
constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayOpen = 5'001;
constexpr UINT kTrayReturnLocal = 5'002;
constexpr UINT kTrayExit = 5'003;
constexpr std::size_t kMaximumLogCharacters = 200'000;
constexpr std::size_t kRetainedLogCharacters = 120'000;
constexpr wchar_t kDeviceKeyName[] = L"DeskLink-Device-Identity-v1";

enum ControlId : int {
    StatusText = 100,
    RefreshStatus,
    ShowIdentity,
    ArrangeMonitors,
    PeerAddress,
    PeerPort,
    DiscoverPeers,
    GrantInput,
    GrantAudioSend,
    GrantAudioReceive,
    GrantTopology,
    GrantClipboardRead,
    GrantClipboardWrite,
    OpenPairing,
    PairPeer,
    CaptureInput,
    EnableEdgeRoaming,
    PointerGain,
    PointerDpi,
    AudioGain,
    ApplyAudioGain,
    ToggleAudioMute,
    SendAudio,
    ReceiveAudio,
    SyncClipboard,
    StartReceiver,
    StartController,
    FocusRemote,
    ReturnLocalButton,
    StopProcess,
    DiagnosticLog,
    ClearLog,
    KeepRunningOnClose,
    StartWithWindows,
};

struct HandleCloser {
    void operator()(void* Value) const noexcept {
        const auto Handle = static_cast<HANDLE>(Value);
        if (Handle && Handle != INVALID_HANDLE_VALUE) CloseHandle(Handle);
    }
};

using UniqueHandle = std::unique_ptr<void, HandleCloser>;

UniqueHandle TakeHandle(HANDLE Handle) {
    return UniqueHandle(Handle == INVALID_HANDLE_VALUE ? nullptr : Handle);
}

std::optional<std::wstring> GetExecutablePath() {
    std::wstring Buffer(512, L'\0');
    for (;;) {
        const auto Length = GetModuleFileNameW(
            nullptr, Buffer.data(), static_cast<DWORD>(Buffer.size()));
        if (Length == 0) return std::nullopt;
        if (Length < Buffer.size() - 1) {
            Buffer.resize(Length);
            return Buffer;
        }
        if (Buffer.size() >= 32'768) return std::nullopt;
        Buffer.resize(Buffer.size() * 2);
    }
}

std::optional<std::filesystem::path> GetDataDirectory() {
    PWSTR RawPath{};
    if (FAILED(SHGetKnownFolderPath(
            FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &RawPath)) ||
        !RawPath) {
        return std::nullopt;
    }
    std::filesystem::path Result(RawPath);
    CoTaskMemFree(RawPath);
    Result /= L"DeskLink";
    std::error_code Error;
    std::filesystem::create_directories(Result, Error);
    return Error ? std::nullopt : std::optional(Result);
}

std::wstring DecodeProcessOutput(std::span<const char> Bytes) {
    if (Bytes.empty()) return {};
    const auto Size = static_cast<int>(std::min<std::size_t>(
        Bytes.size(), static_cast<std::size_t>(INT_MAX)));
    int Length = MultiByteToWideChar(
        CP_UTF8, 0, Bytes.data(), Size, nullptr, 0);
    UINT CodePage = CP_UTF8;
    if (Length <= 0) {
        CodePage = CP_ACP;
        Length = MultiByteToWideChar(CodePage, 0, Bytes.data(), Size, nullptr, 0);
    }
    if (Length <= 0) return L"[Wrapper:Diagnostics] output decode failed\r\n";
    std::wstring Result(static_cast<std::size_t>(Length), L'\0');
    (void)MultiByteToWideChar(
        CodePage, 0, Bytes.data(), Size, Result.data(), Length);
    std::wstring Normalized;
    Normalized.reserve(Result.size() + 8);
    wchar_t Previous{};
    for (const auto Character : Result) {
        if (Character == L'\n' && Previous != L'\r') Normalized.push_back(L'\r');
        Normalized.push_back(Character);
        Previous = Character;
    }
    return Normalized;
}

std::wstring FormatMachine(const desklink::MachineId& Machine) {
    constexpr wchar_t Digits[] = L"0123456789abcdef";
    std::wstring Result;
    Result.reserve(23);
    for (std::size_t Index = 0; Index < 8; ++Index) {
        if (Index != 0 && Index % 2 == 0) Result.push_back(L':');
        Result.push_back(Digits[(Machine[Index] >> 4u) & 0x0fu]);
        Result.push_back(Digits[Machine[Index] & 0x0fu]);
    }
    return Result;
}

std::wstring_view RoleName(desklink::ControlRole Role) noexcept {
    switch (Role) {
        case desklink::ControlRole::Idle: return L"Idle";
        case desklink::ControlRole::Agent: return L"Receiver";
        case desklink::ControlRole::Host: return L"Controller";
    }
    return L"Unknown";
}

std::wstring_view ModeName(desklink::DeskMode Mode) noexcept {
    switch (Mode) {
        case desklink::DeskMode::Roam: return L"Roam";
        case desklink::DeskMode::LockPc1: return L"Local";
        case desklink::DeskMode::LockPc2: return L"Remote locked";
        case desklink::DeskMode::Game: return L"Game / local";
    }
    return L"Unknown";
}

class AlphaWindow final {
public:
    explicit AlphaWindow(HINSTANCE Instance) : Instance_(Instance) {}
    ~AlphaWindow() {
        RemoveTrayIcon();
        ShutDownProcess();
    }

    bool Create(int ShowCommand, bool StartInBackground) {
        const auto DataDirectory = GetDataDirectory();
        if (!DataDirectory) return false;
        RoamingSettingsPath_ = *DataDirectory / L"roaming.settings";
        ApplicationSettingsStore_ =
            std::make_unique<desklink::Win32ApplicationSettingsStore>(
                *DataDirectory / L"application.settings");
        if (!ApplicationSettingsStore_->Load()) return false;
        ApplicationSettings_ = ApplicationSettingsStore_->Current().value_or(
            desklink::Win32ApplicationSettings{});

        WNDCLASSEXW Class{};
        Class.cbSize = sizeof(Class);
        Class.hInstance = Instance_;
        Class.lpfnWndProc = WindowProcedure;
        Class.lpszClassName = kWindowClass;
        Class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        Class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        Class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        if (!RegisterClassExW(&Class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }

        Window_ = CreateWindowExW(
            0, kWindowClass, L"DeskLink Alpha", WS_OVERLAPPED | WS_CAPTION |
            WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
            940, 890, nullptr, nullptr, Instance_, this);
        if (!Window_) return false;
        if (!AddTrayIcon()) {
            DestroyWindow(Window_);
            return false;
        }
        const bool FirstRun = !ApplicationSettings_.FirstRunComplete;
        ShowWindow(
            Window_, StartInBackground && !FirstRun ? SW_HIDE : ShowCommand);
        if (IsWindowVisible(Window_)) UpdateWindow(Window_);
        if (FirstRun) {
            MessageBoxW(
                Window_,
                L"Welcome to DeskLink.\n\n1. Pair this PC with a nearby PC.\n2. Start a receiver or controller session.\n3. Open Arrange monitors to identify displays and create explicit edge connections.\n\nDeskLink always starts with input Local.",
                L"DeskLink first run", MB_OK | MB_ICONINFORMATION);
            ApplicationSettings_.FirstRunComplete = true;
            (void)ApplicationSettingsStore_->Save(ApplicationSettings_);
        }
        return true;
    }

private:
    static LRESULT CALLBACK WindowProcedure(HWND Window, UINT Message,
                                             WPARAM WParam, LPARAM LParam) {
        AlphaWindow* Self = reinterpret_cast<AlphaWindow*>(
            GetWindowLongPtrW(Window, GWLP_USERDATA));
        if (Message == WM_NCCREATE) {
            const auto* Create = reinterpret_cast<const CREATESTRUCTW*>(LParam);
            Self = static_cast<AlphaWindow*>(Create->lpCreateParams);
            Self->Window_ = Window;
            SetWindowLongPtrW(Window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(Self));
        }
        return Self ? Self->HandleMessage(Message, WParam, LParam)
                    : DefWindowProcW(Window, Message, WParam, LParam);
    }

    LRESULT HandleMessage(UINT Message, WPARAM WParam, LPARAM LParam) {
        switch (Message) {
            case WM_CREATE:
                return CreateControls() ? 0 : -1;
            case WM_COMMAND:
                HandleCommand(LOWORD(WParam));
                return 0;
            case WM_TIMER:
                if (WParam == kStatusTimer) RefreshRuntimeStatus();
                return 0;
            case kProcessLogMessage: {
                std::unique_ptr<std::wstring> Text(
                    reinterpret_cast<std::wstring*>(LParam));
                if (Text) AppendLog(*Text);
                return 0;
            }
            case kProcessExitedMessage:
                FinishProcess(static_cast<DWORD>(WParam));
                return 0;
            case kTrayMessage:
                HandleTrayMessage(static_cast<UINT>(LParam));
                return 0;
            case kActivateMessage:
                RestoreWindow();
                return 0;
            case WM_QUERYENDSESSION:
                return TRUE;
            case WM_ENDSESSION:
                if (WParam != FALSE) {
                    ExitRequested_ = true;
                    ReturnLocal(false);
                    StopProcessGracefully();
                    DestroyWindow(Window_);
                }
                return 0;
            case WM_CLOSE:
                if (!ExitRequested_ && ApplicationSettings_.CloseToTray) {
                    ShowWindow(Window_, SW_HIDE);
                    AppendLog(
                        L"[App:Lifecycle] window hidden; DeskLink remains active\r\n");
                    return 0;
                }
                if (ActiveProcess_ && MessageBoxW(
                        Window_,
                        L"DeskLink will return input local and stop the active operation. Continue?",
                        L"DeskLink Alpha", MB_YESNO | MB_ICONWARNING |
                        MB_DEFBUTTON2) != IDYES) {
                    return 0;
                }
                ReturnLocal(false);
                StopProcessGracefully();
                DestroyWindow(Window_);
                return 0;
            case WM_DESTROY:
                KillTimer(Window_, kStatusTimer);
                RemoveTrayIcon();
                ShutDownProcess();
                if (TitleFont_) DeleteObject(TitleFont_);
                if (Font_) DeleteObject(Font_);
                TitleFont_ = nullptr;
                Font_ = nullptr;
                Window_ = nullptr;
                PostQuitMessage(0);
                return 0;
            default:
                return DefWindowProcW(Window_, Message, WParam, LParam);
        }
    }

    HWND CreateControl(const wchar_t* ClassName, const wchar_t* Text,
                       DWORD Style, int X, int Y, int Width, int Height,
                       ControlId Id = static_cast<ControlId>(0),
                       DWORD ExtendedStyle = 0) {
        const auto Control = CreateWindowExW(
            ExtendedStyle, ClassName, Text, WS_CHILD | WS_VISIBLE | Style,
            X, Y, Width, Height, Window_,
            Id == 0 ? nullptr : reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(Id)), Instance_, nullptr);
        if (Control && Font_) SendMessageW(Control, WM_SETFONT,
                                           reinterpret_cast<WPARAM>(Font_), TRUE);
        return Control;
    }

    bool CreateControls() {
        Font_ = CreateFontW(
            -16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        TitleFont_ = CreateFontW(
            -24, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        const auto Title = CreateControl(L"STATIC", L"DeskLink Alpha",
            SS_LEFT, 18, 12, 260, 30);
        if (Title && TitleFont_) SendMessageW(Title, WM_SETFONT,
            reinterpret_cast<WPARAM>(TitleFont_), TRUE);
        CreateControl(L"STATIC",
            L"Windows 11 / Server 2022+  |  Schannel only  |  Ctrl+Alt+Pause returns local",
            SS_LEFT, 290, 18, 620, 24);

        CreateControl(L"BUTTON", L"Runtime status", BS_GROUPBOX,
                      15, 48, 900, 105);
        StatusControl_ = CreateControl(
            L"STATIC", L"Runtime inactive. Start a receiver or controller session.",
            SS_LEFT, 30, 72, 650, 58, StatusText);
        CreateControl(L"BUTTON", L"Refresh", BS_PUSHBUTTON,
                      700, 76, 95, 30, RefreshStatus);
        CreateControl(L"BUTTON", L"Identity", BS_PUSHBUTTON,
                      805, 76, 95, 30, ShowIdentity);
        CreateControl(L"BUTTON", L"Arrange monitors", BS_PUSHBUTTON,
                      700, 112, 200, 28, ArrangeMonitors);

        CreateControl(L"BUTTON", L"Peer", BS_GROUPBOX,
                      15, 160, 900, 85);
        CreateControl(L"STATIC", L"Address", SS_LEFT, 30, 185, 60, 22);
        AddressControl_ = CreateControl(
            L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP,
            95, 181, 430, 28, PeerAddress, WS_EX_CLIENTEDGE);
        SendMessageW(AddressControl_, EM_SETLIMITTEXT, 253, 0);
        CreateControl(L"STATIC", L"UDP port", SS_LEFT, 545, 185, 65, 22);
        PortControl_ = CreateControl(
            L"EDIT", L"43821", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP,
            615, 181, 90, 28, PeerPort, WS_EX_CLIENTEDGE);
        SendMessageW(PortControl_, EM_SETLIMITTEXT, 5, 0);
        CreateControl(L"BUTTON", L"Discover (5s)", BS_PUSHBUTTON,
                      730, 180, 150, 30, DiscoverPeers);

        CreateControl(L"BUTTON", L"Pairing permissions granted to this peer",
                      BS_GROUPBOX, 15, 252, 900, 112);
        CreateControl(L"BUTTON", L"Allow input injection on this PC",
                      BS_AUTOCHECKBOX | WS_TABSTOP, 32, 277, 260, 24,
                      GrantInput);
        CreateControl(L"BUTTON", L"Allow peer audio into this PC",
                      BS_AUTOCHECKBOX | WS_TABSTOP, 310, 277, 250, 24,
                      GrantAudioSend);
        CreateControl(L"BUTTON", L"Allow this PC audio to peer",
                      BS_AUTOCHECKBOX | WS_TABSTOP, 580, 277, 235, 24,
                      GrantAudioReceive);
        CreateControl(L"BUTTON", L"Exchange monitor layouts",
                      BS_AUTOCHECKBOX | WS_TABSTOP, 32, 306, 200, 24,
                      GrantTopology);
        CreateControl(L"BUTTON", L"Peer may read text clipboard",
                      BS_AUTOCHECKBOX | WS_TABSTOP, 245, 306, 195, 24,
                      GrantClipboardRead);
        CreateControl(L"BUTTON", L"Peer may write text clipboard",
                      BS_AUTOCHECKBOX | WS_TABSTOP, 455, 306, 190, 24,
                      GrantClipboardWrite);
        CreateControl(L"BUTTON", L"Open pairing window", BS_PUSHBUTTON,
                      660, 302, 115, 32, OpenPairing);
        CreateControl(L"BUTTON", L"Pair with address", BS_PUSHBUTTON,
                      785, 302, 115, 32, PairPeer);

        CreateControl(L"BUTTON", L"Session", BS_GROUPBOX,
                      15, 371, 900, 178);
        CreateControl(L"BUTTON", L"Capture and route physical input",
                      BS_AUTOCHECKBOX | WS_TABSTOP, 32, 397, 275, 24,
                      CaptureInput);
        CreateControl(L"BUTTON", L"Experimental edge roaming",
                      BS_AUTOCHECKBOX | WS_TABSTOP, 32, 513, 245, 24,
                      EnableEdgeRoaming);
        CreateControl(L"BUTTON", L"Send this PC system audio",
                      BS_AUTOCHECKBOX | WS_TABSTOP, 330, 397, 245, 24,
                      SendAudio);
        CreateControl(L"BUTTON", L"Render peer system audio",
                      BS_AUTOCHECKBOX | WS_TABSTOP, 600, 397, 235, 24,
                      ReceiveAudio);
        CreateControl(L"BUTTON", L"Sync text clipboard",
                      BS_AUTOCHECKBOX | WS_TABSTOP, 600, 513, 235, 24,
                      SyncClipboard);
        CreateControl(L"BUTTON", L"Start receiver", BS_PUSHBUTTON,
                      32, 433, 160, 32, StartReceiver);
        CreateControl(L"BUTTON", L"Start controller", BS_PUSHBUTTON,
                      205, 433, 160, 32, StartController);
        CreateControl(L"BUTTON", L"Focus remote", BS_PUSHBUTTON,
                      378, 433, 150, 32, FocusRemote);
        CreateControl(L"BUTTON", L"RETURN LOCAL", BS_DEFPUSHBUTTON,
                      541, 433, 160, 32, ReturnLocalButton);
        CreateControl(L"BUTTON", L"Stop operation", BS_PUSHBUTTON,
                      714, 433, 160, 32, StopProcess);
        CreateControl(L"STATIC", L"Pointer gain %", SS_LEFT,
                      32, 480, 105, 22);
        PointerGainControl_ = CreateControl(
            L"EDIT", L"100", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP,
            140, 475, 70, 28, PointerGain, WS_EX_CLIENTEDGE);
        SendMessageW(PointerGainControl_, EM_SETLIMITTEXT, 3, 0);
        CreateControl(L"STATIC", L"Mouse DPI (0 = raw)", SS_LEFT,
                      235, 480, 150, 22);
        PointerDpiControl_ = CreateControl(
            L"EDIT", L"0", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP,
            390, 475, 90, 28, PointerDpi, WS_EX_CLIENTEDGE);
        SendMessageW(PointerDpiControl_, EM_SETLIMITTEXT, 5, 0);
        CreateControl(L"STATIC", L"Peer audio %", SS_LEFT,
                      500, 480, 95, 22);
        AudioGainControl_ = CreateControl(
            L"EDIT", L"100", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP,
            595, 475, 55, 28, AudioGain, WS_EX_CLIENTEDGE);
        SendMessageW(AudioGainControl_, EM_SETLIMITTEXT, 3, 0);
        CreateControl(L"BUTTON", L"Apply", BS_PUSHBUTTON,
                      660, 475, 75, 28, ApplyAudioGain);
        AudioMuteControl_ = CreateControl(
            L"BUTTON", L"Mute", BS_PUSHBUTTON,
            745, 475, 90, 28, ToggleAudioMute);
        CreateControl(L"STATIC",
            L"Controller starts Local. Edge roaming requires a saved monitor link and remains fail-local.",
            SS_LEFT, 285, 514, 300, 24);

        CreateControl(L"BUTTON", L"Diagnostics (memory only; input and clipboard content are never logged)",
                      BS_GROUPBOX, 15, 555, 900, 172);
        LogControl_ = CreateControl(
            L"EDIT", L"[Wrapper:Startup] Ready. No network action has been taken.\r\n",
            ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
            30, 580, 780, 128, DiagnosticLog, WS_EX_CLIENTEDGE);
        SendMessageW(LogControl_, EM_SETLIMITTEXT,
                     static_cast<WPARAM>(kMaximumLogCharacters), 0);
        CreateControl(L"BUTTON", L"Clear", BS_PUSHBUTTON,
                      825, 580, 72, 30, ClearLog);

        CreateControl(L"BUTTON", L"Application", BS_GROUPBOX,
                      15, 735, 900, 78);
        CreateControl(
            L"BUTTON", L"Keep DeskLink running when I close the window",
            BS_AUTOCHECKBOX | WS_TABSTOP, 32, 758, 370, 24,
            KeepRunningOnClose);
        CreateControl(
            L"BUTTON", L"Start DeskLink when I sign in to Windows",
            BS_AUTOCHECKBOX | WS_TABSTOP, 430, 758, 340, 24,
            StartWithWindows);
        CreateControl(
            L"STATIC", L"Use the tray menu to open DeskLink, return Local, or exit.",
            SS_LEFT, 32, 787, 700, 20);

        CheckDlgButton(Window_, GrantInput, BST_CHECKED);
        CheckDlgButton(Window_, GrantTopology, BST_CHECKED);
        CheckDlgButton(
            Window_, KeepRunningOnClose,
            ApplicationSettings_.CloseToTray ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(
            Window_, StartWithWindows,
            ApplicationSettings_.RunAtLogin ? BST_CHECKED : BST_UNCHECKED);
        SetTimer(Window_, kStatusTimer, 1'000, nullptr);
        RefreshRuntimeStatus();
        UpdateButtons();
        return AddressControl_ && PortControl_ && PointerGainControl_ &&
               PointerDpiControl_ && AudioGainControl_ && AudioMuteControl_ &&
               StatusControl_ && LogControl_;
    }

    void HandleCommand(int Id) {
        switch (Id) {
            case RefreshStatus: RefreshRuntimeStatus(); break;
            case ShowIdentity: StartSimple(desklink::LauncherOperation::Identity); break;
            case ArrangeMonitors: OpenMonitorConfigurator(); break;
            case DiscoverPeers: StartSimple(desklink::LauncherOperation::Discover); break;
            case OpenPairing: StartPairing(false); break;
            case PairPeer: StartPairing(true); break;
            case StartReceiver: StartSession(false); break;
            case StartController: StartSession(true); break;
            case FocusRemote: SendMode(desklink::DeskMode::Roam, true); break;
            case ReturnLocalButton: ReturnLocal(true); break;
            case StopProcess: StopOwnedOperation(); break;
            case ApplyAudioGain: SendAudioGain(); break;
            case ToggleAudioMute: SendAudioMute(); break;
            case ClearLog: SetWindowTextW(LogControl_, L""); break;
            case KeepRunningOnClose:
            case StartWithWindows:
                SaveApplicationSettings();
                break;
            case kTrayOpen: RestoreWindow(); break;
            case kTrayReturnLocal: ReturnLocal(true); break;
            case kTrayExit: ExitFromTray(); break;
            default: break;
        }
    }

    bool IsChecked(ControlId Id) const {
        return IsDlgButtonChecked(Window_, Id) == BST_CHECKED;
    }

    std::optional<std::uint16_t> ReadPort() const {
        wchar_t Text[8]{};
        const auto Length = GetWindowTextW(PortControl_, Text,
                                           static_cast<int>(std::size(Text)));
        if (Length <= 0 || Length > 5) return std::nullopt;
        unsigned long Value = 0;
        for (int Index = 0; Index < Length; ++Index) {
            if (Text[Index] < L'0' || Text[Index] > L'9') return std::nullopt;
            Value = Value * 10 + static_cast<unsigned long>(Text[Index] - L'0');
        }
        if (Value == 0 || Value > 65'535) return std::nullopt;
        return static_cast<std::uint16_t>(Value);
    }

    std::optional<std::uint16_t> ReadUnsignedControl(
        HWND Control,
        std::uint16_t Minimum,
        std::uint16_t Maximum) const {
        wchar_t Text[8]{};
        const auto Length = GetWindowTextW(
            Control, Text, static_cast<int>(std::size(Text)));
        if (Length <= 0 || Length > 5) return std::nullopt;
        unsigned long Value = 0;
        for (int Index = 0; Index < Length; ++Index) {
            if (Text[Index] < L'0' || Text[Index] > L'9') return std::nullopt;
            Value = Value * 10 + static_cast<unsigned long>(Text[Index] - L'0');
        }
        if (Value < Minimum || Value > Maximum) return std::nullopt;
        return static_cast<std::uint16_t>(Value);
    }

    std::optional<std::wstring> ReadAddress() const {
        const auto Length = GetWindowTextLengthW(AddressControl_);
        if (Length <= 0 || Length > 253) return std::nullopt;
        std::wstring Result(static_cast<std::size_t>(Length) + 1, L'\0');
        const auto Copied = GetWindowTextW(
            AddressControl_, Result.data(), static_cast<int>(Result.size()));
        if (Copied != Length) return std::nullopt;
        Result.resize(static_cast<std::size_t>(Copied));
        return Result;
    }

    std::optional<desklink::ControlTopologyState> QueryDisplayTopologies() {
        const auto Response = SendControl(
            desklink::GetDisplayTopologiesControlRequest{},
            std::chrono::milliseconds{750});
        if (Response && Response->Status == desklink::ControlStatus::Ok &&
            Response->Topologies) {
            return Response->Topologies;
        }

        try {
            desklink::BCryptPairingCrypto Crypto;
            auto Certificate = desklink::Win32DeviceCertificate::LoadOrCreate(
                kDeviceKeyName, Crypto);
            desklink::Win32DisplayTopology Topology;
            if (!Certificate || !Topology.Refresh()) return std::nullopt;
            const auto LocalMachine = desklink::DeriveMachineId(
                Certificate->CertificatePin());
            desklink::ControlTopologyState State;
            State.Machines.push_back(desklink::ControlMachineTopology{
                LocalMachine,
                desklink::DisplayTopologyExchangeStatus::Ready,
                Topology.Current(), true, false});
            return desklink::IsValidControlTopologyState(State)
                ? std::optional(std::move(State))
                : std::nullopt;
        } catch (...) {
            return std::nullopt;
        }
    }

    bool EnsureLocalForConfigurationSave() {
        const bool Session = ActiveOperation_ ==
                desklink::LauncherOperation::Serve ||
            ActiveOperation_ == desklink::LauncherOperation::Focus;
        if (!ActiveProcess_ || !Session) return true;
        const auto Before = SendControl(
            desklink::GetStateControlRequest{},
            std::chrono::milliseconds{500});
        if (!Before || Before->Status != desklink::ControlStatus::Ok ||
            !Before->State) {
            return false;
        }
        if (Before->State->RemoteFocused || Before->State->CaptureActive) {
            if (!SendMode(desklink::DeskMode::LockPc1, true)) return false;
        }
        const auto After = SendControl(
            desklink::GetStateControlRequest{},
            std::chrono::milliseconds{500});
        return After && After->Status == desklink::ControlStatus::Ok &&
               After->State && !After->State->RemoteFocused &&
               !After->State->CaptureActive;
    }

    void OpenMonitorConfigurator() {
        const auto Saved = desklink::ShowWin32MonitorConfigurator(
            Window_, RoamingSettingsPath_,
            desklink::Win32MonitorConfiguratorCallbacks{
                [this] { return QueryDisplayTopologies(); },
                [this] { return EnsureLocalForConfigurationSave(); },
            });
        AppendLog(Saved
            ? L"[Roaming:Configurator] monitor graph saved; edge switching remains disabled\r\n"
            : L"[Roaming:Configurator] configurator closed without a new saved graph\r\n");
    }

    void SaveApplicationSettings() {
        if (!ApplicationSettingsStore_) return;
        const auto Previous = ApplicationSettings_;
        auto Candidate = Previous;
        Candidate.CloseToTray = IsChecked(KeepRunningOnClose);
        Candidate.RunAtLogin = IsChecked(StartWithWindows);
        const auto Executable = GetExecutablePath();
        if (!Executable || !desklink::SetWin32RunAtLogin(
                Candidate.RunAtLogin, *Executable) ||
            !ApplicationSettingsStore_->Save(Candidate)) {
            if (Executable) {
                (void)desklink::SetWin32RunAtLogin(
                    Previous.RunAtLogin, *Executable);
            }
            CheckDlgButton(
                Window_, KeepRunningOnClose,
                Previous.CloseToTray ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(
                Window_, StartWithWindows,
                Previous.RunAtLogin ? BST_CHECKED : BST_UNCHECKED);
            MessageBoxW(
                Window_, L"The application preference could not be saved.",
                L"DeskLink settings", MB_OK | MB_ICONERROR);
            return;
        }
        ApplicationSettings_ = Candidate;
        AppendLog(L"[App:Settings] application preferences saved\r\n");
    }

    bool AddTrayIcon() {
        TrayIcon_ = {};
        TrayIcon_.cbSize = sizeof(TrayIcon_);
        TrayIcon_.hWnd = Window_;
        TrayIcon_.uID = kTrayIconId;
        TrayIcon_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        TrayIcon_.uCallbackMessage = kTrayMessage;
        TrayIcon_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(TrayIcon_.szTip, L"DeskLink — Local");
        TrayActive_ = Shell_NotifyIconW(NIM_ADD, &TrayIcon_) != FALSE;
        return TrayActive_;
    }

    void RemoveTrayIcon() noexcept {
        if (!TrayActive_) return;
        (void)Shell_NotifyIconW(NIM_DELETE, &TrayIcon_);
        TrayActive_ = false;
    }

    void UpdateTrayIcon() {
        if (!TrayActive_) return;
        const wchar_t* Status = RuntimeAvailable_
            ? L"DeskLink — Connected / Local"
            : ActiveProcess_
                ? L"DeskLink — Connecting"
                : L"DeskLink — Local";
        wcscpy_s(TrayIcon_.szTip, Status);
        TrayIcon_.uFlags = NIF_TIP;
        (void)Shell_NotifyIconW(NIM_MODIFY, &TrayIcon_);
    }

    void RestoreWindow() {
        ShowWindow(Window_, SW_RESTORE);
        SetForegroundWindow(Window_);
    }

    void HandleTrayMessage(UINT Message) {
        if (Message == WM_LBUTTONUP || Message == WM_LBUTTONDBLCLK) {
            RestoreWindow();
            return;
        }
        if (Message != WM_RBUTTONUP && Message != WM_CONTEXTMENU) return;
        const auto Menu = CreatePopupMenu();
        if (!Menu) return;
        const wchar_t* Status = RuntimeAvailable_
            ? L"Status: connected"
            : ActiveProcess_ ? L"Status: connecting" : L"Status: local / idle";
        AppendMenuW(Menu, MF_STRING | MF_DISABLED, 0, Status);
        AppendMenuW(Menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(Menu, MF_STRING, kTrayOpen, L"Open DeskLink");
        AppendMenuW(
            Menu, MF_STRING | (ActiveProcess_ ? MF_ENABLED : MF_GRAYED),
            kTrayReturnLocal, L"Return Local");
        AppendMenuW(Menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(Menu, MF_STRING, kTrayExit, L"Exit DeskLink");
        POINT Cursor{};
        GetCursorPos(&Cursor);
        SetForegroundWindow(Window_);
        const auto Command = TrackPopupMenu(
            Menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
            Cursor.x, Cursor.y, 0, Window_, nullptr);
        DestroyMenu(Menu);
        if (Command != 0) HandleCommand(static_cast<int>(Command));
    }

    void ExitFromTray() {
        ExitRequested_ = true;
        ReturnLocal(false);
        StopProcessGracefully();
        DestroyWindow(Window_);
    }

    std::optional<desklink::LauncherRequest> BaseNetworkRequest() const {
        const auto Port = ReadPort();
        if (!Port) {
            MessageBoxW(Window_, L"Enter a UDP port from 1 to 65535.",
                        L"DeskLink Alpha", MB_OK | MB_ICONERROR);
            return std::nullopt;
        }
        desklink::LauncherRequest Request;
        Request.Port = *Port;
        return Request;
    }

    void StartSimple(desklink::LauncherOperation Operation) {
        desklink::LauncherRequest Request;
        Request.Operation = Operation;
        StartRequest(Request);
    }

    void StartPairing(bool Connect) {
        auto Request = BaseNetworkRequest();
        if (!Request) return;
        Request->Operation = Connect ? desklink::LauncherOperation::PairConnect
                                     : desklink::LauncherOperation::PairListen;
        if (Connect) {
            const auto Address = ReadAddress();
            if (!Address) {
                MessageBoxW(Window_, L"Enter the peer address before pairing.",
                            L"DeskLink Alpha", MB_OK | MB_ICONERROR);
                return;
            }
            Request->Host = *Address;
        }
        Request->GrantInput = IsChecked(GrantInput);
        Request->GrantAudioSend = IsChecked(GrantAudioSend);
        Request->GrantAudioReceive = IsChecked(GrantAudioReceive);
        Request->GrantTopology = IsChecked(GrantTopology);
        Request->GrantClipboardRead = IsChecked(GrantClipboardRead);
        Request->GrantClipboardWrite = IsChecked(GrantClipboardWrite);
        StartRequest(*Request);
    }

    void StartSession(bool Controller) {
        auto Request = BaseNetworkRequest();
        if (!Request) return;
        Request->Operation = Controller ? desklink::LauncherOperation::Focus
                                        : desklink::LauncherOperation::Serve;
        Request->CaptureInput = IsChecked(CaptureInput);
        Request->SyncClipboard = IsChecked(SyncClipboard);
        if (Controller) {
            const auto Address = ReadAddress();
            if (!Address) {
                MessageBoxW(Window_, L"Enter the peer address before connecting.",
                            L"DeskLink Alpha", MB_OK | MB_ICONERROR);
                return;
            }
            Request->Host = *Address;
            Request->ReceiveAudio = IsChecked(ReceiveAudio);
        } else {
            Request->SendAudio = IsChecked(SendAudio);
        }
        if (IsChecked(EnableEdgeRoaming)) {
            if (!Request->CaptureInput) {
                MessageBoxW(
                    Window_,
                    L"Experimental edge roaming requires physical input capture.",
                    L"DeskLink Alpha", MB_OK | MB_ICONERROR);
                return;
            }
            Request->EdgeRoamingSettingsPath = RoamingSettingsPath_;
        } else if (!Controller && Request->CaptureInput) {
            MessageBoxW(
                Window_,
                L"Receiver-side physical capture is only enabled for explicit edge roaming.",
                L"DeskLink Alpha", MB_OK | MB_ICONERROR);
            return;
        }
        if (Request->CaptureInput) {
            const auto Gain = ReadUnsignedControl(
                PointerGainControl_, desklink::kMinimumPointerGainPercent,
                desklink::kMaximumPointerGainPercent);
            const auto Dpi = ReadUnsignedControl(
                PointerDpiControl_, 0, desklink::kMaximumPointerDpi);
            if (!Gain || !Dpi ||
                (*Dpi != 0 && *Dpi < desklink::kMinimumPointerDpi)) {
                MessageBoxW(
                    Window_,
                    L"Pointer gain must be 25..400. Mouse DPI must be 0 (raw) or 100..32000.",
                    L"DeskLink Alpha", MB_OK | MB_ICONERROR);
                return;
            }
            Request->PointerCalibration.GainPercent = *Gain;
            Request->PointerCalibration.SourceDpi = *Dpi;
        }
        if (Request->CaptureInput && MessageBoxW(
                Window_,
                L"Physical keyboard and mouse input can be suppressed while the peer is focused.\n\n"
                L"Emergency chord: Ctrl+Alt+Pause/Break\n\n"
                L"The session starts Local and only captures after an explicit focus or edge crossing. Continue?",
                L"Enable physical capture", MB_OKCANCEL | MB_ICONWARNING |
                MB_DEFBUTTON2) != IDOK) {
            return;
        }
        StartRequest(*Request);
    }

    void StartRequest(const desklink::LauncherRequest& Request) {
        if (ActiveProcess_) {
            MessageBoxW(Window_, L"Stop the active operation first.",
                        L"DeskLink Alpha", MB_OK | MB_ICONINFORMATION);
            return;
        }
        const auto Arguments = desklink::BuildLauncherArguments(Request);
        if (!Arguments) {
            MessageBoxW(Window_, L"The requested operation was not valid.",
                        L"DeskLink Alpha", MB_OK | MB_ICONERROR);
            return;
        }
        const auto Executable = GetExecutablePath();
        if (!Executable) {
            MessageBoxW(Window_, L"Could not locate the DeskLink Alpha executable.",
                        L"DeskLink Alpha", MB_OK | MB_ICONERROR);
            return;
        }
        auto PairExecutable = std::filesystem::path(*Executable).parent_path() /
                              L"desklink_pair.exe";
        if (!std::filesystem::is_regular_file(PairExecutable)) {
            MessageBoxW(Window_,
                        L"desklink_pair.exe must be in the same folder as DeskLink Alpha.",
                        L"DeskLink Alpha", MB_OK | MB_ICONERROR);
            return;
        }
        const auto CommandLine = desklink::BuildWindowsCommandLine(
            PairExecutable.native(), *Arguments);
        ActiveOperation_ = Request.Operation;
        if (!CommandLine || !StartChild(PairExecutable, *CommandLine)) {
            ActiveOperation_.reset();
            MessageBoxW(Window_, L"The DeskLink operation could not be started.",
                        L"DeskLink Alpha", MB_OK | MB_ICONERROR);
            return;
        }
        AppendLog(L"[Wrapper:Process] operation started\r\n");
        UpdateButtons();
    }

    bool StartChild(const std::filesystem::path& Application,
                    const std::wstring& CommandLine) {
        SECURITY_ATTRIBUTES Security{};
        Security.nLength = sizeof(Security);
        Security.bInheritHandle = TRUE;
        HANDLE OutputReadRaw{};
        HANDLE OutputWriteRaw{};
        HANDLE InputReadRaw{};
        HANDLE InputWriteRaw{};
        if (!CreatePipe(&OutputReadRaw, &OutputWriteRaw, &Security, 0) ||
            !SetHandleInformation(OutputReadRaw, HANDLE_FLAG_INHERIT, 0) ||
            !CreatePipe(&InputReadRaw, &InputWriteRaw, &Security, 0) ||
            !SetHandleInformation(InputWriteRaw, HANDLE_FLAG_INHERIT, 0)) {
            if (OutputReadRaw) CloseHandle(OutputReadRaw);
            if (OutputWriteRaw) CloseHandle(OutputWriteRaw);
            if (InputReadRaw) CloseHandle(InputReadRaw);
            if (InputWriteRaw) CloseHandle(InputWriteRaw);
            return false;
        }
        auto OutputRead = TakeHandle(OutputReadRaw);
        auto OutputWrite = TakeHandle(OutputWriteRaw);
        auto InputRead = TakeHandle(InputReadRaw);
        auto InputWrite = TakeHandle(InputWriteRaw);

        STARTUPINFOW Startup{};
        Startup.cb = sizeof(Startup);
        Startup.dwFlags = STARTF_USESTDHANDLES;
        Startup.hStdInput = InputRead.get();
        Startup.hStdOutput = OutputWrite.get();
        Startup.hStdError = OutputWrite.get();
        PROCESS_INFORMATION Process{};
        std::wstring MutableCommandLine = CommandLine;
        const auto WorkingDirectory = Application.parent_path().native();
        if (!CreateProcessW(
                Application.c_str(), MutableCommandLine.data(), nullptr, nullptr,
                TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr,
                WorkingDirectory.c_str(), &Startup, &Process)) {
            return false;
        }
        CloseHandle(Process.hThread);
        OutputWrite.reset();
        InputRead.reset();
        ActiveProcess_ = TakeHandle(Process.hProcess);
        ProcessInput_ = std::move(InputWrite);
        ProcessOutput_ = std::move(OutputRead);

        const auto Window = Window_;
        const auto ProcessHandle = static_cast<HANDLE>(ActiveProcess_.get());
        const auto OutputHandle = static_cast<HANDLE>(ProcessOutput_.get());
        ProcessThread_ = std::jthread(
            [Window, ProcessHandle, OutputHandle](std::stop_token StopToken) {
                std::array<char, 4'096> Buffer{};
                for (;;) {
                    DWORD Available{};
                    if (PeekNamedPipe(OutputHandle, nullptr, 0, nullptr,
                                      &Available, nullptr) && Available != 0) {
                        DWORD Read{};
                        const auto Wanted = static_cast<DWORD>(
                            std::min<std::size_t>(Available, Buffer.size()));
                        if (ReadFile(OutputHandle, Buffer.data(), Wanted, &Read,
                                     nullptr) && Read != 0) {
                            auto Text = std::make_unique<std::wstring>(
                                DecodeProcessOutput(std::span<const char>(
                                    Buffer.data(), Read)));
                            if (!PostMessageW(Window, kProcessLogMessage, 0,
                                              reinterpret_cast<LPARAM>(Text.get()))) {
                                break;
                            }
                            (void)Text.release();
                        }
                    }
                    const auto Wait = WaitForSingleObject(ProcessHandle, 50);
                    if (Wait == WAIT_OBJECT_0 || Wait == WAIT_FAILED ||
                        StopToken.stop_requested()) {
                        break;
                    }
                }
                DWORD ExitCode = ERROR_PROCESS_ABORTED;
                if (WaitForSingleObject(ProcessHandle, 0) == WAIT_OBJECT_0) {
                    (void)GetExitCodeProcess(ProcessHandle, &ExitCode);
                    DWORD Available{};
                    while (PeekNamedPipe(OutputHandle, nullptr, 0, nullptr,
                                         &Available, nullptr) && Available != 0) {
                        DWORD Read{};
                        const auto Wanted = static_cast<DWORD>(
                            std::min<std::size_t>(Available, Buffer.size()));
                        if (!ReadFile(OutputHandle, Buffer.data(), Wanted, &Read,
                                      nullptr) || Read == 0) {
                            break;
                        }
                        auto Text = std::make_unique<std::wstring>(
                            DecodeProcessOutput(std::span<const char>(
                                Buffer.data(), Read)));
                        if (!PostMessageW(Window, kProcessLogMessage, 0,
                                          reinterpret_cast<LPARAM>(Text.get()))) {
                            break;
                        }
                        (void)Text.release();
                    }
                }
                (void)PostMessageW(Window, kProcessExitedMessage, ExitCode, 0);
            });
        return true;
    }

    void AppendLog(std::wstring_view Text) {
        if (!LogControl_ || Text.empty()) return;
        auto Length = GetWindowTextLengthW(LogControl_);
        if (Length > static_cast<int>(kMaximumLogCharacters)) {
            std::wstring Existing(static_cast<std::size_t>(Length) + 1, L'\0');
            const auto Copied = GetWindowTextW(
                LogControl_, Existing.data(), static_cast<int>(Existing.size()));
            if (Copied > static_cast<int>(kRetainedLogCharacters)) {
                Existing.erase(0, static_cast<std::size_t>(Copied) -
                    kRetainedLogCharacters);
                SetWindowTextW(LogControl_, Existing.c_str());
            }
        }
        SendMessageW(LogControl_, EM_SETSEL, static_cast<WPARAM>(-1),
                     static_cast<LPARAM>(-1));
        SendMessageW(LogControl_, EM_REPLACESEL, FALSE,
                     reinterpret_cast<LPARAM>(std::wstring(Text).c_str()));
        SendMessageW(LogControl_, EM_SCROLLCARET, 0, 0);
    }

    void FinishProcess(DWORD ExitCode) {
        if (!ActiveProcess_) return;
        if (ProcessThread_.joinable()) ProcessThread_.join();
        ProcessInput_.reset();
        ProcessOutput_.reset();
        ActiveProcess_.reset();
        ActiveOperation_.reset();
        AppendLog(L"[Wrapper:Process] operation exited with code " +
                  std::to_wstring(ExitCode) + L"\r\n");
        RefreshRuntimeStatus();
        UpdateButtons();
    }

    void StopProcessGracefully() {
        if (!ActiveProcess_ || !ProcessInput_) return;
        constexpr char NewLine = '\n';
        DWORD Written{};
        (void)WriteFile(ProcessInput_.get(), &NewLine, 1, &Written, nullptr);
        ProcessInput_.reset();
        AppendLog(L"[Wrapper:Process] graceful stop requested\r\n");
    }

    void StopOwnedOperation() {
        if (!ActiveProcess_) return;
        if (ActiveOperation_ == desklink::LauncherOperation::Serve ||
            ActiveOperation_ == desklink::LauncherOperation::Focus) {
            ReturnLocal(false);
        }
        StopProcessGracefully();
    }

    void ShutDownProcess() noexcept {
        if (!ActiveProcess_) return;
        if (Window_) ReturnLocal(false);
        StopProcessGracefully();
        (void)WaitForSingleObject(ActiveProcess_.get(), 5'000);
        if (ProcessThread_.joinable()) {
            ProcessThread_.request_stop();
            ProcessThread_.join();
        }
        ProcessInput_.reset();
        ProcessOutput_.reset();
        ActiveProcess_.reset();
        ActiveOperation_.reset();
    }

    std::optional<desklink::ControlResponse> SendControl(
        desklink::ControlRequestPayload Payload,
        std::chrono::milliseconds Timeout) {
        auto RequestId = ++RequestId_;
        if (RequestId == 0) RequestId = ++RequestId_;
        return desklink::Win32ControlPipeClient::Send(
            desklink::ControlRequest{RequestId, std::move(Payload)}, {}, Timeout);
    }

    bool SendMode(desklink::DeskMode Mode, bool Report) {
        const auto Response = SendControl(
            desklink::SetDesiredModeControlRequest{Mode},
            std::chrono::milliseconds{500});
        const bool Applied = Response &&
            Response->Status == desklink::ControlStatus::Ok;
        if (Report) {
            AppendLog(Applied
                ? L"[Wrapper:Control] requested mode applied\r\n"
                : L"[Wrapper:Control] runtime did not apply the requested mode\r\n");
        }
        if (Applied) RefreshRuntimeStatus();
        return Applied;
    }

    void SendAudioGain() {
        const auto GainPercent = ReadUnsignedControl(
            AudioGainControl_, 0, 100);
        if (!GainPercent) {
            MessageBoxW(Window_, L"Peer audio volume must be 0..100 percent.",
                        L"DeskLink Alpha", MB_OK | MB_ICONERROR);
            return;
        }
        const auto Response = SendControl(
            desklink::SetAudioGainControlRequest{
                static_cast<std::uint16_t>(*GainPercent * 100)},
            std::chrono::milliseconds{500});
        const bool Applied = Response &&
            Response->Status == desklink::ControlStatus::Ok;
        AppendLog(Applied
            ? L"[Wrapper:Audio] peer volume applied\r\n"
            : L"[Wrapper:Audio] runtime did not apply peer volume\r\n");
        if (Applied) RefreshRuntimeStatus();
    }

    void SendAudioMute() {
        const auto Response = SendControl(
            desklink::ToggleAudioMuteControlRequest{},
            std::chrono::milliseconds{500});
        const bool Applied = Response &&
            Response->Status == desklink::ControlStatus::Ok;
        AppendLog(Applied
            ? L"[Wrapper:Audio] peer mute toggled\r\n"
            : L"[Wrapper:Audio] runtime did not toggle peer mute\r\n");
        if (Applied) RefreshRuntimeStatus();
    }

    void ReturnLocal(bool Report) {
        if (!ActiveProcess_) {
            if (Report) AppendLog(
                L"[Wrapper:Control] no active session; input is local\r\n");
            return;
        }
        if (!SendMode(desklink::DeskMode::LockPc1, Report) &&
            (ActiveOperation_ == desklink::LauncherOperation::Serve ||
             ActiveOperation_ == desklink::LauncherOperation::Focus)) {
            StopProcessGracefully();
            if (Report) AppendLog(
                L"[Wrapper:Control] fail-local fallback stopped the session\r\n");
        }
    }

    void RefreshRuntimeStatus() {
        const auto Response = SendControl(
            desklink::GetStateControlRequest{},
            std::chrono::milliseconds{125});
        if (!Response || Response->Status != desklink::ControlStatus::Ok ||
            !Response->State) {
            SetWindowTextW(StatusControl_,
                ActiveProcess_
                    ? L"Operation running; authenticated runtime is not ready yet."
                    : L"Runtime inactive. Start a receiver or controller session.");
            RuntimeAvailable_ = false;
            UpdateButtons();
            return;
        }
        const auto& State = *Response->State;
        std::wstring Text = L"Role: ";
        Text += RoleName(State.Role);
        Text += L"    Mode: ";
        Text += ModeName(State.DesiredMode);
        Text += L"    Connected peers: ";
        Text += std::to_wstring(State.ConnectedPeerCount);
        Text += L"    Focus: ";
        Text += State.RemoteFocused ? L"Remote" : L"Local";
        Text += L"    Capture: ";
        Text += State.CaptureActive ? L"Active" : L"Off";
        Text += L"    Peer audio: ";
        Text += State.AudioMuted ? L"Muted" :
            std::to_wstring(State.AudioGainPermyriad / 100) + L"%";
        Text += L"\r\nLocal identity: ";
        Text += FormatMachine(State.LocalMachine);
        if (State.RemoteFocused) {
            Text += L"    Focused peer: ";
            Text += FormatMachine(State.FocusedMachine);
        }
        SetWindowTextW(StatusControl_, Text.c_str());
        if (GetFocus() != AudioGainControl_) {
            SetWindowTextW(AudioGainControl_,
                std::to_wstring(State.AudioGainPermyriad / 100).c_str());
        }
        SetWindowTextW(AudioMuteControl_,
            State.AudioMuted ? L"Unmute" : L"Mute");
        RuntimeAvailable_ = true;
        UpdateButtons();
    }

    void UpdateButtons() {
        const bool Busy = ActiveProcess_ != nullptr;
        const bool Session = ActiveOperation_ ==
                desklink::LauncherOperation::Serve ||
            ActiveOperation_ == desklink::LauncherOperation::Focus;
        const bool Controller = ActiveOperation_ ==
            desklink::LauncherOperation::Focus;
        for (const auto Id : {ShowIdentity, DiscoverPeers, OpenPairing,
                              PairPeer, StartReceiver, StartController}) {
            EnableWindow(GetDlgItem(Window_, Id), !Busy);
        }
        EnableWindow(GetDlgItem(Window_, StopProcess), Busy);
        EnableWindow(GetDlgItem(Window_, FocusRemote),
                     Controller && RuntimeAvailable_);
        EnableWindow(GetDlgItem(Window_, ReturnLocalButton), Session);
        EnableWindow(GetDlgItem(Window_, ApplyAudioGain),
                     Controller && RuntimeAvailable_);
        EnableWindow(GetDlgItem(Window_, ToggleAudioMute),
                     Controller && RuntimeAvailable_);
        if (!Busy) RuntimeAvailable_ = false;
        UpdateTrayIcon();
    }

    HINSTANCE Instance_{};
    HWND Window_{};
    HWND StatusControl_{};
    HWND AddressControl_{};
    HWND PortControl_{};
    HWND PointerGainControl_{};
    HWND PointerDpiControl_{};
    HWND AudioGainControl_{};
    HWND AudioMuteControl_{};
    HWND LogControl_{};
    HFONT Font_{};
    HFONT TitleFont_{};
    UniqueHandle ActiveProcess_;
    UniqueHandle ProcessInput_;
    UniqueHandle ProcessOutput_;
    std::jthread ProcessThread_;
    std::optional<desklink::LauncherOperation> ActiveOperation_;
    std::atomic_uint64_t RequestId_{1};
    std::filesystem::path RoamingSettingsPath_;
    std::unique_ptr<desklink::Win32ApplicationSettingsStore>
        ApplicationSettingsStore_;
    desklink::Win32ApplicationSettings ApplicationSettings_;
    NOTIFYICONDATAW TrayIcon_{};
    bool RuntimeAvailable_{};
    bool TrayActive_{};
    bool ExitRequested_{};
};

} // namespace

int WINAPI wWinMain(
    HINSTANCE Instance, HINSTANCE, PWSTR CommandLine, int ShowCommand) {
    INITCOMMONCONTROLSEX Controls{sizeof(Controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&Controls);
    (void)SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const auto InstanceMutex = TakeHandle(CreateMutexW(
        nullptr, FALSE, L"Local\\DeskLink.Alpha.v1"));
    if (!InstanceMutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        const auto Existing = FindWindowW(kWindowClass, nullptr);
        if (Existing) PostMessageW(Existing, kActivateMessage, 0, 0);
        return 0;
    }
    const bool StartInBackground = CommandLine &&
        std::wstring_view(CommandLine) == L"--background";

    AlphaWindow Window(Instance);
    if (!Window.Create(ShowCommand, StartInBackground)) {
        MessageBoxW(nullptr, L"DeskLink Alpha could not create its window.",
                    L"DeskLink Alpha", MB_OK | MB_ICONERROR);
        return 1;
    }
    MSG Message{};
    while (GetMessageW(&Message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(GetActiveWindow(), &Message)) {
            TranslateMessage(&Message);
            DispatchMessageW(&Message);
        }
    }
    return static_cast<int>(Message.wParam);
}
