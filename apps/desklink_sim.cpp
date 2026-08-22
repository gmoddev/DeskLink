#include "desklink/agent.hpp"
#include "desklink/capabilities.hpp"
#include "desklink/host.hpp"
#include "desklink/input.hpp"
#include "desklink/protocol.hpp"
#include "desklink/types.hpp"

#include <chrono>
#include <iostream>
#include <string>

namespace {

class DemoClock final : public desklink::IClock {
public:
    time_point now() const noexcept override { return current_; }
    void advance(std::chrono::milliseconds delta) { current_ += delta; }
private:
    time_point current_{};
};

class PrintingInjector final : public desklink::IInputInjector {
public:
    bool inject_key(const desklink::KeyEventMessage& event) override {
        std::cout << "agent: key scan=" << event.scan_code << (event.down ? " down" : " up") << '\n';
        return true;
    }
    bool inject_button(const desklink::MouseButtonMessage& event) override {
        std::cout << "agent: mouse button=" << static_cast<int>(event.button)
                  << (event.down ? " down" : " up") << '\n';
        return true;
    }
    bool inject_pointer(const desklink::PointerPositionMessage& event) override {
        std::cout << "agent: pointer display=" << event.display_id
                  << " x=" << event.normalized_x << " y=" << event.normalized_y << '\n';
        return true;
    }
    void release_owned_state() noexcept override {
        std::cout << "agent: release DeskLink-owned key/button state\n";
    }
};

bool deliver_agent(desklink::AgentCoordinator& agent, desklink::ByteBuffer packet, bool datagram = false) {
    auto decoded = desklink::decode_packet(packet, datagram);
    if (!decoded.packet) {
        std::cerr << "decode failed: " << decoded.detail << '\n';
        return false;
    }
    const auto result = agent.handle(*decoded.packet);
    std::cout << "agent decision=" << static_cast<int>(result) << '\n';
    return result == desklink::AgentDecision::Accepted;
}

} // namespace

int main() {
    using namespace desklink;

    constexpr std::uint64_t session = 0xD35C11A5u;
    DemoClock clock;
    PrintingInjector injector;
    AgentCoordinator agent(clock, injector);
    HostCoordinator host(session);

    CapabilitySet granted;
    granted.grant(Capability::InputInject);
    granted.grant(Capability::CoreStateRead);
    agent.set_peer_capabilities(granted);

    std::cout << "DeskLink foundation simulation\n";
    std::cout << "requesting remote focus...\n";
    if (!deliver_agent(agent, host.request_remote_focus(750))) return 1;

    EnvelopeHeader ready_header;
    ready_header.session_nonce = session;
    ready_header.epoch = agent.focus_state().epoch();
    ready_header.sequence = 1;
    auto ready_bytes = encode_packet(ready_header, FocusReadyMessage{750, 1});
    auto ready = decode_packet(ready_bytes, false);
    if (!ready.packet || !host.accept_focus_ready(*ready.packet)) return 2;

    std::cout << "focus epoch=" << host.remote_epoch() << '\n';

    auto key_down = host.key_event(KeyEventMessage{0x1E, false, true});
    auto pointer = host.pointer_position(PointerPositionMessage{0, 42000, 25000});
    auto key_up = host.key_event(KeyEventMessage{0x1E, false, false});
    if (!key_down || !pointer || !key_up) return 3;

    (void)deliver_agent(agent, std::move(*key_down));
    (void)deliver_agent(agent, std::move(*pointer), true);
    (void)deliver_agent(agent, std::move(*key_up));

    std::cout << "simulating network silence past lease...\n";
    clock.advance(std::chrono::milliseconds(800));
    agent.tick();

    auto stale = host.key_event(KeyEventMessage{0x1D, false, true});
    if (stale) {
        auto decoded = decode_packet(*stale, false);
        if (decoded.packet) {
            std::cout << "stale decision=" << static_cast<int>(agent.handle(*decoded.packet)) << '\n';
        }
    }

    host.emergency_fail_local();
    std::cout << "host mode after emergency=" << static_cast<int>(host.desired_mode()) << '\n';
    return 0;
}
