#include "desklink/msquic_bootstrap.hpp"
#include "desklink/protocol.hpp"
#include "desklink/win32_pairing.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

#define CHECK(Expression) do { if (!(Expression)) { \
    throw std::runtime_error("CHECK failed at line " + std::to_string(__LINE__) + \
                             ": " #Expression); \
} } while (false)

desklink::MachineId MakeMachineId(std::uint8_t Marker) {
    desklink::MachineId Result{};
    Result[0] = Marker;
    Result[15] = static_cast<std::uint8_t>(Marker ^ 0xA5u);
    return Result;
}

struct Results {
    std::mutex Mutex;
    std::condition_variable Changed;
    std::vector<std::shared_ptr<desklink::MsQuicPairingSession>> PairingSessions;
    std::vector<desklink::MsQuicBootstrapHandlers::TrustedSession> TrustedSessions;
    std::vector<std::string> Failures;
};

bool WaitFor(Results& Shared,
             std::size_t PairingCount,
             std::size_t EndpointCount,
             std::chrono::seconds Timeout) {
    std::unique_lock Lock(Shared.Mutex);
    return Shared.Changed.wait_for(Lock, Timeout, [&] {
        return Shared.PairingSessions.size() >= PairingCount &&
               Shared.TrustedSessions.size() >= EndpointCount;
    });
}

desklink::MsQuicBootstrapHandlers MakeHandlers(Results& Shared) {
    desklink::MsQuicBootstrapHandlers Handlers;
    Handlers.PairingOffered = [&](std::shared_ptr<desklink::MsQuicPairingSession> Session) {
        {
            std::scoped_lock Lock(Shared.Mutex);
            Shared.PairingSessions.push_back(std::move(Session));
        }
        Shared.Changed.notify_all();
    };
    Handlers.Connected = [&](desklink::MsQuicBootstrapHandlers::TrustedSession Session) {
        {
            std::scoped_lock Lock(Shared.Mutex);
            Shared.TrustedSessions.push_back(std::move(Session));
        }
        Shared.Changed.notify_all();
    };
    Handlers.Failed = [&](std::string Message) {
        {
            std::scoped_lock Lock(Shared.Mutex);
            Shared.Failures.push_back(std::move(Message));
        }
        Shared.Changed.notify_all();
    };
    return Handlers;
}

void RunLoopback(const std::wstring& FirstKeyName,
                 const std::wstring& SecondKeyName) {
    using namespace desklink;
    BCryptPairingCrypto Crypto;
    SteadyClock Clock;
    auto FirstCertificate = Win32DeviceCertificate::LoadOrCreate(FirstKeyName, Crypto);
    auto SecondCertificate = Win32DeviceCertificate::LoadOrCreate(SecondKeyName, Crypto);
    CHECK(FirstCertificate.has_value());
    CHECK(SecondCertificate.has_value());

    PeerIdentity FirstIdentity{
        MakeMachineId(1), "Loopback server",
        FormatFingerprint(FirstCertificate->CertificatePin())};
    PeerIdentity SecondIdentity{
        MakeMachineId(2), "Loopback client",
        FormatFingerprint(SecondCertificate->CertificatePin())};
    InMemoryTrustStore FirstTrust;
    InMemoryTrustStore SecondTrust;
    PairingCoordinator FirstPairing(
        FirstIdentity, FirstCertificate->CertificatePin(), Clock, Crypto, FirstTrust);
    PairingCoordinator SecondPairing(
        SecondIdentity, SecondCertificate->CertificatePin(), Clock, Crypto, SecondTrust);
    CHECK(FirstPairing.BeginPairing(std::chrono::seconds(30)));
    CHECK(SecondPairing.BeginPairing(std::chrono::seconds(30)));

    Results Shared;
    auto First = MsQuicBootstrap::Create(
        std::move(*FirstCertificate), FirstTrust, Crypto, FirstPairing, Clock,
        MakeHandlers(Shared));
    auto Second = MsQuicBootstrap::Create(
        std::move(*SecondCertificate), SecondTrust, Crypto, SecondPairing, Clock,
        MakeHandlers(Shared));
    if (!First || !Second) {
        std::scoped_lock Lock(Shared.Mutex);
        for (const auto& Failure : Shared.Failures) {
            std::cerr << "[Transport:MsQuic] " << Failure << '\n';
        }
    }
    CHECK(First);
    CHECK(Second);
    CHECK(First->StartListener());
    CHECK(First->BoundPort() != 0);
    CHECK(Second->ConnectForPairing("127.0.0.1", First->BoundPort()));
    CHECK(WaitFor(Shared, 2, 0, std::chrono::seconds(10)));

    std::shared_ptr<MsQuicPairingSession> FirstSession;
    std::shared_ptr<MsQuicPairingSession> SecondSession;
    {
        std::scoped_lock Lock(Shared.Mutex);
        CHECK(Shared.PairingSessions.size() == 2);
        for (const auto& Session : Shared.PairingSessions) {
            if (Session->RemoteOffer().Machine == SecondIdentity.machine_id) {
                FirstSession = Session;
            } else if (Session->RemoteOffer().Machine == FirstIdentity.machine_id) {
                SecondSession = Session;
            }
        }
    }
    CHECK(FirstSession);
    CHECK(SecondSession);
    CHECK(FirstSession->Candidate().VerificationCode ==
          SecondSession->Candidate().VerificationCode);
    CapabilitySet FirstGrant;
    FirstGrant.grant(Capability::InputInject);
    CHECK(FirstSession->Confirm(
        FirstSession->Candidate().VerificationCode, FirstGrant));
    CHECK(SecondSession->Confirm(
        SecondSession->Candidate().VerificationCode, CapabilitySet{}));
    {
        std::scoped_lock Lock(Shared.Mutex);
        Shared.PairingSessions.clear();
    }
    CHECK(FirstTrust.GetPeer(SecondIdentity.machine_id).has_value());
    CHECK(SecondTrust.GetPeer(FirstIdentity.machine_id).has_value());

    CHECK(Second->ConnectTrusted(
        "127.0.0.1", First->BoundPort(), FirstIdentity.machine_id));
    CHECK(WaitFor(Shared, 0, 2, std::chrono::seconds(10)));

    std::shared_ptr<MsQuicTransportEndpoint> FirstEndpoint;
    std::shared_ptr<MsQuicTransportEndpoint> SecondEndpoint;
    std::uint64_t SessionNonce = 0;
    bool SawInitiator = false;
    bool SawAcceptor = false;
    {
        std::scoped_lock Lock(Shared.Mutex);
        CHECK(Shared.TrustedSessions.size() == 2);
        for (const auto& Session : Shared.TrustedSessions) {
            CHECK(Session.SessionNonce != 0);
            if (SessionNonce == 0) SessionNonce = Session.SessionNonce;
            CHECK(Session.SessionNonce == SessionNonce);
            SawInitiator = SawInitiator || Session.Initiator;
            SawAcceptor = SawAcceptor || !Session.Initiator;
            const auto Peer = Session.Endpoint->peer_info();
            CHECK(Peer.authenticated && Peer.encrypted);
            if (Peer.identity.machine_id == SecondIdentity.machine_id) {
                CHECK(!Session.Initiator);
                FirstEndpoint = Session.Endpoint;
            } else if (Peer.identity.machine_id == FirstIdentity.machine_id) {
                CHECK(Session.Initiator);
                SecondEndpoint = Session.Endpoint;
            }
        }
        CHECK(Shared.Failures.empty());
    }
    CHECK(FirstEndpoint);
    CHECK(SecondEndpoint);
    CHECK(SawInitiator);
    CHECK(SawAcceptor);

    std::mutex ReceiveMutex;
    std::condition_variable ReceiveChanged;
    bool Received = false;
    FirstEndpoint->set_reliable_handler([&](ByteBuffer Bytes) {
        const auto Decoded = decode_packet(Bytes, false);
        {
            std::scoped_lock Lock(ReceiveMutex);
            Received = Decoded.packet.has_value() &&
                       std::holds_alternative<KeyEventMessage>(Decoded.packet->message);
        }
        ReceiveChanged.notify_all();
    });
    EnvelopeHeader Header;
    Header.session_nonce = SessionNonce;
    Header.epoch = 1;
    Header.sequence = 1;
    CHECK(SecondEndpoint->send_reliable(
        encode_packet(Header, KeyEventMessage{30, false, true})));
    {
        std::unique_lock Lock(ReceiveMutex);
        CHECK(ReceiveChanged.wait_for(
            Lock, std::chrono::seconds(5), [&] { return Received; }));
    }

    FirstEndpoint->close();
    SecondEndpoint->close();
    {
        std::scoped_lock Lock(Shared.Mutex);
        Shared.TrustedSessions.clear();
    }

    CHECK(Second->ConnectTrusted(
        "127.0.0.1", First->BoundPort(), FirstIdentity.machine_id));
    CHECK(WaitFor(Shared, 0, 2, std::chrono::seconds(10)));
    {
        std::scoped_lock Lock(Shared.Mutex);
        CHECK(Shared.TrustedSessions.size() == 2);
        const auto ReconnectNonce = Shared.TrustedSessions.front().SessionNonce;
        CHECK(ReconnectNonce != 0);
        CHECK(ReconnectNonce != SessionNonce);
        for (const auto& Session : Shared.TrustedSessions) {
            CHECK(Session.SessionNonce == ReconnectNonce);
            Session.Endpoint->close();
        }
        Shared.TrustedSessions.clear();
    }
    First->Close();
    Second->Close();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
}

} // namespace

int main() {
    const auto Suffix = std::to_wstring(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto FirstKeyName = std::wstring(L"DeskLink-Loopback-A-") + Suffix;
    const auto SecondKeyName = std::wstring(L"DeskLink-Loopback-B-") + Suffix;
    desklink::Win32DeviceCertificate::Remove(FirstKeyName);
    desklink::Win32DeviceCertificate::Remove(SecondKeyName);
    try {
        RunLoopback(FirstKeyName, SecondKeyName);
        CHECK(desklink::Win32DeviceCertificate::Remove(FirstKeyName));
        CHECK(desklink::Win32DeviceCertificate::Remove(SecondKeyName));
        std::cout << "[Transport:MsQuic] native loopback pairing and session passed.\n";
        return 0;
    } catch (const std::exception& Error) {
        desklink::Win32DeviceCertificate::Remove(FirstKeyName);
        desklink::Win32DeviceCertificate::Remove(SecondKeyName);
        std::cerr << "[Transport:MsQuic] " << Error.what() << '\n';
        return 1;
    }
}
