#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#include <iphlpapi.h>
#include <powrprof.h>
#include <shlobj.h>

#include "desklink/control.hpp"
#include "desklink/discovery.hpp"
#include "desklink/runtime_broker.hpp"
#include "desklink/win32_application_settings.hpp"
#include "desklink/win32_control.hpp"
#include "desklink/win32_device_certificate.hpp"
#include "desklink/win32_discovery.hpp"
#include "desklink/win32_launcher.hpp"
#include "desklink/win32_pairing.hpp"
#include "desklink/win32_product_lifecycle.hpp"
#include "desklink/win32_roaming_settings.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kBrokerMutexName[] = L"Local\\DeskLink.RuntimeBroker.v1";
constexpr wchar_t kRuntimeMutexName[] = L"Local\\DeskLink.Runtime.v1";
constexpr wchar_t kDeviceKeyName[] = L"DeskLink-Device-Identity-v1";
constexpr wchar_t kBrokerPipeInstance[] = L"broker";
constexpr wchar_t kProductShellMutexName[] = L"Local\\DeskLink.Shell.v1";
constexpr std::uint16_t kProductionPort = 43'821;
constexpr auto kPairingCandidateLifetime = std::chrono::seconds(90);

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

class UniquePowerNotification final {
public:
    explicit UniquePowerNotification(HPOWERNOTIFY Value = nullptr) noexcept
        : Value_(Value) {}
    ~UniquePowerNotification() { Reset(); }
    UniquePowerNotification(const UniquePowerNotification&) = delete;
    UniquePowerNotification& operator=(const UniquePowerNotification&) = delete;
    void Reset(HPOWERNOTIFY Value = nullptr) noexcept {
        if (Value_) (void)UnregisterSuspendResumeNotification(Value_);
        Value_ = Value;
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return Value_ != nullptr;
    }

private:
    HPOWERNOTIFY Value_{};
};

constexpr ULONG kPowerSuspendPending = 1u << 0u;
constexpr ULONG kPowerResumePending = 1u << 1u;

struct PowerNotificationContext {
    HANDLE Event{};
    std::atomic_ulong Pending{};
};

ULONG CALLBACK OnPowerNotification(
    PVOID RawContext, ULONG Type, PVOID) noexcept {
    auto* Context = static_cast<PowerNotificationContext*>(RawContext);
    if (!Context || !Context->Event) return ERROR_INVALID_PARAMETER;
    ULONG Pending{};
    if (Type == PBT_APMSUSPEND) {
        Pending = kPowerSuspendPending;
    } else if (Type == PBT_APMRESUMEAUTOMATIC ||
               Type == PBT_APMRESUMESUSPEND ||
               Type == PBT_APMRESUMECRITICAL) {
        Pending = kPowerResumePending;
    } else {
        return ERROR_SUCCESS;
    }
    Context->Pending.fetch_or(Pending, std::memory_order_release);
    return SetEvent(Context->Event) ? ERROR_SUCCESS : GetLastError();
}

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

std::uint64_t RuntimeJitterSeed(
    const desklink::MachineId& Machine) noexcept {
    std::uint64_t Result = 0xcbf29ce484222325ull;
    for (const auto Byte : Machine) {
        Result ^= Byte;
        Result *= 0x100000001b3ull;
    }
    return Result;
}

class BrokerRuntimeSupervisor final {
public:
    BrokerRuntimeSupervisor(
        std::filesystem::path PairExecutable,
        std::filesystem::path DataDirectory,
        desklink::MachineId LocalMachine,
        desklink::DpapiTrustStore& TrustStore,
        desklink::Win32ProductPreferencesStore& PreferencesStore,
        BrokerRuntimeSafetyController& SafetyController,
        desklink::SteadyClock& Clock)
        : PairExecutable_(std::move(PairExecutable)),
          RoamingSettingsPath_(
              std::move(DataDirectory) / L"roaming.settings"),
          TrustStore_(TrustStore),
          PreferencesStore_(PreferencesStore),
          SafetyController_(SafetyController),
          Clock_(Clock),
          Reconnect_(RuntimeJitterSeed(LocalMachine)) {}

    ~BrokerRuntimeSupervisor() {
        if (Stop()) return;
        // Cleanup failure must not invoke std::terminate through a joinable
        // worker. Do not force-kill the transport: its own fail-local path is
        // safer than terminating while input state may be held.
        {
            std::scoped_lock Lock(Mutex_);
            Stopping_ = true;
            Transitioning_ = false;
        }
        Changed_.notify_all();
        if (Worker_.joinable()) Worker_.join();
    }

    [[nodiscard]] bool Start() {
        std::scoped_lock Lock(Mutex_);
        if (Worker_.joinable()) return false;
        Stopping_ = false;
        Worker_ = std::thread([this] { Run(); });
        return true;
    }

    [[nodiscard]] bool Stop() noexcept {
        {
            std::scoped_lock Lock(Mutex_);
            if (!Worker_.joinable()) return true;
            if (Transitioning_) return false;
            Transitioning_ = true;
            IntentionalStop_ = true;
        }
        Changed_.notify_all();
        const bool Stopped = StopManagedRuntime();
        {
            std::scoped_lock Lock(Mutex_);
            Transitioning_ = false;
            if (Stopped) {
                Stopping_ = true;
                Blocked_ = false;
            } else {
                IntentionalStop_ = false;
                Blocked_ = true;
                Reconnect_.ProcessStopped(
                    desklink::BrokerRuntimeFailure::Unknown, Clock_.now());
            }
        }
        Changed_.notify_all();
        if (!Stopped) return false;
        if (Worker_.joinable()) Worker_.join();
        return true;
    }

    [[nodiscard]] bool Pause() noexcept {
        {
            std::scoped_lock Lock(Mutex_);
            if (Stopping_ || Transitioning_) return false;
            Transitioning_ = true;
            IntentionalStop_ = true;
        }
        Changed_.notify_all();
        const bool Stopped = StopManagedRuntime();
        {
            std::scoped_lock Lock(Mutex_);
            Transitioning_ = false;
            if (Stopped) {
                Blocked_ = false;
                Reconnect_.Pause(Clock_.now());
            } else {
                IntentionalStop_ = false;
                Blocked_ = true;
                Reconnect_.ProcessStopped(
                    desklink::BrokerRuntimeFailure::Unknown, Clock_.now());
            }
        }
        Changed_.notify_all();
        return Stopped;
    }

    [[nodiscard]] bool Resume() noexcept {
        {
            std::scoped_lock Lock(Mutex_);
            if (Stopping_ || Transitioning_) return false;
            Transitioning_ = true;
            IntentionalStop_ = true;
        }
        Changed_.notify_all();
        const bool Stopped = StopManagedRuntime();
        {
            std::scoped_lock Lock(Mutex_);
            Transitioning_ = false;
            if (Stopped) {
                Blocked_ = false;
                Reconnect_.Resume(Clock_.now());
            } else {
                IntentionalStop_ = false;
                Blocked_ = true;
                Reconnect_.ProcessStopped(
                    desklink::BrokerRuntimeFailure::Unknown, Clock_.now());
            }
        }
        Changed_.notify_all();
        return Stopped;
    }

    [[nodiscard]] bool SystemSuspend() noexcept {
        {
            std::scoped_lock Lock(Mutex_);
            if (Reconnect_.Snapshot().SystemSuspended) return true;
            if (Stopping_ || Transitioning_ || Blocked_) return false;
            Transitioning_ = true;
            IntentionalStop_ = true;
        }
        Changed_.notify_all();
        const bool Stopped = StopManagedRuntime();
        {
            std::scoped_lock Lock(Mutex_);
            Transitioning_ = false;
            if (Stopped) {
                Blocked_ = false;
                Reconnect_.SystemSuspend(Clock_.now());
            } else {
                IntentionalStop_ = false;
                Blocked_ = true;
                Reconnect_.ProcessStopped(
                    desklink::BrokerRuntimeFailure::Unknown, Clock_.now());
            }
        }
        Changed_.notify_all();
        return Stopped;
    }

    [[nodiscard]] bool SystemResume() noexcept {
        bool WasSuspended{};
        bool WasActionRequired{};
        {
            std::scoped_lock Lock(Mutex_);
            if (Stopping_ || Transitioning_ || Blocked_) return false;
            const auto Runtime = Reconnect_.Snapshot();
            WasSuspended = Runtime.SystemSuspended;
            WasActionRequired =
                Runtime.Phase == desklink::BrokerRuntimePhase::ActionRequired;
            Transitioning_ = true;
            IntentionalStop_ = true;
        }
        Changed_.notify_all();
        // A resume notification can arrive after Windows suspended this
        // process before it consumed the suspend event. Stop again before any
        // reconnect so the next transport always starts Local with a fresh
        // TLS connection and session nonce.
        const bool Stopped = StopManagedRuntime();
        {
            std::scoped_lock Lock(Mutex_);
            Transitioning_ = false;
            if (Stopped) {
                Blocked_ = false;
                if (WasSuspended) {
                    Reconnect_.SystemResume(Clock_.now());
                } else if (!WasActionRequired) {
                    Reconnect_.ResetForConfiguration(Clock_.now());
                }
            } else {
                IntentionalStop_ = false;
                Blocked_ = true;
                Reconnect_.ProcessStopped(
                    desklink::BrokerRuntimeFailure::Unknown, Clock_.now());
            }
        }
        Changed_.notify_all();
        return Stopped;
    }

    [[nodiscard]] bool ConfigurationChanged() noexcept {
        {
            std::scoped_lock Lock(Mutex_);
            if (Stopping_ || Transitioning_) return false;
            Transitioning_ = true;
            IntentionalStop_ = true;
        }
        Changed_.notify_all();
        const bool Stopped = StopManagedRuntime();
        {
            std::scoped_lock Lock(Mutex_);
            Transitioning_ = false;
            if (Stopped) {
                Blocked_ = false;
                Reconnect_.ResetForConfiguration(Clock_.now());
            } else {
                IntentionalStop_ = false;
                Blocked_ = true;
                Reconnect_.ProcessStopped(
                    desklink::BrokerRuntimeFailure::Unknown, Clock_.now());
            }
        }
        Changed_.notify_all();
        return Stopped;
    }

    [[nodiscard]] bool BeginPairingOperation() noexcept {
        {
            std::scoped_lock Lock(Mutex_);
            if (Stopping_ || Transitioning_ || Blocked_) return false;
            Transitioning_ = true;
            IntentionalStop_ = true;
        }
        Changed_.notify_all();
        const bool Stopped = StopManagedRuntime();
        {
            std::scoped_lock Lock(Mutex_);
            if (!Stopped) {
                Transitioning_ = false;
                IntentionalStop_ = false;
                Blocked_ = true;
                Reconnect_.ProcessStopped(
                    desklink::BrokerRuntimeFailure::Unknown, Clock_.now());
            }
        }
        Changed_.notify_all();
        return Stopped;
    }

    void EndPairingOperation(bool RuntimeMayResume = true) noexcept {
        {
            std::scoped_lock Lock(Mutex_);
            if (!Transitioning_) return;
            Transitioning_ = false;
            IntentionalStop_ = false;
            Blocked_ = !RuntimeMayResume;
            if (RuntimeMayResume) {
                Reconnect_.ResetForConfiguration(Clock_.now());
            } else {
                Reconnect_.ProcessStopped(
                    desklink::BrokerRuntimeFailure::Unknown, Clock_.now());
            }
        }
        Changed_.notify_all();
    }

    void NetworkChanged() noexcept {
        {
            std::scoped_lock Lock(Mutex_);
            Reconnect_.NetworkChanged(Clock_.now());
        }
        Changed_.notify_all();
    }

    [[nodiscard]] desklink::BrokerRuntimeSnapshot Snapshot() const noexcept {
        std::scoped_lock Lock(Mutex_);
        return Reconnect_.Snapshot();
    }

    void ApplyState(desklink::ControlState& State) const noexcept {
        const auto Runtime = Snapshot();
        State.RuntimePhase = Runtime.Phase;
        State.RuntimeFailure = Runtime.Failure;
        State.RetryAttempt = Runtime.RetryAttempt;
        const auto Preferences = PreferencesStore_.Current();
        if (State.Role == desklink::ControlRole::Idle && Preferences) {
            if (Preferences->Role == desklink::DeskRole::Main) {
                State.Role = desklink::ControlRole::Host;
            } else if (Preferences->Role == desklink::DeskRole::Companion) {
                State.Role = desklink::ControlRole::Agent;
            } else if (Preferences->Role == desklink::DeskRole::Flexible) {
                State.Role = Preferences->AutoConnect &&
                        Preferences->PreferredPeerMachine
                    ? desklink::ControlRole::Host
                    : desklink::ControlRole::Agent;
            }
        }
    }

private:
    [[nodiscard]] bool IsStoppingOrPaused() const noexcept {
        std::scoped_lock Lock(Mutex_);
        return Stopping_ || Transitioning_ || Blocked_ ||
               Reconnect_.Snapshot().Paused;
    }

    [[nodiscard]] bool StopManagedRuntime() noexcept {
        UniqueHandle ProcessWait;
        {
            std::scoped_lock Lock(Mutex_);
            if (Process_) {
                HANDLE Duplicate{};
                if (!DuplicateHandle(
                        GetCurrentProcess(), Process_.Get(),
                        GetCurrentProcess(), &Duplicate, SYNCHRONIZE, FALSE,
                        0)) {
                    return false;
                }
                ProcessWait.Reset(Duplicate);
            }
        }
        if (!ProcessWait) return SafetyController_.StopActiveRuntime();

        const auto Deadline = GetTickCount64() + 5'000u;
        while (GetTickCount64() < Deadline) {
            if (WaitForSingleObject(ProcessWait.Get(), 0) == WAIT_OBJECT_0) {
                return true;
            }
            (void)SafetyController_.StopActiveRuntime();
            const auto Wait = WaitForSingleObject(ProcessWait.Get(), 100);
            if (Wait == WAIT_OBJECT_0) return true;
            if (Wait == WAIT_FAILED) return false;
            Sleep(25);
        }
        return WaitForSingleObject(ProcessWait.Get(), 0) == WAIT_OBJECT_0;
    }

    void WaitForWork(std::chrono::milliseconds Duration) {
        std::unique_lock Lock(Mutex_);
        Changed_.wait_for(Lock, Duration);
    }

    void RecordFailure(desklink::BrokerRuntimeFailure Failure) noexcept {
        {
            std::scoped_lock Lock(Mutex_);
            Reconnect_.ProcessStopped(Failure, Clock_.now());
        }
        Changed_.notify_all();
    }

    [[nodiscard]] bool Begin(desklink::BrokerRuntimePhase Phase) noexcept {
        std::scoped_lock Lock(Mutex_);
        return !Stopping_ && !Transitioning_ && !Blocked_ &&
               Reconnect_.Begin(Phase);
    }

    [[nodiscard]] bool Launch(
        const desklink::LauncherRequest& Request,
        const desklink::ProductPreferences& Preferences) {
        if (!std::filesystem::is_regular_file(PairExecutable_)) {
            std::cerr
                << "[Broker:Process] desklink_pair.exe is unavailable\n";
            RecordFailure(desklink::BrokerRuntimeFailure::Unknown);
            return false;
        }
        // Product operations use the reviewed OS policy: Schannel on Windows
        // 11 / Server 2022+, OpenSSL/CNG on Windows 10. MsQuic runtime loading
        // is fail-closed and never retries with the other provider.
        const auto Arguments =
            desklink::BuildProductLauncherArguments(Request);
        const auto CommandLine = Arguments
            ? desklink::BuildWindowsCommandLine(
                  PairExecutable_.native(), *Arguments)
            : std::nullopt;
        if (!CommandLine) {
            std::cerr
                << "[Broker:Process] managed child arguments were rejected\n";
            RecordFailure(desklink::BrokerRuntimeFailure::Protocol);
            return false;
        }

        auto MutableCommandLine = *CommandLine;
        STARTUPINFOW Startup{};
        Startup.cb = sizeof(Startup);
        PROCESS_INFORMATION Process{};
        const auto WorkingDirectory = PairExecutable_.parent_path().native();
        std::unique_lock RuntimeLock(Mutex_);
        if (Stopping_ || Transitioning_ || Blocked_) return false;
        if (!CreateProcessW(
                PairExecutable_.c_str(), MutableCommandLine.data(), nullptr,
                nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                WorkingDirectory.c_str(), &Startup, &Process)) {
            const auto Error = GetLastError();
            RuntimeLock.unlock();
            std::cerr << "[Broker:Process] managed child launch failed; error="
                      << Error << '\n';
            RecordFailure(desklink::BrokerRuntimeFailure::Unknown);
            return false;
        }
        CloseHandle(Process.hThread);
        Process_.Reset(Process.hProcess);
        ChildPreferences_ = Preferences;
        RoamingArmed_ = Request.CaptureInput &&
            Request.ProfileDefaultMode == desklink::DeskMode::Roam;
        AudioGainApplied_ = false;
        RuntimeLock.unlock();
        std::cout << "[Broker:Process] managed transport started; pid="
                  << Process.dwProcessId << '\n';
        return true;
    }

    [[nodiscard]] bool StartListener(
        const desklink::ProductPreferences& Preferences) {
        if (!Begin(desklink::BrokerRuntimePhase::Listening)) return false;
        desklink::LauncherRequest Request;
        Request.Operation = desklink::LauncherOperation::Serve;
        Request.Port = kProductionPort;
        Request.BrokerManaged = true;
        Request.SyncClipboard = Preferences.ClipboardDesired;
        Request.SendAudio =
            Preferences.AudioRoute ==
                desklink::AudioRoutePreference::LocalToPeer ||
            Preferences.AudioRoute ==
                desklink::AudioRoutePreference::Bidirectional;
        return Launch(Request, Preferences);
    }

    [[nodiscard]] bool StartPreferredConnection(
        const desklink::ProductPreferences& Preferences) {
        if (!Preferences.PreferredPeerMachine) {
            RecordFailure(desklink::BrokerRuntimeFailure::Identity);
            return false;
        }
        if (!TrustStore_.GetPeer(*Preferences.PreferredPeerMachine)) {
            std::cerr
                << "[Broker:Security] preferred peer is not trusted\n";
            RecordFailure(desklink::BrokerRuntimeFailure::Identity);
            return false;
        }
        if (!Begin(desklink::BrokerRuntimePhase::Discovering)) return false;
        const auto Browse = desklink::Win32MdnsBrowser::Browse(
            std::chrono::seconds(1));
        if (IsStoppingOrPaused()) return false;

        const auto Match = std::find_if(
            Browse.Peers.begin(), Browse.Peers.end(), [&](const auto& Peer) {
                return Peer.Endpoint.Advertisement.Machine ==
                    *Preferences.PreferredPeerMachine;
            });
        if (Match == Browse.Peers.end()) {
            RecordFailure(
                desklink::BrokerRuntimeFailure::OrdinaryUnavailable);
            return false;
        }
        if (Match->Ambiguous || Match->EndpointCount == 0) {
            std::cerr
                << "[Broker:Security] preferred discovery identity is ambiguous\n";
            RecordFailure(desklink::BrokerRuntimeFailure::Identity);
            return false;
        }
        if (Match->Endpoint.Advertisement.ProtocolVersion !=
            desklink::kProtocolVersion) {
            std::cerr
                << "[Broker:Protocol] preferred peer protocol is incompatible\n";
            RecordFailure(desklink::BrokerRuntimeFailure::Protocol);
            return false;
        }
        const auto Host = Utf8ToWide(Match->Endpoint.HostName);
        if (!Host || Match->Endpoint.Advertisement.Port == 0) {
            RecordFailure(desklink::BrokerRuntimeFailure::Protocol);
            return false;
        }
        if (!Begin(desklink::BrokerRuntimePhase::Connecting)) return false;

        desklink::LauncherRequest Request;
        Request.Operation = desklink::LauncherOperation::Focus;
        Request.Host = *Host;
        Request.Port = Match->Endpoint.Advertisement.Port;
        Request.ExpectedPeerMachine = *Preferences.PreferredPeerMachine;
        Request.BrokerManaged = true;
        Request.SyncClipboard = Preferences.ClipboardDesired;
        Request.ReceiveAudio =
            Preferences.AudioRoute ==
                desklink::AudioRoutePreference::PeerToLocal ||
            Preferences.AudioRoute ==
                desklink::AudioRoutePreference::Bidirectional;
        if (Preferences.InputRoamingDesired &&
            std::filesystem::is_regular_file(RoamingSettingsPath_)) {
            Request.CaptureInput = true;
            Request.EdgeRoamingSettingsPath = RoamingSettingsPath_;
            Request.ProfileDefaultMode = desklink::DeskMode::Roam;
            Request.KeepLocalWhenFullscreen =
                Preferences.Gaming == desklink::GamingBehavior::KeepLocal;
            Request.ProfileRules = Preferences.ProfileRules;
        }
        return Launch(Request, Preferences);
    }

    void StopChildAfterControlFailure(
        desklink::BrokerRuntimeFailure Failure) noexcept {
        {
            std::scoped_lock Lock(Mutex_);
            Transitioning_ = true;
            IntentionalStop_ = true;
            Reconnect_.ProcessStopped(Failure, Clock_.now());
        }
        const bool Stopped = StopManagedRuntime();
        {
            std::scoped_lock Lock(Mutex_);
            Transitioning_ = false;
            Blocked_ = !Stopped;
            if (!Stopped) IntentionalStop_ = false;
        }
        Changed_.notify_all();
    }

    void PollChildState() {
        const auto Response = ForwardToActiveRuntime(
            desklink::ControlRequest{
                NextRequestId_.fetch_add(1),
                desklink::GetStateControlRequest{}},
            std::chrono::milliseconds{250});
        if (!Response || Response->Status != desklink::ControlStatus::Ok ||
            !Response->State || Response->State->ConnectedPeerCount == 0) {
            return;
        }

        bool ArmRoaming = false;
        bool ApplyGain = false;
        std::uint16_t Gain{};
        {
            std::scoped_lock Lock(Mutex_);
            Reconnect_.ConnectedLocal();
            ArmRoaming = ChildPreferences_.InputRoamingDesired &&
                         !RoamingArmed_ &&
                         std::filesystem::is_regular_file(
                             RoamingSettingsPath_);
            ApplyGain = !AudioGainApplied_;
            Gain = ChildPreferences_.AudioGainPermyriad;
        }
        if (ApplyGain) {
            const auto GainResponse = ForwardToActiveRuntime(
                desklink::ControlRequest{
                    NextRequestId_.fetch_add(1),
                    desklink::SetAudioGainControlRequest{Gain}});
            if (!GainResponse ||
                GainResponse->Status != desklink::ControlStatus::Ok) {
                std::cerr
                    << "[Broker:Audio] saved peer gain could not be applied; "
                       "audio policy remains unchanged and the input session stays active\n";
            }
            std::scoped_lock Lock(Mutex_);
            AudioGainApplied_ = true;
        }
        if (ArmRoaming) {
            const auto ModeResponse = ForwardToActiveRuntime(
                desklink::ControlRequest{
                    NextRequestId_.fetch_add(1),
                    desklink::SetDesiredModeControlRequest{
                        desklink::DeskMode::Roam}});
            if (!ModeResponse ||
                ModeResponse->Status != desklink::ControlStatus::Ok) {
                StopChildAfterControlFailure(
                    desklink::BrokerRuntimeFailure::Capability);
                return;
            }
            std::scoped_lock Lock(Mutex_);
            RoamingArmed_ = true;
            std::cout
                << "[Broker:Roaming] authenticated edge roaming armed; focus remains Local\n";
        }
    }

    [[nodiscard]] bool ObserveChildExit() {
        HANDLE Process{};
        {
            std::scoped_lock Lock(Mutex_);
            Process = Process_.Get();
        }
        if (!Process) return false;
        DWORD ExitCode{};
        if (!GetExitCodeProcess(Process, &ExitCode)) {
            StopChildAfterControlFailure(
                desklink::BrokerRuntimeFailure::Unknown);
            return true;
        }
        if (ExitCode == STILL_ACTIVE) {
            {
                std::scoped_lock Lock(Mutex_);
                if (Stopping_) return false;
                if (Transitioning_ || Blocked_) return true;
            }
            PollChildState();
            return true;
        }

        bool Intentional{};
        {
            std::scoped_lock Lock(Mutex_);
            Process_.Reset();
            Intentional = std::exchange(IntentionalStop_, false);
        }
        std::cout << "[Broker:Process] managed transport exited; code="
                  << ExitCode << '\n';
        if (!Intentional) {
            RecordFailure(
                desklink::ClassifyBrokerManagedProcessExit(ExitCode));
        }
        return true;
    }

    void Run() noexcept {
        try {
            for (;;) {
                if (ObserveChildExit()) {
                    WaitForWork(std::chrono::milliseconds(200));
                    continue;
                }

                desklink::BrokerRuntimeSnapshot Runtime;
                bool Defer{};
                {
                    std::scoped_lock Lock(Mutex_);
                    if (Stopping_) break;
                    Defer = Transitioning_ || Blocked_;
                    Runtime = Reconnect_.Snapshot();
                }
                if (Defer) {
                    // A failed cleanup or an in-flight transition must never
                    // launch or reconfigure another runtime owner.
                    WaitForWork(std::chrono::milliseconds(500));
                    continue;
                }
                if (!ReconnectAttemptDue(Runtime)) {
                    WaitForWork(std::chrono::milliseconds(500));
                    continue;
                }

                const auto Preferences = PreferencesStore_.Current();
                if (!Preferences ||
                    !desklink::IsValidProductPreferences(*Preferences) ||
                    !Preferences->AutoStartRuntime ||
                    Preferences->Role == desklink::DeskRole::Unconfigured) {
                    {
                        std::scoped_lock Lock(Mutex_);
                        Reconnect_.ResetForConfiguration(Clock_.now());
                    }
                    WaitForWork(std::chrono::seconds(1));
                    continue;
                }

                const bool Connect = Preferences->AutoConnect &&
                    Preferences->PreferredPeerMachine &&
                    (Preferences->Role == desklink::DeskRole::Main ||
                     Preferences->Role == desklink::DeskRole::Flexible);
                if (Connect) {
                    (void)StartPreferredConnection(*Preferences);
                } else if (Preferences->Role ==
                               desklink::DeskRole::Companion ||
                           Preferences->Role ==
                               desklink::DeskRole::Flexible) {
                    (void)StartListener(*Preferences);
                } else {
                    {
                        std::scoped_lock Lock(Mutex_);
                        Reconnect_.ResetForConfiguration(Clock_.now());
                    }
                    WaitForWork(std::chrono::seconds(1));
                }
            }
        } catch (...) {
            std::cerr
                << "[Broker:Lifecycle] supervisor failed; runtime remains Local\n";
            RecordFailure(desklink::BrokerRuntimeFailure::Unknown);
        }
    }

    [[nodiscard]] bool ReconnectAttemptDue(
        const desklink::BrokerRuntimeSnapshot& Runtime) const noexcept {
        if (Runtime.Paused) return false;
        return Runtime.Phase == desklink::BrokerRuntimePhase::Stopped ||
            (Runtime.Phase == desklink::BrokerRuntimePhase::RetryWaiting &&
             Clock_.now() >= Runtime.RetryAt);
    }

    std::filesystem::path PairExecutable_;
    std::filesystem::path RoamingSettingsPath_;
    desklink::DpapiTrustStore& TrustStore_;
    desklink::Win32ProductPreferencesStore& PreferencesStore_;
    BrokerRuntimeSafetyController& SafetyController_;
    desklink::SteadyClock& Clock_;
    mutable std::mutex Mutex_;
    std::condition_variable Changed_;
    std::thread Worker_;
    UniqueHandle Process_;
    desklink::BrokerReconnectController Reconnect_;
    desklink::ProductPreferences ChildPreferences_;
    std::atomic_uint64_t NextRequestId_{0xC000'0000u};
    bool Stopping_{};
    bool Transitioning_{};
    bool Blocked_{};
    bool IntentionalStop_{};
    bool RoamingArmed_{};
    bool AudioGainApplied_{};
};

class BrokerDiscoveryController final {
public:
    ~BrokerDiscoveryController() { Stop(); }

    [[nodiscard]] bool Start(std::chrono::seconds Duration) {
        std::thread Previous;
        std::uint64_t Generation{};
        std::stop_token StopToken;
        {
            std::scoped_lock Lock(Mutex_);
            if (Running_) return false;
            if (Worker_.joinable()) Previous = std::move(Worker_);
            ++Generation_;
            Generation = Generation_;
            StopSource_ = std::stop_source{};
            StopToken = StopSource_.get_token();
            Running_ = true;
            Snapshot_ = {};
            Snapshot_.Phase = desklink::ControlDiscoveryPhase::Searching;
            Worker_ = std::thread(
                [this, Duration, Generation, StopToken] {
                    Run(Duration, Generation, StopToken);
                });
        }
        if (Previous.joinable()) Previous.join();
        return true;
    }

    void Stop() noexcept {
        std::thread Worker;
        {
            std::scoped_lock Lock(Mutex_);
            ++Generation_;
            StopSource_.request_stop();
            Running_ = false;
            Snapshot_ = {};
            if (Worker_.joinable()) Worker = std::move(Worker_);
        }
        if (Worker.joinable()) Worker.join();
    }

    [[nodiscard]] desklink::ControlNearbyPeerList Snapshot() const {
        std::scoped_lock Lock(Mutex_);
        return Snapshot_;
    }

    [[nodiscard]] std::optional<desklink::ControlNearbyPeer> PairablePeer(
        const desklink::MachineId& Machine) const {
        std::scoped_lock Lock(Mutex_);
        if (Snapshot_.Phase != desklink::ControlDiscoveryPhase::Complete) {
            return std::nullopt;
        }
        const auto Match = std::find_if(
            Snapshot_.Peers.begin(), Snapshot_.Peers.end(),
            [&](const auto& Peer) { return Peer.Machine == Machine; });
        if (Match == Snapshot_.Peers.end() || Match->Ambiguous ||
            !Match->PairingOpen || Match->EndpointCount == 0 ||
            Match->ProtocolVersion != desklink::kProtocolVersion) {
            return std::nullopt;
        }
        return *Match;
    }

private:
    void Run(
        std::chrono::seconds Duration,
        std::uint64_t Generation,
        std::stop_token StopToken) {
        const auto Browse = desklink::Win32MdnsBrowser::Browse(
            Duration, StopToken);
        desklink::ControlNearbyPeerList Result;
        Result.Phase = Browse.StartStatus == ERROR_SUCCESS
            ? desklink::ControlDiscoveryPhase::Complete
            : desklink::ControlDiscoveryPhase::Failed;
        if (Result.Phase == desklink::ControlDiscoveryPhase::Complete) {
            Result.Peers.reserve(std::min<std::size_t>(
                Browse.Peers.size(), desklink::kMaximumControlNearbyPeers));
            for (const auto& Peer : Browse.Peers) {
                if (Result.Peers.size() >=
                    desklink::kMaximumControlNearbyPeers) {
                    break;
                }
                Result.Peers.push_back({
                    Peer.Endpoint.Advertisement.Machine,
                    Peer.Endpoint.Advertisement.DisplayName,
                    Peer.Endpoint.HostName,
                    desklink::CapabilitySet(
                        Peer.Endpoint.Advertisement.CapabilityHints),
                    Peer.Endpoint.Advertisement.Port,
                    Peer.Endpoint.Advertisement.ProtocolVersion,
                    static_cast<std::uint8_t>(std::min<std::size_t>(
                        Peer.EndpointCount, 255u)),
                    Peer.Endpoint.Advertisement.PairingAvailable,
                    Peer.Ambiguous});
            }
            if (!desklink::IsValidControlNearbyPeerList(Result)) {
                Result = {};
                Result.Phase = desklink::ControlDiscoveryPhase::Failed;
            }
        }
        std::scoped_lock Lock(Mutex_);
        if (Generation != Generation_) return;
        Snapshot_ = std::move(Result);
        Running_ = false;
    }

    mutable std::mutex Mutex_;
    std::thread Worker_;
    desklink::ControlNearbyPeerList Snapshot_;
    std::stop_source StopSource_;
    std::uint64_t Generation_{};
    bool Running_{};
};

bool ProductShellPresent() noexcept {
    const auto Shell = OpenMutexW(
        SYNCHRONIZE, FALSE, kProductShellMutexName);
    if (!Shell) return false;
    CloseHandle(Shell);
    return true;
}

class BrokerPairingOrchestrator final {
public:
    BrokerPairingOrchestrator(
        std::filesystem::path PairExecutable,
        BrokerRuntimeSupervisor& Supervisor,
        desklink::RuntimeTrustAuthority& TrustAuthority,
        desklink::BrokerPairingCandidateLease& Candidates,
        desklink::SteadyClock& Clock)
        : PairExecutable_(std::move(PairExecutable)),
          Supervisor_(Supervisor),
          TrustAuthority_(TrustAuthority),
          Candidates_(Candidates),
          Clock_(Clock) {}

    ~BrokerPairingOrchestrator() { Stop(); }

    [[nodiscard]] bool OpenWindow(
        std::uint16_t Port, desklink::CapabilitySet Capabilities) {
        desklink::LauncherRequest Request;
        Request.Operation = desklink::LauncherOperation::PairListen;
        Request.Port = Port;
        ApplyCapabilities(Request, Capabilities);
        return Start(
            std::move(Request), desklink::ControlPairingSource::Incoming,
            Capabilities);
    }

    [[nodiscard]] bool PairNearby(
        const desklink::ControlNearbyPeer& Peer,
        desklink::CapabilitySet Capabilities) {
        const auto Host = Utf8ToWide(Peer.HostName);
        if (!Host) return false;
        desklink::LauncherRequest Request;
        Request.Operation = desklink::LauncherOperation::PairConnect;
        Request.Host = *Host;
        Request.Port = Peer.Port;
        ApplyCapabilities(Request, Capabilities);
        return Start(
            std::move(Request), desklink::ControlPairingSource::Nearby,
            Capabilities);
    }

    [[nodiscard]] bool PairManual(
        std::string_view Host,
        std::uint16_t Port,
        desklink::CapabilitySet Capabilities) {
        const auto WideHost = Utf8ToWide(Host);
        if (!WideHost) return false;
        desklink::LauncherRequest Request;
        Request.Operation = desklink::LauncherOperation::PairConnect;
        Request.Host = *WideHost;
        Request.Port = Port;
        ApplyCapabilities(Request, Capabilities);
        return Start(
            std::move(Request), desklink::ControlPairingSource::Manual,
            Capabilities);
    }

    [[nodiscard]] bool Present(
        const desklink::PresentManagedPairingCandidateControlRequest&
            Request) {
        std::scoped_lock Lock(Mutex_);
        if (!Operation_ || !Operation_->Active ||
            Operation_->OperationId != Request.OperationId ||
            Operation_->Token != Request.Token ||
            Operation_->Decision !=
                desklink::ControlManagedPairingDecision::Pending ||
            Operation_->CandidatePresented ||
            Operation_->RequestedCapabilities.bits() !=
                Request.RequestedCapabilities.bits() ||
            !ProductShellPresent()) {
            return false;
        }
        desklink::PairingCandidate Candidate;
        Candidate.Status = desklink::PairingStatus::Ready;
        Candidate.Identity = {
            Request.Machine,
            Request.DisplayName,
            Request.CertificateFingerprint};
        Candidate.VerificationCode = Request.VerificationCode;
        Candidate.TranscriptDigest = Request.TranscriptDigest;
        if (!Candidates_.Present(
                desklink::BrokerPairingCandidate{
                    Request.OperationId,
                    Request.OperationId,
                    std::move(Candidate),
                    Request.RequestedCapabilities,
                    Clock_.now() + kPairingCandidateLifetime},
                Clock_.now())) {
            return false;
        }
        Operation_->CandidatePresented = true;
        return true;
    }

    [[nodiscard]] bool Resolve(std::uint64_t OperationId, bool Approved) {
        std::scoped_lock Lock(Mutex_);
        if (!Operation_ || !Operation_->Active ||
            Operation_->OperationId != OperationId ||
            Operation_->Decision !=
                desklink::ControlManagedPairingDecision::Pending ||
            !Operation_->CandidatePresented) {
            return false;
        }
        const auto Current = Candidates_.Current(Clock_.now());
        if (!Current || Current->RequestId != OperationId) return false;
        const auto Resolved = Candidates_.ResolveLocally(
            OperationId, Approved, Clock_.now());
        if (Approved && !Resolved) return false;
        Operation_->Decision = Approved
            ? desklink::ControlManagedPairingDecision::Approved
            : desklink::ControlManagedPairingDecision::Rejected;
        return true;
    }

    [[nodiscard]] std::optional<desklink::ControlManagedPairingDecision>
    Decision(
        const desklink::GetManagedPairingDecisionControlRequest& Request) {
        std::scoped_lock Lock(Mutex_);
        if (!Operation_ || !Operation_->Active ||
            Operation_->OperationId != Request.OperationId ||
            Operation_->Token != Request.Token) {
            return std::nullopt;
        }
        if (Operation_->Decision ==
                desklink::ControlManagedPairingDecision::Pending &&
            Operation_->CandidatePresented &&
            (!ProductShellPresent() ||
             !Candidates_.Current(Clock_.now()))) {
            Candidates_.Reject(Request.OperationId);
            Operation_->Decision =
                desklink::ControlManagedPairingDecision::Rejected;
        }
        return Operation_->Decision;
    }

    [[nodiscard]] desklink::ControlPairingSource Source(
        std::uint64_t OperationId) const noexcept {
        std::scoped_lock Lock(Mutex_);
        return Operation_ && Operation_->OperationId == OperationId
            ? Operation_->Source
            : desklink::ControlPairingSource::Incoming;
    }

    void Stop() noexcept {
        UniqueHandle Process;
        std::thread Worker;
        {
            std::scoped_lock Lock(Mutex_);
            Stopping_ = true;
            if (Operation_) {
                Candidates_.Reject(Operation_->OperationId);
                Operation_->Decision =
                    desklink::ControlManagedPairingDecision::Rejected;
            }
            if (Process_) {
                HANDLE Duplicate{};
                if (DuplicateHandle(
                        GetCurrentProcess(), Process_.Get(),
                        GetCurrentProcess(), &Duplicate,
                        SYNCHRONIZE | PROCESS_TERMINATE, FALSE, 0)) {
                    Process.Reset(Duplicate);
                }
            }
            if (Worker_.joinable()) Worker = std::move(Worker_);
        }
        if (Process && WaitForSingleObject(Process.Get(), 2'000) == WAIT_TIMEOUT) {
            (void)TerminateProcess(Process.Get(), 1);
        }
        if (Worker.joinable()) Worker.join();
    }

private:
    struct OperationState {
        std::uint64_t OperationId{};
        desklink::ControlPairingToken Token{};
        desklink::ControlPairingSource Source{
            desklink::ControlPairingSource::Incoming};
        desklink::CapabilitySet RequestedCapabilities;
        desklink::ControlManagedPairingDecision Decision{
            desklink::ControlManagedPairingDecision::Pending};
        bool CandidatePresented{};
        bool Active{};
    };

    static void ApplyCapabilities(
        desklink::LauncherRequest& Request,
        desklink::CapabilitySet Capabilities) noexcept {
        Request.GrantInput = Capabilities.contains(
            desklink::Capability::InputInject);
        Request.GrantAudioSend = Capabilities.contains(
            desklink::Capability::AudioSend);
        Request.GrantAudioReceive = Capabilities.contains(
            desklink::Capability::AudioReceive);
        Request.GrantTopology = Capabilities.contains(
            desklink::Capability::DisplayTopologyExchange);
        Request.GrantClipboardRead = Capabilities.contains(
            desklink::Capability::ClipboardRead);
        Request.GrantClipboardWrite = Capabilities.contains(
            desklink::Capability::ClipboardWrite);
    }

    [[nodiscard]] static std::optional<OperationState> NewOperation(
        desklink::ControlPairingSource Source,
        desklink::CapabilitySet RequestedCapabilities) noexcept {
        std::array<std::uint8_t, sizeof(std::uint64_t) +
            desklink::kControlPairingTokenSize> Random{};
        if (BCryptGenRandom(
                nullptr, Random.data(), static_cast<ULONG>(Random.size()),
                BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            return std::nullopt;
        }
        OperationState Result;
        std::copy_n(
            Random.begin(), sizeof(Result.OperationId),
            reinterpret_cast<std::uint8_t*>(&Result.OperationId));
        std::copy_n(
            Random.begin() + sizeof(Result.OperationId),
            Result.Token.size(), Result.Token.begin());
        if (Result.OperationId == 0 ||
            std::all_of(Result.Token.begin(), Result.Token.end(),
                        [](std::uint8_t Byte) { return Byte == 0; })) {
            return std::nullopt;
        }
        Result.Source = Source;
        Result.RequestedCapabilities = RequestedCapabilities;
        Result.Active = true;
        return Result;
    }

    [[nodiscard]] bool Start(
        desklink::LauncherRequest Request,
        desklink::ControlPairingSource Source,
        desklink::CapabilitySet RequestedCapabilities) {
        std::thread Previous;
        {
            std::scoped_lock Lock(Mutex_);
            if (Stopping_ || Starting_ ||
                (Operation_ && Operation_->Active)) {
                return false;
            }
            Starting_ = true;
            if (Worker_.joinable()) Previous = std::move(Worker_);
        }
        if (Previous.joinable()) Previous.join();
        const auto Operation = NewOperation(Source, RequestedCapabilities);
        if (!Operation || !Supervisor_.BeginPairingOperation()) {
            std::scoped_lock Lock(Mutex_);
            Starting_ = false;
            return false;
        }
        {
            std::scoped_lock Lock(Mutex_);
            if (Stopping_) {
                Starting_ = false;
                Supervisor_.EndPairingOperation();
                return false;
            }
        }
        Request.BrokerPairingOperationId = Operation->OperationId;
        Request.BrokerPairingToken = Operation->Token;
        const auto Arguments =
            desklink::BuildProductLauncherArguments(std::move(Request));
        const auto CommandLine = Arguments
            ? desklink::BuildWindowsCommandLine(
                  PairExecutable_.native(), *Arguments)
            : std::nullopt;
        if (!CommandLine ||
            !std::filesystem::is_regular_file(PairExecutable_)) {
            Supervisor_.EndPairingOperation();
            std::scoped_lock Lock(Mutex_);
            Starting_ = false;
            return false;
        }

        auto MutableCommandLine = *CommandLine;
        STARTUPINFOW Startup{};
        Startup.cb = sizeof(Startup);
        PROCESS_INFORMATION Process{};
        const auto WorkingDirectory = PairExecutable_.parent_path().native();
        if (!CreateProcessW(
                PairExecutable_.c_str(), MutableCommandLine.data(), nullptr,
                nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                WorkingDirectory.c_str(), &Startup, &Process)) {
            Supervisor_.EndPairingOperation();
            std::scoped_lock Lock(Mutex_);
            Starting_ = false;
            return false;
        }
        CloseHandle(Process.hThread);
        {
            std::scoped_lock Lock(Mutex_);
            if (Stopping_) {
                (void)TerminateProcess(Process.hProcess, 1);
                CloseHandle(Process.hProcess);
                Starting_ = false;
                Supervisor_.EndPairingOperation();
                return false;
            }
            Process_.Reset(Process.hProcess);
            Operation_ = *Operation;
            Starting_ = false;
            Worker_ = std::thread([this, OperationId = Operation->OperationId] {
                HANDLE ProcessHandle{};
                {
                    std::scoped_lock WorkerLock(Mutex_);
                    ProcessHandle = Process_.Get();
                }
                const auto Wait = WaitForSingleObject(ProcessHandle, INFINITE);
                DWORD ExitCode = 1;
                if (Wait == WAIT_OBJECT_0) {
                    (void)GetExitCodeProcess(ProcessHandle, &ExitCode);
                }
                Candidates_.Reject(OperationId);
                const bool TrustReady = ExitCode != 0 ||
                    TrustAuthority_.ReloadAfterExternalPairing();
                if (!TrustReady) {
                    std::cerr
                        << "[Broker:Pairing] trust reload failed; runtime remains Local\n";
                }
                {
                    std::scoped_lock WorkerLock(Mutex_);
                    Process_.Reset();
                    if (Operation_ &&
                        Operation_->OperationId == OperationId) {
                        Operation_->Active = false;
                    }
                }
                Supervisor_.EndPairingOperation(TrustReady);
                std::cout << "[Broker:Pairing] managed pairing exited; code="
                          << ExitCode << '\n';
            });
        }
        std::cout << "[Broker:Pairing] managed pairing started; pid="
                  << Process.dwProcessId << '\n';
        return true;
    }

    std::filesystem::path PairExecutable_;
    BrokerRuntimeSupervisor& Supervisor_;
    desklink::RuntimeTrustAuthority& TrustAuthority_;
    desklink::BrokerPairingCandidateLease& Candidates_;
    desklink::SteadyClock& Clock_;
    mutable std::mutex Mutex_;
    std::thread Worker_;
    UniqueHandle Process_;
    std::optional<OperationState> Operation_;
    bool Starting_{};
    bool Stopping_{};
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

bool ArmNetworkChangeNotification(
    HANDLE Event, OVERLAPPED& Overlapped,
    HANDLE& Notification) noexcept {
    Overlapped = {};
    Overlapped.hEvent = Event;
    Notification = nullptr;
    const auto Status = NotifyAddrChange(&Notification, &Overlapped);
    if (Status == ERROR_IO_PENDING) return true;
    if (Status == NO_ERROR) {
        SetEvent(Event);
        return true;
    }
    SetLastError(Status);
    return false;
}

bool ValidateInstalledBrokerForUpdate() {
    if (!desklink::IsWin32DeskLinkUpdateValidationActive()) return false;
    const auto DataDirectory = GetDataDirectory();
    const auto Executable = GetExecutablePath();
    if (!DataDirectory || !Executable) return false;
    const auto Root = Executable->parent_path();
    for (const auto* Relative : {
             L"desklink.exe", L"desklink_alpha.exe", L"desklink_pair.exe",
             L"desklink_runtime.exe", L"desklink_update.exe",
             L"runtime\\schannel\\msquic.dll"}) {
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

    desklink::BCryptPairingCrypto Crypto;
    auto Certificate = desklink::Win32DeviceCertificate::Load(
        kDeviceKeyName, Crypto);
    const auto Snapshot = Certificate
        ? Certificate->IdentitySnapshot(Crypto) : std::nullopt;
    if (!Snapshot || Snapshot->ExportPolicy != 0) return false;

    desklink::DpapiTrustStore TrustStore(*DataDirectory / L"trust.db");
    desklink::Win32ProductPreferencesStore PreferencesStore(
        *DataDirectory / L"application.settings");
    desklink::Win32RoamingSettingsStore RoamingStore(
        *DataDirectory / L"roaming.settings");
    return TrustStore.Load() && PreferencesStore.Load() &&
           RoamingStore.Load();
}

} // namespace

int wmain(int Count, wchar_t** Values) {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    if (Count == 2 && std::wstring_view(Values[1]) == L"--validate-update") {
        return ValidateInstalledBrokerForUpdate() ? 0 : 1;
    }
    if (Count != 1) {
        std::cerr << "[Broker:Lifecycle] unexpected command-line argument\n";
        return 2;
    }

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
    const auto ProductShellPath =
        desklink::GetWin32ProductShellExecutable(*Executable)
            .value_or(AlphaPath);
    const auto PairPath = Executable->parent_path() / L"desklink_pair.exe";

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
        TrustStore, SafetyController, [&TrustStore] {
            return TrustStore.Load();
        });
    desklink::BrokerPairingCandidateLease PairingCandidates;
    desklink::SteadyClock Clock;
    BrokerRuntimeSupervisor Supervisor(
        PairPath, *DataDirectory, LocalMachine, TrustStore, PreferencesStore,
        SafetyController, Clock);
    BrokerDiscoveryController Discovery;
    BrokerPairingOrchestrator Pairing(
        PairPath, Supervisor, TrustAuthority, PairingCandidates, Clock);
    UniqueHandle StopEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    UniqueHandle NetworkEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    UniqueHandle PowerEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!StopEvent || !NetworkEvent || !PowerEvent) return 1;
    HANDLE NetworkNotification{};
    OVERLAPPED NetworkOverlapped{};
    bool NetworkNotificationArmed = ArmNetworkChangeNotification(
        NetworkEvent.Get(), NetworkOverlapped, NetworkNotification);
    if (!NetworkNotificationArmed) {
        std::cerr
            << "[Broker:Network] address-change notification unavailable; error="
            << GetLastError() << '\n';
    }
    PowerNotificationContext PowerContext{PowerEvent.Get()};
    DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS PowerParameters{
        OnPowerNotification, &PowerContext};
    UniquePowerNotification PowerNotification(
        RegisterSuspendResumeNotification(
            &PowerParameters, DEVICE_NOTIFY_CALLBACK));
    if (!PowerNotification) {
        const auto PowerError = GetLastError();
        if (NetworkNotificationArmed) {
            (void)CancelIPChangeNotify(&NetworkOverlapped);
        }
        std::cerr
            << "[Broker:Power] suspend/resume notification unavailable; input remains Local; error="
            << PowerError << '\n';
        return 1;
    }

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
                const bool StartupSaved =
                    desklink::IsSafeWin32ProductFile(ProductShellPath) &&
                    desklink::SetWin32RunAtLogin(
                        Set->Preferences.RunAtLogin, ProductShellPath);
                const bool Saved = StartupSaved &&
                    PreferencesStore.Save(Set->Preferences);
                if (!Saved && Previous) {
                    (void)desklink::SetWin32RunAtLogin(
                        Previous->RunAtLogin, ProductShellPath);
                }
                const bool Reconciled = !Saved ||
                    Supervisor.ConfigurationChanged();
                return desklink::ControlResponse{
                    Request.RequestId,
                    !Saved
                        ? desklink::ControlStatus::Failed
                        : Reconciled
                            ? desklink::ControlStatus::Ok
                            : desklink::ControlStatus::CleanupFailed};
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
                const auto Status = TrustAuthority.RequestPermissionChange(
                    Change->Machine, Change->DesiredCapabilities);
                if ((Status == desklink::TrustMutationStatus::Applied ||
                     Status == desklink::TrustMutationStatus::NoChange) &&
                    !Supervisor.ConfigurationChanged()) {
                    return desklink::ControlResponse{
                        Request.RequestId,
                        desklink::ControlStatus::CleanupFailed};
                }
                return desklink::ControlResponse{
                    Request.RequestId,
                    MapMutationStatus(Status)};
            }
            if (const auto* Forget = std::get_if<
                    desklink::ForgetTrustedDeviceControlRequest>(
                    &Request.Payload)) {
                const auto Status = TrustAuthority.ForgetPeer(
                    Forget->Machine);
                if ((Status == desklink::TrustMutationStatus::Applied ||
                     Status == desklink::TrustMutationStatus::NoChange) &&
                    !Supervisor.ConfigurationChanged()) {
                    return desklink::ControlResponse{
                        Request.RequestId,
                        desklink::ControlStatus::CleanupFailed};
                }
                return desklink::ControlResponse{
                    Request.RequestId,
                    MapMutationStatus(Status)};
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
                        Candidate->RequestedCapabilities,
                        Pairing.Source(Candidate->RequestId)};
                }
                return Response;
            }
            if (const auto* Start = std::get_if<
                    desklink::StartDiscoveryControlRequest>(
                    &Request.Payload)) {
                return desklink::ControlResponse{
                    Request.RequestId,
                    Discovery.Start(std::chrono::seconds(
                        Start->DurationSeconds))
                        ? desklink::ControlStatus::Ok
                        : desklink::ControlStatus::NotReady};
            }
            if (std::holds_alternative<
                    desklink::GetNearbyPeersControlRequest>(
                    Request.Payload)) {
                desklink::ControlResponse Response{
                    Request.RequestId, desklink::ControlStatus::Ok};
                Response.NearbyPeers = Discovery.Snapshot();
                return Response;
            }
            if (std::holds_alternative<
                    desklink::StopDiscoveryControlRequest>(
                    Request.Payload)) {
                Discovery.Stop();
                return desklink::ControlResponse{
                    Request.RequestId, desklink::ControlStatus::Ok};
            }
            if (const auto* Open = std::get_if<
                    desklink::OpenPairingWindowControlRequest>(
                    &Request.Payload)) {
                return desklink::ControlResponse{
                    Request.RequestId,
                    Pairing.OpenWindow(
                        Open->Port, Open->RequestedCapabilities)
                        ? desklink::ControlStatus::Ok
                        : desklink::ControlStatus::NotReady};
            }
            if (const auto* Nearby = std::get_if<
                    desklink::PairNearbyPeerControlRequest>(
                    &Request.Payload)) {
                const auto Peer = Discovery.PairablePeer(Nearby->Machine);
                return desklink::ControlResponse{
                    Request.RequestId,
                    Peer && Pairing.PairNearby(
                        *Peer, Nearby->RequestedCapabilities)
                        ? desklink::ControlStatus::Ok
                        : desklink::ControlStatus::NotReady};
            }
            if (const auto* Manual = std::get_if<
                    desklink::PairManualAddressControlRequest>(
                    &Request.Payload)) {
                return desklink::ControlResponse{
                    Request.RequestId,
                    Pairing.PairManual(
                        Manual->Host, Manual->Port,
                        Manual->RequestedCapabilities)
                        ? desklink::ControlStatus::Ok
                        : desklink::ControlStatus::NotReady};
            }
            if (const auto* Resolve = std::get_if<
                    desklink::ResolvePairingCandidateControlRequest>(
                    &Request.Payload)) {
                return desklink::ControlResponse{
                    Request.RequestId,
                    Pairing.Resolve(
                        Resolve->OperationId, Resolve->Approved)
                        ? desklink::ControlStatus::Ok
                        : desklink::ControlStatus::NotReady};
            }
            if (const auto* Present = std::get_if<
                    desklink::PresentManagedPairingCandidateControlRequest>(
                    &Request.Payload)) {
                return desklink::ControlResponse{
                    Request.RequestId,
                    Pairing.Present(*Present)
                        ? desklink::ControlStatus::Ok
                        : desklink::ControlStatus::InvalidRequest};
            }
            if (const auto* Decision = std::get_if<
                    desklink::GetManagedPairingDecisionControlRequest>(
                    &Request.Payload)) {
                const auto Value = Pairing.Decision(*Decision);
                desklink::ControlResponse Response{
                    Request.RequestId,
                    Value ? desklink::ControlStatus::Ok
                          : desklink::ControlStatus::InvalidRequest};
                Response.PairingDecision = Value;
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
            if (std::holds_alternative<desklink::PauseDeskLinkControlRequest>(
                    Request.Payload)) {
                return desklink::ControlResponse{
                    Request.RequestId,
                    Supervisor.Pause()
                        ? desklink::ControlStatus::Ok
                        : desklink::ControlStatus::CleanupFailed};
            }
            if (std::holds_alternative<desklink::ResumeDeskLinkControlRequest>(
                    Request.Payload)) {
                return desklink::ControlResponse{
                    Request.RequestId,
                    Supervisor.Resume()
                        ? desklink::ControlStatus::Ok
                        : desklink::ControlStatus::CleanupFailed};
            }
            if (std::holds_alternative<
                    desklink::PrepareForUpdateControlRequest>(
                    Request.Payload)) {
                Pairing.Stop();
                const bool Stopped = Supervisor.Stop();
                if (Stopped) SetEvent(StopEvent.Get());
                return desklink::ControlResponse{
                    Request.RequestId,
                    Stopped ? desklink::ControlStatus::Ok
                            : desklink::ControlStatus::CleanupFailed};
            }

            const auto Forwarded = ForwardToActiveRuntime(Request);
            if (Forwarded) {
                auto Response = *Forwarded;
                if (Response.State) Supervisor.ApplyState(*Response.State);
                return Response;
            }
            if (std::holds_alternative<desklink::GetStateControlRequest>(
                    Request.Payload)) {
                auto State = LocalState(LocalMachine);
                Supervisor.ApplyState(State);
                return desklink::ControlResponse{
                    Request.RequestId, desklink::ControlStatus::Ok,
                    State};
            }
            return desklink::ControlResponse{
                Request.RequestId, desklink::ControlStatus::NotReady};
        }, kBrokerPipeInstance);
    if (!Server.Start()) {
        if (NetworkNotificationArmed) {
            (void)CancelIPChangeNotify(&NetworkOverlapped);
        }
        std::cerr << "[Broker:Control] could not create the current-user endpoint\n";
        return 1;
    }
    if (!Supervisor.Start()) {
        Server.Stop();
        if (NetworkNotificationArmed) {
            (void)CancelIPChangeNotify(&NetworkOverlapped);
        }
        std::cerr << "[Broker:Lifecycle] runtime supervisor could not start\n";
        return 1;
    }

    std::cout << "[Broker:Lifecycle] per-user runtime broker ready; input is Local\n";
    for (;;) {
        const HANDLE Events[] = {
            StopEvent.Get(), NetworkEvent.Get(), PowerEvent.Get()};
        const auto Wait = WaitForMultipleObjects(
            static_cast<DWORD>(std::size(Events)), Events, FALSE, 250);
        if (Wait == WAIT_OBJECT_0) break;
        if (Wait == WAIT_OBJECT_0 + 1) {
            Supervisor.NetworkChanged();
            NetworkNotificationArmed = ArmNetworkChangeNotification(
                NetworkEvent.Get(), NetworkOverlapped,
                NetworkNotification);
            if (!NetworkNotificationArmed) {
                std::cerr
                    << "[Broker:Network] address-change notification recovery failed; error="
                    << GetLastError() << '\n';
            }
            continue;
        }
        if (Wait == WAIT_OBJECT_0 + 2) {
            const auto Pending = PowerContext.Pending.exchange(
                0, std::memory_order_acquire);
            bool Reconciled = true;
            if ((Pending & kPowerSuspendPending) != 0) {
                Reconciled = Supervisor.SystemSuspend();
                if (Reconciled) {
                    std::cout
                        << "[Broker:Power] system suspend reconciled Local\n";
                }
            }
            if (Reconciled && (Pending & kPowerResumePending) != 0) {
                Reconciled = Supervisor.SystemResume();
                if (Reconciled) {
                    std::cout
                        << "[Broker:Power] system resume scheduled a fresh Local session\n";
                }
            }
            if (!Reconciled) {
                std::cerr
                    << "[Broker:Power] power transition cleanup failed; automatic reconnect remains blocked\n";
            }
            continue;
        }
        if (Wait != WAIT_TIMEOUT) {
            std::cerr << "[Broker:Lifecycle] stop-event wait failed\n";
            Pairing.Stop();
            (void)Supervisor.Stop();
            Server.Stop();
            if (NetworkNotificationArmed) {
                (void)CancelIPChangeNotify(&NetworkOverlapped);
            }
            return 1;
        }
        if (Server.Running()) continue;
        Server.Stop();
        if (!Server.Start()) {
            std::cerr << "[Broker:Control] current-user endpoint recovery failed\n";
            Pairing.Stop();
            (void)Supervisor.Stop();
            if (NetworkNotificationArmed) {
                (void)CancelIPChangeNotify(&NetworkOverlapped);
            }
            return 1;
        }
        std::cerr << "[Broker:Control] current-user endpoint recovered after a transient failure\n";
    }
    if (NetworkNotificationArmed) {
        (void)CancelIPChangeNotify(&NetworkOverlapped);
    }
    Pairing.Stop();
    (void)Supervisor.Stop();
    Server.Stop();
    std::cout << "[Broker:Lifecycle] broker stopped after fail-local cleanup\n";
    return 0;
}
