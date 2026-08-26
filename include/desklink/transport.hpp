#pragma once

#include "desklink/types.hpp"

#include <functional>
#include <memory>
#include <string>

namespace desklink {

struct TransportPeerInfo {
    PeerIdentity identity;
    bool authenticated{};
    bool encrypted{};
};

enum class TransportCloseReason : std::uint8_t {
    Unavailable = 0,
    ProtocolFailure = 1,
};

class ITransportEndpoint {
public:
    using ReceiveHandler = std::function<void(ByteBuffer)>;
    using CloseHandler = std::function<void(TransportCloseReason)>;

    virtual ~ITransportEndpoint() = default;
    virtual bool send_reliable(ByteBuffer packet) = 0;
    virtual bool send_datagram(ByteBuffer packet) = 0;
    virtual void set_reliable_handler(ReceiveHandler handler) = 0;
    virtual void set_datagram_handler(ReceiveHandler handler) = 0;
    virtual void set_close_handler(CloseHandler handler) = 0;
    [[nodiscard]] virtual TransportPeerInfo peer_info() const = 0;
    virtual void close() noexcept = 0;
};

struct InMemoryTransportPair {
    std::shared_ptr<ITransportEndpoint> a;
    std::shared_ptr<ITransportEndpoint> b;
};

[[nodiscard]] InMemoryTransportPair make_in_memory_transport_pair(
    TransportPeerInfo a_view_of_b,
    TransportPeerInfo b_view_of_a);

} // namespace desklink
