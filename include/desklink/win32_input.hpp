#pragma once

#ifdef _WIN32

#include "desklink/input.hpp"

namespace desklink {

class Win32InputInjector final : public IInputInjector {
public:
    bool inject_key(const KeyEventMessage& event) override;
    bool inject_button(const MouseButtonMessage& event) override;
    bool inject_pointer(const PointerPositionMessage& event) override;
    bool ReconcileState(const InputStateSnapshotMessage& Snapshot) override;
    void release_owned_state() noexcept override;

private:
    InputStateSnapshotMessage OwnedState_{};
};

} // namespace desklink

#endif
