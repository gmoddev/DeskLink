#pragma once

#ifdef _WIN32

#include "desklink/input.hpp"
#include "desklink/win32_display_topology.hpp"

#include <optional>

namespace desklink {

class Win32InputInjector final : public IInputInjector {
public:
    Win32InputInjector();

    bool inject_key(const KeyEventMessage& event) override;
    bool inject_button(const MouseButtonMessage& event) override;
    bool inject_pointer(const PointerPositionMessage& event) override;
    bool ReconcileState(const InputStateSnapshotMessage& Snapshot) override;
    void release_owned_state() noexcept override;

    [[nodiscard]] bool RefreshDisplayTopology();
    [[nodiscard]] const DisplayTopologySnapshot& CurrentDisplayTopology() const noexcept;

private:
    InputStateSnapshotMessage OwnedState_{};
    Win32DisplayTopology DisplayTopology_;
    std::optional<std::uint64_t> DisplayGeneration_;
};

} // namespace desklink

#endif
