#include "desklink/msquic_bootstrap.hpp"
#include "desklink/protocol.hpp"
#include "desklink/win32_pairing.hpp"

#include <ncrypt.h>

#include <array>
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
    std::size_t PairingCompletions{};
};

struct TestIdentityCleanup {
    std::vector<std::wstring> KeyNames;

    ~TestIdentityCleanup() {
        for (const auto& KeyName : KeyNames) {
            desklink::Win32DeviceCertificate::Remove(KeyName);
        }
    }
};

enum class CertificateValidity {
    Current,
    Expired,
    NotYetValid,
};

enum class CryptoFault {
    None,
    Failure,
    Exception,
    Stall,
};

class FaultingPairingCrypto final : public desklink::IPairingCrypto {
public:
    [[nodiscard]] bool FillRandom(std::span<std::uint8_t> Bytes) override {
        return Inner_.FillRandom(Bytes);
    }

    [[nodiscard]] std::optional<desklink::Sha256Digest> HashSha256(
        desklink::ByteSpan Bytes) const override {
        CryptoFault Fault = CryptoFault::None;
        {
            std::unique_lock Lock(Mutex_);
            Fault = Fault_;
            if (Fault == CryptoFault::Stall) {
                Released_.wait(Lock, [&] { return StallReleased_; });
                Fault = CryptoFault::Failure;
            }
        }
        if (Fault == CryptoFault::Failure) return std::nullopt;
        if (Fault == CryptoFault::Exception) {
            throw std::runtime_error("injected certificate validation exception");
        }
        return Inner_.HashSha256(Bytes);
    }

    void SetFault(CryptoFault Fault) {
        std::scoped_lock Lock(Mutex_);
        Fault_ = Fault;
        StallReleased_ = false;
    }

    void ReleaseStall() {
        {
            std::scoped_lock Lock(Mutex_);
            StallReleased_ = true;
        }
        Released_.notify_all();
    }

private:
    desklink::BCryptPairingCrypto Inner_;
    mutable std::mutex Mutex_;
    mutable std::condition_variable Released_;
    CryptoFault Fault_{CryptoFault::None};
    mutable bool StallReleased_{};
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

bool WaitForFailure(Results& Shared, std::chrono::seconds Timeout) {
    std::unique_lock Lock(Shared.Mutex);
    return Shared.Changed.wait_for(Lock, Timeout, [&] {
        return !Shared.Failures.empty();
    });
}

bool WaitForPairingCompletions(Results& Shared,
                               std::size_t Count,
                               std::chrono::seconds Timeout) {
    std::unique_lock Lock(Shared.Mutex);
    return Shared.Changed.wait_for(Lock, Timeout, [&] {
        return Shared.PairingCompletions >= Count;
    });
}

bool CreatePersistedTestKey(NCRYPT_PROV_HANDLE Provider,
                            const std::wstring& KeyName,
                            bool Exportable,
                            DWORD KeyLength,
                            NCRYPT_KEY_HANDLE& Key) {
    if (NCryptCreatePersistedKey(
            Provider, &Key, NCRYPT_RSA_ALGORITHM, KeyName.c_str(), 0, 0) !=
        ERROR_SUCCESS) {
        return false;
    }
    DWORD Length = KeyLength;
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

bool EncodeCertificateExtension(const char* Type,
                                const void* Value,
                                std::vector<std::uint8_t>& Encoded) {
    DWORD Size = 0;
    if (!CryptEncodeObjectEx(
            X509_ASN_ENCODING, Type, Value, 0, nullptr, nullptr, &Size) ||
        Size == 0) {
        return false;
    }
    Encoded.resize(Size);
    return CryptEncodeObjectEx(
        X509_ASN_ENCODING, Type, Value, 0, nullptr, Encoded.data(), &Size) !=
        FALSE;
}

bool InstallTestIdentity(const std::wstring& CertificateKeyName,
                         const std::wstring& CredentialKeyName,
                         bool Exportable,
                         CertificateValidity Validity = CertificateValidity::Current,
                         desklink::ByteBuffer* CertificateDer = nullptr,
                         DWORD KeyLength = 2048) {
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
            KeyLength,
            CertificateKey)) {
        goto Exit;
    }
    if (CredentialKeyName != CertificateKeyName &&
        !CreatePersistedTestKey(
            Provider, CredentialKeyName, Exportable, KeyLength, CredentialKey)) {
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
        std::uint8_t KeyUsageByte = CERT_DIGITAL_SIGNATURE_KEY_USAGE;
        CRYPT_BIT_BLOB KeyUsage{1, &KeyUsageByte, 0};
        std::vector<std::uint8_t> EncodedKeyUsage;
        std::array<char*, 2> EnhancedUsageOids{
            const_cast<char*>(szOID_PKIX_KP_SERVER_AUTH),
            const_cast<char*>(szOID_PKIX_KP_CLIENT_AUTH)};
        CERT_ENHKEY_USAGE EnhancedUsage{
            static_cast<DWORD>(EnhancedUsageOids.size()),
            EnhancedUsageOids.data()};
        std::vector<std::uint8_t> EncodedEnhancedUsage;
        if (!EncodeCertificateExtension(
                X509_KEY_USAGE, &KeyUsage, EncodedKeyUsage) ||
            !EncodeCertificateExtension(
                X509_ENHANCED_KEY_USAGE, &EnhancedUsage,
                EncodedEnhancedUsage)) {
            goto Exit;
        }
        std::array<CERT_EXTENSION, 2> CertificateExtensions{};
        CertificateExtensions[0] = CERT_EXTENSION{
            const_cast<char*>(szOID_KEY_USAGE), TRUE,
            CRYPT_OBJID_BLOB{
                static_cast<DWORD>(EncodedKeyUsage.size()),
                EncodedKeyUsage.data()}};
        CertificateExtensions[1] = CERT_EXTENSION{
            const_cast<char*>(szOID_ENHANCED_KEY_USAGE), FALSE,
            CRYPT_OBJID_BLOB{
                static_cast<DWORD>(EncodedEnhancedUsage.size()),
                EncodedEnhancedUsage.data()}};
        CERT_EXTENSIONS Extensions{
            static_cast<DWORD>(CertificateExtensions.size()),
            CertificateExtensions.data()};
        SYSTEMTIME StartTime{};
        SYSTEMTIME EndTime{};
        SYSTEMTIME* Start = nullptr;
        SYSTEMTIME* End = nullptr;
        if (Validity == CertificateValidity::Expired) {
            StartTime = SYSTEMTIME{2020, 1, 0, 1, 0, 0, 0, 0};
            EndTime = SYSTEMTIME{2021, 1, 0, 1, 0, 0, 0, 0};
            Start = &StartTime;
            End = &EndTime;
        } else if (Validity == CertificateValidity::NotYetValid) {
            StartTime = SYSTEMTIME{2098, 1, 0, 1, 0, 0, 0, 0};
            EndTime = SYSTEMTIME{2099, 1, 0, 1, 0, 0, 0, 0};
            Start = &StartTime;
            End = &EndTime;
        }
        Certificate = CertCreateSelfSignCertificate(
            static_cast<HCRYPTPROV_OR_NCRYPT_KEY_HANDLE>(CertificateKey),
            &Subject, 0, &CertificateKeyInfo, &Signature, Start, End,
            &Extensions);
        if (!Certificate) goto Exit;

        if (CertificateDer) {
            CertificateDer->assign(
                Certificate->pbCertEncoded,
                Certificate->pbCertEncoded + Certificate->cbCertEncoded);
        }

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

bool CngPssSigningFails(const std::wstring& KeyName) {
    NCRYPT_PROV_HANDLE Provider{};
    NCRYPT_KEY_HANDLE Key{};
    SECURITY_STATUS Status = ERROR_SUCCESS;
    std::array<std::uint8_t, 32> Digest{};
    std::array<std::uint8_t, 64> Signature{};
    DWORD SignatureLength = 0;
    BCRYPT_PSS_PADDING_INFO Padding{
        const_cast<wchar_t*>(BCRYPT_SHA256_ALGORITHM),
        static_cast<ULONG>(Digest.size())};
    if (NCryptOpenStorageProvider(
            &Provider, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS ||
        NCryptOpenKey(
            Provider, &Key, KeyName.c_str(), 0, NCRYPT_SILENT_FLAG) !=
            ERROR_SUCCESS) {
        Status = NTE_BAD_KEY;
        goto Exit;
    }
    Status = NCryptSignHash(
        Key, &Padding, Digest.data(), static_cast<DWORD>(Digest.size()),
        Signature.data(), static_cast<DWORD>(Signature.size()),
        &SignatureLength, NCRYPT_PAD_PSS_FLAG | NCRYPT_SILENT_FLAG);
Exit:
    if (Key) NCryptFreeObject(Key);
    if (Provider) NCryptFreeObject(Provider);
    return Status != ERROR_SUCCESS;
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
    Handlers.PairingCompleted = [&] {
        {
            std::scoped_lock Lock(Shared.Mutex);
            ++Shared.PairingCompletions;
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
    const auto FirstBefore = FirstCertificate->IdentitySnapshot(Crypto);
    const auto SecondBefore = SecondCertificate->IdentitySnapshot(Crypto);
    CHECK(FirstBefore.has_value());
    CHECK(SecondBefore.has_value());
    CHECK(FirstBefore->Provider == MS_KEY_STORAGE_PROVIDER);
    CHECK(SecondBefore->Provider == MS_KEY_STORAGE_PROVIDER);
    CHECK(FirstBefore->Algorithm == NCRYPT_RSA_ALGORITHM);
    CHECK(SecondBefore->Algorithm == NCRYPT_RSA_ALGORITHM);
    CHECK(FirstBefore->ExportPolicy == 0);
    CHECK(SecondBefore->ExportPolicy == 0);
    CHECK(!FirstBefore->PublicKeyDer.empty());
    CHECK(!SecondBefore->PublicKeyDer.empty());

    PeerIdentity FirstIdentity{
        DeriveMachineId(FirstCertificate->CertificatePin()), "Loopback server",
        FormatFingerprint(FirstCertificate->CertificatePin())};
    PeerIdentity SecondIdentity{
        DeriveMachineId(SecondCertificate->CertificatePin()), "Loopback client",
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
    const auto PairingCompleted = WaitForPairingCompletions(
        Shared, 2, std::chrono::seconds(10));
    if (!PairingCompleted) {
        std::scoped_lock Lock(Shared.Mutex);
        std::cerr << "[Transport:MsQuic] pairing completions="
                  << Shared.PairingCompletions << " failures="
                  << Shared.Failures.size() << '\n';
        for (const auto& Failure : Shared.Failures) {
            std::cerr << "[Transport:MsQuic] " << Failure << '\n';
        }
    }
    CHECK(PairingCompleted);
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
    std::uint64_t ReconnectNonce{};
    {
        std::scoped_lock Lock(Shared.Mutex);
        CHECK(Shared.TrustedSessions.size() == 2);
        ReconnectNonce = Shared.TrustedSessions.front().SessionNonce;
        CHECK(ReconnectNonce != 0);
        CHECK(ReconnectNonce != SessionNonce);
        for (const auto& Session : Shared.TrustedSessions) {
            CHECK(Session.SessionNonce == ReconnectNonce);
            const auto Peer = Session.Endpoint->peer_info();
            if (Peer.identity.machine_id == SecondIdentity.machine_id) {
                FirstEndpoint = Session.Endpoint;
            } else if (Peer.identity.machine_id == FirstIdentity.machine_id) {
                SecondEndpoint = Session.Endpoint;
            }
        }
        Shared.TrustedSessions.clear();
    }
    CHECK(FirstEndpoint);
    CHECK(SecondEndpoint);
    {
        std::scoped_lock Lock(ReceiveMutex);
        Received = false;
    }
    FirstEndpoint->set_reliable_handler([&](ByteBuffer Bytes) {
        const auto Decoded = decode_packet(Bytes, false);
        {
            std::scoped_lock Lock(ReceiveMutex);
            Received = Decoded.packet.has_value() &&
                       Decoded.packet->header.session_nonce == ReconnectNonce &&
                       std::holds_alternative<KeyEventMessage>(
                           Decoded.packet->message);
        }
        ReceiveChanged.notify_all();
    });
    Header.session_nonce = ReconnectNonce;
    Header.sequence = 2;
    CHECK(SecondEndpoint->send_reliable(
        encode_packet(Header, KeyEventMessage{30, false, false})));
    {
        std::unique_lock Lock(ReceiveMutex);
        CHECK(ReceiveChanged.wait_for(
            Lock, std::chrono::seconds(5), [&] { return Received; }));
    }
    FirstEndpoint->close();
    SecondEndpoint->close();
    FirstEndpoint.reset();
    SecondEndpoint.reset();
    First->Close();
    Second->Close();
    First.reset();
    Second.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    auto FirstReloaded = Win32DeviceCertificate::Load(FirstKeyName, Crypto);
    auto SecondReloaded = Win32DeviceCertificate::Load(SecondKeyName, Crypto);
    CHECK(FirstReloaded.has_value());
    CHECK(SecondReloaded.has_value());
    const auto FirstAfter = FirstReloaded->IdentitySnapshot(Crypto);
    const auto SecondAfter = SecondReloaded->IdentitySnapshot(Crypto);
    CHECK(FirstAfter.has_value());
    CHECK(SecondAfter.has_value());
    CHECK(*FirstAfter == *FirstBefore);
    CHECK(*SecondAfter == *SecondBefore);
    std::cout << "[Identity:Invariance] "
              << (Backend == TlsBackend::OpenSsl ? "OpenSSL" : "Schannel")
              << " before/after CNG identity snapshots match.\n";
}

void CheckOpenSslCredentialRejected(const std::wstring& KeyName) {
    using namespace desklink;
    BCryptPairingCrypto Crypto;
    auto Certificate = Win32DeviceCertificate::LoadOrCreate(KeyName, Crypto);
    // Credential invariants are enforced centrally before either Schannel or
    // OpenSSL can receive the certificate/key pair.
    CHECK(!Certificate.has_value());
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

    const auto SigningFailureKeyName =
        std::wstring(L"DeskLink-Signing-Failure-Rejected-") + Suffix;
    {
        TestIdentityCleanup Cleanup{{SigningFailureKeyName}};
        desklink::Win32DeviceCertificate::Remove(SigningFailureKeyName);
        CHECK(InstallTestIdentity(
            SigningFailureKeyName, SigningFailureKeyName, false,
            CertificateValidity::Current, nullptr, 512));
        CHECK(CngPssSigningFails(SigningFailureKeyName));
        CheckOpenSslCredentialRejected(SigningFailureKeyName);
    }
}

void CheckNoApplicationAdmission(Results& Shared) {
    std::scoped_lock Lock(Shared.Mutex);
    CHECK(Shared.PairingSessions.empty());
    CHECK(Shared.TrustedSessions.empty());
}

void RunCertificateDerRejectionTests(const std::wstring& Suffix) {
    using namespace desklink;
    CHECK(InspectMsQuicPeerCertificateDer({}) ==
          MsQuicPeerCertificateStatus::Missing);
    const ByteBuffer MalformedDer{0x30u, 0x03u, 0x01u, 0x01u, 0xFFu};
    CHECK(InspectMsQuicPeerCertificateDer(MalformedDer) ==
          MsQuicPeerCertificateStatus::Malformed);

    const auto ExpiredKeyName =
        std::wstring(L"DeskLink-Expired-Certificate-") + Suffix;
    const auto FutureKeyName =
        std::wstring(L"DeskLink-Future-Certificate-") + Suffix;
    TestIdentityCleanup Cleanup{{FutureKeyName, ExpiredKeyName}};
    Win32DeviceCertificate::Remove(ExpiredKeyName);
    Win32DeviceCertificate::Remove(FutureKeyName);
    ByteBuffer ExpiredDer;
    ByteBuffer FutureDer;
    CHECK(InstallTestIdentity(
        ExpiredKeyName, ExpiredKeyName, false,
        CertificateValidity::Expired, &ExpiredDer));
    CHECK(InstallTestIdentity(
        FutureKeyName, FutureKeyName, false,
        CertificateValidity::NotYetValid, &FutureDer));
    CHECK(InspectMsQuicPeerCertificateDer(ExpiredDer) ==
          MsQuicPeerCertificateStatus::Expired);
    CHECK(InspectMsQuicPeerCertificateDer(FutureDer) ==
          MsQuicPeerCertificateStatus::NotYetValid);
}

void RunTrustedValidationRejection(
    const std::wstring& FirstKeyName,
    const std::wstring& SecondKeyName,
    desklink::TlsBackend Backend,
    bool SaveExpectedPeer,
    std::optional<std::string> ExpectedFingerprint,
    CryptoFault Fault) {
    using namespace desklink;
    BCryptPairingCrypto FirstCrypto;
    FaultingPairingCrypto SecondCrypto;
    SteadyClock Clock;
    auto FirstCertificate = Win32DeviceCertificate::LoadOrCreate(
        FirstKeyName, FirstCrypto);
    auto SecondCertificate = Win32DeviceCertificate::LoadOrCreate(
        SecondKeyName, SecondCrypto);
    CHECK(FirstCertificate.has_value());
    CHECK(SecondCertificate.has_value());

    PeerIdentity FirstIdentity{
        DeriveMachineId(FirstCertificate->CertificatePin()), "Rejected server",
        FormatFingerprint(FirstCertificate->CertificatePin())};
    const PeerIdentity SecondIdentity{
        DeriveMachineId(SecondCertificate->CertificatePin()), "Rejected client",
        FormatFingerprint(SecondCertificate->CertificatePin())};
    InMemoryTrustStore FirstTrust;
    InMemoryTrustStore SecondTrust;
    MachineId ExpectedFirstMachine = FirstIdentity.machine_id;
    CHECK(FirstTrust.SavePeer(TrustedPeer{SecondIdentity, CapabilitySet{}}));
    if (SaveExpectedPeer) {
        auto ExpectedIdentity = FirstIdentity;
        if (ExpectedFingerprint) {
            ExpectedIdentity.public_key_fingerprint = *ExpectedFingerprint;
            const auto Parsed = ParseFingerprint(*ExpectedFingerprint);
            CHECK(Parsed.has_value());
            ExpectedIdentity.machine_id = DeriveMachineId(*Parsed);
            ExpectedFirstMachine = ExpectedIdentity.machine_id;
        }
        CHECK(SecondTrust.SavePeer(
            TrustedPeer{std::move(ExpectedIdentity), CapabilitySet{}}));
    }

    PairingCoordinator FirstPairing(
        FirstIdentity, FirstCertificate->CertificatePin(), Clock,
        FirstCrypto, FirstTrust);
    PairingCoordinator SecondPairing(
        SecondIdentity, SecondCertificate->CertificatePin(), Clock,
        SecondCrypto, SecondTrust);
    Results Shared;
    MsQuicRuntimeConfig RuntimeConfig;
    RuntimeConfig.Backend = Backend;
    auto First = MsQuicBootstrap::Create(
        std::move(*FirstCertificate), FirstTrust, FirstCrypto, FirstPairing,
        Clock, RuntimeConfig, MakeHandlers(Shared));
    auto Second = MsQuicBootstrap::Create(
        std::move(*SecondCertificate), SecondTrust, SecondCrypto, SecondPairing,
        Clock, RuntimeConfig, MakeHandlers(Shared));
    CHECK(First);
    CHECK(Second);
    CHECK(First->StartListener());
    CHECK(First->BoundPort() != 0);

    SecondCrypto.SetFault(Fault);
    CHECK(Second->ConnectTrusted(
        "127.0.0.1", First->BoundPort(), ExpectedFirstMachine));
    const auto Timeout = Fault == CryptoFault::Stall
        ? std::chrono::seconds(7)
        : std::chrono::seconds(5);
    CHECK(WaitForFailure(Shared, Timeout));
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    CheckNoApplicationAdmission(Shared);

    SecondCrypto.ReleaseStall();
    First->Close();
    Second->Close();
    First.reset();
    Second.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
}

void RunOpenSslPeerValidationRejectionTests(const std::wstring& Suffix) {
    using namespace desklink;
    const auto MakeName = [&](std::wstring_view Label) {
        return std::wstring(L"DeskLink-") + std::wstring(Label) + L"-" + Suffix;
    };

    {
        const auto FirstKey = MakeName(L"Wrong-Pin-A");
        const auto SecondKey = MakeName(L"Wrong-Pin-B");
        TestIdentityCleanup Cleanup{{SecondKey, FirstKey}};
        BCryptPairingCrypto Crypto;
        auto Certificate = Win32DeviceCertificate::LoadOrCreate(FirstKey, Crypto);
        CHECK(Certificate.has_value());
        auto WrongPin = Certificate->CertificatePin();
        WrongPin[0] ^= 0x5Au;
        Certificate.reset();
        RunTrustedValidationRejection(
            FirstKey, SecondKey, TlsBackend::OpenSsl, true,
            FormatFingerprint(WrongPin), CryptoFault::None);
    }

    {
        const auto FirstKey = MakeName(L"Unknown-A");
        const auto SecondKey = MakeName(L"Unknown-B");
        TestIdentityCleanup Cleanup{{SecondKey, FirstKey}};
        RunTrustedValidationRejection(
            FirstKey, SecondKey, TlsBackend::OpenSsl, false,
            std::nullopt, CryptoFault::None);
    }

    for (const auto [Label, Fault] : {
             std::pair{std::wstring_view(L"Validation-Failure"), CryptoFault::Failure},
             std::pair{std::wstring_view(L"Validation-Exception"), CryptoFault::Exception},
             std::pair{std::wstring_view(L"Validation-Timeout"), CryptoFault::Stall}}) {
        const auto FirstKey = MakeName(std::wstring(Label) + L"-A");
        const auto SecondKey = MakeName(std::wstring(Label) + L"-B");
        TestIdentityCleanup Cleanup{{SecondKey, FirstKey}};
        RunTrustedValidationRejection(
            FirstKey, SecondKey, TlsBackend::OpenSsl, true,
            std::nullopt, Fault);
    }

    {
        const auto OldKey = MakeName(L"Changed-Identity-Old");
        const auto FirstKey = MakeName(L"Changed-Identity-New");
        const auto SecondKey = MakeName(L"Changed-Identity-Client");
        TestIdentityCleanup Cleanup{{SecondKey, FirstKey, OldKey}};
        BCryptPairingCrypto Crypto;
        auto OldCertificate = Win32DeviceCertificate::LoadOrCreate(OldKey, Crypto);
        CHECK(OldCertificate.has_value());
        const auto OldFingerprint =
            FormatFingerprint(OldCertificate->CertificatePin());
        OldCertificate.reset();
        RunTrustedValidationRejection(
            FirstKey, SecondKey, TlsBackend::OpenSsl, true,
            OldFingerprint, CryptoFault::None);
    }
}

} // namespace

int main(int ArgumentCount, char** Arguments) {
    const auto Backend = ArgumentCount == 2 &&
                         std::string_view(Arguments[1]) == "--openssl"
        ? desklink::TlsBackend::OpenSsl
        : desklink::TlsBackend::Schannel;
    const auto Suffix = std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(
            std::chrono::steady_clock::now().time_since_epoch().count());
    const auto FirstKeyName = std::wstring(L"DeskLink-Loopback-A-") + Suffix;
    const auto SecondKeyName = std::wstring(L"DeskLink-Loopback-B-") + Suffix;
    desklink::Win32DeviceCertificate::Remove(FirstKeyName);
    desklink::Win32DeviceCertificate::Remove(SecondKeyName);
    try {
        RunLoopback(FirstKeyName, SecondKeyName, Backend);
        if (Backend == desklink::TlsBackend::OpenSsl) {
            RunOpenSslCredentialRejectionTests(Suffix);
            RunCertificateDerRejectionTests(Suffix);
            RunOpenSslPeerValidationRejectionTests(Suffix);
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
