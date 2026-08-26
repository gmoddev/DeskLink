#pragma once

#include "desklink/pairing.hpp"
#include "desklink/transport.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <msquic.h>

#include <memory>
#include <optional>

namespace desklink {

enum class MsQuicPeerValidation {
    PeerValidated,
};

class MsQuicTransportEndpoint final : public ITransportEndpoint {
public:
    struct State;

    static std::shared_ptr<MsQuicTransportEndpoint> Adopt(
        const QUIC_API_TABLE* Api,
        HQUIC Connection,
        HQUIC ReliableStream,
        TransportPeerInfo Peer,
        MsQuicPeerValidation PeerValidation,
        ByteBuffer InitialReliableBytes = {});

    ~MsQuicTransportEndpoint() override;

    [[nodiscard]] bool OpenReliableStream();
    bool send_reliable(ByteBuffer Packet) override;
    bool send_datagram(ByteBuffer Packet) override;
    void set_reliable_handler(ReceiveHandler Handler) override;
    void set_datagram_handler(ReceiveHandler Handler) override;
    void set_close_handler(CloseHandler Handler) override;
    [[nodiscard]] TransportPeerInfo peer_info() const override;
    void close() noexcept override;

private:
    explicit MsQuicTransportEndpoint(std::shared_ptr<State> SharedState);

    std::shared_ptr<State> State_;
};

[[nodiscard]] std::optional<TransportPeerInfo> VerifyMsQuicPeerCertificate(
    const QUIC_CERTIFICATE* Certificate,
    const ITrustStore& TrustStore,
    const IPairingCrypto& Crypto,
    const MachineId* ExpectedMachine = nullptr);

} // namespace desklink
