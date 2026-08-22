#pragma once

#include "desklink/protocol.hpp"

namespace desklink {

class IInputInjector {
public:
    virtual ~IInputInjector() = default;
    virtual bool inject_key(const KeyEventMessage& event) = 0;
    virtual bool inject_button(const MouseButtonMessage& event) = 0;
    virtual bool inject_pointer(const PointerPositionMessage& event) = 0;
    virtual void release_owned_state() noexcept = 0;
};

class NullInputInjector final : public IInputInjector {
public:
    bool inject_key(const KeyEventMessage&) override { return true; }
    bool inject_button(const MouseButtonMessage&) override { return true; }
    bool inject_pointer(const PointerPositionMessage&) override { return true; }
    void release_owned_state() noexcept override {}
};

} // namespace desklink
