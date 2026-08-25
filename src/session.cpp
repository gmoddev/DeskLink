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
                           std::uint64_t session_nonce,
                           DisplayTopologyExchangeOptions TopologyOptions) noexcept
    : transport_(std::move(transport)),
      coordinator_(coordinator),
      trust_store_(TrustStore),
      session_nonce_(session_nonce),
      TopologyOptions_(TopologyOptions),
      TopologyExchange_(TopologyOptions.Clock) {}

AgentSession::~AgentSession() { stop(); }

bool AgentSession::start() {
    std::scoped_lock Lock(Mutex_);
    if (started_) return true;
    const auto Trusted = GetTrustedTransportPeer(transport_, trust_store_);
    if (!Trusted) return false;
    PeerCapabilities_ = Trusted->Capabilities;
    coordinator_.set_peer_capabilities(Trusted->Capabilities);

    TopologyExchange_.Begin(
        Trusted->Identity.machine_id,
        session_nonce_,
        TopologyOptions_.Enabled,
        PeerCapabilities_.contains(Capability::DisplayTopologyExchange));
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
    PeerCapabilities_ = {};
    AudioDatagramSequence_ = 1;
    TopologyExchange_.Stop();
    LocalTopologyMachine_.reset();
    TopologySequence_ = 1;
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

bool AgentSession::CanSendAudio() const noexcept {
    std::scoped_lock Lock(Mutex_);
    return started_ &&
        PeerCapabilities_.contains(Capability::AudioReceive);
}

bool AgentSession::PeerHasCapability(Capability Value) const noexcept {
    std::scoped_lock Lock(Mutex_);
    return started_ && PeerCapabilities_.contains(Value);
}

bool AgentSession::SendAudioFrame(AudioFrameMessage Frame) {
    std::scoped_lock Lock(Mutex_);
    if (!started_ ||
        !PeerCapabilities_.contains(Capability::AudioReceive) ||
        Frame.stream_id == 0 || !IsDeskLinkAudioFrame(Frame)) {
        ++stats_.AudioSendRejected;
        return false;
    }
    EnvelopeHeader Header;
    Header.session_nonce = session_nonce_;
    Header.sequence = AudioDatagramSequence_++;
    if (AudioDatagramSequence_ == 0) ++AudioDatagramSequence_;
    auto Packet = encode_packet(Header, Frame);
    if (!transport_->send_datagram(std::move(Packet))) {
        ++stats_.AudioSendRejected;
        return false;
    }
    ++stats_.AudioSent;
    return true;
}

bool AgentSession::PublishDisplayTopology(
    const MachineId& LocalMachine,
    const DisplayTopologySnapshot& Topology) {
    std::scoped_lock Lock(Mutex_);
    DisplayTopologySnapshotMessage Message{LocalMachine, session_nonce_, Topology};
    if (!started_ || !TopologyOptions_.Enabled ||
        !PeerCapabilities_.contains(Capability::DisplayTopologyExchange) ||
        (LocalTopologyMachine_ && *LocalTopologyMachine_ != LocalMachine) ||
        !IsValidDisplayTopologySnapshotMessage(Message)) {
        ++stats_.TopologySendRejected;
        return false;
    }
    EnvelopeHeader Header;
    Header.session_nonce = session_nonce_;
    Header.sequence = TopologySequence_++;
    if (TopologySequence_ == 0) ++TopologySequence_;
    if (!transport_->send_reliable(encode_packet(Header, Message))) {
        ++stats_.TopologySendRejected;
        return false;
    }
    LocalTopologyMachine_ = LocalMachine;
    ++stats_.TopologySent;
    return true;
}

DisplayTopologyExchangeStatus
AgentSession::DisplayTopologyStatus() const noexcept {
    std::scoped_lock Lock(Mutex_);
    return TopologyExchange_.Status();
}

std::optional<DisplayTopologySnapshot>
AgentSession::RemoteDisplayTopology() const {
    std::scoped_lock Lock(Mutex_);
    return TopologyExchange_.Snapshot();
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
    const bool TopologyEnvelope =
        PeekMessageType(packet) == MessageType::DisplayTopologySnapshot;
    auto decoded = decode_packet(packet, false);
    if (!decoded.packet) {
        ++stats_.decode_rejected;
        if (TopologyEnvelope) {
            ++stats_.TopologyRejected;
            TopologyExchange_.RejectMalformed();
        }
        return;
    }
    if (!validate_session(*decoded.packet)) {
        if (TopologyEnvelope) {
            ++stats_.TopologyRejected;
            TopologyExchange_.RejectMalformed();
        }
        return;
    }

    const auto type = decoded.packet->header.type;
    if (type == MessageType::DisplayTopologySnapshot) {
        ++stats_.TopologyReceived;
        if (TopologyExchange_.Admit(
                decoded.packet->header,
                std::get<DisplayTopologySnapshotMessage>(
                    decoded.packet->message))) {
            ++stats_.TopologyAccepted;
        } else {
            ++stats_.TopologyRejected;
        }
        return;
    }
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
                         std::function<void()> FocusReadyHandler,
                         AudioReceiver* Receiver,
                         DisplayTopologyExchangeOptions TopologyOptions) noexcept
    : transport_(std::move(transport)),
      coordinator_(coordinator),
      trust_store_(TrustStore),
      session_nonce_(session_nonce),
      FocusReadyHandler_(std::move(FocusReadyHandler)),
      AudioReceiver_(Receiver),
      TopologyOptions_(TopologyOptions),
      TopologyExchange_(TopologyOptions.Clock) {}

HostSession::~HostSession() { stop(); }

bool HostSession::start() {
    std::scoped_lock Lock(Mutex_);
    if (started_) return true;
    const auto Trusted = GetTrustedTransportPeer(transport_, trust_store_);
    if (!Trusted) return false;
    PeerCapabilities_ = Trusted->Capabilities;
    TopologyExchange_.Begin(
        Trusted->Identity.machine_id,
        session_nonce_,
        TopologyOptions_.Enabled,
        PeerCapabilities_.contains(Capability::DisplayTopologyExchange));
    transport_->set_reliable_handler([this](ByteBuffer packet) { on_reliable(std::move(packet)); });
    transport_->set_datagram_handler([this](ByteBuffer packet) { on_datagram(std::move(packet)); });
    started_ = true;
    return true;
}

void HostSession::stop() noexcept {
    std::scoped_lock Lock(Mutex_);
    if (!started_) return;
    coordinator_.emergency_fail_local();
    transport_->close();
    if (AudioReceiver_) AudioReceiver_->Reset();
    PeerCapabilities_ = {};
    TopologyExchange_.Stop();
    LocalTopologyMachine_.reset();
    TopologySequence_ = 1;
    started_ = false;
}

void HostSession::on_reliable(ByteBuffer packet) {
    bool FocusReady = false;
    {
        std::scoped_lock Lock(Mutex_);
        ++stats_.reliable_received;
        const bool TopologyEnvelope =
            PeekMessageType(packet) == MessageType::DisplayTopologySnapshot;
        auto decoded = decode_packet(packet, false);
        if (!decoded.packet) {
            ++stats_.decode_rejected;
            if (TopologyEnvelope) {
                ++stats_.TopologyRejected;
                TopologyExchange_.RejectMalformed();
            }
            return;
        }
        if (decoded.packet->header.session_nonce != session_nonce_) {
            ++stats_.session_rejected;
            if (TopologyEnvelope) {
                ++stats_.TopologyRejected;
                TopologyExchange_.RejectMalformed();
            }
            return;
        }
        if (decoded.packet->header.type ==
            MessageType::DisplayTopologySnapshot) {
            ++stats_.TopologyReceived;
            if (TopologyExchange_.Admit(
                    decoded.packet->header,
                    std::get<DisplayTopologySnapshotMessage>(
                        decoded.packet->message))) {
                ++stats_.TopologyAccepted;
            } else {
                ++stats_.TopologyRejected;
            }
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

void HostSession::on_datagram(ByteBuffer packet) {
    std::scoped_lock Lock(Mutex_);
    ++stats_.datagrams_received;
    auto Decoded = decode_packet(packet, true);
    if (!Decoded.packet) {
        ++stats_.decode_rejected;
        ++stats_.AudioRejected;
        return;
    }
    if (Decoded.packet->header.session_nonce != session_nonce_) {
        ++stats_.session_rejected;
        ++stats_.AudioRejected;
        return;
    }
    ++stats_.AudioReceived;
    if (Decoded.packet->header.type != MessageType::AudioFrame ||
        !PeerCapabilities_.contains(Capability::AudioSend) ||
        AudioReceiver_ == nullptr ||
        !AudioReceiver_->Push(
            Decoded.packet->header.sequence,
            std::get<AudioFrameMessage>(std::move(Decoded.packet->message)))) {
        ++stats_.authorization_rejected;
        ++stats_.AudioRejected;
        return;
    }
    ++stats_.AudioAccepted;
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

bool HostSession::SendPointerMotion(PointerMotionMessage Message) {
    std::scoped_lock Lock(Mutex_);
    if (!started_) return false;
    auto Packet = coordinator_.PointerMotion(Message);
    return Packet.has_value() && transport_->send_datagram(std::move(*Packet));
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

bool HostSession::CanReceiveAudio() const noexcept {
    std::scoped_lock Lock(Mutex_);
    return started_ && AudioReceiver_ != nullptr &&
        PeerCapabilities_.contains(Capability::AudioSend);
}

bool HostSession::PeerHasCapability(Capability Value) const noexcept {
    std::scoped_lock Lock(Mutex_);
    return started_ && PeerCapabilities_.contains(Value);
}

bool HostSession::PublishDisplayTopology(
    const MachineId& LocalMachine,
    const DisplayTopologySnapshot& Topology) {
    std::scoped_lock Lock(Mutex_);
    DisplayTopologySnapshotMessage Message{LocalMachine, session_nonce_, Topology};
    if (!started_ || !TopologyOptions_.Enabled ||
        !PeerCapabilities_.contains(Capability::DisplayTopologyExchange) ||
        (LocalTopologyMachine_ && *LocalTopologyMachine_ != LocalMachine) ||
        !IsValidDisplayTopologySnapshotMessage(Message)) {
        ++stats_.TopologySendRejected;
        return false;
    }
    EnvelopeHeader Header;
    Header.session_nonce = session_nonce_;
    Header.sequence = TopologySequence_++;
    if (TopologySequence_ == 0) ++TopologySequence_;
    if (!transport_->send_reliable(encode_packet(Header, Message))) {
        ++stats_.TopologySendRejected;
        return false;
    }
    LocalTopologyMachine_ = LocalMachine;
    ++stats_.TopologySent;
    return true;
}

DisplayTopologyExchangeStatus
HostSession::DisplayTopologyStatus() const noexcept {
    std::scoped_lock Lock(Mutex_);
    return TopologyExchange_.Status();
}

std::optional<DisplayTopologySnapshot>
HostSession::RemoteDisplayTopology() const {
    std::scoped_lock Lock(Mutex_);
    return TopologyExchange_.Snapshot();
}

SessionStats HostSession::stats() const noexcept {
    std::scoped_lock Lock(Mutex_);
    return stats_;
}

} // namespace desklink
