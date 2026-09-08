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
            PrivilegedInputUnavailableSince_.reset();
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
        PrivilegedInputUnavailableSince_.reset();
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
            const bool SequencedPointer = type == MessageType::PointerPosition ||
                type == MessageType::PointerMotion;
            if (SequencedPointer &&
                packet.header.sequence <= last_pointer_sequence_) {
                return AgentDecision::RejectedSequence;
            }
            if (PrivilegedInput_ && PrivilegedInput_->Authorized()) {
                const auto ForwardResult = PrivilegedInput_->Forward(packet);
                if (ForwardResult == PrivilegedInputForwardResult::Forwarded) {
                    PrivilegedInputUnavailableSince_.reset();
                    if (SequencedPointer) {
                        last_pointer_sequence_ = packet.header.sequence;
                    }
                    return AgentDecision::Accepted;
                }
                if (ForwardResult ==
                    PrivilegedInputForwardResult::TemporarilyUnavailable) {
                    // Winlogon is an intentionally paused input surface for
                    // blocked operations. When Windows has already returned
                    // to Default, allow only a short helper handoff window.
                    // No application input is admitted during either pause.
                    if (!DesktopAvailable) {
                        PrivilegedInputUnavailableSince_.reset();
                        return AgentDecision::RejectedLease;
                    }
                    const auto Now = Clock_.now();
                    if (!PrivilegedInputUnavailableSince_) {
                        PrivilegedInputUnavailableSince_ = Now;
                    }
                    if (Now - *PrivilegedInputUnavailableSince_ <
                        PrivilegedInputTransitionGrace) {
                        return AgentDecision::RejectedLease;
                    }
                }
            }
            // A secure desktop remains a temporary pause when the separately
            // approved privileged path is absent or unavailable. A Default-
            // desktop integrity boundary still fails Local.
            return DesktopAvailable
                ? RejectInputUnavailable()
                : AgentDecision::RejectedLease;
        }

        PrivilegedInputUnavailableSince_.reset();

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
        // A Default-desktop target must either retain an authorized broker or
        // fail Local. An explicitly observed helper handoff gets only the
        // bounded grace started by Forward(); no input is admitted meanwhile.
        const bool DesktopAvailable = injector_.InputDesktopAvailable();
        const bool OrdinaryInputReady = DesktopAvailable &&
            injector_.ReadyForInput();
        if (!DesktopAvailable) {
            // The first broker response can race the injector's short-lived
            // desktop cache as Windows enters Winlogon. Once the secure
            // desktop is positively observed, discard any Default-desktop
            // helper handoff timer so time spent on UAC cannot expire it and
            // tear down focus immediately after Default returns.
            PrivilegedInputUnavailableSince_.reset();
        } else if (OrdinaryInputReady) {
            PrivilegedInputUnavailableSince_.reset();
        } else {
            const bool PrivilegedAuthorized = PrivilegedInput_ &&
                PrivilegedInput_->Authorized();
            const bool TransitionExpired = PrivilegedInputUnavailableSince_ &&
                Now - *PrivilegedInputUnavailableSince_ >=
                    PrivilegedInputTransitionGrace;
            if (!PrivilegedAuthorized || TransitionExpired) {
                (void)RejectInputUnavailable();
            }
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
    InputUnavailable_ = false;
    NextInputAvailabilityCheck_ = {};
    PrivilegedInputUnavailableSince_.reset();
}

AgentDecision AgentCoordinator::RejectInputUnavailable() noexcept {
    focus_.release_remote_focus();
    last_pointer_sequence_ = 0;
    InputUnavailable_ = true;
    PrivilegedInputUnavailableSince_.reset();
    (void)ReleaseOwnedState();
    return AgentDecision::RejectedInputUnavailable;
}

bool AgentCoordinator::ReleaseOwnedState() noexcept {
    const bool OrdinaryReleased = injector_.release_owned_state();
    const bool PrivilegedWasAuthorized = PrivilegedInput_ &&
        PrivilegedInput_->Authorized();
    const bool PrivilegedReleased = !PrivilegedWasAuthorized ||
        PrivilegedInput_->Release();
    if (PrivilegedWasAuthorized) PrivilegedInput_->Revoke();
    InputCleanupPending_ = !OrdinaryReleased || !PrivilegedReleased;
    return !InputCleanupPending_;
}

} // namespace desklink
