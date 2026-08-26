#include "desklink/win32_launcher.hpp"

#include "desklink/discovery.hpp"

#include <algorithm>
#include <charconv>
#include <limits>

namespace desklink {
namespace {

constexpr std::size_t kMaximumHostLength = 253;
constexpr std::size_t kMaximumWindowsCommandLine = 32'767;

bool IsValidHost(std::wstring_view Host) noexcept {
    if (Host.empty() || Host.size() > kMaximumHostLength) return false;
    if (!std::all_of(Host.begin(), Host.end(), [](wchar_t Character) {
        return Character > L' ' && Character != L'\"' && Character != 0x7f;
    })) {
        return false;
    }
    if (Host.front() == L'[') {
        return Host.back() == L']' && Host.size() > 3 &&
               Host.substr(1, Host.size() - 2).find(L':') !=
                   std::wstring_view::npos;
    }
    if (Host.find_first_of(L"[]") != std::wstring_view::npos) return false;
    // The wrapper owns the UDP port as a separate field. Reject the common
    // host:port mistake instead of constructing host:port:port downstream.
    return std::count(Host.begin(), Host.end(), L':') != 1;
}

bool HasPairingGrants(const LauncherRequest& Request) noexcept {
    return Request.GrantInput || Request.GrantAudioSend ||
           Request.GrantAudioReceive || Request.GrantTopology ||
           Request.GrantClipboardRead || Request.GrantClipboardWrite;
}

void AppendPort(std::vector<std::wstring>& Arguments, std::uint16_t Port) {
    Arguments.push_back(std::to_wstring(Port));
}

void AppendPairingGrants(std::vector<std::wstring>& Arguments,
                         const LauncherRequest& Request) {
    if (Request.GrantInput) Arguments.emplace_back(L"--grant-input");
    if (Request.GrantAudioSend) {
        Arguments.emplace_back(L"--grant-audio-send");
    }
    if (Request.GrantAudioReceive) {
        Arguments.emplace_back(L"--grant-audio-receive");
    }
    if (Request.GrantTopology) {
        Arguments.emplace_back(L"--grant-topology");
    }
    if (Request.GrantClipboardRead) {
        Arguments.emplace_back(L"--grant-clipboard-read");
    }
    if (Request.GrantClipboardWrite) {
        Arguments.emplace_back(L"--grant-clipboard-write");
    }
}

void AppendProductionProvider(std::vector<std::wstring>& Arguments) {
    Arguments.emplace_back(L"--tls-provider");
    Arguments.emplace_back(L"schannel");
}

void AppendBrokerManagement(std::vector<std::wstring>& Arguments,
                            const LauncherRequest& Request) {
    if (Request.BrokerManaged) Arguments.emplace_back(L"--broker-managed");
}

std::wstring FormatPairingToken(const ControlPairingToken& Token) {
    constexpr wchar_t Hex[] = L"0123456789abcdef";
    std::wstring Result;
    Result.reserve(Token.size() * 2u);
    for (const auto Byte : Token) {
        Result.push_back(Hex[Byte >> 4u]);
        Result.push_back(Hex[Byte & 0x0fu]);
    }
    return Result;
}

void AppendBrokerPairing(std::vector<std::wstring>& Arguments,
                         const LauncherRequest& Request) {
    if (!Request.BrokerPairingOperationId || !Request.BrokerPairingToken) {
        return;
    }
    Arguments.emplace_back(L"--broker-pairing");
    Arguments.push_back(std::to_wstring(*Request.BrokerPairingOperationId));
    Arguments.push_back(FormatPairingToken(*Request.BrokerPairingToken));
}

void AppendExpectedPeer(std::vector<std::wstring>& Arguments,
                        const LauncherRequest& Request) {
    if (!Request.ExpectedPeerMachine) return;
    const auto Text = FormatDiscoveryMachineId(*Request.ExpectedPeerMachine);
    Arguments.emplace_back(L"--expected-peer");
    Arguments.emplace_back(Text.begin(), Text.end());
}

} // namespace

std::optional<std::vector<std::wstring>> BuildLauncherArguments(
    const LauncherRequest& Request) {
    if (Request.Port == 0 || Request.DiscoverySeconds == 0 ||
        Request.DiscoverySeconds > 30 ||
        !IsValidWin32PointerCalibration(Request.PointerCalibration)) {
        return std::nullopt;
    }
    const auto HasEdgeRoaming =
        !Request.EdgeRoamingSettingsPath.empty();
    const bool DirectionalSession =
        Request.Operation == LauncherOperation::Focus ||
        Request.Operation == LauncherOperation::Serve;
    if (HasEdgeRoaming &&
        (!DirectionalSession ||
         !Request.CaptureInput ||
         !Request.EdgeRoamingSettingsPath.is_absolute() ||
         Request.EdgeRoamingSettingsPath.native().size() >=
             kMaximumWindowsCommandLine)) {
        return std::nullopt;
    }

    const bool HasHost = !Request.Host.empty();
    const bool HostRequired =
        Request.Operation == LauncherOperation::PairConnect ||
        Request.Operation == LauncherOperation::Focus;
    if (HasHost != HostRequired || (HostRequired && !IsValidHost(Request.Host))) {
        return std::nullopt;
    }
    if ((Request.ExpectedPeerMachine &&
         (Request.Operation != LauncherOperation::Focus ||
          std::all_of(Request.ExpectedPeerMachine->begin(),
                      Request.ExpectedPeerMachine->end(),
                      [](std::uint8_t Byte) { return Byte == 0; }))) ||
        (Request.BrokerManaged &&
         Request.Operation != LauncherOperation::Serve &&
         Request.Operation != LauncherOperation::Focus) ||
        (Request.BrokerPairingOperationId.has_value() !=
         Request.BrokerPairingToken.has_value()) ||
        (Request.BrokerPairingOperationId &&
         (*Request.BrokerPairingOperationId == 0 ||
          std::all_of(
              Request.BrokerPairingToken->begin(),
              Request.BrokerPairingToken->end(),
              [](std::uint8_t Byte) { return Byte == 0; }) ||
          (Request.Operation != LauncherOperation::PairListen &&
           Request.Operation != LauncherOperation::PairConnect)))) {
        return std::nullopt;
    }

    std::vector<std::wstring> Arguments;
    switch (Request.Operation) {
        case LauncherOperation::Identity:
            if (HasPairingGrants(Request) || Request.CaptureInput ||
                Request.SendAudio || Request.ReceiveAudio ||
                Request.SyncClipboard ||
                Request.PointerCalibration.GainPercent != 100 ||
                Request.PointerCalibration.SourceDpi != 0) {
                return std::nullopt;
            }
            Arguments.emplace_back(L"identity");
            break;
        case LauncherOperation::Discover:
            if (HasPairingGrants(Request) || Request.CaptureInput ||
                Request.SendAudio || Request.ReceiveAudio ||
                Request.SyncClipboard ||
                Request.PointerCalibration.GainPercent != 100 ||
                Request.PointerCalibration.SourceDpi != 0) {
                return std::nullopt;
            }
            Arguments.emplace_back(L"discover");
            Arguments.push_back(std::to_wstring(Request.DiscoverySeconds));
            break;
        case LauncherOperation::PairListen:
            if (Request.CaptureInput || Request.SendAudio ||
                Request.ReceiveAudio || Request.SyncClipboard ||
                Request.PointerCalibration.GainPercent != 100 ||
                Request.PointerCalibration.SourceDpi != 0) {
                return std::nullopt;
            }
            Arguments.emplace_back(L"listen");
            AppendPort(Arguments, Request.Port);
            AppendPairingGrants(Arguments, Request);
            AppendBrokerPairing(Arguments, Request);
            AppendProductionProvider(Arguments);
            break;
        case LauncherOperation::PairConnect:
            if (Request.CaptureInput || Request.SendAudio ||
                Request.ReceiveAudio || Request.SyncClipboard ||
                Request.PointerCalibration.GainPercent != 100 ||
                Request.PointerCalibration.SourceDpi != 0) {
                return std::nullopt;
            }
            Arguments.emplace_back(L"pair");
            Arguments.push_back(Request.Host);
            AppendPort(Arguments, Request.Port);
            AppendPairingGrants(Arguments, Request);
            AppendBrokerPairing(Arguments, Request);
            AppendProductionProvider(Arguments);
            break;
        case LauncherOperation::Serve:
            if (HasPairingGrants(Request) || Request.ReceiveAudio ||
                (Request.CaptureInput && !HasEdgeRoaming) ||
                (!Request.CaptureInput &&
                 (Request.PointerCalibration.GainPercent != 100 ||
                  Request.PointerCalibration.SourceDpi != 0))) {
                return std::nullopt;
            }
            Arguments.emplace_back(L"serve");
            AppendPort(Arguments, Request.Port);
            if (Request.SendAudio) Arguments.emplace_back(L"--send-audio");
            if (Request.SyncClipboard) {
                Arguments.emplace_back(L"--sync-clipboard");
            }
            if (Request.CaptureInput) Arguments.emplace_back(L"--capture");
            if (Request.CaptureInput &&
                Request.PointerCalibration.GainPercent != 100) {
                Arguments.emplace_back(L"--pointer-gain");
                Arguments.push_back(std::to_wstring(
                    Request.PointerCalibration.GainPercent));
            }
            if (Request.CaptureInput &&
                Request.PointerCalibration.SourceDpi != 0) {
                Arguments.emplace_back(L"--pointer-dpi");
                Arguments.push_back(std::to_wstring(
                    Request.PointerCalibration.SourceDpi));
            }
            if (HasEdgeRoaming) {
                Arguments.emplace_back(L"--edge-roaming");
                Arguments.push_back(
                    Request.EdgeRoamingSettingsPath.native());
            }
            AppendBrokerManagement(Arguments, Request);
            AppendProductionProvider(Arguments);
            break;
        case LauncherOperation::Focus:
            if (HasPairingGrants(Request) || Request.SendAudio) {
                return std::nullopt;
            }
            if (!Request.CaptureInput &&
                (Request.PointerCalibration.GainPercent != 100 ||
                 Request.PointerCalibration.SourceDpi != 0)) {
                return std::nullopt;
            }
            Arguments.emplace_back(L"focus");
            Arguments.push_back(Request.Host);
            AppendPort(Arguments, Request.Port);
            if (Request.CaptureInput) Arguments.emplace_back(L"--capture");
            if (Request.CaptureInput &&
                Request.PointerCalibration.GainPercent != 100) {
                Arguments.emplace_back(L"--pointer-gain");
                Arguments.push_back(std::to_wstring(
                    Request.PointerCalibration.GainPercent));
            }
            if (Request.CaptureInput &&
                Request.PointerCalibration.SourceDpi != 0) {
                Arguments.emplace_back(L"--pointer-dpi");
                Arguments.push_back(std::to_wstring(
                    Request.PointerCalibration.SourceDpi));
            }
            if (Request.ReceiveAudio) {
                Arguments.emplace_back(L"--receive-audio");
            }
            if (Request.SyncClipboard) {
                Arguments.emplace_back(L"--sync-clipboard");
            }
            if (HasEdgeRoaming) {
                Arguments.emplace_back(L"--edge-roaming");
                Arguments.push_back(
                    Request.EdgeRoamingSettingsPath.native());
            }
            AppendExpectedPeer(Arguments, Request);
            AppendBrokerManagement(Arguments, Request);
            // Connecting never immediately steals physical input. The wrapper
            // must issue an explicit authenticated mode=roam request afterward.
            Arguments.emplace_back(L"--default-mode");
            Arguments.emplace_back(L"lock-pc1");
            AppendProductionProvider(Arguments);
            break;
    }
    return Arguments;
}

std::wstring QuoteWindowsCommandArgument(std::wstring_view Argument) {
    if (!Argument.empty() &&
        Argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(Argument);
    }

    std::wstring Result(1, L'\"');
    std::size_t Backslashes = 0;
    for (const auto Character : Argument) {
        if (Character == L'\\') {
            ++Backslashes;
            continue;
        }
        if (Character == L'\"') {
            Result.append(Backslashes * 2 + 1, L'\\');
            Result.push_back(L'\"');
        } else {
            Result.append(Backslashes, L'\\');
            Result.push_back(Character);
        }
        Backslashes = 0;
    }
    Result.append(Backslashes * 2, L'\\');
    Result.push_back(L'\"');
    return Result;
}

std::optional<std::wstring> BuildWindowsCommandLine(
    std::wstring_view Application,
    const std::vector<std::wstring>& Arguments) {
    if (Application.empty()) return std::nullopt;
    std::wstring Result = QuoteWindowsCommandArgument(Application);
    for (const auto& Argument : Arguments) {
        const auto Quoted = QuoteWindowsCommandArgument(Argument);
        if (Result.size() + Quoted.size() + 1 >= kMaximumWindowsCommandLine) {
            return std::nullopt;
        }
        Result.push_back(L' ');
        Result += Quoted;
    }
    return Result;
}

} // namespace desklink
