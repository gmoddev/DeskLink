#include "desklink/discovery.hpp"
#include "desklink/runtime_broker.hpp"
#ifdef DESKLINK_NETWORK_PATH_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "desklink/win32_discovery.hpp"
#include "desklink/win32_launcher.hpp"
#endif
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <set>
#include <thread>

namespace {
void Check(bool Value, const char* Message) {
    if (!Value) throw std::runtime_error(Message);
}
class TestClock final : public desklink::IClock {
public:
    time_point Now{};
    time_point now() const noexcept override { return Now; }
};
desklink::DiscoveryEndpoint GetEndpoint(std::uint32_t Interface,
                                      std::string Address, std::uint64_t Speed) {
    desklink::DiscoveryEndpoint Endpoint;
    Endpoint.Advertisement.Machine[0] = 42;
    Endpoint.Advertisement.DisplayName = "Path test PC";
    Endpoint.Advertisement.Port = 43821;
    Endpoint.Advertisement.PairingAvailable = true;
    Endpoint.InstanceName = "DeskLink-test._desklink._udp.local";
    Endpoint.HostName = "desklink-test.local";
    Endpoint.InterfaceIndex = Interface;
    Endpoint.RemoteAddress = std::move(Address);
    Endpoint.AdapterId = std::to_string(Interface);
    Endpoint.LinkSpeedBitsPerSecond = Speed;
    Endpoint.Available = true;
    return Endpoint;
}
void RunTests() {
    TestClock Clock;
    desklink::DiscoveryCache Cache(Clock);
    auto Fiber = GetEndpoint(9, "10.253.3.2", 10'000'000'000);
    auto Lan = GetEndpoint(10, "192.168.0.108", 2'500'000'000);
    Check(Cache.Observe(Lan, std::chrono::seconds(60)), "observe LAN");
    Check(Cache.Observe(Fiber, std::chrono::seconds(60)), "observe fiber");
    auto Peers = Cache.Snapshot();
    Check(Peers.size() == 1 && Peers[0].Candidates.size() == 2 &&
          Peers[0].EndpointCount == 2 && !Peers[0].Ambiguous,
          "one identity retains both interfaces");
    desklink::DiscoveryPathSelector Selector(Clock);
    Check(Selector.Select(Peers[0])->RemoteAddress == Fiber.RemoteAddress,
          "fast available path selected");
    Check(Selector.Select(Peers[0], Lan.AdapterId)->RemoteAddress == Lan.RemoteAddress,
          "explicit adapter preference");
    Selector.RecordUnavailable(Fiber);
    Check(Selector.Select(Peers[0])->RemoteAddress == Lan.RemoteAddress,
          "ordinary failure selects alternate during cooldown");
    Selector.RecordUnavailable(Lan);
    Check(!Selector.Select(Peers[0]), "no unbound escape when all paths cool down");
    Clock.Now += std::chrono::seconds(31);
    Check(Selector.Select(Peers[0])->RemoteAddress == Fiber.RemoteAddress,
          "cooldown expires");
    Selector.RecordSuccess(Lan);
    Check(Selector.Select(Peers[0])->RemoteAddress == Lan.RemoteAddress,
          "previous authenticated success preferred");
    auto Offline = Peers[0];
    for (auto& Endpoint : Offline.Candidates) Endpoint.Available = false;
    Check(!Selector.Select(Offline), "offline candidates never selected");
    auto Spoof = Fiber;
    Spoof.Advertisement.DisplayName = "conflicting identity metadata";
    Check(Cache.Observe(Spoof, std::chrono::seconds(60)), "observe conflict");
    Check(!Selector.Select(Cache.Snapshot()[0]), "ambiguous identity fails closed");
    Cache.Remove(Fiber.InstanceName, Fiber.InterfaceIndex);
    Check(Cache.Snapshot()[0].Candidates.size() == 1, "remove only one interface");
    Clock.Now += std::chrono::seconds(61);
    Check(Cache.Snapshot().empty(), "all paths expire");
    auto V6 = Fiber;
    V6.RemoteAddress = "fe80::1234%9";
    Check(Cache.Observe(Fiber, std::chrono::seconds(60)), "observe v4");
    Check(Cache.Observe(V6, std::chrono::seconds(60)), "observe v6");
    Check(Cache.Snapshot()[0].Candidates.size() == 2, "retain both address families");
    desklink::BrokerReconnectController Reconnect(42);
    Check(Reconnect.Begin(desklink::BrokerRuntimePhase::Connecting), "begin reconnect");
    Reconnect.ConnectedLocal();
    Reconnect.ProcessStopped(desklink::BrokerRuntimeFailure::OrdinaryUnavailable,
                             Clock.Now);
    Check(Reconnect.Snapshot().Phase == desklink::BrokerRuntimePhase::RetryWaiting,
          "availability failure permits new session");
    Reconnect.ProcessStopped(desklink::BrokerRuntimeFailure::Identity, Clock.Now);
    Check(!Reconnect.AttemptDue(Clock.Now + std::chrono::hours(1)),
          "identity failure never permits candidate fallback");
#ifdef DESKLINK_NETWORK_PATH_WINDOWS
    desklink::LauncherRequest Request;
    Request.Operation = desklink::LauncherOperation::Focus;
    Request.Host = L"10.253.3.2";
    Request.ExpectedPeerMachine = Fiber.Advertisement.Machine;
    Request.LocalInterfaceIndex = 9;
    const auto Args = desklink::BuildProductLauncherArguments(Request);
    Check(Args && std::find(Args->begin(), Args->end(), L"--local-interface") != Args->end(),
          "launcher preserves selected interface");
    Check(std::find(Args->begin(), Args->end(), L"--expected-peer") != Args->end(),
          "launcher retains pinned identity");
    Request.Operation = desklink::LauncherOperation::Identity;
    Request.Host.clear();
    Request.ExpectedPeerMachine.reset();
    Check(!desklink::BuildProductLauncherArguments(Request), "binding rejected on wrong operation");
#endif
}
}

int main(int Count, char** Arguments) {
    try {
#ifdef DESKLINK_NETWORK_PATH_WINDOWS
        if (Count == 2 && (std::string_view(Arguments[1]) == "--browse" ||
                           std::string_view(Arguments[1]) == "--verify-paths")) {
            const auto Result = desklink::Win32MdnsBrowser::Browse(std::chrono::seconds(2));
            std::cout << "[Discovery:Paths] status=" << Result.StartStatus
                      << " peers=" << Result.Peers.size()
                      << " browse-failures=" << Result.BrowseFailures
                      << " resolve-failures=" << Result.ResolveFailures
                      << " resolve-status=" << Result.ResolveStatus
                      << " malformed=" << Result.MalformedRecords << '\n';
            for (auto Name : Result.ServiceNames) {
                std::replace_if(Name.begin(), Name.end(), [](unsigned char Byte) {
                    return Byte < 32 || Byte == 127;
                }, '?');
                std::cout << "[Discovery:Paths] service=" << Name << '\n';
            }
            for (const auto& Peer : Result.Peers)
                for (const auto& Path : Peer.Candidates)
                    std::cout << "[Discovery:Paths] " << Peer.Endpoint.Advertisement.DisplayName
                              << " interface=" << Path.InterfaceIndex << " adapter=" << Path.AdapterName
                              << " address=" << Path.RemoteAddress << " available=" << Path.Available
                              << " host=" << Path.HostName
                              << " link-bps=" << Path.LinkSpeedBitsPerSecond << '\n';
            if (std::string_view(Arguments[1]) == "--verify-paths") {
                bool Verified = false;
                desklink::SteadyClock Clock;
                desklink::DiscoveryPathSelector Selector(Clock);
                for (const auto& Peer : Result.Peers) {
                    std::set<std::uint32_t> Interfaces;
                    std::uint64_t MaximumSpeed{};
                    for (const auto& Path : Peer.Candidates) if (Path.Available) {
                        Interfaces.insert(Path.InterfaceIndex);
                        MaximumSpeed = std::max(MaximumSpeed, Path.LinkSpeedBitsPerSecond);
                    }
                    if (Interfaces.size() < 2) continue;
                    const auto Selected = Selector.Select(Peer);
                    Check(Selected && Selected->LinkSpeedBitsPerSecond == MaximumSpeed,
                          "native multi-interface peer selects fastest eligible path");
                    Selector.RecordUnavailable(*Selected);
                    // Both address families share one adapter. Cool down all
                    // candidates on it to model the physical cable going away.
                    for (const auto& Path : Peer.Candidates)
                        if (Path.InterfaceIndex == Selected->InterfaceIndex)
                            Selector.RecordUnavailable(Path);
                    const auto Alternate = Selector.Select(Peer);
                    Check(Alternate && Alternate->InterfaceIndex != Selected->InterfaceIndex,
                          "native candidates permit a fresh alternate-interface attempt");
                    std::cout << "[Discovery:Paths] verified preferred=" << Selected->RemoteAddress
                              << " alternate=" << Alternate->RemoteAddress << '\n';
                    Verified = true;
                }
                Check(Verified, "no available peer with two distinct interfaces");
            }
            return Result.StartStatus == 0 ? 0 : 1;
        }
        if (Count == 2 && std::string_view(Arguments[1]) == "--advertise") {
            auto Advertisement = GetEndpoint(9, "", 0).Advertisement;
            Advertisement.DisplayName = "DeskLink network path probe";
            char ComputerName[64]{};
            DWORD NameLength = sizeof(ComputerName);
            Check(GetComputerNameA(ComputerName, &NameLength) != FALSE, "probe computer name");
            std::uint32_t Hash = 2'166'136'261u;
            for (DWORD Index = 0; Index < NameLength; ++Index)
                Hash = (Hash ^ static_cast<unsigned char>(ComputerName[Index])) * 16'777'619u;
            for (std::size_t Index = 0; Index < 4; ++Index)
                Advertisement.Machine[Index] = static_cast<std::uint8_t>(Hash >> (Index * 8));
            Advertisement.Machine[15] = 0xfa;
            desklink::Win32MdnsAdvertiser Advertiser;
            Check(Advertiser.Start(Advertisement), "probe advertisement failed");
            std::cout << "[Discovery:Paths] probe advertisement active for 30 seconds\n" << std::flush;
            std::this_thread::sleep_for(std::chrono::seconds(30));
            return 0;
        }
#else
        (void)Count;
        (void)Arguments;
#endif
        RunTests();
        std::cout << "[Discovery:Tests] network path invariants passed\n";
        return 0;
    } catch (const std::exception& Error) {
        std::cerr << "[Discovery:Tests] " << Error.what() << '\n';
        return 1;
    }
}
