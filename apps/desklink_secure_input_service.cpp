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

void RecordForwardDiagnostic(Operation RequestedOperation,
                             Status Result, DWORD Stage = 0,
                             DWORD Win32Error = ERROR_SUCCESS) noexcept {
    if (Result == Status::Ok) return;
    HKEY Key{};
    if (RegCreateKeyExW(
            HKEY_LOCAL_MACHINE,
            desklink::secure_input_wire::kDiagnosticRegistryPath, 0, nullptr,
            REG_OPTION_VOLATILE, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr,
            &Key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    const DWORD RawOperation = static_cast<DWORD>(RequestedOperation);
    const DWORD RawStatus = static_cast<DWORD>(Result);
    const ULONGLONG ObservedAtTick = GetTickCount64();
    (void)RegSetValueExW(Key, L"Operation", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&RawOperation), sizeof(RawOperation));
    (void)RegSetValueExW(Key, L"Status", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&RawStatus), sizeof(RawStatus));
    (void)RegSetValueExW(Key, L"Stage", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&Stage), sizeof(Stage));
    (void)RegSetValueExW(Key, L"Win32Error", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&Win32Error), sizeof(Win32Error));
    (void)RegSetValueExW(Key, L"ObservedAtTick", 0, REG_QWORD,
        reinterpret_cast<const BYTE*>(&ObservedAtTick),
        sizeof(ObservedAtTick));
    RegCloseKey(Key);
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
        LastStartStage_ = 0;
        LastStartError_ = ERROR_SUCCESS;
        SECURITY_ATTRIBUTES Security{};
        Security.nLength = sizeof(Security);
        Security.bInheritHandle = TRUE;
        HANDLE ChildRead{};
        HANDLE ParentWrite{};
        HANDLE ParentRead{};
        HANDLE ChildWrite{};
        if (!CreatePipe(&ChildRead, &ParentWrite, &Security, 4096) ||
            !CreatePipe(&ParentRead, &ChildWrite, &Security, 4096)) {
            LastStartStage_ = 1;
            LastStartError_ = GetLastError();
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
            LastStartStage_ = 2;
            LastStartError_ = GetLastError();
            Reset();
            return false;
        }

        HANDLE ProcessTokenRaw{};
        HANDLE SessionTokenRaw{};
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY,
                &ProcessTokenRaw)) {
            LastStartStage_ = 3;
            LastStartError_ = GetLastError();
            return false;
        }
        UniqueHandle ProcessToken(ProcessTokenRaw);
        if (!DuplicateTokenEx(
                ProcessToken.Get(), TOKEN_ALL_ACCESS, nullptr,
                SecurityImpersonation, TokenPrimary, &SessionTokenRaw)) {
            LastStartStage_ = 4;
            LastStartError_ = GetLastError();
            return false;
        }
        UniqueHandle SessionToken(SessionTokenRaw);
        if (!SetTokenInformation(
                SessionToken.Get(), TokenSessionId, &SessionId,
                sizeof(SessionId))) {
            LastStartStage_ = 5;
            LastStartError_ = GetLastError();
            return false;
        }

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
            LastStartStage_ = 6;
            LastStartError_ = GetLastError();
            Reset();
            return false;
        }
        CloseHandle(Process.hThread);
        Process_.Reset(Process.hProcess);
        ChildReadOwner.Reset();
        ChildWriteOwner.Reset();

        const std::array<HANDLE, 2> ReadyWaitHandles{
            ParentRead_.Get(), Process_.Get()};
        const auto ReadyWait = WaitForMultipleObjects(
            static_cast<DWORD>(ReadyWaitHandles.size()),
            ReadyWaitHandles.data(), FALSE, 500);
        if (ReadyWait == WAIT_OBJECT_0 + 1) {
            DWORD ExitCode{};
            (void)GetExitCodeProcess(Process_.Get(), &ExitCode);
            LastStartStage_ = 8;
            LastStartError_ = ExitCode;
            Reset();
            return false;
        }
        if (ReadyWait != WAIT_OBJECT_0) {
            LastStartStage_ = 9;
            LastStartError_ = ReadyWait == WAIT_TIMEOUT
                ? ERROR_TIMEOUT : GetLastError();
            Reset();
            return false;
        }
        HelperResponse Ready;
        DWORD ReadyRead{};
        if (!ReadFile(
                ParentRead_.Get(), &Ready, sizeof(Ready), &ReadyRead,
                nullptr) || ReadyRead != sizeof(Ready) ||
            Ready.Magic != desklink::secure_input_wire::kMagic ||
            Ready.Version != desklink::secure_input_wire::kVersion ||
            Ready.Size != sizeof(Ready)) {
            LastStartStage_ = 10;
            LastStartError_ = ERROR_INVALID_DATA;
            Reset();
            return false;
        }
        if (Ready.Result != Status::Ok) {
            LastStartStage_ = 11;
            LastStartError_ = static_cast<DWORD>(Ready.Result);
            Reset();
            return false;
        }
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
        const std::array<HANDLE, 2> ResponseWaitHandles{
            ParentRead_.Get(), Process_.Get()};
        const auto ResponseWait = WaitForMultipleObjects(
            static_cast<DWORD>(ResponseWaitHandles.size()),
            ResponseWaitHandles.data(), FALSE, 500);
        if (ResponseWait != WAIT_OBJECT_0) {
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

    [[nodiscard]] DWORD LastStartStage() const noexcept {
        return LastStartStage_;
    }

    [[nodiscard]] DWORD LastStartError() const noexcept {
        return LastStartError_;
    }

private:
    UniqueHandle ParentWrite_;
    UniqueHandle ParentRead_;
    UniqueHandle Process_;
    DWORD LastStartStage_{};
    DWORD LastStartError_{};
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
            if (Message.RequestedOperation == Operation::Authorize) {
                // Starting and validating the helper can consume a material
                // part of a short focus lease. Complete that bounded work
                // before the authorization clock starts so an admitted grant
                // always receives its full requested lifetime.
                const auto Prepared = PrepareHelper(false);
                if (Prepared != Status::Ok) {
                    RecordForwardDiagnostic(
                        Message.RequestedOperation, Prepared,
                        LastPrepareStage_, LastPrepareError_);
                    Revoke();
                    Reply.Result = Prepared;
                    return Reply;
                }
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
            case Operation::PointerPosition:
                return desklink::SecureInputOperation::PointerPosition;
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
            RecordForwardDiagnostic(RequestedOperation, Result, 200);
            return Result;
        }
        auto Prepared = PrepareHelper(false);
        if (Prepared != Status::Ok) {
            RecordForwardDiagnostic(RequestedOperation, Prepared,
                LastPrepareStage_, LastPrepareError_);
            return Prepared;
        }
        const auto DefaultResult = DefaultHelper_.Send(
            RequestedOperation, Payload);
        if (DefaultResult == Status::Ok) return Status::Ok;
        if (DefaultResult != Status::DesktopUnavailable) {
            RecordForwardDiagnostic(RequestedOperation, DefaultResult, 201);
            return DefaultResult;
        }

        // DesktopUnavailable is the only routing result that may try the
        // other fixed desktop. Authentication, authorization, IPC, and
        // injection failures remain terminal and never fall through.
        Prepared = PrepareHelper(true);
        if (Prepared != Status::Ok) {
            RecordForwardDiagnostic(RequestedOperation, Prepared,
                LastPrepareStage_, LastPrepareError_);
            return Prepared;
        }
        const auto SecureResult = SecureHelper_.Send(
            RequestedOperation, Payload);
        RecordForwardDiagnostic(RequestedOperation, SecureResult, 202);
        return SecureResult;
    }

    Status PrepareHelper(bool Secure) noexcept {
        LastPrepareStage_ = 0;
        LastPrepareError_ = ERROR_SUCCESS;
        auto& Helper = Secure ? SecureHelper_ : DefaultHelper_;
        if (Helper.Active()) return Status::Ok;
        const DWORD SessionId = WTSGetActiveConsoleSessionId();
        if (SessionId == 0xffffffffu) {
            LastPrepareStage_ = 2;
            LastPrepareError_ = ERROR_NO_SUCH_LOGON_SESSION;
            return Status::DesktopUnavailable;
        }
        if (!IsSessionUnlocked(SessionId)) {
            LastPrepareStage_ = 3;
            LastPrepareError_ = ERROR_ACCESS_DENIED;
            return Status::DesktopUnavailable;
        }
        if (!Helper.Start(HelperPath_, Directory_, SessionId, Secure)) {
            LastPrepareStage_ = 100 + Helper.LastStartStage();
            LastPrepareError_ = Helper.LastStartError();
            return Status::DesktopUnavailable;
        }
        return Status::Ok;
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
    DWORD LastPrepareStage_{};
    DWORD LastPrepareError_{};
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

enum class PipeIoResult {
    Complete,
    Stopped,
    Failed,
};

PipeIoResult TransferPipeWithStop(
    HANDLE Pipe, HANDLE IoEvent, void* Buffer, DWORD Bytes,
    bool Write) noexcept {
    if (!Pipe || Pipe == INVALID_HANDLE_VALUE || !IoEvent || !Buffer ||
        Bytes == 0) {
        return PipeIoResult::Failed;
    }
    (void)ResetEvent(IoEvent);
    OVERLAPPED Pending{};
    Pending.hEvent = IoEvent;
    DWORD Transferred{};
    const BOOL Started = Write
        ? WriteFile(Pipe, Buffer, Bytes, &Transferred, &Pending)
        : ReadFile(Pipe, Buffer, Bytes, &Transferred, &Pending);
    if (Started) {
        return Transferred == Bytes
            ? PipeIoResult::Complete : PipeIoResult::Failed;
    }
    if (GetLastError() != ERROR_IO_PENDING) return PipeIoResult::Failed;

    const std::array<HANDLE, 2> WaitHandles{StopEvent, IoEvent};
    const auto Wait = WaitForMultipleObjects(
        static_cast<DWORD>(WaitHandles.size()), WaitHandles.data(), FALSE,
        INFINITE);
    if (Wait == WAIT_OBJECT_0) {
        (void)CancelIoEx(Pipe, &Pending);
        (void)GetOverlappedResult(Pipe, &Pending, &Transferred, TRUE);
        return PipeIoResult::Stopped;
    }
    if (Wait != WAIT_OBJECT_0 + 1) {
        (void)CancelIoEx(Pipe, &Pending);
        (void)GetOverlappedResult(Pipe, &Pending, &Transferred, TRUE);
        return PipeIoResult::Failed;
    }
    if (!GetOverlappedResult(Pipe, &Pending, &Transferred, FALSE) ||
        Transferred != Bytes) {
        return PipeIoResult::Failed;
    }
    return PipeIoResult::Complete;
}

bool ConnectPipeWithStop(HANDLE Pipe, HANDLE IoEvent) noexcept {
    (void)ResetEvent(IoEvent);
    OVERLAPPED Pending{};
    Pending.hEvent = IoEvent;
    if (ConnectNamedPipe(Pipe, &Pending)) return true;
    const auto Error = GetLastError();
    if (Error == ERROR_PIPE_CONNECTED) return true;
    if (Error != ERROR_IO_PENDING) return false;

    const std::array<HANDLE, 2> WaitHandles{StopEvent, IoEvent};
    const auto Wait = WaitForMultipleObjects(
        static_cast<DWORD>(WaitHandles.size()), WaitHandles.data(), FALSE,
        INFINITE);
    DWORD Transferred{};
    if (Wait == WAIT_OBJECT_0) {
        (void)CancelIoEx(Pipe, &Pending);
        (void)GetOverlappedResult(Pipe, &Pending, &Transferred, TRUE);
        return false;
    }
    if (Wait != WAIT_OBJECT_0 + 1) {
        (void)CancelIoEx(Pipe, &Pending);
        (void)GetOverlappedResult(Pipe, &Pending, &Transferred, TRUE);
        return false;
    }
    return GetOverlappedResult(Pipe, &Pending, &Transferred, FALSE);
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
    UniqueHandle IoEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!IoEvent) return;
    while (WaitForSingleObject(StopEvent, 0) == WAIT_TIMEOUT) {
        Request Message;
        if (TransferPipeWithStop(
                Pipe, IoEvent.Get(), &Message, sizeof(Message), false) !=
            PipeIoResult::Complete) {
            break;
        }
        const auto Reply = Session.Handle(Message);
        auto MutableReply = Reply;
        if (TransferPipeWithStop(
                Pipe, IoEvent.Get(), &MutableReply, sizeof(MutableReply),
                true) != PipeIoResult::Complete) {
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
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
                PIPE_REJECT_REMOTE_CLIENTS,
            1, 4096, 4096, 0, &Security));
        if (!Pipe || Pipe.Get() == INVALID_HANDLE_VALUE) break;
        UniqueHandle ConnectEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!ConnectEvent) break;
        const bool Connected = ConnectPipeWithStop(
            Pipe.Get(), ConnectEvent.Get());
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
