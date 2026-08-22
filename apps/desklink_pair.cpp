#include "desklink/msquic_bootstrap.hpp"
#include "desklink/session.hpp"
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

enum class Operation {
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

void PrintUsage() {
    std::wcerr
        << L"Usage:\n"
        << L"  desklink_pair listen [port] [--grant-input]\n"
        << L"  desklink_pair pair <host-or-ip> [port] [--grant-input]\n"
        << L"  desklink_pair serve [port]\n"
        << L"  desklink_pair focus <host-or-ip> [port]\n\n"
        << L"--grant-input allows the newly paired remote PC to inject input on this PC.\n";
}

std::optional<CommandLine> ParseCommandLine(int ArgumentCount, wchar_t** Arguments) {
    if (ArgumentCount < 2) return std::nullopt;
    CommandLine Result;
    int Index = 2;
    const std::wstring_view Command(Arguments[1]);
    if (Command == L"listen") {
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
    for (; Index < ArgumentCount; ++Index) {
        const std::wstring_view Argument(Arguments[Index]);
        if (Argument == L"--grant-input") {
            if (Result.GrantInput) return std::nullopt;
            Result.GrantInput = true;
            continue;
        }
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
                    bool GrantInput) {
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

    return MessageBoxW(
        nullptr, Text.c_str(), L"DeskLink pairing confirmation",
        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_SETFOREGROUND | MB_TOPMOST) == IDYES;
}

void HandlePairingOffer(const std::shared_ptr<PairingResult>& Result,
                        std::shared_ptr<desklink::MsQuicPairingSession> Session,
                        bool GrantInput) {
    {
        std::scoped_lock Lock(Result->Mutex);
        if (Result->PromptActive || Result->Completed) {
            Session->Reject();
            return;
        }
        Result->PromptActive = true;
    }

    const bool UserConfirmed = ConfirmPairing(*Session, GrantInput);
    desklink::CapabilitySet Capabilities;
    if (GrantInput) Capabilities.grant(desklink::Capability::InputInject);
    const bool Accepted = UserConfirmed &&
        Session->Confirm(Session->Candidate().VerificationCode, Capabilities);
    if (!UserConfirmed) Session->Reject();

    {
        std::scoped_lock Lock(Result->Mutex);
        Result->PromptActive = false;
        Result->Completed = true;
        Result->Accepted = Accepted;
        if (UserConfirmed && !Accepted) {
            Result->Failure = "pairing confirmation expired or trust persistence failed";
        }
    }
    Result->Changed.notify_all();
}

struct TrustedResult {
    std::mutex Mutex;
    std::condition_variable Changed;
    bool Ready{};
    std::string Failure;
};

struct AgentRuntime {
    AgentRuntime(const desklink::IClock& Clock,
                 const desklink::ITrustStore& TrustStore,
                 desklink::MsQuicBootstrapHandlers::TrustedSession Trusted)
        : Coordinator(Clock, Injector),
          Session(std::move(Trusted.Endpoint), Coordinator, TrustStore,
                  Trusted.SessionNonce) {}

    desklink::Win32InputInjector Injector;
    desklink::AgentCoordinator Coordinator;
    desklink::AgentSession Session;
};

struct HostRuntime {
    HostRuntime(const desklink::ITrustStore& TrustStore,
                desklink::MsQuicBootstrapHandlers::TrustedSession Trusted,
                std::function<void()> FocusReadyHandler)
        : Coordinator(Trusted.SessionNonce),
          Session(std::move(Trusted.Endpoint), Coordinator, TrustStore,
                  Trusted.SessionNonce, std::move(FocusReadyHandler)) {}

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

    desklink::MsQuicBootstrapHandlers Handlers;
    Handlers.PairingOffered = [](std::shared_ptr<desklink::MsQuicPairingSession> Session) {
        Session->Reject();
    };
    Handlers.Failed = [Result](std::string Message) {
        {
            std::scoped_lock Lock(Result->Mutex);
            if (Result->Failure.empty()) Result->Failure = std::move(Message);
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
            auto Runtime = std::make_shared<AgentRuntime>(
                Clock, TrustStore, std::move(Trusted));
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
        std::move(Handlers));
    if (!Bootstrap) {
        std::cerr << "[Session:Control] could not initialize MsQuic\n";
        return 1;
    }

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
        std::string Line;
        (void)std::getline(std::cin, Line);
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
        std::cout << "[Session:Control] remote focus active; press Enter to release\n";
        std::atomic_bool StopRenewal{};
        std::thread Renewal([&] {
            while (!StopRenewal.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if (!StopRenewal.load() && !ActiveHost->Session.renew_focus(750)) break;
            }
        });
        std::string Line;
        (void)std::getline(std::cin, Line);
        StopRenewal.store(true);
        Renewal.join();
        (void)ActiveHost->Session.release_focus();
        ActiveHost->Session.stop();
    }

    Bootstrap->Close();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    return 0;
}

int Run(const CommandLine& Command) {
    const auto DataDirectory = GetDataDirectory();
    const auto DisplayName = GetDisplayName();
    if (!DataDirectory || !DisplayName) {
        std::cerr << "[Pairing:Control] could not initialize current-user data paths\n";
        return 1;
    }

    desklink::BCryptPairingCrypto Crypto;
    auto Certificate = desklink::Win32DeviceCertificate::LoadOrCreate(
        kDeviceKeyName, Crypto);
    if (!Certificate) {
        std::cerr << "[Pairing:Control] could not load or create the device identity\n";
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
    Handlers.PairingOffered = [Result, GrantInput = Command.GrantInput](
        std::shared_ptr<desklink::MsQuicPairingSession> Session) {
        HandlePairingOffer(Result, std::move(Session), GrantInput);
    };
    Handlers.Connected = [](desklink::MsQuicBootstrapHandlers::TrustedSession Session) {
        Session.Endpoint->close();
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
        std::move(*Certificate), TrustStore, Crypto, Pairing, Clock, std::move(Handlers));
    if (!Bootstrap) {
        std::scoped_lock Lock(Result->Mutex);
        std::cerr << "[Pairing:Control] "
                  << (Result->Failure.empty()
                          ? "could not initialize MsQuic"
                          : Result->Failure)
                  << '\n';
        return 1;
    }

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
