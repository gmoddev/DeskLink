#ifdef _WIN32

#include "desklink/win32_background_shell.hpp"

#include "desklink/control.hpp"
#include "desklink/product.hpp"
#include "desklink/product_shell.hpp"
#include "desklink/win32_control.hpp"
#include "desklink/win32_product_lifecycle.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace desklink {
namespace {

constexpr wchar_t kWindowClass[] = L"DeskLinkRuntimeShellWindow.v1";
constexpr wchar_t kUiWindowClass[] = L"DeskLinkShellLifecycleWindow.v1";
constexpr UINT kTrayMessage = WM_APP + 0x44u;
constexpr UINT kRefreshMessage = WM_APP + 0x45u;
constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT kTrayOpen = 1;
constexpr UINT kTrayReturnLocal = 2;
constexpr UINT kTrayPause = 3;
constexpr UINT kTrayExit = 4;
constexpr UINT kTrayFocusPeer = 5;
constexpr UINT kTrayClipboard = 6;
constexpr UINT kTrayAudioMute = 7;
constexpr int kFocusPeerHotkeyId = 0xD311;
constexpr int kReturnLocalHotkeyId = 0xD312;

struct ProductHotkeyChord {
    UINT Modifiers{};
    UINT Key{};
};

std::optional<ProductHotkeyChord> HotkeyChord(
    ProductHotkey Hotkey) noexcept {
    switch (Hotkey) {
        case ProductHotkey::Off:
            return std::nullopt;
        case ProductHotkey::CtrlAltF11:
            return ProductHotkeyChord{
                MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F11};
        case ProductHotkey::CtrlAltF12:
            return ProductHotkeyChord{
                MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F12};
        case ProductHotkey::CtrlShiftF11:
            return ProductHotkeyChord{
                MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F11};
        case ProductHotkey::CtrlShiftF12:
            return ProductHotkeyChord{
                MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F12};
    }
    return std::nullopt;
}

ProductShellState StateFromControl(const ControlState& State) noexcept {
    if (State.RuntimePhase == BrokerRuntimePhase::Paused) {
        return ProductShellState::Paused;
    }
    if (State.RuntimePhase == BrokerRuntimePhase::ActionRequired) {
        return ProductShellState::ActionRequired;
    }
    if (State.RemoteFocused) return ProductShellState::RemoteFocus;
    if (State.ConnectedPeerCount != 0) {
        return ProductShellState::ConnectedLocal;
    }
    if (State.RuntimePhase == BrokerRuntimePhase::Discovering ||
        State.RuntimePhase == BrokerRuntimePhase::Connecting ||
        State.RuntimePhase == BrokerRuntimePhase::RetryWaiting) {
        return ProductShellState::Connecting;
    }
    return ProductShellState::Offline;
}

std::optional<std::wstring> Utf8ToWide(std::string_view Text) {
    if (Text.empty()) return std::nullopt;
    const auto Required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, Text.data(),
        static_cast<int>(Text.size()), nullptr, 0);
    if (Required <= 0) return std::nullopt;
    std::wstring Result(static_cast<std::size_t>(Required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, Text.data(),
            static_cast<int>(Text.size()), Result.data(), Required) !=
        Required) {
        return std::nullopt;
    }
    return Result;
}

UINT GetActivateMessage() noexcept {
    static const UINT Message =
        RegisterWindowMessageW(L"DeskLink.ActivateShell.v1");
    return Message;
}

} // namespace

class Win32BackgroundShell::Implementation final {
public:
    Implementation(
        std::filesystem::path ProductShellExecutable,
        Win32ProductPreferencesStore& PreferencesStore,
        std::function<void()> RequestExit)
        : ProductShellExecutable_(std::move(ProductShellExecutable)),
          PreferencesStore_(PreferencesStore),
          RequestExit_(std::move(RequestExit)) {}

    ~Implementation() { Stop(); }

    [[nodiscard]] bool Start() {
        std::unique_lock Lock(LifecycleMutex_);
        if (Thread_.joinable()) return false;
        StartCompleted_ = false;
        StartSucceeded_ = false;
        Thread_ = std::thread([this] { Run(); });
        if (!LifecycleChanged_.wait_for(
                Lock, std::chrono::seconds(5),
                [this] { return StartCompleted_; })) {
            Lock.unlock();
            Stop();
            return false;
        }
        const bool Result = StartSucceeded_;
        Lock.unlock();
        if (!Result && Thread_.joinable()) Thread_.join();
        return Result;
    }

    void Stop() noexcept {
        const auto Window = Window_.load(std::memory_order_acquire);
        if (Window) {
            (void)PostMessageW(Window, WM_CLOSE, 0, 0);
        } else {
            const auto ThreadId = ThreadId_.load(std::memory_order_acquire);
            if (ThreadId != 0) {
                (void)PostThreadMessageW(ThreadId, WM_QUIT, 0, 0);
            }
        }
        if (Thread_.joinable() &&
            Thread_.get_id() != std::this_thread::get_id()) {
            Thread_.join();
        }
    }

    void PreferencesChanged() noexcept {
        const auto Window = Window_.load(std::memory_order_acquire);
        if (Window) (void)PostMessageW(Window, kRefreshMessage, 0, 0);
    }

    [[nodiscard]] bool ApplyPreferences(
        const ProductPreferences& Preferences) noexcept {
        return ApplyProductHotkeys(Preferences);
    }

private:
    static LRESULT CALLBACK WindowProcedure(
        HWND Window, UINT Message, WPARAM WParam, LPARAM LParam) {
        auto* Self = reinterpret_cast<Implementation*>(
            GetWindowLongPtrW(Window, GWLP_USERDATA));
        if (Message == WM_NCCREATE) {
            const auto Create = reinterpret_cast<CREATESTRUCTW*>(LParam);
            Self = static_cast<Implementation*>(Create->lpCreateParams);
            SetWindowLongPtrW(
                Window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(Self));
        }
        if (!Self) return DefWindowProcW(Window, Message, WParam, LParam);

        if (Message == WM_TIMER && WParam == kRefreshTimer) {
            Self->Refresh();
            return 0;
        }
        if (Message == kRefreshMessage) {
            Self->Refresh();
            return 0;
        }
        if (Message == WM_HOTKEY) {
            if (WParam == kFocusPeerHotkeyId) {
                Self->FocusPreferredPeer();
                return 0;
            }
            if (WParam == kReturnLocalHotkeyId) {
                Self->ReturnLocal();
                return 0;
            }
        }
        if (Message == kTrayMessage) {
            switch (LOWORD(LParam)) {
                case WM_LBUTTONDBLCLK:
                    (void)Self->OpenProductShell();
                    return 0;
                case WM_CONTEXTMENU:
                    Self->ShowTrayMenu();
                    return 0;
                default: break;
            }
        }
        if (Message == WM_CLOSE) {
            DestroyWindow(Window);
            return 0;
        }
        if (Message == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(Window, Message, WParam, LParam);
    }

    void CompleteStart(bool Succeeded) noexcept {
        {
            std::scoped_lock Lock(LifecycleMutex_);
            StartSucceeded_ = Succeeded;
            StartCompleted_ = true;
        }
        LifecycleChanged_.notify_all();
    }

    void Run() noexcept {
        ThreadId_.store(GetCurrentThreadId(), std::memory_order_release);
        const auto Instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW WindowClass{sizeof(WindowClass)};
        WindowClass.lpfnWndProc = WindowProcedure;
        WindowClass.hInstance = Instance;
        WindowClass.lpszClassName = kWindowClass;
        if (!RegisterClassExW(&WindowClass) &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            CompleteStart(false);
            ThreadId_.store(0, std::memory_order_release);
            return;
        }
        const auto Window = CreateWindowExW(
            0, kWindowClass, L"DeskLink background shell", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, Instance, this);
        if (!Window) {
            CompleteStart(false);
            ThreadId_.store(0, std::memory_order_release);
            return;
        }
        Window_.store(Window, std::memory_order_release);
        if (!AddTrayIcon(Window) ||
            SetTimer(Window, kRefreshTimer, 1'500, nullptr) == 0) {
            RemoveTrayIcon();
            DestroyWindow(Window);
            Window_.store(nullptr, std::memory_order_release);
            CompleteStart(false);
            ThreadId_.store(0, std::memory_order_release);
            return;
        }
        Refresh();
        CompleteStart(true);

        MSG Message{};
        while (GetMessageW(&Message, nullptr, 0, 0) > 0) {
            TranslateMessage(&Message);
            DispatchMessageW(&Message);
        }
        (void)KillTimer(Window, kRefreshTimer);
        UnregisterProductHotkeys();
        RemoveTrayIcon();
        if (IsWindow(Window)) DestroyWindow(Window);
        Window_.store(nullptr, std::memory_order_release);
        ThreadId_.store(0, std::memory_order_release);
    }

    [[nodiscard]] bool AddTrayIcon(HWND Window) noexcept {
        TrayIcon_.cbSize = sizeof(TrayIcon_);
        TrayIcon_.hWnd = Window;
        TrayIcon_.uID = 1;
        TrayIcon_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        TrayIcon_.uCallbackMessage = kTrayMessage;
        TrayIcon_.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
        wcsncpy_s(
            TrayIcon_.szTip, std::size(TrayIcon_.szTip),
            L"DeskLink — Starting", _TRUNCATE);
        TrayIcon_.uVersion = NOTIFYICON_VERSION_4;
        if (!Shell_NotifyIconW(NIM_ADD, &TrayIcon_)) return false;
        (void)Shell_NotifyIconW(NIM_SETVERSION, &TrayIcon_);
        TrayActive_ = true;
        return true;
    }

    void RemoveTrayIcon() noexcept {
        if (!TrayActive_) return;
        (void)Shell_NotifyIconW(NIM_DELETE, &TrayIcon_);
        TrayActive_ = false;
    }

    std::optional<ControlResponse> Send(
        ControlRequestPayload Payload,
        std::chrono::milliseconds Timeout = std::chrono::milliseconds{500}) {
        return Win32ControlPipeClient::Send(
            ControlRequest{++NextRequestId_, std::move(Payload)},
            L"broker", Timeout);
    }

    void Refresh() noexcept {
        try {
            const auto Preferences = PreferencesStore_.Current();
            if (Preferences &&
                !ProductHotkeysMatch(*Preferences)) {
                (void)ApplyProductHotkeys(*Preferences);
            }
            const auto Response = Send(GetStateControlRequest{});
            if (Response && Response->Status == ControlStatus::Ok &&
                Response->State) {
                RuntimeState_ = *Response->State;
                RuntimeStateLoaded_ = true;
            } else {
                RuntimeStateLoaded_ = false;
            }
            UpdateTrayTooltip();
        } catch (...) {
            RuntimeStateLoaded_ = false;
        }
    }

    [[nodiscard]] bool ApplyProductHotkeys(
        const ProductPreferences& Preferences) noexcept {
        std::scoped_lock Lock(HotkeyMutex_);
        UnregisterProductHotkeysLocked();
        FocusHotkey_ = Preferences.FocusPeerHotkey;
        ReturnHotkey_ = Preferences.ReturnLocalHotkey;
        const auto Window = Window_.load(std::memory_order_acquire);
        if (!Window) return false;
        const auto Focus = HotkeyChord(FocusHotkey_);
        const auto Return = HotkeyChord(ReturnHotkey_);
        if (Focus && !RegisterHotKey(
                Window, kFocusPeerHotkeyId,
                Focus->Modifiers, Focus->Key)) {
            std::cerr
                << "[Broker:Hotkey] saved focus shortcut is owned by another application\n";
            return false;
        }
        FocusHotkeyRegistered_ = Focus.has_value();
        if (Return && !RegisterHotKey(
                Window, kReturnLocalHotkeyId,
                Return->Modifiers, Return->Key)) {
            UnregisterProductHotkeysLocked();
            std::cerr
                << "[Broker:Hotkey] saved return-local shortcut is owned by another application\n";
            return false;
        }
        ReturnHotkeyRegistered_ = Return.has_value();
        HotkeysApplied_ = true;
        return true;
    }

    [[nodiscard]] bool ProductHotkeysMatch(
        const ProductPreferences& Preferences) noexcept {
        std::scoped_lock Lock(HotkeyMutex_);
        return HotkeysApplied_ &&
            Preferences.FocusPeerHotkey == FocusHotkey_ &&
            Preferences.ReturnLocalHotkey == ReturnHotkey_;
    }

    void UnregisterProductHotkeys() noexcept {
        std::scoped_lock Lock(HotkeyMutex_);
        UnregisterProductHotkeysLocked();
    }

    void UnregisterProductHotkeysLocked() noexcept {
        const auto Window = Window_.load(std::memory_order_acquire);
        if (Window && FocusHotkeyRegistered_) {
            (void)UnregisterHotKey(Window, kFocusPeerHotkeyId);
        }
        if (Window && ReturnHotkeyRegistered_) {
            (void)UnregisterHotKey(Window, kReturnLocalHotkeyId);
        }
        FocusHotkeyRegistered_ = false;
        ReturnHotkeyRegistered_ = false;
        HotkeysApplied_ = false;
    }

    void UpdateTrayTooltip() noexcept {
        if (!TrayActive_) return;
        const auto State = RuntimeStateLoaded_
            ? StateFromControl(RuntimeState_) : ProductShellState::Offline;
        const auto Presentation = PresentProductShellState(State);
        std::wstring Tooltip(L"DeskLink — ");
        Tooltip.append(Presentation.Badge);
        wcsncpy_s(
            TrayIcon_.szTip, std::size(TrayIcon_.szTip),
            Tooltip.c_str(), _TRUNCATE);
        (void)Shell_NotifyIconW(NIM_MODIFY, &TrayIcon_);
    }

    std::optional<ControlTrustedDevice> PreferredDevice(
        const ProductPreferences& Preferences) {
        if (!Preferences.PreferredPeerMachine) return std::nullopt;
        const auto Response = Send(ListTrustedDevicesControlRequest{});
        if (!Response || Response->Status != ControlStatus::Ok ||
            !Response->TrustedDevices) {
            return std::nullopt;
        }
        for (const auto& Device : Response->TrustedDevices->Devices) {
            if (Device.Machine == *Preferences.PreferredPeerMachine) {
                return Device;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] bool OpenProductShell() noexcept {
        if (IsWin32DeskLinkLifecycleOperationActive() ||
            !IsSafeWin32ProductFile(ProductShellExecutable_)) {
            return false;
        }
        if (const auto Window = FindWindowExW(
                HWND_MESSAGE, nullptr, kUiWindowClass, nullptr)) {
            return PostMessageW(Window, GetActivateMessage(), 0, 0) != FALSE;
        }
        auto CommandLine = L"\"" + ProductShellExecutable_.native() + L"\"";
        STARTUPINFOW Startup{sizeof(Startup)};
        PROCESS_INFORMATION Process{};
        const auto WorkingDirectory =
            ProductShellExecutable_.parent_path().native();
        if (!CreateProcessW(
                ProductShellExecutable_.c_str(), CommandLine.data(),
                nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT,
                nullptr, WorkingDirectory.c_str(), &Startup, &Process)) {
            return false;
        }
        CloseHandle(Process.hThread);
        CloseHandle(Process.hProcess);
        return true;
    }

    void FocusPreferredPeer() noexcept {
        const auto Preferences = PreferencesStore_.Current();
        if (!Preferences || !Preferences->PreferredPeerMachine) return;
        const auto Response = Send(FocusMachineControlRequest{
            *Preferences->PreferredPeerMachine});
        if (!Response || Response->Status != ControlStatus::Ok) {
            std::cerr << "[Broker:Tray] preferred-peer focus request failed\n";
        }
        Refresh();
    }

    void ReturnLocal() noexcept {
        const auto Response = Send(ReturnLocalControlRequest{});
        if (!Response || Response->Status != ControlStatus::Ok) {
            std::cerr << "[Broker:Tray] return-local request failed\n";
        }
        Refresh();
    }

    void TogglePause() noexcept {
        Refresh();
        const bool Paused = RuntimeStateLoaded_ &&
            RuntimeState_.RuntimePhase == BrokerRuntimePhase::Paused;
        const auto Response = Paused
            ? Send(ResumeDeskLinkControlRequest{})
            : Send(PauseDeskLinkControlRequest{});
        if (!Response || Response->Status != ControlStatus::Ok) {
            std::cerr << "[Broker:Tray] pause state change failed\n";
        }
        Refresh();
    }

    void ToggleClipboard() noexcept {
        auto Preferences = PreferencesStore_.Current();
        if (!Preferences) return;
        const auto Device = PreferredDevice(*Preferences);
        if (!Device) return;
        const bool Desired = !Preferences->ClipboardDesired;
        if (Desired && !CanEnableClipboardIntent(Device->Capabilities)) {
            (void)OpenProductShell();
            return;
        }
        Preferences->ClipboardDesired = Desired;
        const auto Response = Send(SetProductPreferencesControlRequest{
            *Preferences}, std::chrono::seconds(5));
        if (!Response || Response->Status != ControlStatus::Ok) {
            std::cerr << "[Broker:Tray] clipboard setting change failed\n";
        }
        Refresh();
    }

    void ToggleAudioMute() noexcept {
        const auto Response = Send(ToggleAudioMuteControlRequest{});
        if (!Response || Response->Status != ControlStatus::Ok) {
            std::cerr << "[Broker:Tray] audio mute change failed\n";
        }
        Refresh();
    }

    void ShowTrayMenu() noexcept {
        Refresh();
        const auto Preferences = PreferencesStore_.Current();
        const auto Device = Preferences
            ? PreferredDevice(*Preferences) : std::nullopt;
        const auto Menu = CreatePopupMenu();
        if (!Menu) return;
        const auto State = RuntimeStateLoaded_
            ? StateFromControl(RuntimeState_) : ProductShellState::Offline;
        const auto Presentation = PresentProductShellState(State);
        std::wstring StateLabel(L"Status: ");
        StateLabel.append(Presentation.Badge);
        (void)AppendMenuW(
            Menu, MF_STRING | MF_DISABLED, 0, StateLabel.c_str());
        if (Preferences && Device) {
            const auto PeerName = Utf8ToWide(Device->DisplayName)
                .value_or(L"paired PC");
            std::wstring FocusLabel(L"Focus ");
            FocusLabel.append(PeerName);
            (void)AppendMenuW(
                Menu,
                MF_STRING |
                    (RuntimeStateLoaded_ &&
                     RuntimeState_.ConnectedPeerCount != 0
                        ? MF_ENABLED : MF_GRAYED),
                kTrayFocusPeer, FocusLabel.c_str());
            std::wstring ClipboardLabel = Preferences->ClipboardDesired
                ? L"Turn off clipboard with " : L"Turn on clipboard with ";
            ClipboardLabel.append(PeerName);
            (void)AppendMenuW(
                Menu, MF_STRING, kTrayClipboard, ClipboardLabel.c_str());
            std::wstring AudioLabel = RuntimeStateLoaded_ &&
                    RuntimeState_.AudioMuted
                ? L"Unmute " : L"Mute ";
            AudioLabel.append(PeerName);
            AudioLabel += L" audio (" + std::to_wstring(
                Preferences->AudioGainPermyriad / 100u) + L"%)";
            (void)AppendMenuW(
                Menu,
                MF_STRING |
                    (RuntimeStateLoaded_ &&
                     RuntimeState_.ConnectedPeerCount != 0
                        ? MF_ENABLED : MF_GRAYED),
                kTrayAudioMute, AudioLabel.c_str());
        }
        (void)AppendMenuW(Menu, MF_SEPARATOR, 0, nullptr);
        (void)AppendMenuW(Menu, MF_STRING, kTrayOpen, L"Open DeskLink");
        (void)AppendMenuW(
            Menu, MF_STRING, kTrayReturnLocal, L"Return to this PC");
        (void)AppendMenuW(
            Menu, MF_STRING, kTrayPause,
            State == ProductShellState::Paused
                ? L"Resume DeskLink" : L"Pause DeskLink");
        (void)AppendMenuW(Menu, MF_SEPARATOR, 0, nullptr);
        (void)AppendMenuW(Menu, MF_STRING, kTrayExit, L"Exit DeskLink");

        POINT Cursor{};
        (void)GetCursorPos(&Cursor);
        const auto Window = Window_.load(std::memory_order_acquire);
        (void)SetForegroundWindow(Window);
        const auto Command = TrackPopupMenu(
            Menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
            Cursor.x, Cursor.y, 0, Window, nullptr);
        DestroyMenu(Menu);
        switch (Command) {
            case kTrayOpen: (void)OpenProductShell(); break;
            case kTrayFocusPeer: FocusPreferredPeer(); break;
            case kTrayClipboard: ToggleClipboard(); break;
            case kTrayAudioMute: ToggleAudioMute(); break;
            case kTrayReturnLocal: ReturnLocal(); break;
            case kTrayPause: TogglePause(); break;
            case kTrayExit:
                if (RequestExit_) RequestExit_();
                break;
            default: break;
        }
    }

    std::filesystem::path ProductShellExecutable_;
    Win32ProductPreferencesStore& PreferencesStore_;
    std::function<void()> RequestExit_;
    std::thread Thread_;
    std::mutex LifecycleMutex_;
    std::mutex HotkeyMutex_;
    std::condition_variable LifecycleChanged_;
    std::atomic<HWND> Window_{};
    std::atomic<DWORD> ThreadId_{};
    NOTIFYICONDATAW TrayIcon_{};
    ControlState RuntimeState_{};
    std::uint64_t NextRequestId_{0xD500'0000u};
    ProductHotkey FocusHotkey_{ProductHotkey::Off};
    ProductHotkey ReturnHotkey_{ProductHotkey::Off};
    bool StartCompleted_{};
    bool StartSucceeded_{};
    bool TrayActive_{};
    bool RuntimeStateLoaded_{};
    bool FocusHotkeyRegistered_{};
    bool ReturnHotkeyRegistered_{};
    bool HotkeysApplied_{};
};

Win32BackgroundShell::Win32BackgroundShell(
    std::filesystem::path ProductShellExecutable,
    Win32ProductPreferencesStore& PreferencesStore,
    std::function<void()> RequestExit)
    : Implementation_(std::make_unique<Implementation>(
          std::move(ProductShellExecutable), PreferencesStore,
          std::move(RequestExit))) {}

Win32BackgroundShell::~Win32BackgroundShell() = default;

bool Win32BackgroundShell::Start() {
    return Implementation_ && Implementation_->Start();
}

void Win32BackgroundShell::Stop() noexcept {
    if (Implementation_) Implementation_->Stop();
}

bool Win32BackgroundShell::ApplyPreferences(
    const ProductPreferences& Preferences) noexcept {
    return Implementation_ && Implementation_->ApplyPreferences(Preferences);
}

void Win32BackgroundShell::PreferencesChanged() noexcept {
    if (Implementation_) Implementation_->PreferencesChanged();
}

} // namespace desklink

#endif
