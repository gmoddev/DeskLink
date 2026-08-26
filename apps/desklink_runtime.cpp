#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
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

namespace {

constexpr wchar_t kBrokerMutexName[] = L"Local\\DeskLink.RuntimeBroker.v1";
constexpr wchar_t kRuntimeMutexName[] = L"Local\\DeskLink.Runtime.v1";
constexpr wchar_t kDeviceKeyName[] = L"DeskLink-Device-Identity-v1";
constexpr wchar_t kBrokerPipeInstance[] = L"broker";
constexpr std::uint16_t kProductionPort = 43'821;

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
        const auto Arguments = desklink::BuildLauncherArguments(Request);
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
        RoamingArmed_ = false;
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
                StopChildAfterControlFailure(
                    desklink::BrokerRuntimeFailure::Capability);
                return;
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
        TrustStore, SafetyController);
    desklink::BrokerPairingCandidateLease PairingCandidates;
    desklink::SteadyClock Clock;
    BrokerRuntimeSupervisor Supervisor(
        PairPath, *DataDirectory, LocalMachine, TrustStore, PreferencesStore,
        SafetyController, Clock);
    UniqueHandle StopEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    UniqueHandle NetworkEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!StopEvent || !NetworkEvent) return 1;
    HANDLE NetworkNotification{};
    OVERLAPPED NetworkOverlapped{};
    bool NetworkNotificationArmed = ArmNetworkChangeNotification(
        NetworkEvent.Get(), NetworkOverlapped, NetworkNotification);
    if (!NetworkNotificationArmed) {
        std::cerr
            << "[Broker:Network] address-change notification unavailable; error="
            << GetLastError() << '\n';
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
        const HANDLE Events[] = {StopEvent.Get(), NetworkEvent.Get()};
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
        if (Wait != WAIT_TIMEOUT) {
            std::cerr << "[Broker:Lifecycle] stop-event wait failed\n";
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
    (void)Supervisor.Stop();
    Server.Stop();
    std::cout << "[Broker:Lifecycle] broker stopped after fail-local cleanup\n";
    return 0;
}
