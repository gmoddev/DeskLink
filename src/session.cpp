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
    const auto Gate = CallbackGate_;
    transport_->set_reliable_handler([this, Gate](ByteBuffer packet) {
        auto Guard = Gate->TryEnter();
        if (Guard) on_reliable(std::move(packet));
    });
    transport_->set_datagram_handler([this, Gate](ByteBuffer packet) {
        auto Guard = Gate->TryEnter();
        if (Guard) on_datagram(std::move(packet));
    });
    started_ = true;
    return true;
}

void AgentSession::stop() noexcept {
    {
        std::scoped_lock Lock(Mutex_);
        if (!started_) return;
        started_ = false;
    }
    CallbackGate_->Close();
    transport_->close();
    CallbackGate_->Wait();
    {
        std::scoped_lock Lock(Mutex_);
        coordinator_.disconnect();
        PeerCapabilities_ = {};
        AudioDatagramSequence_ = 1;
        TopologyExchange_.Stop();
        LocalTopologyMachine_.reset();
        TopologySequence_ = 1;
    }
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
    const auto Gate = CallbackGate_;
    transport_->set_reliable_handler([this, Gate](ByteBuffer packet) {
        auto Guard = Gate->TryEnter();
        if (Guard) on_reliable(std::move(packet));
    });
    transport_->set_datagram_handler([this, Gate](ByteBuffer packet) {
        auto Guard = Gate->TryEnter();
        if (Guard) on_datagram(std::move(packet));
    });
    started_ = true;
    return true;
}

void HostSession::stop() noexcept {
    {
        std::scoped_lock Lock(Mutex_);
        if (!started_) return;
        started_ = false;
    }
    CallbackGate_->Close();
    transport_->close();
    CallbackGate_->Wait();
    {
        std::scoped_lock Lock(Mutex_);
        coordinator_.emergency_fail_local();
        if (AudioReceiver_) AudioReceiver_->Reset();
        PeerCapabilities_ = {};
        TopologyExchange_.Stop();
        LocalTopologyMachine_.reset();
        TopologySequence_ = 1;
    }
}

void HostSession::on_reliable(ByteBuffer packet) {
    bool FocusReady = false;
    std::function<void()> FocusReadyHandler;
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
        if (FocusReady) FocusReadyHandler = FocusReadyHandler_;
    }
    if (FocusReadyHandler) FocusReadyHandler();
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

PeerSession::PeerSession(
    std::shared_ptr<ITransportEndpoint> Transport,
    HostCoordinator& OutgoingCoordinator,
    AgentCoordinator& IncomingCoordinator,
    const ITrustStore& TrustStore,
    std::uint64_t SessionNonce,
    PeerSessionHandlers Handlers,
    AudioReceiver* Receiver,
    DisplayTopologyExchangeOptions TopologyOptions,
    ClipboardSessionOptions ClipboardOptions) noexcept
    : Transport_(std::move(Transport)),
      OutgoingCoordinator_(OutgoingCoordinator),
      IncomingCoordinator_(IncomingCoordinator),
      TrustStore_(TrustStore),
      SessionNonce_(SessionNonce),
      Handlers_(std::move(Handlers)),
      AudioReceiver_(Receiver),
      TopologyOptions_(TopologyOptions),
      TopologyExchange_(TopologyOptions.Clock),
      ClipboardOptions_(std::move(ClipboardOptions)),
      ClipboardExchange_(ClipboardOptions_.Clock) {}

PeerSession::~PeerSession() { Stop(); }

bool PeerSession::Start() {
    std::scoped_lock Lock(Mutex_);
    if (Started_) return true;
    const auto Trusted = GetTrustedTransportPeer(Transport_, TrustStore_);
    if (!Trusted || SessionNonce_ == 0 ||
        !DirectionArbiter_.BindSession(
            Trusted->Identity.machine_id, SessionNonce_)) {
        return false;
    }

    PeerMachine_ = Trusted->Identity.machine_id;
    LocalCapabilities_ = Trusted->Capabilities;
    IncomingCoordinator_.set_peer_capabilities(LocalCapabilities_);
    TopologyExchange_.Begin(
        PeerMachine_, SessionNonce_, TopologyOptions_.Enabled,
        LocalCapabilities_.contains(
            Capability::DisplayTopologyExchange));
    ClipboardExchange_.Begin(
        ClipboardOptions_.LocalMachine, PeerMachine_, SessionNonce_,
        ClipboardOptions_.Enabled, LocalCapabilities_);
    const auto Gate = CallbackGate_;
    Transport_->set_reliable_handler(
        [this, Gate](ByteBuffer Packet) {
            auto Guard = Gate->TryEnter();
            if (Guard) OnReliable(std::move(Packet));
        });
    Transport_->set_datagram_handler(
        [this, Gate](ByteBuffer Packet) {
            auto Guard = Gate->TryEnter();
            if (Guard) OnDatagram(std::move(Packet));
        });
    CapabilityConflict_ = false;
    Started_ = true;
    (void)PublishCapabilityGrantLocked();
    return true;
}

void PeerSession::Stop() noexcept {
    {
        std::scoped_lock Lock(Mutex_);
        if (!Started_) return;
        Started_ = false;
    }
    CallbackGate_->Close();
    Transport_->close();
    CallbackGate_->Wait();
    {
        std::scoped_lock Lock(Mutex_);
        FailLocalDirectionsLocked();
        if (AudioReceiver_) AudioReceiver_->Reset();
        LocalCapabilities_ = {};
        RemoteCapabilities_.reset();
        TopologyExchange_.Stop();
        ClipboardExchange_.Stop();
        LocalTopologyMachine_.reset();
        DirectionArbiter_.ResetSession();
        PeerMachine_ = {};
        ReliableSequence_ = 1;
        AudioDatagramSequence_ = 1;
        TopologySequence_ = 1;
        CapabilityConflict_ = false;
    }
}

void PeerSession::Tick() noexcept {
    bool DirectionChanged = false;
    {
        std::scoped_lock Lock(Mutex_);
        if (!Started_) return;
        IncomingCoordinator_.tick();
        if (DirectionArbiter_.State() ==
                PeerDirectionState::IncomingActive &&
            !IncomingCoordinator_.RemoteFocused()) {
            ReleaseIncomingDirectionLocked();
            DirectionChanged = true;
        }
        if (!RemoteCapabilities_ && !CapabilityConflict_) {
            (void)PublishCapabilityGrantLocked();
        }
        (void)PublishClipboardHelloLocked();
    }
    if (DirectionChanged && Handlers_.DirectionChanged) {
        Handlers_.DirectionChanged();
    }
}

void PeerSession::FailLocalDirections() noexcept {
    {
        std::scoped_lock Lock(Mutex_);
        if (!Started_) return;
        FailLocalDirectionsLocked();
    }
    if (Handlers_.DirectionChanged) Handlers_.DirectionChanged();
}

void PeerSession::SetLocalDesiredMode(DeskMode Mode) noexcept {
    bool DirectionChanged = false;
    {
        std::scoped_lock Lock(Mutex_);
        if (!Started_) return;
        IncomingCoordinator_.SetLocalDesiredMode(Mode);
        if (DirectionArbiter_.State() ==
                PeerDirectionState::IncomingActive &&
            !IncomingCoordinator_.RemoteFocused()) {
            ReleaseIncomingDirectionLocked();
            DirectionChanged = true;
        }
    }
    if (DirectionChanged && Handlers_.DirectionChanged) {
        Handlers_.DirectionChanged();
    }
}

DeskMode PeerSession::IncomingDesiredMode() const noexcept {
    std::scoped_lock Lock(Mutex_);
    return IncomingCoordinator_.DesiredMode();
}

bool PeerSession::BeginOutgoingFocus(std::uint32_t LeaseMilliseconds) {
    std::scoped_lock Lock(Mutex_);
    if (!Started_ || CapabilityConflict_ || !RemoteCapabilities_ ||
        !RemoteCapabilities_->contains(Capability::InputInject)) {
        ++Stats_.DirectionRejected;
        return false;
    }
    const auto Decision = DirectionArbiter_.BeginOutgoing();
    if (Decision.Outcome != PeerDirectionOutcome::Admitted ||
        !Decision.Token) {
        ++Stats_.DirectionRejected;
        return false;
    }
    OutgoingToken_ = *Decision.Token;
    if (!Transport_->send_reliable(
            OutgoingCoordinator_.request_remote_focus(
                LeaseMilliseconds))) {
        (void)DirectionArbiter_.Release(*OutgoingToken_);
        OutgoingToken_.reset();
        OutgoingCoordinator_.emergency_fail_local();
        ++Stats_.DirectionRejected;
        return false;
    }
    return true;
}

bool PeerSession::SetDesiredMode(DeskMode Mode) {
    bool DirectionChanged = false;
    bool Sent = false;
    {
        std::scoped_lock Lock(Mutex_);
        if (!Started_) return false;
        auto Packet = OutgoingCoordinator_.set_mode(Mode);
        Sent = Transport_->send_reliable(std::move(Packet));
        if (!OutgoingCoordinator_.remote_focused() && OutgoingToken_) {
            (void)DirectionArbiter_.Release(*OutgoingToken_);
            OutgoingToken_.reset();
            DirectionChanged = true;
        }
    }
    if (DirectionChanged && Handlers_.DirectionChanged) {
        Handlers_.DirectionChanged();
    }
    return Sent;
}

bool PeerSession::RenewOutgoingFocus(
    std::uint32_t LeaseMilliseconds) {
    std::scoped_lock Lock(Mutex_);
    if (!Started_ || !OutgoingToken_ ||
        DirectionArbiter_.State() !=
            PeerDirectionState::OutgoingActive) {
        return false;
    }
    auto Packet = OutgoingCoordinator_.renew_remote_focus(
        LeaseMilliseconds);
    return Packet && Transport_->send_reliable(std::move(*Packet));
}

bool PeerSession::ReleaseOutgoingFocus() {
    bool DirectionChanged = false;
    bool Sent = false;
    {
        std::scoped_lock Lock(Mutex_);
        if (!Started_ || !OutgoingToken_) return false;
        auto Packet = OutgoingCoordinator_.release_remote_focus();
        if (Packet) Sent = Transport_->send_reliable(std::move(*Packet));
        (void)DirectionArbiter_.Release(*OutgoingToken_);
        OutgoingToken_.reset();
        DirectionChanged = true;
    }
    if (DirectionChanged && Handlers_.DirectionChanged) {
        Handlers_.DirectionChanged();
    }
    return Sent;
}

bool PeerSession::SendKey(KeyEventMessage Event) {
    std::scoped_lock Lock(Mutex_);
    if (!Started_ || DirectionArbiter_.State() !=
            PeerDirectionState::OutgoingActive) {
        return false;
    }
    auto Packet = OutgoingCoordinator_.key_event(Event);
    return Packet && Transport_->send_reliable(std::move(*Packet));
}

bool PeerSession::SendButton(MouseButtonMessage Event) {
    std::scoped_lock Lock(Mutex_);
    if (!Started_ || DirectionArbiter_.State() !=
            PeerDirectionState::OutgoingActive) {
        return false;
    }
    auto Packet = OutgoingCoordinator_.mouse_button(Event);
    return Packet && Transport_->send_reliable(std::move(*Packet));
}

bool PeerSession::SendPointer(PointerPositionMessage Event) {
    std::scoped_lock Lock(Mutex_);
    if (!Started_ || DirectionArbiter_.State() !=
            PeerDirectionState::OutgoingActive) {
        return false;
    }
    auto Packet = OutgoingCoordinator_.pointer_position(Event);
    return Packet && Transport_->send_datagram(std::move(*Packet));
}

bool PeerSession::SendPointerMotion(PointerMotionMessage Message) {
    std::scoped_lock Lock(Mutex_);
    if (!Started_ || DirectionArbiter_.State() !=
            PeerDirectionState::OutgoingActive) {
        return false;
    }
    auto Packet = OutgoingCoordinator_.PointerMotion(Message);
    return Packet && Transport_->send_datagram(std::move(*Packet));
}

bool PeerSession::SendWheel(MouseWheelMessage Message) {
    std::scoped_lock Lock(Mutex_);
    if (!Started_ || DirectionArbiter_.State() !=
            PeerDirectionState::OutgoingActive) {
        return false;
    }
    auto Packet = OutgoingCoordinator_.MouseWheel(Message);
    return Packet && Transport_->send_reliable(std::move(*Packet));
}

bool PeerSession::SendInputStateSnapshot() {
    std::scoped_lock Lock(Mutex_);
    if (!Started_ || DirectionArbiter_.State() !=
            PeerDirectionState::OutgoingActive) {
        return false;
    }
    auto Packet = OutgoingCoordinator_.InputStateSnapshot();
    return Packet && Transport_->send_reliable(std::move(*Packet));
}

bool PeerSession::OutgoingFocused() const noexcept {
    std::scoped_lock Lock(Mutex_);
    return Started_ && DirectionArbiter_.State() ==
        PeerDirectionState::OutgoingActive &&
        OutgoingCoordinator_.remote_focused();
}

bool PeerSession::IncomingFocused() const noexcept {
    std::scoped_lock Lock(Mutex_);
    return Started_ && DirectionArbiter_.State() ==
        PeerDirectionState::IncomingActive &&
        IncomingCoordinator_.RemoteFocused();
}

PeerDirectionState PeerSession::DirectionState() const noexcept {
    std::scoped_lock Lock(Mutex_);
    return DirectionArbiter_.State();
}

bool PeerSession::CanBeginOutgoing() const noexcept {
    std::scoped_lock Lock(Mutex_);
    return Started_ && !CapabilityConflict_ && RemoteCapabilities_ &&
        RemoteCapabilities_->contains(Capability::InputInject) &&
        DirectionArbiter_.State() == PeerDirectionState::Local;
}

bool PeerSession::PeerGrantedCapability(Capability Value) const noexcept {
    std::scoped_lock Lock(Mutex_);
    return Started_ && !CapabilityConflict_ && RemoteCapabilities_ &&
        RemoteCapabilities_->contains(Value);
}

bool PeerSession::GrantedToPeer(Capability Value) const noexcept {
    std::scoped_lock Lock(Mutex_);
    return Started_ && LocalCapabilities_.contains(Value);
}

bool PeerSession::CanSendAudio() const noexcept {
    std::scoped_lock Lock(Mutex_);
    return Started_ && !CapabilityConflict_ &&
        LocalCapabilities_.contains(Capability::AudioReceive);
}

bool PeerSession::CanReceiveAudio() const noexcept {
    std::scoped_lock Lock(Mutex_);
    return Started_ && AudioReceiver_ != nullptr &&
        LocalCapabilities_.contains(Capability::AudioSend);
}

bool PeerSession::SendAudioFrame(AudioFrameMessage Frame) {
    std::scoped_lock Lock(Mutex_);
    if (!Started_ || CapabilityConflict_ ||
        !LocalCapabilities_.contains(Capability::AudioReceive) ||
        Frame.stream_id == 0 || !IsDeskLinkAudioFrame(Frame)) {
        ++Stats_.AudioSendRejected;
        return false;
    }
    EnvelopeHeader Header;
    Header.session_nonce = SessionNonce_;
    Header.sequence = AudioDatagramSequence_++;
    if (AudioDatagramSequence_ == 0) ++AudioDatagramSequence_;
    if (!Transport_->send_datagram(encode_packet(Header, Frame))) {
        ++Stats_.AudioSendRejected;
        return false;
    }
    ++Stats_.AudioSent;
    return true;
}

bool PeerSession::CanSendClipboard() const noexcept {
    std::scoped_lock Lock(Mutex_);
    return Started_ && !CapabilityConflict_ && ClipboardExchange_.CanSend();
}

bool PeerSession::CanReceiveClipboard() const noexcept {
    std::scoped_lock Lock(Mutex_);
    return Started_ && !CapabilityConflict_ &&
        ClipboardOptions_.ApplyText && ClipboardExchange_.CanReceive();
}

bool PeerSession::PublishClipboardText(std::string Text) {
    std::scoped_lock Lock(Mutex_);
    if (!Started_ || CapabilityConflict_) {
        ++Stats_.ClipboardSendRejected;
        return false;
    }
    auto Message = ClipboardExchange_.BuildText(std::move(Text));
    if (!Message) {
        ++Stats_.ClipboardSendRejected;
        return false;
    }
    EnvelopeHeader Header;
    Header.session_nonce = SessionNonce_;
    Header.sequence = ReliableSequence_++;
    if (ReliableSequence_ == 0) ++ReliableSequence_;
    if (!Transport_->send_reliable(encode_packet(Header, *Message))) {
        ++Stats_.ClipboardSendRejected;
        return false;
    }
    ++Stats_.ClipboardSent;
    return true;
}

bool PeerSession::PublishDisplayTopology(
    const MachineId& LocalMachine,
    const DisplayTopologySnapshot& Topology) {
    std::scoped_lock Lock(Mutex_);
    DisplayTopologySnapshotMessage Message{
        LocalMachine, SessionNonce_, Topology};
    if (!Started_ || CapabilityConflict_ ||
        !TopologyOptions_.Enabled ||
        !LocalCapabilities_.contains(
            Capability::DisplayTopologyExchange) ||
        (LocalTopologyMachine_ &&
         *LocalTopologyMachine_ != LocalMachine) ||
        !IsValidDisplayTopologySnapshotMessage(Message)) {
        ++Stats_.TopologySendRejected;
        return false;
    }
    EnvelopeHeader Header;
    Header.session_nonce = SessionNonce_;
    Header.sequence = TopologySequence_++;
    if (TopologySequence_ == 0) ++TopologySequence_;
    if (!Transport_->send_reliable(encode_packet(Header, Message))) {
        ++Stats_.TopologySendRejected;
        return false;
    }
    LocalTopologyMachine_ = LocalMachine;
    ++Stats_.TopologySent;
    return true;
}

DisplayTopologyExchangeStatus
PeerSession::DisplayTopologyStatus() const noexcept {
    std::scoped_lock Lock(Mutex_);
    return TopologyExchange_.Status();
}

std::optional<DisplayTopologySnapshot>
PeerSession::RemoteDisplayTopology() const {
    std::scoped_lock Lock(Mutex_);
    return TopologyExchange_.Snapshot();
}

SessionStats PeerSession::Stats() const noexcept {
    std::scoped_lock Lock(Mutex_);
    return Stats_;
}

bool PeerSession::ValidateSession(
    const DecodedPacket& Packet) noexcept {
    if (Packet.header.session_nonce != SessionNonce_) {
        ++Stats_.session_rejected;
        return false;
    }
    return true;
}

void PeerSession::CountDecision(AgentDecision Decision) noexcept {
    if (Decision != AgentDecision::Accepted &&
        Decision != AgentDecision::Ignored) {
        ++Stats_.authorization_rejected;
    }
}

bool PeerSession::PublishCapabilityGrantLocked() {
    if (!Started_) return false;
    EnvelopeHeader Header;
    Header.session_nonce = SessionNonce_;
    Header.sequence = ReliableSequence_++;
    if (ReliableSequence_ == 0) ++ReliableSequence_;
    if (!Transport_->send_reliable(encode_packet(
            Header, CapabilityGrantMessage{
                LocalCapabilities_.bits()}))) {
        return false;
    }
    ++Stats_.CapabilityGrantsSent;
    return true;
}

bool PeerSession::PublishClipboardHelloLocked() {
    if (!Started_ || CapabilityConflict_ ||
        !ClipboardExchange_.ShouldSendHello()) {
        return false;
    }
    EnvelopeHeader Header;
    Header.session_nonce = SessionNonce_;
    Header.sequence = ReliableSequence_++;
    if (ReliableSequence_ == 0) ++ReliableSequence_;
    // Mark before synchronous delivery so two in-memory/session callbacks
    // cannot recursively echo module hellos while each send is still active.
    if (!ClipboardExchange_.MarkHelloSent() ||
        !Transport_->send_reliable(encode_packet(
            Header, ClipboardHelloMessage{}))) {
        return false;
    }
    ++Stats_.ClipboardHellosSent;
    return true;
}

void PeerSession::ReleaseIncomingDirectionLocked() noexcept {
    if (IncomingToken_) {
        (void)DirectionArbiter_.Release(*IncomingToken_);
        IncomingToken_.reset();
    }
}

void PeerSession::FailLocalDirectionsLocked() noexcept {
    if (OutgoingToken_) {
        auto Release = OutgoingCoordinator_.release_remote_focus();
        if (Release) {
            (void)Transport_->send_reliable(std::move(*Release));
        }
    }
    OutgoingCoordinator_.emergency_fail_local();
    IncomingCoordinator_.disconnect();
    OutgoingToken_.reset();
    IncomingToken_.reset();
    DirectionArbiter_.FailLocal();
}

void PeerSession::OnReliable(ByteBuffer Packet) {
    bool NotifyFocusReady = false;
    bool NotifyDirectionChanged = false;
    bool NotifyCollision = false;
    std::function<void()> DirectionCollisionHandler;
    std::function<void()> DirectionChangedHandler;
    std::function<void()> OutgoingFocusReadyHandler;
    {
        std::scoped_lock Lock(Mutex_);
        if (!Started_) return;
        ++Stats_.reliable_received;
        const auto PeekedType = PeekMessageType(Packet);
        const bool TopologyEnvelope = PeekedType ==
            MessageType::DisplayTopologySnapshot;
        const bool ClipboardEnvelope =
            PeekedType == MessageType::ClipboardHello ||
            PeekedType == MessageType::ClipboardText;
        auto Decoded = decode_packet(Packet, false);
        if (!Decoded.packet) {
            ++Stats_.decode_rejected;
            if (TopologyEnvelope) {
                ++Stats_.TopologyRejected;
                TopologyExchange_.RejectMalformed();
            }
            if (ClipboardEnvelope) ++Stats_.ClipboardRejected;
            return;
        }
        if (!ValidateSession(*Decoded.packet)) {
            if (TopologyEnvelope) {
                ++Stats_.TopologyRejected;
                TopologyExchange_.RejectMalformed();
            }
            if (ClipboardEnvelope) ++Stats_.ClipboardRejected;
            return;
        }

        const auto Type = Decoded.packet->header.type;
        if (Type == MessageType::CapabilityGrant) {
            ++Stats_.CapabilityGrantsReceived;
            const auto Bits = std::get<CapabilityGrantMessage>(
                Decoded.packet->message).capabilities;
            const bool InvalidBits = (Bits & ~kKnownCapabilityBits) != 0;
            const bool ChangedGrant = RemoteCapabilities_ &&
                RemoteCapabilities_->bits() != Bits;
            if (InvalidBits || ChangedGrant || CapabilityConflict_) {
                ++Stats_.CapabilityGrantsRejected;
                if (!CapabilityConflict_) {
                    CapabilityConflict_ = true;
                    RemoteCapabilities_.reset();
                    ClipboardExchange_.SetRemoteCapabilities(std::nullopt);
                    FailLocalDirectionsLocked();
                    NotifyDirectionChanged = true;
                }
            } else {
                const bool FirstGrant = !RemoteCapabilities_;
                RemoteCapabilities_ = CapabilitySet(Bits);
                ClipboardExchange_.SetRemoteCapabilities(
                    RemoteCapabilities_);
                if (FirstGrant) {
                    (void)PublishCapabilityGrantLocked();
                    (void)PublishClipboardHelloLocked();
                    NotifyDirectionChanged = true;
                }
            }
        } else if (Type == MessageType::DisplayTopologySnapshot) {
            ++Stats_.TopologyReceived;
            if (TopologyExchange_.Admit(
                    Decoded.packet->header,
                    std::get<DisplayTopologySnapshotMessage>(
                        Decoded.packet->message))) {
                ++Stats_.TopologyAccepted;
            } else {
                ++Stats_.TopologyRejected;
            }
            return;
        } else if (Type == MessageType::ClipboardHello) {
            ++Stats_.ClipboardHellosReceived;
            if (ClipboardExchange_.AdmitHello(
                    std::get<ClipboardHelloMessage>(
                        Decoded.packet->message)) !=
                    ClipboardAdmission::Accepted) {
                ++Stats_.ClipboardRejected;
            } else {
                (void)PublishClipboardHelloLocked();
            }
            return;
        } else if (Type == MessageType::ClipboardText) {
            ++Stats_.ClipboardReceived;
            const auto& Message = std::get<ClipboardTextMessage>(
                Decoded.packet->message);
            if (ClipboardExchange_.AdmitText(
                    Decoded.packet->header.session_nonce, Message) !=
                    ClipboardAdmission::Accepted ||
                !ClipboardOptions_.ApplyText) {
                ++Stats_.ClipboardRejected;
                return;
            }
            bool Applied = false;
            try {
                Applied = ClipboardOptions_.ApplyText(Message);
            } catch (...) {
                Applied = false;
            }
            if (Applied) ++Stats_.ClipboardApplied;
            else ++Stats_.ClipboardRejected;
            return;
        }
        if (Type == MessageType::FocusReady) {
            if (!OutgoingToken_ ||
                DirectionArbiter_.State() !=
                    PeerDirectionState::OutgoingPending ||
                !OutgoingCoordinator_.accept_focus_ready(
                    *Decoded.packet) ||
                !DirectionArbiter_.AdmitOutgoing(
                    *OutgoingToken_)) {
                ++Stats_.authorization_rejected;
                return;
            }
            ++Stats_.OutgoingFocusAccepted;
            NotifyFocusReady = true;
            NotifyDirectionChanged = true;
        } else if (Type == MessageType::FocusRequest) {
            const auto Direction = DirectionArbiter_.BeginIncoming();
            if (Direction.Outcome ==
                    PeerDirectionOutcome::CollisionFailLocal) {
                ++Stats_.DirectionCollisions;
                FailLocalDirectionsLocked();
                NotifyCollision = true;
                NotifyDirectionChanged = true;
            } else if (Direction.Outcome !=
                           PeerDirectionOutcome::Admitted ||
                       !Direction.Token) {
                ++Stats_.DirectionRejected;
            } else {
                IncomingToken_ = *Direction.Token;
                const auto Decision = IncomingCoordinator_.handle(
                    *Decoded.packet);
                CountDecision(Decision);
                if (Decision != AgentDecision::Accepted) {
                    ReleaseIncomingDirectionLocked();
                    ++Stats_.DirectionRejected;
                } else {
                    const auto& Request = std::get<FocusRequestMessage>(
                        Decoded.packet->message);
                    EnvelopeHeader Response;
                    Response.session_nonce = SessionNonce_;
                    Response.epoch =
                        IncomingCoordinator_.focus_state().epoch();
                    Response.sequence = ReliableSequence_++;
                    if (ReliableSequence_ == 0) ++ReliableSequence_;
                    if (!Transport_->send_reliable(encode_packet(
                            Response, FocusReadyMessage{
                                EffectiveLease(
                                    Request.requested_lease_ms),
                                Request.request_id}))) {
                        IncomingCoordinator_.disconnect();
                        ReleaseIncomingDirectionLocked();
                        ++Stats_.DirectionRejected;
                    } else {
                        ++Stats_.IncomingFocusAccepted;
                        NotifyDirectionChanged = true;
                    }
                }
            }
        } else {
            const bool RequiresIncomingDirection =
                Type == MessageType::FocusRenew ||
                Type == MessageType::FocusRelease ||
                Type == MessageType::KeyEvent ||
                Type == MessageType::MouseButton ||
                Type == MessageType::PointerPosition ||
                Type == MessageType::PointerMotion ||
                Type == MessageType::InputStateSnapshot ||
                Type == MessageType::MouseWheel;
            if (RequiresIncomingDirection &&
                DirectionArbiter_.State() !=
                    PeerDirectionState::IncomingActive) {
                ++Stats_.DirectionRejected;
                ++Stats_.authorization_rejected;
            } else {
                const auto Decision = IncomingCoordinator_.handle(
                    *Decoded.packet);
                CountDecision(Decision);
                if ((Type == MessageType::FocusRelease ||
                     Type == MessageType::SetMode) &&
                    Decision == AgentDecision::Accepted &&
                    !IncomingCoordinator_.RemoteFocused() &&
                    DirectionArbiter_.State() ==
                        PeerDirectionState::IncomingActive) {
                    ReleaseIncomingDirectionLocked();
                    NotifyDirectionChanged = true;
                }
            }
        }
        if (NotifyCollision) {
            DirectionCollisionHandler = Handlers_.DirectionCollision;
        }
        if (NotifyDirectionChanged) {
            DirectionChangedHandler = Handlers_.DirectionChanged;
        }
        if (NotifyFocusReady) {
            OutgoingFocusReadyHandler = Handlers_.OutgoingFocusReady;
        }
    }
    if (DirectionCollisionHandler) {
        DirectionCollisionHandler();
    }
    if (DirectionChangedHandler) {
        DirectionChangedHandler();
    }
    if (OutgoingFocusReadyHandler) {
        OutgoingFocusReadyHandler();
    }
}

void PeerSession::OnDatagram(ByteBuffer Packet) {
    std::scoped_lock Lock(Mutex_);
    if (!Started_) return;
    ++Stats_.datagrams_received;
    const auto PeekedType = PeekMessageType(Packet);
    auto Decoded = decode_packet(Packet, true);
    if (!Decoded.packet) {
        ++Stats_.decode_rejected;
        if (PeekedType == MessageType::AudioFrame) {
            ++Stats_.AudioRejected;
        }
        return;
    }
    if (!ValidateSession(*Decoded.packet)) {
        if (Decoded.packet->header.type == MessageType::AudioFrame) {
            ++Stats_.AudioRejected;
        }
        return;
    }
    if (Decoded.packet->header.type == MessageType::AudioFrame) {
        ++Stats_.AudioReceived;
        if (!LocalCapabilities_.contains(Capability::AudioSend) ||
            AudioReceiver_ == nullptr ||
            !AudioReceiver_->Push(
                Decoded.packet->header.sequence,
                std::get<AudioFrameMessage>(
                    std::move(Decoded.packet->message)))) {
            ++Stats_.authorization_rejected;
            ++Stats_.AudioRejected;
            return;
        }
        ++Stats_.AudioAccepted;
        return;
    }
    if (DirectionArbiter_.State() !=
            PeerDirectionState::IncomingActive) {
        ++Stats_.DirectionRejected;
        ++Stats_.authorization_rejected;
        return;
    }
    CountDecision(IncomingCoordinator_.handle(*Decoded.packet));
}

} // namespace desklink
