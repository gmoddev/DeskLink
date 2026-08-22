#include "desklink/input.hpp"

namespace desklink {
namespace {

void AddKeyTransitions(std::vector<InputStateTransition>& Result,
                       const InputStateSnapshotMessage& Current,
                       const InputStateSnapshotMessage& Desired,
                       bool Extended,
                       bool Down) {
    for (std::uint16_t ScanCode = 1; ScanCode <= 255; ++ScanCode) {
        const bool CurrentDown = InputSnapshotKeyDown(Current, ScanCode, Extended);
        const bool DesiredDown = InputSnapshotKeyDown(Desired, ScanCode, Extended);
        if (CurrentDown != DesiredDown && DesiredDown == Down) {
            Result.emplace_back(KeyEventMessage{ScanCode, Extended, Down});
        }
    }
}

void AddButtonTransitions(std::vector<InputStateTransition>& Result,
                          const InputStateSnapshotMessage& Current,
                          const InputStateSnapshotMessage& Desired,
                          bool Down) {
    for (std::uint8_t Raw = static_cast<std::uint8_t>(MouseButtonId::Left);
         Raw <= static_cast<std::uint8_t>(MouseButtonId::X2); ++Raw) {
        const auto Button = static_cast<MouseButtonId>(Raw);
        const bool CurrentDown = InputSnapshotButtonDown(Current, Button);
        const bool DesiredDown = InputSnapshotButtonDown(Desired, Button);
        if (CurrentDown != DesiredDown && DesiredDown == Down) {
            Result.emplace_back(MouseButtonMessage{Button, Down});
        }
    }
}

} // namespace

std::vector<InputStateTransition> BuildInputStateTransitions(
    const InputStateSnapshotMessage& Current,
    const InputStateSnapshotMessage& Desired) {
    std::vector<InputStateTransition> Result;
    Result.reserve(517);

    // Release stale state before pressing missing state so reconciliation cannot
    // transiently accumulate more held input than either authoritative snapshot.
    AddKeyTransitions(Result, Current, Desired, false, false);
    AddKeyTransitions(Result, Current, Desired, true, false);
    AddButtonTransitions(Result, Current, Desired, false);
    AddKeyTransitions(Result, Current, Desired, false, true);
    AddKeyTransitions(Result, Current, Desired, true, true);
    AddButtonTransitions(Result, Current, Desired, true);
    return Result;
}

} // namespace desklink
