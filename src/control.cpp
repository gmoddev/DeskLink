#include "desklink/control.hpp"

#include <algorithm>
#include <array>
#include <type_traits>

namespace desklink {
namespace {

class Writer {
public:
    void U8(std::uint8_t Value) { Bytes_.push_back(Value); }
    void U16(std::uint16_t Value) {
        Bytes_.push_back(static_cast<std::uint8_t>((Value >> 8u) & 0xffu));
        Bytes_.push_back(static_cast<std::uint8_t>(Value & 0xffu));
    }
    void U32(std::uint32_t Value) {
        for (int Shift = 24; Shift >= 0; Shift -= 8) {
            Bytes_.push_back(static_cast<std::uint8_t>(
                (Value >> static_cast<unsigned>(Shift)) & 0xffu));
        }
    }
    void U64(std::uint64_t Value) {
        for (int Shift = 56; Shift >= 0; Shift -= 8) {
            Bytes_.push_back(static_cast<std::uint8_t>(
                (Value >> static_cast<unsigned>(Shift)) & 0xffu));
        }
    }
    void Raw(ByteSpan Value) { Bytes_.insert(Bytes_.end(), Value.begin(), Value.end()); }
    [[nodiscard]] ByteBuffer Take() { return std::move(Bytes_); }

private:
    ByteBuffer Bytes_;
};

class Reader {
public:
    explicit Reader(ByteSpan Bytes) : Bytes_(Bytes) {}

    [[nodiscard]] bool U8(std::uint8_t& Value) {
        if (Remaining() < 1) return false;
        Value = Bytes_[Offset_++];
        return true;
    }
    [[nodiscard]] bool U16(std::uint16_t& Value) {
        if (Remaining() < 2) return false;
        Value = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(Bytes_[Offset_]) << 8u) |
            static_cast<std::uint16_t>(Bytes_[Offset_ + 1]));
        Offset_ += 2;
        return true;
    }
    [[nodiscard]] bool U32(std::uint32_t& Value) {
        if (Remaining() < 4) return false;
        Value = 0;
        for (int Index = 0; Index < 4; ++Index) {
            Value = (Value << 8u) | Bytes_[Offset_++];
        }
        return true;
    }
    [[nodiscard]] bool U64(std::uint64_t& Value) {
        if (Remaining() < 8) return false;
        Value = 0;
        for (int Index = 0; Index < 8; ++Index) {
            Value = (Value << 8u) | Bytes_[Offset_++];
        }
        return true;
    }
    [[nodiscard]] bool Raw(std::span<std::uint8_t> Value) {
        if (Remaining() < Value.size()) return false;
        std::copy_n(Bytes_.begin() + static_cast<std::ptrdiff_t>(Offset_),
                    Value.size(), Value.begin());
        Offset_ += Value.size();
        return true;
    }
    [[nodiscard]] std::size_t Remaining() const noexcept {
        return Bytes_.size() - Offset_;
    }

private:
    ByteSpan Bytes_;
    std::size_t Offset_{};
};

struct FrameHeader {
    ControlFrameType Type{ControlFrameType::Request};
    std::uint64_t RequestId{};
    std::uint32_t PayloadSize{};
};

bool IsValidDeskMode(DeskMode Mode) noexcept {
    const auto Raw = static_cast<std::uint8_t>(Mode);
    return Raw >= static_cast<std::uint8_t>(DeskMode::Roam) &&
           Raw <= static_cast<std::uint8_t>(DeskMode::Game);
}

bool IsValidRole(ControlRole Role) noexcept {
    return static_cast<std::uint8_t>(Role) <=
           static_cast<std::uint8_t>(ControlRole::Host);
}

bool IsNonzeroMachine(const MachineId& Machine) noexcept {
    return std::any_of(Machine.begin(), Machine.end(),
                       [](std::uint8_t Byte) { return Byte != 0; });
}

bool HasOnlyKnownCapabilities(CapabilitySet Capabilities) noexcept {
    return (Capabilities.bits() & ~kKnownCapabilityBits) == 0;
}

void EncodeHeader(Writer& Output, ControlFrameType Type,
                  std::uint64_t RequestId, std::uint32_t PayloadSize) {
    Output.U32(kControlWireMagic);
    Output.U16(kControlProtocolVersion);
    Output.U16(static_cast<std::uint16_t>(Type));
    Output.U64(RequestId);
    Output.U32(PayloadSize);
}

ControlDecodeResult<FrameHeader> DecodeHeader(ByteSpan Frame,
                                              ControlFrameType ExpectedType) {
    if (Frame.size() < kControlFrameHeaderSize) {
        return {std::nullopt, ControlDecodeError::Truncated};
    }
    Reader Input(Frame.first(kControlFrameHeaderSize));
    std::uint32_t Magic{};
    std::uint16_t Version{};
    std::uint16_t RawType{};
    FrameHeader Header;
    if (!Input.U32(Magic) || !Input.U16(Version) || !Input.U16(RawType) ||
        !Input.U64(Header.RequestId) || !Input.U32(Header.PayloadSize)) {
        return {std::nullopt, ControlDecodeError::Truncated};
    }
    Header.Type = static_cast<ControlFrameType>(RawType);
    if (Magic != kControlWireMagic || Version != kControlProtocolVersion ||
        Header.Type != ExpectedType || Header.RequestId == 0) {
        return {std::nullopt, ControlDecodeError::InvalidHeader};
    }
    if (Header.PayloadSize > kMaximumControlPayload) {
        return {std::nullopt, ControlDecodeError::Oversized};
    }
    if (Frame.size() != kControlFrameHeaderSize + Header.PayloadSize) {
        return {std::nullopt,
                Frame.size() < kControlFrameHeaderSize + Header.PayloadSize
                    ? ControlDecodeError::Truncated
                    : ControlDecodeError::InvalidPayload};
    }
    return {Header, ControlDecodeError::None};
}

ControlCommand GetCommand(const ControlRequestPayload& Payload) noexcept {
    return std::visit([](const auto& Value) {
        using ValueType = std::decay_t<decltype(Value)>;
        if constexpr (std::is_same_v<ValueType, GetStateControlRequest>) {
            return ControlCommand::GetState;
        } else if constexpr (std::is_same_v<ValueType,
                                             SetDesiredModeControlRequest>) {
            return ControlCommand::SetDesiredMode;
        } else if constexpr (std::is_same_v<ValueType,
                                             FocusMachineControlRequest>) {
            return ControlCommand::FocusMachine;
        } else if constexpr (std::is_same_v<ValueType,
                                             SetAudioGainControlRequest>) {
            return ControlCommand::SetAudioGain;
        } else if constexpr (std::is_same_v<ValueType,
                                              ToggleAudioMuteControlRequest>) {
            return ControlCommand::ToggleAudioMute;
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 GetDisplayTopologiesControlRequest>) {
            return ControlCommand::GetDisplayTopologies;
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 PrepareForUpdateControlRequest>) {
            return ControlCommand::PrepareForUpdate;
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 GetProductPreferencesControlRequest>) {
            return ControlCommand::GetProductPreferences;
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 SetProductPreferencesControlRequest>) {
            return ControlCommand::SetProductPreferences;
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 ListTrustedDevicesControlRequest>) {
            return ControlCommand::ListTrustedDevices;
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 RequestLocalPermissionChangeControlRequest>) {
            return ControlCommand::RequestLocalPermissionChange;
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 ForgetTrustedDeviceControlRequest>) {
            return ControlCommand::ForgetTrustedDevice;
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 ReturnLocalControlRequest>) {
            return ControlCommand::ReturnLocal;
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 GetPairingCandidateControlRequest>) {
            return ControlCommand::GetPairingCandidate;
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 PauseDeskLinkControlRequest>) {
            return ControlCommand::PauseDeskLink;
        } else {
            return ControlCommand::ResumeDeskLink;
        }
    }, Payload);
}

ByteBuffer EncodeRequestPayload(const ControlRequest& Request) {
    Writer Output;
    Output.U16(static_cast<std::uint16_t>(GetCommand(Request.Payload)));
    std::visit([&](const auto& Value) {
        using ValueType = std::decay_t<decltype(Value)>;
        if constexpr (std::is_same_v<ValueType,
                                     SetDesiredModeControlRequest>) {
            Output.U8(static_cast<std::uint8_t>(Value.Mode));
        } else if constexpr (std::is_same_v<ValueType,
                                            FocusMachineControlRequest>) {
            Output.Raw(Value.Machine);
        } else if constexpr (std::is_same_v<ValueType,
                                            SetAudioGainControlRequest>) {
            Output.U16(Value.GainPermyriad);
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 SetProductPreferencesControlRequest>) {
            const auto& Preferences = Value.Preferences;
            Output.U8(static_cast<std::uint8_t>(Preferences.Role));
            Output.U8(static_cast<std::uint8_t>(Preferences.AudioRoute));
            Output.U8(static_cast<std::uint8_t>(Preferences.Gaming));
            std::uint16_t Flags = 0;
            if (Preferences.PreferredPeerMachine) Flags |= 0x0001u;
            if (Preferences.RunAtLogin) Flags |= 0x0002u;
            if (Preferences.CloseToTray) Flags |= 0x0004u;
            if (Preferences.AutoStartRuntime) Flags |= 0x0008u;
            if (Preferences.AutoConnect) Flags |= 0x0010u;
            if (Preferences.InputRoamingDesired) Flags |= 0x0020u;
            if (Preferences.ClipboardDesired) Flags |= 0x0040u;
            if (Preferences.AdvancedModeEnabled) Flags |= 0x0080u;
            if (Preferences.FirstRunComplete) Flags |= 0x0100u;
            Output.U16(Flags);
            Output.U16(Preferences.AudioGainPermyriad);
            if (Preferences.PreferredPeerMachine) {
                Output.Raw(*Preferences.PreferredPeerMachine);
            }
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 RequestLocalPermissionChangeControlRequest>) {
            Output.Raw(Value.Machine);
            Output.U64(Value.DesiredCapabilities.bits());
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 ForgetTrustedDeviceControlRequest>) {
            Output.Raw(Value.Machine);
        }
    }, Request.Payload);
    return Output.Take();
}

std::optional<ControlRequestPayload> DecodeRequestPayload(ByteSpan Payload) {
    Reader Input(Payload);
    std::uint16_t RawCommand{};
    if (!Input.U16(RawCommand)) return std::nullopt;
    switch (static_cast<ControlCommand>(RawCommand)) {
        case ControlCommand::GetState:
            if (Input.Remaining() != 0) return std::nullopt;
            return GetStateControlRequest{};
        case ControlCommand::SetDesiredMode: {
            std::uint8_t RawMode{};
            if (!Input.U8(RawMode) || Input.Remaining() != 0) return std::nullopt;
            SetDesiredModeControlRequest Request{static_cast<DeskMode>(RawMode)};
            if (!IsValidDeskMode(Request.Mode)) return std::nullopt;
            return Request;
        }
        case ControlCommand::FocusMachine: {
            FocusMachineControlRequest Request;
            if (!Input.Raw(Request.Machine) || Input.Remaining() != 0 ||
                !IsNonzeroMachine(Request.Machine)) {
                return std::nullopt;
            }
            return Request;
        }
        case ControlCommand::SetAudioGain: {
            SetAudioGainControlRequest Request;
            if (!Input.U16(Request.GainPermyriad) || Input.Remaining() != 0 ||
                Request.GainPermyriad > 10'000) {
                return std::nullopt;
            }
            return Request;
        }
        case ControlCommand::ToggleAudioMute:
            if (Input.Remaining() != 0) return std::nullopt;
            return ToggleAudioMuteControlRequest{};
        case ControlCommand::GetDisplayTopologies:
            if (Input.Remaining() != 0) return std::nullopt;
            return GetDisplayTopologiesControlRequest{};
        case ControlCommand::PrepareForUpdate:
            if (Input.Remaining() != 0) return std::nullopt;
            return PrepareForUpdateControlRequest{};
        case ControlCommand::GetProductPreferences:
            if (Input.Remaining() != 0) return std::nullopt;
            return GetProductPreferencesControlRequest{};
        case ControlCommand::SetProductPreferences: {
            std::uint8_t RawRole{};
            std::uint8_t RawAudioRoute{};
            std::uint8_t RawGaming{};
            std::uint16_t Flags{};
            SetProductPreferencesControlRequest Request;
            if (!Input.U8(RawRole) || !Input.U8(RawAudioRoute) ||
                !Input.U8(RawGaming) || !Input.U16(Flags) ||
                !Input.U16(Request.Preferences.AudioGainPermyriad) ||
                (Flags & 0xfe00u) != 0) {
                return std::nullopt;
            }
            Request.Preferences.Role = static_cast<DeskRole>(RawRole);
            Request.Preferences.AudioRoute =
                static_cast<AudioRoutePreference>(RawAudioRoute);
            Request.Preferences.Gaming = static_cast<GamingBehavior>(RawGaming);
            Request.Preferences.RunAtLogin = (Flags & 0x0002u) != 0;
            Request.Preferences.CloseToTray = (Flags & 0x0004u) != 0;
            Request.Preferences.AutoStartRuntime = (Flags & 0x0008u) != 0;
            Request.Preferences.AutoConnect = (Flags & 0x0010u) != 0;
            Request.Preferences.InputRoamingDesired = (Flags & 0x0020u) != 0;
            Request.Preferences.ClipboardDesired = (Flags & 0x0040u) != 0;
            Request.Preferences.AdvancedModeEnabled = (Flags & 0x0080u) != 0;
            Request.Preferences.FirstRunComplete = (Flags & 0x0100u) != 0;
            if ((Flags & 0x0001u) != 0) {
                MachineId Machine{};
                if (!Input.Raw(Machine)) return std::nullopt;
                Request.Preferences.PreferredPeerMachine = Machine;
            }
            if (Input.Remaining() != 0 ||
                !IsValidProductPreferences(Request.Preferences)) {
                return std::nullopt;
            }
            return Request;
        }
        case ControlCommand::ListTrustedDevices:
            if (Input.Remaining() != 0) return std::nullopt;
            return ListTrustedDevicesControlRequest{};
        case ControlCommand::RequestLocalPermissionChange: {
            RequestLocalPermissionChangeControlRequest Request;
            std::uint64_t CapabilityBits{};
            if (!Input.Raw(Request.Machine) || !Input.U64(CapabilityBits) ||
                Input.Remaining() != 0) {
                return std::nullopt;
            }
            Request.DesiredCapabilities = CapabilitySet(CapabilityBits);
            if (!IsNonzeroMachine(Request.Machine) ||
                !HasOnlyKnownCapabilities(Request.DesiredCapabilities)) {
                return std::nullopt;
            }
            return Request;
        }
        case ControlCommand::ForgetTrustedDevice: {
            ForgetTrustedDeviceControlRequest Request;
            if (!Input.Raw(Request.Machine) || Input.Remaining() != 0 ||
                !IsNonzeroMachine(Request.Machine)) {
                return std::nullopt;
            }
            return Request;
        }
        case ControlCommand::ReturnLocal:
            if (Input.Remaining() != 0) return std::nullopt;
            return ReturnLocalControlRequest{};
        case ControlCommand::GetPairingCandidate:
            if (Input.Remaining() != 0) return std::nullopt;
            return GetPairingCandidateControlRequest{};
        case ControlCommand::PauseDeskLink:
            if (Input.Remaining() != 0) return std::nullopt;
            return PauseDeskLinkControlRequest{};
        case ControlCommand::ResumeDeskLink:
            if (Input.Remaining() != 0) return std::nullopt;
            return ResumeDeskLinkControlRequest{};
        default:
            return std::nullopt;
    }
}

void EncodePreferences(Writer& Output,
                       const ProductPreferences& Preferences) {
    Output.U8(static_cast<std::uint8_t>(Preferences.Role));
    Output.U8(static_cast<std::uint8_t>(Preferences.AudioRoute));
    Output.U8(static_cast<std::uint8_t>(Preferences.Gaming));
    std::uint16_t Flags = 0;
    if (Preferences.PreferredPeerMachine) Flags |= 0x0001u;
    if (Preferences.RunAtLogin) Flags |= 0x0002u;
    if (Preferences.CloseToTray) Flags |= 0x0004u;
    if (Preferences.AutoStartRuntime) Flags |= 0x0008u;
    if (Preferences.AutoConnect) Flags |= 0x0010u;
    if (Preferences.InputRoamingDesired) Flags |= 0x0020u;
    if (Preferences.ClipboardDesired) Flags |= 0x0040u;
    if (Preferences.AdvancedModeEnabled) Flags |= 0x0080u;
    if (Preferences.FirstRunComplete) Flags |= 0x0100u;
    Output.U16(Flags);
    Output.U16(Preferences.AudioGainPermyriad);
    if (Preferences.PreferredPeerMachine) {
        Output.Raw(*Preferences.PreferredPeerMachine);
    }
}

std::optional<ProductPreferences> DecodePreferences(Reader& Input) {
    std::uint8_t RawRole{};
    std::uint8_t RawAudioRoute{};
    std::uint8_t RawGaming{};
    std::uint16_t Flags{};
    ProductPreferences Preferences;
    if (!Input.U8(RawRole) || !Input.U8(RawAudioRoute) ||
        !Input.U8(RawGaming) || !Input.U16(Flags) ||
        !Input.U16(Preferences.AudioGainPermyriad) ||
        (Flags & 0xfe00u) != 0) {
        return std::nullopt;
    }
    Preferences.Role = static_cast<DeskRole>(RawRole);
    Preferences.AudioRoute =
        static_cast<AudioRoutePreference>(RawAudioRoute);
    Preferences.Gaming = static_cast<GamingBehavior>(RawGaming);
    Preferences.RunAtLogin = (Flags & 0x0002u) != 0;
    Preferences.CloseToTray = (Flags & 0x0004u) != 0;
    Preferences.AutoStartRuntime = (Flags & 0x0008u) != 0;
    Preferences.AutoConnect = (Flags & 0x0010u) != 0;
    Preferences.InputRoamingDesired = (Flags & 0x0020u) != 0;
    Preferences.ClipboardDesired = (Flags & 0x0040u) != 0;
    Preferences.AdvancedModeEnabled = (Flags & 0x0080u) != 0;
    Preferences.FirstRunComplete = (Flags & 0x0100u) != 0;
    if ((Flags & 0x0001u) != 0) {
        MachineId Machine{};
        if (!Input.Raw(Machine)) return std::nullopt;
        Preferences.PreferredPeerMachine = Machine;
    }
    return IsValidProductPreferences(Preferences)
        ? std::optional<ProductPreferences>(Preferences)
        : std::nullopt;
}

bool IsBoundedDisplayName(std::string_view Name) noexcept {
    if (Name.empty() || Name.size() > kMaximumControlDisplayName) return false;
    return std::none_of(Name.begin(), Name.end(), [](unsigned char Byte) {
        return Byte == 0 || Byte == 0x7fu || Byte < 0x20u;
    });
}

void EncodeTrustedDevices(Writer& Output,
                          const ControlTrustedDeviceList& Devices) {
    Output.U8(static_cast<std::uint8_t>(Devices.Devices.size()));
    for (const auto& Device : Devices.Devices) {
        Output.Raw(Device.Machine);
        Output.U64(Device.Capabilities.bits());
        Output.U8(static_cast<std::uint8_t>(Device.DisplayName.size()));
        Output.Raw(ByteSpan{
            reinterpret_cast<const std::uint8_t*>(Device.DisplayName.data()),
            Device.DisplayName.size()});
    }
}

std::optional<ControlTrustedDeviceList> DecodeTrustedDevices(Reader& Input) {
    std::uint8_t Count{};
    if (!Input.U8(Count) || Count > kMaximumControlTrustedDevices) {
        return std::nullopt;
    }
    ControlTrustedDeviceList Result;
    Result.Devices.reserve(Count);
    for (std::uint8_t Index = 0; Index < Count; ++Index) {
        ControlTrustedDevice Device;
        std::uint64_t CapabilityBits{};
        std::uint8_t NameLength{};
        if (!Input.Raw(Device.Machine) || !Input.U64(CapabilityBits) ||
            !Input.U8(NameLength) || NameLength == 0 ||
            NameLength > kMaximumControlDisplayName ||
            Input.Remaining() < NameLength) {
            return std::nullopt;
        }
        ByteBuffer Name(NameLength);
        if (!Input.Raw(Name)) return std::nullopt;
        Device.DisplayName.assign(
            reinterpret_cast<const char*>(Name.data()), Name.size());
        Device.Capabilities = CapabilitySet(CapabilityBits);
        Result.Devices.push_back(std::move(Device));
    }
    return IsValidControlTrustedDeviceList(Result)
        ? std::optional<ControlTrustedDeviceList>(std::move(Result))
        : std::nullopt;
}

void EncodePairingCandidate(Writer& Output,
                            const ControlPairingCandidate& Candidate) {
    Output.U64(Candidate.OperationId);
    Output.Raw(Candidate.Machine);
    Output.U64(Candidate.RequestedCapabilities.bits());
    Output.U8(static_cast<std::uint8_t>(Candidate.DisplayName.size()));
    Output.Raw(ByteSpan{
        reinterpret_cast<const std::uint8_t*>(Candidate.DisplayName.data()),
        Candidate.DisplayName.size()});
    Output.Raw(ByteSpan{
        reinterpret_cast<const std::uint8_t*>(
            Candidate.VerificationCode.data()),
        Candidate.VerificationCode.size()});
}

std::optional<ControlPairingCandidate> DecodePairingCandidate(Reader& Input) {
    ControlPairingCandidate Candidate;
    std::uint64_t CapabilityBits{};
    std::uint8_t NameLength{};
    if (!Input.U64(Candidate.OperationId) || !Input.Raw(Candidate.Machine) ||
        !Input.U64(CapabilityBits) || !Input.U8(NameLength) ||
        NameLength == 0 || NameLength > kMaximumControlDisplayName ||
        Input.Remaining() < NameLength + 6u) {
        return std::nullopt;
    }
    ByteBuffer Name(NameLength);
    ByteBuffer Code(6);
    if (!Input.Raw(Name) || !Input.Raw(Code)) return std::nullopt;
    Candidate.DisplayName.assign(
        reinterpret_cast<const char*>(Name.data()), Name.size());
    Candidate.VerificationCode.assign(
        reinterpret_cast<const char*>(Code.data()), Code.size());
    Candidate.RequestedCapabilities = CapabilitySet(CapabilityBits);
    return IsValidControlPairingCandidate(Candidate)
        ? std::optional<ControlPairingCandidate>(std::move(Candidate))
        : std::nullopt;
}

void EncodeState(Writer& Output, const ControlState& State) {
    Output.Raw(State.LocalMachine);
    Output.Raw(State.FocusedMachine);
    Output.U8(static_cast<std::uint8_t>(State.Role));
    Output.U8(static_cast<std::uint8_t>(State.DesiredMode));
    Output.U16(State.ConnectedPeerCount);
    Output.U16(State.AudioGainPermyriad);
    Output.U16(State.RetryAttempt);
    Output.U8(static_cast<std::uint8_t>(State.RuntimePhase));
    Output.U8(static_cast<std::uint8_t>(State.RuntimeFailure));
    std::uint8_t Flags = 0;
    if (State.RemoteFocused) Flags |= 0x01u;
    if (State.CaptureActive) Flags |= 0x02u;
    if (State.AudioMuted) Flags |= 0x04u;
    Output.U8(Flags);
}

std::optional<ControlState> DecodeState(Reader& Input) {
    ControlState State;
    std::uint8_t RawRole{};
    std::uint8_t RawMode{};
    std::uint8_t RawRuntimePhase{};
    std::uint8_t RawRuntimeFailure{};
    std::uint8_t Flags{};
    if (!Input.Raw(State.LocalMachine) || !Input.Raw(State.FocusedMachine) ||
        !Input.U8(RawRole) || !Input.U8(RawMode) ||
        !Input.U16(State.ConnectedPeerCount) ||
        !Input.U16(State.AudioGainPermyriad) ||
        !Input.U16(State.RetryAttempt) ||
        !Input.U8(RawRuntimePhase) || !Input.U8(RawRuntimeFailure) ||
        !Input.U8(Flags)) {
        return std::nullopt;
    }
    State.Role = static_cast<ControlRole>(RawRole);
    State.DesiredMode = static_cast<DeskMode>(RawMode);
    State.RuntimePhase = static_cast<BrokerRuntimePhase>(RawRuntimePhase);
    State.RuntimeFailure = static_cast<BrokerRuntimeFailure>(RawRuntimeFailure);
    State.RemoteFocused = (Flags & 0x01u) != 0;
    State.CaptureActive = (Flags & 0x02u) != 0;
    State.AudioMuted = (Flags & 0x04u) != 0;
    if ((Flags & 0xf8u) != 0 || !IsValidControlState(State)) return std::nullopt;
    return State;
}

bool IsKnownTopologyStatus(DisplayTopologyExchangeStatus Status) noexcept {
    switch (Status) {
        case DisplayTopologyExchangeStatus::Offline:
        case DisplayTopologyExchangeStatus::Disabled:
        case DisplayTopologyExchangeStatus::CapabilityMissing:
        case DisplayTopologyExchangeStatus::Synchronizing:
        case DisplayTopologyExchangeStatus::Ready:
        case DisplayTopologyExchangeStatus::TimedOut:
        case DisplayTopologyExchangeStatus::Rejected:
            return true;
    }
    return false;
}

void EncodeTopologyState(Writer& Output, const ControlTopologyState& State) {
    Output.U8(static_cast<std::uint8_t>(State.Machines.size()));
    std::uint64_t Sequence = 1;
    for (const auto& Machine : State.Machines) {
        Output.Raw(Machine.Machine);
        Output.U8(static_cast<std::uint8_t>(Machine.Status));
        std::uint8_t Flags = 0;
        if (Machine.Local) Flags |= 0x01u;
        if (Machine.PeerInputAllowed) Flags |= 0x02u;
        Output.U8(Flags);
        Output.U8(Machine.Topology.has_value() ? 1u : 0u);
        if (!Machine.Topology) continue;

        EnvelopeHeader Header;
        Header.session_nonce = 1;
        Header.sequence = Sequence++;
        const auto Packet = encode_packet(
            Header,
            DisplayTopologySnapshotMessage{
                Machine.Machine, 1, *Machine.Topology});
        Output.U32(static_cast<std::uint32_t>(Packet.size()));
        Output.Raw(Packet);
    }
}

std::optional<ControlTopologyState> DecodeTopologyState(Reader& Input) {
    std::uint8_t Count{};
    if (!Input.U8(Count) || Count == 0 ||
        Count > kMaximumControlTopologyMachines) {
        return std::nullopt;
    }
    ControlTopologyState Result;
    Result.Machines.reserve(Count);
    for (std::size_t Index = 0; Index < Count; ++Index) {
        ControlMachineTopology Machine;
        std::uint8_t RawStatus{};
        std::uint8_t Flags{};
        std::uint8_t HasTopology{};
        if (!Input.Raw(Machine.Machine) || !Input.U8(RawStatus) ||
            !Input.U8(Flags) || !Input.U8(HasTopology) ||
            (Flags & 0xfcu) != 0 || HasTopology > 1) {
            return std::nullopt;
        }
        Machine.Status = static_cast<DisplayTopologyExchangeStatus>(RawStatus);
        Machine.Local = (Flags & 0x01u) != 0;
        Machine.PeerInputAllowed = (Flags & 0x02u) != 0;
        if (HasTopology != 0) {
            std::uint32_t PacketSize{};
            if (!Input.U32(PacketSize) || PacketSize == 0 ||
                PacketSize > kMaxReliablePayload + kEnvelopeSize ||
                Input.Remaining() < PacketSize) {
                return std::nullopt;
            }
            ByteBuffer Packet(PacketSize);
            if (!Input.Raw(Packet)) return std::nullopt;
            const auto Decoded = decode_packet(Packet, false);
            if (!Decoded.packet ||
                Decoded.packet->header.type !=
                    MessageType::DisplayTopologySnapshot ||
                Decoded.packet->header.session_nonce != 1 ||
                Decoded.packet->header.epoch != 0 ||
                Decoded.packet->header.sequence == 0) {
                return std::nullopt;
            }
            const auto& Message = std::get<DisplayTopologySnapshotMessage>(
                Decoded.packet->message);
            if (Message.Machine != Machine.Machine || Message.SessionNonce != 1) {
                return std::nullopt;
            }
            Machine.Topology = Message.Topology;
        }
        Result.Machines.push_back(std::move(Machine));
    }
    if (!IsValidControlTopologyState(Result)) return std::nullopt;
    return Result;
}

bool IsKnownStatus(ControlStatus Status) noexcept {
    return static_cast<std::uint16_t>(Status) <=
           static_cast<std::uint16_t>(ControlStatus::CleanupFailed);
}

bool IsKnownRuntimePhase(BrokerRuntimePhase Phase) noexcept {
    return static_cast<std::uint8_t>(Phase) <=
           static_cast<std::uint8_t>(BrokerRuntimePhase::ActionRequired);
}

bool IsKnownRuntimeFailure(BrokerRuntimeFailure Failure) noexcept {
    return static_cast<std::uint8_t>(Failure) <=
           static_cast<std::uint8_t>(BrokerRuntimeFailure::Unknown);
}

} // namespace

bool IsValidControlRequest(const ControlRequest& Request) noexcept {
    if (Request.RequestId == 0) return false;
    return std::visit([](const auto& Value) {
        using ValueType = std::decay_t<decltype(Value)>;
        if constexpr (std::is_same_v<ValueType,
                                     SetDesiredModeControlRequest>) {
            return IsValidDeskMode(Value.Mode);
        } else if constexpr (std::is_same_v<ValueType,
                                            FocusMachineControlRequest>) {
            return IsNonzeroMachine(Value.Machine);
        } else if constexpr (std::is_same_v<ValueType,
                                            SetAudioGainControlRequest>) {
            return Value.GainPermyriad <= 10'000;
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 SetProductPreferencesControlRequest>) {
            return IsValidProductPreferences(Value.Preferences);
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 RequestLocalPermissionChangeControlRequest>) {
            return IsNonzeroMachine(Value.Machine) &&
                   HasOnlyKnownCapabilities(Value.DesiredCapabilities);
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 ForgetTrustedDeviceControlRequest>) {
            return IsNonzeroMachine(Value.Machine);
        } else {
            return true;
        }
    }, Request.Payload);
}

bool IsValidControlState(const ControlState& State) noexcept {
    if (!IsValidRole(State.Role) || !IsValidDeskMode(State.DesiredMode) ||
        State.AudioGainPermyriad > 10'000 ||
        !IsKnownRuntimePhase(State.RuntimePhase) ||
        !IsKnownRuntimeFailure(State.RuntimeFailure)) {
        return false;
    }
    if (State.RuntimePhase == BrokerRuntimePhase::RetryWaiting) {
        if (!IsRetryableBrokerRuntimeFailure(State.RuntimeFailure) ||
            State.RetryAttempt == 0) {
            return false;
        }
    } else if (State.RuntimePhase == BrokerRuntimePhase::ActionRequired) {
        if (State.RuntimeFailure == BrokerRuntimeFailure::None ||
            IsRetryableBrokerRuntimeFailure(State.RuntimeFailure) ||
            State.RetryAttempt != 0) {
            return false;
        }
    } else if (State.RuntimeFailure != BrokerRuntimeFailure::None ||
               State.RetryAttempt != 0) {
        return false;
    }
    if (State.CaptureActive && (!State.RemoteFocused ||
                                State.Role != ControlRole::Host)) {
        return false;
    }
    if (State.RemoteFocused && !IsNonzeroMachine(State.FocusedMachine)) {
        return false;
    }
    if (!State.RemoteFocused && IsNonzeroMachine(State.FocusedMachine)) {
        return false;
    }
    return true;
}

bool IsValidControlTopologyState(const ControlTopologyState& State) {
    if (State.Machines.empty() ||
        State.Machines.size() > kMaximumControlTopologyMachines) {
        return false;
    }
    std::size_t LocalCount = 0;
    for (std::size_t Index = 0; Index < State.Machines.size(); ++Index) {
        const auto& Machine = State.Machines[Index];
        if (!IsNonzeroMachine(Machine.Machine) ||
            !IsKnownTopologyStatus(Machine.Status) ||
            (Machine.Local &&
             Machine.Status != DisplayTopologyExchangeStatus::Ready) ||
            (Machine.Local && Machine.PeerInputAllowed)) {
            return false;
        }
        if (Machine.Local) ++LocalCount;
        const bool Ready =
            Machine.Status == DisplayTopologyExchangeStatus::Ready;
        if (Ready != Machine.Topology.has_value() ||
            (Machine.Topology &&
             !IsValidDisplayTopologySnapshot(*Machine.Topology))) {
            return false;
        }
        for (std::size_t Previous = 0; Previous < Index; ++Previous) {
            if (State.Machines[Previous].Machine == Machine.Machine) {
                return false;
            }
        }
    }
    return LocalCount == 1;
}

bool IsValidControlTrustedDeviceList(
    const ControlTrustedDeviceList& Devices) noexcept {
    if (Devices.Devices.size() > kMaximumControlTrustedDevices) return false;
    for (std::size_t Index = 0; Index < Devices.Devices.size(); ++Index) {
        const auto& Device = Devices.Devices[Index];
        if (!IsNonzeroMachine(Device.Machine) ||
            !IsBoundedDisplayName(Device.DisplayName) ||
            !HasOnlyKnownCapabilities(Device.Capabilities)) {
            return false;
        }
        for (std::size_t Previous = 0; Previous < Index; ++Previous) {
            if (Devices.Devices[Previous].Machine == Device.Machine) {
                return false;
            }
        }
    }
    return true;
}

bool IsValidControlPairingCandidate(
    const ControlPairingCandidate& Candidate) noexcept {
    return Candidate.OperationId != 0 &&
           IsNonzeroMachine(Candidate.Machine) &&
           IsBoundedDisplayName(Candidate.DisplayName) &&
           Candidate.VerificationCode.size() == 6 &&
           std::all_of(Candidate.VerificationCode.begin(),
                       Candidate.VerificationCode.end(),
                       [](char Value) { return Value >= '0' && Value <= '9'; }) &&
           HasOnlyKnownCapabilities(Candidate.RequestedCapabilities);
}

bool IsValidControlResponse(const ControlResponse& Response) {
    if (Response.RequestId == 0 || !IsKnownStatus(Response.Status)) return false;
    if (Response.Status != ControlStatus::Ok &&
        (Response.State.has_value() || Response.Topologies.has_value() ||
         Response.Preferences.has_value() ||
         Response.TrustedDevices.has_value() ||
         Response.PairingCandidate.has_value())) {
        return false;
    }
    const auto PayloadCount = static_cast<unsigned>(Response.State.has_value()) +
        static_cast<unsigned>(Response.Topologies.has_value()) +
        static_cast<unsigned>(Response.Preferences.has_value()) +
        static_cast<unsigned>(Response.TrustedDevices.has_value()) +
        static_cast<unsigned>(Response.PairingCandidate.has_value());
    if (PayloadCount > 1) return false;
    return (!Response.State || IsValidControlState(*Response.State)) &&
           (!Response.Topologies ||
            IsValidControlTopologyState(*Response.Topologies)) &&
           (!Response.Preferences ||
            IsValidProductPreferences(*Response.Preferences)) &&
           (!Response.TrustedDevices ||
            IsValidControlTrustedDeviceList(*Response.TrustedDevices)) &&
           (!Response.PairingCandidate ||
            IsValidControlPairingCandidate(*Response.PairingCandidate));
}

std::optional<ByteBuffer> EncodeControlRequest(const ControlRequest& Request) {
    if (!IsValidControlRequest(Request)) return std::nullopt;
    auto Payload = EncodeRequestPayload(Request);
    if (Payload.size() > kMaximumControlPayload) return std::nullopt;
    Writer Output;
    EncodeHeader(Output, ControlFrameType::Request, Request.RequestId,
                 static_cast<std::uint32_t>(Payload.size()));
    Output.Raw(Payload);
    return Output.Take();
}

ControlDecodeResult<ControlRequest> DecodeControlRequest(ByteSpan Frame) {
    const auto Header = DecodeHeader(Frame, ControlFrameType::Request);
    if (!Header.Decoded) return {std::nullopt, Header.Error};
    const auto Payload = DecodeRequestPayload(Frame.subspan(kControlFrameHeaderSize));
    if (!Payload) return {std::nullopt, ControlDecodeError::InvalidPayload};
    ControlRequest Request{Header.Decoded->RequestId, *Payload};
    if (!IsValidControlRequest(Request)) {
        return {std::nullopt, ControlDecodeError::InvalidPayload};
    }
    return {std::move(Request), ControlDecodeError::None};
}

std::optional<ByteBuffer> EncodeControlResponse(const ControlResponse& Response) {
    if (!IsValidControlResponse(Response)) return std::nullopt;
    Writer Payload;
    Payload.U16(static_cast<std::uint16_t>(Response.Status));
    const auto PayloadKind = Response.State ? 1u
        : Response.Topologies ? 2u
        : Response.Preferences ? 3u
        : Response.TrustedDevices ? 4u
        : Response.PairingCandidate ? 5u
        : 0u;
    Payload.U8(static_cast<std::uint8_t>(PayloadKind));
    if (Response.State) {
        EncodeState(Payload, *Response.State);
    } else if (Response.Topologies) {
        EncodeTopologyState(Payload, *Response.Topologies);
    } else if (Response.Preferences) {
        EncodePreferences(Payload, *Response.Preferences);
    } else if (Response.TrustedDevices) {
        EncodeTrustedDevices(Payload, *Response.TrustedDevices);
    } else if (Response.PairingCandidate) {
        EncodePairingCandidate(Payload, *Response.PairingCandidate);
    }
    auto PayloadBytes = Payload.Take();
    if (PayloadBytes.size() > kMaximumControlPayload) return std::nullopt;
    Writer Output;
    EncodeHeader(Output, ControlFrameType::Response, Response.RequestId,
                 static_cast<std::uint32_t>(PayloadBytes.size()));
    Output.Raw(PayloadBytes);
    return Output.Take();
}

ControlDecodeResult<ControlResponse> DecodeControlResponse(ByteSpan Frame) {
    const auto Header = DecodeHeader(Frame, ControlFrameType::Response);
    if (!Header.Decoded) return {std::nullopt, Header.Error};
    Reader Input(Frame.subspan(kControlFrameHeaderSize));
    std::uint16_t RawStatus{};
    std::uint8_t PayloadKind{};
    if (!Input.U16(RawStatus) || !Input.U8(PayloadKind) || PayloadKind > 5) {
        return {std::nullopt, ControlDecodeError::InvalidPayload};
    }
    ControlResponse Response;
    Response.RequestId = Header.Decoded->RequestId;
    Response.Status = static_cast<ControlStatus>(RawStatus);
    if (PayloadKind == 1) {
        Response.State = DecodeState(Input);
        if (!Response.State) {
            return {std::nullopt, ControlDecodeError::InvalidPayload};
        }
    } else if (PayloadKind == 2) {
        Response.Topologies = DecodeTopologyState(Input);
        if (!Response.Topologies) {
            return {std::nullopt, ControlDecodeError::InvalidPayload};
        }
    } else if (PayloadKind == 3) {
        Response.Preferences = DecodePreferences(Input);
        if (!Response.Preferences) {
            return {std::nullopt, ControlDecodeError::InvalidPayload};
        }
    } else if (PayloadKind == 4) {
        Response.TrustedDevices = DecodeTrustedDevices(Input);
        if (!Response.TrustedDevices) {
            return {std::nullopt, ControlDecodeError::InvalidPayload};
        }
    } else if (PayloadKind == 5) {
        Response.PairingCandidate = DecodePairingCandidate(Input);
        if (!Response.PairingCandidate) {
            return {std::nullopt, ControlDecodeError::InvalidPayload};
        }
    }
    if (Input.Remaining() != 0 || !IsValidControlResponse(Response)) {
        return {std::nullopt, ControlDecodeError::InvalidPayload};
    }
    return {std::move(Response), ControlDecodeError::None};
}

} // namespace desklink
