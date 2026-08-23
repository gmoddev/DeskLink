#include "desklink/session.hpp"

#include <algorithm>

namespace desklink {
namespace {

std::optional<TrustedPeer> GetTrustedTransportPeer(
    const std::shared_ptr<ITransportEndpoint>& Transport,
    const ITrustStore& TrustStore) {
    if (!Transport) return std::nullopt;
    const auto Peer = Transport->peer_info();
    if (!Peer.authenticated || !Peer.encrypted || !IsTrustedPeer(TrustStore, Peer.identity)) {
        return std::nullopt;
    }
    return TrustStore.GetPeer(Peer.identity.machine_id);
}

std::uint32_t EffectiveLease(std::uint32_t Requested) {
    return std::clamp<std::uint32_t>(Requested, 100u, 2000u);
}

} // namespace

AgentSession::AgentSession(std::shared_ptr<ITransportEndpoint> transport,
                           AgentCoordinator& coordinator,
                           const ITrustStore& TrustStore,
                           std::uint64_t session_nonce) noexcept
    : transport_(std::move(transport)),
      coordinator_(coordinator),
      trust_store_(TrustStore),
      session_nonce_(session_nonce) {}

AgentSession::~AgentSession() { stop(); }

bool AgentSession::start() {
    std::scoped_lock Lock(Mutex_);
    if (started_) return true;
    const auto Trusted = GetTrustedTransportPeer(transport_, trust_store_);
    if (!Trusted) return false;
    coordinator_.set_peer_capabilities(Trusted->Capabilities);

    transport_->set_reliable_handler([this](ByteBuffer packet) { on_reliable(std::move(packet)); });
    transport_->set_datagram_handler([this](ByteBuffer packet) { on_datagram(std::move(packet)); });
    started_ = true;
    return true;
}

void AgentSession::stop() noexcept {
    std::scoped_lock Lock(Mutex_);
    if (!started_) return;
    coordinator_.disconnect();
    transport_->close();
    started_ = false;
}

void AgentSession::tick() noexcept {
    std::scoped_lock Lock(Mutex_);
    coordinator_.tick();
}

void AgentSession::SetLocalDesiredMode(DeskMode Mode) noexcept {
    std::scoped_lock Lock(Mutex_);
    coordinator_.SetLocalDesiredMode(Mode);
}

DeskMode AgentSession::DesiredMode() const noexcept {
    std::scoped_lock Lock(Mutex_);
    return coordinator_.DesiredMode();
}

bool AgentSession::RemoteFocused() const noexcept {
    std::scoped_lock Lock(Mutex_);
    return coordinator_.RemoteFocused();
}

SessionStats AgentSession::stats() const noexcept {
    std::scoped_lock Lock(Mutex_);
    return stats_;
}

bool AgentSession::validate_session(const DecodedPacket& packet) noexcept {
    if (packet.header.session_nonce != session_nonce_) {
        ++stats_.session_rejected;
        return false;
    }
    return true;
}

void AgentSession::count_decision(AgentDecision decision) noexcept {
    if (decision != AgentDecision::Accepted && decision != AgentDecision::Ignored) {
        ++stats_.authorization_rejected;
    }
}

void AgentSession::on_reliable(ByteBuffer packet) {
    std::scoped_lock Lock(Mutex_);
    ++stats_.reliable_received;
    auto decoded = decode_packet(packet, false);
    if (!decoded.packet) {
        ++stats_.decode_rejected;
        return;
    }
    if (!validate_session(*decoded.packet)) return;

    const auto type = decoded.packet->header.type;
    const auto decision = coordinator_.handle(*decoded.packet);
    count_decision(decision);

    if (type == MessageType::FocusRequest && decision == AgentDecision::Accepted) {
        const auto& request = std::get<FocusRequestMessage>(decoded.packet->message);
        EnvelopeHeader response;
        response.session_nonce = session_nonce_;
        response.epoch = coordinator_.focus_state().epoch();
        response.sequence = response_sequence_++;
        auto ready = encode_packet(response, FocusReadyMessage{
            EffectiveLease(request.requested_lease_ms), request.request_id});
        (void)transport_->send_reliable(std::move(ready));
    }
}

void AgentSession::on_datagram(ByteBuffer packet) {
    std::scoped_lock Lock(Mutex_);
    ++stats_.datagrams_received;
    auto decoded = decode_packet(packet, true);
    if (!decoded.packet) {
        ++stats_.decode_rejected;
        return;
    }
    if (!validate_session(*decoded.packet)) return;
    count_decision(coordinator_.handle(*decoded.packet));
}

HostSession::HostSession(std::shared_ptr<ITransportEndpoint> transport,
                         HostCoordinator& coordinator,
                         const ITrustStore& TrustStore,
                         std::uint64_t session_nonce,
                         std::function<void()> FocusReadyHandler) noexcept
    : transport_(std::move(transport)),
      coordinator_(coordinator),
      trust_store_(TrustStore),
      session_nonce_(session_nonce),
      FocusReadyHandler_(std::move(FocusReadyHandler)) {}

HostSession::~HostSession() { stop(); }

bool HostSession::start() {
    std::scoped_lock Lock(Mutex_);
    if (started_) return true;
    if (!GetTrustedTransportPeer(transport_, trust_store_)) return false;
    transport_->set_reliable_handler([this](ByteBuffer packet) { on_reliable(std::move(packet)); });
    started_ = true;
    return true;
}

void HostSession::stop() noexcept {
    std::scoped_lock Lock(Mutex_);
    if (!started_) return;
    coordinator_.emergency_fail_local();
    transport_->close();
    started_ = false;
}

void HostSession::on_reliable(ByteBuffer packet) {
    bool FocusReady = false;
    {
        std::scoped_lock Lock(Mutex_);
        ++stats_.reliable_received;
        auto decoded = decode_packet(packet, false);
        if (!decoded.packet) {
            ++stats_.decode_rejected;
            return;
        }
        if (decoded.packet->header.session_nonce != session_nonce_) {
            ++stats_.session_rejected;
            return;
        }
        if (decoded.packet->header.type == MessageType::FocusReady) {
            if (!coordinator_.accept_focus_ready(*decoded.packet)) {
                ++stats_.authorization_rejected;
            } else {
                FocusReady = true;
            }
        }
    }
    if (FocusReady && FocusReadyHandler_) FocusReadyHandler_();
}

bool HostSession::focus_remote(std::uint32_t lease_ms) {
    std::scoped_lock Lock(Mutex_);
    if (!started_) return false;
    return transport_->send_reliable(coordinator_.request_remote_focus(lease_ms));
}

bool HostSession::SetDesiredMode(DeskMode Mode) {
    std::scoped_lock Lock(Mutex_);
    if (!started_) return false;
    auto Packet = coordinator_.set_mode(Mode);
    return transport_->send_reliable(std::move(Packet));
}

bool HostSession::renew_focus(std::uint32_t lease_ms) {
    std::scoped_lock Lock(Mutex_);
    if (!started_) return false;
    auto packet = coordinator_.renew_remote_focus(lease_ms);
    return packet.has_value() && transport_->send_reliable(std::move(*packet));
}

bool HostSession::release_focus() {
    std::scoped_lock Lock(Mutex_);
    if (!started_) return false;
    auto packet = coordinator_.release_remote_focus();
    return packet.has_value() && transport_->send_reliable(std::move(*packet));
}

bool HostSession::send_key(KeyEventMessage event) {
    std::scoped_lock Lock(Mutex_);
    if (!started_) return false;
    auto packet = coordinator_.key_event(event);
    return packet.has_value() && transport_->send_reliable(std::move(*packet));
}

bool HostSession::send_button(MouseButtonMessage event) {
    std::scoped_lock Lock(Mutex_);
    if (!started_) return false;
    auto packet = coordinator_.mouse_button(event);
    return packet.has_value() && transport_->send_reliable(std::move(*packet));
}

bool HostSession::send_pointer(PointerPositionMessage event) {
    std::scoped_lock Lock(Mutex_);
    if (!started_) return false;
    auto packet = coordinator_.pointer_position(event);
    return packet.has_value() && transport_->send_datagram(std::move(*packet));
}

bool HostSession::SendWheel(MouseWheelMessage Message) {
    std::scoped_lock Lock(Mutex_);
    if (!started_) return false;
    auto Packet = coordinator_.MouseWheel(Message);
    return Packet.has_value() && transport_->send_reliable(std::move(*Packet));
}

bool HostSession::SendInputStateSnapshot() {
    std::scoped_lock Lock(Mutex_);
    if (!started_) return false;
    auto Packet = coordinator_.InputStateSnapshot();
    return Packet.has_value() && transport_->send_reliable(std::move(*Packet));
}

bool HostSession::RemoteFocused() const noexcept {
    std::scoped_lock Lock(Mutex_);
    return coordinator_.remote_focused();
}

DeskMode HostSession::DesiredMode() const noexcept {
    std::scoped_lock Lock(Mutex_);
    return coordinator_.desired_mode();
}

SessionStats HostSession::stats() const noexcept {
    std::scoped_lock Lock(Mutex_);
    return stats_;
}

} // namespace desklink
