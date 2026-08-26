#include "desklink/msquic_transport.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

namespace desklink {
namespace {

constexpr QUIC_UINT62 kProtocolError = 0x444C0001u;
constexpr std::size_t kMaximumPreHandlerBuffered = 1024u * 1024u;

struct SendContext {
    explicit SendContext(ByteBuffer Packet) : Bytes(std::move(Packet)) {
        Buffer.Length = static_cast<std::uint32_t>(Bytes.size());
        Buffer.Buffer = Bytes.data();
    }

    ByteBuffer Bytes;
    QUIC_BUFFER Buffer{};
};

std::uint32_t ReadPayloadSize(ByteSpan Bytes) noexcept {
    return (static_cast<std::uint32_t>(Bytes[8]) << 24u) |
           (static_cast<std::uint32_t>(Bytes[9]) << 16u) |
           (static_cast<std::uint32_t>(Bytes[10]) << 8u) |
           static_cast<std::uint32_t>(Bytes[11]);
}

} // namespace

struct MsQuicTransportEndpoint::State {
    const QUIC_API_TABLE* Api{};
    HQUIC Connection{};
    HQUIC ReliableStream{};
    TransportPeerInfo Peer;
    ReceiveHandler ReliableHandler;
    ReceiveHandler DatagramHandler;
    ByteBuffer ReliableBuffer;
    std::shared_ptr<State> SelfHold;
    std::mutex Mutex;
    bool PeerValidated{};
    bool DatagramEnabled{};
    std::uint16_t MaximumDatagramLength{};
    bool Closed{};
};

namespace {

using StateHolder = std::shared_ptr<MsQuicTransportEndpoint::State>;

StateHolder HoldState(void* Context) {
    auto* State = static_cast<MsQuicTransportEndpoint::State*>(Context);
    std::scoped_lock Lock(State->Mutex);
    return State->SelfHold;
}

void ShutdownConnection(const StateHolder& SharedState) noexcept {
    HQUIC Connection{};
    {
        std::scoped_lock Lock(SharedState->Mutex);
        if (SharedState->Closed) return;
        SharedState->Closed = true;
        SharedState->ReliableHandler = {};
        SharedState->DatagramHandler = {};
        Connection = SharedState->Connection;
    }
    if (Connection) {
        SharedState->Api->ConnectionShutdown(
            Connection, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, kProtocolError);
    }
}

void HandleReliableBytes(const StateHolder& SharedState, ByteSpan Bytes) {
    std::size_t Offset = 0;
    for (;;) {
        ByteBuffer Packet;
        ITransportEndpoint::ReceiveHandler Handler;
        bool Invalid = false;
        {
            std::scoped_lock Lock(SharedState->Mutex);
            if (SharedState->Closed) return;
            if (!SharedState->PeerValidated) {
                Invalid = true;
            } else if (!SharedState->ReliableHandler) {
                const auto Remaining = Bytes.size() - Offset;
                if (Remaining > kMaximumPreHandlerBuffered ||
                    SharedState->ReliableBuffer.size() >
                        kMaximumPreHandlerBuffered - Remaining) {
                    Invalid = true;
                } else {
                    SharedState->ReliableBuffer.insert(
                        SharedState->ReliableBuffer.end(),
                        Bytes.begin() + static_cast<std::ptrdiff_t>(Offset),
                        Bytes.end());
                    return;
                }
            }
            if (!Invalid) {
                if (SharedState->ReliableBuffer.size() < 12 && Offset < Bytes.size()) {
                    const auto Needed = 12 - SharedState->ReliableBuffer.size();
                    const auto Count = std::min(Needed, Bytes.size() - Offset);
                    SharedState->ReliableBuffer.insert(
                        SharedState->ReliableBuffer.end(),
                        Bytes.begin() + static_cast<std::ptrdiff_t>(Offset),
                        Bytes.begin() + static_cast<std::ptrdiff_t>(Offset + Count));
                    Offset += Count;
                }
                if (SharedState->ReliableBuffer.size() < 12) return;
                const auto PayloadSize = ReadPayloadSize(SharedState->ReliableBuffer);
                if (PayloadSize > kMaxReliablePayload) {
                    Invalid = true;
                } else {
                    const auto PacketSize =
                        kEnvelopeSize + static_cast<std::size_t>(PayloadSize);
                    if (SharedState->ReliableBuffer.size() < PacketSize &&
                        Offset < Bytes.size()) {
                        const auto Needed =
                            PacketSize - SharedState->ReliableBuffer.size();
                        const auto Count = std::min(Needed, Bytes.size() - Offset);
                        SharedState->ReliableBuffer.insert(
                            SharedState->ReliableBuffer.end(),
                            Bytes.begin() + static_cast<std::ptrdiff_t>(Offset),
                            Bytes.begin() + static_cast<std::ptrdiff_t>(Offset + Count));
                        Offset += Count;
                    }
                    if (SharedState->ReliableBuffer.size() < PacketSize) return;
                    Packet.assign(
                        SharedState->ReliableBuffer.begin(),
                        SharedState->ReliableBuffer.begin() +
                            static_cast<std::ptrdiff_t>(PacketSize));
                    SharedState->ReliableBuffer.erase(
                        SharedState->ReliableBuffer.begin(),
                        SharedState->ReliableBuffer.begin() +
                            static_cast<std::ptrdiff_t>(PacketSize));
                }
            }
            Handler = SharedState->ReliableHandler;
        }
        if (Invalid || !Handler) {
            ShutdownConnection(SharedState);
            return;
        }
        try {
            Handler(std::move(Packet));
        } catch (...) {
            ShutdownConnection(SharedState);
            return;
        }
        if (Offset == Bytes.size()) {
            std::scoped_lock Lock(SharedState->Mutex);
            if (SharedState->ReliableBuffer.empty()) return;
        }
    }
}

QUIC_STATUS QUIC_API RejectStreamCallback(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event) {
    const auto* Api = static_cast<const QUIC_API_TABLE*>(Context);
    if (Event->Type == QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE) Api->StreamClose(Stream);
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API StreamCallback(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event) {
    try {
        const auto SharedState = HoldState(Context);
        if (!SharedState) return QUIC_STATUS_SUCCESS;
        switch (Event->Type) {
    case QUIC_STREAM_EVENT_RECEIVE:
        {
            bool PeerValidated = false;
            {
                std::scoped_lock Lock(SharedState->Mutex);
                PeerValidated = SharedState->PeerValidated;
            }
            if (!PeerValidated) {
                ShutdownConnection(SharedState);
                return QUIC_STATUS_SUCCESS;
            }
        }
        if ((Event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_0_RTT) != 0) {
            ShutdownConnection(SharedState);
            return QUIC_STATUS_SUCCESS;
        }
        for (std::uint32_t Index = 0; Index < Event->RECEIVE.BufferCount; ++Index) {
            const auto& Buffer = Event->RECEIVE.Buffers[Index];
            HandleReliableBytes(SharedState, ByteSpan{Buffer.Buffer, Buffer.Length});
        }
        return QUIC_STATUS_SUCCESS;
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        delete static_cast<SendContext*>(Event->SEND_COMPLETE.ClientContext);
        return QUIC_STATUS_SUCCESS;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        {
            std::scoped_lock Lock(SharedState->Mutex);
            if (SharedState->ReliableStream == Stream) SharedState->ReliableStream = nullptr;
        }
        SharedState->Api->StreamClose(Stream);
        return QUIC_STATUS_SUCCESS;
    default:
        return QUIC_STATUS_SUCCESS;
        }
    } catch (...) {
        try {
            if (const auto SharedState = HoldState(Context)) {
                ShutdownConnection(SharedState);
            }
        } catch (...) {
        }
        return QUIC_STATUS_INTERNAL_ERROR;
    }
}

QUIC_STATUS QUIC_API ConnectionCallback(HQUIC Connection,
                                        void* Context,
                                        QUIC_CONNECTION_EVENT* Event) {
    try {
        const auto SharedState = HoldState(Context);
        if (!SharedState) return QUIC_STATUS_SUCCESS;
        switch (Event->Type) {
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
        {
            bool Accept = false;
            {
                std::scoped_lock Lock(SharedState->Mutex);
                if (!SharedState->Closed && SharedState->PeerValidated &&
                    !SharedState->ReliableStream &&
                    (Event->PEER_STREAM_STARTED.Flags & QUIC_STREAM_OPEN_FLAG_0_RTT) == 0) {
                    SharedState->ReliableStream = Event->PEER_STREAM_STARTED.Stream;
                    Accept = true;
                }
            }
            if (Accept) {
                SharedState->Api->SetCallbackHandler(
                    Event->PEER_STREAM_STARTED.Stream,
                    reinterpret_cast<void*>(StreamCallback), SharedState.get());
            } else {
                SharedState->Api->SetCallbackHandler(
                    Event->PEER_STREAM_STARTED.Stream,
                    reinterpret_cast<void*>(RejectStreamCallback),
                    const_cast<QUIC_API_TABLE*>(SharedState->Api));
                (void)SharedState->Api->StreamShutdown(
                    Event->PEER_STREAM_STARTED.Stream,
                    QUIC_STREAM_SHUTDOWN_FLAG_ABORT, kProtocolError);
                ShutdownConnection(SharedState);
            }
        }
        return QUIC_STATUS_SUCCESS;
    case QUIC_CONNECTION_EVENT_DATAGRAM_STATE_CHANGED:
        {
            std::scoped_lock Lock(SharedState->Mutex);
            SharedState->DatagramEnabled = SharedState->PeerValidated &&
                                           Event->DATAGRAM_STATE_CHANGED.SendEnabled != FALSE;
            SharedState->MaximumDatagramLength = Event->DATAGRAM_STATE_CHANGED.MaxSendLength;
        }
        return QUIC_STATUS_SUCCESS;
    case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED:
        {
            if ((Event->DATAGRAM_RECEIVED.Flags & QUIC_RECEIVE_FLAG_0_RTT) != 0) {
                ShutdownConnection(SharedState);
                return QUIC_STATUS_SUCCESS;
            }
            if (Event->DATAGRAM_RECEIVED.Buffer->Length > kMaxEncodedDatagramSize) {
                return QUIC_STATUS_SUCCESS;
            }
            ITransportEndpoint::ReceiveHandler Handler;
            bool PeerValidated = false;
            {
                std::scoped_lock Lock(SharedState->Mutex);
                if (SharedState->Closed) return QUIC_STATUS_SUCCESS;
                PeerValidated = SharedState->PeerValidated;
                if (PeerValidated) Handler = SharedState->DatagramHandler;
            }
            if (!PeerValidated) {
                ShutdownConnection(SharedState);
                return QUIC_STATUS_SUCCESS;
            }
            if (Handler) {
                const auto& Buffer = *Event->DATAGRAM_RECEIVED.Buffer;
                try {
                    Handler(ByteBuffer(Buffer.Buffer, Buffer.Buffer + Buffer.Length));
                } catch (...) {
                    ShutdownConnection(SharedState);
                }
            }
        }
        return QUIC_STATUS_SUCCESS;
    case QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED:
        if (QUIC_DATAGRAM_SEND_STATE_IS_FINAL(Event->DATAGRAM_SEND_STATE_CHANGED.State)) {
            delete static_cast<SendContext*>(Event->DATAGRAM_SEND_STATE_CHANGED.ClientContext);
        }
        return QUIC_STATUS_SUCCESS;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        {
            std::scoped_lock Lock(SharedState->Mutex);
            SharedState->Closed = true;
            SharedState->ReliableHandler = {};
            SharedState->DatagramHandler = {};
        }
        return QUIC_STATUS_SUCCESS;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        {
            std::scoped_lock Lock(SharedState->Mutex);
            SharedState->Connection = nullptr;
        }
        SharedState->Api->ConnectionClose(Connection);
        {
            std::scoped_lock Lock(SharedState->Mutex);
            SharedState->SelfHold.reset();
        }
        return QUIC_STATUS_SUCCESS;
    default:
        return QUIC_STATUS_SUCCESS;
        }
    } catch (...) {
        try {
            if (const auto SharedState = HoldState(Context)) {
                ShutdownConnection(SharedState);
            }
        } catch (...) {
        }
        return QUIC_STATUS_INTERNAL_ERROR;
    }
}

} // namespace

std::shared_ptr<MsQuicTransportEndpoint> MsQuicTransportEndpoint::Adopt(
    const QUIC_API_TABLE* Api,
    HQUIC Connection,
    HQUIC ReliableStream,
    TransportPeerInfo Peer,
    MsQuicPeerValidation PeerValidation,
    ByteBuffer InitialReliableBytes) {
    if (!Api || !Connection ||
        PeerValidation != MsQuicPeerValidation::PeerValidated ||
        !Peer.authenticated || !Peer.encrypted ||
        !ParseFingerprint(Peer.identity.public_key_fingerprint) ||
        InitialReliableBytes.size() > kMaximumPreHandlerBuffered) {
        return {};
    }

    auto SharedState = std::make_shared<State>();
    SharedState->Api = Api;
    SharedState->Connection = Connection;
    SharedState->ReliableStream = ReliableStream;
    SharedState->Peer = std::move(Peer);
    SharedState->PeerValidated = true;
    SharedState->ReliableBuffer = std::move(InitialReliableBytes);
    SharedState->SelfHold = SharedState;
    Api->SetCallbackHandler(
        Connection, reinterpret_cast<void*>(ConnectionCallback), SharedState.get());
    if (ReliableStream) {
        Api->SetCallbackHandler(
            ReliableStream, reinterpret_cast<void*>(StreamCallback), SharedState.get());
    }
    return std::shared_ptr<MsQuicTransportEndpoint>(
        new MsQuicTransportEndpoint(std::move(SharedState)));
}

MsQuicTransportEndpoint::MsQuicTransportEndpoint(std::shared_ptr<State> SharedState)
    : State_(std::move(SharedState)) {}

MsQuicTransportEndpoint::~MsQuicTransportEndpoint() { close(); }

bool MsQuicTransportEndpoint::OpenReliableStream() {
    HQUIC Connection{};
    {
        std::scoped_lock Lock(State_->Mutex);
        if (State_->Closed || !State_->PeerValidated) return false;
        if (State_->ReliableStream) return true;
        Connection = State_->Connection;
    }
    if (!Connection) return false;

    HQUIC Stream{};
    if (QUIC_FAILED(State_->Api->StreamOpen(
            Connection, QUIC_STREAM_OPEN_FLAG_NONE, StreamCallback, State_.get(), &Stream))) {
        return false;
    }
    {
        std::scoped_lock Lock(State_->Mutex);
        if (State_->Closed || State_->ReliableStream) {
            State_->Api->StreamClose(Stream);
            return false;
        }
        State_->ReliableStream = Stream;
    }
    if (QUIC_FAILED(State_->Api->StreamStart(Stream, QUIC_STREAM_START_FLAG_IMMEDIATE))) {
        std::scoped_lock Lock(State_->Mutex);
        State_->ReliableStream = nullptr;
        State_->Api->StreamClose(Stream);
        return false;
    }
    return true;
}

bool MsQuicTransportEndpoint::send_reliable(ByteBuffer Packet) {
    if (Packet.empty() || Packet.size() > kEnvelopeSize + kMaxReliablePayload ||
        Packet.size() > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    HQUIC Stream{};
    {
        std::scoped_lock Lock(State_->Mutex);
        if (State_->Closed || !State_->PeerValidated) return false;
        Stream = State_->ReliableStream;
    }
    if (!Stream) return false;
    auto* Context = new SendContext(std::move(Packet));
    if (QUIC_FAILED(State_->Api->StreamSend(
            Stream, &Context->Buffer, 1, QUIC_SEND_FLAG_NONE, Context))) {
        delete Context;
        return false;
    }
    return true;
}

bool MsQuicTransportEndpoint::send_datagram(ByteBuffer Packet) {
    if (Packet.empty() || Packet.size() > kMaxEncodedDatagramSize ||
        Packet.size() > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    HQUIC Connection{};
    {
        std::scoped_lock Lock(State_->Mutex);
        if (State_->Closed || !State_->PeerValidated || !State_->DatagramEnabled ||
            Packet.size() > State_->MaximumDatagramLength) {
            return false;
        }
        Connection = State_->Connection;
    }
    if (!Connection) return false;
    auto* Context = new SendContext(std::move(Packet));
    if (QUIC_FAILED(State_->Api->DatagramSend(
            Connection, &Context->Buffer, 1, QUIC_SEND_FLAG_NONE, Context))) {
        delete Context;
        return false;
    }
    return true;
}

void MsQuicTransportEndpoint::set_reliable_handler(ReceiveHandler Handler) {
    bool ProcessBuffered = false;
    {
        std::scoped_lock Lock(State_->Mutex);
        if (!State_->Closed && State_->PeerValidated) {
            State_->ReliableHandler = std::move(Handler);
            ProcessBuffered = State_->ReliableHandler && !State_->ReliableBuffer.empty();
        }
    }
    if (ProcessBuffered) HandleReliableBytes(State_, {});
}

void MsQuicTransportEndpoint::set_datagram_handler(ReceiveHandler Handler) {
    std::scoped_lock Lock(State_->Mutex);
    if (!State_->Closed && State_->PeerValidated) {
        State_->DatagramHandler = std::move(Handler);
    }
}

TransportPeerInfo MsQuicTransportEndpoint::peer_info() const {
    std::scoped_lock Lock(State_->Mutex);
    return State_->Peer;
}

void MsQuicTransportEndpoint::close() noexcept { ShutdownConnection(State_); }

std::optional<TransportPeerInfo> VerifyMsQuicPeerCertificate(
    const QUIC_CERTIFICATE* Certificate,
    const ITrustStore& TrustStore,
    const IPairingCrypto& Crypto,
    const MachineId* ExpectedMachine) {
    if (!Certificate) return std::nullopt;
    const auto* Context = reinterpret_cast<const CERT_CONTEXT*>(Certificate);
    if (!Context->pbCertEncoded || Context->cbCertEncoded == 0) return std::nullopt;
    const auto Match = MatchPeerCertificate(
        TrustStore, Crypto, ByteSpan{Context->pbCertEncoded, Context->cbCertEncoded}, ExpectedMachine);
    if (!Match) return std::nullopt;
    return TransportPeerInfo{Match->Identity, true, true};
}

} // namespace desklink
