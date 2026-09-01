#pragma once

#include "desklink/roaming.hpp"

#include <chrono>
#include <functional>
#include <optional>
#include <string_view>

namespace desklink {

// The broker's fail-local managed-runtime stop is bounded at five seconds.
// Permission approval must wait beyond that boundary so the shell does not
// report failure while the broker safely completes the reviewed mutation.
inline constexpr std::chrono::milliseconds
    kProductPermissionResolutionTimeout{7'000};

// The broker serializes security-sensitive mutations and managed-runtime
// transitions. Preserve the last authenticated presentation through those
// bounded operations, but report a continuous outage after a finite grace.
inline constexpr std::chrono::milliseconds
    kProductBrokerUnavailableGrace{8'000};

class ProductBrokerAvailability final {
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    void ObserveAvailable(TimePoint) noexcept { FailureSince_.reset(); }

    [[nodiscard]] bool ObserveUnavailable(TimePoint Now) noexcept {
        if (!FailureSince_) {
            FailureSince_ = Now;
            return false;
        }
        return Now - *FailureSince_ >= kProductBrokerUnavailableGrace;
    }

private:
    std::optional<TimePoint> FailureSince_;
};

// A broker state request may include one bounded hop to an active transport
// owner. Keep this longer than that 500 ms hop without allowing an
// unresponsive runtime to stall the product shell indefinitely.
inline constexpr std::chrono::milliseconds kProductBrokerStateTimeout{750};

enum class ProductShellState {
    Offline,
    Connecting,
    ConnectedLocal,
    RemoteFocus,
    ActionRequired,
    Paused,
};

struct ProductShellPresentation {
    std::wstring_view Badge;
    std::wstring_view KeyboardAndMouseTitle;
    std::wstring_view KeyboardAndMouseSummary;
    std::wstring_view ConnectionDetail;
    bool ShowReturnLocal{};
    bool ShowActionRequired{};
};

enum class ProductMonitorSaveStatus {
    Applied,
    CleanupFailed,
    StoreFailed,
    PreferenceApplyFailed,
};

enum class ProductCrossingPreset {
    CrossImmediately,
    PauseAndPush,
    PushTwice,
};

// Applies a plain-language preset to both the defaults used for new routes
// and every direction of every existing route.
[[nodiscard]] bool ApplyProductCrossingPreset(
    RoamingConfiguration& Configuration,
    ProductCrossingPreset Preset) noexcept;

struct ProductMonitorSaveActions {
    std::function<bool()> ConfirmLocal;
    std::function<bool()> ReturnLocal;
    std::function<bool()> SaveAtomically;
    std::function<bool()> ApplyPreferencesLive;
};

// A remotely focused runtime must return and then confirm Local before the
// atomic graph save. The authenticated transport remains alive; preferences
// are negotiated only after persistence succeeds.
[[nodiscard]] ProductMonitorSaveStatus ApplyProductMonitorLayout(
    bool RuntimeRemote,
    const ProductMonitorSaveActions& Actions);

constexpr ProductShellPresentation PresentProductShellState(
    ProductShellState State) noexcept {
    switch (State) {
        case ProductShellState::Offline:
            return {
                L"Offline",
                L"Keyboard & mouse",
                L"Waiting for your Companion PC",
                L"No authenticated peer is connected.",
                false,
                false,
            };
        case ProductShellState::Connecting:
            return {
                L"Connecting",
                L"Keyboard & mouse",
                L"Connecting safely; input remains on this PC",
                L"DeskLink is retrying an ordinary availability failure.",
                false,
                false,
            };
        case ProductShellState::ConnectedLocal:
            return {
                L"Connected",
                L"Keyboard & mouse",
                L"Ready to move between both PCs",
                L"HOSTPC is authenticated; focus is Local.",
                false,
                false,
            };
        case ProductShellState::RemoteFocus:
            return {
                L"Controlling HOSTPC",
                L"Currently controlling HOSTPC",
                L"Use Return to this PC or Ctrl+Alt+Pause/Break",
                L"Remote focus is active on the authenticated peer.",
                true,
                false,
            };
        case ProductShellState::ActionRequired:
            return {
                L"Action required",
                L"Keyboard & mouse",
                L"Input is Local until the connection is reviewed",
                L"Automatic retry stopped after a security or protocol failure.",
                false,
                true,
            };
        case ProductShellState::Paused:
            return {
                L"Paused",
                L"Keyboard & mouse",
                L"DeskLink is paused; input remains on this PC",
                L"Automatic listening and connection attempts are paused.",
                false,
                false,
            };
    }
    return {};
}

} // namespace desklink
