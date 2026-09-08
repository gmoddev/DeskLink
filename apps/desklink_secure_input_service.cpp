#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#include <shlobj.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <wtsapi32.h>

#include "desklink/secure_input.hpp"
#include "desklink/secure_input_wire.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using desklink::secure_input_wire::HelperRequest;
using desklink::secure_input_wire::HelperResponse;
using desklink::secure_input_wire::Operation;
using desklink::secure_input_wire::Request;
using desklink::secure_input_wire::Response;
using desklink::secure_input_wire::Status;

constexpr wchar_t kHelperName[] = L"desklink_secure_input_helper.exe";
constexpr wchar_t kRuntimeName[] = L"desklink_pair.exe";

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

class UniqueHandle final {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE Value) noexcept : Value_(Value) {}
    ~UniqueHandle() { Reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& Other) noexcept
        : Value_(Other.Release()) {}
    UniqueHandle& operator=(UniqueHandle&& Other) noexcept {
        if (this != &Other) Reset(Other.Release());
        return *this;
    }
    [[nodiscard]] HANDLE Get() const noexcept { return Value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return Value_ && Value_ != INVALID_HANDLE_VALUE;
    }
    HANDLE Release() noexcept {
        const auto Result = Value_;
        Value_ = nullptr;
        return Result;
    }
    void Reset(HANDLE Value = nullptr) noexcept {
        if (*this) CloseHandle(Value_);
        Value_ = Value;
    }
private:
    HANDLE Value_{};
};

bool IsLocalSystem() noexcept {
    SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
    PSID LocalSystemSid{};
    if (!AllocateAndInitializeSid(
            &NtAuthority, 1, SECURITY_LOCAL_SYSTEM_RID,
            0, 0, 0, 0, 0, 0, 0, &LocalSystemSid)) {
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
    return Length == 0 || Length >= Buffer.size()
        ? std::wstring{} : std::wstring(Buffer.data(), Length);
}

std::wstring ParentDirectory(std::wstring_view Path) {
    const auto Separator = Path.find_last_of(L"\\/");
    return Separator == std::wstring_view::npos
        ? std::wstring{} : std::wstring(Path.substr(0, Separator));
}

bool IsPathUnderProgramFiles(std::wstring_view Path) {
    PWSTR Raw{};
    if (FAILED(SHGetKnownFolderPath(
            FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, &Raw)) || !Raw) {
        return false;
    }
    const std::wstring ProgramFiles(Raw);
    CoTaskMemFree(Raw);
    return Path.size() > ProgramFiles.size() &&
        _wcsnicmp(Path.data(), ProgramFiles.c_str(), ProgramFiles.size()) == 0 &&
        (Path[ProgramFiles.size()] == L'\\' ||
         Path[ProgramFiles.size()] == L'/');
}

bool IsRegularNonReparseFile(const std::wstring& Path) noexcept {
    const DWORD Attributes = GetFileAttributesW(Path.c_str());
    return Attributes != INVALID_FILE_ATTRIBUTES &&
        (Attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
        (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

bool IsNonReparseDirectory(const std::wstring& Path) noexcept {
    const DWORD Attributes = GetFileAttributesW(Path.c_str());
    return Attributes != INVALID_FILE_ATTRIBUTES &&
        (Attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

std::optional<std::array<std::uint8_t, 32>> AuthenticodeSignerHash(
    const std::wstring& Path) noexcept {
    WINTRUST_FILE_INFO FileInfo{};
    FileInfo.cbStruct = sizeof(FileInfo);
    FileInfo.pcwszFilePath = Path.c_str();
    WINTRUST_DATA TrustData{};
    TrustData.cbStruct = sizeof(TrustData);
    TrustData.dwUIChoice = WTD_UI_NONE;
    TrustData.fdwRevocationChecks = WTD_REVOKE_NONE;
    TrustData.dwUnionChoice = WTD_CHOICE_FILE;
    TrustData.pFile = &FileInfo;
    TrustData.dwStateAction = WTD_STATEACTION_VERIFY;
    TrustData.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID Policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const auto Verified = WinVerifyTrust(
        nullptr, &Policy, &TrustData) == ERROR_SUCCESS;
    TrustData.dwStateAction = WTD_STATEACTION_CLOSE;
    (void)WinVerifyTrust(nullptr, &Policy, &TrustData);
    if (!Verified) return std::nullopt;

    HCERTSTORE Store{};
    HCRYPTMSG Message{};
    DWORD Encoding{};
    DWORD Content{};
    DWORD Format{};
    if (!CryptQueryObject(
            CERT_QUERY_OBJECT_FILE, Path.c_str(),
            CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
            CERT_QUERY_FORMAT_FLAG_BINARY, 0, &Encoding, &Content, &Format,
            &Store, &Message, nullptr)) {
        return std::nullopt;
    }
    DWORD SignerBytes{};
    if (!CryptMsgGetParam(
            Message, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &SignerBytes) ||
        SignerBytes == 0 || SignerBytes > 64 * 1024) {
        CryptMsgClose(Message);
        CertCloseStore(Store, 0);
        return std::nullopt;
    }
    std::vector<std::uint8_t> Buffer(SignerBytes);
    if (!CryptMsgGetParam(
            Message, CMSG_SIGNER_INFO_PARAM, 0, Buffer.data(), &SignerBytes)) {
        CryptMsgClose(Message);
        CertCloseStore(Store, 0);
        return std::nullopt;
    }
    const auto* Signer = reinterpret_cast<const CMSG_SIGNER_INFO*>(
        Buffer.data());
    CERT_INFO Find{};
    Find.Issuer = Signer->Issuer;
    Find.SerialNumber = Signer->SerialNumber;
    PCCERT_CONTEXT Certificate = CertFindCertificateInStore(
        Store, Encoding, 0, CERT_FIND_SUBJECT_CERT, &Find, nullptr);
    std::array<std::uint8_t, 32> Result{};
    DWORD HashBytes = static_cast<DWORD>(Result.size());
    const bool HasHash = Certificate && CertGetCertificateContextProperty(
        Certificate, CERT_SHA256_HASH_PROP_ID, Result.data(), &HashBytes) &&
        HashBytes == Result.size();
    if (Certificate) CertFreeCertificateContext(Certificate);
    CryptMsgClose(Message);
    CertCloseStore(Store, 0);
    return HasHash ? std::optional(Result) : std::nullopt;
}

bool EnablePrivilege(const wchar_t* Name) noexcept {
    UniqueHandle Token;
    HANDLE Raw{};
    if (!OpenProcessToken(GetCurrentProcess(),
            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &Raw)) return false;
    Token.Reset(Raw);
    LUID Luid{};
    if (!LookupPrivilegeValueW(nullptr, Name, &Luid)) return false;
    TOKEN_PRIVILEGES Privileges{};
    Privileges.PrivilegeCount = 1;
    Privileges.Privileges[0].Luid = Luid;
    Privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    SetLastError(ERROR_SUCCESS);
    return AdjustTokenPrivileges(
        Token.Get(), FALSE, &Privileges, 0, nullptr, nullptr) &&
        GetLastError() == ERROR_SUCCESS;
}

bool IsSessionUnlocked(DWORD SessionId) noexcept {
    LPWSTR Buffer{};
    DWORD Bytes{};
    if (!WTSQuerySessionInformationW(
            WTS_CURRENT_SERVER_HANDLE, SessionId, WTSSessionInfoEx,
            &Buffer, &Bytes) || !Buffer || Bytes < sizeof(WTSINFOEXW)) {
        if (Buffer) WTSFreeMemory(Buffer);
        return false;
    }
    const auto* Info = reinterpret_cast<const WTSINFOEXW*>(Buffer);
    const bool Unlocked = Info->Level == 1 &&
        Info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_UNLOCK;
    WTSFreeMemory(Buffer);
    return Unlocked;
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

struct ProtectedGrant {
    desklink::MachineId PeerMachine{};
    desklink::CertificateDerHash PeerCertificateDerHash{};
    std::uint64_t PolicyRevision{};
    bool Enabled{};
};

bool ReadRegistryBinary(
    HKEY Key, const wchar_t* Name, void* Output, DWORD ExpectedBytes) noexcept {
    DWORD Type{};
    DWORD Bytes = ExpectedBytes;
    return RegQueryValueExW(
               Key, Name, nullptr, &Type, static_cast<BYTE*>(Output),
               &Bytes) == ERROR_SUCCESS &&
        Type == REG_BINARY && Bytes == ExpectedBytes;
}

std::optional<ProtectedGrant> LoadProtectedGrant() noexcept {
    HKEY Key{};
    if (RegOpenKeyExW(
            HKEY_LOCAL_MACHINE, desklink::secure_input_wire::kRegistryPath, 0,
            KEY_QUERY_VALUE | KEY_WOW64_64KEY, &Key) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    ProtectedGrant Result;
    DWORD Enabled{};
    DWORD EnabledType{};
    DWORD EnabledBytes = sizeof(Enabled);
    ULONGLONG Revision{};
    DWORD RevisionType{};
    DWORD RevisionBytes = sizeof(Revision);
    const bool Valid = RegQueryValueExW(
            Key, L"Enabled", nullptr, &EnabledType,
            reinterpret_cast<BYTE*>(&Enabled), &EnabledBytes) == ERROR_SUCCESS &&
        EnabledType == REG_DWORD && EnabledBytes == sizeof(Enabled) &&
        Enabled == 1 &&
        ReadRegistryBinary(Key, L"PeerMachine", Result.PeerMachine.data(), 16) &&
        ReadRegistryBinary(
            Key, L"PeerCertificateDerHash",
            Result.PeerCertificateDerHash.data(), 32) &&
        RegQueryValueExW(
            Key, L"Revision", nullptr, &RevisionType,
            reinterpret_cast<BYTE*>(&Revision), &RevisionBytes) == ERROR_SUCCESS &&
        RevisionType == REG_QWORD && RevisionBytes == sizeof(Revision) &&
        Revision != 0 &&
        std::equal(Result.PeerMachine.begin(), Result.PeerMachine.end(),
            Result.PeerCertificateDerHash.begin());
    RegCloseKey(Key);
    if (!Valid) return std::nullopt;
    Result.Enabled = true;
    Result.PolicyRevision = Revision;
    return Result;
}

class HelperChannel final {
public:
    HelperChannel() = default;
    ~HelperChannel() { Reset(); }
    HelperChannel(const HelperChannel&) = delete;
    HelperChannel& operator=(const HelperChannel&) = delete;

    bool Start(
        const std::wstring& HelperPath, const std::wstring& Directory,
        DWORD SessionId, bool SecureDesktop) noexcept {
        Reset();
        SECURITY_ATTRIBUTES Security{};
        Security.nLength = sizeof(Security);
        Security.bInheritHandle = TRUE;
        HANDLE ChildRead{};
        HANDLE ParentWrite{};
        HANDLE ParentRead{};
        HANDLE ChildWrite{};
        if (!CreatePipe(&ChildRead, &ParentWrite, &Security, 4096) ||
            !CreatePipe(&ParentRead, &ChildWrite, &Security, 4096)) {
            if (ChildRead) CloseHandle(ChildRead);
            if (ParentWrite) CloseHandle(ParentWrite);
            if (ParentRead) CloseHandle(ParentRead);
            if (ChildWrite) CloseHandle(ChildWrite);
            return false;
        }
        UniqueHandle ChildReadOwner(ChildRead);
        UniqueHandle ChildWriteOwner(ChildWrite);
        ParentWrite_.Reset(ParentWrite);
        ParentRead_.Reset(ParentRead);
        if (!SetHandleInformation(
                ParentWrite_.Get(), HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(ParentRead_.Get(), HANDLE_FLAG_INHERIT, 0)) {
            Reset();
            return false;
        }

        HANDLE ProcessTokenRaw{};
        HANDLE SessionTokenRaw{};
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY,
                &ProcessTokenRaw)) return false;
        UniqueHandle ProcessToken(ProcessTokenRaw);
        if (!DuplicateTokenEx(
                ProcessToken.Get(), TOKEN_ALL_ACCESS, nullptr,
                SecurityImpersonation, TokenPrimary, &SessionTokenRaw)) {
            return false;
        }
        UniqueHandle SessionToken(SessionTokenRaw);
        if (!SetTokenInformation(
                SessionToken.Get(), TokenSessionId, &SessionId,
                sizeof(SessionId))) return false;

        const auto ReadValue = reinterpret_cast<std::uintptr_t>(
            ChildReadOwner.Get());
        const auto WriteValue = reinterpret_cast<std::uintptr_t>(
            ChildWriteOwner.Get());
        std::wstring CommandLine = L"\"" + HelperPath + L"\" --broker " +
            std::to_wstring(ReadValue) + L" " + std::to_wstring(WriteValue) +
            (SecureDesktop ? L" winlogon" : L" default");
        std::vector<wchar_t> Mutable(
            CommandLine.begin(), CommandLine.end());
        Mutable.push_back(L'\0');
        STARTUPINFOW Startup{};
        Startup.cb = sizeof(Startup);
        Startup.lpDesktop = const_cast<wchar_t*>(SecureDesktop
            ? L"winsta0\\winlogon" : L"winsta0\\default");
        PROCESS_INFORMATION Process{};
        if (!CreateProcessAsUserW(
                SessionToken.Get(), HelperPath.c_str(), Mutable.data(),
                nullptr, nullptr, TRUE,
                CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                nullptr, Directory.c_str(), &Startup, &Process)) {
            Reset();
            return false;
        }
        CloseHandle(Process.hThread);
        Process_.Reset(Process.hProcess);
        ChildReadOwner.Reset();
        ChildWriteOwner.Reset();
        return true;
    }

    Status Send(Operation RequestedOperation,
                const std::array<std::uint8_t, 72>& Payload) noexcept {
        if (!Process_ || WaitForSingleObject(Process_.Get(), 0) != WAIT_TIMEOUT) {
            return Status::DesktopUnavailable;
        }
        HelperRequest Request;
        Request.RequestedOperation = RequestedOperation;
        Request.Payload = Payload;
        DWORD Written{};
        if (!WriteFile(
                ParentWrite_.Get(), &Request, sizeof(Request), &Written,
                nullptr) || Written != sizeof(Request)) {
            Reset();
            return Status::InternalFailure;
        }
        const auto Deadline = GetTickCount64() + 500;
        DWORD Available{};
        while (GetTickCount64() < Deadline) {
            if (!PeekNamedPipe(
                    ParentRead_.Get(), nullptr, 0, nullptr, &Available,
                    nullptr)) {
                Reset();
                return Status::InternalFailure;
            }
            if (Available >= sizeof(HelperResponse)) break;
            if (WaitForSingleObject(Process_.Get(), 0) != WAIT_TIMEOUT) {
                Reset();
                return Status::InternalFailure;
            }
            Sleep(1);
        }
        if (Available < sizeof(HelperResponse)) {
            Reset();
            return Status::InternalFailure;
        }
        HelperResponse Response;
        DWORD Read{};
        if (!ReadFile(
                ParentRead_.Get(), &Response, sizeof(Response), &Read,
                nullptr) || Read != sizeof(Response) ||
            Response.Magic != desklink::secure_input_wire::kMagic ||
            Response.Version != desklink::secure_input_wire::kVersion ||
            Response.Size != sizeof(Response)) {
            Reset();
            return Status::InternalFailure;
        }
        return Response.Result;
    }

    void Reset() noexcept {
        if (Process_ && WaitForSingleObject(Process_.Get(), 0) == WAIT_TIMEOUT) {
            ParentWrite_.Reset();
            (void)WaitForSingleObject(Process_.Get(), 250);
            if (WaitForSingleObject(Process_.Get(), 0) == WAIT_TIMEOUT) {
                (void)TerminateProcess(Process_.Get(), ERROR_PROCESS_ABORTED);
                (void)WaitForSingleObject(Process_.Get(), 250);
            }
        }
        ParentRead_.Reset();
        ParentWrite_.Reset();
        Process_.Reset();
    }

    [[nodiscard]] bool Active() const noexcept {
        return Process_ && WaitForSingleObject(Process_.Get(), 0) == WAIT_TIMEOUT;
    }

private:
    UniqueHandle ParentWrite_;
    UniqueHandle ParentRead_;
    UniqueHandle Process_;
};

class BrokerSession final {
public:
    BrokerSession(std::wstring Directory, std::wstring HelperPath)
        : Directory_(std::move(Directory)), HelperPath_(std::move(HelperPath)),
          Gate_(Clock_) {}

    Response Handle(const Request& Message) noexcept {
        Response Reply;
        if (!ValidHeader(Message)) {
            Reply.Result = Status::InvalidRequest;
            return Reply;
        }
        if (Message.RequestedOperation == Operation::Revoke) {
            Revoke();
            Reply.Result = Status::Ok;
            return Reply;
        }
        if (Message.RequestedOperation == Operation::Authorize ||
            Message.RequestedOperation == Operation::Renew) {
            const auto Protected = LoadProtectedGrant();
            if (!Protected ||
                Protected->PeerMachine != Message.PeerMachine ||
                Protected->PeerCertificateDerHash !=
                    Message.PeerCertificateDerHash ||
                Message.SessionNonce == 0 || Message.Epoch == 0 ||
                Message.LeaseMilliseconds < 100 ||
                Message.LeaseMilliseconds > 2'000 ||
                (Message.RequestedOperation == Operation::Renew &&
                    (!Gate_.Authorized() ||
                     Message.GrantRevision != CurrentGrantRevision_))) {
                Revoke();
                Reply.Result = Status::GrantRejected;
                return Reply;
            }
            if (AuthorizationRevision_ ==
                std::numeric_limits<std::uint64_t>::max()) {
                Revoke();
                Reply.Result = Status::InternalFailure;
                return Reply;
            }
            ++AuthorizationRevision_;
            desklink::SecureInputGrant Grant{
                Message.PeerMachine, Message.PeerCertificateDerHash,
                Message.SessionNonce, Message.Epoch, AuthorizationRevision_,
                true};
            if (!Gate_.Authorize(
                    Grant, std::chrono::milliseconds{
                        Message.LeaseMilliseconds})) {
                Revoke();
                Reply.Result = Status::GrantRejected;
                return Reply;
            }
            CurrentGrantRevision_ = AuthorizationRevision_;
            PolicyRevision_ = Protected->PolicyRevision;
            Reply.GrantRevision = CurrentGrantRevision_;
            Reply.Result = Status::Ok;
            return Reply;
        }

        const auto Protected = LoadProtectedGrant();
        if (!Protected || Protected->PolicyRevision != PolicyRevision_) {
            Revoke();
            Reply.Result = Status::GrantRejected;
            return Reply;
        }
        const auto SecureOperation = ToSecureOperation(
            Message.RequestedOperation);
        if (!SecureOperation) {
            Reply.Result = Status::InvalidRequest;
            return Reply;
        }
        desklink::SecureInputEnvelope Envelope{
            Message.PeerMachine, Message.PeerCertificateDerHash,
            Message.SessionNonce, Message.Epoch, Message.GrantRevision,
            Message.Sequence, *SecureOperation};
        const auto Decision = Gate_.Admit(Envelope);
        if (Decision != desklink::SecureInputDecision::Accepted) {
            Reply.Result =
                Decision == desklink::SecureInputDecision::RejectedExpired
                    ? Status::Expired :
                Decision == desklink::SecureInputDecision::RejectedSequence
                    ? Status::ReplayRejected : Status::GrantRejected;
            if (Reply.Result != Status::ReplayRejected) Revoke();
            return Reply;
        }
        Reply.Result = Dispatch(Message.RequestedOperation, Message.Payload);
        return Reply;
    }

    void Revoke() noexcept {
        if (DefaultHelper_.Active()) {
            (void)DefaultHelper_.Send(
                Operation::ReleaseOwnedState, {});
        }
        if (SecureHelper_.Active()) {
            (void)SecureHelper_.Send(
                Operation::ReleaseOwnedState, {});
        }
        DefaultHelper_.Reset();
        SecureHelper_.Reset();
        Gate_.Revoke();
        CurrentGrantRevision_ = 0;
        PolicyRevision_ = 0;
    }

private:
    static bool ValidHeader(const Request& Message) noexcept {
        return Message.Magic == desklink::secure_input_wire::kMagic &&
            Message.Version == desklink::secure_input_wire::kVersion &&
            Message.Size == sizeof(Message) && Message.Reserved == 0;
    }

    static std::optional<desklink::SecureInputOperation> ToSecureOperation(
        Operation Value) noexcept {
        switch (Value) {
            case Operation::ReleaseOwnedState:
                return desklink::SecureInputOperation::ReleaseOwnedState;
            case Operation::Key:
                return desklink::SecureInputOperation::Key;
            case Operation::MouseButton:
                return desklink::SecureInputOperation::MouseButton;
            case Operation::PointerMotion:
                return desklink::SecureInputOperation::PointerMotion;
            case Operation::Wheel:
                return desklink::SecureInputOperation::Wheel;
            case Operation::ReconcileState:
                return desklink::SecureInputOperation::ReconcileState;
            default:
                return std::nullopt;
        }
    }

    Status Dispatch(
        Operation RequestedOperation,
        const std::array<std::uint8_t, 72>& Payload) noexcept {
        if (RequestedOperation == Operation::ReleaseOwnedState) {
            Status Result = Status::Ok;
            if (DefaultHelper_.Active()) {
                Result = DefaultHelper_.Send(RequestedOperation, Payload);
            }
            if (SecureHelper_.Active()) {
                const auto SecureResult = SecureHelper_.Send(
                    RequestedOperation, Payload);
                if (SecureResult != Status::Ok) Result = SecureResult;
            }
            return Result;
        }
        const auto Desktop = ActiveInputDesktopName();
        const bool Secure = _wcsicmp(Desktop.c_str(), L"Winlogon") == 0;
        if (!Secure && _wcsicmp(Desktop.c_str(), L"Default") != 0) {
            return Status::DesktopUnavailable;
        }
        auto& Helper = Secure ? SecureHelper_ : DefaultHelper_;
        if (!Helper.Active() && !StartHelper(Helper, Secure)) {
            return Status::DesktopUnavailable;
        }
        return Helper.Send(RequestedOperation, Payload);
    }

    bool StartHelper(HelperChannel& Helper, bool Secure) noexcept {
        const DWORD SessionId = WTSGetActiveConsoleSessionId();
        return SessionId != 0xffffffffu && IsSessionUnlocked(SessionId) &&
            Helper.Start(HelperPath_, Directory_, SessionId, Secure);
    }

    std::wstring Directory_;
    std::wstring HelperPath_;
    desklink::SteadyClock Clock_;
    desklink::SecureInputAuthorizationGate Gate_;
    HelperChannel DefaultHelper_;
    HelperChannel SecureHelper_;
    std::uint64_t AuthorizationRevision_{};
    std::uint64_t CurrentGrantRevision_{};
    std::uint64_t PolicyRevision_{};
};

bool ValidateClient(
    HANDLE Pipe, const std::wstring& RuntimePath,
    const std::array<std::uint8_t, 32>& ExpectedSigner) noexcept {
    ULONG ProcessId{};
    if (!GetNamedPipeClientProcessId(Pipe, &ProcessId) || ProcessId == 0) {
        return false;
    }
    DWORD SessionId{};
    if (!ProcessIdToSessionId(ProcessId, &SessionId) ||
        SessionId != WTSGetActiveConsoleSessionId() ||
        !IsSessionUnlocked(SessionId)) {
        return false;
    }
    UniqueHandle Process(OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ProcessId));
    if (!Process) return false;
    std::array<wchar_t, 32'768> Path{};
    DWORD Length = static_cast<DWORD>(Path.size());
    if (!QueryFullProcessImageNameW(
            Process.Get(), 0, Path.data(), &Length) || Length == 0 ||
        Length >= Path.size() ||
        _wcsicmp(std::wstring(Path.data(), Length).c_str(),
            RuntimePath.c_str()) != 0) {
        return false;
    }
    const auto Signer = AuthenticodeSignerHash(RuntimePath);
    return Signer && *Signer == ExpectedSigner;
}

void ServePipe(
    HANDLE Pipe, const std::wstring& Directory,
    const std::wstring& HelperPath,
    const std::array<std::uint8_t, 32>& ExpectedSigner) noexcept {
    const std::wstring RuntimePath = Directory + L"\\" + kRuntimeName;
    if (!ValidateClient(Pipe, RuntimePath, ExpectedSigner)) {
        Log(L"pipe client rejected before any grant or input was admitted");
        return;
    }
    BrokerSession Session(Directory, HelperPath);
    while (WaitForSingleObject(StopEvent, 0) == WAIT_TIMEOUT) {
        DWORD Available{};
        if (!PeekNamedPipe(Pipe, nullptr, 0, nullptr, &Available, nullptr)) {
            break;
        }
        if (Available < sizeof(Request)) {
            Sleep(2);
            continue;
        }
        Request Message;
        DWORD Read{};
        if (!ReadFile(Pipe, &Message, sizeof(Message), &Read, nullptr) ||
            Read != sizeof(Message)) {
            break;
        }
        const auto Reply = Session.Handle(Message);
        DWORD Written{};
        if (!WriteFile(
                Pipe, &Reply, sizeof(Reply), &Written, nullptr) ||
            Written != sizeof(Reply)) {
            break;
        }
    }
    Session.Revoke();
}

SECURITY_ATTRIBUTES PipeSecurity(PSECURITY_DESCRIPTOR& Descriptor) noexcept {
    Descriptor = nullptr;
    SECURITY_ATTRIBUTES Result{};
    Result.nLength = sizeof(Result);
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)",
            SDDL_REVISION_1, &Descriptor, nullptr)) {
        Result.lpSecurityDescriptor = Descriptor;
    }
    return Result;
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
        (void)SetServiceStatus(ServiceStatusHandle, &ServiceStatus);
    }
}

DWORD WINAPI ServiceControlHandler(
    DWORD Control, DWORD, void*, void*) noexcept {
    if (Control == SERVICE_CONTROL_STOP ||
        Control == SERVICE_CONTROL_SHUTDOWN) {
        ReportServiceStatus(SERVICE_STOP_PENDING);
        if (StopEvent) SetEvent(StopEvent);
        return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

void WINAPI ServiceMain(DWORD, wchar_t**) noexcept {
    ServiceStatusHandle = RegisterServiceCtrlHandlerExW(
        desklink::secure_input_wire::kServiceName,
        ServiceControlHandler, nullptr);
    if (!ServiceStatusHandle) return;
    ReportServiceStatus(SERVICE_START_PENDING);

    const auto ServicePath = ModulePath();
    const auto Directory = ParentDirectory(ServicePath);
    const auto HelperPath = Directory + L"\\" + kHelperName;
    const auto RuntimePath = Directory + L"\\" + kRuntimeName;
    const auto Signer = AuthenticodeSignerHash(ServicePath);
    if (!IsLocalSystem() || Directory.empty() ||
        !IsPathUnderProgramFiles(ServicePath) ||
        !IsNonReparseDirectory(Directory) ||
        !IsRegularNonReparseFile(ServicePath) ||
        !IsRegularNonReparseFile(HelperPath) ||
        !IsRegularNonReparseFile(RuntimePath) || !Signer ||
        AuthenticodeSignerHash(HelperPath) != Signer ||
        AuthenticodeSignerHash(RuntimePath) != Signer ||
        !EnablePrivilege(SE_ASSIGNPRIMARYTOKEN_NAME) ||
        !EnablePrivilege(SE_INCREASE_QUOTA_NAME) ||
        !EnablePrivilege(SE_TCB_NAME)) {
        ReportServiceStatus(SERVICE_STOPPED, ERROR_ACCESS_DENIED);
        return;
    }
    StopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!StopEvent) {
        ReportServiceStatus(SERVICE_STOPPED, GetLastError());
        return;
    }
    PSECURITY_DESCRIPTOR Descriptor{};
    auto Security = PipeSecurity(Descriptor);
    if (!Descriptor) {
        CloseHandle(StopEvent);
        StopEvent = nullptr;
        ReportServiceStatus(SERVICE_STOPPED, ERROR_ACCESS_DENIED);
        return;
    }
    ReportServiceStatus(SERVICE_RUNNING);
    Log(L"authenticated fixed-envelope broker active; networking remains absent");
    while (WaitForSingleObject(StopEvent, 0) == WAIT_TIMEOUT) {
        UniqueHandle Pipe(CreateNamedPipeW(
            desklink::secure_input_wire::kPipeName,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_NOWAIT |
                PIPE_REJECT_REMOTE_CLIENTS,
            1, 4096, 4096, 0, &Security));
        if (!Pipe || Pipe.Get() == INVALID_HANDLE_VALUE) break;
        bool Connected{};
        while (WaitForSingleObject(StopEvent, 0) == WAIT_TIMEOUT) {
            if (ConnectNamedPipe(Pipe.Get(), nullptr)) {
                Connected = true;
                break;
            }
            const DWORD Error = GetLastError();
            if (Error == ERROR_PIPE_CONNECTED) {
                Connected = true;
                break;
            }
            if (Error != ERROR_PIPE_LISTENING && Error != ERROR_NO_DATA) break;
            Sleep(10);
        }
        if (Connected) {
            ServePipe(Pipe.Get(), Directory, HelperPath, *Signer);
            (void)DisconnectNamedPipe(Pipe.Get());
        }
    }
    LocalFree(Descriptor);
    CloseHandle(StopEvent);
    StopEvent = nullptr;
    ReportServiceStatus(SERVICE_STOPPED);
}

} // namespace

int wmain(int ArgumentCount, wchar_t** Arguments) {
    if (ArgumentCount == 2 &&
        std::wstring_view(Arguments[1]) == L"--self-test") {
        return ModulePath().empty() ? 1 : 0;
    }
    if (ArgumentCount != 1) return 2;
    SERVICE_TABLE_ENTRYW Table[] = {
        {const_cast<wchar_t*>(desklink::secure_input_wire::kServiceName),
         ServiceMain},
        {nullptr, nullptr},
    };
    return StartServiceCtrlDispatcherW(Table) ? 0 : 1;
}
