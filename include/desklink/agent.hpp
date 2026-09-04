#pragma once

#include "desklink/capabilities.hpp"
#include "desklink/focus.hpp"
#include "desklink/input.hpp"

#include <optional>
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
    RejectedInputUnavailable,
    Ignored,
};

class AgentCoordinator {
public:
    AgentCoordinator(const IClock& clock, IInputInjector& injector) noexcept;

    void set_peer_capabilities(CapabilitySet capabilities) noexcept;
    [[nodiscard]] CapabilitySet peer_capabilities() const noexcept { return peer_capabilities_; }
    [[nodiscard]] InputFocusStateMachine& focus_state() noexcept { return focus_; }
    [[nodiscard]] DeskMode DesiredMode() const noexcept { return focus_.mode(); }
    [[nodiscard]] bool RemoteFocused() const noexcept {
        return focus_.focus() == FocusLocation::Remote && focus_.lease_active();
    }
    [[nodiscard]] bool InputCleanupPending() const noexcept {
        return InputCleanupPending_;
    }
    [[nodiscard]] bool InputUnavailable() const noexcept {
        return InputUnavailable_;
    }

    [[nodiscard]] AgentDecision handle(const DecodedPacket& packet);
    [[nodiscard]] std::optional<PointerPositionMessage>
    CurrentPointerPosition();
    void SetLocalDesiredMode(DeskMode Mode) noexcept;
    void tick() noexcept;
    void disconnect() noexcept;

private:
    [[nodiscard]] bool can_inject() const noexcept;
    void SetRemoteDesiredMode(DeskMode Mode) noexcept;
    void ApplyDesiredMode() noexcept;
    [[nodiscard]] bool ReleaseOwnedState() noexcept;
    [[nodiscard]] AgentDecision RejectInputUnavailable() noexcept;

    IInputInjector& injector_;
    CapabilitySet peer_capabilities_;
    InputFocusStateMachine focus_;
    DeskMode LocalDesiredMode_{DeskMode::Roam};
    DeskMode RemoteDesiredMode_{DeskMode::Roam};
    std::uint64_t last_pointer_sequence_{};
    bool InputCleanupPending_{};
    bool InputUnavailable_{};
};

} // namespace desklink
