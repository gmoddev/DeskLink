#include "desklink/control.hpp"
#include "desklink/runtime_broker.hpp"
#include "desklink/win32_application_settings.hpp"
#include "desklink/win32_control.hpp"
#include "desklink/win32_device_certificate.hpp"
#include "desklink/win32_pairing.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>

namespace {

constexpr wchar_t kBrokerMutexName[] = L"Local\\DeskLink.RuntimeBroker.v1";
constexpr wchar_t kRuntimeMutexName[] = L"Local\\DeskLink.Runtime.v1";
constexpr wchar_t kDeviceKeyName[] = L"DeskLink-Device-Identity-v1";
constexpr wchar_t kBrokerPipeInstance[] = L"broker";

class UniqueHandle final {
public:
    explicit UniqueHandle(HANDLE Value = nullptr) noexcept : Value_(Value) {}
    ~UniqueHandle() { Reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& Other) noexcept
        : Value_(std::exchange(Other.Value_, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& Other) noexcept {
        if (this != &Other) Reset(std::exchange(Other.Value_, nullptr));
        return *this;
    }
    void Reset(HANDLE Value = nullptr) noexcept {
        if (Value_ && Value_ != INVALID_HANDLE_VALUE) CloseHandle(Value_);
        Value_ = Value;
    }
    [[nodiscard]] HANDLE Get() const noexcept { return Value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return Value_ && Value_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE Value_{};
};

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

std::optional<std::filesystem::path> GetExecutablePath() {
    std::wstring Buffer(32'768, L'\0');
    const auto Length = GetModuleFileNameW(
        nullptr, Buffer.data(), static_cast<DWORD>(Buffer.size()));
    if (Length == 0 ||
        static_cast<std::size_t>(Length) >= Buffer.size()) {
        return std::nullopt;
    }
    Buffer.resize(Length);
    return std::filesystem::path(std::move(Buffer));
}

bool RuntimeProcessMayExist() noexcept {
    SetLastError(ERROR_SUCCESS);
    UniqueHandle Runtime(OpenMutexW(SYNCHRONIZE, FALSE, kRuntimeMutexName));
    const auto Error = GetLastError();
    return desklink::RuntimeOwnerMayBeActive(
        static_cast<bool>(Runtime), Error == ERROR_FILE_NOT_FOUND);
}

std::optional<desklink::ControlResponse> ForwardToActiveRuntime(
    const desklink::ControlRequest& Request,
    std::chrono::milliseconds Timeout = std::chrono::milliseconds{500}) {
    return desklink::Win32ControlPipeClient::Send(Request, {}, Timeout);
}

class BrokerRuntimeSafetyController final
    : public desklink::IRuntimeSafetyController {
public:
    [[nodiscard]] bool ReturnLocalOnly() noexcept {
        if (!RuntimeProcessMayExist()) return true;
        const auto RequestId = NextRequestId_.fetch_add(1);
        const auto Response = ForwardToActiveRuntime(
            desklink::ControlRequest{
                RequestId,
                desklink::SetDesiredModeControlRequest{
                    desklink::DeskMode::LockPc1}});
        if (!Response || Response->Status != desklink::ControlStatus::Ok) {
            return false;
        }
        const auto State = ForwardToActiveRuntime(desklink::ControlRequest{
            NextRequestId_.fetch_add(1),
            desklink::GetStateControlRequest{}});
        return State && State->Status == desklink::ControlStatus::Ok &&
               State->State && !State->State->RemoteFocused &&
               !State->State->CaptureActive;
    }

    [[nodiscard]] bool StopActiveRuntime() noexcept {
        if (!RuntimeProcessMayExist()) return true;
        if (!ReturnLocalOnly()) return false;
        const auto Response = ForwardToActiveRuntime(desklink::ControlRequest{
            NextRequestId_.fetch_add(1),
            desklink::PrepareForUpdateControlRequest{}},
            std::chrono::milliseconds{2'000});
        if (!Response || Response->Status != desklink::ControlStatus::Ok) {
            return false;
        }
        const auto Deadline = GetTickCount64() + 1'500u;
        while (GetTickCount64() < Deadline) {
            if (!RuntimeProcessMayExist()) return true;
            Sleep(25);
        }
        return !RuntimeProcessMayExist();
    }

    bool ReturnLocalAndStopPeer(
        const desklink::MachineId&) noexcept override {
        // The current transport process owns one authenticated peer session.
        // Stop that entire owner before changing or forgetting its trust.
        return StopActiveRuntime();
    }

private:
    std::atomic_uint64_t NextRequestId_{0xB000'0000u};
};

desklink::ControlStatus MapMutationStatus(
    desklink::TrustMutationStatus Status) noexcept {
    switch (Status) {
        case desklink::TrustMutationStatus::Applied:
        case desklink::TrustMutationStatus::NoChange:
            return desklink::ControlStatus::Ok;
        case desklink::TrustMutationStatus::ReauthorizationRequired:
            return desklink::ControlStatus::ReauthorizationRequired;
        case desklink::TrustMutationStatus::InvalidRequest:
            return desklink::ControlStatus::InvalidRequest;
        case desklink::TrustMutationStatus::PeerNotFound:
            return desklink::ControlStatus::NotReady;
        case desklink::TrustMutationStatus::CleanupFailed:
            return desklink::ControlStatus::CleanupFailed;
        case desklink::TrustMutationStatus::StoreFailed:
            return desklink::ControlStatus::Failed;
    }
    return desklink::ControlStatus::Failed;
}

desklink::ControlState LocalState(const desklink::MachineId& LocalMachine) {
    desklink::ControlState State;
    State.LocalMachine = LocalMachine;
    State.Role = desklink::ControlRole::Idle;
    State.DesiredMode = desklink::DeskMode::LockPc1;
    return State;
}

} // namespace

int wmain() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    UniqueHandle BrokerMutex(CreateMutexW(
        nullptr, FALSE, kBrokerMutexName));
    if (!BrokerMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        std::cerr << "[Broker:Lifecycle] another per-user broker is active\n";
        return 1;
    }

    const auto DataDirectory = GetDataDirectory();
    const auto Executable = GetExecutablePath();
    if (!DataDirectory || !Executable) {
        std::cerr << "[Broker:Storage] current-user data directory is unavailable\n";
        return 1;
    }
    const auto AlphaPath = Executable->parent_path() / L"desklink_alpha.exe";

    desklink::BCryptPairingCrypto Crypto;
    auto Certificate = desklink::Win32DeviceCertificate::LoadOrCreate(
        kDeviceKeyName, Crypto);
    if (!Certificate) {
        std::cerr << "[Broker:Identity] device identity is unavailable\n";
        return 1;
    }
    const auto LocalMachine =
        desklink::DeriveMachineId(Certificate->CertificatePin());

    desklink::DpapiTrustStore TrustStore(*DataDirectory / L"trust.db");
    desklink::Win32ProductPreferencesStore PreferencesStore(
        *DataDirectory / L"application.settings");
    if (!TrustStore.Load() || !PreferencesStore.Load()) {
        std::cerr << "[Broker:Storage] protected state could not be loaded\n";
        return 1;
    }

    BrokerRuntimeSafetyController SafetyController;
    desklink::RuntimeTrustAuthority TrustAuthority(
        TrustStore, SafetyController);
    desklink::BrokerPairingCandidateLease PairingCandidates;
    desklink::SteadyClock Clock;
    UniqueHandle StopEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!StopEvent) return 1;

    desklink::Win32ControlPipeServer Server(
        [&](const desklink::ControlRequest& Request) {
            if (std::holds_alternative<
                    desklink::GetProductPreferencesControlRequest>(
                    Request.Payload)) {
                const auto Preferences = PreferencesStore.Current();
                desklink::ControlResponse Response{
                    Request.RequestId,
                    Preferences ? desklink::ControlStatus::Ok
                                : desklink::ControlStatus::Failed};
                Response.Preferences = Preferences;
                return Response;
            }
            if (const auto* Set = std::get_if<
                    desklink::SetProductPreferencesControlRequest>(
                    &Request.Payload)) {
                const auto Previous = PreferencesStore.Current();
                const bool StartupSaved = std::filesystem::is_regular_file(
                        AlphaPath) &&
                    desklink::SetWin32RunAtLogin(
                        Set->Preferences.RunAtLogin, AlphaPath);
                const bool Saved = StartupSaved &&
                    PreferencesStore.Save(Set->Preferences);
                if (!Saved && Previous) {
                    (void)desklink::SetWin32RunAtLogin(
                        Previous->RunAtLogin, AlphaPath);
                }
                return desklink::ControlResponse{
                    Request.RequestId,
                    Saved
                        ? desklink::ControlStatus::Ok
                        : desklink::ControlStatus::Failed};
            }
            if (std::holds_alternative<
                    desklink::ListTrustedDevicesControlRequest>(
                    Request.Payload)) {
                const auto Peers = TrustAuthority.ListTrustedPeers();
                desklink::ControlResponse Response{
                    Request.RequestId,
                    Peers ? desklink::ControlStatus::Ok
                          : desklink::ControlStatus::Failed};
                if (Peers) {
                    desklink::ControlTrustedDeviceList Devices;
                    Devices.Devices.reserve(Peers->size());
                    for (const auto& Peer : *Peers) {
                        Devices.Devices.push_back({
                            Peer.Identity.machine_id,
                            Peer.Identity.display_name,
                            Peer.Capabilities});
                    }
                    Response.TrustedDevices = std::move(Devices);
                }
                return Response;
            }
            if (const auto* Change = std::get_if<
                    desklink::RequestLocalPermissionChangeControlRequest>(
                    &Request.Payload)) {
                return desklink::ControlResponse{
                    Request.RequestId,
                    MapMutationStatus(TrustAuthority.RequestPermissionChange(
                        Change->Machine, Change->DesiredCapabilities))};
            }
            if (const auto* Forget = std::get_if<
                    desklink::ForgetTrustedDeviceControlRequest>(
                    &Request.Payload)) {
                return desklink::ControlResponse{
                    Request.RequestId,
                    MapMutationStatus(
                        TrustAuthority.ForgetPeer(Forget->Machine))};
            }
            if (std::holds_alternative<
                    desklink::GetPairingCandidateControlRequest>(
                    Request.Payload)) {
                const auto Candidate = PairingCandidates.Current(Clock.now());
                desklink::ControlResponse Response{
                    Request.RequestId,
                    Candidate ? desklink::ControlStatus::Ok
                              : desklink::ControlStatus::NotReady};
                if (Candidate) {
                    Response.PairingCandidate = desklink::ControlPairingCandidate{
                        Candidate->RequestId,
                        Candidate->Candidate.Identity.machine_id,
                        Candidate->Candidate.Identity.display_name,
                        Candidate->Candidate.VerificationCode,
                        Candidate->RequestedCapabilities};
                }
                return Response;
            }
            if (std::holds_alternative<desklink::ReturnLocalControlRequest>(
                    Request.Payload)) {
                return desklink::ControlResponse{
                    Request.RequestId,
                    SafetyController.ReturnLocalOnly()
                        ? desklink::ControlStatus::Ok
                        : desklink::ControlStatus::CleanupFailed};
            }
            if (std::holds_alternative<
                    desklink::PrepareForUpdateControlRequest>(
                    Request.Payload)) {
                const bool Stopped = SafetyController.StopActiveRuntime();
                if (Stopped) SetEvent(StopEvent.Get());
                return desklink::ControlResponse{
                    Request.RequestId,
                    Stopped ? desklink::ControlStatus::Ok
                            : desklink::ControlStatus::CleanupFailed};
            }

            const auto Forwarded = ForwardToActiveRuntime(Request);
            if (Forwarded) return *Forwarded;
            if (std::holds_alternative<desklink::GetStateControlRequest>(
                    Request.Payload)) {
                return desklink::ControlResponse{
                    Request.RequestId, desklink::ControlStatus::Ok,
                    LocalState(LocalMachine)};
            }
            return desklink::ControlResponse{
                Request.RequestId, desklink::ControlStatus::NotReady};
        }, kBrokerPipeInstance);
    if (!Server.Start()) {
        std::cerr << "[Broker:Control] could not create the current-user endpoint\n";
        return 1;
    }

    std::cout << "[Broker:Lifecycle] per-user runtime broker ready; input is Local\n";
    for (;;) {
        const auto Wait = WaitForSingleObject(StopEvent.Get(), 250);
        if (Wait == WAIT_OBJECT_0) break;
        if (Wait != WAIT_TIMEOUT) {
            std::cerr << "[Broker:Lifecycle] stop-event wait failed\n";
            Server.Stop();
            return 1;
        }
        if (Server.Running()) continue;
        Server.Stop();
        if (!Server.Start()) {
            std::cerr << "[Broker:Control] current-user endpoint recovery failed\n";
            return 1;
        }
        std::cerr << "[Broker:Control] current-user endpoint recovered after a transient failure\n";
    }
    Server.Stop();
    std::cout << "[Broker:Lifecycle] broker stopped after fail-local cleanup\n";
    return 0;
}
