#include "desklink/win32_control.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace desklink {
namespace {

constexpr DWORD kControlIoTimeoutMs = 2'000;
constexpr std::uint8_t kControlResponseAcknowledgement = 0xa5u;

void ReportPipeFailure(std::string_view Message) {
    std::cerr << "[Control:Pipe] " << Message << '\n';
}

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE Value) noexcept : Value_(Value) {}
    ~UniqueHandle() { Reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& Other) noexcept
        : Value_(std::exchange(Other.Value_, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& Other) noexcept {
        if (this != &Other) Reset(std::exchange(Other.Value_, nullptr));
        return *this;
    }

    [[nodiscard]] HANDLE Get() const noexcept { return Value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return Value_ && Value_ != INVALID_HANDLE_VALUE;
    }
    HANDLE Release() noexcept { return std::exchange(Value_, nullptr); }
    void Reset(HANDLE Value = nullptr) noexcept {
        if (*this) CloseHandle(Value_);
        Value_ = Value;
    }

private:
    HANDLE Value_{};
};

bool IsValidInstance(std::wstring_view Instance) noexcept {
    if (Instance.size() > 32) return false;
    return std::all_of(Instance.begin(), Instance.end(), [](wchar_t Character) {
        return (Character >= L'a' && Character <= L'z') ||
               (Character >= L'A' && Character <= L'Z') ||
               (Character >= L'0' && Character <= L'9') || Character == L'-';
    });
}

std::optional<std::vector<std::uint8_t>> GetProcessUserSid(DWORD ProcessId) {
    UniqueHandle Process;
    HANDLE ProcessHandle = GetCurrentProcess();
    if (ProcessId != GetCurrentProcessId()) {
        Process.Reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                  ProcessId));
        if (!Process) return std::nullopt;
        ProcessHandle = Process.Get();
    }

    UniqueHandle Token;
    HANDLE RawToken{};
    if (!OpenProcessToken(ProcessHandle, TOKEN_QUERY, &RawToken)) {
        return std::nullopt;
    }
    Token.Reset(RawToken);

    DWORD Required{};
    (void)GetTokenInformation(Token.Get(), TokenUser, nullptr, 0, &Required);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        Required < sizeof(TOKEN_USER)) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> TokenBytes(Required);
    if (!GetTokenInformation(Token.Get(), TokenUser, TokenBytes.data(),
                             Required, &Required)) {
        return std::nullopt;
    }
    const auto* User = reinterpret_cast<const TOKEN_USER*>(TokenBytes.data());
    if (!IsValidSid(User->User.Sid)) return std::nullopt;
    const auto SidSize = GetLengthSid(User->User.Sid);
    std::vector<std::uint8_t> Sid(SidSize);
    if (!CopySid(SidSize, Sid.data(), User->User.Sid)) return std::nullopt;
    return Sid;
}

std::optional<std::wstring> GetPipeName(std::wstring_view Instance,
                                        const std::vector<std::uint8_t>& Sid) {
    if (!IsValidInstance(Instance) || Sid.empty()) return std::nullopt;
    LPWSTR SidText{};
    if (!ConvertSidToStringSidW(
            const_cast<void*>(static_cast<const void*>(Sid.data())), &SidText)) {
        return std::nullopt;
    }
    const std::wstring SidString(SidText);
    LocalFree(SidText);
    std::wstring Name = L"\\\\.\\pipe\\DeskLink.Control.v1." + SidString;
    if (!Instance.empty()) Name += L"." + std::wstring(Instance);
    return Name;
}

bool IsSameSid(const std::vector<std::uint8_t>& Left,
               const std::vector<std::uint8_t>& Right) noexcept {
    return !Left.empty() && !Right.empty() &&
           EqualSid(const_cast<void*>(static_cast<const void*>(Left.data())),
                    const_cast<void*>(static_cast<const void*>(Right.data()))) != FALSE;
}

bool VerifyPipeClient(HANDLE Pipe,
                      const std::vector<std::uint8_t>& ExpectedSid) {
    ULONG ProcessId{};
    if (!GetNamedPipeClientProcessId(Pipe, &ProcessId) || ProcessId == 0) {
        return false;
    }
    const auto ClientSid = GetProcessUserSid(ProcessId);
    return ClientSid && IsSameSid(ExpectedSid, *ClientSid);
}

bool VerifyPipeServer(HANDLE Pipe,
                      const std::vector<std::uint8_t>& ExpectedSid) {
    ULONG ProcessId{};
    if (!GetNamedPipeServerProcessId(Pipe, &ProcessId) || ProcessId == 0) {
        return false;
    }
    const auto ServerSid = GetProcessUserSid(ProcessId);
    return ServerSid && IsSameSid(ExpectedSid, *ServerSid);
}

bool VerifyPipeSecurity(HANDLE Pipe,
                        const std::vector<std::uint8_t>& ExpectedSid) {
    PACL Dacl{};
    PSECURITY_DESCRIPTOR Descriptor{};
    const auto Result = GetSecurityInfo(
        Pipe, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &Dacl, nullptr, &Descriptor);
    if (Result != ERROR_SUCCESS || !Descriptor || !Dacl) {
        if (Descriptor) LocalFree(Descriptor);
        return false;
    }

    bool Valid = Dacl->AceCount == 1;
    void* RawAce{};
    if (Valid) Valid = GetAce(Dacl, 0, &RawAce) != FALSE && RawAce;
    if (Valid) {
        const auto* Ace = static_cast<const ACCESS_ALLOWED_ACE*>(RawAce);
        const auto* AceSid = reinterpret_cast<const SID*>(&Ace->SidStart);
        Valid = Ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
                Ace->Header.AceFlags == 0 &&
                Ace->Mask == (FILE_GENERIC_READ | FILE_GENERIC_WRITE) &&
                IsValidSid(const_cast<SID*>(AceSid)) &&
                EqualSid(
                    const_cast<void*>(static_cast<const void*>(ExpectedSid.data())),
                    const_cast<SID*>(AceSid)) != FALSE;
    }
    SECURITY_DESCRIPTOR_CONTROL Control{};
    DWORD Revision{};
    Valid = Valid &&
            GetSecurityDescriptorControl(Descriptor, &Control, &Revision) != FALSE &&
            (Control & SE_DACL_PROTECTED) != 0;
    LocalFree(Descriptor);
    return Valid;
}

struct PipeSecurity {
    std::vector<std::uint8_t> AclBytes;
    SECURITY_DESCRIPTOR Descriptor{};
    SECURITY_ATTRIBUTES Attributes{};

    [[nodiscard]] bool Bind() noexcept {
        if (AclBytes.empty() ||
            !InitializeSecurityDescriptor(&Descriptor,
                                          SECURITY_DESCRIPTOR_REVISION) ||
            !SetSecurityDescriptorDacl(
                &Descriptor, TRUE,
                reinterpret_cast<ACL*>(AclBytes.data()), FALSE) ||
            !SetSecurityDescriptorControl(&Descriptor, SE_DACL_PROTECTED,
                                          SE_DACL_PROTECTED)) {
            return false;
        }
        Attributes.nLength = sizeof(SECURITY_ATTRIBUTES);
        Attributes.lpSecurityDescriptor = &Descriptor;
        Attributes.bInheritHandle = FALSE;
        return true;
    }
};

std::optional<PipeSecurity> BuildPipeSecurity(
    const std::vector<std::uint8_t>& Sid) {
    if (Sid.empty() || !IsValidSid(
            const_cast<void*>(static_cast<const void*>(Sid.data())))) {
        return std::nullopt;
    }
    const auto SidSize = GetLengthSid(
        const_cast<void*>(static_cast<const void*>(Sid.data())));
    const auto AclSize = sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) +
                         SidSize;
    PipeSecurity Security;
    Security.AclBytes.resize(AclSize);
    auto* Acl = reinterpret_cast<ACL*>(Security.AclBytes.data());
    if (!InitializeAcl(Acl, static_cast<DWORD>(AclSize), ACL_REVISION) ||
        !AddAccessAllowedAceEx(
            Acl, ACL_REVISION, 0, FILE_GENERIC_READ | FILE_GENERIC_WRITE,
            const_cast<void*>(static_cast<const void*>(Sid.data())))) {
        return std::nullopt;
    }
    return Security;
}

UniqueHandle CreatePipe(std::wstring_view Name, PipeSecurity& Security,
                        bool FirstInstance) {
    if (!Security.Bind()) return {};
    DWORD OpenMode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
    if (FirstInstance) OpenMode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
    return UniqueHandle(CreateNamedPipeW(
        std::wstring(Name).c_str(), OpenMode,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES,
        static_cast<DWORD>(kMaximumControlFrameSize),
        static_cast<DWORD>(kMaximumControlFrameSize),
        kControlIoTimeoutMs, &Security.Attributes));
}

bool WaitForOverlapped(HANDLE Pipe, OVERLAPPED& Operation, HANDLE StopEvent,
                       DWORD TimeoutMs, DWORD& Transferred) {
    HANDLE Events[2]{Operation.hEvent, StopEvent};
    const DWORD EventCount = StopEvent ? 2u : 1u;
    const auto WaitResult = WaitForMultipleObjects(EventCount, Events, FALSE,
                                                   TimeoutMs);
    if (WaitResult != WAIT_OBJECT_0) {
        (void)CancelIoEx(Pipe, &Operation);
        (void)WaitForSingleObject(Operation.hEvent, INFINITE);
        return false;
    }
    return GetOverlappedResult(Pipe, &Operation, &Transferred, FALSE) != FALSE;
}

bool TransferExact(HANDLE Pipe, std::span<std::uint8_t> Buffer, bool Write,
                   HANDLE StopEvent, DWORD TimeoutMs) {
    std::size_t Offset = 0;
    while (Offset < Buffer.size()) {
        UniqueHandle Event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!Event) return false;
        OVERLAPPED Operation{};
        Operation.hEvent = Event.Get();
        const auto Remaining = Buffer.size() - Offset;
        const auto Chunk = static_cast<DWORD>(std::min<std::size_t>(
            Remaining, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD Transferred{};
        const BOOL Started = Write
            ? WriteFile(Pipe, Buffer.data() + Offset, Chunk, &Transferred,
                        &Operation)
            : ReadFile(Pipe, Buffer.data() + Offset, Chunk, &Transferred,
                       &Operation);
        if (!Started) {
            if (GetLastError() != ERROR_IO_PENDING ||
                !WaitForOverlapped(Pipe, Operation, StopEvent, TimeoutMs,
                                   Transferred)) {
                return false;
            }
        }
        if (Transferred == 0 || Transferred > Chunk) return false;
        Offset += Transferred;
    }
    return true;
}

std::optional<std::uint32_t> ReadPayloadSize(ByteSpan Header) noexcept {
    if (Header.size() != kControlFrameHeaderSize) return std::nullopt;
    std::uint32_t Size = 0;
    for (std::size_t Index = 16; Index < 20; ++Index) {
        Size = (Size << 8u) | Header[Index];
    }
    if (Size > kMaximumControlPayload) return std::nullopt;
    return Size;
}

bool ConnectPipe(HANDLE Pipe, HANDLE StopEvent) {
    UniqueHandle Event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!Event) return false;
    OVERLAPPED Operation{};
    Operation.hEvent = Event.Get();
    if (ConnectNamedPipe(Pipe, &Operation)) return true;
    const auto Error = GetLastError();
    if (Error == ERROR_PIPE_CONNECTED) return true;
    if (Error != ERROR_IO_PENDING) return false;
    DWORD Transferred{};
    return WaitForOverlapped(Pipe, Operation, StopEvent, INFINITE, Transferred);
}

void ServeClient(HANDLE Pipe, HANDLE StopEvent,
                 const std::vector<std::uint8_t>& UserSid,
                 const Win32ControlPipeServer::Handler& Handler) {
    if (!VerifyPipeClient(Pipe, UserSid)) {
        ReportPipeFailure("rejected client process identity");
        return;
    }
    ByteBuffer Frame(kControlFrameHeaderSize);
    if (!TransferExact(Pipe, Frame, false, StopEvent, kControlIoTimeoutMs)) {
        ReportPipeFailure("request header read failed");
        return;
    }
    const auto PayloadSize = ReadPayloadSize(Frame);
    if (!PayloadSize) {
        ReportPipeFailure("request payload size was invalid");
        return;
    }
    Frame.resize(kControlFrameHeaderSize + *PayloadSize);
    if (*PayloadSize != 0 && !TransferExact(
            Pipe, std::span<std::uint8_t>(Frame).subspan(kControlFrameHeaderSize),
            false, StopEvent, kControlIoTimeoutMs)) {
        ReportPipeFailure("request payload read failed");
        return;
    }
    const auto Request = DecodeControlRequest(Frame);
    if (!Request.Decoded) {
        ReportPipeFailure("request frame was malformed");
        return;
    }

    ControlResponse Response;
    try {
        Response = Handler(*Request.Decoded);
    } catch (...) {
        Response = {Request.Decoded->RequestId, ControlStatus::Failed, std::nullopt};
    }
    Response.RequestId = Request.Decoded->RequestId;
    if (!IsValidControlResponse(Response)) {
        Response = {Request.Decoded->RequestId, ControlStatus::Failed, std::nullopt};
    }
    auto ResponseFrame = EncodeControlResponse(Response);
    if (!ResponseFrame) {
        ReportPipeFailure("response encoding failed");
        return;
    }
    if (!TransferExact(Pipe, *ResponseFrame, true, StopEvent,
                       kControlIoTimeoutMs)) {
        ReportPipeFailure("response write failed");
        return;
    }
    std::array<std::uint8_t, 1> Acknowledgement{};
    if (!TransferExact(Pipe, Acknowledgement, false, StopEvent,
                       kControlIoTimeoutMs) ||
        Acknowledgement[0] != kControlResponseAcknowledgement) {
        ReportPipeFailure("response acknowledgement failed");
    }
}

DWORD ClampTimeout(std::chrono::milliseconds Timeout) noexcept {
    return static_cast<DWORD>(std::clamp<std::int64_t>(
        Timeout.count(), 1, 5'000));
}

UniqueHandle OpenPipeWithinDeadline(std::wstring_view Name, DWORD TimeoutMs) {
    const auto Deadline = GetTickCount64() + TimeoutMs;
    for (;;) {
        const auto Now = GetTickCount64();
        if (Now >= Deadline) return {};
        const auto Remaining = static_cast<DWORD>(std::min<ULONGLONG>(
            Deadline - Now, static_cast<ULONGLONG>(50)));
        if (WaitNamedPipeW(std::wstring(Name).c_str(), Remaining)) {
            UniqueHandle Pipe(CreateFileW(
                std::wstring(Name).c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                nullptr, OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT |
                    SECURITY_IDENTIFICATION,
                nullptr));
            if (Pipe) return Pipe;
        }
        const auto Error = GetLastError();
        if (Error != ERROR_FILE_NOT_FOUND && Error != ERROR_PIPE_BUSY &&
            Error != ERROR_SEM_TIMEOUT) {
            return {};
        }
        Sleep(static_cast<DWORD>(std::min<ULONGLONG>(
            10, Deadline > GetTickCount64()
                    ? Deadline - GetTickCount64() : 0)));
    }
}

} // namespace

struct Win32ControlPipeServer::State {
    State(Handler RequestHandler, std::wstring InstanceName)
        : RequestHandler(std::move(RequestHandler)),
          Instance(std::move(InstanceName)) {}

    Handler RequestHandler;
    std::wstring Instance;
    std::wstring Name;
    std::vector<std::uint8_t> UserSid;
    UniqueHandle StopEvent;
    std::thread Worker;
    std::atomic_bool IsRunning{};
    std::mutex LifecycleMutex;
};

std::optional<std::wstring> GetWin32ControlPipeName(
    std::wstring_view Instance) {
    const auto Sid = GetProcessUserSid(GetCurrentProcessId());
    return Sid ? GetPipeName(Instance, *Sid) : std::nullopt;
}

Win32ControlPipeServer::Win32ControlPipeServer(Handler RequestHandler,
                                               std::wstring Instance)
    : State_(std::make_unique<State>(std::move(RequestHandler),
                                     std::move(Instance))) {}

Win32ControlPipeServer::~Win32ControlPipeServer() { Stop(); }

bool Win32ControlPipeServer::Start() {
    std::scoped_lock Lock(State_->LifecycleMutex);
    if (State_->IsRunning.load() || !State_->RequestHandler ||
        !IsValidInstance(State_->Instance)) {
        return false;
    }
    auto Sid = GetProcessUserSid(GetCurrentProcessId());
    if (!Sid) return false;
    auto Name = GetPipeName(State_->Instance, *Sid);
    auto Security = BuildPipeSecurity(*Sid);
    if (!Name || !Security) return false;
    auto Pipe = CreatePipe(*Name, *Security, true);
    if (!Pipe) return false;
    State_->StopEvent.Reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!State_->StopEvent) return false;

    State_->UserSid = std::move(*Sid);
    State_->Name = std::move(*Name);
    State_->IsRunning.store(true);
    State_->Worker = std::thread([State = State_.get(),
                                  Pipe = std::move(Pipe)]() mutable {
        auto CurrentPipe = std::move(Pipe);
        while (State->IsRunning.load()) {
            if (!ConnectPipe(CurrentPipe.Get(), State->StopEvent.Get())) break;
            auto NextSecurity = BuildPipeSecurity(State->UserSid);
            auto NextPipe = NextSecurity
                ? CreatePipe(State->Name, *NextSecurity, false)
                : UniqueHandle{};
            if (!NextSecurity) {
                ReportPipeFailure("replacement pipe security construction failed");
                State->IsRunning.store(false);
            } else if (!NextPipe) {
                const auto Error = GetLastError();
                std::cerr << "[Control:Pipe] replacement pipe creation failed error="
                          << Error << '\n';
                State->IsRunning.store(false);
            }
            ServeClient(CurrentPipe.Get(), State->StopEvent.Get(),
                        State->UserSid, State->RequestHandler);
            (void)DisconnectNamedPipe(CurrentPipe.Get());
            CurrentPipe = std::move(NextPipe);
            if (!CurrentPipe) break;
        }
        State->IsRunning.store(false);
    });
    return true;
}

void Win32ControlPipeServer::Stop() noexcept {
    std::unique_lock Lock(State_->LifecycleMutex);
    State_->IsRunning.store(false);
    if (State_->StopEvent) SetEvent(State_->StopEvent.Get());
    auto Worker = std::move(State_->Worker);
    Lock.unlock();
    if (Worker.joinable()) Worker.join();
    Lock.lock();
    State_->StopEvent.Reset();
    State_->Name.clear();
    State_->UserSid.clear();
}

bool Win32ControlPipeServer::Running() const noexcept {
    return State_->IsRunning.load();
}

std::wstring Win32ControlPipeServer::PipeName() const {
    std::scoped_lock Lock(State_->LifecycleMutex);
    return State_->Name;
}

std::optional<ControlResponse> Win32ControlPipeClient::Send(
    const ControlRequest& Request, std::wstring_view Instance,
    std::chrono::milliseconds Timeout) {
    if (!IsValidInstance(Instance)) return std::nullopt;
    auto Frame = EncodeControlRequest(Request);
    const auto UserSid = GetProcessUserSid(GetCurrentProcessId());
    if (!Frame || !UserSid) {
        ReportPipeFailure("request or current-user identity was invalid");
        return std::nullopt;
    }
    const auto Name = GetPipeName(Instance, *UserSid);
    if (!Name) {
        ReportPipeFailure("pipe name construction failed");
        return std::nullopt;
    }
    const auto TimeoutMs = ClampTimeout(Timeout);
    auto Pipe = OpenPipeWithinDeadline(*Name, TimeoutMs);
    if (!Pipe) {
        ReportPipeFailure("same-user server was unavailable");
        return std::nullopt;
    }
    if (!VerifyPipeServer(Pipe.Get(), *UserSid) ||
        !VerifyPipeSecurity(Pipe.Get(), *UserSid)) {
        ReportPipeFailure("rejected server process identity");
        return std::nullopt;
    }
    if (!TransferExact(Pipe.Get(), *Frame, true, nullptr, TimeoutMs)) {
        ReportPipeFailure("request write failed");
        return std::nullopt;
    }

    ByteBuffer ResponseFrame(kControlFrameHeaderSize);
    if (!TransferExact(Pipe.Get(), ResponseFrame, false, nullptr, TimeoutMs)) {
        ReportPipeFailure("response header read failed");
        return std::nullopt;
    }
    const auto PayloadSize = ReadPayloadSize(ResponseFrame);
    if (!PayloadSize) {
        ReportPipeFailure("response payload size was invalid");
        return std::nullopt;
    }
    ResponseFrame.resize(kControlFrameHeaderSize + *PayloadSize);
    if (*PayloadSize != 0 && !TransferExact(
            Pipe.Get(),
            std::span<std::uint8_t>(ResponseFrame).subspan(kControlFrameHeaderSize),
            false, nullptr, TimeoutMs)) {
        ReportPipeFailure("response payload read failed");
        return std::nullopt;
    }
    const auto Response = DecodeControlResponse(ResponseFrame);
    if (!Response.Decoded || Response.Decoded->RequestId != Request.RequestId) {
        ReportPipeFailure("response frame was invalid or mismatched");
        return std::nullopt;
    }
    std::array<std::uint8_t, 1> Acknowledgement{
        kControlResponseAcknowledgement};
    if (!TransferExact(Pipe.Get(), Acknowledgement, true, nullptr, TimeoutMs)) {
        ReportPipeFailure("response acknowledgement write failed");
        return std::nullopt;
    }
    return Response.Decoded;
}

} // namespace desklink
