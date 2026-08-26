#include "desklink/transport.hpp"

#include <mutex>
#include <optional>
#include <utility>

namespace desklink {
namespace {

class InMemoryEndpoint final : public ITransportEndpoint,
                               public std::enable_shared_from_this<InMemoryEndpoint> {
public:
    explicit InMemoryEndpoint(TransportPeerInfo peer) : peer_(std::move(peer)) {}

    void connect(const std::shared_ptr<InMemoryEndpoint>& other) { peer_endpoint_ = other; }

    bool send_reliable(ByteBuffer packet) override { return deliver(std::move(packet), false); }
    bool send_datagram(ByteBuffer packet) override { return deliver(std::move(packet), true); }

    void set_reliable_handler(ReceiveHandler handler) override {
        std::scoped_lock lock(mutex_);
        reliable_handler_ = std::move(handler);
    }

    void set_datagram_handler(ReceiveHandler handler) override {
        std::scoped_lock lock(mutex_);
        datagram_handler_ = std::move(handler);
    }

    void set_close_handler(CloseHandler handler) override {
        std::optional<TransportCloseReason> ExistingReason;
        {
            std::scoped_lock lock(mutex_);
            if (!closed_) close_handler_ = handler;
            else ExistingReason = close_reason_;
        }
        if (handler && ExistingReason) {
            try {
                handler(*ExistingReason);
            } catch (...) {
            }
        }
    }

    TransportPeerInfo peer_info() const override { return peer_; }

    void close() noexcept override {
        std::shared_ptr<InMemoryEndpoint> peer = peer_endpoint_.lock();
        {
            std::scoped_lock lock(mutex_);
            if (closed_) return;
            closed_ = true;
            reliable_handler_ = {};
            datagram_handler_ = {};
            close_handler_ = {};
        }
        if (peer) peer->remote_closed();
    }

private:
    void remote_closed() noexcept {
        CloseHandler Handler;
        {
            std::scoped_lock lock(mutex_);
            if (closed_) return;
            closed_ = true;
            reliable_handler_ = {};
            datagram_handler_ = {};
            close_reason_ = TransportCloseReason::Unavailable;
            Handler = std::move(close_handler_);
        }
        if (Handler) {
            try {
                Handler(TransportCloseReason::Unavailable);
            } catch (...) {
            }
        }
    }

    bool deliver(ByteBuffer packet, bool datagram) {
        std::shared_ptr<InMemoryEndpoint> peer = peer_endpoint_.lock();
        if (!peer) return false;

        ReceiveHandler handler;
        {
            std::scoped_lock lock(peer->mutex_);
            if (peer->closed_) return false;
            handler = datagram ? peer->datagram_handler_ : peer->reliable_handler_;
        }
        if (!handler) return false;
        handler(std::move(packet));
        return true;
    }

    TransportPeerInfo peer_;
    std::weak_ptr<InMemoryEndpoint> peer_endpoint_;
    mutable std::mutex mutex_;
    ReceiveHandler reliable_handler_;
    ReceiveHandler datagram_handler_;
    CloseHandler close_handler_;
    std::optional<TransportCloseReason> close_reason_;
    bool closed_{};
};

} // namespace

InMemoryTransportPair make_in_memory_transport_pair(
    TransportPeerInfo a_view_of_b,
    TransportPeerInfo b_view_of_a) {
    auto a = std::make_shared<InMemoryEndpoint>(std::move(a_view_of_b));
    auto b = std::make_shared<InMemoryEndpoint>(std::move(b_view_of_a));
    a->connect(b);
    b->connect(a);
    return {a, b};
}

} // namespace desklink
