#include "desklink/msquic_bootstrap.hpp"
#include "desklink/capabilities.hpp"
#include "desklink/discovery.hpp"
#include "desklink/host_input_lifecycle.hpp"
#include "desklink/profile.hpp"
#include "desklink/session.hpp"
#include "desklink/win32_audio.hpp"
#include "desklink/win32_capture.hpp"
#include "desklink/win32_control.hpp"
#include "desklink/win32_foreground.hpp"
#include "desklink/win32_input.hpp"
#include "desklink/win32_pairing.hpp"
#include "desklink/win32_discovery.hpp"

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
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
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
constexpr std::chrono::milliseconds kAudioRecoveryInitialDelay{250};
constexpr std::chrono::milliseconds kAudioRecoveryMaximumDelay{5'000};
constexpr wchar_t kDeviceKeyName[] = L"DeskLink-Device-Identity-v1";
#ifdef DESKLINK_ENABLE_VALIDATION_FAULTS
constexpr std::uint16_t kValidationAcceptedScanCode = 0x7cu;
constexpr std::uint16_t kValidationStaleEpochScanCode = 0x7bu;
constexpr std::uint16_t kValidationStaleSessionScanCode = 0x7au;
#endif

enum class Operation {
    Identity,
    Discover,
    PairListen,
    PairConnect,
    Serve,
    Focus,
    Control,
};

std::chrono::milliseconds NextAudioRecoveryDelay(
    std::chrono::milliseconds Current) noexcept {
    return std::min(Current * 2, kAudioRecoveryMaximumDelay);
}

struct CommandLine {
    Operation Mode{Operation::PairListen};
    std::string Host;
    std::uint16_t Port{kDefaultPort};
    std::uint32_t DiscoveryDurationMs{3'000};
    bool GrantInput{};
    bool GrantAudioSend{};
    bool GrantAudioReceive{};
    bool CaptureInput{};
    bool SendAudio{};
    bool ReceiveAudio{};
    desklink::Win32PointerCalibration PointerCalibration;
    bool ConsoleConfirm{};
    bool ProfileConfigurationSeen{};
    desklink::DeskMode ProfileDefaultMode{desklink::DeskMode::Roam};
    std::vector<desklink::ForegroundProfileRule> ProfileRules;
    desklink::TlsBackend TlsBackend{desklink::TlsBackend::Auto};
    desklink::ControlRequestPayload ControlPayload{
        desklink::GetStateControlRequest{}};
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

std::optional<std::uint32_t> ParseDiscoveryDuration(
    std::wstring_view Value) {
    if (Value.empty() || Value.size() > 2) return std::nullopt;
    std::uint32_t Seconds = 0;
    for (const auto Character : Value) {
        if (Character < L'0' || Character > L'9') return std::nullopt;
        Seconds = Seconds * 10u +
                  static_cast<std::uint32_t>(Character - L'0');
    }
    if (Seconds < 1 || Seconds > 30) return std::nullopt;
    return Seconds * 1'000u;
}

std::optional<std::uint16_t> ParseBoundedUnsigned(
    std::wstring_view Value,
    std::uint16_t Minimum,
    std::uint16_t Maximum) {
    if (Value.empty() || Value.size() > 5) return std::nullopt;
    std::uint32_t Parsed = 0;
    for (const auto Character : Value) {
        if (Character < L'0' || Character > L'9') return std::nullopt;
        Parsed = Parsed * 10u + static_cast<std::uint32_t>(Character - L'0');
        if (Parsed > Maximum) return std::nullopt;
    }
    if (Parsed < Minimum) return std::nullopt;
    return static_cast<std::uint16_t>(Parsed);
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
        << L"  desklink_pair discover [seconds: 1..30]\n"
        << L"  desklink_pair listen [port] [--grant-input] [--grant-audio-send|--grant-audio-receive]\n"
        << L"  desklink_pair pair <host-or-ip> [port] [--grant-input] [--grant-audio-send|--grant-audio-receive]\n"
        << L"  desklink_pair serve [port] [--send-audio]\n"
        << L"  desklink_pair focus <host-or-ip> [port] [--capture] [--pointer-gain 25..400] [--pointer-dpi 100..32000] [--receive-audio]\n"
        << L"  desklink_pair control state\n"
        << L"  desklink_pair control mode roam|lock|game\n\n"
        << L"--grant-input allows the newly paired remote PC to inject input on this PC.\n"
        << L"--grant-audio-send allows the remote PC to send audio into this PC.\n"
        << L"--grant-audio-receive allows the remote PC to receive audio captured on this PC.\n"
        << L"--send-audio explicitly starts loopback capture for authorized peers.\n"
        << L"--receive-audio explicitly starts shared-mode rendering for an authorized peer.\n"
        << L"--capture forwards physical input and suppresses it locally until release.\n"
        << L"--pointer-gain scales relative motion; 100 preserves raw counts.\n"
        << L"--pointer-dpi normalizes a known source DPI to an 800-DPI reference.\n"
        << L"focus --default-mode roam|lock-pc1|lock-pc2|game sets the fallback policy.\n"
        << L"focus --profile <exe>=<mode> adds an exact executable-name rule.\n"
        << L"focus --profile-fullscreen <exe>=<mode> requires a fullscreen match.\n"
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
    } else if (Command == L"discover") {
        Result.Mode = Operation::Discover;
        if (ArgumentCount == 3) {
            const auto Duration = ParseDiscoveryDuration(Arguments[2]);
            if (!Duration) return std::nullopt;
            Result.DiscoveryDurationMs = *Duration;
        } else if (ArgumentCount != 2) {
            return std::nullopt;
        }
        return Result;
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
    } else if (Command == L"control") {
        Result.Mode = Operation::Control;
        if (ArgumentCount == 3 &&
            std::wstring_view(Arguments[2]) == L"state") {
            Result.ControlPayload = desklink::GetStateControlRequest{};
            return Result;
        }
        if (ArgumentCount == 4 &&
            std::wstring_view(Arguments[2]) == L"mode") {
            const std::wstring_view Mode(Arguments[3]);
            if (Mode == L"roam") {
                Result.ControlPayload = desklink::SetDesiredModeControlRequest{
                    desklink::DeskMode::Roam};
            } else if (Mode == L"lock") {
                Result.ControlPayload = desklink::SetDesiredModeControlRequest{
                    desklink::DeskMode::LockPc1};
            } else if (Mode == L"game") {
                Result.ControlPayload = desklink::SetDesiredModeControlRequest{
                    desklink::DeskMode::Game};
            } else {
                return std::nullopt;
            }
            return Result;
        }
        return std::nullopt;
    } else {
        return std::nullopt;
    }

    bool PortSeen = false;
    bool ProviderSeen = false;
    bool DefaultModeSeen = false;
    bool PointerGainSeen = false;
    bool PointerDpiSeen = false;
    for (; Index < ArgumentCount; ++Index) {
        const std::wstring_view Argument(Arguments[Index]);
        if (Argument == L"--grant-input") {
            if (Result.GrantInput) return std::nullopt;
            Result.GrantInput = true;
            continue;
        }
        if (Argument == L"--grant-audio-send") {
            if (Result.GrantAudioSend) return std::nullopt;
            Result.GrantAudioSend = true;
            continue;
        }
        if (Argument == L"--grant-audio-receive") {
            if (Result.GrantAudioReceive) return std::nullopt;
            Result.GrantAudioReceive = true;
            continue;
        }
        if (Argument == L"--capture") {
            if (Result.CaptureInput) return std::nullopt;
            Result.CaptureInput = true;
            continue;
        }
        if (Argument == L"--pointer-gain") {
            if (PointerGainSeen || Index + 1 >= ArgumentCount) {
                return std::nullopt;
            }
            PointerGainSeen = true;
            const auto Gain = ParseBoundedUnsigned(
                Arguments[++Index], desklink::kMinimumPointerGainPercent,
                desklink::kMaximumPointerGainPercent);
            if (!Gain) return std::nullopt;
            Result.PointerCalibration.GainPercent = *Gain;
            continue;
        }
        if (Argument == L"--pointer-dpi") {
            if (PointerDpiSeen || Index + 1 >= ArgumentCount) {
                return std::nullopt;
            }
            PointerDpiSeen = true;
            const auto Dpi = ParseBoundedUnsigned(
                Arguments[++Index], desklink::kMinimumPointerDpi,
                desklink::kMaximumPointerDpi);
            if (!Dpi) return std::nullopt;
            Result.PointerCalibration.SourceDpi = *Dpi;
            continue;
        }
        if (Argument == L"--send-audio") {
            if (Result.SendAudio) return std::nullopt;
            Result.SendAudio = true;
            continue;
        }
        if (Argument == L"--receive-audio") {
            if (Result.ReceiveAudio) return std::nullopt;
            Result.ReceiveAudio = true;
            continue;
        }
        if (Argument == L"--console-confirm") {
            if (Result.ConsoleConfirm) return std::nullopt;
            Result.ConsoleConfirm = true;
            continue;
        }
        if (Argument == L"--default-mode") {
            if (DefaultModeSeen || Index + 1 >= ArgumentCount) {
                return std::nullopt;
            }
            DefaultModeSeen = true;
            Result.ProfileConfigurationSeen = true;
            const auto ModeName = ToUtf8(Arguments[++Index]);
            if (!ModeName) return std::nullopt;
            const auto Mode = desklink::ParseDeskModeName(*ModeName);
            if (!Mode) return std::nullopt;
            Result.ProfileDefaultMode = *Mode;
            continue;
        }
        if (Argument == L"--profile" ||
            Argument == L"--profile-fullscreen") {
            if (Index + 1 >= ArgumentCount ||
                Result.ProfileRules.size() >=
                    desklink::kMaximumForegroundProfileRules) {
                return std::nullopt;
            }
            Result.ProfileConfigurationSeen = true;
            const auto Specification = ToUtf8(Arguments[++Index]);
            if (!Specification) return std::nullopt;
            const auto Rule = desklink::ParseForegroundProfileRule(
                *Specification, Argument == L"--profile-fullscreen");
            if (!Rule) return std::nullopt;
            Result.ProfileRules.push_back(*Rule);
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
    if ((Result.GrantInput || Result.GrantAudioSend ||
         Result.GrantAudioReceive) &&
        Result.Mode != Operation::PairListen &&
        Result.Mode != Operation::PairConnect) {
        return std::nullopt;
    }
    if (Result.CaptureInput && Result.Mode != Operation::Focus) return std::nullopt;
    if ((PointerGainSeen || PointerDpiSeen) &&
        (Result.Mode != Operation::Focus || !Result.CaptureInput)) {
        return std::nullopt;
    }
    if (!desklink::IsValidWin32PointerCalibration(
            Result.PointerCalibration)) {
        return std::nullopt;
    }
    if (Result.SendAudio && Result.Mode != Operation::Serve) return std::nullopt;
    if (Result.ReceiveAudio && Result.Mode != Operation::Focus) return std::nullopt;
    if (Result.ProfileConfigurationSeen && Result.Mode != Operation::Focus) {
        return std::nullopt;
    }
    desklink::ForegroundProfileEngine ProfileValidator(
        Result.ProfileDefaultMode);
    if (!ProfileValidator.SetRules(Result.ProfileRules)) return std::nullopt;
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

int RunDiscovery(const CommandLine& Command) {
    std::cout << "[Discovery:Security] LAN advertisements are untrusted; "
                 "pair manually and verify the code\n"
              << "[Discovery:Browse] browsing for "
              << Command.DiscoveryDurationMs / 1'000u << " second(s)\n";
    const auto Result = desklink::Win32MdnsBrowser::Browse(
        std::chrono::milliseconds(Command.DiscoveryDurationMs));
    if (Result.StartStatus != ERROR_SUCCESS) {
        std::cerr << "[Discovery:Browse] could not start native DNS-SD browse; "
                  << "status=" << Result.StartStatus << '\n';
        return 1;
    }
    for (const auto& Peer : Result.Peers) {
        const auto& Endpoint = Peer.Endpoint;
        const auto& Advertisement = Endpoint.Advertisement;
        std::cout
            << "[Discovery:Peer] machine="
            << desklink::FormatDiscoveryMachineId(Advertisement.Machine)
            << " name=";
        for (const auto Character : Advertisement.DisplayName) {
            const auto Byte = static_cast<unsigned char>(Character);
            if (Byte >= 0x20u && Byte <= 0x7eu && Character != '\\') {
                std::cout << Character;
            } else {
                constexpr char Digits[] = "0123456789abcdef";
                std::cout << "\\x" << Digits[Byte >> 4u]
                          << Digits[Byte & 0x0fu];
            }
        }
        std::cout
            << " host=" << Endpoint.HostName
            << " port=" << Advertisement.Port
            << " protocol=" << Advertisement.ProtocolVersion
            << " caps=" << Advertisement.CapabilityHints
            << " pairing="
            << (Advertisement.PairingAvailable ? "open" : "closed")
            << " endpoints=" << Peer.EndpointCount
            << " ambiguous=" << (Peer.Ambiguous ? "true" : "false")
            << '\n';
    }
    if (Result.Peers.empty()) {
        std::cout << "[Discovery:Browse] no DeskLink peers observed\n";
    }
    if (Result.BrowseFailures || Result.ResolveFailures ||
        Result.MalformedRecords) {
        std::cerr << "[Discovery:Browse] ignored browse_failures="
                  << Result.BrowseFailures
                  << " resolve_failures=" << Result.ResolveFailures
                  << " malformed_records=" << Result.MalformedRecords << '\n';
    }
    return 0;
}

desklink::DiscoveryAdvertisement GetDiscoveryAdvertisement(
    const desklink::PeerIdentity& Identity,
    std::uint16_t Port,
    bool PairingAvailable) {
    desklink::DiscoveryAdvertisement Result;
    Result.Machine = Identity.machine_id;
    Result.DisplayName = Identity.display_name;
    Result.Port = Port;
    Result.CapabilityHints =
        static_cast<std::uint64_t>(desklink::Capability::InputInject) |
        static_cast<std::uint64_t>(desklink::Capability::AudioSend) |
        static_cast<std::uint64_t>(desklink::Capability::AudioReceive);
    Result.PairingAvailable = PairingAvailable;
    return Result;
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

std::string_view ControlRoleName(desklink::ControlRole Role) noexcept {
    switch (Role) {
        case desklink::ControlRole::Idle: return "idle";
        case desklink::ControlRole::Agent: return "agent";
        case desklink::ControlRole::Host: return "host";
    }
    return "invalid";
}

std::string_view DeskModeName(desklink::DeskMode Mode) noexcept {
    switch (Mode) {
        case desklink::DeskMode::Roam: return "roam";
        case desklink::DeskMode::LockPc1: return "lock-pc1";
        case desklink::DeskMode::LockPc2: return "lock-pc2";
        case desklink::DeskMode::Game: return "game";
    }
    return "invalid";
}

std::string_view ControlStatusName(desklink::ControlStatus Status) noexcept {
    switch (Status) {
        case desklink::ControlStatus::Ok: return "ok";
        case desklink::ControlStatus::InvalidRequest: return "invalid_request";
        case desklink::ControlStatus::Unsupported: return "unsupported";
        case desklink::ControlStatus::NotReady: return "not_ready";
        case desklink::ControlStatus::Failed: return "failed";
    }
    return "invalid";
}

int RunControl(const CommandLine& Command) {
    auto RequestId = static_cast<std::uint64_t>(GetTickCount64()) ^
        (static_cast<std::uint64_t>(GetCurrentProcessId()) << 32u);
    if (RequestId == 0) RequestId = 1;
    const auto Response = desklink::Win32ControlPipeClient::Send(
        desklink::ControlRequest{RequestId, Command.ControlPayload});
    if (!Response) {
        std::cerr << "[Control:Client] current-user DeskLink endpoint was unavailable\n";
        return 1;
    }
    if (Response->Status != desklink::ControlStatus::Ok) {
        std::cerr << "[Control:Client] request status="
                  << ControlStatusName(Response->Status) << '\n';
        return 1;
    }
    if (!Response->State) {
        std::cout << "[Control:Client] request applied\n";
        return 0;
    }

    const auto& State = *Response->State;
    std::cout << "[Control:State] role=" << ControlRoleName(State.Role)
              << " mode=" << DeskModeName(State.DesiredMode)
              << " peers=" << State.ConnectedPeerCount
              << " remote_focused=" << (State.RemoteFocused ? "true" : "false")
              << " capture_active=" << (State.CaptureActive ? "true" : "false")
              << " audio_gain=" << State.AudioGainPermyriad
              << " audio_muted=" << (State.AudioMuted ? "true" : "false")
              << " local_machine=" << FormatHex(State.LocalMachine);
    if (State.RemoteFocused) {
        std::cout << " focused_machine=" << FormatHex(State.FocusedMachine);
    }
    std::cout << '\n';
    return 0;
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
                    bool GrantAudioSend,
                    bool GrantAudioReceive,
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
    Text += L"\nAudio send into this PC: ";
    Text += GrantAudioSend ? L"allowed" : L"not allowed";
    Text += L"\nAudio receive from this PC: ";
    Text += GrantAudioReceive ? L"allowed" : L"not allowed";
    Text += L"\n\nSelect Yes only if the code matches on both PCs.";

    if (ConsoleConfirm) {
        std::wcout << L"[Pairing:Confirmation] verification_code=" << *Code << L'\n'
                   << L"[Pairing:Confirmation] remote_pc=" << *RemoteName << L'\n'
                   << L"[Pairing:Confirmation] grant_input="
                   << (GrantInput ? L"yes" : L"no") << L'\n'
                   << L"[Pairing:Confirmation] grant_audio_send="
                   << (GrantAudioSend ? L"yes" : L"no") << L'\n'
                   << L"[Pairing:Confirmation] grant_audio_receive="
                   << (GrantAudioReceive ? L"yes" : L"no") << L'\n'
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
                        bool GrantAudioSend,
                        bool GrantAudioReceive,
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
        *Session, GrantInput, GrantAudioSend, GrantAudioReceive,
        ConsoleConfirm);
    desklink::CapabilitySet Capabilities;
    if (GrantInput) Capabilities.grant(desklink::Capability::InputInject);
    if (GrantAudioSend) {
        Capabilities.grant(desklink::Capability::AudioSend);
    }
    if (GrantAudioReceive) {
        Capabilities.grant(desklink::Capability::AudioReceive);
    }
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
    bool Connected{};
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

    bool InjectPointerMotion(
        const desklink::PointerMotionMessage& Message) override {
        return Injector_.InjectPointerMotion(Message);
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
        : PeerMachine(Trusted.Endpoint->peer_info().identity.machine_id),
          Injector(DropNextKeyRelease, DropNextButtonRelease, ObserveCleanup,
                   ObserveRejections),
          Coordinator(Clock, Injector),
#else
        : PeerMachine(Trusted.Endpoint->peer_info().identity.machine_id),
          Coordinator(Clock, Injector),
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

    ~AgentRuntime() { StopAudio(); }

    void RequestAudioRecovery(
        desklink::Win32WasapiFailureKind Kind,
        std::string Message) noexcept {
        std::cerr << "[Audio:Capture] "
                  << (Message.empty() ? "capture stopped" : Message);
        if (!desklink::IsRecoverableWasapiFailure(Kind)) {
            std::cerr << "; audio stopped without restart; input session remains active\n";
            return;
        }
        std::cerr << "; scheduling audio-only recovery\n";
        AudioRecoveryGeneration.fetch_add(1);
        AudioRecoveryChanged.notify_one();
    }

    void RunAudioRecovery() noexcept {
        std::uint64_t SeenGeneration{};
        while (!AudioStop.load()) {
            {
                std::unique_lock Lock(AudioRecoveryMutex);
                AudioRecoveryChanged.wait(Lock, [&] {
                    return AudioStop.load() ||
                        AudioRecoveryGeneration.load() != SeenGeneration;
                });
            }
            if (AudioStop.load()) break;
            SeenGeneration = AudioRecoveryGeneration.load();
            auto Delay = kAudioRecoveryInitialDelay;
            while (!AudioStop.load()) {
                std::unique_lock Lock(AudioRecoveryMutex);
                if (AudioRecoveryChanged.wait_for(Lock, Delay, [&] {
                        return AudioStop.load();
                    })) {
                    break;
                }
                Lock.unlock();

                bool Restarted = false;
                const auto AttemptGeneration =
                    AudioRecoveryGeneration.load();
                try {
                    std::scoped_lock LifecycleLock(AudioLifecycleMutex);
                    if (AudioCapture) {
                        AudioCapture->Stop();
                        Restarted = !AudioStop.load() &&
                            AudioCapture->Start();
                    }
                } catch (...) {
                    Restarted = false;
                }
                if (Restarted) {
                    SeenGeneration = AttemptGeneration;
                    ++AudioRestartCount;
                    std::cout << "[Audio:Capture] endpoint recovered; "
                                 "session and input remained active\n";
                    break;
                }
                SeenGeneration = AudioRecoveryGeneration.load();
                Delay = NextAudioRecoveryDelay(Delay);
            }
        }
    }

    [[nodiscard]] bool StartAudio() {
        if (AudioCapture) return true;
        if (!Session.CanSendAudio()) {
            std::cerr << "[Audio:Security] peer lacks audio.receive grant; "
                         "capture remains stopped\n";
            return false;
        }
        desklink::Win32WasapiCaptureHandlers Handlers;
        Handlers.Frame = [this](desklink::AudioFrameMessage Frame) {
            return Session.SendAudioFrame(std::move(Frame));
        };
        Handlers.Failed = [this](
            desklink::Win32WasapiFailureKind Kind,
            std::string Message) {
            RequestAudioRecovery(Kind, std::move(Message));
        };
        try {
            AudioCapture =
                std::make_unique<desklink::Win32WasapiLoopbackCapture>(
                    1, std::move(Handlers));
            AudioStop.store(false);
            AudioRecoveryGeneration.store(0);
            AudioRestartCount.store(0);
            AudioRecovery = std::thread([this] { RunAudioRecovery(); });
        } catch (...) {
            AudioCapture.reset();
            return false;
        }
        bool Started = false;
        try {
            std::scoped_lock LifecycleLock(AudioLifecycleMutex);
            Started = AudioCapture->Start();
        } catch (...) {
            StopAudio();
            return false;
        }
        if (Started) {
            std::cout << "[Audio:Capture] capability-gated loopback active\n";
        } else {
            std::cout << "[Audio:Capture] waiting for a usable default endpoint; "
                         "input session remains active\n";
        }
        return true;
    }

    void StopAudio() noexcept {
        if (!AudioCapture) return;
        AudioStop.store(true);
        AudioRecoveryChanged.notify_all();
        {
            std::scoped_lock LifecycleLock(AudioLifecycleMutex);
            AudioCapture->Stop();
        }
        if (AudioRecovery.joinable() &&
            AudioRecovery.get_id() != std::this_thread::get_id()) {
            AudioRecovery.join();
        }
        AudioCapture.reset();
        const auto Stats = Session.stats();
        std::cout << "[Audio:Capture] stopped sent=" << Stats.AudioSent
                  << " rejected=" << Stats.AudioSendRejected
                  << " restarts=" << AudioRestartCount.load() << '\n';
    }

    desklink::MachineId PeerMachine{};
    AgentInputInjector Injector;
    desklink::AgentCoordinator Coordinator;
    desklink::AgentSession Session;
    std::unique_ptr<desklink::Win32WasapiLoopbackCapture> AudioCapture;
    std::mutex AudioLifecycleMutex;
    std::mutex AudioRecoveryMutex;
    std::condition_variable AudioRecoveryChanged;
    std::atomic_bool AudioStop{};
    std::atomic_uint64_t AudioRecoveryGeneration{};
    std::atomic_uint64_t AudioRestartCount{};
    std::thread AudioRecovery;
};

struct HostRuntime {
    HostRuntime(const desklink::ITrustStore& TrustStore,
                desklink::MsQuicBootstrapHandlers::TrustedSession Trusted,
                std::function<void()> FocusReadyHandler)
        : PeerMachine(Trusted.Endpoint->peer_info().identity.machine_id),
          Endpoint(Trusted.Endpoint),
          SessionNonce(Trusted.SessionNonce),
          Coordinator(Trusted.SessionNonce),
          Renderer(desklink::Win32WasapiRenderHandlers{
              [this](desklink::Win32WasapiFailureKind Kind,
                     std::string Message) {
                  RequestAudioRecovery(Kind, std::move(Message));
              }}),
          Receiver([this](desklink::AudioFrameMessage Frame) {
              return Renderer.Submit(std::move(Frame));
          }),
          Session(std::move(Trusted.Endpoint), Coordinator, TrustStore,
                  Trusted.SessionNonce, std::move(FocusReadyHandler),
                  &Receiver) {}

    ~HostRuntime() { StopAudio(); }

    void RequestAudioRecovery(
        desklink::Win32WasapiFailureKind Kind,
        std::string Message) noexcept {
        std::cerr << "[Audio:Render] "
                  << (Message.empty() ? "renderer stopped" : Message);
        if (!desklink::IsRecoverableWasapiFailure(Kind)) {
            std::cerr << "; audio stopped without restart; input session remains active\n";
            return;
        }
        std::cerr << "; scheduling audio-only recovery\n";
        AudioRecoveryGeneration.fetch_add(1);
    }

    void RunAudioPump() noexcept {
        std::uint64_t SeenGeneration{};
        bool RendererReady = Renderer.Running();
        auto Delay = kAudioRecoveryInitialDelay;
        auto RetryAt = std::chrono::steady_clock::now() + Delay;
        while (!AudioStop.load()) {
            const auto Now = std::chrono::steady_clock::now();
            const auto Generation = AudioRecoveryGeneration.load();
            if (RendererReady &&
                (Generation != SeenGeneration || !Renderer.Running())) {
                Renderer.Stop();
                Receiver.Reset();
                RendererReady = false;
                SeenGeneration = Generation;
                Delay = kAudioRecoveryInitialDelay;
                RetryAt = Now + Delay;
            }
            if (!RendererReady) {
                SeenGeneration = AudioRecoveryGeneration.load();
                if (Now >= RetryAt) {
                    bool Restarted = false;
                    const auto AttemptGeneration =
                        AudioRecoveryGeneration.load();
                    try {
                        Renderer.Stop();
                        Receiver.Reset();
                        Restarted = !AudioStop.load() && Renderer.Start();
                    } catch (...) {
                        Restarted = false;
                    }
                    if (Restarted) {
                        SeenGeneration = AttemptGeneration;
                        RendererReady = true;
                        Delay = kAudioRecoveryInitialDelay;
                        ++AudioRestartCount;
                        std::cout << "[Audio:Render] endpoint recovered; "
                                     "session and input remained active\n";
                    } else {
                        SeenGeneration = AudioRecoveryGeneration.load();
                        Delay = NextAudioRecoveryDelay(Delay);
                        RetryAt = std::chrono::steady_clock::now() + Delay;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            if (Receiver.Pump() ==
                desklink::AudioPumpResult::RenderRejected) {
                std::cerr << "[Audio:Render] receiver rejected playout; "
                             "scheduling audio-only recovery\n";
                AudioRecoveryGeneration.fetch_add(1);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    [[nodiscard]] bool StartAudio() {
        if (AudioPump.joinable()) return true;
        if (!Session.CanReceiveAudio()) {
            std::cerr << "[Audio:Security] peer lacks audio.send grant; "
                         "renderer remains stopped\n";
            return false;
        }
        Receiver.Reset();
        AudioStop.store(false);
        AudioRecoveryGeneration.store(0);
        AudioRestartCount.store(0);
        bool Started = false;
        try {
            Started = Renderer.Start();
            AudioPump = std::thread([this] { RunAudioPump(); });
        } catch (...) {
            Renderer.Stop();
            return false;
        }
        if (Started) {
            std::cout << "[Audio:Render] capability-gated receiver active\n";
        } else {
            std::cout << "[Audio:Render] waiting for a usable default endpoint; "
                         "input session remains active\n";
        }
        return true;
    }

    void StopAudio() noexcept {
        AudioStop.store(true);
        if (AudioPump.joinable() &&
            AudioPump.get_id() != std::this_thread::get_id()) {
            AudioPump.join();
        }
        Renderer.Stop();
        const auto Stats = Receiver.Stats();
        if (Stats.Accepted || Stats.Submitted || Stats.RenderRejected) {
            std::cout << "[Audio:Render] stopped accepted=" << Stats.Accepted
                      << " submitted=" << Stats.Submitted
                      << " concealed=" << Stats.Concealed
                      << " rejected="
                      << (Stats.FormatRejected + Stats.StreamRejected +
                          Stats.SequenceRejected + Stats.RenderRejected)
                      << " restarts=" << AudioRestartCount.load()
                      << " drift_ppm=" << Stats.AppliedClockDriftPpm
                      << " drift_adjustments="
                      << Stats.ClockDriftAdjustments
                      << " drift_resets="
                      << Stats.ClockDriftDiscontinuities
                      << '\n';
        }
        Receiver.Reset();
    }

    desklink::MachineId PeerMachine{};
    std::shared_ptr<desklink::MsQuicTransportEndpoint> Endpoint;
    std::uint64_t SessionNonce{};
    desklink::HostCoordinator Coordinator;
    desklink::Win32WasapiRenderer Renderer;
    desklink::AudioReceiver Receiver;
    desklink::HostSession Session;
    std::atomic_bool AudioStop{};
    std::atomic_uint64_t AudioRecoveryGeneration{};
    std::atomic_uint64_t AudioRestartCount{};
    std::thread AudioPump;
};

const char* ProfileSourceName(desklink::ProfileModeSource Source) noexcept {
    switch (Source) {
        case desklink::ProfileModeSource::SystemDefault: return "default";
        case desklink::ProfileModeSource::ProfileRule: return "profile";
        case desklink::ProfileModeSource::ManualOverride: return "manual";
        case desklink::ProfileModeSource::ForegroundUnavailable:
            return "foreground-unavailable";
        case desklink::ProfileModeSource::Emergency: return "emergency";
    }
    return "invalid";
}

class HostInputRuntime final
    : public desklink::IHostInputLifecycleBackend,
      public std::enable_shared_from_this<HostInputRuntime> {
public:
    HostInputRuntime(std::shared_ptr<HostRuntime> Host,
                     std::shared_ptr<TrustedResult> Result,
                     desklink::DeskMode DefaultMode,
                     std::vector<desklink::ForegroundProfileRule> Rules,
                     bool CaptureRequested,
                     desklink::Win32PointerCalibration PointerCalibration)
        : Host_(std::move(Host)),
          Result_(std::move(Result)),
          Profiles_(DefaultMode),
          Lifecycle_(*this, CaptureRequested),
          PointerCalibration_(PointerCalibration),
          ConfigurationValid_(Profiles_.SetRules(std::move(Rules))) {}

    ~HostInputRuntime() override { Stop(); }

    HostInputRuntime(const HostInputRuntime&) = delete;
    HostInputRuntime& operator=(const HostInputRuntime&) = delete;

    [[nodiscard]] bool Start() {
        if (Dispatcher_.joinable() || !ConfigurationValid_) return false;
        Stopping_.store(false);
        FailLocalRequested_.store(false);
        StopRequested_ = false;
        std::promise<bool> Promise;
        auto Future = Promise.get_future();
        Dispatcher_ = std::thread(
            [this, Promise = std::move(Promise)]() mutable {
                RunDispatcher(std::move(Promise));
            });
        const bool Started = Future.get();
        if (!Started && Dispatcher_.joinable()) Dispatcher_.join();
        return Started;
    }

    void Stop() noexcept {
        if (!Dispatcher_.joinable()) return;
        Stopping_.store(true);
        DisableCaptureImmediately();
        try {
            {
                std::scoped_lock Lock(QueueMutex_);
                StopRequested_ = true;
                Queue_.clear();
            }
            QueueChanged_.notify_all();
            if (Dispatcher_.get_id() != std::this_thread::get_id()) {
                Dispatcher_.join();
            }
        } catch (...) {
        }
    }

    void NotifyFocusReady() noexcept {
        if (!Post([this] {
            LastBackendFailure_ = BackendFailure::None;
            if (Lifecycle_.FocusReady()) {
                PublishLifecycleStatus();
                {
                    std::scoped_lock Lock(Result_->Mutex);
                    Result_->Ready = true;
                }
                Result_->Changed.notify_all();
                std::cout
                    << "[Input:Lifecycle] fresh focus admitted after initial snapshot\n";
                return;
            }
            const auto Status = Lifecycle_.Status();
            PublishLifecycleStatus();
            if (Status.State == desklink::HostInputLifecycleState::Local &&
                LastBackendFailure_ != BackendFailure::None) {
                ReportTerminalFailure(BackendFailureMessage());
            } else {
                std::cerr
                    << "[Input:Lifecycle] stale or unexpected FocusReady rejected\n";
            }
        }) && !Stopping_.load()) {
            RequestAsynchronousFailLocal("Host input event queue overflow");
        }
    }

    [[nodiscard]] bool ApplyManualMode(desklink::DeskMode Mode) {
        if (Stopping_.load()) return false;
        if (std::this_thread::get_id() == DispatcherId_) {
            return Profiles_.SetManualOverride(Mode) && ApplyDecision();
        }
        auto Promise = std::make_shared<std::promise<bool>>();
        auto Future = Promise->get_future();
        if (!Post([this, Mode, Promise] {
                bool Applied = false;
                try {
                    Applied = Profiles_.SetManualOverride(Mode);
                    if (Applied) Applied = ApplyDecision();
                } catch (...) {
                    ReportTerminalFailure("manual mode transition failed");
                }
                try {
                    Promise->set_value(Applied);
                } catch (...) {
                }
            })) {
            RequestAsynchronousFailLocal(
                "manual mode event queue admission failed");
            return false;
        }
        try {
            if (Future.wait_for(std::chrono::milliseconds(1'500)) !=
                std::future_status::ready) {
                RequestAsynchronousFailLocal(
                    "manual mode transition timed out");
                return false;
            }
            return Future.get();
        } catch (...) {
            RequestAsynchronousFailLocal(
                "manual mode transition result failed");
            return false;
        }
    }

    [[nodiscard]] desklink::DeskMode DesiredMode() const noexcept {
        return DesiredMode_.load();
    }

    [[nodiscard]] bool CaptureActive() const noexcept {
        return CaptureActive_.load();
    }

    [[nodiscard]] bool RemoteFocused() const noexcept {
        return LifecycleState_.load() ==
                   desklink::HostInputLifecycleState::Remote &&
               Host_->Session.RemoteFocused();
    }

    void DisableCapture() noexcept override {
        CaptureActive_.store(false);
        try {
            std::scoped_lock Lock(CaptureLifetimeMutex_);
            if (ActiveCapture_) ActiveCapture_->SetRemoteRouting(false);
        } catch (...) {
        }
    }

    void StopCapture() noexcept override {
        CaptureActive_.store(false);
        try {
            {
                std::scoped_lock Lock(CaptureLifetimeMutex_);
                if (ActiveCapture_) ActiveCapture_->SetRemoteRouting(false);
                ActiveCapture_ = nullptr;
            }
            if (Capture_) {
                Capture_->Stop();
                Capture_.reset();
            }
        } catch (...) {
        }
    }

    [[nodiscard]] bool ReleaseFocus() noexcept override {
        try {
            return Host_->Session.release_focus();
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool SetDesiredMode(desklink::DeskMode Mode) noexcept override {
        DesiredMode_.store(Mode);
        try {
            if (Host_->Session.SetDesiredMode(Mode)) return true;
        } catch (...) {
        }
        LastBackendFailure_ = BackendFailure::SetMode;
        return false;
    }

    [[nodiscard]] bool RequestFocus() noexcept override {
        try {
            if (Host_->Session.focus_remote(750)) return true;
        } catch (...) {
        }
        LastBackendFailure_ = BackendFailure::RequestFocus;
        return false;
    }

    [[nodiscard]] bool SendInputStateSnapshot() noexcept override {
        try {
            if (Host_->Session.SendInputStateSnapshot()) return true;
        } catch (...) {
        }
        LastBackendFailure_ = BackendFailure::Snapshot;
        return false;
    }

    [[nodiscard]] bool StartCapture() noexcept override {
        try {
            desklink::Win32CaptureHandlers Handlers;
            const auto Weak = weak_from_this();
            Handlers.Key = [Weak](desklink::KeyEventMessage Event) {
                if (const auto Runtime = Weak.lock()) Runtime->ForwardKey(Event);
            };
            Handlers.Button = [Weak](desklink::MouseButtonMessage Event) {
                if (const auto Runtime = Weak.lock()) Runtime->ForwardButton(Event);
            };
            Handlers.Pointer = [Weak](desklink::PointerPositionMessage Event) {
                if (const auto Runtime = Weak.lock()) Runtime->ForwardPointer(Event);
            };
            Handlers.PointerMotion = [Weak](
                desklink::PointerMotionMessage Message) {
                if (const auto Runtime = Weak.lock()) {
                    Runtime->ForwardPointerMotion(Message);
                }
            };
            Handlers.Wheel = [Weak](desklink::MouseWheelMessage Message) {
                if (const auto Runtime = Weak.lock()) Runtime->ForwardWheel(Message);
            };
            Handlers.Emergency = [Weak] {
                if (const auto Runtime = Weak.lock()) Runtime->EmergencyRelease();
            };
            Handlers.Failed = [Weak](std::string Message) {
                if (const auto Runtime = Weak.lock()) {
                    Runtime->CaptureFailed(std::move(Message));
                }
            };

            Capture_ = std::make_unique<desklink::Win32InputCapture>(
                std::move(Handlers), PointerCalibration_);
            if (!Capture_->Start()) {
                Capture_.reset();
                LastBackendFailure_ = BackendFailure::StartCapture;
                return false;
            }
            {
                std::scoped_lock Lock(CaptureLifetimeMutex_);
                ActiveCapture_ = Capture_.get();
            }
            return true;
        } catch (...) {
            Capture_.reset();
            LastBackendFailure_ = BackendFailure::StartCapture;
            return false;
        }
    }

    void EnableCapture() noexcept override {
        try {
            std::scoped_lock Lock(CaptureLifetimeMutex_);
            if (!ActiveCapture_ || FailLocalRequested_.load()) {
                CaptureActive_.store(false);
                if (ActiveCapture_) ActiveCapture_->SetRemoteRouting(false);
                return;
            }
            ActiveCapture_->SetRemoteRouting(true);
            CaptureActive_.store(true);
            std::cout
                << "[Input:Capture] physical input is routed remotely\n"
                << "[Input:Capture] Ctrl+Alt+Pause immediately fails local\n"
                << "[Input:Pointer] relative gain="
                << PointerCalibration_.GainPercent << "% source_dpi="
                << (PointerCalibration_.SourceDpi == 0
                        ? std::string("raw")
                        : std::to_string(PointerCalibration_.SourceDpi))
                << '\n';
        } catch (...) {
            CaptureActive_.store(false);
        }
    }

private:
    enum class BackendFailure {
        None,
        SetMode,
        RequestFocus,
        Snapshot,
        StartCapture,
    };

    [[nodiscard]] const char* BackendFailureMessage() const noexcept {
        switch (LastBackendFailure_) {
            case BackendFailure::SetMode:
                return "desired-mode update failed";
            case BackendFailure::RequestFocus:
                return "remote focus request failed";
            case BackendFailure::Snapshot:
                return "input state reconciliation failed";
            case BackendFailure::StartCapture:
                return "Raw Input capture installation failed";
            case BackendFailure::None:
                return "Host input lifecycle failed";
        }
        return "Host input lifecycle failed";
    }

    [[nodiscard]] bool Initialize() {
        if (!Profiles_.Rules().empty()) {
            Profiles_.SetForeground(desklink::ReadWin32ForegroundWindow());
        }
        LastBackendFailure_ = BackendFailure::None;
        const auto Decision = Profiles_.Decision();
        if (!Lifecycle_.Start(Decision.Mode)) {
            PublishLifecycleStatus();
            ReportTerminalFailure(BackendFailureMessage());
            return false;
        }
        LastDecision_ = Decision;
        PublishLifecycleStatus();
        LogDecision(Decision);

        if (Profiles_.Rules().empty()) return true;
        const auto Weak = weak_from_this();
        desklink::Win32ForegroundHandlers Handlers;
        Handlers.Changed = [Weak](desklink::ForegroundWindowSnapshot Snapshot) {
            if (const auto Runtime = Weak.lock()) {
                Runtime->ForegroundChanged(std::move(Snapshot));
            }
        };
        Handlers.Failed = [Weak](std::string Message) {
            if (const auto Runtime = Weak.lock()) {
                Runtime->ForegroundFailed(std::move(Message));
            }
        };
        ForegroundMonitor_ =
            std::make_unique<desklink::Win32ForegroundMonitor>(
                std::move(Handlers));
        if (!ForegroundMonitor_->Start()) {
            Profiles_.EmergencyFailLocal();
            Lifecycle_.FailLocal();
            PublishLifecycleStatus();
            ReportTerminalFailure("foreground monitor could not start");
            return false;
        }
        std::cout << "[Input:Profile] foreground monitoring active rules="
                  << Profiles_.Rules().size() << '\n';
        return true;
    }

    void RunDispatcher(std::promise<bool> Promise) noexcept {
        DispatcherId_ = std::this_thread::get_id();
        bool Initialized = false;
        try {
            Initialized = Initialize();
        } catch (...) {
            ReportTerminalFailure("Host input runtime initialization failed");
        }
        Promise.set_value(Initialized);
        if (!Initialized) {
            Lifecycle_.FailLocal();
            PublishLifecycleStatus();
            if (ForegroundMonitor_) ForegroundMonitor_->Stop();
            return;
        }

        auto NextRenewal = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(500);
        for (;;) {
            std::function<void()> Task;
            {
                std::unique_lock Lock(QueueMutex_);
                QueueChanged_.wait_until(Lock, NextRenewal, [&] {
                    return StopRequested_ || FailLocalRequested_.load() ||
                           !Queue_.empty();
                });
                if (StopRequested_) break;
                if (!FailLocalRequested_.load() && !Queue_.empty()) {
                    Task = std::move(Queue_.front());
                    Queue_.pop_front();
                }
            }
            if (FailLocalRequested_.exchange(false)) {
                ReportTerminalFailure("asynchronous fail-local requested");
                continue;
            }
            if (Task) {
                try {
                    Task();
                } catch (...) {
                    ReportTerminalFailure("serialized Host input event failed");
                }
            }
            if (FailLocalRequested_.exchange(false)) {
                ReportTerminalFailure("asynchronous fail-local requested");
                continue;
            }
            const auto Now = std::chrono::steady_clock::now();
            if (Now >= NextRenewal) {
                RenewAndReconcile();
                NextRenewal = Now + std::chrono::milliseconds(500);
            }
        }

        Lifecycle_.FailLocal();
        PublishLifecycleStatus();
        if (ForegroundMonitor_) ForegroundMonitor_->Stop();
        PrintCaptureSummary();
    }

    [[nodiscard]] bool Post(std::function<void()> Task) noexcept {
        if (Stopping_.load()) return false;
        try {
            {
                std::scoped_lock Lock(QueueMutex_);
                if (StopRequested_ || Stopping_.load() ||
                    Queue_.size() >= MaximumQueuedEvents) {
                    return false;
                }
                Queue_.push_back(std::move(Task));
            }
            QueueChanged_.notify_one();
            return true;
        } catch (...) {
            return false;
        }
    }

    void ForegroundChanged(desklink::ForegroundWindowSnapshot Snapshot) noexcept {
        if (!Post([this, Snapshot = std::move(Snapshot)]() mutable {
            Profiles_.SetForeground(std::move(Snapshot));
            (void)ApplyDecision();
        }) && !Stopping_.load()) {
            RequestAsynchronousFailLocal("foreground event queue overflow");
        }
    }

    void ForegroundFailed(std::string Message) noexcept {
        if (!Post([this, Message = std::move(Message)] {
            ReportTerminalFailure(Message.empty()
                ? "foreground monitoring failed"
                : Message);
        }) && !Stopping_.load()) {
            RequestAsynchronousFailLocal("foreground failure queue overflow");
        }
    }

    [[nodiscard]] bool ApplyDecision() {
        const auto Decision = Profiles_.Decision();
        LastBackendFailure_ = BackendFailure::None;
        const bool Applied = Lifecycle_.ApplyMode(Decision.Mode);
        PublishLifecycleStatus();
        if (!Applied) {
            ReportTerminalFailure(BackendFailureMessage());
            return false;
        }
        if (!LastDecision_ || *LastDecision_ != Decision) {
            LogDecision(Decision);
            LastDecision_ = Decision;
        }
        if (Lifecycle_.Status().State !=
            desklink::HostInputLifecycleState::Remote) {
            std::scoped_lock Lock(Result_->Mutex);
            Result_->Ready = false;
        }
        Result_->Changed.notify_all();
        return true;
    }

    void RenewAndReconcile() noexcept {
        if (Lifecycle_.Status().State !=
            desklink::HostInputLifecycleState::Remote) {
            return;
        }
        bool Renewed = false;
        bool Reconciled = false;
        try {
            Renewed = Host_->Session.renew_focus(750);
            Reconciled = Renewed && Host_->Session.SendInputStateSnapshot();
        } catch (...) {
        }
        if (!Reconciled) {
            ReportTerminalFailure(Renewed
                ? "input state reconciliation failed"
                : "focus lease renewal failed");
        }
    }

    void PublishLifecycleStatus() noexcept {
        const auto Status = Lifecycle_.Status();
        DesiredMode_.store(Status.Mode);
        LifecycleState_.store(Status.State);
        if (Status.State != desklink::HostInputLifecycleState::Remote) {
            CaptureActive_.store(false);
        }
    }

    void LogDecision(const desklink::ProfileModeDecision& Decision) const {
        std::cout << "[Input:Profile] effective_mode="
                  << DeskModeName(Decision.Mode)
                  << " source=" << ProfileSourceName(Decision.Source);
        if (Decision.RuleIndex) {
            std::cout << " rule=" << *Decision.RuleIndex;
        }
        std::cout << '\n';
    }

    void ReportTerminalFailure(std::string_view Message) noexcept {
        Profiles_.EmergencyFailLocal();
        Lifecycle_.FailLocal();
        PublishLifecycleStatus();
        try {
            std::scoped_lock Lock(Result_->Mutex);
            Result_->Ready = false;
            Result_->Emergency = true;
            if (Result_->Failure.empty()) Result_->Failure = Message;
        } catch (...) {
        }
        Result_->Changed.notify_all();
    }

    void SignalEmergencyOnly(std::string_view Message = {}) noexcept {
        try {
            std::scoped_lock Lock(Result_->Mutex);
            Result_->Ready = false;
            Result_->Emergency = true;
            if (!Message.empty() && Result_->Failure.empty()) {
                Result_->Failure = Message;
            }
        } catch (...) {
        }
        Result_->Changed.notify_all();
    }

    void DisableCaptureImmediately() noexcept {
        CaptureActive_.store(false);
        try {
            std::scoped_lock Lock(CaptureLifetimeMutex_);
            if (ActiveCapture_) ActiveCapture_->SetRemoteRouting(false);
        } catch (...) {
        }
    }

    void RequestAsynchronousFailLocal(std::string_view Message) noexcept {
        try {
            std::scoped_lock Lock(CaptureLifetimeMutex_);
            FailLocalRequested_.store(true);
            CaptureActive_.store(false);
            if (ActiveCapture_) ActiveCapture_->SetRemoteRouting(false);
        } catch (...) {
            FailLocalRequested_.store(true);
            CaptureActive_.store(false);
        }
        SignalEmergencyOnly(Message);
        QueueChanged_.notify_all();
    }

    void CaptureTransportFailed(std::string Message) noexcept {
        DisableCaptureImmediately();
        if (!Post([this, Message = std::move(Message)] {
                ReportTerminalFailure(Message);
            })) {
            RequestAsynchronousFailLocal(
                "capture failure event queue admission failed");
        }
    }

    void ForwardKey(desklink::KeyEventMessage Event) noexcept {
        KeyEventsCaptured_.fetch_add(1, std::memory_order_relaxed);
        bool Sent = false;
        try {
            Sent = Host_->Session.send_key(Event);
        } catch (...) {
        }
        if (Sent) {
            KeyEventsForwarded_.fetch_add(1, std::memory_order_relaxed);
        } else {
            CaptureTransportFailed("reliable keyboard forwarding failed");
        }
    }

    void ForwardButton(desklink::MouseButtonMessage Event) noexcept {
        ButtonEventsCaptured_.fetch_add(1, std::memory_order_relaxed);
        bool Sent = false;
        try {
            Sent = Host_->Session.send_button(Event);
        } catch (...) {
        }
        if (Sent) {
            ButtonEventsForwarded_.fetch_add(1, std::memory_order_relaxed);
        } else {
            CaptureTransportFailed("reliable mouse-button forwarding failed");
        }
    }

    void ForwardPointer(desklink::PointerPositionMessage Event) noexcept {
        PointerEventsCaptured_.fetch_add(1, std::memory_order_relaxed);
        bool Sent = false;
        try {
            Sent = Host_->Session.send_pointer(Event);
        } catch (...) {
        }
        if (Sent) {
            PointerEventsForwarded_.fetch_add(1, std::memory_order_relaxed);
        } else {
            CaptureTransportFailed("pointer forwarding failed");
        }
    }

    void ForwardPointerMotion(desklink::PointerMotionMessage Message) noexcept {
        PointerEventsCaptured_.fetch_add(1, std::memory_order_relaxed);
        bool Sent = false;
        try {
            Sent = Host_->Session.SendPointerMotion(Message);
        } catch (...) {
        }
        if (Sent) {
            PointerEventsForwarded_.fetch_add(1, std::memory_order_relaxed);
        } else {
            CaptureTransportFailed("relative pointer forwarding failed");
        }
    }

    void ForwardWheel(desklink::MouseWheelMessage Message) noexcept {
        WheelEventsCaptured_.fetch_add(1, std::memory_order_relaxed);
        bool Sent = false;
        try {
            Sent = Host_->Session.SendWheel(Message);
        } catch (...) {
        }
        if (Sent) {
            WheelEventsForwarded_.fetch_add(1, std::memory_order_relaxed);
        } else {
            CaptureTransportFailed("reliable mouse-wheel forwarding failed");
        }
    }

    void EmergencyRelease() noexcept {
        DisableCaptureImmediately();
        EmergencyTriggered_.store(true, std::memory_order_relaxed);
        if (!Post([this] {
            Profiles_.EmergencyFailLocal();
            Lifecycle_.FailLocal();
            PublishLifecycleStatus();
            {
                std::scoped_lock Lock(Result_->Mutex);
                Result_->Ready = false;
                Result_->Emergency = true;
            }
            Result_->Changed.notify_all();
        })) {
            RequestAsynchronousFailLocal(
                "emergency event queue admission failed");
        }
    }

    void CaptureFailed(std::string Message) noexcept {
        CaptureTransportFailed(Message.empty()
            ? "physical input capture failed"
            : std::move(Message));
    }

    void PrintCaptureSummary() const {
        std::cout << "[Input:Capture] summary"
                  << " key_captured="
                  << KeyEventsCaptured_.load(std::memory_order_relaxed)
                  << " key_forwarded="
                  << KeyEventsForwarded_.load(std::memory_order_relaxed)
                  << " button_captured="
                  << ButtonEventsCaptured_.load(std::memory_order_relaxed)
                  << " button_forwarded="
                  << ButtonEventsForwarded_.load(std::memory_order_relaxed)
                  << " pointer_captured="
                  << PointerEventsCaptured_.load(std::memory_order_relaxed)
                  << " pointer_forwarded="
                  << PointerEventsForwarded_.load(std::memory_order_relaxed)
                  << " wheel_captured="
                  << WheelEventsCaptured_.load(std::memory_order_relaxed)
                  << " wheel_forwarded="
                  << WheelEventsForwarded_.load(std::memory_order_relaxed)
                  << " emergency_triggered="
                  << (EmergencyTriggered_.load(std::memory_order_relaxed)
                          ? "true" : "false")
                  << '\n';
    }

    std::shared_ptr<HostRuntime> Host_;
    std::shared_ptr<TrustedResult> Result_;
    desklink::ForegroundProfileEngine Profiles_;
    desklink::HostInputLifecycle Lifecycle_;
    desklink::Win32PointerCalibration PointerCalibration_;
    bool ConfigurationValid_{};
    std::optional<desklink::ProfileModeDecision> LastDecision_;
    BackendFailure LastBackendFailure_{BackendFailure::None};

    std::unique_ptr<desklink::Win32ForegroundMonitor> ForegroundMonitor_;
    std::unique_ptr<desklink::Win32InputCapture> Capture_;
    std::mutex CaptureLifetimeMutex_;
    desklink::Win32InputCapture* ActiveCapture_{};

    std::thread Dispatcher_;
    std::thread::id DispatcherId_{};
    std::mutex QueueMutex_;
    std::condition_variable QueueChanged_;
    std::deque<std::function<void()>> Queue_;
    static constexpr std::size_t MaximumQueuedEvents = 64;
    bool StopRequested_{};
    std::atomic_bool Stopping_{};
    std::atomic_bool FailLocalRequested_{};

    std::atomic<desklink::DeskMode> DesiredMode_{desklink::DeskMode::LockPc1};
    std::atomic<desklink::HostInputLifecycleState> LifecycleState_{
        desklink::HostInputLifecycleState::Local};
    std::atomic_bool CaptureActive_{};
    std::atomic_bool EmergencyTriggered_{};
    std::atomic_uint64_t KeyEventsCaptured_{};
    std::atomic_uint64_t KeyEventsForwarded_{};
    std::atomic_uint64_t ButtonEventsCaptured_{};
    std::atomic_uint64_t ButtonEventsForwarded_{};
    std::atomic_uint64_t PointerEventsCaptured_{};
    std::atomic_uint64_t PointerEventsForwarded_{};
    std::atomic_uint64_t WheelEventsCaptured_{};
    std::atomic_uint64_t WheelEventsForwarded_{};
};

int RunTrusted(const CommandLine& Command,
               desklink::Win32DeviceCertificate Certificate,
               desklink::DpapiTrustStore& TrustStore,
               desklink::BCryptPairingCrypto& Crypto,
               desklink::PairingCoordinator& Pairing,
               desklink::SteadyClock& Clock,
               const desklink::PeerIdentity& LocalIdentity) {
    const auto Result = std::make_shared<TrustedResult>();
    std::mutex RuntimesMutex;
    std::vector<std::shared_ptr<AgentRuntime>> AgentRuntimes;
    std::shared_ptr<HostRuntime> Host;
    std::shared_ptr<HostInputRuntime> HostInput;
    int ExitCode = 0;
    std::atomic<desklink::DeskMode> DesiredMode{desklink::DeskMode::Roam};

    desklink::Win32ControlPipeServer ControlServer(
        [&](const desklink::ControlRequest& Request) {
            if (std::holds_alternative<desklink::GetStateControlRequest>(
                    Request.Payload)) {
                desklink::ControlState State;
                State.LocalMachine = LocalIdentity.machine_id;
                State.Role = Command.Mode == Operation::Serve
                    ? desklink::ControlRole::Agent
                    : desklink::ControlRole::Host;
                State.DesiredMode = DesiredMode.load();

                std::vector<std::shared_ptr<AgentRuntime>> Agents;
                std::shared_ptr<HostRuntime> ActiveHost;
                std::shared_ptr<HostInputRuntime> ActiveInput;
                {
                    std::scoped_lock Lock(RuntimesMutex);
                    Agents = AgentRuntimes;
                    ActiveHost = Host;
                    ActiveInput = HostInput;
                }
                if (Command.Mode == Operation::Serve) {
                    State.ConnectedPeerCount = static_cast<std::uint16_t>(
                        std::min<std::size_t>(Agents.size(),
                            std::numeric_limits<std::uint16_t>::max()));
                    for (const auto& Runtime : Agents) {
                        if (Runtime->Session.RemoteFocused()) {
                            State.RemoteFocused = true;
                            State.FocusedMachine = Runtime->PeerMachine;
                            break;
                        }
                    }
                } else if (ActiveHost && ActiveInput) {
                    State.ConnectedPeerCount = 1;
                    State.DesiredMode = ActiveInput->DesiredMode();
                    State.CaptureActive = ActiveInput->CaptureActive();
                    State.RemoteFocused = ActiveInput->RemoteFocused();
                    if (State.RemoteFocused) {
                        State.FocusedMachine = ActiveHost->PeerMachine;
                    }
                }
                if (!desklink::IsValidControlState(State)) {
                    return desklink::ControlResponse{
                        Request.RequestId, desklink::ControlStatus::Failed,
                        std::nullopt};
                }
                return desklink::ControlResponse{
                    Request.RequestId, desklink::ControlStatus::Ok, State};
            }

            const auto* SetMode = std::get_if<
                desklink::SetDesiredModeControlRequest>(&Request.Payload);
            if (!SetMode) {
                return desklink::ControlResponse{
                    Request.RequestId, desklink::ControlStatus::Unsupported,
                    std::nullopt};
            }

            if (Command.Mode == Operation::Serve) {
                DesiredMode.store(SetMode->Mode);
                std::vector<std::shared_ptr<AgentRuntime>> Agents;
                {
                    std::scoped_lock Lock(RuntimesMutex);
                    Agents = AgentRuntimes;
                }
                for (const auto& Runtime : Agents) {
                    Runtime->Session.SetLocalDesiredMode(SetMode->Mode);
                }
                return desklink::ControlResponse{
                    Request.RequestId, desklink::ControlStatus::Ok,
                    std::nullopt};
            }

            std::shared_ptr<HostRuntime> ActiveHost;
            std::shared_ptr<HostInputRuntime> ActiveInput;
            {
                std::scoped_lock Lock(RuntimesMutex);
                ActiveHost = Host;
                ActiveInput = HostInput;
            }
            if (!ActiveHost || !ActiveInput) {
                return desklink::ControlResponse{
                    Request.RequestId, desklink::ControlStatus::NotReady,
                    std::nullopt};
            }
            const bool Applied = ActiveInput->ApplyManualMode(SetMode->Mode);
            return desklink::ControlResponse{
                Request.RequestId,
                Applied ? desklink::ControlStatus::Ok
                        : desklink::ControlStatus::Failed,
                std::nullopt};
        });
    if (!ControlServer.Start()) {
        std::cerr << "[Control:Pipe] could not create the current-user endpoint\n";
        return 1;
    }
    std::cout << "[Control:Pipe] current-user endpoint active\n";

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
    const auto FocusTarget =
        std::make_shared<std::weak_ptr<HostInputRuntime>>();
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
            if (Command.SendAudio && !Runtime->StartAudio()) {
                std::cerr << "[Audio:Capture] requested audio was not started; "
                             "input session remains active\n";
            }
            Runtime->Session.SetLocalDesiredMode(DesiredMode.load());
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
                TrustStore, std::move(Trusted), [FocusTarget] {
                    if (const auto Input = FocusTarget->lock()) {
                        Input->NotifyFocusReady();
                    }
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
            if (Command.ReceiveAudio && !Runtime->StartAudio()) {
                std::cerr << "[Audio:Render] requested audio was not started; "
                             "input session remains active\n";
            }
            auto Input = std::make_shared<HostInputRuntime>(
                Runtime, Result, Command.ProfileDefaultMode,
                Command.ProfileRules, Command.CaptureInput,
                Command.PointerCalibration);
            *FocusTarget = Input;
            {
                std::scoped_lock Lock(RuntimesMutex);
                Host = Runtime;
                HostInput = Input;
            }
            if (!Input->Start()) {
                Input->Stop();
                Runtime->StopAudio();
                Runtime->Session.stop();
                {
                    std::scoped_lock Lock(Result->Mutex);
                    if (Result->Failure.empty()) {
                        Result->Failure = "Host input runtime could not start";
                    }
                }
                Result->Changed.notify_all();
                return;
            }
            {
                std::scoped_lock Lock(Result->Mutex);
                Result->Connected = true;
            }
            Result->Changed.notify_all();
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
        desklink::Win32MdnsAdvertiser Advertiser;
        const auto Advertisement = GetDiscoveryAdvertisement(
            LocalIdentity, Bootstrap->BoundPort(), false);
        if (Advertiser.Start(Advertisement)) {
            std::cout << "[Discovery:Advertise] trusted-session endpoint "
                         "advertised on the local link\n";
        } else {
            std::cerr << "[Discovery:Advertise] unavailable; status="
                      << Advertiser.LastStatus()
                      << "; manual address connection remains active\n";
        }
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
            for (const auto& Runtime : AgentRuntimes) {
                Runtime->StopAudio();
                Runtime->Session.stop();
            }
            AgentRuntimes.clear();
        }
        Advertiser.Stop();
    } else {
        if (!Bootstrap->ConnectTrusted(Command.Host, Command.Port)) {
            std::cerr << "[Session:Control] could not connect to trusted peer "
                      << Command.Host << ':' << Command.Port << '\n';
            return 1;
        }
        {
            std::unique_lock Lock(Result->Mutex);
            if (!Result->Changed.wait_for(Lock, std::chrono::seconds(10), [&] {
                    return Result->Connected || !Result->Failure.empty();
                }) || !Result->Connected) {
                std::cerr << "[Session:Control] "
                          << (Result->Failure.empty()
                                  ? "trusted Host runtime did not become ready"
                                  : Result->Failure)
                          << '\n';
                Bootstrap->Close();
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                return 1;
            }
        }
        std::shared_ptr<HostRuntime> ActiveHost;
        std::shared_ptr<HostInputRuntime> ActiveInput;
        {
            std::scoped_lock Lock(RuntimesMutex);
            ActiveHost = Host;
            ActiveInput = HostInput;
        }
        if (!ActiveHost || !ActiveInput) {
            std::cerr << "[Session:Control] trusted Host runtime is unavailable\n";
            Bootstrap->Close();
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            return 1;
        }
        bool RequireRemote = Command.ProfileRules.empty() &&
            Command.ProfileDefaultMode != desklink::DeskMode::LockPc1 &&
            Command.ProfileDefaultMode != desklink::DeskMode::Game;
#ifdef DESKLINK_ENABLE_VALIDATION_FAULTS
        RequireRemote = RequireRemote ||
            Command.ValidationReconciliationProbe ||
            Command.ValidationTerminateHeldInput ||
            Command.ValidationStaleSessionNonce != 0;
#endif
        if (RequireRemote) {
            std::unique_lock Lock(Result->Mutex);
            if (!Result->Changed.wait_for(Lock, std::chrono::seconds(10), [&] {
                    return Result->Ready || !Result->Failure.empty();
                }) || !Result->Ready || !ActiveInput->RemoteFocused()) {
                std::cerr << "[Input:Lifecycle] "
                          << (Result->Failure.empty()
                                  ? "fresh remote focus was not admitted"
                                  : Result->Failure)
                          << '\n';
                Lock.unlock();
                ActiveInput->Stop();
                ActiveHost->StopAudio();
                ActiveHost->Session.stop();
                Bootstrap->Close();
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                return 1;
            }
        }
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
        std::cout
            << "[Session:Control] trusted Host runtime active; profiles control focus; "
               "press Enter to stop\n";
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
            if (std::chrono::steady_clock::now() >= ValidationDeadline) {
                std::cout
                    << "[Input:Lifecycle] validation duration elapsed; stopping locally\n";
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
        ActiveInput->Stop();
        ActiveHost->StopAudio();
        ActiveHost->Session.stop();
        {
            std::scoped_lock Lock(Result->Mutex);
            if (!Result->Failure.empty()) {
                std::cerr << "[Input:Lifecycle] " << Result->Failure << '\n';
                ExitCode = 1;
            }
        }
    }

    Bootstrap->Close();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    return ExitCode;
}

int Run(const CommandLine& Command) {
    if (Command.Mode == Operation::Control) return RunControl(Command);
    if (Command.Mode == Operation::Discover) return RunDiscovery(Command);

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
                          Pairing, Clock, LocalIdentity);
    }
    if (!Pairing.BeginPairing(kPairingWindow)) {
        std::cerr << "[Pairing:Control] could not open the pairing window\n";
        return 1;
    }

    const auto Result = std::make_shared<PairingResult>();
    desklink::MsQuicBootstrapHandlers Handlers;
    Handlers.PairingOffered = [Result,
                               GrantInput = Command.GrantInput,
                               GrantAudioSend = Command.GrantAudioSend,
                               GrantAudioReceive = Command.GrantAudioReceive,
                               ConsoleConfirm = Command.ConsoleConfirm](
        std::shared_ptr<desklink::MsQuicPairingSession> Session) {
        HandlePairingOffer(
            Result, std::move(Session), GrantInput, GrantAudioSend,
            GrantAudioReceive, ConsoleConfirm);
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

    std::unique_ptr<desklink::Win32MdnsAdvertiser> Advertiser;
    if (Command.Mode == Operation::PairListen) {
        if (!Bootstrap->StartListener(Command.Port)) {
            std::cerr << "[Pairing:Control] could not listen on UDP port "
                      << Command.Port << '\n';
            return 1;
        }
        std::cout << "[Pairing:Control] pairing window open on UDP port "
                  << Bootstrap->BoundPort() << " for five minutes\n";
        Advertiser = std::make_unique<desklink::Win32MdnsAdvertiser>();
        const auto Advertisement = GetDiscoveryAdvertisement(
            LocalIdentity, Bootstrap->BoundPort(), true);
        if (Advertiser->Start(Advertisement)) {
            std::cout << "[Discovery:Advertise] pairing endpoint advertised "
                         "on the local link\n";
        } else {
            std::cerr << "[Discovery:Advertise] unavailable; status="
                      << Advertiser->LastStatus()
                      << "; manual address pairing remains active\n";
        }
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
    if (Advertiser) Advertiser->Stop();
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
    // The native alpha wrapper captures these streams through anonymous pipes.
    // Immediate flushing keeps diagnostics observable without changing any
    // transport, credential, or admission behavior.
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    const auto Command = ParseCommandLine(ArgumentCount, Arguments);
    if (!Command) {
        PrintUsage();
        return 2;
    }
    return Run(*Command);
}
