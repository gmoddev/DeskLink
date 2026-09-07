#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <wtsapi32.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr wchar_t kServiceName[] = L"DeskLinkSecureInputRnd";
constexpr wchar_t kHelperName[] = L"desklink_secure_input_helper.exe";
constexpr DWORD kControlDefaultReleaseProbe = 128;
constexpr DWORD kControlSecureCancelProbe = 129;

SERVICE_STATUS_HANDLE ServiceStatusHandle{};
SERVICE_STATUS ServiceStatus{};
HANDLE StopEvent{};

void Log(std::wstring_view Message) {
    std::wstring Line = L"[SecureInput:Service] ";
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

std::wstring ModulePath() {
    std::array<wchar_t, 32'768> Buffer{};
    const DWORD Length = GetModuleFileNameW(
        nullptr, Buffer.data(), static_cast<DWORD>(Buffer.size()));
    if (Length == 0 || Length >= Buffer.size()) return {};
    return std::wstring(Buffer.data(), Length);
}

std::wstring ParentDirectory(std::wstring_view Path) {
    const auto Separator = Path.find_last_of(L"\\/");
    if (Separator == std::wstring_view::npos) return {};
    return std::wstring(Path.substr(0, Separator));
}

bool IsPathUnderProgramFiles(std::wstring_view Path) {
    PWSTR ProgramFilesRaw{};
    if (FAILED(SHGetKnownFolderPath(
            FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr,
            &ProgramFilesRaw)) || !ProgramFilesRaw) {
        return false;
    }
    std::wstring ProgramFiles(ProgramFilesRaw);
    CoTaskMemFree(ProgramFilesRaw);
    if (Path.size() <= ProgramFiles.size() ||
        _wcsnicmp(Path.data(), ProgramFiles.c_str(), ProgramFiles.size()) != 0) {
        return false;
    }
    const wchar_t Boundary = Path[ProgramFiles.size()];
    return Boundary == L'\\' || Boundary == L'/';
}

bool EnablePrivilege(const wchar_t* Name) noexcept {
    HANDLE Token{};
    if (!OpenProcessToken(GetCurrentProcess(),
            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &Token)) {
        return false;
    }
    LUID Luid{};
    if (!LookupPrivilegeValueW(nullptr, Name, &Luid)) {
        CloseHandle(Token);
        return false;
    }
    TOKEN_PRIVILEGES Privileges{};
    Privileges.PrivilegeCount = 1;
    Privileges.Privileges[0].Luid = Luid;
    Privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    SetLastError(ERROR_SUCCESS);
    const BOOL Adjusted = AdjustTokenPrivileges(
        Token, FALSE, &Privileges, 0, nullptr, nullptr);
    const DWORD Error = GetLastError();
    CloseHandle(Token);
    return Adjusted && Error == ERROR_SUCCESS;
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

bool LaunchFixedProbe(bool SecureDesktop) {
    if (!IsLocalSystem()) {
        Log(L"probe refused because the service is not LocalSystem");
        return false;
    }

    const auto ServicePath = ModulePath();
    const auto Directory = ParentDirectory(ServicePath);
    if (Directory.empty() || !IsPathUnderProgramFiles(ServicePath)) {
        Log(L"probe refused because the service is outside Program Files");
        return false;
    }
    const auto HelperPath = Directory + L"\\" + kHelperName;
    const DWORD Attributes = GetFileAttributesW(HelperPath.c_str());
    if (Attributes == INVALID_FILE_ATTRIBUTES ||
        (Attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        Log(L"probe refused because the fixed helper is missing or unsafe");
        return false;
    }

    DWORD SessionId = WTSGetActiveConsoleSessionId();
    if (SessionId == 0xffffffffu) {
        Log(L"probe refused because there is no active console session");
        return false;
    }
    if (!IsSessionUnlocked(SessionId)) {
        Log(L"probe refused because the active session is locked or unknown");
        return false;
    }
    if (!EnablePrivilege(SE_ASSIGNPRIMARYTOKEN_NAME) ||
        !EnablePrivilege(SE_INCREASE_QUOTA_NAME) ||
        !EnablePrivilege(SE_TCB_NAME)) {
        Log(L"probe refused because required service privileges are unavailable");
        return false;
    }

    HANDLE ProcessToken{};
    HANDLE SessionToken{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY,
            &ProcessToken) ||
        !DuplicateTokenEx(ProcessToken, TOKEN_ALL_ACCESS, nullptr,
            SecurityImpersonation, TokenPrimary, &SessionToken)) {
        if (ProcessToken) CloseHandle(ProcessToken);
        Log(L"probe refused because a SYSTEM primary token could not be created");
        return false;
    }
    CloseHandle(ProcessToken);
    if (!SetTokenInformation(SessionToken, TokenSessionId,
            &SessionId, sizeof(SessionId))) {
        CloseHandle(SessionToken);
        Log(L"probe refused because the SYSTEM token could not be session-bound");
        return false;
    }

    const wchar_t* Desktop = SecureDesktop
        ? L"winsta0\\winlogon" : L"winsta0\\default";
    const wchar_t* Operation = SecureDesktop
        ? L"secure-cancel" : L"default-release";
    std::wstring CommandLine = L"\"" + HelperPath +
        L"\" --service-probe " + Operation;
    std::vector<wchar_t> MutableCommand(
        CommandLine.begin(), CommandLine.end());
    MutableCommand.push_back(L'\0');

    STARTUPINFOW Startup{};
    Startup.cb = sizeof(Startup);
    Startup.lpDesktop = const_cast<wchar_t*>(Desktop);
    PROCESS_INFORMATION Process{};
    const BOOL Created = CreateProcessAsUserW(
        SessionToken, HelperPath.c_str(), MutableCommand.data(), nullptr,
        nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
        nullptr, Directory.c_str(), &Startup, &Process);
    CloseHandle(SessionToken);
    if (!Created) {
        Log(L"fixed helper launch failed closed");
        return false;
    }
    CloseHandle(Process.hThread);
    const DWORD Wait = WaitForSingleObject(Process.hProcess, 5'000);
    DWORD ExitCode = ERROR_PROCESS_ABORTED;
    bool Completed = Wait == WAIT_OBJECT_0 &&
        GetExitCodeProcess(Process.hProcess, &ExitCode) && ExitCode == 0;
    if (Wait == WAIT_TIMEOUT) {
        // The helper is a fixed, single-operation child. A hung probe must not
        // retain SYSTEM authority after its bounded control request expires.
        (void)TerminateProcess(Process.hProcess, ERROR_TIMEOUT);
        (void)WaitForSingleObject(Process.hProcess, 1'000);
        Completed = false;
    }
    CloseHandle(Process.hProcess);
    if (!Completed) {
        Log(L"fixed helper failed or timed out; probe failed closed");
        return false;
    }
    Log(SecureDesktop
        ? L"secure-desktop cancel probe completed"
        : L"Default-desktop release probe completed");
    return true;
}

void ReportServiceStatus(DWORD State, DWORD Error = ERROR_SUCCESS) noexcept {
    ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    ServiceStatus.dwCurrentState = State;
    ServiceStatus.dwControlsAccepted = State == SERVICE_RUNNING
        ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    ServiceStatus.dwWin32ExitCode = Error;
    ServiceStatus.dwCheckPoint = 0;
    ServiceStatus.dwWaitHint = 0;
    if (ServiceStatusHandle) {
        SetServiceStatus(ServiceStatusHandle, &ServiceStatus);
    }
}

DWORD WINAPI ServiceControlHandler(
    DWORD Control, DWORD, void*, void*) noexcept {
    switch (Control) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            ReportServiceStatus(SERVICE_STOP_PENDING);
            if (StopEvent) SetEvent(StopEvent);
            return NO_ERROR;
        case kControlDefaultReleaseProbe:
            return LaunchFixedProbe(false) ? NO_ERROR : ERROR_ACCESS_DENIED;
        case kControlSecureCancelProbe:
            return LaunchFixedProbe(true) ? NO_ERROR : ERROR_ACCESS_DENIED;
        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void WINAPI ServiceMain(DWORD, wchar_t**) noexcept {
    ServiceStatusHandle = RegisterServiceCtrlHandlerExW(
        kServiceName, ServiceControlHandler, nullptr);
    if (!ServiceStatusHandle) return;
    ReportServiceStatus(SERVICE_START_PENDING);
    if (!IsLocalSystem() || !IsPathUnderProgramFiles(ModulePath())) {
        ReportServiceStatus(SERVICE_STOPPED, ERROR_ACCESS_DENIED);
        return;
    }
    StopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!StopEvent) {
        ReportServiceStatus(SERVICE_STOPPED, GetLastError());
        return;
    }
    ReportServiceStatus(SERVICE_RUNNING);
    Log(L"validation-only service active; networking and product IPC are absent");
    WaitForSingleObject(StopEvent, INFINITE);
    CloseHandle(StopEvent);
    StopEvent = nullptr;
    ReportServiceStatus(SERVICE_STOPPED);
}

int SelfTest() {
    const auto Path = ModulePath();
    if (Path.empty() || ParentDirectory(Path).empty()) return 1;
    Log(L"self-test passed; privileged roles remain inactive");
    return 0;
}

} // namespace

int wmain(int ArgumentCount, wchar_t** Arguments) {
    if (ArgumentCount == 2 &&
        std::wstring_view(Arguments[1]) == L"--self-test") {
        return SelfTest();
    }
    if (ArgumentCount != 1) {
        std::wcerr << L"[SecureInput:Service] unsupported argument\n";
        return 2;
    }
    SERVICE_TABLE_ENTRYW Table[] = {
        {const_cast<wchar_t*>(kServiceName), ServiceMain},
        {nullptr, nullptr},
    };
    if (!StartServiceCtrlDispatcherW(Table)) {
        std::wcerr
            << L"[SecureInput:Service] SCM launch required; error="
            << GetLastError() << L'\n';
        return 1;
    }
    return ServiceStatus.dwWin32ExitCode == ERROR_SUCCESS ? 0 : 1;
}
