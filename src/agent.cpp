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
    : injector_(injector), Clock_(clock), focus_(clock) {}

AgentCoordinator::AgentCoordinator(
    const IClock& Clock, IInputInjector& Injector,
    IPrivilegedInputBroker* PrivilegedInput) noexcept
    : injector_(Injector), PrivilegedInput_(PrivilegedInput), Clock_(Clock),
      focus_(Clock) {}

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
        const bool DesktopAvailable = injector_.InputDesktopAvailable();
        const bool OrdinaryInputReady = DesktopAvailable &&
            injector_.ReadyForInput();
        const auto& request = std::get<FocusRequestMessage>(packet.message);
        if (focus_.focus() == FocusLocation::Remote && !ReleaseOwnedState()) {
            focus_.release_remote_focus();
            return AgentDecision::RejectedMalformed;
        }
        if (InputCleanupPending_) return AgentDecision::RejectedLease;
        const auto new_epoch = focus_.begin_remote_focus(clamp_lease(request.requested_lease_ms));
        if (new_epoch != 0) {
            last_pointer_sequence_ = 0;
            InputUnavailable_ = false;
            const bool PrivilegedReady = PrivilegedInput_ &&
                PrivilegedInput_->Begin(
                    new_epoch, clamp_lease(request.requested_lease_ms));
            if (!OrdinaryInputReady && !PrivilegedReady) {
                focus_.release_remote_focus();
                return DesktopAvailable
                    ? RejectInputUnavailable()
                    : AgentDecision::RejectedLease;
            }
        }
        return new_epoch == 0 ? AgentDecision::RejectedLease : AgentDecision::Accepted;
    }

    if (type == MessageType::FocusRenew) {
        if (!can_inject()) return AgentDecision::RejectedCapability;
        const auto& renew = std::get<FocusRenewMessage>(packet.message);
        if (packet.header.epoch != focus_.epoch()) return AgentDecision::RejectedEpoch;
        const auto Lease = clamp_lease(renew.requested_lease_ms);
        if (!focus_.renew(packet.header.epoch, Lease)) {
            return AgentDecision::RejectedLease;
        }
        if (PrivilegedInput_ && PrivilegedInput_->Authorized()) {
            (void)PrivilegedInput_->Renew(packet.header.epoch, Lease);
        }
        return AgentDecision::Accepted;
    }

    if (type == MessageType::FocusRelease) {
        if (!can_inject()) return AgentDecision::RejectedCapability;
        if (packet.header.epoch != focus_.epoch()) return AgentDecision::RejectedEpoch;
        focus_.release_remote_focus();
        if (!ReleaseOwnedState()) return AgentDecision::RejectedMalformed;
        (void)injector_.ParkPointer();
        return AgentDecision::Accepted;
    }

    if (type == MessageType::KeyEvent || type == MessageType::MouseButton ||
        type == MessageType::PointerPosition || type == MessageType::PointerMotion ||
        type == MessageType::InputStateSnapshot ||
        type == MessageType::MouseWheel) {
        if (!can_inject()) return AgentDecision::RejectedCapability;
        if (InputCleanupPending_) return AgentDecision::RejectedLease;
        if (packet.header.epoch != focus_.epoch()) return AgentDecision::RejectedEpoch;
        if (!focus_.accepts_remote_input(packet.header.epoch)) return AgentDecision::RejectedLease;

        const bool DesktopAvailable = injector_.InputDesktopAvailable();
        const bool OrdinaryInputReady = DesktopAvailable &&
            injector_.ReadyForInput();
        if (!OrdinaryInputReady) {
            if (PrivilegedInput_ && PrivilegedInput_->Authorized() &&
                PrivilegedInput_->Forward(packet)) {
                return AgentDecision::Accepted;
            }
            // A secure desktop remains a temporary pause when the separately
            // approved privileged path is absent or unavailable. A Default-
            // desktop integrity boundary still fails Local.
            return DesktopAvailable
                ? RejectInputUnavailable()
                : AgentDecision::RejectedLease;
        }

        switch (type) {
            case MessageType::KeyEvent:
                return injector_.inject_key(std::get<KeyEventMessage>(packet.message))
                    ? AgentDecision::Accepted : RejectInputUnavailable();
            case MessageType::MouseButton:
                return injector_.inject_button(std::get<MouseButtonMessage>(packet.message))
                    ? AgentDecision::Accepted : RejectInputUnavailable();
            case MessageType::PointerPosition:
            case MessageType::PointerMotion:
                if (packet.header.sequence <= last_pointer_sequence_) {
                    return AgentDecision::RejectedSequence;
                }
                if (type == MessageType::PointerPosition &&
                    !injector_.inject_pointer(
                        std::get<PointerPositionMessage>(packet.message))) {
                    return RejectInputUnavailable();
                }
                if (type == MessageType::PointerMotion &&
                    !injector_.InjectPointerMotion(
                        std::get<PointerMotionMessage>(packet.message))) {
                    return RejectInputUnavailable();
                }
                last_pointer_sequence_ = packet.header.sequence;
                return AgentDecision::Accepted;
            case MessageType::InputStateSnapshot:
                return injector_.ReconcileState(
                    std::get<InputStateSnapshotMessage>(packet.message))
                    ? AgentDecision::Accepted : RejectInputUnavailable();
            case MessageType::MouseWheel:
                return injector_.InjectWheel(
                    std::get<MouseWheelMessage>(packet.message))
                    ? AgentDecision::Accepted : RejectInputUnavailable();
            default:
                break;
        }
    }

    return AgentDecision::Ignored;
}

std::optional<PointerPositionMessage>
AgentCoordinator::CurrentPointerPosition() {
    if (!RemoteFocused() || InputCleanupPending_) return std::nullopt;
    return injector_.CurrentPointerPosition();
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
    constexpr auto AvailabilityCheckInterval = std::chrono::milliseconds(50);
    const auto Now = Clock_.now();
    if (RemoteFocused() && Now >= NextInputAvailabilityCheck_) {
        NextInputAvailabilityCheck_ = Now + AvailabilityCheckInterval;
        // A secure/non-Default desktop is handled as a temporary input pause.
        // An unusable foreground on the ordinary Default desktop (for example,
        // a higher-integrity Task Manager) cannot receive SendInput and must
        // revoke remote focus instead of leaving the controller suppressed.
        if (injector_.InputDesktopAvailable() &&
            !injector_.ReadyForInput() &&
            !(PrivilegedInput_ && PrivilegedInput_->Authorized())) {
            (void)RejectInputUnavailable();
        }
    }
    if (focus_.poll_expiry()) {
        (void)ReleaseOwnedState();
    }
}

void AgentCoordinator::disconnect() noexcept {
    focus_.release_remote_focus();
    last_pointer_sequence_ = 0;
    (void)ReleaseOwnedState();
    if (PrivilegedInput_) PrivilegedInput_->Revoke();
    InputUnavailable_ = false;
    NextInputAvailabilityCheck_ = {};
}

AgentDecision AgentCoordinator::RejectInputUnavailable() noexcept {
    focus_.release_remote_focus();
    last_pointer_sequence_ = 0;
    InputUnavailable_ = true;
    (void)ReleaseOwnedState();
    return AgentDecision::RejectedInputUnavailable;
}

bool AgentCoordinator::ReleaseOwnedState() noexcept {
    const bool OrdinaryReleased = injector_.release_owned_state();
    const bool PrivilegedReleased = !PrivilegedInput_ ||
        !PrivilegedInput_->Authorized() || PrivilegedInput_->Release();
    InputCleanupPending_ = !OrdinaryReleased || !PrivilegedReleased;
    return !InputCleanupPending_;
}

} // namespace desklink
