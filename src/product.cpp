#include "desklink/product.hpp"
#include "desklink/product_shell.hpp"

#include <algorithm>

namespace desklink {
namespace {

[[nodiscard]] bool IsValidDeskRole(DeskRole Role) noexcept {
    return Role == DeskRole::Unconfigured || Role == DeskRole::Main ||
           Role == DeskRole::Companion || Role == DeskRole::Flexible;
}

[[nodiscard]] bool IsValidAudioRoute(AudioRoutePreference Route) noexcept {
    return Route == AudioRoutePreference::Off ||
           Route == AudioRoutePreference::PeerToLocal ||
           Route == AudioRoutePreference::LocalToPeer ||
           Route == AudioRoutePreference::Bidirectional;
}

[[nodiscard]] bool IsValidGamingBehavior(GamingBehavior Behavior) noexcept {
    return Behavior == GamingBehavior::KeepLocal ||
           Behavior == GamingBehavior::FollowProfileRules;
}

[[nodiscard]] bool IsZeroMachine(const MachineId& Machine) noexcept {
    return std::all_of(
        Machine.begin(), Machine.end(),
        [](std::uint8_t Value) { return Value == 0; });
}

void AddBlocker(DesiredDeskConfiguration& Configuration,
                RuntimePlanBlocker Blocker) noexcept {
    Configuration.Blockers |= static_cast<std::uint32_t>(Blocker);
}

[[nodiscard]] bool HasBothClipboardDirections(
    const RuntimePlannerContext& Context) noexcept {
    return Context.LocalGrantsToPeer.contains(Capability::ClipboardRead) &&
           Context.LocalGrantsToPeer.contains(Capability::ClipboardWrite) &&
           Context.PeerGrantsToLocal.contains(Capability::ClipboardRead) &&
           Context.PeerGrantsToLocal.contains(Capability::ClipboardWrite);
}

} // namespace

bool IsValidProductPreferences(
    const ProductPreferences& Preferences) noexcept {
    return IsValidDeskRole(Preferences.Role) &&
           IsValidAudioRoute(Preferences.AudioRoute) &&
           IsValidGamingBehavior(Preferences.Gaming) &&
           Preferences.AudioGainPermyriad <= 10'000 &&
           (!Preferences.PreferredPeerMachine ||
            !IsZeroMachine(*Preferences.PreferredPeerMachine));
}

DesiredDeskConfiguration PlanDesiredDeskConfiguration(
    const ProductPreferences& Preferences,
    const RuntimePlannerContext& Context) noexcept {
    DesiredDeskConfiguration Result;
    if (!IsValidProductPreferences(Preferences)) {
        AddBlocker(Result, RuntimePlanBlocker::InvalidPreferences);
        return Result;
    }

    Result.PreferencesValid = true;
    Result.PreferredPeerMachine = Preferences.PreferredPeerMachine;
    Result.AudioGainPermyriad = Preferences.AudioGainPermyriad;

    if (Preferences.Role == DeskRole::Unconfigured) {
        AddBlocker(Result, RuntimePlanBlocker::RoleUnconfigured);
        return Result;
    }
    if (!Preferences.AutoStartRuntime) {
        AddBlocker(Result, RuntimePlanBlocker::RuntimeDisabled);
        return Result;
    }

    Result.StartRuntime = true;
    Result.Listen = Preferences.Role == DeskRole::Companion ||
                    Preferences.Role == DeskRole::Flexible;

    const bool WantsConnection = Preferences.AutoConnect &&
        (Preferences.Role == DeskRole::Main ||
         Preferences.Role == DeskRole::Flexible);
    if (WantsConnection) {
        if (!Preferences.PreferredPeerMachine) {
            AddBlocker(Result, RuntimePlanBlocker::PreferredPeerMissing);
        } else if (!Context.PreferredPeerTrusted) {
            AddBlocker(Result, RuntimePlanBlocker::PeerNotTrusted);
        } else {
            Result.ConnectPreferredPeer = true;
        }
    }

    const bool WantsPeerFeature = Preferences.InputRoamingDesired ||
        Preferences.ClipboardDesired ||
        Preferences.AudioRoute != AudioRoutePreference::Off;
    if (!WantsPeerFeature) return Result;
    if (!Preferences.PreferredPeerMachine) {
        AddBlocker(Result, RuntimePlanBlocker::PreferredPeerMissing);
        return Result;
    }
    if (!Context.PreferredPeerTrusted) {
        AddBlocker(Result, RuntimePlanBlocker::PeerNotTrusted);
        return Result;
    }
    if (!Context.PeerValidated) {
        AddBlocker(Result, RuntimePlanBlocker::PeerNotValidated);
        return Result;
    }

    if (Preferences.InputRoamingDesired) {
        const bool InputAllowed = Context.PeerGrantsToLocal.contains(
            Capability::InputInject);
        const bool TopologyAllowed = Context.LocalGrantsToPeer.contains(
            Capability::DisplayTopologyExchange) &&
            Context.PeerGrantsToLocal.contains(
                Capability::DisplayTopologyExchange);
        if (!InputAllowed) {
            AddBlocker(Result, RuntimePlanBlocker::InputCapabilityMissing);
        }
        if (!TopologyAllowed) {
            AddBlocker(Result, RuntimePlanBlocker::TopologyCapabilityMissing);
        }
        if (!Context.RoamingRouteReady) {
            AddBlocker(Result, RuntimePlanBlocker::RoamingRouteUnavailable);
        }
        Result.EnableInputRoaming =
            InputAllowed && TopologyAllowed && Context.RoamingRouteReady;
    }

    if (Preferences.ClipboardDesired) {
        Result.EnableClipboard = HasBothClipboardDirections(Context);
        if (!Result.EnableClipboard) {
            AddBlocker(
                Result, RuntimePlanBlocker::ClipboardCapabilityMissing);
        }
    }

    const bool WantsReceiveAudio =
        Preferences.AudioRoute == AudioRoutePreference::PeerToLocal ||
        Preferences.AudioRoute == AudioRoutePreference::Bidirectional;
    const bool WantsSendAudio =
        Preferences.AudioRoute == AudioRoutePreference::LocalToPeer ||
        Preferences.AudioRoute == AudioRoutePreference::Bidirectional;
    if (WantsReceiveAudio) {
        Result.ReceiveAudio = Context.LocalGrantsToPeer.contains(
            Capability::AudioSend) &&
            Context.PeerGrantsToLocal.contains(Capability::AudioReceive);
    }
    if (WantsSendAudio) {
        Result.SendAudio = Context.LocalGrantsToPeer.contains(
            Capability::AudioReceive) &&
            Context.PeerGrantsToLocal.contains(Capability::AudioSend);
    }
    if ((WantsReceiveAudio && !Result.ReceiveAudio) ||
        (WantsSendAudio && !Result.SendAudio)) {
        AddBlocker(Result, RuntimePlanBlocker::AudioCapabilityMissing);
    }
    return Result;
}

bool HasRuntimePlanBlocker(
    const DesiredDeskConfiguration& Configuration,
    RuntimePlanBlocker Blocker) noexcept {
    const auto Bit = static_cast<std::uint32_t>(Blocker);
    if (Bit == 0) return Configuration.Blockers == 0;
    return (Configuration.Blockers & Bit) == Bit;
}

ProductMonitorSaveStatus ApplyProductMonitorLayout(
    bool RuntimeWasPaused,
    const ProductMonitorSaveActions& Actions) {
    if (!Actions.ConfirmStoppedLocalWhilePaused ||
        !Actions.PauseAndStopRuntime || !Actions.SaveAtomically ||
        !Actions.ResumeRuntime) {
        return ProductMonitorSaveStatus::CleanupFailed;
    }
    const bool LocalConfirmed = RuntimeWasPaused
        ? Actions.ConfirmStoppedLocalWhilePaused()
        : Actions.PauseAndStopRuntime();
    if (!LocalConfirmed) return ProductMonitorSaveStatus::CleanupFailed;

    const bool Saved = Actions.SaveAtomically();
    if (RuntimeWasPaused) {
        return Saved
            ? ProductMonitorSaveStatus::AppliedRuntimePaused
            : ProductMonitorSaveStatus::StoreFailed;
    }
    const bool Resumed = Actions.ResumeRuntime();
    if (!Saved) {
        return Resumed
            ? ProductMonitorSaveStatus::StoreFailed
            : ProductMonitorSaveStatus::StoreFailedRuntimePaused;
    }
    return Resumed
        ? ProductMonitorSaveStatus::Applied
        : ProductMonitorSaveStatus::AppliedRuntimePaused;
}

} // namespace desklink
