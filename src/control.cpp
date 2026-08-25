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
        } else {
            return ControlCommand::GetDisplayTopologies;
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
        default:
            return std::nullopt;
    }
}

void EncodeState(Writer& Output, const ControlState& State) {
    Output.Raw(State.LocalMachine);
    Output.Raw(State.FocusedMachine);
    Output.U8(static_cast<std::uint8_t>(State.Role));
    Output.U8(static_cast<std::uint8_t>(State.DesiredMode));
    Output.U16(State.ConnectedPeerCount);
    Output.U16(State.AudioGainPermyriad);
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
    std::uint8_t Flags{};
    if (!Input.Raw(State.LocalMachine) || !Input.Raw(State.FocusedMachine) ||
        !Input.U8(RawRole) || !Input.U8(RawMode) ||
        !Input.U16(State.ConnectedPeerCount) ||
        !Input.U16(State.AudioGainPermyriad) || !Input.U8(Flags)) {
        return std::nullopt;
    }
    State.Role = static_cast<ControlRole>(RawRole);
    State.DesiredMode = static_cast<DeskMode>(RawMode);
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
                PacketSize > kMaxReliablePayload + 36u ||
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
           static_cast<std::uint16_t>(ControlStatus::Failed);
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
        } else {
            return true;
        }
    }, Request.Payload);
}

bool IsValidControlState(const ControlState& State) noexcept {
    if (!IsValidRole(State.Role) || !IsValidDeskMode(State.DesiredMode) ||
        State.AudioGainPermyriad > 10'000) {
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

bool IsValidControlResponse(const ControlResponse& Response) {
    if (Response.RequestId == 0 || !IsKnownStatus(Response.Status)) return false;
    if (Response.Status != ControlStatus::Ok &&
        (Response.State.has_value() || Response.Topologies.has_value())) {
        return false;
    }
    if (Response.State && Response.Topologies) return false;
    return (!Response.State || IsValidControlState(*Response.State)) &&
           (!Response.Topologies ||
            IsValidControlTopologyState(*Response.Topologies));
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
    const auto PayloadKind = Response.State ? 1u : Response.Topologies ? 2u : 0u;
    Payload.U8(static_cast<std::uint8_t>(PayloadKind));
    if (Response.State) {
        EncodeState(Payload, *Response.State);
    } else if (Response.Topologies) {
        EncodeTopologyState(Payload, *Response.Topologies);
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
    if (!Input.U16(RawStatus) || !Input.U8(PayloadKind) || PayloadKind > 2) {
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
    }
    if (Input.Remaining() != 0 || !IsValidControlResponse(Response)) {
        return {std::nullopt, ControlDecodeError::InvalidPayload};
    }
    return {std::move(Response), ControlDecodeError::None};
}

} // namespace desklink
