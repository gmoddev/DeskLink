#include "desklink/msquic_bootstrap.hpp"
#include "desklink/protocol.hpp"
#include "desklink/win32_pairing.hpp"

#include <ncrypt.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
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

struct TestIdentityCleanup {
    std::vector<std::wstring> KeyNames;

    ~TestIdentityCleanup() {
        for (const auto& KeyName : KeyNames) {
            desklink::Win32DeviceCertificate::Remove(KeyName);
        }
    }
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

bool CreatePersistedTestKey(NCRYPT_PROV_HANDLE Provider,
                            const std::wstring& KeyName,
                            bool Exportable,
                            NCRYPT_KEY_HANDLE& Key) {
    if (NCryptCreatePersistedKey(
            Provider, &Key, NCRYPT_RSA_ALGORITHM, KeyName.c_str(), 0, 0) !=
        ERROR_SUCCESS) {
        return false;
    }
    DWORD Length = 2048;
    DWORD Usage = NCRYPT_ALLOW_SIGNING_FLAG;
    DWORD ExportPolicy = NCRYPT_ALLOW_EXPORT_FLAG;
    if (NCryptSetProperty(
            Key, NCRYPT_LENGTH_PROPERTY, reinterpret_cast<PBYTE>(&Length),
            sizeof(Length), 0) != ERROR_SUCCESS ||
        NCryptSetProperty(
            Key, NCRYPT_KEY_USAGE_PROPERTY, reinterpret_cast<PBYTE>(&Usage),
            sizeof(Usage), 0) != ERROR_SUCCESS ||
        (Exportable && NCryptSetProperty(
            Key, NCRYPT_EXPORT_POLICY_PROPERTY,
            reinterpret_cast<PBYTE>(&ExportPolicy), sizeof(ExportPolicy), 0) !=
            ERROR_SUCCESS) ||
        NCryptFinalizeKey(Key, 0) != ERROR_SUCCESS) {
        NCryptDeleteKey(Key, 0);
        Key = 0;
        return false;
    }
    return true;
}

bool InstallTestIdentity(const std::wstring& CertificateKeyName,
                         const std::wstring& CredentialKeyName,
                         bool Exportable) {
    NCRYPT_PROV_HANDLE Provider{};
    NCRYPT_KEY_HANDLE CertificateKey{};
    NCRYPT_KEY_HANDLE CredentialKey{};
    PCCERT_CONTEXT Certificate{};
    HCERTSTORE Store{};
    bool Success = false;
    std::vector<std::uint8_t> SubjectBytes;
    DWORD SubjectSize = 0;
    if (NCryptOpenStorageProvider(
            &Provider, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS ||
        !CreatePersistedTestKey(
            Provider, CertificateKeyName,
            Exportable && CertificateKeyName == CredentialKeyName,
            CertificateKey)) {
        goto Exit;
    }
    if (CredentialKeyName != CertificateKeyName &&
        !CreatePersistedTestKey(
            Provider, CredentialKeyName, Exportable, CredentialKey)) {
        goto Exit;
    }
    if (!CertStrToNameW(
            X509_ASN_ENCODING, L"CN=DeskLink Credential Test",
            CERT_X500_NAME_STR, nullptr, nullptr, &SubjectSize, nullptr) ||
        SubjectSize == 0) {
        goto Exit;
    }
    SubjectBytes.resize(SubjectSize);
    if (!CertStrToNameW(
            X509_ASN_ENCODING, L"CN=DeskLink Credential Test",
            CERT_X500_NAME_STR, nullptr, SubjectBytes.data(), &SubjectSize,
            nullptr)) {
        goto Exit;
    }
    {
        CERT_NAME_BLOB Subject{SubjectSize, SubjectBytes.data()};
        CRYPT_KEY_PROV_INFO CertificateKeyInfo{};
        CertificateKeyInfo.pwszContainerName =
            const_cast<wchar_t*>(CertificateKeyName.c_str());
        CertificateKeyInfo.pwszProvName =
            const_cast<wchar_t*>(MS_KEY_STORAGE_PROVIDER);
        CertificateKeyInfo.dwFlags = NCRYPT_SILENT_FLAG;
        CertificateKeyInfo.dwKeySpec = AT_KEYEXCHANGE;
        CRYPT_ALGORITHM_IDENTIFIER Signature{};
        Signature.pszObjId = const_cast<char*>(szOID_RSA_SHA256RSA);
        Certificate = CertCreateSelfSignCertificate(
            static_cast<HCRYPTPROV_OR_NCRYPT_KEY_HANDLE>(CertificateKey),
            &Subject, 0, &CertificateKeyInfo, &Signature, nullptr, nullptr,
            nullptr);
        if (!Certificate) goto Exit;

        CRYPT_KEY_PROV_INFO CredentialKeyInfo = CertificateKeyInfo;
        CredentialKeyInfo.pwszContainerName =
            const_cast<wchar_t*>(CredentialKeyName.c_str());
        if (!CertSetCertificateContextProperty(
                Certificate, CERT_KEY_PROV_INFO_PROP_ID, 0,
                &CredentialKeyInfo)) {
            goto Exit;
        }
    }
    Store = CertOpenStore(
        CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER, L"MY");
    if (!Store || !CertAddCertificateContextToStore(
            Store, Certificate, CERT_STORE_ADD_REPLACE_EXISTING, nullptr)) {
        goto Exit;
    }
    Success = true;
Exit:
    if (Store) CertCloseStore(Store, 0);
    if (Certificate) CertFreeCertificateContext(Certificate);
    if (CredentialKey) NCryptFreeObject(CredentialKey);
    if (CertificateKey) NCryptFreeObject(CertificateKey);
    if (Provider) NCryptFreeObject(Provider);
    return Success;
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
                 const std::wstring& SecondKeyName,
                 desklink::TlsBackend Backend) {
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
    MsQuicRuntimeConfig RuntimeConfig;
    RuntimeConfig.Backend = Backend;
    auto First = MsQuicBootstrap::Create(
        std::move(*FirstCertificate), FirstTrust, Crypto, FirstPairing, Clock,
        RuntimeConfig,
        MakeHandlers(Shared));
    auto Second = MsQuicBootstrap::Create(
        std::move(*SecondCertificate), SecondTrust, Crypto, SecondPairing, Clock,
        RuntimeConfig,
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
    FirstEndpoint.reset();
    SecondEndpoint.reset();
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
    First.reset();
    Second.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
}

void CheckOpenSslCredentialRejected(const std::wstring& KeyName) {
    using namespace desklink;
    BCryptPairingCrypto Crypto;
    SteadyClock Clock;
    auto Certificate = Win32DeviceCertificate::LoadOrCreate(KeyName, Crypto);
    CHECK(Certificate.has_value());
    const PeerIdentity Identity{
        MakeMachineId(9), "Rejected credential",
        FormatFingerprint(Certificate->CertificatePin())};
    InMemoryTrustStore Trust;
    PairingCoordinator Pairing(
        Identity, Certificate->CertificatePin(), Clock, Crypto, Trust);
    Results Shared;
    MsQuicRuntimeConfig RuntimeConfig;
    RuntimeConfig.Backend = TlsBackend::OpenSsl;
    const auto Bootstrap = MsQuicBootstrap::Create(
        std::move(*Certificate), Trust, Crypto, Pairing, Clock,
        RuntimeConfig, MakeHandlers(Shared));
    CHECK(!Bootstrap);
    {
        std::scoped_lock Lock(Shared.Mutex);
        CHECK(!Shared.Failures.empty());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
}

void RunOpenSslCredentialRejectionTests(const std::wstring& Suffix) {
    const auto ExportableKeyName =
        std::wstring(L"DeskLink-Exportable-Rejected-") + Suffix;
    {
        TestIdentityCleanup Cleanup{{ExportableKeyName}};
        desklink::Win32DeviceCertificate::Remove(ExportableKeyName);
        CHECK(InstallTestIdentity(
            ExportableKeyName, ExportableKeyName, true));
        CheckOpenSslCredentialRejected(ExportableKeyName);
    }

    const auto CertificateKeyName =
        std::wstring(L"DeskLink-Mismatch-Certificate-") + Suffix;
    const auto CredentialKeyName =
        std::wstring(L"DeskLink-Mismatch-Credential-") + Suffix;
    {
        TestIdentityCleanup Cleanup{{CredentialKeyName, CertificateKeyName}};
        desklink::Win32DeviceCertificate::Remove(CredentialKeyName);
        desklink::Win32DeviceCertificate::Remove(CertificateKeyName);
        CHECK(InstallTestIdentity(
            CertificateKeyName, CredentialKeyName, false));
        CheckOpenSslCredentialRejected(CredentialKeyName);
    }
}

} // namespace

int main(int ArgumentCount, char** Arguments) {
    const auto Backend = ArgumentCount == 2 &&
                         std::string_view(Arguments[1]) == "--openssl"
        ? desklink::TlsBackend::OpenSsl
        : desklink::TlsBackend::Schannel;
    const auto Suffix = std::to_wstring(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto FirstKeyName = std::wstring(L"DeskLink-Loopback-A-") + Suffix;
    const auto SecondKeyName = std::wstring(L"DeskLink-Loopback-B-") + Suffix;
    desklink::Win32DeviceCertificate::Remove(FirstKeyName);
    desklink::Win32DeviceCertificate::Remove(SecondKeyName);
    try {
        RunLoopback(FirstKeyName, SecondKeyName, Backend);
        if (Backend == desklink::TlsBackend::OpenSsl) {
            RunOpenSslCredentialRejectionTests(Suffix);
        }
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
