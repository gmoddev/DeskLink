#pragma once

#include "desklink/protocol.hpp"
#include "desklink/product.hpp"
#include "desklink/topology_exchange.hpp"
#include "desklink/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace desklink {

inline constexpr std::uint32_t kControlWireMagic = 0x444C4354u; // "DLCT"
inline constexpr std::uint16_t kControlProtocolVersion = 1;
inline constexpr std::size_t kMaximumControlPayload = 512u * 1024u;
inline constexpr std::size_t kMaximumControlTopologyMachines = 8;
inline constexpr std::size_t kMaximumControlTrustedDevices = 64;
inline constexpr std::size_t kMaximumControlDisplayName = 64;
inline constexpr std::size_t kControlFrameHeaderSize = 20;
inline constexpr std::size_t kMaximumControlFrameSize =
    kControlFrameHeaderSize + kMaximumControlPayload;

enum class ControlFrameType : std::uint16_t {
    Request = 1,
    Response = 2,
};

enum class ControlCommand : std::uint16_t {
    GetState = 1,
    SetDesiredMode = 2,
    FocusMachine = 3,
    SetAudioGain = 4,
    ToggleAudioMute = 5,
    GetDisplayTopologies = 6,
    PrepareForUpdate = 7,
    GetProductPreferences = 8,
    SetProductPreferences = 9,
    ListTrustedDevices = 10,
    RequestLocalPermissionChange = 11,
    ForgetTrustedDevice = 12,
    ReturnLocal = 13,
    GetPairingCandidate = 14,
};

enum class ControlStatus : std::uint16_t {
    Ok = 0,
    InvalidRequest = 1,
    Unsupported = 2,
    NotReady = 3,
    Failed = 4,
    ReauthorizationRequired = 5,
    CleanupFailed = 6,
};

enum class ControlRole : std::uint8_t {
    Idle = 0,
    Agent = 1,
    Host = 2,
};

struct GetStateControlRequest {};

struct SetDesiredModeControlRequest {
    DeskMode Mode{DeskMode::Roam};
};

struct FocusMachineControlRequest {
    MachineId Machine{};
};

struct SetAudioGainControlRequest {
    std::uint16_t GainPermyriad{};
};

struct ToggleAudioMuteControlRequest {};

struct GetDisplayTopologiesControlRequest {};

struct PrepareForUpdateControlRequest {};

struct GetProductPreferencesControlRequest {};

struct SetProductPreferencesControlRequest {
    ProductPreferences Preferences;
};

struct ListTrustedDevicesControlRequest {};

struct RequestLocalPermissionChangeControlRequest {
    MachineId Machine{};
    CapabilitySet DesiredCapabilities;
};

struct ForgetTrustedDeviceControlRequest {
    MachineId Machine{};
};

struct ReturnLocalControlRequest {};

struct GetPairingCandidateControlRequest {};

using ControlRequestPayload = std::variant<
    GetStateControlRequest,
    SetDesiredModeControlRequest,
    FocusMachineControlRequest,
    SetAudioGainControlRequest,
    ToggleAudioMuteControlRequest,
    GetDisplayTopologiesControlRequest,
    PrepareForUpdateControlRequest,
    GetProductPreferencesControlRequest,
    SetProductPreferencesControlRequest,
    ListTrustedDevicesControlRequest,
    RequestLocalPermissionChangeControlRequest,
    ForgetTrustedDeviceControlRequest,
    ReturnLocalControlRequest,
    GetPairingCandidateControlRequest>;

struct ControlRequest {
    std::uint64_t RequestId{};
    ControlRequestPayload Payload;
};

struct ControlState {
    MachineId LocalMachine{};
    MachineId FocusedMachine{};
    ControlRole Role{ControlRole::Idle};
    DeskMode DesiredMode{DeskMode::Roam};
    std::uint16_t ConnectedPeerCount{};
    std::uint16_t AudioGainPermyriad{10'000};
    bool RemoteFocused{};
    bool CaptureActive{};
    bool AudioMuted{};
};

struct ControlMachineTopology {
    MachineId Machine{};
    DisplayTopologyExchangeStatus Status{
        DisplayTopologyExchangeStatus::Offline};
    std::optional<DisplayTopologySnapshot> Topology;
    bool Local{};
    bool PeerInputAllowed{};

    [[nodiscard]] bool operator==(
        const ControlMachineTopology&) const noexcept = default;
};

struct ControlTopologyState {
    std::vector<ControlMachineTopology> Machines;

    [[nodiscard]] bool operator==(
        const ControlTopologyState&) const noexcept = default;
};

struct ControlTrustedDevice {
    MachineId Machine{};
    std::string DisplayName;
    CapabilitySet Capabilities;

    [[nodiscard]] bool operator==(
        const ControlTrustedDevice&) const noexcept = default;
};

struct ControlTrustedDeviceList {
    std::vector<ControlTrustedDevice> Devices;

    [[nodiscard]] bool operator==(
        const ControlTrustedDeviceList&) const noexcept = default;
};

struct ControlPairingCandidate {
    std::uint64_t OperationId{};
    MachineId Machine{};
    std::string DisplayName;
    std::string VerificationCode;
    CapabilitySet RequestedCapabilities;

    [[nodiscard]] bool operator==(
        const ControlPairingCandidate&) const noexcept = default;
};

struct ControlResponse {
    std::uint64_t RequestId{};
    ControlStatus Status{ControlStatus::Failed};
    std::optional<ControlState> State;
    std::optional<ControlTopologyState> Topologies;
    std::optional<ProductPreferences> Preferences;
    std::optional<ControlTrustedDeviceList> TrustedDevices;
    std::optional<ControlPairingCandidate> PairingCandidate;
};

enum class ControlDecodeError {
    None,
    Truncated,
    InvalidHeader,
    Oversized,
    InvalidPayload,
};

template <typename DecodedType>
struct ControlDecodeResult {
    std::optional<DecodedType> Decoded;
    ControlDecodeError Error{ControlDecodeError::None};
};

[[nodiscard]] bool IsValidControlRequest(const ControlRequest& Request) noexcept;
[[nodiscard]] bool IsValidControlState(const ControlState& State) noexcept;
[[nodiscard]] bool IsValidControlTopologyState(
    const ControlTopologyState& State);
[[nodiscard]] bool IsValidControlTrustedDeviceList(
    const ControlTrustedDeviceList& Devices) noexcept;
[[nodiscard]] bool IsValidControlPairingCandidate(
    const ControlPairingCandidate& Candidate) noexcept;
[[nodiscard]] bool IsValidControlResponse(const ControlResponse& Response);
[[nodiscard]] std::optional<ByteBuffer> EncodeControlRequest(
    const ControlRequest& Request);
[[nodiscard]] ControlDecodeResult<ControlRequest> DecodeControlRequest(
    ByteSpan Frame);
[[nodiscard]] std::optional<ByteBuffer> EncodeControlResponse(
    const ControlResponse& Response);
[[nodiscard]] ControlDecodeResult<ControlResponse> DecodeControlResponse(
    ByteSpan Frame);

} // namespace desklink
