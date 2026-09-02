#pragma once

#include "desklink/protocol.hpp"

#include <variant>
#include <vector>
#include <optional>

namespace desklink {

class IInputInjector {
public:
    virtual ~IInputInjector() = default;
    virtual bool inject_key(const KeyEventMessage& event) = 0;
    virtual bool inject_button(const MouseButtonMessage& event) = 0;
    virtual bool inject_pointer(const PointerPositionMessage& event) = 0;
    virtual bool InjectPointerMotion(const PointerMotionMessage& Message) = 0;
    virtual bool InjectWheel(const MouseWheelMessage& Message) = 0;
    virtual bool ReconcileState(const InputStateSnapshotMessage& Snapshot) = 0;
    [[nodiscard]] virtual std::optional<PointerPositionMessage>
    CurrentPointerPosition() { return std::nullopt; }
    [[nodiscard]] virtual bool release_owned_state() noexcept = 0;
    // Cosmetic only: an implementation may park its now-inactive pointer
    // after an authenticated, orderly focus release. Failure must never
    // prevent key/button cleanup or fail-local focus release.
    [[nodiscard]] virtual bool ParkPointer() noexcept { return true; }
};

using InputStateTransition = std::variant<KeyEventMessage, MouseButtonMessage>;

[[nodiscard]] std::vector<InputStateTransition> BuildInputStateTransitions(
    const InputStateSnapshotMessage& Current,
    const InputStateSnapshotMessage& Desired);

class NullInputInjector final : public IInputInjector {
public:
    bool inject_key(const KeyEventMessage&) override { return true; }
    bool inject_button(const MouseButtonMessage&) override { return true; }
    bool inject_pointer(const PointerPositionMessage&) override { return true; }
    bool InjectPointerMotion(const PointerMotionMessage&) override { return true; }
    bool InjectWheel(const MouseWheelMessage&) override { return true; }
    bool ReconcileState(const InputStateSnapshotMessage&) override { return true; }
    [[nodiscard]] bool release_owned_state() noexcept override { return true; }
};

} // namespace desklink
