#pragma once

#include "desklink/agent.hpp"
#include "desklink/audio.hpp"
#include "desklink/host.hpp"
#include "desklink/pairing.hpp"
#include "desklink/transport.hpp"

#include <cstdint>
#include <functional>
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
};

class AgentSession {
public:
    AgentSession(std::shared_ptr<ITransportEndpoint> transport,
                 AgentCoordinator& coordinator,
                 const ITrustStore& TrustStore,
                 std::uint64_t session_nonce) noexcept;
    ~AgentSession();

    [[nodiscard]] bool start();
    void stop() noexcept;
    void tick() noexcept;
    void SetLocalDesiredMode(DeskMode Mode) noexcept;
    [[nodiscard]] DeskMode DesiredMode() const noexcept;
    [[nodiscard]] bool RemoteFocused() const noexcept;
    [[nodiscard]] bool CanSendAudio() const noexcept;
    [[nodiscard]] bool SendAudioFrame(AudioFrameMessage Frame);
    [[nodiscard]] SessionStats stats() const noexcept;

private:
    void on_reliable(ByteBuffer packet);
    void on_datagram(ByteBuffer packet);
    [[nodiscard]] bool validate_session(const DecodedPacket& packet) noexcept;
    void count_decision(AgentDecision decision) noexcept;

    std::shared_ptr<ITransportEndpoint> transport_;
    AgentCoordinator& coordinator_;
    const ITrustStore& trust_store_;
    std::uint64_t session_nonce_{};
    std::uint64_t response_sequence_{1};
    std::uint64_t AudioDatagramSequence_{1};
    CapabilitySet PeerCapabilities_;
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
                AudioReceiver* Receiver = nullptr) noexcept;
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

    [[nodiscard]] SessionStats stats() const noexcept;

private:
    void on_reliable(ByteBuffer packet);
    void on_datagram(ByteBuffer packet);

    std::shared_ptr<ITransportEndpoint> transport_;
    HostCoordinator& coordinator_;
    const ITrustStore& trust_store_;
    std::uint64_t session_nonce_{};
    std::function<void()> FocusReadyHandler_;
    AudioReceiver* AudioReceiver_{};
    CapabilitySet PeerCapabilities_;
    SessionStats stats_;
    mutable std::recursive_mutex Mutex_;
    bool started_{};
};

} // namespace desklink
