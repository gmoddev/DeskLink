#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"

namespace {

constexpr wchar_t kUiMutexName[] = L"Local\\DeskLink.Shell.v1";
constexpr wchar_t kLifecycleWindowClass[] =
    L"DeskLinkShellLifecycleWindow.v1";

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

bool HasCommandLineArgument(std::wstring_view Expected) noexcept {
    int Count{};
    const auto Arguments = CommandLineToArgvW(GetCommandLineW(), &Count);
    if (!Arguments) return false;
    bool Found = false;
    for (int Index = 1; Index < Count; ++Index) {
        if (std::wstring_view(Arguments[Index]) == Expected) {
            Found = true;
            break;
        }
    }
    LocalFree(Arguments);
    return Found;
}

} // namespace

namespace winrt::DeskLink::Product::implementation {

App::App() {
    InitializeComponent();
}

App::~App() {
    if (InstanceMutex_) CloseHandle(InstanceMutex_);
}

bool App::IsSecondaryInstance() noexcept {
    InstanceMutex_ = CreateMutexW(nullptr, FALSE, kUiMutexName);
    return !InstanceMutex_ || GetLastError() == ERROR_ALREADY_EXISTS;
}

void App::RedirectToPrimary() noexcept {
    const bool RequestExit = HasCommandLineArgument(L"--request-exit");
    for (int Attempt = 0; Attempt < 20; ++Attempt) {
        if (const auto Window =
                FindWindowExW(
                    HWND_MESSAGE,
                    nullptr,
                    kLifecycleWindowClass,
                    nullptr)) {
            PostMessageW(
                Window,
                RequestExit ? GetExitMessage() : GetActivateMessage(),
                0,
                0);
            return;
        }
        Sleep(50);
    }
}

void App::OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&) {
    if (IsSecondaryInstance()) {
        RedirectToPrimary();
        Exit();
        return;
    }

    const auto ProductWindow = winrt::make_self<MainWindow>();
    Window_ = *ProductWindow;
    Window_.Activate();
    ProductWindow->InitializeWindowLifecycle();
    if (HasCommandLineArgument(L"--background")) {
        ProductWindow->HideToTray();
    }
    if (HasCommandLineArgument(L"--smoke-test")) {
        ProductWindow->RequestExit();
    }
}

} // namespace winrt::DeskLink::Product::implementation
