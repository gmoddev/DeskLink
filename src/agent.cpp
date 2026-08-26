#include "desklink/agent.hpp"

#include <algorithm>
#include <chrono>

namespace desklink {
namespace {

std::chrono::milliseconds clamp_lease(std::uint32_t requested) {
    constexpr std::uint32_t min_ms = 100;
    constexpr std::uint32_t max_ms = 2000;
    return std::chrono::milliseconds(std::clamp(requested, min_ms, max_ms));
}

} // namespace

AgentCoordinator::AgentCoordinator(const IClock& clock, IInputInjector& injector) noexcept
    : injector_(injector), focus_(clock) {}

void AgentCoordinator::set_peer_capabilities(CapabilitySet capabilities) noexcept {
    peer_capabilities_ = capabilities;
    if (!can_inject()) {
        focus_.release_remote_focus();
        (void)ReleaseOwnedState();
    }
}

bool AgentCoordinator::can_inject() const noexcept {
    return peer_capabilities_.contains(Capability::InputInject);
}

AgentDecision AgentCoordinator::handle(const DecodedPacket& packet) {
    const auto type = packet.header.type;

    if (type == MessageType::SetMode) {
        if (!can_inject()) return AgentDecision::RejectedCapability;
        SetRemoteDesiredMode(std::get<SetModeMessage>(packet.message).mode);
        return AgentDecision::Accepted;
    }

    if (type == MessageType::FocusRequest) {
        if (!can_inject()) return AgentDecision::RejectedCapability;
        const auto& request = std::get<FocusRequestMessage>(packet.message);
        if (focus_.focus() == FocusLocation::Remote && !ReleaseOwnedState()) {
            focus_.release_remote_focus();
            return AgentDecision::RejectedMalformed;
        }
        if (InputCleanupPending_) return AgentDecision::RejectedLease;
        const auto new_epoch = focus_.begin_remote_focus(clamp_lease(request.requested_lease_ms));
        if (new_epoch != 0) last_pointer_sequence_ = 0;
        return new_epoch == 0 ? AgentDecision::RejectedLease : AgentDecision::Accepted;
    }

    if (type == MessageType::FocusRenew) {
        if (!can_inject()) return AgentDecision::RejectedCapability;
        const auto& renew = std::get<FocusRenewMessage>(packet.message);
        if (packet.header.epoch != focus_.epoch()) return AgentDecision::RejectedEpoch;
        return focus_.renew(packet.header.epoch, clamp_lease(renew.requested_lease_ms))
            ? AgentDecision::Accepted
            : AgentDecision::RejectedLease;
    }

    if (type == MessageType::FocusRelease) {
        if (!can_inject()) return AgentDecision::RejectedCapability;
        if (packet.header.epoch != focus_.epoch()) return AgentDecision::RejectedEpoch;
        focus_.release_remote_focus();
        return ReleaseOwnedState()
            ? AgentDecision::Accepted : AgentDecision::RejectedMalformed;
    }

    if (type == MessageType::KeyEvent || type == MessageType::MouseButton ||
        type == MessageType::PointerPosition || type == MessageType::PointerMotion ||
        type == MessageType::InputStateSnapshot ||
        type == MessageType::MouseWheel) {
        if (!can_inject()) return AgentDecision::RejectedCapability;
        if (InputCleanupPending_) return AgentDecision::RejectedLease;
        if (packet.header.epoch != focus_.epoch()) return AgentDecision::RejectedEpoch;
        if (!focus_.accepts_remote_input(packet.header.epoch)) return AgentDecision::RejectedLease;

        switch (type) {
            case MessageType::KeyEvent:
                return injector_.inject_key(std::get<KeyEventMessage>(packet.message))
                    ? AgentDecision::Accepted : AgentDecision::RejectedMalformed;
            case MessageType::MouseButton:
                return injector_.inject_button(std::get<MouseButtonMessage>(packet.message))
                    ? AgentDecision::Accepted : AgentDecision::RejectedMalformed;
            case MessageType::PointerPosition:
            case MessageType::PointerMotion:
                if (packet.header.sequence <= last_pointer_sequence_) {
                    return AgentDecision::RejectedSequence;
                }
                if (type == MessageType::PointerPosition &&
                    !injector_.inject_pointer(
                        std::get<PointerPositionMessage>(packet.message))) {
                    return AgentDecision::RejectedMalformed;
                }
                if (type == MessageType::PointerMotion &&
                    !injector_.InjectPointerMotion(
                        std::get<PointerMotionMessage>(packet.message))) {
                    return AgentDecision::RejectedMalformed;
                }
                last_pointer_sequence_ = packet.header.sequence;
                return AgentDecision::Accepted;
            case MessageType::InputStateSnapshot:
                return injector_.ReconcileState(
                    std::get<InputStateSnapshotMessage>(packet.message))
                    ? AgentDecision::Accepted : AgentDecision::RejectedMalformed;
            case MessageType::MouseWheel:
                return injector_.InjectWheel(
                    std::get<MouseWheelMessage>(packet.message))
                    ? AgentDecision::Accepted : AgentDecision::RejectedMalformed;
            default:
                break;
        }
    }

    return AgentDecision::Ignored;
}

void AgentCoordinator::SetLocalDesiredMode(DeskMode Mode) noexcept {
    LocalDesiredMode_ = Mode;
    ApplyDesiredMode();
}

void AgentCoordinator::SetRemoteDesiredMode(DeskMode Mode) noexcept {
    RemoteDesiredMode_ = Mode;
    ApplyDesiredMode();
}

void AgentCoordinator::ApplyDesiredMode() noexcept {
    const auto EffectiveMode = LocalDesiredMode_ == DeskMode::Roam
        ? RemoteDesiredMode_ : LocalDesiredMode_;
    const bool WasRemote = focus_.focus() == FocusLocation::Remote;
    focus_.set_mode(EffectiveMode);
    if (WasRemote && focus_.focus() == FocusLocation::Local) {
        (void)ReleaseOwnedState();
    }
}

void AgentCoordinator::tick() noexcept {
    if (InputCleanupPending_) (void)ReleaseOwnedState();
    if (focus_.poll_expiry()) {
        (void)ReleaseOwnedState();
    }
}

void AgentCoordinator::disconnect() noexcept {
    focus_.release_remote_focus();
    last_pointer_sequence_ = 0;
    (void)ReleaseOwnedState();
}

bool AgentCoordinator::ReleaseOwnedState() noexcept {
    InputCleanupPending_ = !injector_.release_owned_state();
    return !InputCleanupPending_;
}

} // namespace desklink
