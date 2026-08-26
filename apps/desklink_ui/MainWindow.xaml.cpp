#include "pch.h"
#include "MainWindow.xaml.h"
#include "MainWindow.g.cpp"

namespace {

constexpr wchar_t kLifecycleWindowClass[] =
    L"DeskLinkShellLifecycleWindow.v1";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kTrayOpen = 1;
constexpr UINT kTrayReturnLocal = 2;
constexpr UINT kTrayPause = 3;
constexpr UINT kTrayExit = 4;

UINT GetActivateMessage() noexcept {
    static const UINT Message =
        RegisterWindowMessageW(L"DeskLink.ActivateShell.v1");
    return Message;
}

UINT GetExitMessage() noexcept {
    static const UINT Message =
        RegisterWindowMessageW(L"DeskLink.ExitShell.v1");
    return Message;
}

UINT GetPrepareUpdateMessage() noexcept {
    static const UINT Message =
        RegisterWindowMessageW(L"DeskLink.PrepareUpdate.v1");
    return Message;
}

desklink::ProductShellState StateFromTag(std::wstring_view Tag) noexcept {
    if (Tag == L"Offline") return desklink::ProductShellState::Offline;
    if (Tag == L"Connecting") return desklink::ProductShellState::Connecting;
    if (Tag == L"RemoteFocus") return desklink::ProductShellState::RemoteFocus;
    if (Tag == L"ActionRequired") {
        return desklink::ProductShellState::ActionRequired;
    }
    if (Tag == L"Paused") return desklink::ProductShellState::Paused;
    return desklink::ProductShellState::ConnectedLocal;
}

} // namespace

namespace winrt::DeskLink::Product::implementation {

MainWindow::MainWindow() {
    InitializeComponent();
    ContentReady_ = true;
    Title(L"DeskLink");
    Navigation().SelectedItem(HomeNavigation());
    if (!CreateLifecycleWindow() || !AddTrayIcon()) {
        throw winrt::hresult_error(E_FAIL, L"Could not initialize DeskLink lifecycle controls.");
    }
    ApplyState(State_);
}

void MainWindow::InitializeWindowLifecycle() {
    const Microsoft::UI::Xaml::Window ProductWindow = *this;
    winrt::check_hresult(
        ProductWindow.as<IWindowNative>()->get_WindowHandle(
            &MainWindowHandle_));
    if (!MainWindowHandle_ || !SetWindowSubclass(
            MainWindowHandle_,
            MainWindowSubclassProcedure,
            1,
            reinterpret_cast<DWORD_PTR>(this))) {
        throw winrt::hresult_error(
            E_UNEXPECTED,
            L"DeskLink window activation did not create a lifecycle window.");
    }
    SetWindowPos(
        MainWindowHandle_,
        nullptr,
        0,
        0,
        1'040,
        720,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

MainWindow::~MainWindow() {
    RemoveTrayIcon();
    if (MainWindowHandle_) {
        RemoveWindowSubclass(
            MainWindowHandle_, MainWindowSubclassProcedure, 1);
    }
    if (LifecycleWindow_) DestroyWindow(LifecycleWindow_);
}

void MainWindow::OnNavigationChanged(
    Microsoft::UI::Xaml::Controls::NavigationView const&,
    Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const& Args) {
    const auto Item = Args.SelectedItemContainer();
    const auto Tag = Item
        ? winrt::unbox_value_or<winrt::hstring>(Item.Tag(), {})
        : winrt::hstring{};
    HomePage().Visibility(Tag == L"Home"
        ? Microsoft::UI::Xaml::Visibility::Visible
        : Microsoft::UI::Xaml::Visibility::Collapsed);
    AdvancedPage().Visibility(Tag == L"Advanced"
        ? Microsoft::UI::Xaml::Visibility::Visible
        : Microsoft::UI::Xaml::Visibility::Collapsed);
    DiagnosticsPage().Visibility(Tag == L"Diagnostics"
        ? Microsoft::UI::Xaml::Visibility::Visible
        : Microsoft::UI::Xaml::Visibility::Collapsed);
}

void MainWindow::OnSimulationChanged(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&) {
    if (!ContentReady_) return;
    const auto Item =
        SimulationState().SelectedItem().try_as<Microsoft::UI::Xaml::Controls::ComboBoxItem>();
    if (!Item) return;
    ApplyState(StateFromTag(
        winrt::unbox_value_or<winrt::hstring>(Item.Tag(), {})));
}

void MainWindow::OnReturnLocal(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    ApplyState(desklink::ProductShellState::ConnectedLocal);
}

void MainWindow::ApplyState(desklink::ProductShellState State) {
    State_ = State;
    const auto Presentation = desklink::PresentProductShellState(State);
    StatusBadge().Text(Presentation.Badge);
    InputTitle().Text(Presentation.KeyboardAndMouseTitle);
    InputSummary().Text(Presentation.KeyboardAndMouseSummary);
    DiagnosticRuntimePhase().Text(Presentation.ConnectionDetail);
    ReturnLocalButton().Visibility(Presentation.ShowReturnLocal
        ? Microsoft::UI::Xaml::Visibility::Visible
        : Microsoft::UI::Xaml::Visibility::Collapsed);
    ActionRequiredBar().IsOpen(Presentation.ShowActionRequired);
    if (TrayActive_) {
        StringCchPrintfW(
            TrayIcon_.szTip,
            ARRAYSIZE(TrayIcon_.szTip),
            L"DeskLink — %.*s",
            static_cast<int>(Presentation.Badge.size()),
            Presentation.Badge.data());
        Shell_NotifyIconW(NIM_MODIFY, &TrayIcon_);
    }
}

bool MainWindow::CreateLifecycleWindow() {
    WNDCLASSEXW Class{sizeof(Class)};
    Class.lpfnWndProc = LifecycleWindowProcedure;
    Class.hInstance = GetModuleHandleW(nullptr);
    Class.lpszClassName = kLifecycleWindowClass;
    if (!RegisterClassExW(&Class) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    LifecycleWindow_ = CreateWindowExW(
        0,
        kLifecycleWindowClass,
        L"DeskLink product shell lifecycle",
        0,
        0,
        0,
        0,
        0,
        HWND_MESSAGE,
        nullptr,
        GetModuleHandleW(nullptr),
        this);
    return LifecycleWindow_ != nullptr;
}

bool MainWindow::AddTrayIcon() {
    TrayIcon_.cbSize = sizeof(TrayIcon_);
    TrayIcon_.hWnd = LifecycleWindow_;
    TrayIcon_.uID = 1;
    TrayIcon_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    TrayIcon_.uCallbackMessage = kTrayMessage;
    TrayIcon_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    StringCchCopyW(TrayIcon_.szTip, ARRAYSIZE(TrayIcon_.szTip),
                   L"DeskLink — Connected");
    TrayIcon_.uVersion = NOTIFYICON_VERSION_4;
    if (!Shell_NotifyIconW(NIM_ADD, &TrayIcon_)) return false;
    Shell_NotifyIconW(NIM_SETVERSION, &TrayIcon_);
    TrayActive_ = true;
    return true;
}

void MainWindow::RemoveTrayIcon() noexcept {
    if (!TrayActive_) return;
    Shell_NotifyIconW(NIM_DELETE, &TrayIcon_);
    TrayActive_ = false;
}

void MainWindow::HideToTray() {
    if (MainWindowHandle_) ShowWindow(MainWindowHandle_, SW_HIDE);
}

void MainWindow::ShowFromTray() {
    if (!MainWindowHandle_) return;
    ShowWindow(MainWindowHandle_, SW_SHOW);
    SetForegroundWindow(MainWindowHandle_);
}

void MainWindow::TogglePaused() {
    ApplyState(State_ == desklink::ProductShellState::Paused
        ? desklink::ProductShellState::ConnectedLocal
        : desklink::ProductShellState::Paused);
}

void MainWindow::ShowTrayMenu() {
    const auto Menu = CreatePopupMenu();
    if (!Menu) return;
    AppendMenuW(Menu, MF_STRING, kTrayOpen, L"Open DeskLink");
    AppendMenuW(Menu, MF_STRING, kTrayReturnLocal, L"Return to this PC");
    AppendMenuW(
        Menu,
        MF_STRING,
        kTrayPause,
        State_ == desklink::ProductShellState::Paused
            ? L"Resume DeskLink"
            : L"Pause DeskLink");
    AppendMenuW(Menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(Menu, MF_STRING, kTrayExit, L"Exit");
    POINT Cursor{};
    GetCursorPos(&Cursor);
    SetForegroundWindow(LifecycleWindow_);
    const auto Command = TrackPopupMenu(
        Menu,
        TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
        Cursor.x,
        Cursor.y,
        0,
        LifecycleWindow_,
        nullptr);
    DestroyMenu(Menu);
    switch (Command) {
        case kTrayOpen: ShowFromTray(); break;
        case kTrayReturnLocal:
            ApplyState(desklink::ProductShellState::ConnectedLocal);
            break;
        case kTrayPause: TogglePaused(); break;
        case kTrayExit: RequestExit(); break;
        default: break;
    }
}

void MainWindow::RequestExit() {
    if (ExplicitExit_) return;
    ExplicitExit_ = true;
    RemoveTrayIcon();
    if (MainWindowHandle_) {
        SendMessageW(MainWindowHandle_, WM_CLOSE, 0, 0);
    }
    Microsoft::UI::Xaml::Application::Current().Exit();
}

LRESULT CALLBACK MainWindow::MainWindowSubclassProcedure(
    HWND Window,
    UINT Message,
    WPARAM WParam,
    LPARAM LParam,
    UINT_PTR,
    DWORD_PTR ReferenceData) {
    const auto Self = reinterpret_cast<MainWindow*>(ReferenceData);
    if (Message == WM_CLOSE && Self && !Self->ExplicitExit_) {
        Self->HideToTray();
        return 0;
    }
    if (Message == WM_NCDESTROY) {
        RemoveWindowSubclass(Window, MainWindowSubclassProcedure, 1);
        if (Self) Self->MainWindowHandle_ = nullptr;
    }
    return DefSubclassProc(Window, Message, WParam, LParam);
}

LRESULT CALLBACK MainWindow::LifecycleWindowProcedure(
    HWND Window, UINT Message, WPARAM WParam, LPARAM LParam) {
    auto Self = reinterpret_cast<MainWindow*>(
        GetWindowLongPtrW(Window, GWLP_USERDATA));
    if (Message == WM_NCCREATE) {
        const auto Create = reinterpret_cast<CREATESTRUCTW*>(LParam);
        Self = static_cast<MainWindow*>(Create->lpCreateParams);
        SetWindowLongPtrW(
            Window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(Self));
    }
    if (!Self) return DefWindowProcW(Window, Message, WParam, LParam);
    if (Message == GetActivateMessage()) {
        Self->ShowFromTray();
        return 0;
    }
    if (Message == GetExitMessage() ||
        Message == GetPrepareUpdateMessage()) {
        Self->RequestExit();
        return 0;
    }
    if (Message == kTrayMessage) {
        switch (LOWORD(LParam)) {
            case WM_LBUTTONDBLCLK:
                Self->ShowFromTray();
                return 0;
            case WM_CONTEXTMENU:
                Self->ShowTrayMenu();
                return 0;
            default:
                break;
        }
    }
    return DefWindowProcW(Window, Message, WParam, LParam);
}

} // namespace winrt::DeskLink::Product::implementation
