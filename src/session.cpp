#include "desklink/session.hpp"

#include <algorithm>

namespace desklink {
namespace {

bool transport_is_secure(const std::shared_ptr<ITransportEndpoint>& transport) {
    if (!transport) return false;
    const auto peer = transport->peer_info();
    return peer.authenticated && peer.encrypted;
}

std::uint32_t effective_lease(std::uint32_t requested) {
    return std::clamp<std::uint32_t>(requested, 100u, 2000u);
}

} // namespace

AgentSession::AgentSession(std::shared_ptr<ITransportEndpoint> transport,
                           AgentCoordinator& coordinator,
                           std::uint64_t session_nonce) noexcept
    : transport_(std::move(transport)), coordinator_(coordinator), session_nonce_(session_nonce) {}

AgentSession::~AgentSession() { stop(); }

bool AgentSession::start() {
    if (started_) return true;
    if (!transport_is_secure(transport_)) return false;

    transport_->set_reliable_handler([this](ByteBuffer packet) { on_reliable(std::move(packet)); });
    transport_->set_datagram_handler([this](ByteBuffer packet) { on_datagram(std::move(packet)); });
    started_ = true;
    return true;
}

void AgentSession::stop() noexcept {
    if (!started_) return;
    coordinator_.disconnect();
    transport_->close();
    started_ = false;
}

void AgentSession::tick() noexcept { coordinator_.tick(); }

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
            effective_lease(request.requested_lease_ms), request.request_id});
        (void)transport_->send_reliable(std::move(ready));
    }
}

void AgentSession::on_datagram(ByteBuffer packet) {
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
                         std::uint64_t session_nonce) noexcept
    : transport_(std::move(transport)), coordinator_(coordinator), session_nonce_(session_nonce) {}

HostSession::~HostSession() { stop(); }

bool HostSession::start() {
    if (started_) return true;
    if (!transport_is_secure(transport_)) return false;
    transport_->set_reliable_handler([this](ByteBuffer packet) { on_reliable(std::move(packet)); });
    started_ = true;
    return true;
}

void HostSession::stop() noexcept {
    if (!started_) return;
    coordinator_.emergency_fail_local();
    transport_->close();
    started_ = false;
}

void HostSession::on_reliable(ByteBuffer packet) {
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
        }
    }
}

bool HostSession::focus_remote(std::uint32_t lease_ms) {
    if (!started_) return false;
    return transport_->send_reliable(coordinator_.request_remote_focus(lease_ms));
}

bool HostSession::renew_focus(std::uint32_t lease_ms) {
    if (!started_) return false;
    auto packet = coordinator_.renew_remote_focus(lease_ms);
    return packet.has_value() && transport_->send_reliable(std::move(*packet));
}

bool HostSession::release_focus() {
    if (!started_) return false;
    auto packet = coordinator_.release_remote_focus();
    return packet.has_value() && transport_->send_reliable(std::move(*packet));
}

bool HostSession::send_key(KeyEventMessage event) {
    if (!started_) return false;
    auto packet = coordinator_.key_event(event);
    return packet.has_value() && transport_->send_reliable(std::move(*packet));
}

bool HostSession::send_button(MouseButtonMessage event) {
    if (!started_) return false;
    auto packet = coordinator_.mouse_button(event);
    return packet.has_value() && transport_->send_reliable(std::move(*packet));
}

bool HostSession::send_pointer(PointerPositionMessage event) {
    if (!started_) return false;
    auto packet = coordinator_.pointer_position(event);
    return packet.has_value() && transport_->send_datagram(std::move(*packet));
}

} // namespace desklink
