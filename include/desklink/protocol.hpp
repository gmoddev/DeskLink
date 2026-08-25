#pragma once

#include "desklink/display_topology.hpp"
#include "desklink/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace desklink {

enum class MessageType : std::uint16_t {
    Hello              = 1,
    CapabilityGrant    = 2,
    SetMode            = 10,
    FocusRequest       = 11,
    FocusReady         = 12,
    FocusRenew         = 13,
    FocusRelease       = 14,
    KeyEvent           = 20,
    MouseButton        = 21,
    PointerPosition    = 22,
    InputStateSnapshot = 23,
    MouseWheel         = 24,
    PointerMotion      = 25,
    SetAudioGain       = 30,
    AudioFrame         = 31,
    Heartbeat          = 40,
    DisplayTopologySnapshot = 50,
};

enum class DeskMode : std::uint8_t {
    Roam = 0,
    LockPc1 = 1,
    LockPc2 = 2,
    Game = 3,
};

enum class MouseButtonId : std::uint8_t {
    Left = 1,
    Right = 2,
    Middle = 3,
    X1 = 4,
    X2 = 5,
};

enum class MouseWheelAxis : std::uint8_t {
    Vertical = 1,
    Horizontal = 2,
};

inline constexpr std::int16_t kMaximumMouseWheelDelta = 1200;
inline constexpr std::int32_t kMaximumPointerMotionDelta = 1'000'000;

struct EnvelopeHeader {
    std::uint32_t magic{kWireMagic};
    std::uint16_t version{kProtocolVersion};
    MessageType type{MessageType::Heartbeat};
    std::uint32_t payload_size{};
    std::uint64_t session_nonce{};
    std::uint64_t epoch{};
    std::uint64_t sequence{};
};

struct HelloMessage {
    MachineId machine_id{};
    std::uint16_t min_version{kProtocolVersion};
    std::uint16_t max_version{kProtocolVersion};
    std::uint64_t offered_capabilities{};
};

struct CapabilityGrantMessage { std::uint64_t capabilities{}; };
struct SetModeMessage { DeskMode mode{DeskMode::Roam}; };
struct FocusRequestMessage {
    std::uint32_t requested_lease_ms{750};
    std::uint64_t request_id{};
};
struct FocusReadyMessage {
    std::uint32_t granted_lease_ms{750};
    std::uint64_t request_id{};
};
struct FocusRenewMessage { std::uint32_t requested_lease_ms{750}; };
struct FocusReleaseMessage {};
struct HeartbeatMessage {};

struct KeyEventMessage {
    std::uint16_t scan_code{};
    bool extended{};
    bool down{};
};

struct MouseButtonMessage {
    MouseButtonId button{MouseButtonId::Left};
    bool down{};
};

struct PointerPositionMessage {
    std::uint16_t display_id{};
    std::uint16_t normalized_x{};
    std::uint16_t normalized_y{};
};

// Relative Raw Input motion. PointerPosition remains reserved for explicit
// display mapping/warps and must not be used for ordinary mouse movement.
struct PointerMotionMessage {
    std::int32_t DeltaX{};
    std::int32_t DeltaY{};
};

struct MouseWheelMessage {
    MouseWheelAxis Axis{MouseWheelAxis::Vertical};
    std::int16_t Delta{};
};

struct InputStateSnapshotMessage {
    std::array<std::uint8_t, 32> KeyBitmap{}; // 256 non-extended scan-code slots
    std::array<std::uint8_t, 32> ExtendedKeyBitmap{}; // 256 extended scan-code slots
    std::uint8_t MouseButtonBitmap{};
};

struct SetAudioGainMessage {
    std::uint16_t gain_permyriad{10000}; // 0..10000 == 0..100%
};

struct AudioFrameMessage {
    std::uint32_t stream_id{};
    std::uint32_t sample_rate{48000};
    std::uint16_t frames_per_channel{240};
    std::uint8_t channels{2};
    std::uint8_t bytes_per_sample{2};
    std::uint64_t capture_timestamp_us{};
    ByteBuffer pcm;
};

struct DisplayTopologySnapshotMessage {
    MachineId Machine{};
    std::uint64_t SessionNonce{};
    DisplayTopologySnapshot Topology;

    [[nodiscard]] bool operator==(
        const DisplayTopologySnapshotMessage&) const noexcept = default;
};

using Message = std::variant<
    HelloMessage,
    CapabilityGrantMessage,
    SetModeMessage,
    FocusRequestMessage,
    FocusReadyMessage,
    FocusRenewMessage,
    FocusReleaseMessage,
    KeyEventMessage,
    MouseButtonMessage,
    PointerPositionMessage,
    PointerMotionMessage,
    InputStateSnapshotMessage,
    MouseWheelMessage,
    SetAudioGainMessage,
    AudioFrameMessage,
    HeartbeatMessage,
    DisplayTopologySnapshotMessage>;

struct DecodedPacket {
    EnvelopeHeader header;
    Message message;
};

enum class DecodeError {
    None,
    Truncated,
    BadMagic,
    UnsupportedVersion,
    UnknownMessageType,
    PayloadTooLarge,
    InvalidPayload,
    TrailingData,
};

struct DecodeResult {
    std::optional<DecodedPacket> packet;
    DecodeError error{DecodeError::None};
    std::string detail;
};

[[nodiscard]] MessageType message_type(const Message& message) noexcept;
[[nodiscard]] bool is_datagram_message(MessageType type) noexcept;
[[nodiscard]] bool SetInputSnapshotKey(InputStateSnapshotMessage& Snapshot,
                                       std::uint16_t ScanCode,
                                       bool Extended,
                                       bool Down) noexcept;
[[nodiscard]] bool InputSnapshotKeyDown(const InputStateSnapshotMessage& Snapshot,
                                        std::uint16_t ScanCode,
                                        bool Extended) noexcept;
[[nodiscard]] bool SetInputSnapshotButton(InputStateSnapshotMessage& Snapshot,
                                          MouseButtonId Button,
                                          bool Down) noexcept;
[[nodiscard]] bool InputSnapshotButtonDown(const InputStateSnapshotMessage& Snapshot,
                                           MouseButtonId Button) noexcept;
[[nodiscard]] bool IsValidMouseWheelMessage(
    const MouseWheelMessage& Message) noexcept;
[[nodiscard]] bool IsValidPointerMotionMessage(
    const PointerMotionMessage& Message) noexcept;
[[nodiscard]] bool IsValidDisplayTopologySnapshotMessage(
    const DisplayTopologySnapshotMessage& Message);
[[nodiscard]] std::optional<MessageType> PeekMessageType(
    ByteSpan Bytes) noexcept;
[[nodiscard]] ByteBuffer encode_packet(const EnvelopeHeader& header, const Message& message);
[[nodiscard]] DecodeResult decode_packet(ByteSpan bytes, bool datagram);

} // namespace desklink
