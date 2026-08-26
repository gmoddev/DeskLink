#ifdef _WIN32

#include "desklink/win32_product_lifecycle.hpp"

#include "desklink/control.hpp"
#include "desklink/win32_control.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <memory>
#include <string>

namespace desklink {
namespace {

constexpr wchar_t kInstallerMutexName[] = L"Local\\DeskLink.Install.v1";
constexpr wchar_t kUpdateMutexName[] = L"Local\\DeskLink.Update.v1";
constexpr wchar_t kBrokerMutexName[] = L"Local\\DeskLink.RuntimeBroker.v1";

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

bool MutexIsOpen(const wchar_t* Name) noexcept {
    SetLastError(ERROR_SUCCESS);
    const auto Handle = TakeHandle(OpenMutexW(SYNCHRONIZE, FALSE, Name));
    return Handle || GetLastError() != ERROR_FILE_NOT_FOUND;
}

bool MutexCanBeOpened(const wchar_t* Name) noexcept {
    return static_cast<bool>(
        TakeHandle(OpenMutexW(SYNCHRONIZE, FALSE, Name)));
}

bool ProbeBroker() noexcept {
    static std::atomic_uint64_t RequestId{0xD100'0000u};
    const auto Response = Win32ControlPipeClient::Send(
        ControlRequest{++RequestId, GetStateControlRequest{}},
        L"broker", std::chrono::milliseconds{100});
    return Response && Response->Status == ControlStatus::Ok &&
           Response->State;
}

} // namespace

bool IsWin32DeskLinkLifecycleOperationActive() noexcept {
    return MutexIsOpen(kInstallerMutexName) || MutexIsOpen(kUpdateMutexName);
}

bool IsWin32DeskLinkUpdateValidationActive() noexcept {
    return MutexCanBeOpened(kUpdateMutexName) &&
           !MutexIsOpen(kInstallerMutexName);
}

bool IsSafeWin32ProductFile(
    const std::filesystem::path& Path) noexcept {
    if (Path.empty() || !Path.is_absolute()) return false;
    const auto Attributes = GetFileAttributesW(Path.c_str());
    return Attributes != INVALID_FILE_ATTRIBUTES &&
           (Attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
           (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

std::optional<std::filesystem::path> GetWin32ProductShellExecutable(
    const std::filesystem::path& SiblingExecutable) {
    if (SiblingExecutable.empty() || !SiblingExecutable.is_absolute()) {
        return std::nullopt;
    }
    const auto Shell = SiblingExecutable.parent_path() / L"desklink.exe";
    return IsSafeWin32ProductFile(Shell)
        ? std::optional(Shell) : std::nullopt;
}

Win32BrokerLaunchStatus EnsureWin32RuntimeBroker(
    const std::filesystem::path& FrontendExecutable,
    std::chrono::milliseconds Timeout) {
    if (ProbeBroker()) return Win32BrokerLaunchStatus::Ready;
    if (IsWin32DeskLinkLifecycleOperationActive()) {
        return Win32BrokerLaunchStatus::LifecycleBlocked;
    }
    if (FrontendExecutable.empty() || !FrontendExecutable.is_absolute() ||
        Timeout <= std::chrono::milliseconds::zero()) {
        return Win32BrokerLaunchStatus::BrokerUnavailable;
    }
    const auto BrokerPath =
        FrontendExecutable.parent_path() / L"desklink_runtime.exe";
    if (!IsSafeWin32ProductFile(BrokerPath)) {
        return Win32BrokerLaunchStatus::BrokerUnavailable;
    }

    const bool BrokerStarting = MutexIsOpen(kBrokerMutexName);
    UniqueHandle Process;
    if (!BrokerStarting) {
        auto MutableCommandLine = L"\"" + BrokerPath.native() + L"\"";
        STARTUPINFOW Startup{sizeof(Startup)};
        PROCESS_INFORMATION Information{};
        const auto WorkingDirectory = BrokerPath.parent_path().native();
        if (!CreateProcessW(
                BrokerPath.c_str(), MutableCommandLine.data(), nullptr,
                nullptr, FALSE,
                CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                nullptr, WorkingDirectory.c_str(), &Startup, &Information)) {
            return Win32BrokerLaunchStatus::BrokerUnavailable;
        }
        CloseHandle(Information.hThread);
        Process = TakeHandle(Information.hProcess);
    }

    const auto Deadline = std::chrono::steady_clock::now() + Timeout;
    while (std::chrono::steady_clock::now() < Deadline) {
        if (ProbeBroker()) {
            return BrokerStarting ? Win32BrokerLaunchStatus::Ready
                                  : Win32BrokerLaunchStatus::Started;
        }
        if (Process &&
            WaitForSingleObject(Process.get(), 25) == WAIT_OBJECT_0) {
            return Win32BrokerLaunchStatus::BrokerUnavailable;
        }
        Sleep(25);
    }
    return Win32BrokerLaunchStatus::BrokerUnavailable;
}

} // namespace desklink

#endif
