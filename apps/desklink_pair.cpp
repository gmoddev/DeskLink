#include "desklink/msquic_bootstrap.hpp"
#include "desklink/session.hpp"
#include "desklink/win32_capture.hpp"
#include "desklink/win32_input.hpp"
#include "desklink/win32_pairing.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::uint16_t kDefaultPort = 43821;
constexpr std::chrono::seconds kPairingWindow{300};
constexpr wchar_t kDeviceKeyName[] = L"DeskLink-Device-Identity-v1";
#ifdef DESKLINK_ENABLE_VALIDATION_FAULTS
constexpr std::uint16_t kValidationAcceptedScanCode = 0x7cu;
constexpr std::uint16_t kValidationStaleEpochScanCode = 0x7bu;
constexpr std::uint16_t kValidationStaleSessionScanCode = 0x7au;
#endif

enum class Operation {
    Identity,
    PairListen,
    PairConnect,
    Serve,
    Focus,
};

struct CommandLine {
    Operation Mode{Operation::PairListen};
    std::string Host;
    std::uint16_t Port{kDefaultPort};
    bool GrantInput{};
    bool CaptureInput{};
    bool ConsoleConfirm{};
    desklink::TlsBackend TlsBackend{desklink::TlsBackend::Auto};
    bool ValidationDropNextKeyRelease{};
    bool ValidationDropNextButtonRelease{};
    bool ValidationReconciliationProbe{};
    bool ValidationObserveCleanup{};
    bool ValidationObserveRejections{};
    bool ValidationTerminateHeldInput{};
    std::uint64_t ValidationStaleSessionNonce{};
    std::uint32_t ValidationDurationMs{};
};

struct PairingResult {
    std::mutex Mutex;
    std::condition_variable Changed;
    bool PromptActive{};
    bool Completed{};
    bool Accepted{};
    std::string Failure;
};

std::optional<std::string> ToUtf8(std::wstring_view Value) {
    if (Value.empty()) return std::string{};
    if (Value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    const auto Length = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, Value.data(), static_cast<int>(Value.size()),
        nullptr, 0, nullptr, nullptr);
    if (Length <= 0) return std::nullopt;
    std::string Result(static_cast<std::size_t>(Length), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, Value.data(), static_cast<int>(Value.size()),
            Result.data(), Length, nullptr, nullptr) != Length) {
        return std::nullopt;
    }
    return Result;
}

std::optional<std::wstring> ToWide(std::string_view Value) {
    if (Value.empty()) return std::wstring{};
    if (Value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    const auto Length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, Value.data(), static_cast<int>(Value.size()),
        nullptr, 0);
    if (Length <= 0) return std::nullopt;
    std::wstring Result(static_cast<std::size_t>(Length), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, Value.data(), static_cast<int>(Value.size()),
            Result.data(), Length) != Length) {
        return std::nullopt;
    }
    return Result;
}

std::optional<std::uint16_t> ParsePort(std::wstring_view Value) {
    if (Value.empty() || Value.size() > 5) return std::nullopt;
    std::uint32_t Port = 0;
    for (const auto Character : Value) {
        if (Character < L'0' || Character > L'9') return std::nullopt;
        Port = Port * 10u + static_cast<std::uint32_t>(Character - L'0');
        if (Port > 65'535u) return std::nullopt;
    }
    if (Port == 0) return std::nullopt;
    return static_cast<std::uint16_t>(Port);
}

#ifdef DESKLINK_ENABLE_VALIDATION_FAULTS
std::optional<std::uint32_t> ParseValidationDuration(std::wstring_view Value) {
    if (Value.empty() || Value.size() > 5) return std::nullopt;
    std::uint32_t Duration = 0;
    for (const auto Character : Value) {
        if (Character < L'0' || Character > L'9') return std::nullopt;
        Duration = Duration * 10u + static_cast<std::uint32_t>(Character - L'0');
    }
    if (Duration < 1'000u || Duration > 60'000u) return std::nullopt;
    return Duration;
}

std::optional<std::uint64_t> ParseValidationNonce(std::wstring_view Value) {
    if (Value.empty() || Value.size() > 20) return std::nullopt;
    std::uint64_t Nonce = 0;
    for (const auto Character : Value) {
        if (Character < L'0' || Character > L'9') return std::nullopt;
        const auto Digit = static_cast<std::uint64_t>(Character - L'0');
        if (Nonce > (std::numeric_limits<std::uint64_t>::max() - Digit) / 10u) {
            return std::nullopt;
        }
        Nonce = Nonce * 10u + Digit;
    }
    return Nonce == 0 ? std::nullopt : std::optional<std::uint64_t>{Nonce};
}
#endif

void PrintUsage() {
    std::wcerr
        << L"Usage:\n"
        << L"  desklink_pair identity\n"
        << L"  desklink_pair listen [port] [--grant-input]\n"
        << L"  desklink_pair pair <host-or-ip> [port] [--grant-input]\n"
        << L"  desklink_pair serve [port]\n"
        << L"  desklink_pair focus <host-or-ip> [port] [--capture]\n\n"
        << L"--grant-input allows the newly paired remote PC to inject input on this PC.\n"
        << L"--capture forwards physical input and suppresses it locally until release.\n"
        << L"--console-confirm requires typing yes after comparing the pairing code.\n"
        << L"--tls-provider auto|schannel|openssl selects the packaged TLS runtime.\n";
#ifdef DESKLINK_ENABLE_VALIDATION_FAULTS
    std::wcerr
        << L"Validation-only options (not present in desklink_pair.exe):\n"
        << L"  serve --validation-drop-next-key-release\n"
        << L"        --validation-drop-next-button-release\n"
        << L"        --validation-observe-cleanup\n"
        << L"        --validation-observe-rejections\n"
        << L"  focus --validation-reconciliation-probe\n"
        << L"        --validation-terminate-held-input\n"
        << L"        --validation-rejection-probe <prior-session-nonce>\n"
        << L"  serve|focus --validation-duration-ms <1000..60000>\n";
#endif
}

std::optional<CommandLine> ParseCommandLine(int ArgumentCount, wchar_t** Arguments) {
    if (ArgumentCount < 2) return std::nullopt;
    CommandLine Result;
    int Index = 2;
    const std::wstring_view Command(Arguments[1]);
    if (Command == L"identity") {
        Result.Mode = Operation::Identity;
        if (ArgumentCount != 2) return std::nullopt;
    } else if (Command == L"listen") {
        Result.Mode = Operation::PairListen;
    } else if (Command == L"pair") {
        Result.Mode = Operation::PairConnect;
        if (ArgumentCount < 3) return std::nullopt;
        const auto Host = ToUtf8(Arguments[2]);
        if (!Host || Host->empty()) return std::nullopt;
        Result.Host = *Host;
        Index = 3;
    } else if (Command == L"serve") {
        Result.Mode = Operation::Serve;
    } else if (Command == L"focus") {
        Result.Mode = Operation::Focus;
        if (ArgumentCount < 3) return std::nullopt;
        const auto Host = ToUtf8(Arguments[2]);
        if (!Host || Host->empty()) return std::nullopt;
        Result.Host = *Host;
        Index = 3;
    } else {
        return std::nullopt;
    }

    bool PortSeen = false;
    bool ProviderSeen = false;
    for (; Index < ArgumentCount; ++Index) {
        const std::wstring_view Argument(Arguments[Index]);
        if (Argument == L"--grant-input") {
            if (Result.GrantInput) return std::nullopt;
            Result.GrantInput = true;
            continue;
        }
        if (Argument == L"--capture") {
            if (Result.CaptureInput) return std::nullopt;
            Result.CaptureInput = true;
            continue;
        }
        if (Argument == L"--console-confirm") {
            if (Result.ConsoleConfirm) return std::nullopt;
            Result.ConsoleConfirm = true;
            continue;
        }
        if (Argument == L"--tls-provider") {
            if (ProviderSeen || Index + 1 >= ArgumentCount) return std::nullopt;
            ProviderSeen = true;
            const std::wstring_view Provider(Arguments[++Index]);
            if (Provider == L"auto") {
                Result.TlsBackend = desklink::TlsBackend::Auto;
            } else if (Provider == L"schannel") {
                Result.TlsBackend = desklink::TlsBackend::Schannel;
            } else if (Provider == L"openssl") {
                Result.TlsBackend = desklink::TlsBackend::OpenSsl;
            } else {
                return std::nullopt;
            }
            continue;
        }
#ifdef DESKLINK_ENABLE_VALIDATION_FAULTS
        if (Argument == L"--validation-drop-next-key-release") {
            if (Result.ValidationDropNextKeyRelease) return std::nullopt;
            Result.ValidationDropNextKeyRelease = true;
            continue;
        }
        if (Argument == L"--validation-drop-next-button-release") {
            if (Result.ValidationDropNextButtonRelease) return std::nullopt;
            Result.ValidationDropNextButtonRelease = true;
            continue;
        }
        if (Argument == L"--validation-reconciliation-probe") {
            if (Result.ValidationReconciliationProbe) return std::nullopt;
            Result.ValidationReconciliationProbe = true;
            continue;
        }
        if (Argument == L"--validation-observe-cleanup") {
            if (Result.ValidationObserveCleanup) return std::nullopt;
            Result.ValidationObserveCleanup = true;
            continue;
        }
        if (Argument == L"--validation-observe-rejections") {
            if (Result.ValidationObserveRejections) return std::nullopt;
            Result.ValidationObserveRejections = true;
            continue;
        }
        if (Argument == L"--validation-terminate-held-input") {
            if (Result.ValidationTerminateHeldInput) return std::nullopt;
            Result.ValidationTerminateHeldInput = true;
            continue;
        }
        if (Argument == L"--validation-rejection-probe") {
            if (Result.ValidationStaleSessionNonce != 0 ||
                Index + 1 >= ArgumentCount) {
                return std::nullopt;
            }
            const auto Nonce = ParseValidationNonce(Arguments[++Index]);
            if (!Nonce) return std::nullopt;
            Result.ValidationStaleSessionNonce = *Nonce;
            continue;
        }
        if (Argument == L"--validation-duration-ms") {
            if (Result.ValidationDurationMs != 0 || Index + 1 >= ArgumentCount) {
                return std::nullopt;
            }
            const auto Duration = ParseValidationDuration(Arguments[++Index]);
            if (!Duration) return std::nullopt;
            Result.ValidationDurationMs = *Duration;
            continue;
        }
#endif
        if (PortSeen) return std::nullopt;
        const auto Port = ParsePort(Argument);
        if (!Port) return std::nullopt;
        Result.Port = *Port;
        PortSeen = true;
    }
    if (Result.GrantInput &&
        Result.Mode != Operation::PairListen &&
        Result.Mode != Operation::PairConnect) {
        return std::nullopt;
    }
    if (Result.CaptureInput && Result.Mode != Operation::Focus) return std::nullopt;
    if (Result.ConsoleConfirm &&
        Result.Mode != Operation::PairListen &&
        Result.Mode != Operation::PairConnect) {
        return std::nullopt;
    }
    if ((Result.ValidationDropNextKeyRelease ||
         Result.ValidationDropNextButtonRelease ||
         Result.ValidationObserveCleanup ||
         Result.ValidationObserveRejections) &&
        Result.Mode != Operation::Serve) {
        return std::nullopt;
    }
    if ((Result.ValidationReconciliationProbe ||
         Result.ValidationTerminateHeldInput ||
         Result.ValidationStaleSessionNonce != 0) &&
        Result.Mode != Operation::Focus) {
        return std::nullopt;
    }
    if (Result.ValidationReconciliationProbe &&
        Result.ValidationTerminateHeldInput) {
        return std::nullopt;
    }
    if (Result.ValidationStaleSessionNonce != 0 &&
        (Result.ValidationReconciliationProbe ||
         Result.ValidationTerminateHeldInput)) {
        return std::nullopt;
    }
    if (Result.ValidationStaleSessionNonce != 0 &&
        Result.ValidationDurationMs == 0) {
        return std::nullopt;
    }
    if (Result.ValidationDurationMs != 0 &&
        Result.Mode != Operation::Serve &&
        Result.Mode != Operation::Focus) {
        return std::nullopt;
    }
    return Result;
}

void PrintTransportDiagnostics(const desklink::MsQuicBootstrap& Bootstrap) {
    const auto Version = Bootstrap.WindowsVersion();
    std::cout << "[Transport:MsQuic] version=" << Bootstrap.RuntimeVersion()
              << " tls_provider=" << desklink::TlsBackendName(Bootstrap.Backend())
              << " os_build=" << Version.Build << '\n';
}

desklink::MsQuicRuntimeConfig GetRuntimeConfig(
    desklink::TlsBackend Backend) {
    desklink::MsQuicRuntimeConfig Result;
    Result.Backend = Backend;
    return Result;
}

std::optional<std::filesystem::path> GetDataDirectory() {
    PWSTR RawPath = nullptr;
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
    if (Error) return std::nullopt;
    return Result;
}

std::string FormatHex(desklink::ByteSpan Bytes) {
    constexpr char Digits[] = "0123456789abcdef";
    std::string Result(Bytes.size() * 2, '0');
    for (std::size_t Index = 0; Index < Bytes.size(); ++Index) {
        Result[Index * 2] = Digits[Bytes[Index] >> 4];
        Result[Index * 2 + 1] = Digits[Bytes[Index] & 0x0fu];
    }
    return Result;
}

int PrintIdentitySnapshot(const desklink::Win32DeviceCertificate& Certificate,
                          const desklink::IPairingCrypto& Crypto) {
    const auto Snapshot = Certificate.IdentitySnapshot(Crypto);
    if (!Snapshot) {
        std::cerr << "[Identity:Snapshot] could not read the CNG identity metadata\n";
        return 1;
    }
    const auto KeyName = ToUtf8(Snapshot->KeyName);
    const auto Provider = ToUtf8(Snapshot->Provider);
    const auto Algorithm = ToUtf8(Snapshot->Algorithm);
    if (!KeyName || !Provider || !Algorithm) {
        std::cerr << "[Identity:Snapshot] identity metadata is not valid UTF-8\n";
        return 1;
    }
    std::cout
        << "[Identity:Snapshot] key_name=" << *KeyName << '\n'
        << "[Identity:Snapshot] provider=" << *Provider << '\n'
        << "[Identity:Snapshot] algorithm=" << *Algorithm << '\n'
        << "[Identity:Snapshot] export_policy=" << Snapshot->ExportPolicy << '\n'
        << "[Identity:Snapshot] public_key_der="
        << FormatHex(Snapshot->PublicKeyDer) << '\n'
        << "[Identity:Snapshot] certificate_der_sha256="
        << desklink::FormatFingerprint(Snapshot->CertificateDerHash) << '\n'
        << "[Identity:Snapshot] desklink_identity_pin="
        << desklink::FormatFingerprint(Snapshot->DeskLinkIdentityPin) << '\n';
    return Snapshot->ExportPolicy == 0 ? 0 : 1;
}

std::optional<std::string> GetDisplayName() {
    wchar_t Buffer[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD Length = static_cast<DWORD>(std::size(Buffer));
    if (!GetComputerNameW(Buffer, &Length) || Length == 0) return std::nullopt;
    return ToUtf8(std::wstring_view(Buffer, Length));
}

desklink::MachineId GetMachineId(const desklink::Sha256Digest& CertificatePin) {
    desklink::MachineId Result{};
    std::copy_n(CertificatePin.begin(), Result.size(), Result.begin());
    return Result;
}

bool ConfirmPairing(const desklink::MsQuicPairingSession& Session,
                    bool GrantInput,
                    bool ConsoleConfirm) {
    const auto& Candidate = Session.Candidate();
    const auto RemoteName = ToWide(Candidate.Identity.display_name);
    if (!RemoteName || Candidate.Status != desklink::PairingStatus::Ready) return false;

    std::wstring Text =
        L"Compare this code with the code shown on the other PC:\n\n    ";
    const auto Code = ToWide(Candidate.VerificationCode);
    if (!Code) return false;
    Text += *Code;
    Text += L"\n\nRemote PC: ";
    Text += *RemoteName;
    Text += L"\n\n";
    Text += GrantInput
        ? L"This PC will allow the remote PC to inject keyboard and mouse input."
        : L"No input-injection capability will be granted on this PC.";
    Text += L"\n\nSelect Yes only if the code matches on both PCs.";

    if (ConsoleConfirm) {
        std::wcout << L"[Pairing:Confirmation] verification_code=" << *Code << L'\n'
                   << L"[Pairing:Confirmation] remote_pc=" << *RemoteName << L'\n'
                   << L"[Pairing:Confirmation] grant_input="
                   << (GrantInput ? L"yes" : L"no") << L'\n'
                   << L"[Pairing:Confirmation] type yes only after comparing both PCs: "
                   << std::flush;
        std::wstring Answer;
        return std::getline(std::wcin, Answer) && Answer == L"yes";
    }

    return MessageBoxW(
        nullptr, Text.c_str(), L"DeskLink pairing confirmation",
        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_SETFOREGROUND | MB_TOPMOST) == IDYES;
}

void HandlePairingOffer(const std::shared_ptr<PairingResult>& Result,
                        std::shared_ptr<desklink::MsQuicPairingSession> Session,
                        bool GrantInput,
                        bool ConsoleConfirm) {
    {
        std::scoped_lock Lock(Result->Mutex);
        if (Result->PromptActive || Result->Completed) {
            Session->Reject();
            return;
        }
        Result->PromptActive = true;
    }

    const bool UserConfirmed = ConfirmPairing(
        *Session, GrantInput, ConsoleConfirm);
    desklink::CapabilitySet Capabilities;
    if (GrantInput) Capabilities.grant(desklink::Capability::InputInject);
    const bool ConfirmationSent = UserConfirmed &&
        Session->Confirm(Session->Candidate().VerificationCode, Capabilities);
    if (!UserConfirmed) Session->Reject();

    {
        std::scoped_lock Lock(Result->Mutex);
        Result->PromptActive = false;
        if (!ConfirmationSent) {
            Result->Completed = true;
            Result->Accepted = false;
        }
        if (UserConfirmed && !ConfirmationSent) {
            Result->Failure = "pairing confirmation could not be sent";
        }
    }
    Result->Changed.notify_all();
}

struct TrustedResult {
    std::mutex Mutex;
    std::condition_variable Changed;
    bool Ready{};
    bool Emergency{};
    std::string Failure;
};

#ifdef DESKLINK_ENABLE_VALIDATION_FAULTS
class ValidationInputInjector final : public desklink::IInputInjector {
public:
    ValidationInputInjector(bool DropNextKeyRelease,
                            bool DropNextButtonRelease,
                            bool ObserveCleanup,
                            bool ObserveRejections) noexcept
        : DropNextKeyRelease_(DropNextKeyRelease),
          DropNextButtonRelease_(DropNextButtonRelease),
          ObserveCleanup_(ObserveCleanup),
          ObserveRejections_(ObserveRejections) {}

    bool inject_key(const desklink::KeyEventMessage& Event) override {
        if (Event.down) {
            const bool Injected = Injector_.inject_key(Event);
            if (Injected && DropNextKeyRelease_ && !PendingKeyRelease_) {
                ObservedKeyDown_ = Event;
            }
            const bool Tracked = Injected && desklink::SetInputSnapshotKey(
                ObservedState_, Event.scan_code, Event.extended, true);
            if (Tracked) RecordKeyDelivery(Event);
            return Tracked;
        }
        if (DropNextKeyRelease_ && ObservedKeyDown_ &&
            ObservedKeyDown_->scan_code == Event.scan_code &&
            ObservedKeyDown_->extended == Event.extended) {
            PendingKeyRelease_ = Event;
            ObservedKeyDown_.reset();
            DropNextKeyRelease_ = false;
            std::cout
                << "[Input:Reconciliation] validation fault injected=key_release"
                << std::endl;
            return true;
        }
        const bool Tracked = Injector_.inject_key(Event) &&
            desklink::SetInputSnapshotKey(
                ObservedState_, Event.scan_code, Event.extended, false);
        if (Tracked) RecordKeyDelivery(Event);
        return Tracked;
    }

    bool inject_button(const desklink::MouseButtonMessage& Event) override {
        if (Event.down) {
            const bool Injected = Injector_.inject_button(Event);
            if (Injected && DropNextButtonRelease_ && !PendingButtonRelease_) {
                ObservedButtonDown_ = Event;
            }
            return Injected && desklink::SetInputSnapshotButton(
                ObservedState_, Event.button, true);
        }
        if (DropNextButtonRelease_ && ObservedButtonDown_ &&
            ObservedButtonDown_->button == Event.button) {
            PendingButtonRelease_ = Event;
            ObservedButtonDown_.reset();
            DropNextButtonRelease_ = false;
            std::cout
                << "[Input:Reconciliation] validation fault injected=button_release"
                << std::endl;
            return true;
        }
        return Injector_.inject_button(Event) && desklink::SetInputSnapshotButton(
            ObservedState_, Event.button, false);
    }

    bool inject_pointer(const desklink::PointerPositionMessage& Event) override {
        return Injector_.inject_pointer(Event);
    }

    bool InjectWheel(const desklink::MouseWheelMessage& Message) override {
        return Injector_.InjectWheel(Message);
    }

    bool ReconcileState(
        const desklink::InputStateSnapshotMessage& Snapshot) override {
        if (!Injector_.ReconcileState(Snapshot)) return false;
        if (PendingKeyRelease_ &&
            !desklink::InputSnapshotKeyDown(
                Snapshot, PendingKeyRelease_->scan_code,
                PendingKeyRelease_->extended)) {
            PendingKeyRelease_.reset();
            std::cout
                << "[Input:Reconciliation] validation fault recovered=key_release"
                << std::endl;
        }
        if (PendingButtonRelease_ &&
            !desklink::InputSnapshotButtonDown(
                Snapshot, PendingButtonRelease_->button)) {
            PendingButtonRelease_.reset();
            std::cout
                << "[Input:Reconciliation] validation fault recovered=button_release"
                << std::endl;
        }
        ObservedState_ = Snapshot;
        return true;
    }

    void release_owned_state() noexcept override {
        if (ObserveCleanup_) {
            const bool KeyHeld = AnyKeyDown();
            const bool ButtonHeld = AnyButtonDown();
            const bool Released = Injector_.ReconcileState({});
            if (!Released) Injector_.release_owned_state();
            std::cout
                << "[Input:Cleanup] validation lease cleanup key_held="
                << (KeyHeld ? "true" : "false")
                << " button_held=" << (ButtonHeld ? "true" : "false")
                << " release_succeeded=" << (Released ? "true" : "false")
                << std::endl;
        } else {
            Injector_.release_owned_state();
        }
        if (ObserveRejections_ && !RejectionReportEmitted_) {
            RejectionReportEmitted_ = true;
            std::cout
                << "[Session:Validation] accepted_probe_deliveries="
                << AcceptedProbeDeliveries_
                << " stale_epoch_deliveries=" << StaleEpochDeliveries_
                << " stale_session_deliveries=" << StaleSessionDeliveries_
                << std::endl;
        }
        ObservedState_ = {};
        ObservedKeyDown_.reset();
        PendingKeyRelease_.reset();
        ObservedButtonDown_.reset();
        PendingButtonRelease_.reset();
    }

private:
    void RecordKeyDelivery(
        const desklink::KeyEventMessage& Event) noexcept {
        if (!ObserveRejections_) return;
        if (Event.scan_code == kValidationAcceptedScanCode) {
            ++AcceptedProbeDeliveries_;
        } else if (Event.scan_code == kValidationStaleEpochScanCode) {
            ++StaleEpochDeliveries_;
        } else if (Event.scan_code == kValidationStaleSessionScanCode) {
            ++StaleSessionDeliveries_;
        }
    }

    bool AnyKeyDown() const noexcept {
        for (std::uint16_t ScanCode = 1; ScanCode <= 255; ++ScanCode) {
            if (desklink::InputSnapshotKeyDown(
                    ObservedState_, ScanCode, false) ||
                desklink::InputSnapshotKeyDown(
                    ObservedState_, ScanCode, true)) {
                return true;
            }
        }
        return false;
    }

    bool AnyButtonDown() const noexcept {
        for (std::uint8_t Raw =
                 static_cast<std::uint8_t>(desklink::MouseButtonId::Left);
             Raw <= static_cast<std::uint8_t>(desklink::MouseButtonId::X2);
             ++Raw) {
            if (desklink::InputSnapshotButtonDown(
                    ObservedState_,
                    static_cast<desklink::MouseButtonId>(Raw))) {
                return true;
            }
        }
        return false;
    }

    desklink::Win32InputInjector Injector_;
    bool DropNextKeyRelease_{};
    bool DropNextButtonRelease_{};
    bool ObserveCleanup_{};
    bool ObserveRejections_{};
    bool RejectionReportEmitted_{};
    std::uint64_t AcceptedProbeDeliveries_{};
    std::uint64_t StaleEpochDeliveries_{};
    std::uint64_t StaleSessionDeliveries_{};
    desklink::InputStateSnapshotMessage ObservedState_{};
    std::optional<desklink::KeyEventMessage> ObservedKeyDown_;
    std::optional<desklink::KeyEventMessage> PendingKeyRelease_;
    std::optional<desklink::MouseButtonMessage> ObservedButtonDown_;
    std::optional<desklink::MouseButtonMessage> PendingButtonRelease_;
};
using AgentInputInjector = ValidationInputInjector;
#else
using AgentInputInjector = desklink::Win32InputInjector;
#endif

struct AgentRuntime {
    AgentRuntime(const desklink::IClock& Clock,
                 const desklink::ITrustStore& TrustStore,
                 desklink::MsQuicBootstrapHandlers::TrustedSession Trusted,
                 bool DropNextKeyRelease,
                 bool DropNextButtonRelease,
                 bool ObserveCleanup,
                 bool ObserveRejections)
#ifdef DESKLINK_ENABLE_VALIDATION_FAULTS
        : Injector(DropNextKeyRelease, DropNextButtonRelease, ObserveCleanup,
                   ObserveRejections),
          Coordinator(Clock, Injector),
#else
        : Coordinator(Clock, Injector),
#endif
          Session(std::move(Trusted.Endpoint), Coordinator, TrustStore,
                  Trusted.SessionNonce) {
#ifndef DESKLINK_ENABLE_VALIDATION_FAULTS
        (void)DropNextKeyRelease;
        (void)DropNextButtonRelease;
        (void)ObserveCleanup;
        (void)ObserveRejections;
#endif
    }

    AgentInputInjector Injector;
    desklink::AgentCoordinator Coordinator;
    desklink::AgentSession Session;
};

struct HostRuntime {
    HostRuntime(const desklink::ITrustStore& TrustStore,
                desklink::MsQuicBootstrapHandlers::TrustedSession Trusted,
                std::function<void()> FocusReadyHandler)
        : Endpoint(Trusted.Endpoint),
          SessionNonce(Trusted.SessionNonce),
          Coordinator(Trusted.SessionNonce),
          Session(std::move(Trusted.Endpoint), Coordinator, TrustStore,
                  Trusted.SessionNonce, std::move(FocusReadyHandler)) {}

    std::shared_ptr<desklink::MsQuicTransportEndpoint> Endpoint;
    std::uint64_t SessionNonce{};
    desklink::HostCoordinator Coordinator;
    desklink::HostSession Session;
};

int RunTrusted(const CommandLine& Command,
               desklink::Win32DeviceCertificate Certificate,
               desklink::DpapiTrustStore& TrustStore,
               desklink::BCryptPairingCrypto& Crypto,
               desklink::PairingCoordinator& Pairing,
               desklink::SteadyClock& Clock) {
    const auto Result = std::make_shared<TrustedResult>();
    std::mutex RuntimesMutex;
    std::vector<std::shared_ptr<AgentRuntime>> AgentRuntimes;
    std::shared_ptr<HostRuntime> Host;
    int ExitCode = 0;

    desklink::MsQuicBootstrapHandlers Handlers;
    Handlers.PairingOffered = [](std::shared_ptr<desklink::MsQuicPairingSession> Session) {
        Session->Reject();
    };
    Handlers.Failed = [Result, ReportImmediately = Command.Mode == Operation::Serve](
                          std::string Message) {
        std::string Report;
        {
            std::scoped_lock Lock(Result->Mutex);
            if (Result->Failure.empty()) {
                Result->Failure = std::move(Message);
                if (ReportImmediately) Report = Result->Failure;
            }
        }
        if (!Report.empty()) {
            std::cerr << "[Session:Control] " << Report << '\n';
        }
        Result->Changed.notify_all();
    };
    if (Command.Mode == Operation::Serve) {
        Handlers.Connected = [&, Result](
            desklink::MsQuicBootstrapHandlers::TrustedSession Trusted) {
            if (Trusted.Initiator) {
                Trusted.Endpoint->close();
                return;
            }
            std::cout << "[Session:Security] nonce=" << Trusted.SessionNonce
                      << " role=acceptor\n";
            auto Runtime = std::make_shared<AgentRuntime>(
                Clock, TrustStore, std::move(Trusted),
                Command.ValidationDropNextKeyRelease,
                Command.ValidationDropNextButtonRelease,
                Command.ValidationObserveCleanup,
                Command.ValidationObserveRejections);
            if (!Runtime->Session.start()) {
                Runtime->Session.stop();
                return;
            }
            {
                std::scoped_lock Lock(RuntimesMutex);
                AgentRuntimes.push_back(std::move(Runtime));
            }
            std::cout << "[Session:Control] trusted agent session connected\n";
            Result->Changed.notify_all();
        };
    } else {
        Handlers.Connected = [&, Result](
            desklink::MsQuicBootstrapHandlers::TrustedSession Trusted) {
            if (!Trusted.Initiator) {
                Trusted.Endpoint->close();
                return;
            }
            std::cout << "[Session:Security] nonce=" << Trusted.SessionNonce
                      << " role=initiator\n";
            auto Runtime = std::make_shared<HostRuntime>(
                TrustStore, std::move(Trusted), [Result] {
                    {
                        std::scoped_lock Lock(Result->Mutex);
                        Result->Ready = true;
                    }
                    Result->Changed.notify_all();
                });
            if (!Runtime->Session.start()) {
                Runtime->Session.stop();
                {
                    std::scoped_lock Lock(Result->Mutex);
                    Result->Failure = "trusted host session could not start";
                }
                Result->Changed.notify_all();
                return;
            }
            {
                std::scoped_lock Lock(RuntimesMutex);
                Host = Runtime;
            }
            if (!Runtime->Session.focus_remote(750)) {
                Runtime->Session.stop();
                {
                    std::scoped_lock Lock(Result->Mutex);
                    Result->Failure = "remote focus request could not be sent";
                }
                Result->Changed.notify_all();
            }
        };
    }

    auto Bootstrap = desklink::MsQuicBootstrap::Create(
        std::move(Certificate), TrustStore, Crypto, Pairing, Clock,
        GetRuntimeConfig(Command.TlsBackend), std::move(Handlers));
    if (!Bootstrap) {
        std::cerr << "[Session:Control] could not initialize MsQuic\n";
        return 1;
    }
    PrintTransportDiagnostics(*Bootstrap);

    if (Command.Mode == Operation::Serve) {
        if (!Bootstrap->StartListener(Command.Port)) {
            std::cerr << "[Session:Control] could not listen on UDP port "
                      << Command.Port << '\n';
            return 1;
        }
        std::cout << "[Session:Control] serving trusted sessions on UDP port "
                  << Bootstrap->BoundPort() << "\n"
                  << "[Session:Control] press Enter to stop\n";
        std::atomic_bool StopTicker{};
        std::thread Ticker([&] {
            while (!StopTicker.load()) {
                {
                    std::scoped_lock Lock(RuntimesMutex);
                    for (const auto& Runtime : AgentRuntimes) Runtime->Session.tick();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });
        if (Command.ValidationDurationMs == 0) {
            std::string Line;
            (void)std::getline(std::cin, Line);
        } else {
            std::cout
                << "[Session:Control] validation duration active; listener will stop automatically\n";
            std::this_thread::sleep_for(
                std::chrono::milliseconds(Command.ValidationDurationMs));
        }
        StopTicker.store(true);
        Ticker.join();
        {
            std::scoped_lock Lock(RuntimesMutex);
            for (const auto& Runtime : AgentRuntimes) Runtime->Session.stop();
            AgentRuntimes.clear();
        }
    } else {
        if (!Bootstrap->ConnectTrusted(Command.Host, Command.Port)) {
            std::cerr << "[Session:Control] could not connect to trusted peer "
                      << Command.Host << ':' << Command.Port << '\n';
            return 1;
        }
        {
            std::unique_lock Lock(Result->Mutex);
            if (!Result->Changed.wait_for(Lock, std::chrono::seconds(10), [&] {
                    return Result->Ready || !Result->Failure.empty();
                }) || !Result->Ready) {
                std::cerr << "[Session:Control] "
                          << (Result->Failure.empty()
                                  ? "remote focus was not granted"
                                  : Result->Failure)
                          << '\n';
                Bootstrap->Close();
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                return 1;
            }
        }
        std::shared_ptr<HostRuntime> ActiveHost;
        {
            std::scoped_lock Lock(RuntimesMutex);
            ActiveHost = Host;
        }
        if (!ActiveHost || !ActiveHost->Session.RemoteFocused()) {
            std::cerr << "[Session:Control] remote focus state was not established\n";
            Bootstrap->Close();
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            return 1;
        }
        if (!ActiveHost->Session.SendInputStateSnapshot()) {
            std::cerr << "[Input:Reconciliation] initial state snapshot failed\n";
            Bootstrap->Close();
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            return 1;
        }
        std::unique_ptr<desklink::Win32InputCapture> Capture;
        std::atomic<desklink::Win32InputCapture*> ActiveCapture{};
        std::atomic_uint64_t KeyEventsCaptured{};
        std::atomic_uint64_t KeyEventsForwarded{};
        std::atomic_uint64_t ButtonEventsCaptured{};
        std::atomic_uint64_t ButtonEventsForwarded{};
        std::atomic_uint64_t PointerEventsCaptured{};
        std::atomic_uint64_t PointerEventsForwarded{};
        std::atomic_uint64_t WheelEventsCaptured{};
        std::atomic_uint64_t WheelEventsForwarded{};
        std::atomic_bool EmergencyTriggered{};
        std::atomic_bool StopRenewal{};
        std::thread Renewal([&, Result] {
            while (!StopRenewal.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if (StopRenewal.load()) continue;
                const bool Renewed = ActiveHost->Session.renew_focus(750);
                const bool Reconciled = Renewed && ActiveHost->Session.SendInputStateSnapshot();
                if (!Reconciled) {
                    if (auto* Current = ActiveCapture.load()) {
                        Current->SetRemoteRouting(false);
                    }
                    {
                        std::scoped_lock Lock(Result->Mutex);
                        Result->Emergency = true;
                        Result->Failure = Renewed
                            ? "input state reconciliation failed"
                            : "focus lease renewal failed";
                    }
                    Result->Changed.notify_all();
                    break;
                }
            }
        });
#ifdef DESKLINK_ENABLE_VALIDATION_FAULTS
        if (Command.ValidationReconciliationProbe) {
            constexpr std::uint16_t ValidationScanCode = 0x7eu;
            const bool ProbeSent =
                ActiveHost->Session.send_key(
                    desklink::KeyEventMessage{ValidationScanCode, false, true}) &&
                ActiveHost->Session.send_button(
                    desklink::MouseButtonMessage{
                        desklink::MouseButtonId::X2, true}) &&
                ActiveHost->Session.send_key(
                    desklink::KeyEventMessage{ValidationScanCode, false, false}) &&
                ActiveHost->Session.send_button(
                    desklink::MouseButtonMessage{
                        desklink::MouseButtonId::X2, false});
            if (!ProbeSent) {
                {
                    std::scoped_lock Lock(Result->Mutex);
                    Result->Emergency = true;
                    Result->Failure = "validation reconciliation probe failed";
                }
                Result->Changed.notify_all();
            } else {
                std::cout
                    << "[Input:Reconciliation] validation probe transitions sent\n";
            }
        }
        if (Command.ValidationTerminateHeldInput) {
            constexpr std::uint16_t ValidationScanCode = 0x7du;
            const bool HeldInputSent =
                ActiveHost->Session.send_key(
                    desklink::KeyEventMessage{ValidationScanCode, false, true}) &&
                ActiveHost->Session.send_button(
                    desklink::MouseButtonMessage{
                        desklink::MouseButtonId::X1, true});
            if (!HeldInputSent) {
                {
                    std::scoped_lock Lock(Result->Mutex);
                    Result->Emergency = true;
                    Result->Failure = "held-input termination probe failed";
                }
                Result->Changed.notify_all();
            } else {
                std::cout
                    << "[Input:Cleanup] validation held input sent; terminating abruptly"
                    << std::endl;
                TerminateProcess(GetCurrentProcess(), 86u);
            }
        }
        if (Command.ValidationStaleSessionNonce != 0) {
            const auto CurrentEpoch =
                ActiveHost->Coordinator.remote_epoch();
            const auto StaleEpoch = CurrentEpoch > 1
                ? CurrentEpoch - 1
                : CurrentEpoch + 1;
            desklink::EnvelopeHeader StaleEpochHeader;
            StaleEpochHeader.session_nonce = ActiveHost->SessionNonce;
            StaleEpochHeader.epoch = StaleEpoch;
            StaleEpochHeader.sequence = 0xf001u;
            desklink::EnvelopeHeader StaleSessionHeader;
            StaleSessionHeader.session_nonce =
                Command.ValidationStaleSessionNonce;
            StaleSessionHeader.epoch = CurrentEpoch;
            StaleSessionHeader.sequence = 0xf002u;
            const bool ProbeSent =
                Command.ValidationStaleSessionNonce !=
                    ActiveHost->SessionNonce &&
                ActiveHost->Session.send_key(
                    desklink::KeyEventMessage{
                        kValidationAcceptedScanCode, false, true}) &&
                ActiveHost->Session.send_key(
                    desklink::KeyEventMessage{
                        kValidationAcceptedScanCode, false, false}) &&
                ActiveHost->Endpoint->send_reliable(
                    desklink::encode_packet(
                        StaleEpochHeader,
                        desklink::KeyEventMessage{
                            kValidationStaleEpochScanCode, false, true})) &&
                ActiveHost->Endpoint->send_reliable(
                    desklink::encode_packet(
                        StaleSessionHeader,
                        desklink::KeyEventMessage{
                            kValidationStaleSessionScanCode, false, true}));
            if (!ProbeSent) {
                {
                    std::scoped_lock Lock(Result->Mutex);
                    Result->Emergency = true;
                    Result->Failure = "stale epoch/session probe failed";
                }
                Result->Changed.notify_all();
            } else {
                std::cout
                    << "[Session:Validation] stale epoch/session probes sent"
                    << std::endl;
            }
        }
#endif
        if (Command.CaptureInput) {
            desklink::Win32CaptureHandlers CaptureHandlers;
            CaptureHandlers.Key = [ActiveHost, Result, &ActiveCapture,
                                   &KeyEventsCaptured, &KeyEventsForwarded](
                desklink::KeyEventMessage Event) {
                KeyEventsCaptured.fetch_add(1, std::memory_order_relaxed);
                if (ActiveHost->Session.send_key(Event)) {
                    KeyEventsForwarded.fetch_add(1, std::memory_order_relaxed);
                } else {
                    if (auto* Current = ActiveCapture.load()) {
                        Current->SetRemoteRouting(false);
                    }
                    {
                        std::scoped_lock Lock(Result->Mutex);
                        Result->Emergency = true;
                        Result->Failure = "reliable keyboard forwarding failed";
                    }
                    Result->Changed.notify_all();
                }
            };
            CaptureHandlers.Button = [ActiveHost, Result, &ActiveCapture,
                                      &ButtonEventsCaptured,
                                      &ButtonEventsForwarded](
                desklink::MouseButtonMessage Event) {
                ButtonEventsCaptured.fetch_add(1, std::memory_order_relaxed);
                if (ActiveHost->Session.send_button(Event)) {
                    ButtonEventsForwarded.fetch_add(1, std::memory_order_relaxed);
                } else {
                    if (auto* Current = ActiveCapture.load()) {
                        Current->SetRemoteRouting(false);
                    }
                    {
                        std::scoped_lock Lock(Result->Mutex);
                        Result->Emergency = true;
                        Result->Failure = "reliable mouse-button forwarding failed";
                    }
                    Result->Changed.notify_all();
                }
            };
            CaptureHandlers.Pointer = [ActiveHost, &PointerEventsCaptured,
                                       &PointerEventsForwarded](
                desklink::PointerPositionMessage Event) {
                PointerEventsCaptured.fetch_add(1, std::memory_order_relaxed);
                if (ActiveHost->Session.send_pointer(Event)) {
                    PointerEventsForwarded.fetch_add(1, std::memory_order_relaxed);
                }
            };
            CaptureHandlers.Wheel = [ActiveHost, Result, &ActiveCapture,
                                     &WheelEventsCaptured,
                                     &WheelEventsForwarded](
                desklink::MouseWheelMessage Message) {
                WheelEventsCaptured.fetch_add(1, std::memory_order_relaxed);
                if (ActiveHost->Session.SendWheel(Message)) {
                    WheelEventsForwarded.fetch_add(1, std::memory_order_relaxed);
                } else {
                    if (auto* Current = ActiveCapture.load()) {
                        Current->SetRemoteRouting(false);
                    }
                    {
                        std::scoped_lock Lock(Result->Mutex);
                        Result->Emergency = true;
                        Result->Failure = "reliable mouse-wheel forwarding failed";
                    }
                    Result->Changed.notify_all();
                }
            };
            CaptureHandlers.Emergency = [Result, &EmergencyTriggered] {
                EmergencyTriggered.store(true, std::memory_order_relaxed);
                {
                    std::scoped_lock Lock(Result->Mutex);
                    Result->Emergency = true;
                }
                Result->Changed.notify_all();
            };
            CaptureHandlers.Failed = [Result](std::string Message) {
                {
                    std::scoped_lock Lock(Result->Mutex);
                    Result->Emergency = true;
                    Result->Failure = std::move(Message);
                }
                Result->Changed.notify_all();
            };
            Capture = std::make_unique<desklink::Win32InputCapture>(
                std::move(CaptureHandlers));
            ActiveCapture.store(Capture.get());
            if (!Capture->Start()) {
                ActiveCapture.store(nullptr);
                StopRenewal.store(true);
                Renewal.join();
                std::cerr << "[Input:Capture] could not install Raw Input and fail-local hooks\n";
                (void)ActiveHost->Session.release_focus();
                ActiveHost->Session.stop();
                Bootstrap->Close();
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                return 1;
            }
            bool RenewalFailed = false;
            {
                std::scoped_lock Lock(Result->Mutex);
                RenewalFailed = Result->Emergency;
            }
            if (!RenewalFailed) {
                Capture->SetRemoteRouting(true);
                std::cout << "[Input:Capture] physical input is routed remotely\n"
                          << "[Input:Capture] Ctrl+Alt+Pause immediately fails local\n";
            }
        }
        std::cout << "[Session:Control] remote focus active; press Enter to release\n";
        const auto ValidationDeadline = Command.ValidationDurationMs == 0
            ? std::chrono::steady_clock::time_point::max()
            : std::chrono::steady_clock::now() +
                std::chrono::milliseconds(Command.ValidationDurationMs);
        const auto InputHandle = GetStdHandle(STD_INPUT_HANDLE);
        for (;;) {
            {
                std::scoped_lock Lock(Result->Mutex);
                if (Result->Emergency) break;
            }
            if (Capture && !Capture->RemoteRouting()) break;
            if (std::chrono::steady_clock::now() >= ValidationDeadline) {
                std::cout
                    << "[Input:Capture] validation duration elapsed; releasing locally\n";
                break;
            }
            if (Command.ValidationDurationMs != 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            const auto WaitResult = InputHandle == INVALID_HANDLE_VALUE ||
                    InputHandle == nullptr
                ? WAIT_FAILED
                : WaitForSingleObject(InputHandle, 100);
            if (WaitResult != WAIT_TIMEOUT) {
                std::string Line;
                if (WaitResult == WAIT_OBJECT_0) {
                    (void)std::getline(std::cin, Line);
                }
                break;
            }
        }
        if (Capture) Capture->SetRemoteRouting(false);
        StopRenewal.store(true);
        Renewal.join();
        if (Capture) {
            Capture->Stop();
            ActiveCapture.store(nullptr);
            std::cout << "[Input:Capture] summary"
                      << " key_captured="
                      << KeyEventsCaptured.load(std::memory_order_relaxed)
                      << " key_forwarded="
                      << KeyEventsForwarded.load(std::memory_order_relaxed)
                      << " button_captured="
                      << ButtonEventsCaptured.load(std::memory_order_relaxed)
                      << " button_forwarded="
                      << ButtonEventsForwarded.load(std::memory_order_relaxed)
                      << " pointer_captured="
                      << PointerEventsCaptured.load(std::memory_order_relaxed)
                      << " pointer_forwarded="
                      << PointerEventsForwarded.load(std::memory_order_relaxed)
                      << " wheel_captured="
                      << WheelEventsCaptured.load(std::memory_order_relaxed)
                      << " wheel_forwarded="
                      << WheelEventsForwarded.load(std::memory_order_relaxed)
                      << " emergency_triggered="
                      << (EmergencyTriggered.load(std::memory_order_relaxed)
                              ? "true" : "false")
                      << '\n';
        }
        (void)ActiveHost->Session.release_focus();
        ActiveHost->Session.stop();
        {
            std::scoped_lock Lock(Result->Mutex);
            if (!Result->Failure.empty()) {
                std::cerr << "[Input:Capture] " << Result->Failure << '\n';
                ExitCode = 1;
            }
        }
    }

    Bootstrap->Close();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    return ExitCode;
}

int Run(const CommandLine& Command) {
    desklink::BCryptPairingCrypto Crypto;
    auto Certificate = Command.Mode == Operation::Identity
        ? desklink::Win32DeviceCertificate::Load(kDeviceKeyName, Crypto)
        : desklink::Win32DeviceCertificate::LoadOrCreate(kDeviceKeyName, Crypto);
    if (!Certificate) {
        std::cerr << (Command.Mode == Operation::Identity
            ? "[Identity:Snapshot] existing device identity was not found\n"
            : "[Pairing:Control] could not load or create the device identity\n");
        return 1;
    }
    if (Command.Mode == Operation::Identity) {
        return PrintIdentitySnapshot(*Certificate, Crypto);
    }

    const auto DataDirectory = GetDataDirectory();
    const auto DisplayName = GetDisplayName();
    if (!DataDirectory || !DisplayName) {
        std::cerr << "[Pairing:Control] could not initialize current-user data paths\n";
        return 1;
    }

    desklink::DpapiTrustStore TrustStore(*DataDirectory / L"trust.db");
    if (!TrustStore.Load()) {
        std::cerr << "[Pairing:Control] could not load the DPAPI trust store\n";
        return 1;
    }

    desklink::SteadyClock Clock;
    desklink::PeerIdentity LocalIdentity{
        GetMachineId(Certificate->CertificatePin()),
        *DisplayName,
        desklink::FormatFingerprint(Certificate->CertificatePin())};
    desklink::PairingCoordinator Pairing(
        LocalIdentity, Certificate->CertificatePin(), Clock, Crypto, TrustStore);
    if (Command.Mode == Operation::Serve || Command.Mode == Operation::Focus) {
        return RunTrusted(Command, std::move(*Certificate), TrustStore, Crypto,
                          Pairing, Clock);
    }
    if (!Pairing.BeginPairing(kPairingWindow)) {
        std::cerr << "[Pairing:Control] could not open the pairing window\n";
        return 1;
    }

    const auto Result = std::make_shared<PairingResult>();
    desklink::MsQuicBootstrapHandlers Handlers;
    Handlers.PairingOffered = [Result,
                               GrantInput = Command.GrantInput,
                               ConsoleConfirm = Command.ConsoleConfirm](
        std::shared_ptr<desklink::MsQuicPairingSession> Session) {
        HandlePairingOffer(
            Result, std::move(Session), GrantInput, ConsoleConfirm);
    };
    Handlers.Connected = [](desklink::MsQuicBootstrapHandlers::TrustedSession Session) {
        Session.Endpoint->close();
    };
    Handlers.PairingCompleted = [Result] {
        {
            std::scoped_lock Lock(Result->Mutex);
            Result->Completed = true;
            Result->Accepted = true;
        }
        Result->Changed.notify_all();
    };
    Handlers.Failed = [Result](std::string Message) {
        {
            std::scoped_lock Lock(Result->Mutex);
            if (!Result->Completed && Result->Failure.empty()) {
                Result->Failure = std::move(Message);
            }
        }
        Result->Changed.notify_all();
    };

    auto Bootstrap = desklink::MsQuicBootstrap::Create(
        std::move(*Certificate), TrustStore, Crypto, Pairing, Clock,
        GetRuntimeConfig(Command.TlsBackend), std::move(Handlers));
    if (!Bootstrap) {
        std::scoped_lock Lock(Result->Mutex);
        std::cerr << "[Pairing:Control] "
                  << (Result->Failure.empty()
                          ? "could not initialize MsQuic"
                          : Result->Failure)
                  << '\n';
        return 1;
    }
    PrintTransportDiagnostics(*Bootstrap);

    if (Command.Mode == Operation::PairListen) {
        if (!Bootstrap->StartListener(Command.Port)) {
            std::cerr << "[Pairing:Control] could not listen on UDP port "
                      << Command.Port << '\n';
            return 1;
        }
        std::cout << "[Pairing:Control] pairing window open on UDP port "
                  << Bootstrap->BoundPort() << " for five minutes\n";
    } else {
        if (!Bootstrap->ConnectForPairing(Command.Host, Command.Port)) {
            std::cerr << "[Pairing:Control] could not start pairing with "
                      << Command.Host << ':' << Command.Port << '\n';
            return 1;
        }
        std::cout << "[Pairing:Control] pairing request sent to "
                  << Command.Host << ':' << Command.Port << '\n';
    }

    {
        std::unique_lock Lock(Result->Mutex);
        const auto Finished = Result->Changed.wait_for(
            Lock, kPairingWindow + std::chrono::seconds(5), [&] {
                return Result->Completed ||
                       (!Result->Failure.empty() && !Result->PromptActive);
            });
        if (!Finished && Result->PromptActive) {
            Result->Changed.wait(Lock, [&] { return Result->Completed; });
        }
    }

    Pairing.ClosePairing();
    Bootstrap->Close();
    std::scoped_lock Lock(Result->Mutex);
    if (Result->Accepted) {
        std::cout << "[Pairing:Control] pairing accepted and trust persisted\n";
        return 0;
    }
    if (!Result->Failure.empty()) {
        std::cerr << "[Pairing:Control] " << Result->Failure << '\n';
    } else {
        std::cerr << "[Pairing:Control] pairing was rejected or timed out\n";
    }
    return 1;
}

} // namespace

int wmain(int ArgumentCount, wchar_t** Arguments) {
    const auto Command = ParseCommandLine(ArgumentCount, Arguments);
    if (!Command) {
        PrintUsage();
        return 2;
    }
    return Run(*Command);
}
