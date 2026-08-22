#pragma once

#include "desklink/agent.hpp"
#include "desklink/host.hpp"
#include "desklink/pairing.hpp"
#include "desklink/transport.hpp"

#include <cstdint>
#include <memory>

namespace desklink {

struct SessionStats {
    std::uint64_t reliable_received{};
    std::uint64_t datagrams_received{};
    std::uint64_t decode_rejected{};
    std::uint64_t session_rejected{};
    std::uint64_t authorization_rejected{};
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
    [[nodiscard]] const SessionStats& stats() const noexcept { return stats_; }

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
    SessionStats stats_;
    bool started_{};
};

class HostSession {
public:
    HostSession(std::shared_ptr<ITransportEndpoint> transport,
                HostCoordinator& coordinator,
                const ITrustStore& TrustStore,
                std::uint64_t session_nonce) noexcept;
    ~HostSession();

    [[nodiscard]] bool start();
    void stop() noexcept;

    [[nodiscard]] bool focus_remote(std::uint32_t lease_ms = 750);
    [[nodiscard]] bool renew_focus(std::uint32_t lease_ms = 750);
    [[nodiscard]] bool release_focus();
    [[nodiscard]] bool send_key(KeyEventMessage event);
    [[nodiscard]] bool send_button(MouseButtonMessage event);
    [[nodiscard]] bool send_pointer(PointerPositionMessage event);

    [[nodiscard]] const SessionStats& stats() const noexcept { return stats_; }

private:
    void on_reliable(ByteBuffer packet);

    std::shared_ptr<ITransportEndpoint> transport_;
    HostCoordinator& coordinator_;
    const ITrustStore& trust_store_;
    std::uint64_t session_nonce_{};
    SessionStats stats_;
    bool started_{};
};

} // namespace desklink
