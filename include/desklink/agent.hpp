#pragma once

#include "desklink/capabilities.hpp"
#include "desklink/focus.hpp"
#include "desklink/input.hpp"
#include "desklink/protocol.hpp"

#include <string>

namespace desklink {

enum class AgentDecision {
    Accepted,
    RejectedCapability,
    RejectedLease,
    RejectedEpoch,
    RejectedSequence,
    RejectedMalformed,
    Ignored,
};

class AgentCoordinator {
public:
    AgentCoordinator(const IClock& clock, IInputInjector& injector) noexcept;

    void set_peer_capabilities(CapabilitySet capabilities) noexcept;
    [[nodiscard]] CapabilitySet peer_capabilities() const noexcept { return peer_capabilities_; }
    [[nodiscard]] InputFocusStateMachine& focus_state() noexcept { return focus_; }

    [[nodiscard]] AgentDecision handle(const DecodedPacket& packet);
    void tick() noexcept;
    void disconnect() noexcept;

private:
    [[nodiscard]] bool can_inject() const noexcept;

    IInputInjector& injector_;
    CapabilitySet peer_capabilities_;
    InputFocusStateMachine focus_;
    std::uint64_t last_pointer_sequence_{};
};

} // namespace desklink
