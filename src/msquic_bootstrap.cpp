#include "desklink/msquic_bootstrap.hpp"

#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

namespace desklink {
namespace {

constexpr QUIC_UINT62 kBootstrapError = 0x444C1001u;
constexpr QUIC_UINT62 kPairingRejectedError = 0x444C1002u;
constexpr std::size_t kMaximumCertificateSize = 16u * 1024u;
constexpr std::size_t kMaximumServerNameSize = 253;
constexpr std::size_t kSessionPrefaceSize = 16;
constexpr std::size_t kMaximumSessionBuffered =
    kSessionPrefaceSize + 36u + kMaxReliablePayload;
constexpr std::array<std::uint8_t, 4> kSessionPrefaceMagic{'D', 'L', 'S', 'N'};

enum class ConnectionPurpose {
    Trusted,
    Pairing,
};

struct BootstrapConnectionState;
using ConnectionHolder = std::shared_ptr<BootstrapConnectionState>;

struct PairingSendContext {
    explicit PairingSendContext(ByteBuffer Frame) : Bytes(std::move(Frame)) {
        Buffer.Length = static_cast<std::uint32_t>(Bytes.size());
        Buffer.Buffer = Bytes.data();
    }

    ByteBuffer Bytes;
    QUIC_BUFFER Buffer{};
};

struct SessionSendContext {
    SessionSendContext(ByteBuffer Preface, std::uint64_t OwnedNonce)
        : Bytes(std::move(Preface)), Nonce(OwnedNonce) {
        Buffer.Length = static_cast<std::uint32_t>(Bytes.size());
        Buffer.Buffer = Bytes.data();
    }

    ByteBuffer Bytes;
    QUIC_BUFFER Buffer{};
    std::uint64_t Nonce{};
};

struct BootstrapConnectionState {
    std::shared_ptr<MsQuicBootstrap::State> Owner;
    std::shared_ptr<BootstrapConnectionState> SelfHold;
    HQUIC Connection{};
    HQUIC PairingStream{};
    HQUIC SessionStream{};
    ConnectionPurpose Purpose{ConnectionPurpose::Trusted};
    std::optional<MachineId> ExpectedMachine;
    std::optional<Sha256Digest> PresentedPin;
    std::optional<TransportPeerInfo> TrustedPeer;
    PairingFrameDecoder Decoder;
    ByteBuffer SessionBuffer;
    std::mutex Mutex;
    bool Outgoing{};
    bool ValidationStarted{};
    bool OfferSent{};
    bool OfferDelivered{};
    bool EndpointDelivered{};
    bool Closed{};
};

struct RuntimeCleanup {
    explicit RuntimeCleanup(Win32DeviceCertificate OwnedCertificate)
        : Certificate(std::move(OwnedCertificate)) {}

    void Run() noexcept {
        if (Listener) Api->ListenerClose(Listener);
        if (PairingClientConfiguration) Api->ConfigurationClose(PairingClientConfiguration);
        if (PairingServerConfiguration) Api->ConfigurationClose(PairingServerConfiguration);
        if (SessionClientConfiguration) Api->ConfigurationClose(SessionClientConfiguration);
        if (SessionServerConfiguration) Api->ConfigurationClose(SessionServerConfiguration);
        if (Registration) Api->RegistrationClose(Registration);
        if (Api) MsQuicClose(Api);
    }

    Win32DeviceCertificate Certificate;
    const QUIC_API_TABLE* Api{};
    HQUIC Registration{};
    HQUIC SessionServerConfiguration{};
    HQUIC SessionClientConfiguration{};
    HQUIC PairingServerConfiguration{};
    HQUIC PairingClientConfiguration{};
    HQUIC Listener{};
};

void CALLBACK RunRuntimeCleanup(PTP_CALLBACK_INSTANCE, void* Context) noexcept {
    std::unique_ptr<RuntimeCleanup> Cleanup(static_cast<RuntimeCleanup*>(Context));
    Cleanup->Run();
}

ConnectionHolder HoldConnection(void* Context) {
    auto* State = static_cast<BootstrapConnectionState*>(Context);
    std::scoped_lock Lock(State->Mutex);
    return State->SelfHold;
}

bool IsSamePin(const Sha256Digest& Left, const Sha256Digest& Right) noexcept {
    std::uint8_t Difference = 0;
    for (std::size_t Index = 0; Index < Left.size(); ++Index) {
        Difference = static_cast<std::uint8_t>(Difference | (Left[Index] ^ Right[Index]));
    }
    return Difference == 0;
}

bool IsValidServerName(std::string_view ServerName) noexcept {
    return !ServerName.empty() && ServerName.size() <= kMaximumServerNameSize &&
           ServerName.find('\0') == std::string_view::npos;
}

bool MatchesAlpn(const QUIC_NEW_CONNECTION_INFO& Info, std::string_view Expected) noexcept {
    return Info.NegotiatedAlpn && Info.NegotiatedAlpnLength == Expected.size() &&
           std::memcmp(Info.NegotiatedAlpn, Expected.data(), Expected.size()) == 0;
}

std::string AddressKey(const QUIC_ADDR* Address) {
    if (!Address) return {};
    char Text[INET6_ADDRSTRLEN]{};
    const auto* Base = reinterpret_cast<const sockaddr*>(Address);
    if (Base->sa_family == AF_INET) {
        const auto* V4 = reinterpret_cast<const sockaddr_in*>(Address);
        return InetNtopA(AF_INET, const_cast<IN_ADDR*>(&V4->sin_addr), Text,
                         static_cast<DWORD>(std::size(Text))) ? std::string(Text) : std::string{};
    }
    if (Base->sa_family == AF_INET6) {
        const auto* V6 = reinterpret_cast<const sockaddr_in6*>(Address);
        return InetNtopA(AF_INET6, const_cast<IN6_ADDR*>(&V6->sin6_addr), Text,
                         static_cast<DWORD>(std::size(Text))) ? std::string(Text) : std::string{};
    }
    return {};
}

void ReportFailure(const std::shared_ptr<MsQuicBootstrap::State>& State,
                   std::string Message);
void ShutdownConnection(const ConnectionHolder& State,
                        QUIC_UINT62 ErrorCode = kBootstrapError) noexcept;
QUIC_STATUS QUIC_API BootstrapConnectionCallback(
    HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event);
QUIC_STATUS QUIC_API PairingStreamCallback(
    HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event);
QUIC_STATUS QUIC_API SessionStreamCallback(
    HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event);

} // namespace

struct MsQuicBootstrap::State {
    State(Win32DeviceCertificate OwnedCertificate,
          ITrustStore& OwnedTrustStore,
          IPairingCrypto& OwnedCrypto,
          PairingCoordinator& OwnedPairing,
          IClock& OwnedClock,
          MsQuicBootstrapHandlers OwnedHandlers)
        : Certificate(std::move(OwnedCertificate)),
          TrustStore(OwnedTrustStore),
          Crypto(OwnedCrypto),
          Pairing(OwnedPairing),
          Clock(OwnedClock),
          Handlers(std::move(OwnedHandlers)),
          ConnectionLimiter(Clock, 20, std::chrono::seconds(10)),
          PairingLimiter(Clock, 4, std::chrono::minutes(1)) {}

    ~State() {
        auto Cleanup = std::make_unique<RuntimeCleanup>(std::move(Certificate));
        Cleanup->Api = std::exchange(Api, nullptr);
        Cleanup->Registration = std::exchange(Registration, nullptr);
        Cleanup->SessionServerConfiguration =
            std::exchange(SessionServerConfiguration, nullptr);
        Cleanup->SessionClientConfiguration =
            std::exchange(SessionClientConfiguration, nullptr);
        Cleanup->PairingServerConfiguration =
            std::exchange(PairingServerConfiguration, nullptr);
        Cleanup->PairingClientConfiguration =
            std::exchange(PairingClientConfiguration, nullptr);
        Cleanup->Listener = std::exchange(Listener, nullptr);
        if (TrySubmitThreadpoolCallback(RunRuntimeCleanup, Cleanup.get(), nullptr)) {
            (void)Cleanup.release();
        } else {
            // Fail closed on shutdown: leaking process-scoped handles is safer than
            // risking RegistrationClose on an MsQuic callback thread.
            (void)Cleanup.release();
        }
    }

    Win32DeviceCertificate Certificate;
    ITrustStore& TrustStore;
    IPairingCrypto& Crypto;
    PairingCoordinator& Pairing;
    IClock& Clock;
    MsQuicBootstrapHandlers Handlers;
    AttemptRateLimiter ConnectionLimiter;
    AttemptRateLimiter PairingLimiter;
    const QUIC_API_TABLE* Api{};
    HQUIC Registration{};
    HQUIC SessionServerConfiguration{};
    HQUIC SessionClientConfiguration{};
    HQUIC PairingServerConfiguration{};
    HQUIC PairingClientConfiguration{};
    HQUIC Listener{};
    std::shared_ptr<State> ListenerHold;
    mutable std::mutex Mutex;
    std::uint16_t Port{};
    bool Closed{};
};

struct MsQuicPairingSession::State {
    PairingOffer Offer;
    PairingCandidate InspectedCandidate;
    PairingCoordinator* Pairing{};
    std::weak_ptr<BootstrapConnectionState> Connection;
    std::mutex Mutex;
    bool Completed{};
};

namespace {

void ReportFailure(const std::shared_ptr<MsQuicBootstrap::State>& State,
                   std::string Message) {
    std::function<void(std::string)> Handler;
    {
        std::scoped_lock Lock(State->Mutex);
        Handler = State->Handlers.Failed;
    }
    if (Handler) {
        try {
            Handler(std::move(Message));
        } catch (...) {
        }
    }
}

void ShutdownConnection(const ConnectionHolder& State, QUIC_UINT62 ErrorCode) noexcept {
    HQUIC Connection{};
    {
        std::scoped_lock Lock(State->Mutex);
        if (State->Closed) return;
        State->Closed = true;
        Connection = State->Connection;
    }
    if (Connection) {
        State->Owner->Api->ConnectionShutdown(
            Connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, ErrorCode);
    }
}

QUIC_STATUS QUIC_API RejectedStreamCallback(
    HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event) {
    const auto* Api = static_cast<const QUIC_API_TABLE*>(Context);
    if (Event->Type == QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE) Api->StreamClose(Stream);
    return QUIC_STATUS_SUCCESS;
}

void RejectStream(const ConnectionHolder& State, HQUIC Stream) {
    State->Owner->Api->SetCallbackHandler(
        Stream, reinterpret_cast<void*>(RejectedStreamCallback),
        const_cast<QUIC_API_TABLE*>(State->Owner->Api));
    (void)State->Owner->Api->StreamShutdown(
        Stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, kBootstrapError);
}

bool SendPairingOffer(const ConnectionHolder& State) {
    HQUIC Stream{};
    {
        std::scoped_lock Lock(State->Mutex);
        if (State->Closed || State->OfferSent || !State->PairingStream) return false;
        Stream = State->PairingStream;
    }
    const auto Offer = State->Owner->Pairing.CreateOffer();
    const auto Frame = Offer ? EncodePairingOfferFrame(*Offer) : std::nullopt;
    if (!Frame) return false;
    auto* Context = new PairingSendContext(*Frame);
    if (QUIC_FAILED(State->Owner->Api->StreamSend(
            Stream, &Context->Buffer, 1, QUIC_SEND_FLAG_NONE, Context))) {
        delete Context;
        return false;
    }
    std::scoped_lock Lock(State->Mutex);
    State->OfferSent = true;
    return true;
}

bool OpenPairingStream(const ConnectionHolder& State) {
    HQUIC Connection{};
    {
        std::scoped_lock Lock(State->Mutex);
        if (State->Closed || State->PairingStream) return false;
        Connection = State->Connection;
    }
    HQUIC Stream{};
    if (QUIC_FAILED(State->Owner->Api->StreamOpen(
            Connection, QUIC_STREAM_OPEN_FLAG_NONE, PairingStreamCallback,
            State.get(), &Stream))) {
        return false;
    }
    {
        std::scoped_lock Lock(State->Mutex);
        if (State->Closed || State->PairingStream) {
            State->Owner->Api->StreamClose(Stream);
            return false;
        }
        State->PairingStream = Stream;
    }
    if (QUIC_FAILED(State->Owner->Api->StreamStart(
            Stream, QUIC_STREAM_START_FLAG_IMMEDIATE))) {
        std::scoped_lock Lock(State->Mutex);
        State->PairingStream = nullptr;
        State->Owner->Api->StreamClose(Stream);
        return false;
    }
    return SendPairingOffer(State);
}

void DeliverPairingOffer(const ConnectionHolder& State, PairingOffer Offer) {
    Sha256Digest PresentedPin{};
    bool MissingPin = false;
    {
        std::scoped_lock Lock(State->Mutex);
        if (State->Closed || State->OfferDelivered || !State->PresentedPin) {
            MissingPin = true;
        } else {
            PresentedPin = *State->PresentedPin;
        }
    }
    if (MissingPin) {
        ShutdownConnection(State);
        return;
    }
    if (!IsSamePin(PresentedPin, Offer.CertificatePin)) {
        ShutdownConnection(State, kPairingRejectedError);
        return;
    }
    const auto Candidate = State->Owner->Pairing.InspectOffer(Offer);
    if (Candidate.Status != PairingStatus::Ready) {
        ShutdownConnection(State, kPairingRejectedError);
        return;
    }

    auto SessionState = std::make_shared<MsQuicPairingSession::State>();
    SessionState->Offer = std::move(Offer);
    SessionState->InspectedCandidate = Candidate;
    SessionState->Pairing = &State->Owner->Pairing;
    SessionState->Connection = State;
    auto Session = std::make_shared<MsQuicPairingSession>(std::move(SessionState));
    std::function<void(std::shared_ptr<MsQuicPairingSession>)> Handler;
    {
        std::scoped_lock Lock(State->Mutex, State->Owner->Mutex);
        if (State->Closed || State->OfferDelivered) return;
        State->OfferDelivered = true;
        Handler = State->Owner->Handlers.PairingOffered;
    }
    if (!Handler) {
        ShutdownConnection(State, kPairingRejectedError);
        return;
    }
    try {
        Handler(std::move(Session));
    } catch (...) {
        ShutdownConnection(State, kPairingRejectedError);
    }
}

void HandlePairingBytes(const ConnectionHolder& State, ByteSpan Bytes) {
    PairingWireStatus Status = PairingWireStatus::InvalidFrame;
    std::optional<PairingOffer> Offer;
    {
        std::scoped_lock Lock(State->Mutex);
        if (State->Closed || State->OfferDelivered) {
            Status = PairingWireStatus::InvalidFrame;
        } else {
            Status = State->Decoder.Push(Bytes);
            if (Status == PairingWireStatus::Ready) Offer = State->Decoder.TakeOffer();
        }
    }
    if (Status == PairingWireStatus::InvalidFrame) {
        ShutdownConnection(State, kPairingRejectedError);
    } else if (Offer) {
        DeliverPairingOffer(State, std::move(*Offer));
    }
}

QUIC_STATUS QUIC_API PairingStreamCallback(
    HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event) {
    const auto State = HoldConnection(Context);
    if (!State) return QUIC_STATUS_SUCCESS;
    switch (Event->Type) {
    case QUIC_STREAM_EVENT_RECEIVE:
        if ((Event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_0_RTT) != 0) {
            ShutdownConnection(State, kPairingRejectedError);
            return QUIC_STATUS_SUCCESS;
        }
        for (std::uint32_t Index = 0; Index < Event->RECEIVE.BufferCount; ++Index) {
            const auto& Buffer = Event->RECEIVE.Buffers[Index];
            HandlePairingBytes(State, ByteSpan{Buffer.Buffer, Buffer.Length});
        }
        return QUIC_STATUS_SUCCESS;
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        delete static_cast<PairingSendContext*>(Event->SEND_COMPLETE.ClientContext);
        return QUIC_STATUS_SUCCESS;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        {
            std::scoped_lock Lock(State->Mutex);
            if (State->PairingStream == Stream) State->PairingStream = nullptr;
        }
        State->Owner->Api->StreamClose(Stream);
        return QUIC_STATUS_SUCCESS;
    default:
        return QUIC_STATUS_SUCCESS;
    }
}

void CompleteCertificateValidation(const ConnectionHolder& State,
                                   ByteBuffer CertificateDer,
                                   bool TimeValid) {
    bool Accepted = false;
    std::optional<Sha256Digest> PresentedPin;
    std::optional<TransportPeerInfo> TrustedPeer;
    if (TimeValid && !CertificateDer.empty()) {
        if (State->Purpose == ConnectionPurpose::Pairing) {
            PresentedPin = State->Owner->Crypto.HashSha256(CertificateDer);
            Accepted = PresentedPin.has_value() && State->Owner->Pairing.IsPairingOpen();
        } else {
            const MachineId* Expected = State->ExpectedMachine ? &*State->ExpectedMachine : nullptr;
            const auto Match = MatchPeerCertificate(
                State->Owner->TrustStore, State->Owner->Crypto, CertificateDer, Expected);
            if (Match) {
                TrustedPeer = TransportPeerInfo{Match->Identity, true, true};
                Accepted = true;
            }
        }
    }

    HQUIC Connection{};
    {
        std::scoped_lock Lock(State->Mutex);
        if (!State->Closed) {
            State->PresentedPin = PresentedPin;
            State->TrustedPeer = TrustedPeer;
            Connection = State->Connection;
        }
    }
    if (!Connection || QUIC_FAILED(State->Owner->Api->ConnectionCertificateValidationComplete(
            Connection, Accepted ? TRUE : FALSE,
            Accepted ? QUIC_TLS_ALERT_CODE_SUCCESS : QUIC_TLS_ALERT_CODE_BAD_CERTIFICATE))) {
        ShutdownConnection(State, kPairingRejectedError);
    }
}

void HandleCertificateEvent(const ConnectionHolder& State,
                            const QUIC_CONNECTION_EVENT& Event) {
    ByteBuffer CertificateDer;
    bool TimeValid = false;
    const auto* Certificate = reinterpret_cast<const CERT_CONTEXT*>(
        Event.PEER_CERTIFICATE_RECEIVED.Certificate);
    if (Certificate && Certificate->pbCertEncoded && Certificate->cbCertEncoded > 0 &&
        Certificate->cbCertEncoded <= kMaximumCertificateSize) {
        CertificateDer.assign(
            Certificate->pbCertEncoded,
            Certificate->pbCertEncoded + Certificate->cbCertEncoded);
        TimeValid = CertVerifyTimeValidity(nullptr, Certificate->pCertInfo) == 0;
    }
    try {
        std::thread([State, CertificateDer = std::move(CertificateDer), TimeValid]() mutable {
            CompleteCertificateValidation(State, std::move(CertificateDer), TimeValid);
        }).detach();
    } catch (...) {
        ShutdownConnection(State, kPairingRejectedError);
    }
}

std::uint64_t ReadSessionNonce(ByteSpan Bytes) noexcept {
    std::uint64_t Result = 0;
    for (std::size_t Index = 8; Index < kSessionPrefaceSize; ++Index) {
        Result = (Result << 8u) | Bytes[Index];
    }
    return Result;
}

bool IsValidSessionPreface(ByteSpan Bytes) noexcept {
    return Bytes.size() >= kSessionPrefaceSize &&
           std::equal(kSessionPrefaceMagic.begin(), kSessionPrefaceMagic.end(),
                      Bytes.begin()) &&
           Bytes[4] == 1u && Bytes[5] == 0u && Bytes[6] == 0u && Bytes[7] == 0u &&
           ReadSessionNonce(Bytes) != 0;
}

void DeliverTrustedEndpoint(const ConnectionHolder& State,
                            HQUIC Stream,
                            std::uint64_t SessionNonce,
                            ByteBuffer InitialReliableBytes) {
    HQUIC Connection{};
    bool Outgoing = false;
    TransportPeerInfo Peer;
    bool MissingPeer = false;
    {
        std::scoped_lock Lock(State->Mutex);
        if (State->Closed || State->EndpointDelivered || !State->Connection ||
            !State->TrustedPeer || !Stream || SessionNonce == 0) {
            MissingPeer = true;
        } else {
            State->EndpointDelivered = true;
            Connection = State->Connection;
            Outgoing = State->Outgoing;
            Peer = *State->TrustedPeer;
        }
    }
    if (MissingPeer) {
        ShutdownConnection(State);
        return;
    }
    auto Endpoint = MsQuicTransportEndpoint::Adopt(
        State->Owner->Api, Connection, Stream, std::move(Peer),
        std::move(InitialReliableBytes));
    if (!Endpoint) {
        ShutdownConnection(State);
        return;
    }
    {
        std::scoped_lock Lock(State->Mutex);
        State->Connection = nullptr;
        State->SessionStream = nullptr;
        State->Closed = true;
        State->SelfHold.reset();
    }

    std::function<void(MsQuicBootstrapHandlers::TrustedSession)> Handler;
    {
        std::scoped_lock Lock(State->Owner->Mutex);
        Handler = State->Owner->Handlers.Connected;
    }
    if (!Handler) {
        Endpoint->close();
        return;
    }
    try {
        Handler(MsQuicBootstrapHandlers::TrustedSession{
            std::move(Endpoint), SessionNonce, Outgoing});
    } catch (...) {
        // The moved endpoint is destroyed and closes the connection.
    }
}

bool OpenSessionStream(const ConnectionHolder& State) {
    std::array<std::uint8_t, 8> Random{};
    if (!State->Owner->Crypto.FillRandom(Random)) return false;
    std::uint64_t SessionNonce = 0;
    for (const auto Byte : Random) SessionNonce = (SessionNonce << 8u) | Byte;
    if (SessionNonce == 0) return false;

    HQUIC Connection{};
    {
        std::scoped_lock Lock(State->Mutex);
        if (State->Closed || State->SessionStream) return false;
        Connection = State->Connection;
    }

    HQUIC Stream{};
    if (QUIC_FAILED(State->Owner->Api->StreamOpen(
            Connection, QUIC_STREAM_OPEN_FLAG_NONE, SessionStreamCallback,
            State.get(), &Stream))) {
        return false;
    }
    {
        std::scoped_lock Lock(State->Mutex);
        if (State->Closed || State->SessionStream) {
            State->Owner->Api->StreamClose(Stream);
            return false;
        }
        State->SessionStream = Stream;
    }
    if (QUIC_FAILED(State->Owner->Api->StreamStart(
            Stream, QUIC_STREAM_START_FLAG_IMMEDIATE))) {
        std::scoped_lock Lock(State->Mutex);
        State->SessionStream = nullptr;
        State->Owner->Api->StreamClose(Stream);
        return false;
    }

    ByteBuffer Preface(kSessionPrefaceSize, 0);
    std::copy(kSessionPrefaceMagic.begin(), kSessionPrefaceMagic.end(),
              Preface.begin());
    Preface[4] = 1u;
    for (std::size_t Index = 0; Index < Random.size(); ++Index) {
        Preface[8 + Index] = Random[Index];
    }
    auto* Context = new SessionSendContext(std::move(Preface), SessionNonce);
    if (QUIC_FAILED(State->Owner->Api->StreamSend(
            Stream, &Context->Buffer, 1, QUIC_SEND_FLAG_NONE, Context))) {
        delete Context;
        return false;
    }
    return true;
}

void HandleSessionBytes(const ConnectionHolder& State, ByteSpan Bytes) {
    bool Deliver = false;
    bool Invalid = false;
    std::uint64_t SessionNonce = 0;
    HQUIC Stream{};
    ByteBuffer InitialReliableBytes;
    {
        std::scoped_lock Lock(State->Mutex);
        if (State->Closed || State->EndpointDelivered ||
            Bytes.size() > kMaximumSessionBuffered ||
            State->SessionBuffer.size() > kMaximumSessionBuffered - Bytes.size()) {
            Invalid = !State->Closed && !State->EndpointDelivered;
        } else {
            State->SessionBuffer.insert(
                State->SessionBuffer.end(), Bytes.begin(), Bytes.end());
            if (!State->Outgoing &&
                State->SessionBuffer.size() >= kSessionPrefaceSize) {
                if (IsValidSessionPreface(State->SessionBuffer)) {
                    SessionNonce = ReadSessionNonce(State->SessionBuffer);
                    Stream = State->SessionStream;
                    InitialReliableBytes.assign(
                        State->SessionBuffer.begin() +
                            static_cast<std::ptrdiff_t>(kSessionPrefaceSize),
                        State->SessionBuffer.end());
                    Deliver = true;
                } else {
                    Invalid = true;
                }
            }
        }
    }
    if (Deliver) {
        DeliverTrustedEndpoint(
            State, Stream, SessionNonce, std::move(InitialReliableBytes));
    } else if (Invalid) {
        ReportFailure(State->Owner, "invalid or oversized trusted-session preface");
        ShutdownConnection(State);
    }
}

QUIC_STATUS QUIC_API SessionStreamCallback(
    HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event) {
    const auto State = HoldConnection(Context);
    if (!State) return QUIC_STATUS_SUCCESS;
    switch (Event->Type) {
    case QUIC_STREAM_EVENT_RECEIVE:
        if ((Event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_0_RTT) != 0) {
            ShutdownConnection(State);
            return QUIC_STATUS_SUCCESS;
        }
        {
            ByteBuffer Bytes;
            std::size_t Total = 0;
            for (std::uint32_t Index = 0; Index < Event->RECEIVE.BufferCount; ++Index) {
                Total += Event->RECEIVE.Buffers[Index].Length;
            }
            if (Total > kMaximumSessionBuffered) {
                ShutdownConnection(State);
                return QUIC_STATUS_SUCCESS;
            }
            Bytes.reserve(Total);
            for (std::uint32_t Index = 0; Index < Event->RECEIVE.BufferCount; ++Index) {
                const auto& Buffer = Event->RECEIVE.Buffers[Index];
                Bytes.insert(Bytes.end(), Buffer.Buffer, Buffer.Buffer + Buffer.Length);
            }
            HandleSessionBytes(State, Bytes);
        }
        return QUIC_STATUS_SUCCESS;
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        if (State->Outgoing) {
            std::unique_ptr<SessionSendContext> Context(
                static_cast<SessionSendContext*>(Event->SEND_COMPLETE.ClientContext));
            if (!Context || Event->SEND_COMPLETE.Canceled) {
                ReportFailure(State->Owner, "session nonce preface send was canceled");
                ShutdownConnection(State);
                return QUIC_STATUS_SUCCESS;
            }
            ByteBuffer InitialReliableBytes;
            {
                std::scoped_lock Lock(State->Mutex);
                if (State->Closed) return QUIC_STATUS_SUCCESS;
                InitialReliableBytes = std::move(State->SessionBuffer);
            }
            DeliverTrustedEndpoint(
                State, Stream, Context->Nonce, std::move(InitialReliableBytes));
        }
        return QUIC_STATUS_SUCCESS;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        {
            std::scoped_lock Lock(State->Mutex);
            if (State->SessionStream == Stream) State->SessionStream = nullptr;
        }
        State->Owner->Api->StreamClose(Stream);
        return QUIC_STATUS_SUCCESS;
    default:
        return QUIC_STATUS_SUCCESS;
    }
}

QUIC_STATUS QUIC_API BootstrapConnectionCallback(
    HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event) {
    const auto State = HoldConnection(Context);
    if (!State) return QUIC_STATUS_SUCCESS;
    switch (Event->Type) {
    case QUIC_CONNECTION_EVENT_PEER_CERTIFICATE_RECEIVED:
        {
            std::scoped_lock Lock(State->Mutex);
            if (State->ValidationStarted) return QUIC_STATUS_BAD_CERTIFICATE;
            State->ValidationStarted = true;
        }
        HandleCertificateEvent(State, *Event);
        return QUIC_STATUS_PENDING;
    case QUIC_CONNECTION_EVENT_CONNECTED:
        if (State->Purpose == ConnectionPurpose::Trusted) {
            if (State->Outgoing && !OpenSessionStream(State)) {
                ReportFailure(State->Owner, "failed to negotiate a fresh session nonce");
                ShutdownConnection(State);
            }
        } else if (State->Outgoing && !OpenPairingStream(State)) {
            ShutdownConnection(State);
        }
        return QUIC_STATUS_SUCCESS;
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
        {
            bool Accept = false;
            {
                std::scoped_lock Lock(State->Mutex);
                if (!State->Closed && !State->Outgoing &&
                    (Event->PEER_STREAM_STARTED.Flags & QUIC_STREAM_OPEN_FLAG_0_RTT) == 0) {
                    if (State->Purpose == ConnectionPurpose::Pairing &&
                        !State->PairingStream) {
                        State->PairingStream = Event->PEER_STREAM_STARTED.Stream;
                        Accept = true;
                    } else if (State->Purpose == ConnectionPurpose::Trusted &&
                               !State->SessionStream) {
                        State->SessionStream = Event->PEER_STREAM_STARTED.Stream;
                        Accept = true;
                    }
                }
            }
            if (!Accept) {
                RejectStream(State, Event->PEER_STREAM_STARTED.Stream);
                return QUIC_STATUS_SUCCESS;
            }
            if (State->Purpose == ConnectionPurpose::Pairing) {
                State->Owner->Api->SetCallbackHandler(
                    Event->PEER_STREAM_STARTED.Stream,
                    reinterpret_cast<void*>(PairingStreamCallback), State.get());
                if (!SendPairingOffer(State)) ShutdownConnection(State);
            } else {
                State->Owner->Api->SetCallbackHandler(
                    Event->PEER_STREAM_STARTED.Stream,
                    reinterpret_cast<void*>(SessionStreamCallback), State.get());
            }
        }
        return QUIC_STATUS_SUCCESS;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        {
            bool ReportSessionFailure = false;
            {
                std::scoped_lock Lock(State->Mutex);
                ReportSessionFailure = State->Purpose == ConnectionPurpose::Trusted &&
                                       !State->EndpointDelivered;
                State->Closed = true;
            }
            if (ReportSessionFailure) {
                ReportFailure(State->Owner,
                              "trusted connection closed before nonce negotiation");
            }
        }
        return QUIC_STATUS_SUCCESS;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        {
            std::scoped_lock Lock(State->Mutex);
            State->Connection = nullptr;
        }
        State->Owner->Api->ConnectionClose(Connection);
        {
            std::scoped_lock Lock(State->Mutex);
            State->SelfHold.reset();
        }
        return QUIC_STATUS_SUCCESS;
    default:
        return QUIC_STATUS_SUCCESS;
    }
}

std::optional<ConnectionPurpose> GetPurpose(const QUIC_NEW_CONNECTION_INFO& Info) {
    if (MatchesAlpn(Info, kMsQuicSessionAlpn)) return ConnectionPurpose::Trusted;
    if (MatchesAlpn(Info, kMsQuicPairingAlpn)) return ConnectionPurpose::Pairing;
    return std::nullopt;
}

QUIC_STATUS QUIC_API ListenerCallback(
    HQUIC Listener, void* Context, QUIC_LISTENER_EVENT* Event) {
    auto* RawState = static_cast<MsQuicBootstrap::State*>(Context);
    std::shared_ptr<MsQuicBootstrap::State> State;
    {
        std::scoped_lock Lock(RawState->Mutex);
        State = RawState->ListenerHold;
    }
    if (!State) return QUIC_STATUS_ABORTED;
    if (Event->Type == QUIC_LISTENER_EVENT_STOP_COMPLETE) {
        State->Api->ListenerClose(Listener);
        std::scoped_lock Lock(State->Mutex);
        State->Listener = nullptr;
        State->Port = 0;
        State->ListenerHold.reset();
        return QUIC_STATUS_SUCCESS;
    }
    if (Event->Type != QUIC_LISTENER_EVENT_NEW_CONNECTION || !Event->NEW_CONNECTION.Info) {
        return QUIC_STATUS_SUCCESS;
    }

    {
        std::scoped_lock Lock(State->Mutex);
        if (State->Closed) return QUIC_STATUS_CONNECTION_REFUSED;
    }

    const auto Purpose = GetPurpose(*Event->NEW_CONNECTION.Info);
    const auto Key = AddressKey(Event->NEW_CONNECTION.Info->RemoteAddress);
    if (!Purpose || Key.empty() || !State->ConnectionLimiter.Allow(Key) ||
        (*Purpose == ConnectionPurpose::Pairing &&
         (!State->Pairing.IsPairingOpen() || !State->PairingLimiter.Allow(Key)))) {
        return QUIC_STATUS_CONNECTION_REFUSED;
    }

    auto ConnectionState = std::make_shared<BootstrapConnectionState>();
    ConnectionState->Owner = State;
    ConnectionState->SelfHold = ConnectionState;
    ConnectionState->Connection = Event->NEW_CONNECTION.Connection;
    ConnectionState->Purpose = *Purpose;
    State->Api->SetCallbackHandler(
        Event->NEW_CONNECTION.Connection,
        reinterpret_cast<void*>(BootstrapConnectionCallback), ConnectionState.get());
    const HQUIC Configuration = *Purpose == ConnectionPurpose::Pairing
        ? State->PairingServerConfiguration : State->SessionServerConfiguration;
    if (QUIC_FAILED(State->Api->ConnectionSetConfiguration(
            Event->NEW_CONNECTION.Connection, Configuration))) {
        State->Api->ConnectionClose(Event->NEW_CONNECTION.Connection);
        ConnectionState->Connection = nullptr;
        ConnectionState->SelfHold.reset();
        return QUIC_STATUS_CONNECTION_REFUSED;
    }
    return QUIC_STATUS_SUCCESS;
}

bool OpenConfiguration(MsQuicBootstrap::State& State,
                       std::string_view Alpn,
                       bool Client,
                       HQUIC& Configuration,
                       std::string& Failure) {
    QUIC_SETTINGS Settings{};
    Settings.HandshakeIdleTimeoutMs = 5'000;
    Settings.IsSet.HandshakeIdleTimeoutMs = TRUE;
    Settings.IdleTimeoutMs = 15'000;
    Settings.IsSet.IdleTimeoutMs = TRUE;
    Settings.DisconnectTimeoutMs = 3'000;
    Settings.IsSet.DisconnectTimeoutMs = TRUE;
    Settings.PeerBidiStreamCount = 1;
    Settings.IsSet.PeerBidiStreamCount = TRUE;
    Settings.DatagramReceiveEnabled = TRUE;
    Settings.IsSet.DatagramReceiveEnabled = TRUE;
    Settings.ServerResumptionLevel = QUIC_SERVER_NO_RESUME;
    Settings.IsSet.ServerResumptionLevel = TRUE;
    QUIC_BUFFER AlpnBuffer{
        static_cast<std::uint32_t>(Alpn.size()),
        reinterpret_cast<std::uint8_t*>(const_cast<char*>(Alpn.data()))};
    const auto OpenStatus = State.Api->ConfigurationOpen(
        State.Registration, &AlpnBuffer, 1, &Settings, sizeof(Settings),
        nullptr, &Configuration);
    if (QUIC_FAILED(OpenStatus)) {
        Failure = "ConfigurationOpen failed for " + std::string(Alpn) +
                  " with status " + std::to_string(OpenStatus);
        return false;
    }

    QUIC_CERTIFICATE_HASH CertificateHash{};
    DWORD CertificateHashSize = sizeof(CertificateHash.ShaHash);
    if (!CertGetCertificateContextProperty(
            State.Certificate.Context(), CERT_SHA1_HASH_PROP_ID,
            CertificateHash.ShaHash, &CertificateHashSize) ||
        CertificateHashSize != sizeof(CertificateHash.ShaHash)) {
        Failure = "Could not read the local certificate store thumbprint for " +
                  std::string(Alpn) + (Client ? " client" : " server");
        State.Api->ConfigurationClose(Configuration);
        Configuration = nullptr;
        return false;
    }

    QUIC_CREDENTIAL_CONFIG Credentials{};
    Credentials.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_HASH;
    Credentials.CertificateHash = &CertificateHash;
    Credentials.Flags = static_cast<QUIC_CREDENTIAL_FLAGS>(
        QUIC_CREDENTIAL_FLAG_INDICATE_CERTIFICATE_RECEIVED |
        QUIC_CREDENTIAL_FLAG_DEFER_CERTIFICATE_VALIDATION |
        (Client ? QUIC_CREDENTIAL_FLAG_CLIENT
                : QUIC_CREDENTIAL_FLAG_REQUIRE_CLIENT_AUTHENTICATION));
    const auto CredentialStatus = State.Api->ConfigurationLoadCredential(
        Configuration, &Credentials);
    if (QUIC_FAILED(CredentialStatus)) {
        Failure = "ConfigurationLoadCredential failed for " + std::string(Alpn) +
                  (Client ? " client" : " server") + " with status " +
                  std::to_string(CredentialStatus);
        State.Api->ConfigurationClose(Configuration);
        Configuration = nullptr;
        return false;
    }
    return true;
}

bool OpenRuntime(const std::shared_ptr<MsQuicBootstrap::State>& State) {
    std::string Failure;
    const auto OpenStatus = MsQuicOpenVersion(
        QUIC_API_VERSION_2, reinterpret_cast<const void**>(&State->Api));
    if (QUIC_FAILED(OpenStatus)) {
        ReportFailure(State, "MsQuicOpenVersion failed with status " +
                              std::to_string(OpenStatus));
        return false;
    }
    const QUIC_REGISTRATION_CONFIG RegistrationConfig{
        "DeskLink", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
    const auto RegistrationStatus = State->Api->RegistrationOpen(
        &RegistrationConfig, &State->Registration);
    if (QUIC_FAILED(RegistrationStatus)) {
        ReportFailure(State, "RegistrationOpen failed with status " +
                              std::to_string(RegistrationStatus));
        return false;
    }
    const bool Opened =
        OpenConfiguration(*State, kMsQuicSessionAlpn, false,
                          State->SessionServerConfiguration, Failure) &&
        OpenConfiguration(*State, kMsQuicSessionAlpn, true,
                          State->SessionClientConfiguration, Failure) &&
        OpenConfiguration(*State, kMsQuicPairingAlpn, false,
                          State->PairingServerConfiguration, Failure) &&
        OpenConfiguration(*State, kMsQuicPairingAlpn, true,
                          State->PairingClientConfiguration, Failure);
    if (!Opened) ReportFailure(State, std::move(Failure));
    return Opened;
}

bool Connect(const std::shared_ptr<MsQuicBootstrap::State>& State,
             std::string ServerName,
             std::uint16_t Port,
             ConnectionPurpose Purpose,
             std::optional<MachineId> ExpectedMachine) {
    if (!IsValidServerName(ServerName) || Port == 0) return false;
    {
        std::scoped_lock Lock(State->Mutex);
        if (State->Closed) return false;
    }
    if (!State->ConnectionLimiter.Allow("out:" + ServerName) ||
        (Purpose == ConnectionPurpose::Pairing &&
         (!State->Pairing.IsPairingOpen() ||
          !State->PairingLimiter.Allow("out:" + ServerName)))) {
        return false;
    }

    auto ConnectionState = std::make_shared<BootstrapConnectionState>();
    ConnectionState->Owner = State;
    ConnectionState->SelfHold = ConnectionState;
    ConnectionState->Purpose = Purpose;
    ConnectionState->ExpectedMachine = ExpectedMachine;
    ConnectionState->Outgoing = true;
    if (QUIC_FAILED(State->Api->ConnectionOpen(
            State->Registration, BootstrapConnectionCallback,
            ConnectionState.get(), &ConnectionState->Connection))) {
        ConnectionState->SelfHold.reset();
        return false;
    }
    const HQUIC Configuration = Purpose == ConnectionPurpose::Pairing
        ? State->PairingClientConfiguration : State->SessionClientConfiguration;
    if (QUIC_FAILED(State->Api->ConnectionStart(
            ConnectionState->Connection, Configuration,
            QUIC_ADDRESS_FAMILY_UNSPEC, ServerName.c_str(), Port))) {
        State->Api->ConnectionClose(ConnectionState->Connection);
        ConnectionState->Connection = nullptr;
        ConnectionState->SelfHold.reset();
        return false;
    }
    return true;
}

} // namespace

MsQuicPairingSession::MsQuicPairingSession(std::shared_ptr<State> SharedState)
    : State_(std::move(SharedState)) {}

MsQuicPairingSession::~MsQuicPairingSession() { Reject(); }

const PairingOffer& MsQuicPairingSession::RemoteOffer() const noexcept {
    return State_->Offer;
}

const PairingCandidate& MsQuicPairingSession::Candidate() const noexcept {
    return State_->InspectedCandidate;
}

bool MsQuicPairingSession::Confirm(std::string_view VerificationCode,
                                   CapabilitySet Capabilities) {
    std::shared_ptr<BootstrapConnectionState> Connection;
    bool Accepted = false;
    {
        std::scoped_lock Lock(State_->Mutex);
        if (State_->Completed) return false;
        State_->Completed = true;
        Connection = State_->Connection.lock();
        Accepted = State_->Pairing && State_->Pairing->ConfirmOffer(
            State_->Offer, VerificationCode, Capabilities);
    }
    if (Connection) {
        ShutdownConnection(
            Connection, Accepted ? kBootstrapError : kPairingRejectedError);
    }
    return Accepted;
}

void MsQuicPairingSession::Reject() noexcept {
    std::shared_ptr<BootstrapConnectionState> Connection;
    {
        std::scoped_lock Lock(State_->Mutex);
        if (State_->Completed) return;
        State_->Completed = true;
        Connection = State_->Connection.lock();
    }
    if (Connection) ShutdownConnection(Connection, kPairingRejectedError);
}

std::shared_ptr<MsQuicBootstrap> MsQuicBootstrap::Create(
    Win32DeviceCertificate Certificate,
    ITrustStore& TrustStore,
    IPairingCrypto& Crypto,
    PairingCoordinator& Pairing,
    IClock& Clock,
    MsQuicBootstrapHandlers Handlers) {
    auto SharedState = std::make_shared<State>(
        std::move(Certificate), TrustStore, Crypto, Pairing, Clock, std::move(Handlers));
    if (!OpenRuntime(SharedState)) return {};
    return std::shared_ptr<MsQuicBootstrap>(
        new MsQuicBootstrap(std::move(SharedState)));
}

MsQuicBootstrap::MsQuicBootstrap(std::shared_ptr<State> SharedState)
    : State_(std::move(SharedState)) {}

MsQuicBootstrap::~MsQuicBootstrap() { Close(); }

bool MsQuicBootstrap::StartListener(std::uint16_t Port) {
    {
        std::scoped_lock Lock(State_->Mutex);
        if (State_->Closed || State_->Listener) return false;
    }
    HQUIC Listener{};
    if (QUIC_FAILED(State_->Api->ListenerOpen(
            State_->Registration, ListenerCallback, State_.get(), &Listener))) {
        return false;
    }
    {
        std::scoped_lock Lock(State_->Mutex);
        State_->Listener = Listener;
        State_->ListenerHold = State_;
    }

    std::array<QUIC_BUFFER, 2> Alpns{
        QUIC_BUFFER{static_cast<std::uint32_t>(kMsQuicSessionAlpn.size()),
                    reinterpret_cast<std::uint8_t*>(
                        const_cast<char*>(kMsQuicSessionAlpn.data()))},
        QUIC_BUFFER{static_cast<std::uint32_t>(kMsQuicPairingAlpn.size()),
                    reinterpret_cast<std::uint8_t*>(
                        const_cast<char*>(kMsQuicPairingAlpn.data()))}};
    QUIC_ADDR Address{};
    auto* V4 = reinterpret_cast<sockaddr_in*>(&Address);
    V4->sin_family = AF_INET;
    V4->sin_addr.s_addr = htonl(INADDR_ANY);
    V4->sin_port = htons(Port);
    if (QUIC_FAILED(State_->Api->ListenerStart(
            Listener, Alpns.data(), static_cast<std::uint32_t>(Alpns.size()), &Address))) {
        State_->Api->ListenerClose(Listener);
        std::scoped_lock Lock(State_->Mutex);
        State_->Listener = nullptr;
        State_->ListenerHold.reset();
        return false;
    }

    QUIC_ADDR BoundAddress{};
    std::uint32_t Size = sizeof(BoundAddress);
    if (QUIC_FAILED(State_->Api->GetParam(
            Listener, QUIC_PARAM_LISTENER_LOCAL_ADDRESS, &Size, &BoundAddress))) {
        Close();
        return false;
    }
    const auto* BoundV4 = reinterpret_cast<const sockaddr_in*>(&BoundAddress);
    std::scoped_lock Lock(State_->Mutex);
    State_->Port = ntohs(BoundV4->sin_port);
    return State_->Port != 0;
}

std::uint16_t MsQuicBootstrap::BoundPort() const noexcept {
    std::scoped_lock Lock(State_->Mutex);
    return State_->Port;
}

bool MsQuicBootstrap::ConnectTrusted(
    std::string ServerName,
    std::uint16_t Port,
    std::optional<MachineId> ExpectedMachine) {
    return Connect(State_, std::move(ServerName), Port,
                   ConnectionPurpose::Trusted, ExpectedMachine);
}

bool MsQuicBootstrap::ConnectForPairing(std::string ServerName,
                                        std::uint16_t Port) {
    return Connect(State_, std::move(ServerName), Port,
                   ConnectionPurpose::Pairing, std::nullopt);
}

void MsQuicBootstrap::Close() noexcept {
    HQUIC Listener{};
    HQUIC Registration{};
    {
        std::scoped_lock Lock(State_->Mutex);
        if (State_->Closed) return;
        State_->Closed = true;
        Listener = State_->Listener;
        Registration = State_->Registration;
    }
    if (Listener) State_->Api->ListenerStop(Listener);
    if (Registration) {
        State_->Api->RegistrationShutdown(
            Registration, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, kBootstrapError);
    }
}

} // namespace desklink
