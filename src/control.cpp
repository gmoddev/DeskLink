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

bool IsKnownRoamingState(ControlRoamingState State) noexcept {
    return static_cast<std::uint8_t>(State) <=
        static_cast<std::uint8_t>(ControlRoamingState::LocalCooldown);
}

bool IsKnownPeerDirectionState(
    ControlPeerDirectionState State) noexcept {
    return static_cast<std::uint8_t>(State) <=
        static_cast<std::uint8_t>(
            ControlPeerDirectionState::IncomingActive);
}

bool IsNonzeroMachine(const MachineId& Machine) noexcept {
    return std::any_of(Machine.begin(), Machine.end(),
                       [](std::uint8_t Byte) { return Byte != 0; });
}

bool HasOnlyKnownCapabilities(CapabilitySet Capabilities) noexcept {
    return (Capabilities.bits() & ~kKnownCapabilityBits) == 0;
}

bool IsSubset(CapabilitySet Candidate, CapabilitySet Existing) noexcept {
    return (Candidate.bits() & ~Existing.bits()) == 0;
}

bool IsNonzeroPairingToken(const ControlPairingToken& Token) noexcept {
    return std::any_of(Token.begin(), Token.end(),
                       [](std::uint8_t Byte) { return Byte != 0; });
}

bool IsValidControlHostName(std::string_view Host) noexcept {
    return !Host.empty() && Host.size() <= kMaximumControlHostName &&
        std::all_of(Host.begin(), Host.end(), [](unsigned char Character) {
            return Character > 0x20u && Character != 0x7fu &&
                   Character != static_cast<unsigned char>('"');
        });
}

bool IsKnownPairingSource(ControlPairingSource Source) noexcept {
    return static_cast<std::uint8_t>(Source) <=
        static_cast<std::uint8_t>(ControlPairingSource::Manual);
}

bool IsKnownDiscoveryPhase(ControlDiscoveryPhase Phase) noexcept {
    return static_cast<std::uint8_t>(Phase) <=
        static_cast<std::uint8_t>(ControlDiscoveryPhase::Failed);
}

bool IsBoundedDisplayName(std::string_view Name) noexcept;

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
                                 IdentifyPeerDisplaysControlRequest>) {
            return ControlCommand::IdentifyPeerDisplays;
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
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 ResumeDeskLinkControlRequest>) {
            return ControlCommand::ResumeDeskLink;
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 StartDiscoveryControlRequest>) {
            return ControlCommand::StartDiscovery;
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 GetNearbyPeersControlRequest>) {
            return ControlCommand::GetNearbyPeers;
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 StopDiscoveryControlRequest>) {
            return ControlCommand::StopDiscovery;
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 OpenPairingWindowControlRequest>) {
            return ControlCommand::OpenPairingWindow;
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 PairNearbyPeerControlRequest>) {
            return ControlCommand::PairNearbyPeer;
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 PairManualAddressControlRequest>) {
            return ControlCommand::PairManualAddress;
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 ResolvePairingCandidateControlRequest>) {
            return ControlCommand::ResolvePairingCandidate;
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 PresentManagedPairingCandidateControlRequest>) {
            return ControlCommand::PresentManagedPairingCandidate;
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 GetManagedPairingDecisionControlRequest>) {
            return ControlCommand::GetManagedPairingDecision;
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 GetPermissionCandidateControlRequest>) {
            return ControlCommand::GetPermissionCandidate;
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 ResolvePermissionCandidateControlRequest>) {
            return ControlCommand::ResolvePermissionCandidate;
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 GetPairingOperationControlRequest>) {
            return ControlCommand::GetPairingOperation;
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 RefreshTrustedPeerCapabilitiesControlRequest>) {
            return ControlCommand::RefreshTrustedPeerCapabilities;
        } else {
            return ControlCommand::ApplyManagedPreferences;
        }
    }, Payload);
}

void EncodePreferences(Writer& Output,
                       const ProductPreferences& Preferences);
std::optional<ProductPreferences> DecodePreferences(Reader& Input);

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
            EncodePreferences(Output, Value.Preferences);
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 IdentifyPeerDisplaysControlRequest>) {
            Output.U16(Value.FirstDisplayNumber);
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 RequestLocalPermissionChangeControlRequest>) {
            Output.Raw(Value.Machine);
            Output.U64(Value.DesiredCapabilities.bits());
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 ForgetTrustedDeviceControlRequest>) {
            Output.Raw(Value.Machine);
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 StartDiscoveryControlRequest>) {
            Output.U8(Value.DurationSeconds);
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 OpenPairingWindowControlRequest>) {
            Output.U16(Value.Port);
            Output.U64(Value.RequestedCapabilities.bits());
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 PairNearbyPeerControlRequest>) {
            Output.Raw(Value.Machine);
            Output.U64(Value.RequestedCapabilities.bits());
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 PairManualAddressControlRequest>) {
            Output.U16(Value.Port);
            Output.U64(Value.RequestedCapabilities.bits());
            Output.U16(static_cast<std::uint16_t>(Value.Host.size()));
            Output.Raw(ByteSpan{
                reinterpret_cast<const std::uint8_t*>(Value.Host.data()),
                Value.Host.size()});
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 ResolvePairingCandidateControlRequest>) {
            Output.U64(Value.OperationId);
            Output.U8(Value.Approved ? 1u : 0u);
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 PresentManagedPairingCandidateControlRequest>) {
            Output.Raw(Value.Token);
            Output.U64(Value.OperationId);
            Output.Raw(Value.Machine);
            Output.U64(Value.RequestedCapabilities.bits());
            Output.U8(static_cast<std::uint8_t>(Value.DisplayName.size()));
            Output.Raw(ByteSpan{
                reinterpret_cast<const std::uint8_t*>(Value.DisplayName.data()),
                Value.DisplayName.size()});
            Output.Raw(ByteSpan{
                reinterpret_cast<const std::uint8_t*>(
                    Value.VerificationCode.data()),
                Value.VerificationCode.size()});
            Output.Raw(ByteSpan{
                reinterpret_cast<const std::uint8_t*>(
                    Value.CertificateFingerprint.data()),
                Value.CertificateFingerprint.size()});
            Output.Raw(Value.TranscriptDigest);
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 GetManagedPairingDecisionControlRequest>) {
            Output.Raw(Value.Token);
            Output.U64(Value.OperationId);
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 ResolvePermissionCandidateControlRequest>) {
            Output.U64(Value.OperationId);
            Output.U8(Value.Approved ? 1u : 0u);
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 RefreshTrustedPeerCapabilitiesControlRequest>) {
            Output.Raw(Value.Machine);
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 ApplyManagedPreferencesControlRequest>) {
            EncodePreferences(Output, Value.Preferences);
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
        case ControlCommand::IdentifyPeerDisplays: {
            IdentifyPeerDisplaysControlRequest Request;
            if (!Input.U16(Request.FirstDisplayNumber) ||
                Input.Remaining() != 0 ||
                Request.FirstDisplayNumber == 0 ||
                Request.FirstDisplayNumber > kMaxDisplayCount) {
                return std::nullopt;
            }
            return Request;
        }
        case ControlCommand::PrepareForUpdate:
            if (Input.Remaining() != 0) return std::nullopt;
            return PrepareForUpdateControlRequest{};
        case ControlCommand::GetProductPreferences:
            if (Input.Remaining() != 0) return std::nullopt;
            return GetProductPreferencesControlRequest{};
        case ControlCommand::SetProductPreferences: {
            const auto Preferences = DecodePreferences(Input);
            if (!Preferences || Input.Remaining() != 0) {
                return std::nullopt;
            }
            return SetProductPreferencesControlRequest{*Preferences};
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
        case ControlCommand::StartDiscovery: {
            StartDiscoveryControlRequest Request;
            if (!Input.U8(Request.DurationSeconds) || Input.Remaining() != 0 ||
                Request.DurationSeconds == 0 || Request.DurationSeconds > 30) {
                return std::nullopt;
            }
            return Request;
        }
        case ControlCommand::GetNearbyPeers:
            if (Input.Remaining() != 0) return std::nullopt;
            return GetNearbyPeersControlRequest{};
        case ControlCommand::StopDiscovery:
            if (Input.Remaining() != 0) return std::nullopt;
            return StopDiscoveryControlRequest{};
        case ControlCommand::OpenPairingWindow: {
            OpenPairingWindowControlRequest Request;
            std::uint64_t CapabilityBits{};
            if (!Input.U16(Request.Port) || !Input.U64(CapabilityBits) ||
                Input.Remaining() != 0 || Request.Port == 0) {
                return std::nullopt;
            }
            Request.RequestedCapabilities = CapabilitySet(CapabilityBits);
            return HasOnlyKnownCapabilities(Request.RequestedCapabilities)
                ? std::optional<ControlRequestPayload>(Request)
                : std::nullopt;
        }
        case ControlCommand::PairNearbyPeer: {
            PairNearbyPeerControlRequest Request;
            std::uint64_t CapabilityBits{};
            if (!Input.Raw(Request.Machine) || !Input.U64(CapabilityBits) ||
                Input.Remaining() != 0 || !IsNonzeroMachine(Request.Machine)) {
                return std::nullopt;
            }
            Request.RequestedCapabilities = CapabilitySet(CapabilityBits);
            return HasOnlyKnownCapabilities(Request.RequestedCapabilities)
                ? std::optional<ControlRequestPayload>(Request)
                : std::nullopt;
        }
        case ControlCommand::PairManualAddress: {
            PairManualAddressControlRequest Request;
            std::uint64_t CapabilityBits{};
            std::uint16_t HostLength{};
            if (!Input.U16(Request.Port) || !Input.U64(CapabilityBits) ||
                !Input.U16(HostLength) || HostLength == 0 ||
                HostLength > kMaximumControlHostName ||
                Input.Remaining() != HostLength) {
                return std::nullopt;
            }
            ByteBuffer Host(HostLength);
            if (!Input.Raw(Host)) return std::nullopt;
            Request.Host.assign(
                reinterpret_cast<const char*>(Host.data()), Host.size());
            Request.RequestedCapabilities = CapabilitySet(CapabilityBits);
            return Request.Port != 0 && IsValidControlHostName(Request.Host) &&
                    HasOnlyKnownCapabilities(Request.RequestedCapabilities)
                ? std::optional<ControlRequestPayload>(std::move(Request))
                : std::nullopt;
        }
        case ControlCommand::ResolvePairingCandidate: {
            ResolvePairingCandidateControlRequest Request;
            std::uint8_t Approved{};
            if (!Input.U64(Request.OperationId) || !Input.U8(Approved) ||
                Input.Remaining() != 0 || Request.OperationId == 0 ||
                Approved > 1) {
                return std::nullopt;
            }
            Request.Approved = Approved != 0;
            return Request;
        }
        case ControlCommand::PresentManagedPairingCandidate: {
            PresentManagedPairingCandidateControlRequest Request;
            std::uint64_t CapabilityBits{};
            std::uint8_t NameLength{};
            if (!Input.Raw(Request.Token) || !Input.U64(Request.OperationId) ||
                !Input.Raw(Request.Machine) || !Input.U64(CapabilityBits) ||
                !Input.U8(NameLength) || NameLength == 0 ||
                NameLength > kMaximumControlDisplayName ||
                Input.Remaining() != NameLength + 6u +
                    kControlFingerprintLength + kSha256DigestSize) {
                return std::nullopt;
            }
            ByteBuffer Name(NameLength);
            ByteBuffer Code(6);
            ByteBuffer Fingerprint(kControlFingerprintLength);
            if (!Input.Raw(Name) || !Input.Raw(Code) ||
                !Input.Raw(Fingerprint) ||
                !Input.Raw(Request.TranscriptDigest)) {
                return std::nullopt;
            }
            Request.DisplayName.assign(
                reinterpret_cast<const char*>(Name.data()), Name.size());
            Request.VerificationCode.assign(
                reinterpret_cast<const char*>(Code.data()), Code.size());
            Request.CertificateFingerprint.assign(
                reinterpret_cast<const char*>(Fingerprint.data()),
                Fingerprint.size());
            Request.RequestedCapabilities = CapabilitySet(CapabilityBits);
            return IsNonzeroPairingToken(Request.Token) &&
                    Request.OperationId != 0 &&
                    IsNonzeroMachine(Request.Machine) &&
                    IsBoundedDisplayName(Request.DisplayName) &&
                    Request.VerificationCode.size() == 6 &&
                    std::all_of(
                        Request.VerificationCode.begin(),
                        Request.VerificationCode.end(),
                        [](char Value) {
                            return Value >= '0' && Value <= '9';
                        }) &&
                    Request.CertificateFingerprint.size() ==
                        kControlFingerprintLength &&
                    std::any_of(
                        Request.TranscriptDigest.begin(),
                        Request.TranscriptDigest.end(),
                        [](std::uint8_t Byte) { return Byte != 0; }) &&
                    HasOnlyKnownCapabilities(Request.RequestedCapabilities)
                ? std::optional<ControlRequestPayload>(std::move(Request))
                : std::nullopt;
        }
        case ControlCommand::GetManagedPairingDecision: {
            GetManagedPairingDecisionControlRequest Request;
            if (!Input.Raw(Request.Token) || !Input.U64(Request.OperationId) ||
                Input.Remaining() != 0 ||
                !IsNonzeroPairingToken(Request.Token) ||
                Request.OperationId == 0) {
                return std::nullopt;
            }
            return Request;
        }
        case ControlCommand::GetPermissionCandidate:
            if (Input.Remaining() != 0) return std::nullopt;
            return GetPermissionCandidateControlRequest{};
        case ControlCommand::ResolvePermissionCandidate: {
            ResolvePermissionCandidateControlRequest Request;
            std::uint8_t Approved{};
            if (!Input.U64(Request.OperationId) || !Input.U8(Approved) ||
                Input.Remaining() != 0 || Request.OperationId == 0 ||
                Approved > 1) {
                return std::nullopt;
            }
            Request.Approved = Approved != 0;
            return Request;
        }
        case ControlCommand::GetPairingOperation:
            if (Input.Remaining() != 0) return std::nullopt;
            return GetPairingOperationControlRequest{};
        case ControlCommand::RefreshTrustedPeerCapabilities: {
            RefreshTrustedPeerCapabilitiesControlRequest Request;
            if (!Input.Raw(Request.Machine) || Input.Remaining() != 0 ||
                !IsNonzeroMachine(Request.Machine)) {
                return std::nullopt;
            }
            return Request;
        }
        case ControlCommand::ApplyManagedPreferences: {
            const auto Preferences = DecodePreferences(Input);
            if (!Preferences || Input.Remaining() != 0) {
                return std::nullopt;
            }
            return ApplyManagedPreferencesControlRequest{*Preferences};
        }
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
    if (Preferences.PreferredPeerEndpoint) Flags |= 0x0200u;
    Output.U16(Flags);
    Output.U16(Preferences.AudioGainPermyriad);
    Output.U8(static_cast<std::uint8_t>(Preferences.FocusPeerHotkey));
    Output.U8(static_cast<std::uint8_t>(Preferences.ReturnLocalHotkey));
    if (Preferences.PreferredPeerMachine) {
        Output.Raw(*Preferences.PreferredPeerMachine);
    }
    if (Preferences.PreferredPeerEndpoint) {
        Output.U16(static_cast<std::uint16_t>(
            Preferences.PreferredPeerEndpoint->Host.size()));
        Output.Raw(ByteSpan{
            reinterpret_cast<const std::uint8_t*>(
                Preferences.PreferredPeerEndpoint->Host.data()),
            Preferences.PreferredPeerEndpoint->Host.size()});
        Output.U16(Preferences.PreferredPeerEndpoint->Port);
    }
    Output.U8(static_cast<std::uint8_t>(Preferences.ProfileRules.size()));
    for (const auto& Rule : Preferences.ProfileRules) {
        Output.U8(static_cast<std::uint8_t>(Rule.Mode));
        Output.U8(Rule.FullscreenOnly ? 1u : 0u);
        Output.U16(static_cast<std::uint16_t>(Rule.ExecutableName.size()));
        Output.Raw(ByteSpan{
            reinterpret_cast<const std::uint8_t*>(
                Rule.ExecutableName.data()),
            Rule.ExecutableName.size()});
    }
    Output.U8(static_cast<std::uint8_t>(Preferences.VoiceRoute));
    Output.U16(Preferences.VoiceGainPermyriad);
    Output.U8(Preferences.VoiceEchoGuard ? 1u : 0u);
    const auto EndpointSize = Preferences.VoiceInputEndpointId
        ? Preferences.VoiceInputEndpointId->size() : 0u;
    Output.U16(static_cast<std::uint16_t>(EndpointSize));
    if (Preferences.VoiceInputEndpointId) {
        Output.Raw(ByteSpan{
            reinterpret_cast<const std::uint8_t*>(
                Preferences.VoiceInputEndpointId->data()),
            Preferences.VoiceInputEndpointId->size()});
    }
}

std::optional<ProductPreferences> DecodePreferences(Reader& Input) {
    std::uint8_t RawRole{};
    std::uint8_t RawAudioRoute{};
    std::uint8_t RawGaming{};
    std::uint8_t RawFocusHotkey{};
    std::uint8_t RawReturnHotkey{};
    std::uint16_t Flags{};
    ProductPreferences Preferences;
    if (!Input.U8(RawRole) || !Input.U8(RawAudioRoute) ||
        !Input.U8(RawGaming) || !Input.U16(Flags) ||
        !Input.U16(Preferences.AudioGainPermyriad) ||
        !Input.U8(RawFocusHotkey) || !Input.U8(RawReturnHotkey) ||
        (Flags & 0xfc00u) != 0) {
        return std::nullopt;
    }
    Preferences.Role = static_cast<DeskRole>(RawRole);
    Preferences.AudioRoute =
        static_cast<AudioRoutePreference>(RawAudioRoute);
    Preferences.Gaming = static_cast<GamingBehavior>(RawGaming);
    Preferences.FocusPeerHotkey =
        static_cast<ProductHotkey>(RawFocusHotkey);
    Preferences.ReturnLocalHotkey =
        static_cast<ProductHotkey>(RawReturnHotkey);
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
    if ((Flags & 0x0200u) != 0) {
        std::uint16_t HostSize{};
        if (!Input.U16(HostSize) || HostSize == 0 ||
            HostSize > kMaximumPreferredPeerHostBytes ||
            Input.Remaining() < static_cast<std::size_t>(HostSize) + 2u) {
            return std::nullopt;
        }
        ByteBuffer Host(HostSize);
        std::uint16_t Port{};
        if (!Input.Raw(Host) || !Input.U16(Port)) return std::nullopt;
        Preferences.PreferredPeerEndpoint = ProductPeerEndpoint{
            std::string(
                reinterpret_cast<const char*>(Host.data()), Host.size()),
            Port};
    }
    std::uint8_t RuleCount{};
    if (!Input.U8(RuleCount) || RuleCount > kMaximumForegroundProfileRules) {
        return std::nullopt;
    }
    Preferences.ProfileRules.reserve(RuleCount);
    for (std::uint8_t Index = 0; Index < RuleCount; ++Index) {
        std::uint8_t RawMode{};
        std::uint8_t RuleFlags{};
        std::uint16_t NameSize{};
        if (!Input.U8(RawMode) || !Input.U8(RuleFlags) ||
            !Input.U16(NameSize) || (RuleFlags & 0xfeu) != 0 ||
            NameSize == 0 || NameSize > kMaximumExecutableNameBytes ||
            Input.Remaining() < NameSize) {
            return std::nullopt;
        }
        ByteBuffer Name(NameSize);
        if (!Input.Raw(Name)) return std::nullopt;
        Preferences.ProfileRules.push_back(ForegroundProfileRule{
            std::string(
                reinterpret_cast<const char*>(Name.data()), Name.size()),
            static_cast<DeskMode>(RawMode),
            (RuleFlags & 0x01u) != 0});
    }
    std::uint8_t RawVoiceRoute{};
    std::uint8_t RawEchoGuard{};
    std::uint16_t VoiceEndpointSize{};
    if (!Input.U8(RawVoiceRoute) ||
        !Input.U16(Preferences.VoiceGainPermyriad) ||
        !Input.U8(RawEchoGuard) || RawEchoGuard > 1 ||
        !Input.U16(VoiceEndpointSize) ||
        VoiceEndpointSize > kMaximumVoiceEndpointIdBytes ||
        Input.Remaining() < VoiceEndpointSize) {
        return std::nullopt;
    }
    Preferences.VoiceRoute =
        static_cast<VoiceRoutePreference>(RawVoiceRoute);
    Preferences.VoiceEchoGuard = RawEchoGuard != 0;
    if (VoiceEndpointSize != 0) {
        ByteBuffer Endpoint(VoiceEndpointSize);
        if (!Input.Raw(Endpoint)) return std::nullopt;
        Preferences.VoiceInputEndpointId = std::string(
            reinterpret_cast<const char*>(Endpoint.data()), Endpoint.size());
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
        Output.U8(Device.Connected ? 1u : 0u);
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
        std::uint8_t Connected{};
        std::uint8_t NameLength{};
        if (!Input.Raw(Device.Machine) || !Input.U64(CapabilityBits) ||
            !Input.U8(Connected) || Connected > 1 ||
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
        Device.Connected = Connected != 0;
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
    Output.U8(static_cast<std::uint8_t>(Candidate.Source));
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
    std::uint8_t RawSource{};
    if (!Input.U8(RawSource)) return std::nullopt;
    Candidate.Source = static_cast<ControlPairingSource>(RawSource);
    Candidate.RequestedCapabilities = CapabilitySet(CapabilityBits);
    return IsValidControlPairingCandidate(Candidate)
        ? std::optional<ControlPairingCandidate>(std::move(Candidate))
        : std::nullopt;
}

void EncodePermissionCandidate(
    Writer& Output, const ControlPermissionCandidate& Candidate) {
    Output.U64(Candidate.OperationId);
    Output.Raw(Candidate.Machine);
    Output.U64(Candidate.CurrentCapabilities.bits());
    Output.U64(Candidate.DesiredCapabilities.bits());
    Output.U8(static_cast<std::uint8_t>(Candidate.DisplayName.size()));
    Output.Raw(ByteSpan{
        reinterpret_cast<const std::uint8_t*>(Candidate.DisplayName.data()),
        Candidate.DisplayName.size()});
}

std::optional<ControlPermissionCandidate> DecodePermissionCandidate(
    Reader& Input) {
    ControlPermissionCandidate Candidate;
    std::uint64_t CurrentBits{};
    std::uint64_t DesiredBits{};
    std::uint8_t NameLength{};
    if (!Input.U64(Candidate.OperationId) ||
        !Input.Raw(Candidate.Machine) || !Input.U64(CurrentBits) ||
        !Input.U64(DesiredBits) || !Input.U8(NameLength) || NameLength == 0 ||
        NameLength > kMaximumControlDisplayName ||
        Input.Remaining() != NameLength) {
        return std::nullopt;
    }
    ByteBuffer Name(NameLength);
    if (!Input.Raw(Name)) return std::nullopt;
    Candidate.DisplayName.assign(
        reinterpret_cast<const char*>(Name.data()), Name.size());
    Candidate.CurrentCapabilities = CapabilitySet(CurrentBits);
    Candidate.DesiredCapabilities = CapabilitySet(DesiredBits);
    return IsValidControlPermissionCandidate(Candidate)
        ? std::optional<ControlPermissionCandidate>(std::move(Candidate))
        : std::nullopt;
}

void EncodeNearbyPeers(Writer& Output,
                       const ControlNearbyPeerList& Nearby) {
    Output.U8(static_cast<std::uint8_t>(Nearby.Phase));
    Output.U8(static_cast<std::uint8_t>(Nearby.Peers.size()));
    for (const auto& Peer : Nearby.Peers) {
        Output.Raw(Peer.Machine);
        Output.U64(Peer.CapabilityHints.bits());
        Output.U16(Peer.Port);
        Output.U16(Peer.ProtocolVersion);
        Output.U8(Peer.EndpointCount);
        std::uint8_t Flags = 0;
        if (Peer.PairingOpen) Flags |= 0x01u;
        if (Peer.Ambiguous) Flags |= 0x02u;
        Output.U8(Flags);
        Output.U8(static_cast<std::uint8_t>(Peer.DisplayName.size()));
        Output.Raw(ByteSpan{
            reinterpret_cast<const std::uint8_t*>(Peer.DisplayName.data()),
            Peer.DisplayName.size()});
        Output.U16(static_cast<std::uint16_t>(Peer.HostName.size()));
        Output.Raw(ByteSpan{
            reinterpret_cast<const std::uint8_t*>(Peer.HostName.data()),
            Peer.HostName.size()});
    }
}

std::optional<ControlNearbyPeerList> DecodeNearbyPeers(Reader& Input) {
    std::uint8_t RawPhase{};
    std::uint8_t Count{};
    if (!Input.U8(RawPhase) || !Input.U8(Count) ||
        Count > kMaximumControlNearbyPeers) {
        return std::nullopt;
    }
    ControlNearbyPeerList Result;
    Result.Phase = static_cast<ControlDiscoveryPhase>(RawPhase);
    Result.Peers.reserve(Count);
    for (std::uint8_t Index = 0; Index < Count; ++Index) {
        ControlNearbyPeer Peer;
        std::uint64_t CapabilityBits{};
        std::uint8_t Flags{};
        std::uint8_t NameLength{};
        std::uint16_t HostLength{};
        if (!Input.Raw(Peer.Machine) || !Input.U64(CapabilityBits) ||
            !Input.U16(Peer.Port) || !Input.U16(Peer.ProtocolVersion) ||
            !Input.U8(Peer.EndpointCount) || !Input.U8(Flags) ||
            !Input.U8(NameLength) || NameLength == 0 ||
            NameLength > kMaximumControlDisplayName ||
            Input.Remaining() < NameLength + 2u) {
            return std::nullopt;
        }
        ByteBuffer Name(NameLength);
        if (!Input.Raw(Name) || !Input.U16(HostLength) || HostLength == 0 ||
            HostLength > kMaximumControlHostName ||
            Input.Remaining() < HostLength) {
            return std::nullopt;
        }
        ByteBuffer Host(HostLength);
        if (!Input.Raw(Host)) return std::nullopt;
        Peer.DisplayName.assign(
            reinterpret_cast<const char*>(Name.data()), Name.size());
        Peer.HostName.assign(
            reinterpret_cast<const char*>(Host.data()), Host.size());
        Peer.CapabilityHints = CapabilitySet(CapabilityBits);
        Peer.PairingOpen = (Flags & 0x01u) != 0;
        Peer.Ambiguous = (Flags & 0x02u) != 0;
        if ((Flags & 0xfcu) != 0) return std::nullopt;
        Result.Peers.push_back(std::move(Peer));
    }
    return IsValidControlNearbyPeerList(Result)
        ? std::optional<ControlNearbyPeerList>(std::move(Result))
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
    Output.U32(State.RetryDelayMilliseconds);
    Output.U8(static_cast<std::uint8_t>(State.RuntimePhase));
    Output.U8(static_cast<std::uint8_t>(State.RuntimeFailure));
    Output.U8(static_cast<std::uint8_t>(State.RoamingState));
    Output.U8(static_cast<std::uint8_t>(State.PeerDirection));
    Output.U16(State.ReadyRoamingRouteCount);
    std::uint8_t Flags = 0;
    if (State.RemoteFocused) Flags |= 0x01u;
    if (State.CaptureActive) Flags |= 0x02u;
    if (State.AudioMuted) Flags |= 0x04u;
    if (State.RoamingObserverActive) Flags |= 0x08u;
    Output.U8(Flags);
}

std::optional<ControlState> DecodeState(Reader& Input) {
    ControlState State;
    std::uint8_t RawRole{};
    std::uint8_t RawMode{};
    std::uint8_t RawRuntimePhase{};
    std::uint8_t RawRuntimeFailure{};
    std::uint8_t RawRoamingState{};
    std::uint8_t RawPeerDirection{};
    std::uint8_t Flags{};
    if (!Input.Raw(State.LocalMachine) || !Input.Raw(State.FocusedMachine) ||
        !Input.U8(RawRole) || !Input.U8(RawMode) ||
        !Input.U16(State.ConnectedPeerCount) ||
        !Input.U16(State.AudioGainPermyriad) ||
        !Input.U16(State.RetryAttempt) ||
        !Input.U32(State.RetryDelayMilliseconds) ||
        !Input.U8(RawRuntimePhase) || !Input.U8(RawRuntimeFailure) ||
        !Input.U8(RawRoamingState) || !Input.U8(RawPeerDirection) ||
        !Input.U16(State.ReadyRoamingRouteCount) ||
        !Input.U8(Flags)) {
        return std::nullopt;
    }
    State.Role = static_cast<ControlRole>(RawRole);
    State.DesiredMode = static_cast<DeskMode>(RawMode);
    State.RuntimePhase = static_cast<BrokerRuntimePhase>(RawRuntimePhase);
    State.RuntimeFailure = static_cast<BrokerRuntimeFailure>(RawRuntimeFailure);
    State.RoamingState = static_cast<ControlRoamingState>(RawRoamingState);
    State.PeerDirection = static_cast<ControlPeerDirectionState>(
        RawPeerDirection);
    State.RemoteFocused = (Flags & 0x01u) != 0;
    State.CaptureActive = (Flags & 0x02u) != 0;
    State.AudioMuted = (Flags & 0x04u) != 0;
    State.RoamingObserverActive = (Flags & 0x08u) != 0;
    if ((Flags & 0xf0u) != 0 || !IsValidControlState(State)) return std::nullopt;
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
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 StartDiscoveryControlRequest>) {
            return Value.DurationSeconds != 0 &&
                   Value.DurationSeconds <= 30;
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 OpenPairingWindowControlRequest>) {
            return Value.Port != 0 &&
                   HasOnlyKnownCapabilities(Value.RequestedCapabilities);
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 PairNearbyPeerControlRequest>) {
            return IsNonzeroMachine(Value.Machine) &&
                   HasOnlyKnownCapabilities(Value.RequestedCapabilities);
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 PairManualAddressControlRequest>) {
            return Value.Port != 0 && IsValidControlHostName(Value.Host) &&
                   HasOnlyKnownCapabilities(Value.RequestedCapabilities);
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 ResolvePairingCandidateControlRequest>) {
            return Value.OperationId != 0;
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 PresentManagedPairingCandidateControlRequest>) {
            return IsNonzeroPairingToken(Value.Token) &&
                   Value.CertificateFingerprint.size() ==
                       kControlFingerprintLength &&
                   std::any_of(
                       Value.TranscriptDigest.begin(),
                       Value.TranscriptDigest.end(),
                       [](std::uint8_t Byte) { return Byte != 0; }) &&
                   IsValidControlPairingCandidate(ControlPairingCandidate{
                       Value.OperationId,
                       Value.Machine,
                       Value.DisplayName,
                       Value.VerificationCode,
                       Value.RequestedCapabilities,
                       ControlPairingSource::Incoming});
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 GetManagedPairingDecisionControlRequest>) {
            return IsNonzeroPairingToken(Value.Token) &&
                   Value.OperationId != 0;
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 ResolvePermissionCandidateControlRequest>) {
            return Value.OperationId != 0;
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 RefreshTrustedPeerCapabilitiesControlRequest>) {
            return IsNonzeroMachine(Value.Machine);
        } else if constexpr (std::is_same_v<
                                 ValueType,
                                 ApplyManagedPreferencesControlRequest>) {
            return IsValidProductPreferences(Value.Preferences);
        } else {
            return true;
        }
    }, Request.Payload);
}

bool IsValidControlState(const ControlState& State) noexcept {
    if (!IsValidRole(State.Role) || !IsValidDeskMode(State.DesiredMode) ||
        State.AudioGainPermyriad > 10'000 ||
        !IsKnownRuntimePhase(State.RuntimePhase) ||
        !IsKnownRuntimeFailure(State.RuntimeFailure) ||
        !IsKnownRoamingState(State.RoamingState) ||
        !IsKnownPeerDirectionState(State.PeerDirection)) {
        return false;
    }
    if (State.RuntimePhase == BrokerRuntimePhase::RetryWaiting) {
        if (!IsRetryableBrokerRuntimeFailure(State.RuntimeFailure) ||
            State.RetryAttempt == 0 ||
            State.RetryDelayMilliseconds >
                static_cast<std::uint32_t>(
                    kBrokerReconnectMaximumDelay.count() * 1'000)) {
            return false;
        }
    } else if (State.RuntimePhase == BrokerRuntimePhase::ActionRequired) {
        if (State.RuntimeFailure == BrokerRuntimeFailure::None ||
            IsRetryableBrokerRuntimeFailure(State.RuntimeFailure) ||
            State.RetryAttempt != 0 ||
            State.RetryDelayMilliseconds != 0) {
            return false;
        }
    } else if (State.RuntimeFailure != BrokerRuntimeFailure::None ||
               State.RetryAttempt != 0 ||
               State.RetryDelayMilliseconds != 0) {
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
    if (State.RoamingObserverActive &&
        (State.Role != ControlRole::Host ||
         State.RoamingState == ControlRoamingState::Unavailable)) {
        return false;
    }
    if (State.ReadyRoamingRouteCount != 0 &&
        State.RoamingState == ControlRoamingState::Unavailable) {
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
           HasOnlyKnownCapabilities(Candidate.RequestedCapabilities) &&
           IsKnownPairingSource(Candidate.Source);
}

bool IsValidControlNearbyPeerList(
    const ControlNearbyPeerList& Peers) noexcept {
    if (!IsKnownDiscoveryPhase(Peers.Phase) ||
        Peers.Peers.size() > kMaximumControlNearbyPeers ||
        (Peers.Phase != ControlDiscoveryPhase::Complete &&
         !Peers.Peers.empty())) {
        return false;
    }
    for (std::size_t Index = 0; Index < Peers.Peers.size(); ++Index) {
        const auto& Peer = Peers.Peers[Index];
        if (!IsNonzeroMachine(Peer.Machine) ||
            !IsBoundedDisplayName(Peer.DisplayName) ||
            !IsValidControlHostName(Peer.HostName) || Peer.Port == 0 ||
            Peer.ProtocolVersion == 0 || Peer.EndpointCount == 0 ||
            !HasOnlyKnownCapabilities(Peer.CapabilityHints)) {
            return false;
        }
        for (std::size_t Previous = 0; Previous < Index; ++Previous) {
            if (Peers.Peers[Previous].Machine == Peer.Machine) return false;
        }
    }
    return true;
}

bool IsValidControlPermissionCandidate(
    const ControlPermissionCandidate& Candidate) noexcept {
    return Candidate.OperationId != 0 &&
           IsNonzeroMachine(Candidate.Machine) &&
           IsBoundedDisplayName(Candidate.DisplayName) &&
           HasOnlyKnownCapabilities(Candidate.CurrentCapabilities) &&
           HasOnlyKnownCapabilities(Candidate.DesiredCapabilities) &&
           Candidate.CurrentCapabilities.bits() !=
               Candidate.DesiredCapabilities.bits() &&
           IsSubset(Candidate.CurrentCapabilities,
                    Candidate.DesiredCapabilities);
}

bool IsValidControlPairingOperation(
    const ControlPairingOperation& Operation) noexcept {
    return Operation.OperationId != 0 &&
           static_cast<std::uint8_t>(Operation.Phase) <=
               static_cast<std::uint8_t>(ControlPairingPhase::Failed);
}

bool IsValidControlResponse(const ControlResponse& Response) {
    if (Response.RequestId == 0 || !IsKnownStatus(Response.Status)) return false;
    if (Response.Status != ControlStatus::Ok &&
        (Response.State.has_value() || Response.Topologies.has_value() ||
         Response.Preferences.has_value() ||
         Response.TrustedDevices.has_value() ||
         Response.PairingCandidate.has_value() ||
         Response.NearbyPeers.has_value() ||
         Response.PairingDecision.has_value() ||
         Response.PermissionCandidate.has_value() ||
         Response.PairingOperation.has_value())) {
        return false;
    }
    const auto PayloadCount = static_cast<unsigned>(Response.State.has_value()) +
        static_cast<unsigned>(Response.Topologies.has_value()) +
        static_cast<unsigned>(Response.Preferences.has_value()) +
        static_cast<unsigned>(Response.TrustedDevices.has_value()) +
        static_cast<unsigned>(Response.PairingCandidate.has_value()) +
        static_cast<unsigned>(Response.NearbyPeers.has_value()) +
        static_cast<unsigned>(Response.PairingDecision.has_value()) +
        static_cast<unsigned>(Response.PermissionCandidate.has_value()) +
        static_cast<unsigned>(Response.PairingOperation.has_value());
    if (PayloadCount > 1) return false;
    return (!Response.State || IsValidControlState(*Response.State)) &&
           (!Response.Topologies ||
            IsValidControlTopologyState(*Response.Topologies)) &&
           (!Response.Preferences ||
            IsValidProductPreferences(*Response.Preferences)) &&
           (!Response.TrustedDevices ||
            IsValidControlTrustedDeviceList(*Response.TrustedDevices)) &&
           (!Response.PairingCandidate ||
            IsValidControlPairingCandidate(*Response.PairingCandidate)) &&
           (!Response.NearbyPeers ||
            IsValidControlNearbyPeerList(*Response.NearbyPeers)) &&
           (!Response.PairingDecision ||
             static_cast<std::uint8_t>(*Response.PairingDecision) <=
                 static_cast<std::uint8_t>(
                     ControlManagedPairingDecision::Rejected)) &&
           (!Response.PermissionCandidate ||
            IsValidControlPermissionCandidate(
                *Response.PermissionCandidate)) &&
           (!Response.PairingOperation ||
            IsValidControlPairingOperation(*Response.PairingOperation));
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
        : Response.NearbyPeers ? 6u
        : Response.PairingDecision ? 7u
        : Response.PermissionCandidate ? 8u
        : Response.PairingOperation ? 9u
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
    } else if (Response.NearbyPeers) {
        EncodeNearbyPeers(Payload, *Response.NearbyPeers);
    } else if (Response.PairingDecision) {
        Payload.U8(static_cast<std::uint8_t>(*Response.PairingDecision));
    } else if (Response.PermissionCandidate) {
        EncodePermissionCandidate(Payload, *Response.PermissionCandidate);
    } else if (Response.PairingOperation) {
        Payload.U64(Response.PairingOperation->OperationId);
        Payload.U8(static_cast<std::uint8_t>(
            Response.PairingOperation->Phase));
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
    if (!Input.U16(RawStatus) || !Input.U8(PayloadKind) || PayloadKind > 9) {
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
    } else if (PayloadKind == 6) {
        Response.NearbyPeers = DecodeNearbyPeers(Input);
        if (!Response.NearbyPeers) {
            return {std::nullopt, ControlDecodeError::InvalidPayload};
        }
    } else if (PayloadKind == 7) {
        std::uint8_t RawDecision{};
        if (!Input.U8(RawDecision) ||
            RawDecision > static_cast<std::uint8_t>(
                ControlManagedPairingDecision::Rejected)) {
            return {std::nullopt, ControlDecodeError::InvalidPayload};
        }
        Response.PairingDecision =
            static_cast<ControlManagedPairingDecision>(RawDecision);
    } else if (PayloadKind == 8) {
        Response.PermissionCandidate = DecodePermissionCandidate(Input);
        if (!Response.PermissionCandidate) {
            return {std::nullopt, ControlDecodeError::InvalidPayload};
        }
    } else if (PayloadKind == 9) {
        ControlPairingOperation Operation;
        std::uint8_t RawPhase{};
        if (!Input.U64(Operation.OperationId) || !Input.U8(RawPhase)) {
            return {std::nullopt, ControlDecodeError::InvalidPayload};
        }
        Operation.Phase = static_cast<ControlPairingPhase>(RawPhase);
        if (!IsValidControlPairingOperation(Operation)) {
            return {std::nullopt, ControlDecodeError::InvalidPayload};
        }
        Response.PairingOperation = Operation;
    }
    if (Input.Remaining() != 0 || !IsValidControlResponse(Response)) {
        return {std::nullopt, ControlDecodeError::InvalidPayload};
    }
    return {std::move(Response), ControlDecodeError::None};
}

} // namespace desklink
