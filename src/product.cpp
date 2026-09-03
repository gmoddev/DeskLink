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

[[nodiscard]] bool IsValidVoiceRoute(VoiceRoutePreference Route) noexcept {
    return Route == VoiceRoutePreference::Off ||
           Route == VoiceRoutePreference::PeerToLocal ||
           Route == VoiceRoutePreference::LocalToPeer ||
           Route == VoiceRoutePreference::Bidirectional;
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

[[nodiscard]] bool SameExecutableName(
    std::string_view Left, std::string_view Right) noexcept {
    if (Left.size() != Right.size()) return false;
    for (std::size_t Index = 0; Index < Left.size(); ++Index) {
        auto A = static_cast<unsigned char>(Left[Index]);
        auto B = static_cast<unsigned char>(Right[Index]);
        if (A >= 'A' && A <= 'Z') A = static_cast<unsigned char>(A + 32u);
        if (B >= 'A' && B <= 'Z') B = static_cast<unsigned char>(B + 32u);
        if (A != B) return false;
    }
    return true;
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
    if (!IsValidDeskRole(Preferences.Role) ||
        !IsValidAudioRoute(Preferences.AudioRoute) ||
        !IsValidVoiceRoute(Preferences.VoiceRoute) ||
        !IsValidGamingBehavior(Preferences.Gaming) ||
        !IsValidProductHotkey(Preferences.FocusPeerHotkey) ||
        !IsValidProductHotkey(Preferences.ReturnLocalHotkey) ||
        Preferences.AudioGainPermyriad > 10'000 ||
        Preferences.VoiceGainPermyriad > 10'000 ||
        (Preferences.VoiceInputEndpointId &&
         (Preferences.VoiceInputEndpointId->empty() ||
          Preferences.VoiceInputEndpointId->size() >
              kMaximumVoiceEndpointIdBytes)) ||
        Preferences.ProfileRules.size() > kMaximumForegroundProfileRules ||
        (Preferences.PreferredPeerMachine &&
         IsZeroMachine(*Preferences.PreferredPeerMachine)) ||
        (Preferences.PreferredPeerEndpoint &&
         (!Preferences.PreferredPeerMachine ||
          !IsValidProductPeerEndpoint(
              *Preferences.PreferredPeerEndpoint))) ||
        (Preferences.FocusPeerHotkey != ProductHotkey::Off &&
         Preferences.FocusPeerHotkey == Preferences.ReturnLocalHotkey)) {
        return false;
    }
    for (std::size_t Index = 0; Index < Preferences.ProfileRules.size();
         ++Index) {
        const auto& Rule = Preferences.ProfileRules[Index];
        if (!IsValidForegroundProfileRule(Rule)) return false;
        for (std::size_t Other = Index + 1;
             Other < Preferences.ProfileRules.size(); ++Other) {
            const auto& Candidate = Preferences.ProfileRules[Other];
            if (Rule.FullscreenOnly == Candidate.FullscreenOnly &&
                SameExecutableName(
                    Rule.ExecutableName, Candidate.ExecutableName)) {
                return false;
            }
        }
    }
    return true;
}

bool IsValidProductPeerEndpoint(
    const ProductPeerEndpoint& Endpoint) noexcept {
    if (Endpoint.Host.empty() ||
        Endpoint.Host.size() > kMaximumPreferredPeerHostBytes ||
        Endpoint.Port == 0) {
        return false;
    }
    if (!std::all_of(
        Endpoint.Host.begin(), Endpoint.Host.end(),
        [](unsigned char Character) {
            return Character > 0x20u && Character != 0x7fu &&
                   Character != static_cast<unsigned char>('"');
        })) {
        return false;
    }
    if (Endpoint.Host.front() == '[') {
        return Endpoint.Host.back() == ']' && Endpoint.Host.size() > 3 &&
            Endpoint.Host.substr(1, Endpoint.Host.size() - 2).find(':') !=
                std::string::npos;
    }
    if (Endpoint.Host.find_first_of("[]") != std::string::npos) return false;
    // The port is stored separately. Reject a likely host:port typo while
    // retaining unbracketed IPv6 literals with multiple colons.
    return std::count(
        Endpoint.Host.begin(), Endpoint.Host.end(), ':') != 1;
}

bool IsValidProductHotkey(ProductHotkey Hotkey) noexcept {
    return Hotkey == ProductHotkey::Off ||
           Hotkey == ProductHotkey::CtrlAltF11 ||
           Hotkey == ProductHotkey::CtrlAltF12 ||
           Hotkey == ProductHotkey::CtrlShiftF11 ||
           Hotkey == ProductHotkey::CtrlShiftF12;
}

bool CanEnableClipboardIntent(CapabilitySet LocalGrantsToPeer) noexcept {
    return LocalGrantsToPeer.contains(Capability::ClipboardRead) &&
           LocalGrantsToPeer.contains(Capability::ClipboardWrite);
}

bool CanEnablePeerAudioIntent(CapabilitySet LocalGrantsToPeer) noexcept {
    return LocalGrantsToPeer.contains(Capability::AudioSend);
}

bool CanEnableLocalAudioIntent(CapabilitySet LocalGrantsToPeer) noexcept {
    return LocalGrantsToPeer.contains(Capability::AudioReceive);
}

bool CanEnablePeerVoiceIntent(CapabilitySet LocalGrantsToPeer) noexcept {
    return LocalGrantsToPeer.contains(Capability::VoiceSend);
}

bool CanEnableLocalVoiceIntent(CapabilitySet LocalGrantsToPeer) noexcept {
    return LocalGrantsToPeer.contains(Capability::VoiceReceive);
}

bool ApplyProductCrossingPreset(
    RoamingConfiguration& Configuration,
    ProductCrossingPreset Preset) noexcept {
    CrossingConfiguration Crossing;
    switch (Preset) {
        case ProductCrossingPreset::CrossImmediately:
            Crossing = {CrossingPolicy::Push, 8, 0, 500};
            break;
        case ProductCrossingPreset::PauseAndPush:
            Crossing = {CrossingPolicy::DwellAndPush, 8, 180, 500};
            break;
        case ProductCrossingPreset::PushTwice:
            Crossing = {CrossingPolicy::DoublePush, 8, 0, 600};
            break;
        default:
            return false;
    }
    if (!IsValidCrossingConfiguration(Crossing)) return false;

    Configuration.CrossingDefaults = Crossing;
    for (auto& Link : Configuration.Links) {
        Link.AToB = Crossing;
        Link.BToA = Crossing;
    }
    return IsValidRoamingConfiguration(Configuration);
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
    Result.VoiceGainPermyriad = Preferences.VoiceGainPermyriad;

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
    const bool WantsAnyVoice =
        Preferences.VoiceRoute != VoiceRoutePreference::Off;
    const bool WantsAnyPeerFeature = WantsPeerFeature || WantsAnyVoice;
    if (!WantsAnyPeerFeature) return Result;
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

    const bool WantsReceiveVoice =
        Preferences.VoiceRoute == VoiceRoutePreference::PeerToLocal ||
        Preferences.VoiceRoute == VoiceRoutePreference::Bidirectional;
    const bool WantsSendVoice =
        Preferences.VoiceRoute == VoiceRoutePreference::LocalToPeer ||
        Preferences.VoiceRoute == VoiceRoutePreference::Bidirectional;
    if (WantsReceiveVoice) {
        Result.ReceiveVoice = Context.LocalGrantsToPeer.contains(
            Capability::VoiceSend) &&
            Context.PeerGrantsToLocal.contains(Capability::VoiceReceive);
    }
    if (WantsSendVoice) {
        Result.SendVoice = Context.LocalGrantsToPeer.contains(
            Capability::VoiceReceive) &&
            Context.PeerGrantsToLocal.contains(Capability::VoiceSend);
    }
    if ((WantsReceiveVoice && !Result.ReceiveVoice) ||
        (WantsSendVoice && !Result.SendVoice)) {
        AddBlocker(Result, RuntimePlanBlocker::VoiceCapabilityMissing);
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
    bool RuntimeRemote,
    const ProductMonitorSaveActions& Actions) {
    if (!Actions.ConfirmLocal || !Actions.ReturnLocal ||
        !Actions.SaveAtomically || !Actions.ApplyPreferencesLive) {
        return ProductMonitorSaveStatus::CleanupFailed;
    }
    if (RuntimeRemote && !Actions.ReturnLocal()) {
        return ProductMonitorSaveStatus::CleanupFailed;
    }
    if (!Actions.ConfirmLocal()) {
        return ProductMonitorSaveStatus::CleanupFailed;
    }
    if (!Actions.SaveAtomically()) {
        return ProductMonitorSaveStatus::StoreFailed;
    }
    return Actions.ApplyPreferencesLive()
        ? ProductMonitorSaveStatus::Applied
        : ProductMonitorSaveStatus::PreferenceApplyFailed;
}

} // namespace desklink
