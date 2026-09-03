#pragma once

#include "desklink/agent.hpp"
#include "desklink/audio.hpp"
#include "desklink/callback_gate.hpp"
#include "desklink/host.hpp"
#include "desklink/pairing.hpp"
#include "desklink/roaming_runtime.hpp"
#include "desklink/topology_exchange.hpp"
#include "desklink/transport.hpp"
#ifdef DESKLINK_BUILD_VOICE
#include "desklink/voice.hpp"
#endif

#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

namespace desklink {

struct SessionStats {
    std::uint64_t reliable_received{};
    std::uint64_t datagrams_received{};
    std::uint64_t decode_rejected{};
    std::uint64_t session_rejected{};
    std::uint64_t authorization_rejected{};
    std::uint64_t AudioSent{};
    std::uint64_t AudioSendRejected{};
    std::uint64_t AudioReceived{};
    std::uint64_t AudioAccepted{};
    std::uint64_t AudioRejected{};
    std::uint64_t VoiceSent{};
    std::uint64_t VoiceSendRejected{};
    std::uint64_t VoiceReceived{};
    std::uint64_t VoiceAccepted{};
    std::uint64_t VoiceRejected{};
    std::uint64_t TopologySent{};
    std::uint64_t TopologySendRejected{};
    std::uint64_t TopologyReceived{};
    std::uint64_t TopologyAccepted{};
    std::uint64_t TopologyRejected{};
    std::uint64_t DisplayIdentifySent{};
    std::uint64_t DisplayIdentifySendRejected{};
    std::uint64_t DisplayIdentifyReceived{};
    std::uint64_t DisplayIdentifyAccepted{};
    std::uint64_t DisplayIdentifyRejected{};
    std::uint64_t CapabilityGrantsSent{};
    std::uint64_t CapabilityGrantsReceived{};
    std::uint64_t CapabilityGrantsRejected{};
    std::uint64_t CapabilityGrantAcksSent{};
    std::uint64_t CapabilityGrantAcksReceived{};
    std::uint64_t CapabilityGrantAcksRejected{};
    std::uint64_t ClipboardHellosSent{};
    std::uint64_t ClipboardHellosReceived{};
    std::uint64_t ClipboardSent{};
    std::uint64_t ClipboardSendRejected{};
    std::uint64_t ClipboardReceived{};
    std::uint64_t ClipboardApplied{};
    std::uint64_t ClipboardRejected{};
    std::uint64_t DirectionRejected{};
    std::uint64_t DirectionCollisions{};
    std::uint64_t IncomingFocusAccepted{};
    std::uint64_t OutgoingFocusAccepted{};
    std::uint64_t ClockSyncSent{};
    std::uint64_t ClockSyncReceived{};
    std::uint64_t ClockSyncRejected{};
};

struct DisplayTopologyExchangeOptions {
    bool Enabled{};
    const IClock* Clock{};
};

struct ClipboardSessionOptions {
    bool Enabled{};
    MachineId LocalMachine{};
    const IClock* Clock{};
    std::function<bool(ClipboardTextMessage)> ApplyText;
};

struct LatencyDiagnosticOptions {
    bool Enabled{};
    const IClock* Clock{};
    std::function<void(
        const ClockSyncResponseMessage&, std::uint64_t)> ClockSample;
    std::function<void(
        std::uint64_t, const AudioFrameMessage&, std::uint64_t)>
        AudioFrameArrival;
};

class AgentSession {
public:
    AgentSession(std::shared_ptr<ITransportEndpoint> transport,
                 AgentCoordinator& coordinator,
                 const ITrustStore& TrustStore,
                 std::uint64_t session_nonce,
                 DisplayTopologyExchangeOptions TopologyOptions = {}) noexcept;
    ~AgentSession();

    [[nodiscard]] bool start();
    void stop() noexcept;
    void tick() noexcept;
    void SetLocalDesiredMode(DeskMode Mode) noexcept;
    [[nodiscard]] DeskMode DesiredMode() const noexcept;
    [[nodiscard]] bool RemoteFocused() const noexcept;
    [[nodiscard]] bool CanSendAudio() const noexcept;
    [[nodiscard]] bool PeerHasCapability(Capability Value) const noexcept;
    [[nodiscard]] bool SendAudioFrame(AudioFrameMessage Frame);
    [[nodiscard]] bool PublishDisplayTopology(
        const MachineId& LocalMachine,
        const DisplayTopologySnapshot& Topology);
    [[nodiscard]] DisplayTopologyExchangeStatus
    DisplayTopologyStatus() const noexcept;
    [[nodiscard]] std::optional<DisplayTopologySnapshot>
    RemoteDisplayTopology() const;
    [[nodiscard]] SessionStats stats() const noexcept;

private:
    void on_reliable(ByteBuffer packet);
    void on_datagram(ByteBuffer packet);
    [[nodiscard]] bool validate_session(const DecodedPacket& packet) noexcept;
    void count_decision(AgentDecision decision) noexcept;

    std::shared_ptr<ITransportEndpoint> transport_;
    std::shared_ptr<CallbackGate> CallbackGate_{std::make_shared<CallbackGate>()};
    AgentCoordinator& coordinator_;
    const ITrustStore& trust_store_;
    std::uint64_t session_nonce_{};
    std::uint64_t response_sequence_{1};
    std::uint64_t AudioDatagramSequence_{1};
    CapabilitySet PeerCapabilities_;
    DisplayTopologyExchangeOptions TopologyOptions_;
    DisplayTopologyExchangeTracker TopologyExchange_;
    std::optional<MachineId> LocalTopologyMachine_;
    std::uint64_t TopologySequence_{1};
    SessionStats stats_;
    mutable std::recursive_mutex Mutex_;
    bool started_{};
};

class HostSession {
public:
    HostSession(std::shared_ptr<ITransportEndpoint> transport,
                HostCoordinator& coordinator,
                const ITrustStore& TrustStore,
                std::uint64_t session_nonce,
                std::function<void()> FocusReadyHandler = {},
                AudioReceiver* Receiver = nullptr,
                DisplayTopologyExchangeOptions TopologyOptions = {}) noexcept;
    ~HostSession();

    [[nodiscard]] bool start();
    void stop() noexcept;

    [[nodiscard]] bool focus_remote(std::uint32_t lease_ms = 750);
    [[nodiscard]] bool SetDesiredMode(DeskMode Mode);
    [[nodiscard]] bool renew_focus(std::uint32_t lease_ms = 750);
    [[nodiscard]] bool release_focus();
    [[nodiscard]] bool send_key(KeyEventMessage event);
    [[nodiscard]] bool send_button(MouseButtonMessage event);
    [[nodiscard]] bool send_pointer(PointerPositionMessage event);
    [[nodiscard]] bool SendPointerMotion(PointerMotionMessage Message);
    [[nodiscard]] bool SendWheel(MouseWheelMessage Message);
    [[nodiscard]] bool SendInputStateSnapshot();
    [[nodiscard]] bool RemoteFocused() const noexcept;
    [[nodiscard]] DeskMode DesiredMode() const noexcept;
    [[nodiscard]] bool CanReceiveAudio() const noexcept;
    [[nodiscard]] bool PeerHasCapability(Capability Value) const noexcept;
    [[nodiscard]] bool PublishDisplayTopology(
        const MachineId& LocalMachine,
        const DisplayTopologySnapshot& Topology);
    [[nodiscard]] DisplayTopologyExchangeStatus
    DisplayTopologyStatus() const noexcept;
    [[nodiscard]] std::optional<DisplayTopologySnapshot>
    RemoteDisplayTopology() const;

    [[nodiscard]] SessionStats stats() const noexcept;

private:
    void on_reliable(ByteBuffer packet);
    void on_datagram(ByteBuffer packet);

    std::shared_ptr<ITransportEndpoint> transport_;
    std::shared_ptr<CallbackGate> CallbackGate_{std::make_shared<CallbackGate>()};
    HostCoordinator& coordinator_;
    const ITrustStore& trust_store_;
    std::uint64_t session_nonce_{};
    std::function<void()> FocusReadyHandler_;
    AudioReceiver* AudioReceiver_{};
    CapabilitySet PeerCapabilities_;
    DisplayTopologyExchangeOptions TopologyOptions_;
    DisplayTopologyExchangeTracker TopologyExchange_;
    std::optional<MachineId> LocalTopologyMachine_;
    std::uint64_t TopologySequence_{1};
    SessionStats stats_;
    mutable std::recursive_mutex Mutex_;
    bool started_{};
};

struct PeerSessionHandlers {
    std::function<void()> OutgoingFocusReady;
    std::function<void()> DirectionChanged;
    std::function<void()> DirectionCollision;
    std::function<void(TransportCloseReason)> TransportClosed;
    std::function<void(PointerPositionFeedbackMessage)>
        RemotePointerFeedback;
    // Must return promptly. The runtime owns any overlay worker lifetime.
    std::function<void(std::uint16_t)> IdentifyDisplays;
#ifdef DESKLINK_BUILD_VOICE
    // Signals that reciprocal voice admission may have changed. The runtime
    // must re-check CanSendVoice/CanReceiveVoice and stop capture on loss.
    std::function<void()> VoiceAuthorizationChanged;
#endif
};

// Owns both directions of one authenticated peer connection. Capability grants
// are exchanged after transport validation and describe only the persisted
// local trust decision; they never mutate either trust store.
class PeerSession final {
public:
    PeerSession(std::shared_ptr<ITransportEndpoint> Transport,
                HostCoordinator& OutgoingCoordinator,
                AgentCoordinator& IncomingCoordinator,
                const ITrustStore& TrustStore,
                std::uint64_t SessionNonce,
                PeerSessionHandlers Handlers = {},
                AudioReceiver* Receiver = nullptr,
                DisplayTopologyExchangeOptions TopologyOptions = {},
                ClipboardSessionOptions ClipboardOptions = {},
                LatencyDiagnosticOptions LatencyOptions = {}
#ifdef DESKLINK_BUILD_VOICE
                , VoiceReceiver* VoiceReceiver = nullptr
#endif
                ) noexcept;
    ~PeerSession();

    [[nodiscard]] bool Start();
    void Stop() noexcept;
    void Tick() noexcept;
    void FailLocalDirections() noexcept;

    // Reloads the already-authenticated peer's exact persisted trust record,
    // returns both directions Local, and waits for an ordered session-scoped
    // acknowledgement before reporting success. No identity or capability
    // data is accepted from the caller.
    [[nodiscard]] bool RefreshLocalCapabilities(
        std::chrono::milliseconds Timeout = std::chrono::milliseconds{1'500});
    void SetClipboardEnabled(bool Enabled) noexcept;

    void SetLocalDesiredMode(DeskMode Mode) noexcept;
    [[nodiscard]] DeskMode IncomingDesiredMode() const noexcept;
    [[nodiscard]] bool BeginOutgoingFocus(
        std::uint32_t LeaseMilliseconds = 750);
    [[nodiscard]] bool SetDesiredMode(DeskMode Mode);
    [[nodiscard]] bool RenewOutgoingFocus(
        std::uint32_t LeaseMilliseconds = 750);
    [[nodiscard]] bool ReleaseOutgoingFocus();
    [[nodiscard]] bool SendKey(KeyEventMessage Event);
    [[nodiscard]] bool SendButton(MouseButtonMessage Event);
    [[nodiscard]] bool SendPointer(PointerPositionMessage Event);
    [[nodiscard]] bool SendPointerMotion(PointerMotionMessage Message);
    [[nodiscard]] bool SendWheel(MouseWheelMessage Message);
    [[nodiscard]] bool SendInputStateSnapshot();

    [[nodiscard]] bool OutgoingFocused() const noexcept;
    [[nodiscard]] bool IncomingFocused() const noexcept;
    [[nodiscard]] PeerDirectionState DirectionState() const noexcept;
    [[nodiscard]] bool CanBeginOutgoing() const noexcept;
    [[nodiscard]] bool PeerGrantedCapability(Capability Value) const noexcept;
    [[nodiscard]] bool GrantedToPeer(Capability Value) const noexcept;

    [[nodiscard]] bool CanSendAudio() const noexcept;
    [[nodiscard]] bool CanReceiveAudio() const noexcept;
    [[nodiscard]] bool SendAudioFrame(AudioFrameMessage Frame);
#ifdef DESKLINK_BUILD_VOICE
    [[nodiscard]] bool CanSendVoice() const noexcept;
    [[nodiscard]] bool CanReceiveVoice() const noexcept;
    [[nodiscard]] bool SendVoiceFrame(VoiceFrameMessage Frame);
#endif
    [[nodiscard]] bool SendClockSyncProbe(std::uint64_t ProbeId);
    [[nodiscard]] bool CanSendClipboard() const noexcept;
    [[nodiscard]] bool CanReceiveClipboard() const noexcept;
    [[nodiscard]] bool PublishClipboardText(std::string Text);
    [[nodiscard]] bool PublishDisplayTopology(
        const MachineId& LocalMachine,
        const DisplayTopologySnapshot& Topology);
    [[nodiscard]] bool RequestPeerDisplayIdentification(
        std::uint16_t FirstDisplayNumber);
    [[nodiscard]] DisplayTopologyExchangeStatus
    DisplayTopologyStatus() const noexcept;
    [[nodiscard]] std::optional<DisplayTopologySnapshot>
    RemoteDisplayTopology() const;
    [[nodiscard]] SessionStats Stats() const noexcept;

private:
    void OnReliable(ByteBuffer Packet);
    void OnDatagram(ByteBuffer Packet);
    [[nodiscard]] bool ValidateSession(
        const DecodedPacket& Packet) noexcept;
    void CountDecision(AgentDecision Decision) noexcept;
    [[nodiscard]] bool PublishCapabilityGrantLocked();
    [[nodiscard]] bool PublishCapabilityGrantAckLocked(
        std::uint64_t Capabilities, std::uint64_t Revision);
    [[nodiscard]] bool PublishClipboardHelloLocked();
    void ReleaseIncomingDirectionLocked() noexcept;
    void FailLocalDirectionsLocked() noexcept;

    std::shared_ptr<ITransportEndpoint> Transport_;
    std::shared_ptr<CallbackGate> CallbackGate_{std::make_shared<CallbackGate>()};
    HostCoordinator& OutgoingCoordinator_;
    AgentCoordinator& IncomingCoordinator_;
    const ITrustStore& TrustStore_;
    std::uint64_t SessionNonce_{};
    PeerSessionHandlers Handlers_;
    AudioReceiver* AudioReceiver_{};
#ifdef DESKLINK_BUILD_VOICE
    VoiceReceiver* VoiceReceiver_{};
#endif
    CapabilitySet LocalCapabilities_;
    std::optional<CapabilitySet> RemoteCapabilities_;
    DisplayTopologyExchangeOptions TopologyOptions_;
    DisplayTopologyExchangeTracker TopologyExchange_;
    ClipboardSessionOptions ClipboardOptions_;
    ClipboardExchange ClipboardExchange_;
    LatencyDiagnosticOptions LatencyOptions_;
    std::map<std::uint64_t, std::uint64_t> PendingClockProbes_;
    std::optional<MachineId> LocalTopologyMachine_;
    PeerDirectionArbiter DirectionArbiter_;
    std::optional<PeerDirectionToken> OutgoingToken_;
    std::optional<PeerDirectionToken> IncomingToken_;
    std::uint64_t ReliableSequence_{1};
    std::uint64_t AudioDatagramSequence_{1};
#ifdef DESKLINK_BUILD_VOICE
    std::uint64_t VoiceDatagramSequence_{1};
#endif
    std::uint64_t PointerFeedbackSequence_{1};
    std::uint64_t LastPointerFeedbackSequence_{};
    std::uint64_t TopologySequence_{1};
    std::uint64_t LocalCapabilityRevision_{1};
    std::uint64_t RemoteCapabilityRevision_{};
    std::uint64_t AcknowledgedLocalCapabilityRevision_{};
    SessionStats Stats_;
    MachineId PeerMachine_{};
    mutable std::recursive_mutex Mutex_;
    std::condition_variable_any CapabilityChanged_;
    bool CapabilityConflict_{};
    bool Started_{};
};

} // namespace desklink
