#include "desklink/protocol.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <type_traits>

namespace desklink {
namespace {

class Writer {
public:
    void u8(std::uint8_t value) { bytes_.push_back(value); }
    void u16(std::uint16_t value) {
        bytes_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
        bytes_.push_back(static_cast<std::uint8_t>(value & 0xffu));
    }
    void i16(std::int16_t value) { u16(static_cast<std::uint16_t>(value)); }
    void u32(std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            bytes_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
        }
    }
    void i32(std::int32_t Value) { u32(static_cast<std::uint32_t>(Value)); }
    void u64(std::uint64_t value) {
        for (int shift = 56; shift >= 0; shift -= 8) {
            bytes_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
        }
    }
    void raw(ByteSpan bytes) { bytes_.insert(bytes_.end(), bytes.begin(), bytes.end()); }
    void string(std::string_view Value) {
        u16(static_cast<std::uint16_t>(Value.size()));
        raw(ByteSpan{
            reinterpret_cast<const std::uint8_t*>(Value.data()),
            Value.size()});
    }
    [[nodiscard]] ByteBuffer take() && { return std::move(bytes_); }

private:
    ByteBuffer bytes_;
};

class Reader {
public:
    explicit Reader(ByteSpan bytes) : bytes_(bytes) {}

    [[nodiscard]] bool u8(std::uint8_t& out) {
        if (remaining() < 1) return false;
        out = bytes_[offset_++];
        return true;
    }

    [[nodiscard]] bool u16(std::uint16_t& out) {
        if (remaining() < 2) return false;
        out = static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes_[offset_]) << 8) |
                                         static_cast<std::uint16_t>(bytes_[offset_ + 1]));
        offset_ += 2;
        return true;
    }

    [[nodiscard]] bool i16(std::int16_t& out) {
        std::uint16_t Raw{};
        if (!u16(Raw)) return false;
        const auto Signed = Raw <= 0x7fffu
            ? static_cast<std::int32_t>(Raw)
            : static_cast<std::int32_t>(Raw) - 0x1'0000;
        out = static_cast<std::int16_t>(Signed);
        return true;
    }

    [[nodiscard]] bool u32(std::uint32_t& out) {
        if (remaining() < 4) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            out = (out << 8) | bytes_[offset_ + static_cast<std::size_t>(i)];
        }
        offset_ += 4;
        return true;
    }

    [[nodiscard]] bool i32(std::int32_t& Out) {
        std::uint32_t Raw{};
        if (!u32(Raw)) return false;
        const auto Signed = Raw <= 0x7fffffffu
            ? static_cast<std::int64_t>(Raw)
            : static_cast<std::int64_t>(Raw) - 0x1'0000'0000ll;
        Out = static_cast<std::int32_t>(Signed);
        return true;
    }

    [[nodiscard]] bool u64(std::uint64_t& out) {
        if (remaining() < 8) return false;
        out = 0;
        for (int i = 0; i < 8; ++i) {
            out = (out << 8) | bytes_[offset_ + static_cast<std::size_t>(i)];
        }
        offset_ += 8;
        return true;
    }

    [[nodiscard]] bool raw(std::span<std::uint8_t> out) {
        if (remaining() < out.size()) return false;
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                    static_cast<std::ptrdiff_t>(out.size()), out.begin());
        offset_ += out.size();
        return true;
    }

    [[nodiscard]] bool raw_vector(std::size_t size, ByteBuffer& out) {
        if (remaining() < size) return false;
        out.assign(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                   bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + size));
        offset_ += size;
        return true;
    }

    [[nodiscard]] bool string(std::string& Out, std::size_t Maximum,
                              bool AllowEmpty) {
        std::uint16_t Length{};
        if (!u16(Length) || Length > Maximum ||
            (!AllowEmpty && Length == 0) || remaining() < Length) {
            return false;
        }
        Out.assign(
            reinterpret_cast<const char*>(bytes_.data() + offset_), Length);
        offset_ += Length;
        return Out.find('\0') == std::string::npos;
    }

    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

private:
    ByteSpan bytes_;
    std::size_t offset_{};
};

ByteBuffer encode_payload(const Message& message) {
    Writer w;
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, HelloMessage>) {
            w.raw(value.machine_id);
            w.u16(value.min_version);
            w.u16(value.max_version);
            w.u64(value.offered_capabilities);
        } else if constexpr (std::is_same_v<T, CapabilityGrantMessage>) {
            w.u64(value.capabilities);
            w.u64(value.revision);
        } else if constexpr (
            std::is_same_v<T, CapabilityGrantAckMessage>) {
            w.u64(value.capabilities);
            w.u64(value.revision);
        } else if constexpr (std::is_same_v<T, SetModeMessage>) {
            w.u8(static_cast<std::uint8_t>(value.mode));
        } else if constexpr (std::is_same_v<T, FocusRequestMessage>) {
            w.u32(value.requested_lease_ms);
            w.u64(value.request_id);
        } else if constexpr (std::is_same_v<T, FocusReadyMessage>) {
            w.u32(value.granted_lease_ms);
            w.u64(value.request_id);
        } else if constexpr (std::is_same_v<T, FocusRenewMessage>) {
            w.u32(value.requested_lease_ms);
        } else if constexpr (std::is_same_v<T, FocusReleaseMessage>) {
        } else if constexpr (std::is_same_v<T, KeyEventMessage>) {
            w.u16(value.scan_code);
            w.u8(value.extended ? 1u : 0u);
            w.u8(value.down ? 1u : 0u);
        } else if constexpr (std::is_same_v<T, MouseButtonMessage>) {
            w.u8(static_cast<std::uint8_t>(value.button));
            w.u8(value.down ? 1u : 0u);
        } else if constexpr (std::is_same_v<T, PointerPositionMessage>) {
            w.u16(value.display_id);
            w.u16(value.normalized_x);
            w.u16(value.normalized_y);
        } else if constexpr (std::is_same_v<T, PointerMotionMessage>) {
            w.i32(value.DeltaX);
            w.i32(value.DeltaY);
        } else if constexpr (
            std::is_same_v<T, PointerPositionFeedbackMessage>) {
            w.u16(value.DisplayId);
            w.u16(value.NormalizedX);
            w.u16(value.NormalizedY);
        } else if constexpr (std::is_same_v<T, InputStateSnapshotMessage>) {
            w.raw(value.KeyBitmap);
            w.raw(value.ExtendedKeyBitmap);
            w.u8(value.MouseButtonBitmap);
        } else if constexpr (std::is_same_v<T, MouseWheelMessage>) {
            w.u8(static_cast<std::uint8_t>(value.Axis));
            w.i16(value.Delta);
        } else if constexpr (std::is_same_v<T, SetAudioGainMessage>) {
            w.u16(value.gain_permyriad);
        } else if constexpr (std::is_same_v<T, AudioFrameMessage>) {
            w.u32(value.stream_id);
            w.u32(value.sample_rate);
            w.u16(value.frames_per_channel);
            w.u8(value.channels);
            w.u8(value.bytes_per_sample);
            w.u64(value.capture_timestamp_us);
            w.raw(value.pcm);
        } else if constexpr (
            std::is_same_v<T, DisplayTopologySnapshotMessage>) {
            w.raw(value.Machine);
            w.u64(value.SessionNonce);
            w.u64(value.Topology.Generation);
            w.i32(value.Topology.VirtualBounds.Left);
            w.i32(value.Topology.VirtualBounds.Top);
            w.i32(value.Topology.VirtualBounds.Right);
            w.i32(value.Topology.VirtualBounds.Bottom);
            w.u16(static_cast<std::uint16_t>(
                value.Topology.Displays.size()));
            for (const auto& Display : value.Topology.Displays) {
                w.u16(Display.Id);
                w.string(Display.StableIdentity);
                w.string(Display.FriendlyName);
                w.i32(Display.Bounds.Left);
                w.i32(Display.Bounds.Top);
                w.i32(Display.Bounds.Right);
                w.i32(Display.Bounds.Bottom);
                w.u8(Display.Primary ? 1u : 0u);
                w.u32(Display.PixelWidth);
                w.u32(Display.PixelHeight);
                w.u32(Display.RefreshMilliHertz);
                w.u16(Display.PhysicalWidthMillimeters);
                w.u16(Display.PhysicalHeightMillimeters);
                w.u8(static_cast<std::uint8_t>(Display.PhysicalSize));
                w.u8(static_cast<std::uint8_t>(Display.Orientation));
            }
        } else if constexpr (
            std::is_same_v<T, DisplayIdentifyRequestMessage>) {
            w.u16(value.FirstDisplayNumber);
        } else if constexpr (std::is_same_v<T, ClipboardHelloMessage>) {
            w.u16(value.Version);
            w.u32(value.MaximumTextBytes);
        } else if constexpr (std::is_same_v<T, ClipboardTextMessage>) {
            w.raw(value.OriginMachine);
            w.u64(value.UpdateId);
            w.u32(static_cast<std::uint32_t>(value.Text.size()));
            w.raw(ByteSpan{
                reinterpret_cast<const std::uint8_t*>(value.Text.data()),
                value.Text.size()});
        } else if constexpr (
            std::is_same_v<T, ClockSyncRequestMessage>) {
            w.u64(value.ProbeId);
            w.u64(value.OriginSendTimestampUs);
        } else if constexpr (
            std::is_same_v<T, ClockSyncResponseMessage>) {
            w.u64(value.ProbeId);
            w.u64(value.OriginSendTimestampUs);
            w.u64(value.RemoteReceiveTimestampUs);
            w.u64(value.RemoteSendTimestampUs);
        } else if constexpr (std::is_same_v<T, HeartbeatMessage>) {
        }
    }, message);
    return std::move(w).take();
}

bool valid_mode(std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(DeskMode::Game);
}

bool valid_button(std::uint8_t value) {
    return value >= static_cast<std::uint8_t>(MouseButtonId::Left) &&
           value <= static_cast<std::uint8_t>(MouseButtonId::X2);
}

bool ValidWheelAxis(std::uint8_t Value) {
    return Value >= static_cast<std::uint8_t>(MouseWheelAxis::Vertical) &&
           Value <= static_cast<std::uint8_t>(MouseWheelAxis::Horizontal);
}

std::optional<Message> decode_payload(MessageType type, ByteSpan payload) {
    Reader r(payload);
    switch (type) {
        case MessageType::Hello: {
            HelloMessage m;
            if (!r.raw(m.machine_id) || !r.u16(m.min_version) || !r.u16(m.max_version) ||
                !r.u64(m.offered_capabilities) || r.remaining() != 0 ||
                m.min_version > m.max_version) return std::nullopt;
            return m;
        }
        case MessageType::CapabilityGrant: {
            CapabilityGrantMessage m;
            if (!r.u64(m.capabilities) || !r.u64(m.revision) ||
                m.revision == 0 || r.remaining() != 0) {
                return std::nullopt;
            }
            return m;
        }
        case MessageType::CapabilityGrantAck: {
            CapabilityGrantAckMessage m;
            if (!r.u64(m.capabilities) || !r.u64(m.revision) ||
                m.revision == 0 || r.remaining() != 0) {
                return std::nullopt;
            }
            return m;
        }
        case MessageType::SetMode: {
            std::uint8_t raw{};
            if (!r.u8(raw) || !valid_mode(raw) || r.remaining() != 0) return std::nullopt;
            return SetModeMessage{static_cast<DeskMode>(raw)};
        }
        case MessageType::FocusRequest: {
            FocusRequestMessage m;
            if (!r.u32(m.requested_lease_ms) || !r.u64(m.request_id) || r.remaining() != 0 ||
                m.requested_lease_ms < 100 || m.requested_lease_ms > 5000 || m.request_id == 0) {
                return std::nullopt;
            }
            return m;
        }
        case MessageType::FocusReady: {
            FocusReadyMessage m;
            if (!r.u32(m.granted_lease_ms) || !r.u64(m.request_id) || r.remaining() != 0 ||
                m.granted_lease_ms < 100 || m.granted_lease_ms > 5000 || m.request_id == 0) {
                return std::nullopt;
            }
            return m;
        }
        case MessageType::FocusRenew: {
            FocusRenewMessage m;
            if (!r.u32(m.requested_lease_ms) || r.remaining() != 0 ||
                m.requested_lease_ms < 100 || m.requested_lease_ms > 5000) return std::nullopt;
            return m;
        }
        case MessageType::FocusRelease:
            if (r.remaining() != 0) return std::nullopt;
            return FocusReleaseMessage{};
        case MessageType::KeyEvent: {
            KeyEventMessage m;
            std::uint8_t ext{}, down{};
            if (!r.u16(m.scan_code) || !r.u8(ext) || !r.u8(down) || r.remaining() != 0 ||
                m.scan_code == 0 || m.scan_code > 255 || ext > 1 || down > 1) {
                return std::nullopt;
            }
            m.extended = ext != 0;
            m.down = down != 0;
            return m;
        }
        case MessageType::MouseButton: {
            MouseButtonMessage m;
            std::uint8_t button{}, down{};
            if (!r.u8(button) || !r.u8(down) || r.remaining() != 0 ||
                !valid_button(button) || down > 1) return std::nullopt;
            m.button = static_cast<MouseButtonId>(button);
            m.down = down != 0;
            return m;
        }
        case MessageType::PointerPosition: {
            PointerPositionMessage m;
            if (!r.u16(m.display_id) || !r.u16(m.normalized_x) || !r.u16(m.normalized_y) ||
                r.remaining() != 0) return std::nullopt;
            return m;
        }
        case MessageType::PointerMotion: {
            PointerMotionMessage Message;
            if (!r.i32(Message.DeltaX) || !r.i32(Message.DeltaY) ||
                r.remaining() != 0 || !IsValidPointerMotionMessage(Message)) {
                return std::nullopt;
            }
            return Message;
        }
        case MessageType::PointerPositionFeedback: {
            PointerPositionFeedbackMessage Message;
            if (!r.u16(Message.DisplayId) ||
                !r.u16(Message.NormalizedX) ||
                !r.u16(Message.NormalizedY) || r.remaining() != 0) {
                return std::nullopt;
            }
            return Message;
        }
        case MessageType::InputStateSnapshot: {
            InputStateSnapshotMessage m;
            if (!r.raw(m.KeyBitmap) || !r.raw(m.ExtendedKeyBitmap) ||
                !r.u8(m.MouseButtonBitmap) || r.remaining() != 0 ||
                (m.MouseButtonBitmap & 0xE0u) != 0) {
                return std::nullopt;
            }
            return m;
        }
        case MessageType::MouseWheel: {
            MouseWheelMessage Message;
            std::uint8_t Axis{};
            if (!r.u8(Axis) || !r.i16(Message.Delta) || r.remaining() != 0 ||
                !ValidWheelAxis(Axis)) {
                return std::nullopt;
            }
            Message.Axis = static_cast<MouseWheelAxis>(Axis);
            if (!IsValidMouseWheelMessage(Message)) return std::nullopt;
            return Message;
        }
        case MessageType::SetAudioGain: {
            SetAudioGainMessage m;
            if (!r.u16(m.gain_permyriad) || r.remaining() != 0 || m.gain_permyriad > 10000) {
                return std::nullopt;
            }
            return m;
        }
        case MessageType::AudioFrame: {
            AudioFrameMessage m;
            if (!r.u32(m.stream_id) || !r.u32(m.sample_rate) || !r.u16(m.frames_per_channel) ||
                !r.u8(m.channels) || !r.u8(m.bytes_per_sample) || !r.u64(m.capture_timestamp_us)) {
                return std::nullopt;
            }
            if (m.sample_rate < 8000 || m.sample_rate > 192000 ||
                m.frames_per_channel == 0 || m.frames_per_channel > 2048 ||
                m.channels == 0 || m.channels > 8 ||
                (m.bytes_per_sample != 2 && m.bytes_per_sample != 4)) {
                return std::nullopt;
            }
            const auto expected = static_cast<std::size_t>(m.frames_per_channel) *
                                  static_cast<std::size_t>(m.channels) *
                                  static_cast<std::size_t>(m.bytes_per_sample);
            if (expected != r.remaining() || expected > kMaxDatagramPayload) return std::nullopt;
            if (!r.raw_vector(expected, m.pcm) || r.remaining() != 0) return std::nullopt;
            return m;
        }
        case MessageType::ClockSyncRequest: {
            ClockSyncRequestMessage Message;
            if (!r.u64(Message.ProbeId) ||
                !r.u64(Message.OriginSendTimestampUs) ||
                Message.ProbeId == 0 || r.remaining() != 0) {
                return std::nullopt;
            }
            return Message;
        }
        case MessageType::ClockSyncResponse: {
            ClockSyncResponseMessage Message;
            if (!r.u64(Message.ProbeId) ||
                !r.u64(Message.OriginSendTimestampUs) ||
                !r.u64(Message.RemoteReceiveTimestampUs) ||
                !r.u64(Message.RemoteSendTimestampUs) ||
                Message.ProbeId == 0 ||
                Message.RemoteSendTimestampUs <
                    Message.RemoteReceiveTimestampUs ||
                r.remaining() != 0) {
                return std::nullopt;
            }
            return Message;
        }
        case MessageType::DisplayTopologySnapshot: {
            DisplayTopologySnapshotMessage Message;
            std::uint16_t DisplayCount{};
            if (!r.raw(Message.Machine) ||
                !r.u64(Message.SessionNonce) ||
                !r.u64(Message.Topology.Generation) ||
                !r.i32(Message.Topology.VirtualBounds.Left) ||
                !r.i32(Message.Topology.VirtualBounds.Top) ||
                !r.i32(Message.Topology.VirtualBounds.Right) ||
                !r.i32(Message.Topology.VirtualBounds.Bottom) ||
                !r.u16(DisplayCount) || DisplayCount == 0 ||
                DisplayCount > kMaxDisplayCount) {
                return std::nullopt;
            }
            Message.Topology.Displays.reserve(DisplayCount);
            for (std::size_t Index = 0; Index < DisplayCount; ++Index) {
                DisplayDescriptor Display;
                std::uint8_t Primary{};
                std::uint8_t PhysicalSize{};
                std::uint8_t Orientation{};
                if (!r.u16(Display.Id) ||
                    !r.string(Display.StableIdentity,
                              kMaxDisplayIdentityLength, false) ||
                    !r.string(Display.FriendlyName,
                              kMaxDisplayFriendlyNameLength, true) ||
                    !r.i32(Display.Bounds.Left) ||
                    !r.i32(Display.Bounds.Top) ||
                    !r.i32(Display.Bounds.Right) ||
                    !r.i32(Display.Bounds.Bottom) ||
                    !r.u8(Primary) || Primary > 1 ||
                    !r.u32(Display.PixelWidth) ||
                    !r.u32(Display.PixelHeight) ||
                    !r.u32(Display.RefreshMilliHertz) ||
                    !r.u16(Display.PhysicalWidthMillimeters) ||
                    !r.u16(Display.PhysicalHeightMillimeters) ||
                    !r.u8(PhysicalSize) || !r.u8(Orientation)) {
                    return std::nullopt;
                }
                Display.Primary = Primary != 0;
                Display.PhysicalSize =
                    static_cast<PhysicalSizeSource>(PhysicalSize);
                Display.Orientation =
                    static_cast<DisplayOrientation>(Orientation);
                Message.Topology.Displays.push_back(std::move(Display));
            }
            if (r.remaining() != 0 ||
                !IsValidDisplayTopologySnapshotMessage(Message)) {
                return std::nullopt;
            }
            return Message;
        }
        case MessageType::DisplayIdentifyRequest: {
            DisplayIdentifyRequestMessage Message;
            if (!r.u16(Message.FirstDisplayNumber) || r.remaining() != 0 ||
                !IsValidDisplayIdentifyRequestMessage(Message)) {
                return std::nullopt;
            }
            return Message;
        }
        case MessageType::ClipboardHello: {
            ClipboardHelloMessage Message;
            if (!r.u16(Message.Version) ||
                !r.u32(Message.MaximumTextBytes) || r.remaining() != 0 ||
                !IsValidClipboardHelloMessage(Message)) {
                return std::nullopt;
            }
            return Message;
        }
        case MessageType::ClipboardText: {
            ClipboardTextMessage Message;
            std::uint32_t TextSize{};
            ByteBuffer Text;
            if (!r.raw(Message.OriginMachine) || !r.u64(Message.UpdateId) ||
                !r.u32(TextSize) || TextSize > kMaximumClipboardTextBytes ||
                TextSize != r.remaining() ||
                !r.raw_vector(TextSize, Text) || r.remaining() != 0) {
                return std::nullopt;
            }
            if (!Text.empty()) {
                Message.Text.assign(
                    reinterpret_cast<const char*>(Text.data()), Text.size());
            }
            if (!IsValidClipboardTextMessage(Message)) {
                return std::nullopt;
            }
            return Message;
        }
        case MessageType::Heartbeat:
            if (r.remaining() != 0) return std::nullopt;
            return HeartbeatMessage{};
        default:
            return std::nullopt;
    }
}

bool known_type(std::uint16_t raw) {
    switch (static_cast<MessageType>(raw)) {
        case MessageType::Hello:
        case MessageType::CapabilityGrant:
        case MessageType::CapabilityGrantAck:
        case MessageType::SetMode:
        case MessageType::FocusRequest:
        case MessageType::FocusReady:
        case MessageType::FocusRenew:
        case MessageType::FocusRelease:
        case MessageType::KeyEvent:
        case MessageType::MouseButton:
        case MessageType::PointerPosition:
        case MessageType::PointerMotion:
        case MessageType::PointerPositionFeedback:
        case MessageType::InputStateSnapshot:
        case MessageType::MouseWheel:
        case MessageType::SetAudioGain:
        case MessageType::AudioFrame:
        case MessageType::DisplayTopologySnapshot:
        case MessageType::DisplayIdentifyRequest:
        case MessageType::ClipboardHello:
        case MessageType::ClipboardText:
        case MessageType::ClockSyncRequest:
        case MessageType::ClockSyncResponse:
        case MessageType::Heartbeat:
            return true;
        default:
            return false;
    }
}

} // namespace

MessageType message_type(const Message& message) noexcept {
    return std::visit([](const auto& value) -> MessageType {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, HelloMessage>) return MessageType::Hello;
        else if constexpr (std::is_same_v<T, CapabilityGrantMessage>) return MessageType::CapabilityGrant;
        else if constexpr (std::is_same_v<T, CapabilityGrantAckMessage>) return MessageType::CapabilityGrantAck;
        else if constexpr (std::is_same_v<T, SetModeMessage>) return MessageType::SetMode;
        else if constexpr (std::is_same_v<T, FocusRequestMessage>) return MessageType::FocusRequest;
        else if constexpr (std::is_same_v<T, FocusReadyMessage>) return MessageType::FocusReady;
        else if constexpr (std::is_same_v<T, FocusRenewMessage>) return MessageType::FocusRenew;
        else if constexpr (std::is_same_v<T, FocusReleaseMessage>) return MessageType::FocusRelease;
        else if constexpr (std::is_same_v<T, KeyEventMessage>) return MessageType::KeyEvent;
        else if constexpr (std::is_same_v<T, MouseButtonMessage>) return MessageType::MouseButton;
        else if constexpr (std::is_same_v<T, PointerPositionMessage>) return MessageType::PointerPosition;
        else if constexpr (std::is_same_v<T, PointerMotionMessage>) return MessageType::PointerMotion;
        else if constexpr (std::is_same_v<T, PointerPositionFeedbackMessage>) {
            return MessageType::PointerPositionFeedback;
        }
        else if constexpr (std::is_same_v<T, InputStateSnapshotMessage>) return MessageType::InputStateSnapshot;
        else if constexpr (std::is_same_v<T, MouseWheelMessage>) return MessageType::MouseWheel;
        else if constexpr (std::is_same_v<T, SetAudioGainMessage>) return MessageType::SetAudioGain;
        else if constexpr (std::is_same_v<T, AudioFrameMessage>) return MessageType::AudioFrame;
        else if constexpr (std::is_same_v<T, DisplayTopologySnapshotMessage>) {
            return MessageType::DisplayTopologySnapshot;
        }
        else if constexpr (std::is_same_v<T, DisplayIdentifyRequestMessage>) {
            return MessageType::DisplayIdentifyRequest;
        }
        else if constexpr (std::is_same_v<T, ClipboardHelloMessage>) {
            return MessageType::ClipboardHello;
        }
        else if constexpr (std::is_same_v<T, ClipboardTextMessage>) {
            return MessageType::ClipboardText;
        }
        else if constexpr (std::is_same_v<T, ClockSyncRequestMessage>) {
            return MessageType::ClockSyncRequest;
        }
        else if constexpr (std::is_same_v<T, ClockSyncResponseMessage>) {
            return MessageType::ClockSyncResponse;
        }
        else return MessageType::Heartbeat;
    }, message);
}

bool is_datagram_message(MessageType type) noexcept {
    return type == MessageType::PointerPosition ||
           type == MessageType::PointerMotion ||
           type == MessageType::PointerPositionFeedback ||
           type == MessageType::AudioFrame;
}

bool SetInputSnapshotKey(InputStateSnapshotMessage& Snapshot,
                         std::uint16_t ScanCode,
                         bool Extended,
                         bool Down) noexcept {
    if (ScanCode == 0 || ScanCode > 255) return false;
    auto& Bitmap = Extended ? Snapshot.ExtendedKeyBitmap : Snapshot.KeyBitmap;
    const auto ByteIndex = static_cast<std::size_t>(ScanCode / 8u);
    const auto Mask = static_cast<std::uint8_t>(1u << (ScanCode % 8u));
    if (Down) Bitmap[ByteIndex] |= Mask;
    else Bitmap[ByteIndex] &= static_cast<std::uint8_t>(~Mask);
    return true;
}

bool InputSnapshotKeyDown(const InputStateSnapshotMessage& Snapshot,
                          std::uint16_t ScanCode,
                          bool Extended) noexcept {
    if (ScanCode == 0 || ScanCode > 255) return false;
    const auto& Bitmap = Extended ? Snapshot.ExtendedKeyBitmap : Snapshot.KeyBitmap;
    const auto ByteIndex = static_cast<std::size_t>(ScanCode / 8u);
    const auto Mask = static_cast<std::uint8_t>(1u << (ScanCode % 8u));
    return (Bitmap[ByteIndex] & Mask) != 0;
}

bool SetInputSnapshotButton(InputStateSnapshotMessage& Snapshot,
                            MouseButtonId Button,
                            bool Down) noexcept {
    const auto Raw = static_cast<std::uint8_t>(Button);
    if (!valid_button(Raw)) return false;
    const auto Mask = static_cast<std::uint8_t>(1u << (Raw - 1u));
    if (Down) Snapshot.MouseButtonBitmap |= Mask;
    else Snapshot.MouseButtonBitmap &= static_cast<std::uint8_t>(~Mask);
    return true;
}

bool InputSnapshotButtonDown(const InputStateSnapshotMessage& Snapshot,
                             MouseButtonId Button) noexcept {
    const auto Raw = static_cast<std::uint8_t>(Button);
    if (!valid_button(Raw)) return false;
    const auto Mask = static_cast<std::uint8_t>(1u << (Raw - 1u));
    return (Snapshot.MouseButtonBitmap & Mask) != 0;
}

bool IsValidMouseWheelMessage(const MouseWheelMessage& Message) noexcept {
    const auto Axis = static_cast<std::uint8_t>(Message.Axis);
    return ValidWheelAxis(Axis) && Message.Delta != 0 &&
           Message.Delta >= -kMaximumMouseWheelDelta &&
           Message.Delta <= kMaximumMouseWheelDelta;
}

bool IsValidPointerMotionMessage(const PointerMotionMessage& Message) noexcept {
    return (Message.DeltaX != 0 || Message.DeltaY != 0) &&
           Message.DeltaX >= -kMaximumPointerMotionDelta &&
           Message.DeltaX <= kMaximumPointerMotionDelta &&
           Message.DeltaY >= -kMaximumPointerMotionDelta &&
           Message.DeltaY <= kMaximumPointerMotionDelta;
}

bool IsValidDisplayTopologySnapshotMessage(
    const DisplayTopologySnapshotMessage& Message) {
    const auto NonzeroMachine = std::any_of(
        Message.Machine.begin(), Message.Machine.end(),
        [](std::uint8_t Byte) { return Byte != 0; });
    if (!NonzeroMachine || Message.SessionNonce == 0 ||
        !IsValidDisplayTopologySnapshot(Message.Topology)) {
        return false;
    }
    constexpr std::size_t FixedPayloadSize =
        sizeof(MachineId) + sizeof(std::uint64_t) +
        sizeof(std::uint64_t) + 4 * sizeof(std::int32_t) +
        sizeof(std::uint16_t);
    constexpr std::size_t FixedDisplaySize =
        3 * sizeof(std::uint16_t) + 4 * sizeof(std::int32_t) +
        sizeof(std::uint8_t) + 3 * sizeof(std::uint32_t) +
        2 * sizeof(std::uint16_t) + 2 * sizeof(std::uint8_t);
    std::size_t PayloadSize = FixedPayloadSize;
    for (const auto& Display : Message.Topology.Displays) {
        const auto DisplaySize = FixedDisplaySize +
            Display.StableIdentity.size() + Display.FriendlyName.size();
        if (DisplaySize > kMaxReliablePayload - PayloadSize) return false;
        PayloadSize += DisplaySize;
    }
    return true;
}

bool IsValidDisplayIdentifyRequestMessage(
    const DisplayIdentifyRequestMessage& Message) noexcept {
    return Message.FirstDisplayNumber != 0 &&
           Message.FirstDisplayNumber <= kMaxDisplayCount;
}

std::optional<MessageType> PeekMessageType(ByteSpan Bytes) noexcept {
    if (Bytes.size() < 8) return std::nullopt;
    Reader Reader(Bytes);
    std::uint32_t Magic{};
    std::uint16_t Version{};
    std::uint16_t RawType{};
    if (!Reader.u32(Magic) || !Reader.u16(Version) ||
        !Reader.u16(RawType) || Magic != kWireMagic ||
        Version != kProtocolVersion || !known_type(RawType)) {
        return std::nullopt;
    }
    return static_cast<MessageType>(RawType);
}

ByteBuffer encode_packet(const EnvelopeHeader& requested_header, const Message& message) {
    auto payload = encode_payload(message);
    EnvelopeHeader header = requested_header;
    header.magic = kWireMagic;
    header.version = kProtocolVersion;
    header.type = message_type(message);
    header.payload_size = static_cast<std::uint32_t>(payload.size());

    Writer w;
    w.u32(header.magic);
    w.u16(header.version);
    w.u16(static_cast<std::uint16_t>(header.type));
    w.u32(header.payload_size);
    w.u64(header.session_nonce);
    w.u64(header.epoch);
    w.u64(header.sequence);
    w.raw(payload);
    return std::move(w).take();
}

DecodeResult decode_packet(ByteSpan bytes, bool datagram) {
    constexpr std::size_t header_size = 36;
    if (bytes.size() < header_size) {
        return {std::nullopt, DecodeError::Truncated, "packet shorter than envelope header"};
    }

    Reader r(bytes);
    EnvelopeHeader h;
    std::uint16_t raw_type{};
    if (!r.u32(h.magic) || !r.u16(h.version) || !r.u16(raw_type) || !r.u32(h.payload_size) ||
        !r.u64(h.session_nonce) || !r.u64(h.epoch) || !r.u64(h.sequence)) {
        return {std::nullopt, DecodeError::Truncated, "failed to read envelope"};
    }

    if (h.magic != kWireMagic) {
        return {std::nullopt, DecodeError::BadMagic, "wire magic mismatch"};
    }
    if (h.version != kProtocolVersion) {
        return {std::nullopt, DecodeError::UnsupportedVersion, "unsupported protocol version"};
    }
    if (!known_type(raw_type)) {
        return {std::nullopt, DecodeError::UnknownMessageType, "unknown message type"};
    }
    h.type = static_cast<MessageType>(raw_type);

    const auto max_payload = datagram ? kMaxDatagramPayload : kMaxReliablePayload;
    if (h.payload_size > max_payload) {
        return {std::nullopt, DecodeError::PayloadTooLarge, "payload exceeds lane limit"};
    }
    if (r.remaining() < h.payload_size) {
        return {std::nullopt, DecodeError::Truncated, "payload truncated"};
    }
    if (r.remaining() != h.payload_size) {
        return {std::nullopt, DecodeError::TrailingData, "packet contains trailing data"};
    }
    if (is_datagram_message(h.type) != datagram && h.type != MessageType::Heartbeat) {
        return {std::nullopt, DecodeError::InvalidPayload, "message used on wrong transport lane"};
    }

    const auto payload = bytes.subspan(header_size, h.payload_size);
    auto message = decode_payload(h.type, payload);
    if (!message.has_value()) {
        return {std::nullopt, DecodeError::InvalidPayload, "message payload failed validation"};
    }

    return {DecodedPacket{h, std::move(*message)}, DecodeError::None, {}};
}

} // namespace desklink
