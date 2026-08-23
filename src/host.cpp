#include "desklink/host.hpp"

namespace desklink {

HostCoordinator::HostCoordinator(std::uint64_t session_nonce) noexcept
    : session_nonce_(session_nonce) {}

EnvelopeHeader HostCoordinator::next_header(std::uint64_t epoch, bool datagram) noexcept {
    EnvelopeHeader h;
    h.session_nonce = session_nonce_;
    h.epoch = epoch;
    h.sequence = datagram ? datagram_sequence_++ : reliable_sequence_++;
    return h;
}

ByteBuffer HostCoordinator::set_mode(DeskMode mode) {
    desired_mode_ = mode;
    if (mode == DeskMode::Game || mode == DeskMode::LockPc1) {
        remote_epoch_ = 0;
        pending_focus_request_id_ = 0;
        InputState_ = {};
    }
    auto header = next_header(remote_epoch_, false);
    return encode_packet(header, SetModeMessage{mode});
}

ByteBuffer HostCoordinator::request_remote_focus(std::uint32_t lease_ms) {
    auto header = next_header(0, false);
    const auto request_id = next_focus_request_id_++;
    if (next_focus_request_id_ == 0) ++next_focus_request_id_;
    pending_focus_request_id_ = request_id;
    return encode_packet(header, FocusRequestMessage{lease_ms, request_id});
}

bool HostCoordinator::accept_focus_ready(const DecodedPacket& packet) noexcept {
    if (packet.header.type != MessageType::FocusReady || packet.header.session_nonce != session_nonce_ ||
        packet.header.epoch == 0 || pending_focus_request_id_ == 0) {
        return false;
    }
    const auto& ready = std::get<FocusReadyMessage>(packet.message);
    if (ready.request_id != pending_focus_request_id_) return false;
    pending_focus_request_id_ = 0;
    remote_epoch_ = packet.header.epoch;
    InputState_ = {};
    return true;
}

std::optional<ByteBuffer> HostCoordinator::renew_remote_focus(std::uint32_t lease_ms) {
    if (remote_epoch_ == 0) return std::nullopt;
    auto header = next_header(remote_epoch_, false);
    return encode_packet(header, FocusRenewMessage{lease_ms});
}

std::optional<ByteBuffer> HostCoordinator::release_remote_focus() {
    if (remote_epoch_ == 0) return std::nullopt;
    const auto epoch = remote_epoch_;
    remote_epoch_ = 0;
    pending_focus_request_id_ = 0;
    InputState_ = {};
    auto header = next_header(epoch, false);
    return encode_packet(header, FocusReleaseMessage{});
}

std::optional<ByteBuffer> HostCoordinator::key_event(KeyEventMessage event) {
    if (remote_epoch_ == 0) return std::nullopt;
    if (!SetInputSnapshotKey(InputState_, event.scan_code, event.extended, event.down)) {
        return std::nullopt;
    }
    auto header = next_header(remote_epoch_, false);
    return encode_packet(header, event);
}

std::optional<ByteBuffer> HostCoordinator::mouse_button(MouseButtonMessage event) {
    if (remote_epoch_ == 0) return std::nullopt;
    if (!SetInputSnapshotButton(InputState_, event.button, event.down)) return std::nullopt;
    auto header = next_header(remote_epoch_, false);
    return encode_packet(header, event);
}

std::optional<ByteBuffer> HostCoordinator::pointer_position(PointerPositionMessage event) {
    if (remote_epoch_ == 0) return std::nullopt;
    auto header = next_header(remote_epoch_, true);
    return encode_packet(header, event);
}

std::optional<ByteBuffer> HostCoordinator::MouseWheel(MouseWheelMessage Message) {
    if (remote_epoch_ == 0 || !IsValidMouseWheelMessage(Message)) {
        return std::nullopt;
    }
    auto Header = next_header(remote_epoch_, false);
    return encode_packet(Header, Message);
}

std::optional<ByteBuffer> HostCoordinator::InputStateSnapshot() {
    if (remote_epoch_ == 0) return std::nullopt;
    auto Header = next_header(remote_epoch_, false);
    return encode_packet(Header, InputState_);
}

void HostCoordinator::emergency_fail_local() noexcept {
    desired_mode_ = DeskMode::LockPc1;
    remote_epoch_ = 0;
    pending_focus_request_id_ = 0;
    InputState_ = {};
}

} // namespace desklink
