#pragma once

#include "desklink/roaming.hpp"

#include <functional>
#include <string_view>

namespace desklink {

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
    AppliedRuntimePaused,
    CleanupFailed,
    StoreFailed,
    StoreFailedRuntimePaused,
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
    std::function<bool()> ConfirmStoppedLocalWhilePaused;
    std::function<bool()> PauseAndStopRuntime;
    std::function<bool()> SaveAtomically;
    std::function<bool()> ResumeRuntime;
};

// The save callback is never reached until a stopped/paused runtime has
// confirmed Local. A runtime stopped by this operation is offered exactly one
// resume attempt after either save success or failure.
[[nodiscard]] ProductMonitorSaveStatus ApplyProductMonitorLayout(
    bool RuntimeWasPaused,
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
