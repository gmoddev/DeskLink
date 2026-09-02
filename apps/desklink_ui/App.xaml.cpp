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

std::optional<std::filesystem::path> GetExecutablePath() {
    std::wstring Buffer(512, L'\0');
    for (;;) {
        const auto Length = GetModuleFileNameW(
            nullptr, Buffer.data(), static_cast<DWORD>(Buffer.size()));
        if (Length == 0) return std::nullopt;
        if (Length < Buffer.size() - 1) {
            Buffer.resize(Length);
            return std::filesystem::path(std::move(Buffer));
        }
        if (Buffer.size() >= 32'768) return std::nullopt;
        Buffer.resize(Buffer.size() * 2);
    }
}

bool ValidateInstalledProductShell(
    const std::filesystem::path& Executable) noexcept {
    if (!desklink::IsWin32DeskLinkUpdateValidationActive()) return false;
    const auto Root = Executable.parent_path();
    for (const auto* Relative : {
             L"desklink.exe", L"desklink_alpha.exe", L"desklink_pair.exe",
             L"desklink_runtime.exe", L"desklink_update.exe",
             L"runtime\\schannel\\msquic.dll", L"App.xbf",
             L"MainWindow.xbf", L"Microsoft.WindowsAppRuntime.dll",
             L"Microsoft.ui.xaml.dll", L"WindowsAppSDK-LICENSE.txt"}) {
        if (!desklink::IsSafeWin32ProductFile(Root / Relative)) return false;
    }
#ifdef DESKLINK_EXPERIMENTAL_WINDOWS10
    for (const auto* Relative : {
             L"runtime\\openssl\\msquic.dll",
             L"runtime\\openssl\\libcrypto-3-x64.dll",
             L"runtime\\openssl\\libssl-3-x64.dll"}) {
        if (!desklink::IsSafeWin32ProductFile(Root / Relative)) return false;
    }
#endif
    return true;
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
    const bool ValidateUpdate = HasCommandLineArgument(L"--validate-update");
    const auto Executable = GetExecutablePath();
    if (ValidateUpdate) {
        if (!Executable || !ValidateInstalledProductShell(*Executable)) {
            ExitProcess(ERROR_INVALID_DATA);
        }
    } else if (desklink::IsWin32DeskLinkLifecycleOperationActive()) {
        ExitProcess(ERROR_INSTALL_ALREADY_RUNNING);
    }
    if (IsSecondaryInstance()) {
        if (ValidateUpdate) ExitProcess(ERROR_BUSY);
        RedirectToPrimary();
        Exit();
        return;
    }

    if (!ValidateUpdate && Executable) {
        (void)desklink::EnsureWin32RuntimeBroker(*Executable);
    }

    const bool DeveloperMode = HasCommandLineArgument(L"-dev") ||
        HasCommandLineArgument(L"--dev");
    const auto ProductWindow = winrt::make_self<MainWindow>(DeveloperMode);
    Window_ = *ProductWindow;
    if (!ValidateUpdate) Window_.Activate();
    ProductWindow->InitializeWindowLifecycle();
    if (ValidateUpdate || HasCommandLineArgument(L"--background")) {
        ProductWindow->HideToTray();
    }
    if (ValidateUpdate || HasCommandLineArgument(L"--smoke-test")) {
        ProductWindow->RequestExit();
    }
}

} // namespace winrt::DeskLink::Product::implementation
