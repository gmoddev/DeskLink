#pragma once

#include "desklink/capabilities.hpp"
#include "desklink/profile.hpp"
#include "desklink/protocol.hpp"
#include "desklink/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace desklink {

inline constexpr std::uint16_t kProductPreferencesSchemaVersion = 4;
inline constexpr std::size_t kMaximumPreferredPeerHostBytes = 253;

enum class DeskRole : std::uint8_t {
    Unconfigured = 0,
    Main = 1,
    Companion = 2,
    Flexible = 3,
};

enum class AudioRoutePreference : std::uint8_t {
    Off = 0,
    PeerToLocal = 1,
    LocalToPeer = 2,
    Bidirectional = 3,
};

enum class GamingBehavior : std::uint8_t {
    KeepLocal = 0,
    FollowProfileRules = 1,
};

// Product hotkeys intentionally use a small allowlist rather than persisting
// arbitrary virtual-key/modifier combinations. This keeps the UI predictable,
// avoids bare-key capture, and reserves Ctrl+Alt+Pause/Break exclusively for
// the independent emergency fail-local path.
enum class ProductHotkey : std::uint8_t {
    Off = 0,
    CtrlAltF11 = 1,
    CtrlAltF12 = 2,
    CtrlShiftF11 = 3,
    CtrlShiftF12 = 4,
};

// This is only a routing hint for an already-trusted peer. Transport setup
// must still bind the connection to PreferredPeerMachine and its stored
// certificate pin before any application traffic is admitted.
struct ProductPeerEndpoint {
    std::string Host;
    std::uint16_t Port{};

    [[nodiscard]] bool operator==(
        const ProductPeerEndpoint&) const noexcept = default;
};

struct ProductPreferences {
    DeskRole Role{DeskRole::Unconfigured};
    std::optional<MachineId> PreferredPeerMachine;
    std::optional<ProductPeerEndpoint> PreferredPeerEndpoint;
    bool RunAtLogin{};
    bool CloseToTray{true};
    bool AutoStartRuntime{};
    bool AutoConnect{};
    bool InputRoamingDesired{};
    bool ClipboardDesired{};
    AudioRoutePreference AudioRoute{AudioRoutePreference::Off};
    std::uint16_t AudioGainPermyriad{10'000};
    GamingBehavior Gaming{GamingBehavior::KeepLocal};
    ProductHotkey FocusPeerHotkey{ProductHotkey::Off};
    ProductHotkey ReturnLocalHotkey{ProductHotkey::Off};
    std::vector<ForegroundProfileRule> ProfileRules;
    bool AdvancedModeEnabled{};
    bool FirstRunComplete{};

    [[nodiscard]] bool operator==(
        const ProductPreferences&) const noexcept = default;
};

[[nodiscard]] bool IsValidProductPreferences(
    const ProductPreferences& Preferences) noexcept;
[[nodiscard]] bool IsValidProductPeerEndpoint(
    const ProductPeerEndpoint& Endpoint) noexcept;
[[nodiscard]] bool IsValidProductHotkey(ProductHotkey Hotkey) noexcept;
[[nodiscard]] bool CanEnableClipboardIntent(
    CapabilitySet LocalGrantsToPeer) noexcept;
[[nodiscard]] bool CanEnablePeerAudioIntent(
    CapabilitySet LocalGrantsToPeer) noexcept;

enum class RuntimePlanBlocker : std::uint32_t {
    None = 0,
    InvalidPreferences = 1u << 0u,
    RoleUnconfigured = 1u << 1u,
    RuntimeDisabled = 1u << 2u,
    PreferredPeerMissing = 1u << 3u,
    PeerNotTrusted = 1u << 4u,
    PeerNotValidated = 1u << 5u,
    InputCapabilityMissing = 1u << 6u,
    TopologyCapabilityMissing = 1u << 7u,
    RoamingRouteUnavailable = 1u << 8u,
    ClipboardCapabilityMissing = 1u << 9u,
    AudioCapabilityMissing = 1u << 10u,
};

struct RuntimePlannerContext {
    bool PreferredPeerTrusted{};
    bool PeerValidated{};
    bool RoamingRouteReady{};
    CapabilitySet LocalGrantsToPeer;
    CapabilitySet PeerGrantsToLocal;
};

struct DesiredDeskConfiguration {
    std::optional<MachineId> PreferredPeerMachine;
    DeskMode InitialMode{DeskMode::LockPc1};
    std::uint16_t AudioGainPermyriad{10'000};
    std::uint32_t Blockers{};
    bool PreferencesValid{};
    bool StartRuntime{};
    bool Listen{};
    bool ConnectPreferredPeer{};
    bool EnableInputRoaming{};
    bool EnableClipboard{};
    bool SendAudio{};
    bool ReceiveAudio{};

    [[nodiscard]] bool operator==(
        const DesiredDeskConfiguration&) const noexcept = default;
};

[[nodiscard]] DesiredDeskConfiguration PlanDesiredDeskConfiguration(
    const ProductPreferences& Preferences,
    const RuntimePlannerContext& Context) noexcept;

[[nodiscard]] bool HasRuntimePlanBlocker(
    const DesiredDeskConfiguration& Configuration,
    RuntimePlanBlocker Blocker) noexcept;

} // namespace desklink
