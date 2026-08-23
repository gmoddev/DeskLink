#pragma once

#include "desklink/protocol.hpp"

#include <cstdint>
#include <optional>

namespace desklink {

class HostCoordinator {
public:
    explicit HostCoordinator(std::uint64_t session_nonce) noexcept;

    [[nodiscard]] DeskMode desired_mode() const noexcept { return desired_mode_; }
    [[nodiscard]] std::uint64_t remote_epoch() const noexcept { return remote_epoch_; }
    [[nodiscard]] bool remote_focused() const noexcept { return remote_epoch_ != 0; }

    [[nodiscard]] ByteBuffer set_mode(DeskMode mode);
    [[nodiscard]] ByteBuffer request_remote_focus(std::uint32_t lease_ms = 750);
    [[nodiscard]] bool accept_focus_ready(const DecodedPacket& packet) noexcept;
    [[nodiscard]] std::optional<ByteBuffer> renew_remote_focus(std::uint32_t lease_ms = 750);
    [[nodiscard]] std::optional<ByteBuffer> release_remote_focus();

    [[nodiscard]] std::optional<ByteBuffer> key_event(KeyEventMessage event);
    [[nodiscard]] std::optional<ByteBuffer> mouse_button(MouseButtonMessage event);
    [[nodiscard]] std::optional<ByteBuffer> pointer_position(PointerPositionMessage event);
    [[nodiscard]] std::optional<ByteBuffer> MouseWheel(MouseWheelMessage Message);
    [[nodiscard]] std::optional<ByteBuffer> InputStateSnapshot();

    void emergency_fail_local() noexcept;

private:
    [[nodiscard]] EnvelopeHeader next_header(std::uint64_t epoch, bool datagram) noexcept;

    std::uint64_t session_nonce_{};
    std::uint64_t reliable_sequence_{1};
    std::uint64_t datagram_sequence_{1};
    std::uint64_t remote_epoch_{};
    std::uint64_t next_focus_request_id_{1};
    std::uint64_t pending_focus_request_id_{};
    DeskMode desired_mode_{DeskMode::Roam};
    InputStateSnapshotMessage InputState_{};
};

} // namespace desklink
