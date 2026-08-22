#pragma once

#ifdef _WIN32

#include "desklink/input.hpp"

#include <cstdint>
#include <set>

namespace desklink {

class Win32InputInjector final : public IInputInjector {
public:
    bool inject_key(const KeyEventMessage& event) override;
    bool inject_button(const MouseButtonMessage& event) override;
    bool inject_pointer(const PointerPositionMessage& event) override;
    void release_owned_state() noexcept override;

private:
    struct KeyState {
        std::uint16_t scan_code{};
        bool extended{};
        bool operator<(const KeyState& other) const noexcept {
            if (scan_code != other.scan_code) return scan_code < other.scan_code;
            return extended < other.extended;
        }
    };

    std::set<KeyState> owned_keys_;
    std::set<MouseButtonId> owned_buttons_;
};

} // namespace desklink

#endif
