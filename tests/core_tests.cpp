#include "desklink/agent.hpp"
#include "desklink/audio.hpp"
#include "desklink/capabilities.hpp"
#include "desklink/control.hpp"
#include "desklink/display_topology.hpp"
#include "desklink/discovery.hpp"
#include "desklink/host.hpp"
#include "desklink/host_input_lifecycle.hpp"
#include "desklink/input.hpp"
#include "desklink/pairing.hpp"
#include "desklink/pairing_wire.hpp"
#include "desklink/profile.hpp"
#include "desklink/protocol.hpp"
#include "desklink/session.hpp"
#include "desklink/transport.hpp"
#include "desklink/types.hpp"
#ifdef _WIN32
#include "desklink/win32_capture.hpp"
#include "desklink/win32_control.hpp"
#include "desklink/win32_device_certificate.hpp"
#include "desklink/win32_display_topology.hpp"
#include "desklink/win32_foreground.hpp"
#include "desklink/win32_pairing.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

#define CHECK(expr) do { if (!(expr)) { \
    std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": " #expr "\n"; \
    std::exit(1); \
} } while (false)

class ManualClock final : public desklink::IClock {
public:
    time_point now() const noexcept override { return current_; }
    void advance(std::chrono::milliseconds delta) { current_ += delta; }
private:
    time_point current_{};
};

desklink::DiscoveryAdvertisement MakeDiscoveryAdvertisement(
    std::uint8_t Marker = 1) {
    desklink::DiscoveryAdvertisement Result;
    Result.Machine[0] = Marker;
    Result.DisplayName = "DeskLink test PC";
    Result.Port = 4433;
    Result.CapabilityHints = 4;
    Result.PairingAvailable = true;
    return Result;
}

desklink::DiscoveryEndpoint MakeDiscoveryEndpoint(
    std::uint8_t Marker = 1,
    std::string HostName = "desklink-test.local",
    std::uint32_t InterfaceIndex = 7) {
    desklink::DiscoveryEndpoint Result;
    Result.Advertisement = MakeDiscoveryAdvertisement(Marker);
    Result.InstanceName = "DeskLink test PC._desklink._udp.local";
    Result.HostName = std::move(HostName);
    Result.InterfaceIndex = InterfaceIndex;
    return Result;
}

class RecordingInjector final : public desklink::IInputInjector {
public:
    bool inject_key(const desklink::KeyEventMessage& event) override {
        keys.push_back(event); return true;
    }
    bool inject_button(const desklink::MouseButtonMessage& event) override {
        buttons.push_back(event); return true;
    }
    bool inject_pointer(const desklink::PointerPositionMessage& event) override {
        pointers.push_back(event); return true;
    }
    bool InjectWheel(const desklink::MouseWheelMessage& Message) override {
        wheels.push_back(Message); return true;
    }
    bool ReconcileState(const desklink::InputStateSnapshotMessage& Snapshot) override {
        snapshots.push_back(Snapshot); return ReconcileSucceeds;
    }
    void release_owned_state() noexcept override { ++release_calls; }

    std::vector<desklink::KeyEventMessage> keys;
    std::vector<desklink::MouseButtonMessage> buttons;
    std::vector<desklink::PointerPositionMessage> pointers;
    std::vector<desklink::MouseWheelMessage> wheels;
    std::vector<desklink::InputStateSnapshotMessage> snapshots;
    bool ReconcileSucceeds{true};
    int release_calls{};
};

class DeterministicPairingCrypto final : public desklink::IPairingCrypto {
public:
    explicit DeterministicPairingCrypto(std::uint8_t Seed) : Seed_(Seed) {}

    bool FillRandom(std::span<std::uint8_t> Bytes) override {
        for (auto& Byte : Bytes) Byte = Seed_++;
        return true;
    }

    std::optional<desklink::Sha256Digest> HashSha256(desklink::ByteSpan Bytes) const override {
        desklink::Sha256Digest Digest{};
        std::uint32_t State = 2166136261u;
        for (const auto Byte : Bytes) {
            State ^= Byte;
            State *= 16777619u;
            Digest[State % Digest.size()] ^= static_cast<std::uint8_t>(State >> 16u);
        }
        for (std::size_t Index = 0; Index < Digest.size(); ++Index) {
            State = State * 1664525u + 1013904223u;
            Digest[Index] ^= static_cast<std::uint8_t>(State >> 24u);
        }
        return Digest;
    }

private:
    std::uint8_t Seed_;
};

desklink::MachineId MakeMachineId(std::uint8_t Marker) {
    desklink::MachineId Result{};
    Result[0] = Marker;
    Result[15] = static_cast<std::uint8_t>(Marker ^ 0xA5u);
    return Result;
}

desklink::Sha256Digest MakeDigest(std::uint8_t Marker) {
    desklink::Sha256Digest Result{};
    for (std::size_t Index = 0; Index < Result.size(); ++Index) {
        Result[Index] = static_cast<std::uint8_t>(Marker + Index);
    }
    return Result;
}

desklink::PeerIdentity MakeIdentity(std::uint8_t Marker, std::string Name) {
    desklink::PeerIdentity Result;
    Result.machine_id = MakeMachineId(Marker);
    Result.display_name = std::move(Name);
    Result.public_key_fingerprint = desklink::FormatFingerprint(MakeDigest(Marker));
    return Result;
}

void SaveTrustedPeer(desklink::InMemoryTrustStore& Store,
                     const desklink::PeerIdentity& Identity,
                     desklink::CapabilitySet Capabilities = {}) {
    CHECK(Store.SavePeer(desklink::TrustedPeer{Identity, Capabilities}));
}

void protocol_round_trip() {
    using namespace desklink;
    EnvelopeHeader h;
    h.session_nonce = 42;
    h.epoch = 7;
    h.sequence = 99;
    PointerPositionMessage pointer{3, 12345, 54321};

    auto bytes = encode_packet(h, pointer);
    auto decoded = decode_packet(bytes, true);
    CHECK(decoded.packet.has_value());
    CHECK(decoded.packet->header.session_nonce == 42);
    CHECK(decoded.packet->header.epoch == 7);
    CHECK(decoded.packet->header.sequence == 99);
    const auto& got = std::get<PointerPositionMessage>(decoded.packet->message);
    CHECK(got.display_id == 3);
    CHECK(got.normalized_x == 12345);
    CHECK(got.normalized_y == 54321);
}

void ControlProtocolRoundTripAndValidation() {
    using namespace desklink;

    const std::array<ControlRequest, 5> Requests{
        ControlRequest{1, GetStateControlRequest{}},
        ControlRequest{2, SetDesiredModeControlRequest{DeskMode::LockPc1}},
        ControlRequest{3, FocusMachineControlRequest{
            MachineId{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}}},
        ControlRequest{4, SetAudioGainControlRequest{7'500}},
        ControlRequest{5, ToggleAudioMuteControlRequest{}},
    };
    for (const auto& Request : Requests) {
        const auto Frame = EncodeControlRequest(Request);
        CHECK(Frame.has_value());
        CHECK(Frame->size() <= kMaximumControlFrameSize);
        const auto Decoded = DecodeControlRequest(*Frame);
        CHECK(Decoded.Decoded.has_value());
        CHECK(Decoded.Decoded->RequestId == Request.RequestId);
        CHECK(Decoded.Decoded->Payload.index() == Request.Payload.index());
    }

    ControlState State;
    State.LocalMachine[0] = 0x11;
    State.FocusedMachine[0] = 0x22;
    State.Role = ControlRole::Host;
    State.DesiredMode = DeskMode::Roam;
    State.ConnectedPeerCount = 1;
    State.AudioGainPermyriad = 8'000;
    State.RemoteFocused = true;
    State.CaptureActive = true;
    const auto ResponseFrame = EncodeControlResponse(
        ControlResponse{9, ControlStatus::Ok, State});
    CHECK(ResponseFrame.has_value());
    const auto Response = DecodeControlResponse(*ResponseFrame);
    CHECK(Response.Decoded.has_value());
    CHECK(Response.Decoded->RequestId == 9);
    CHECK(Response.Decoded->Status == ControlStatus::Ok);
    CHECK(Response.Decoded->State.has_value());
    CHECK(Response.Decoded->State->FocusedMachine == State.FocusedMachine);
    CHECK(Response.Decoded->State->CaptureActive);

    CHECK(!EncodeControlRequest(ControlRequest{}).has_value());
    CHECK(!EncodeControlRequest(ControlRequest{
        6, SetDesiredModeControlRequest{static_cast<DeskMode>(99)}}).has_value());
    CHECK(!EncodeControlRequest(ControlRequest{
        7, FocusMachineControlRequest{}}).has_value());
    CHECK(!EncodeControlRequest(ControlRequest{
        8, SetAudioGainControlRequest{10'001}}).has_value());
    CHECK(!EncodeControlResponse(ControlResponse{
        10, ControlStatus::Failed, State}).has_value());

    auto WrongType = *EncodeControlRequest(Requests[0]);
    WrongType[7] = static_cast<std::uint8_t>(ControlFrameType::Response);
    CHECK(!DecodeControlRequest(WrongType).Decoded.has_value());
    CHECK(DecodeControlRequest(WrongType).Error ==
          ControlDecodeError::InvalidHeader);

    auto Oversized = *EncodeControlRequest(Requests[0]);
    Oversized[16] = 0;
    Oversized[17] = 0;
    Oversized[18] = 0;
    Oversized[19] = static_cast<std::uint8_t>(kMaximumControlPayload + 1);
    CHECK(!DecodeControlRequest(Oversized).Decoded.has_value());
    CHECK(DecodeControlRequest(Oversized).Error == ControlDecodeError::Oversized);

    auto Trailing = *EncodeControlRequest(Requests[0]);
    Trailing.push_back(0);
    CHECK(!DecodeControlRequest(Trailing).Decoded.has_value());
    CHECK(DecodeControlRequest(Trailing).Error ==
          ControlDecodeError::InvalidPayload);
}

void MouseWheelRoundTripAndValidation() {
    using namespace desklink;
    EnvelopeHeader Header;
    Header.session_nonce = 43;
    Header.epoch = 8;
    Header.sequence = 100;

    const auto Bytes = encode_packet(
        Header, MouseWheelMessage{MouseWheelAxis::Horizontal, -120});
    const auto Decoded = decode_packet(Bytes, false);
    CHECK(Decoded.packet.has_value());
    const auto& Wheel = std::get<MouseWheelMessage>(Decoded.packet->message);
    CHECK(Wheel.Axis == MouseWheelAxis::Horizontal);
    CHECK(Wheel.Delta == -120);

    const auto WrongLane = decode_packet(Bytes, true);
    CHECK(!WrongLane.packet.has_value());
    CHECK(WrongLane.error == DecodeError::InvalidPayload);
    CHECK(!decode_packet(
        encode_packet(Header, MouseWheelMessage{MouseWheelAxis::Vertical, 0}),
        false).packet.has_value());
    CHECK(!decode_packet(
        encode_packet(Header, MouseWheelMessage{
            MouseWheelAxis::Vertical,
            static_cast<std::int16_t>(kMaximumMouseWheelDelta + 1)}),
        false).packet.has_value());
    CHECK(!decode_packet(
        encode_packet(Header, MouseWheelMessage{
            static_cast<MouseWheelAxis>(3), 120}), false).packet.has_value());
    CHECK(decode_packet(
        encode_packet(Header, MouseWheelMessage{
            MouseWheelAxis::Vertical, kMaximumMouseWheelDelta}),
        false).packet.has_value());
    CHECK(decode_packet(
        encode_packet(Header, MouseWheelMessage{
            MouseWheelAxis::Horizontal,
            static_cast<std::int16_t>(-kMaximumMouseWheelDelta)}),
        false).packet.has_value());
}

void DisplayTopologyMappingIsStableAndInvalidates() {
    using namespace desklink;

    DisplayTopologyMap Topology;
    std::vector<DiscoveredDisplay> Displays{
        {"monitor-right", "Right", {0, 0, 1920, 1080}, true},
        {"monitor-left", "Left", {-1920, 0, 0, 1080}, false},
    };
    CHECK(Topology.Update(Displays) == DisplayTopologyUpdate::Changed);
    const auto FirstGeneration = Topology.Current().Generation;
    CHECK(FirstGeneration == 1);
    CHECK(Topology.Current().VirtualBounds == (DisplayRect{-1920, 0, 1920, 1080}));

    const auto LeftId = DeriveStableDisplayId("monitor-left");
    const auto RightId = DeriveStableDisplayId("monitor-right");
    CHECK(LeftId != 0);
    CHECK(RightId != 0);
    CHECK(LeftId != RightId);
    CHECK(Topology.Current().Find(LeftId) != nullptr);
    CHECK(Topology.Current().Find(RightId) != nullptr);

    const auto LeftStart = Topology.MapToVirtualDesktop(LeftId, FirstGeneration, 0, 0);
    const auto LeftEnd = Topology.MapToVirtualDesktop(LeftId, FirstGeneration, 65535, 65535);
    const auto RightStart = Topology.MapToVirtualDesktop(RightId, FirstGeneration, 0, 0);
    const auto RightEnd = Topology.MapToVirtualDesktop(RightId, FirstGeneration, 65535, 65535);
    CHECK(LeftStart == (NormalizedDisplayPoint{0, 0}));
    CHECK(LeftEnd.has_value());
    CHECK(LeftEnd->X < 32768);
    CHECK(LeftEnd->Y == 65535);
    CHECK(RightStart.has_value());
    CHECK(RightStart->X > 32767);
    CHECK(RightStart->Y == 0);
    CHECK(RightEnd == (NormalizedDisplayPoint{65535, 65535}));

    std::reverse(Displays.begin(), Displays.end());
    for (auto& Display : Displays) {
        if (Display.StableIdentity == "monitor-right") {
            Display.FriendlyName = "Renamed right monitor";
        }
    }
    CHECK(Topology.Update(Displays) == DisplayTopologyUpdate::Unchanged);
    CHECK(Topology.Current().Generation == FirstGeneration);
    CHECK(Topology.Current().Find(RightId)->FriendlyName == "Renamed right monitor");

    Displays[0].Bounds.Right = 2560;
    CHECK(Topology.Update(Displays) == DisplayTopologyUpdate::Changed);
    const auto SecondGeneration = Topology.Current().Generation;
    CHECK(SecondGeneration == FirstGeneration + 1);
    CHECK(!Topology.MapToVirtualDesktop(LeftId, FirstGeneration, 0, 0));
    CHECK(Topology.MapToVirtualDesktop(LeftId, SecondGeneration, 0, 0));
    CHECK(!Topology.MapToVirtualDesktop(kLegacyVirtualDesktopDisplayId,
                                        SecondGeneration, 0, 0));
    CHECK(!Topology.MapToVirtualDesktop(65535, SecondGeneration, 0, 0));

    const auto BeforeInvalid = Topology.Current().Generation;
    CHECK(Topology.Update({{"no-primary", "Invalid", {0, 0, 100, 100}, false}}) ==
          DisplayTopologyUpdate::Invalid);
    CHECK(Topology.Current().Generation == BeforeInvalid);
    CHECK(!MapDisplayPointToVirtualDesktop(
        {0, 0, 0, 100}, {0, 0, 100, 100}, 0, 0));
}

void DisplayTopologyRejectsAmbiguousStableIds() {
    using namespace desklink;

    std::unordered_map<DisplayId, std::string> Seen;
    std::string FirstIdentity;
    std::string SecondIdentity;
    for (std::uint32_t Index = 0; Index <= 65535 && FirstIdentity.empty(); ++Index) {
        auto Identity = std::string("collision-monitor-") + std::to_string(Index);
        const auto Id = DeriveStableDisplayId(Identity);
        const auto [It, Inserted] = Seen.emplace(Id, Identity);
        if (!Inserted && It->second != Identity) {
            FirstIdentity = It->second;
            SecondIdentity = std::move(Identity);
        }
    }
    CHECK(!FirstIdentity.empty());
    CHECK(DeriveStableDisplayId(FirstIdentity) == DeriveStableDisplayId(SecondIdentity));

    DisplayTopologyMap Topology;
    CHECK(Topology.Update({
        {FirstIdentity, "First", {0, 0, 100, 100}, true},
        {SecondIdentity, "Second", {100, 0, 200, 100}, false},
    }) == DisplayTopologyUpdate::Invalid);
    CHECK(Topology.Current().Generation == 0);
    CHECK(Topology.Current().Displays.empty());
}

void InputStateSnapshotRoundTripAndValidation() {
    using namespace desklink;
    InputStateSnapshotMessage Snapshot;
    CHECK(SetInputSnapshotKey(Snapshot, 0x1E, false, true));
    CHECK(SetInputSnapshotKey(Snapshot, 0x1D, true, true));
    CHECK(SetInputSnapshotButton(Snapshot, MouseButtonId::X2, true));
    CHECK(!SetInputSnapshotKey(Snapshot, 0, false, true));
    CHECK(!SetInputSnapshotKey(Snapshot, 256, false, true));

    EnvelopeHeader Header;
    auto Bytes = encode_packet(Header, Snapshot);
    auto Decoded = decode_packet(Bytes, false);
    CHECK(Decoded.packet.has_value());
    const auto& Restored = std::get<InputStateSnapshotMessage>(Decoded.packet->message);
    CHECK(InputSnapshotKeyDown(Restored, 0x1E, false));
    CHECK(!InputSnapshotKeyDown(Restored, 0x1E, true));
    CHECK(InputSnapshotKeyDown(Restored, 0x1D, true));
    CHECK(InputSnapshotButtonDown(Restored, MouseButtonId::X2));

    Bytes.back() |= 0x80u;
    auto ReservedBits = decode_packet(Bytes, false);
    CHECK(!ReservedBits.packet.has_value());
    CHECK(ReservedBits.error == DecodeError::InvalidPayload);

    auto InvalidKey = encode_packet(Header, KeyEventMessage{256, false, true});
    CHECK(!decode_packet(InvalidKey, false).packet.has_value());
}

void InputStateTransitionsReleaseBeforePress() {
    using namespace desklink;
    InputStateSnapshotMessage Current;
    InputStateSnapshotMessage Desired;
    CHECK(SetInputSnapshotKey(Current, 0x1E, false, true));
    CHECK(SetInputSnapshotButton(Current, MouseButtonId::Right, true));
    CHECK(SetInputSnapshotKey(Desired, 0x1D, true, true));
    CHECK(SetInputSnapshotButton(Desired, MouseButtonId::Left, true));

    const auto Transitions = BuildInputStateTransitions(Current, Desired);
    CHECK(Transitions.size() == 4);
    CHECK(std::holds_alternative<KeyEventMessage>(Transitions[0]));
    CHECK(!std::get<KeyEventMessage>(Transitions[0]).down);
    CHECK(std::holds_alternative<MouseButtonMessage>(Transitions[1]));
    CHECK(!std::get<MouseButtonMessage>(Transitions[1]).down);
    CHECK(std::holds_alternative<KeyEventMessage>(Transitions[2]));
    CHECK(std::get<KeyEventMessage>(Transitions[2]).extended);
    CHECK(std::get<KeyEventMessage>(Transitions[2]).down);
    CHECK(std::holds_alternative<MouseButtonMessage>(Transitions[3]));
    CHECK(std::get<MouseButtonMessage>(Transitions[3]).down);
}

void rejects_wrong_lane_and_oversize() {
    using namespace desklink;
    EnvelopeHeader h;
    auto key = encode_packet(h, KeyEventMessage{30, false, true});
    auto wrong_lane = decode_packet(key, true);
    CHECK(!wrong_lane.packet.has_value());
    CHECK(wrong_lane.error == DecodeError::InvalidPayload);

    AudioFrameMessage frame;
    frame.frames_per_channel = 300;
    frame.channels = 2;
    frame.bytes_per_sample = 2;
    frame.pcm.resize(1200, 0);
    auto huge = encode_packet(h, frame);
    auto oversized = decode_packet(huge, true);
    CHECK(!oversized.packet.has_value());
    CHECK(oversized.error == DecodeError::PayloadTooLarge);
}

void capability_and_lease_gate_input() {
    using namespace desklink;
    ManualClock clock;
    RecordingInjector injector;
    AgentCoordinator agent(clock, injector);

    EnvelopeHeader request_header;
    request_header.session_nonce = 1;
    auto req_bytes = encode_packet(request_header, FocusRequestMessage{750, 1});
    auto req = decode_packet(req_bytes, false);
    CHECK(req.packet.has_value());
    CHECK(agent.handle(*req.packet) == AgentDecision::RejectedCapability);

    CapabilitySet caps;
    caps.grant(Capability::InputInject);
    agent.set_peer_capabilities(caps);
    CHECK(agent.handle(*req.packet) == AgentDecision::Accepted);
    const auto epoch = agent.focus_state().epoch();

    EnvelopeHeader key_header;
    key_header.session_nonce = 1;
    key_header.epoch = epoch;
    key_header.sequence = 1;
    auto key_bytes = encode_packet(key_header, KeyEventMessage{0x1D, false, true});
    auto key = decode_packet(key_bytes, false);
    CHECK(key.packet.has_value());
    CHECK(agent.handle(*key.packet) == AgentDecision::Accepted);
    CHECK(injector.keys.size() == 1);

    auto WheelBytes = encode_packet(
        key_header, MouseWheelMessage{MouseWheelAxis::Vertical, 120});
    auto Wheel = decode_packet(WheelBytes, false);
    CHECK(Wheel.packet.has_value());
    CHECK(agent.handle(*Wheel.packet) == AgentDecision::Accepted);
    CHECK(injector.wheels.size() == 1);

    clock.advance(std::chrono::milliseconds(751));
    agent.tick();
    CHECK(injector.release_calls == 1);
    CHECK(agent.handle(*key.packet) == AgentDecision::RejectedEpoch);
}

void DesiredModeControlIsCapabilityGatedAndFailsLocal() {
    using namespace desklink;

    ManualClock Clock;
    RecordingInjector Injector;
    AgentCoordinator Agent(Clock, Injector);
    EnvelopeHeader Header;
    const auto LockPacket = decode_packet(
        encode_packet(Header, SetModeMessage{DeskMode::LockPc1}), false);
    CHECK(LockPacket.packet.has_value());
    CHECK(Agent.handle(*LockPacket.packet) ==
          AgentDecision::RejectedCapability);

    CapabilitySet Capabilities;
    Capabilities.grant(Capability::InputInject);
    Agent.set_peer_capabilities(Capabilities);
    const auto FocusPacket = decode_packet(
        encode_packet(Header, FocusRequestMessage{750, 1}), false);
    CHECK(FocusPacket.packet.has_value());
    CHECK(Agent.handle(*FocusPacket.packet) == AgentDecision::Accepted);
    CHECK(Agent.RemoteFocused());

    CHECK(Agent.handle(*LockPacket.packet) == AgentDecision::Accepted);
    CHECK(Agent.DesiredMode() == DeskMode::LockPc1);
    CHECK(!Agent.RemoteFocused());
    CHECK(Injector.release_calls == 1);
    CHECK(Agent.handle(*FocusPacket.packet) == AgentDecision::RejectedLease);

    const auto RoamPacket = decode_packet(
        encode_packet(Header, SetModeMessage{DeskMode::Roam}), false);
    CHECK(RoamPacket.packet.has_value());
    Agent.SetLocalDesiredMode(DeskMode::Game);
    CHECK(Agent.handle(*RoamPacket.packet) == AgentDecision::Accepted);
    CHECK(Agent.DesiredMode() == DeskMode::Game);
    CHECK(Agent.handle(*FocusPacket.packet) == AgentDecision::RejectedLease);
    Agent.SetLocalDesiredMode(DeskMode::Roam);
    CHECK(Agent.handle(*FocusPacket.packet) == AgentDecision::Accepted);
    CHECK(Agent.RemoteFocused());
}

void stale_epoch_rejected_after_refocus() {
    using namespace desklink;
    ManualClock clock;
    RecordingInjector injector;
    AgentCoordinator agent(clock, injector);
    CapabilitySet caps;
    caps.grant(Capability::InputInject);
    agent.set_peer_capabilities(caps);

    EnvelopeHeader h;
    auto request = decode_packet(encode_packet(h, FocusRequestMessage{750, 1}), false);
    CHECK(request.packet.has_value());
    CHECK(agent.handle(*request.packet) == AgentDecision::Accepted);
    const auto old_epoch = agent.focus_state().epoch();

    EnvelopeHeader release_h;
    release_h.epoch = old_epoch;
    auto release = decode_packet(encode_packet(release_h, FocusReleaseMessage{}), false);
    CHECK(release.packet.has_value());
    CHECK(agent.handle(*release.packet) == AgentDecision::Accepted);

    CHECK(agent.handle(*request.packet) == AgentDecision::Accepted);
    const auto new_epoch = agent.focus_state().epoch();
    CHECK(new_epoch != old_epoch);

    EnvelopeHeader stale_h;
    stale_h.epoch = old_epoch;
    auto stale = decode_packet(encode_packet(stale_h, KeyEventMessage{0x2A, false, true}), false);
    CHECK(stale.packet.has_value());
    CHECK(agent.handle(*stale.packet) == AgentDecision::RejectedEpoch);
}

void host_agent_focus_transaction() {
    using namespace desklink;
    constexpr std::uint64_t nonce = 0x12345678u;
    ManualClock clock;
    RecordingInjector injector;
    AgentCoordinator agent(clock, injector);
    CapabilitySet caps;
    caps.grant(Capability::InputInject);
    agent.set_peer_capabilities(caps);
    HostCoordinator host(nonce);

    auto request = decode_packet(host.request_remote_focus(750), false);
    CHECK(request.packet.has_value());
    CHECK(agent.handle(*request.packet) == AgentDecision::Accepted);

    EnvelopeHeader ready_h;
    ready_h.session_nonce = nonce;
    ready_h.epoch = agent.focus_state().epoch();
    ready_h.sequence = 1;
    auto ready = decode_packet(encode_packet(ready_h, FocusReadyMessage{750, 1}), false);
    CHECK(ready.packet.has_value());
    CHECK(host.accept_focus_ready(*ready.packet));
    CHECK(host.remote_focused());

    CHECK(!host.MouseWheel(
        MouseWheelMessage{MouseWheelAxis::Vertical, 0}).has_value());
    CHECK(!host.MouseWheel(MouseWheelMessage{
        MouseWheelAxis::Vertical,
        static_cast<std::int16_t>(kMaximumMouseWheelDelta + 1)}).has_value());

    auto pointer_bytes = host.pointer_position(PointerPositionMessage{0, 100, 200});
    CHECK(pointer_bytes.has_value());
    auto pointer = decode_packet(*pointer_bytes, true);
    CHECK(pointer.packet.has_value());
    CHECK(agent.handle(*pointer.packet) == AgentDecision::Accepted);
    CHECK(injector.pointers.size() == 1);

    auto WheelBytes = host.MouseWheel(
        MouseWheelMessage{MouseWheelAxis::Vertical, 120});
    CHECK(WheelBytes.has_value());
    auto Wheel = decode_packet(*WheelBytes, false);
    CHECK(Wheel.packet.has_value());
    CHECK(agent.handle(*Wheel.packet) == AgentDecision::Accepted);
    CHECK(injector.wheels.size() == 1);

    host.emergency_fail_local();
    CHECK(!host.remote_focused());
    CHECK(host.desired_mode() == DeskMode::LockPc1);
}

void jitter_buffer_reorders_and_conceals() {
    using namespace desklink;
    AudioJitterBuffer jitter(2, 8);
    auto make_frame = [](std::uint8_t marker) {
        AudioFrameMessage f;
        f.frames_per_channel = 2;
        f.channels = 2;
        f.bytes_per_sample = 2;
        f.pcm.assign(8, marker);
        return f;
    };

    CHECK(jitter.push(10, make_frame(10)));
    CHECK(jitter.push(12, make_frame(12)));
    auto ten = jitter.pop();
    CHECK(ten.has_value() && !ten->concealed && ten->frame.pcm[0] == 10);

    CHECK(jitter.push(13, make_frame(13)));
    auto eleven = jitter.pop();
    CHECK(eleven.has_value() && eleven->concealed);
    CHECK(eleven->frame.pcm[0] == 0);
    CHECK(jitter.concealed_frames() == 1);

    auto twelve = jitter.pop();
    CHECK(twelve.has_value() && !twelve->concealed && twelve->frame.pcm[0] == 12);
}



void out_of_order_pointer_rejected() {
    using namespace desklink;
    ManualClock clock;
    RecordingInjector injector;
    AgentCoordinator agent(clock, injector);
    CapabilitySet caps;
    caps.grant(Capability::InputInject);
    agent.set_peer_capabilities(caps);

    EnvelopeHeader request_h;
    auto request = decode_packet(encode_packet(request_h, FocusRequestMessage{750, 55}), false);
    CHECK(request.packet.has_value());
    CHECK(agent.handle(*request.packet) == AgentDecision::Accepted);
    const auto epoch = agent.focus_state().epoch();

    EnvelopeHeader newest_h;
    newest_h.epoch = epoch;
    newest_h.sequence = 20;
    auto newest = decode_packet(encode_packet(newest_h, PointerPositionMessage{0, 50000, 50000}), true);
    CHECK(newest.packet.has_value());
    CHECK(agent.handle(*newest.packet) == AgentDecision::Accepted);

    EnvelopeHeader old_h;
    old_h.epoch = epoch;
    old_h.sequence = 19;
    auto old = decode_packet(encode_packet(old_h, PointerPositionMessage{0, 100, 100}), true);
    CHECK(old.packet.has_value());
    CHECK(agent.handle(*old.packet) == AgentDecision::RejectedSequence);
    CHECK(injector.pointers.size() == 1);
}

void stale_focus_ready_cannot_win_new_transaction() {
    using namespace desklink;
    constexpr std::uint64_t nonce = 9001;
    HostCoordinator host(nonce);

    auto first_request = decode_packet(host.request_remote_focus(750), false);
    CHECK(first_request.packet.has_value());
    const auto first_id = std::get<FocusRequestMessage>(first_request.packet->message).request_id;

    auto second_request = decode_packet(host.request_remote_focus(750), false);
    CHECK(second_request.packet.has_value());
    const auto second_id = std::get<FocusRequestMessage>(second_request.packet->message).request_id;
    CHECK(second_id != first_id);

    EnvelopeHeader stale_h;
    stale_h.session_nonce = nonce;
    stale_h.epoch = 10;
    auto stale_ready = decode_packet(encode_packet(stale_h, FocusReadyMessage{750, first_id}), false);
    CHECK(stale_ready.packet.has_value());
    CHECK(!host.accept_focus_ready(*stale_ready.packet));
    CHECK(!host.remote_focused());

    EnvelopeHeader current_h;
    current_h.session_nonce = nonce;
    current_h.epoch = 11;
    auto current_ready = decode_packet(encode_packet(current_h, FocusReadyMessage{750, second_id}), false);
    CHECK(current_ready.packet.has_value());
    CHECK(host.accept_focus_ready(*current_ready.packet));
    CHECK(host.remote_epoch() == 11);
}

void secure_session_end_to_end() {
    using namespace desklink;
    constexpr std::uint64_t nonce = 0xBADC0FFEEu;

    TransportPeerInfo host_view;
    host_view.authenticated = true;
    host_view.encrypted = true;
    host_view.identity = MakeIdentity(2, "PC2");
    TransportPeerInfo agent_view;
    agent_view.authenticated = true;
    agent_view.encrypted = true;
    agent_view.identity = MakeIdentity(1, "PC1");
    auto pair = make_in_memory_transport_pair(host_view, agent_view);

    ManualClock clock;
    RecordingInjector injector;
    AgentCoordinator agent_core(clock, injector);
    CapabilitySet caps;
    caps.grant(Capability::InputInject);
    InMemoryTrustStore host_trust;
    InMemoryTrustStore agent_trust;
    SaveTrustedPeer(host_trust, host_view.identity);
    SaveTrustedPeer(agent_trust, agent_view.identity, caps);
    HostCoordinator host_core(nonce);

    AgentSession agent(pair.b, agent_core, agent_trust, nonce);
    bool FocusReadyNotified = false;
    HostSession host(pair.a, host_core, host_trust, nonce, [&] {
        FocusReadyNotified = true;
    });
    CHECK(agent.start());
    CHECK(host.start());
    CHECK(host.focus_remote(750));
    CHECK(host_core.remote_focused());
    CHECK(host.RemoteFocused());
    CHECK(FocusReadyNotified);
    CHECK(host.send_key(KeyEventMessage{0x20, false, true}));
    CHECK(host.send_key(KeyEventMessage{0x1D, true, true}));
    CHECK(host.send_button(MouseButtonMessage{MouseButtonId::X1, true}));
    CHECK(host.SendInputStateSnapshot());
    CHECK(host.send_pointer(PointerPositionMessage{0, 30000, 31000}));
    CHECK(host.SendWheel(MouseWheelMessage{MouseWheelAxis::Horizontal, -120}));
    CHECK(injector.keys.size() == 2);
    CHECK(injector.buttons.size() == 1);
    CHECK(injector.snapshots.size() == 1);
    CHECK(InputSnapshotKeyDown(injector.snapshots.back(), 0x20, false));
    CHECK(InputSnapshotKeyDown(injector.snapshots.back(), 0x1D, true));
    CHECK(InputSnapshotButtonDown(injector.snapshots.back(), MouseButtonId::X1));
    CHECK(injector.pointers.size() == 1);
    CHECK(injector.wheels.size() == 1);

    clock.advance(std::chrono::milliseconds(800));
    agent.tick();
    CHECK(injector.release_calls == 1);
    CHECK(host.send_key(KeyEventMessage{0x20, false, false}));
    CHECK(injector.keys.size() == 2); // stale epoch was rejected
    CHECK(host.SendInputStateSnapshot());
    CHECK(injector.snapshots.size() == 1); // expired lease rejects reconciliation too
    CHECK(agent.stats().authorization_rejected >= 1);
    CHECK(host.SetDesiredMode(DeskMode::Game));
    CHECK(host.DesiredMode() == DeskMode::Game);
    CHECK(agent.DesiredMode() == DeskMode::Game);
    CHECK(!agent.RemoteFocused());
}

void insecure_transport_refused() {
    using namespace desklink;
    TransportPeerInfo insecure;
    insecure.authenticated = false;
    insecure.encrypted = true;
    insecure.identity = MakeIdentity(3, "Untrusted transport");
    auto pair = make_in_memory_transport_pair(insecure, insecure);
    ManualClock clock;
    RecordingInjector injector;
    AgentCoordinator agent_core(clock, injector);
    HostCoordinator host_core(7);
    InMemoryTrustStore trust;
    SaveTrustedPeer(trust, insecure.identity);
    AgentSession agent(pair.b, agent_core, trust, 7);
    HostSession host(pair.a, host_core, trust, 7);
    CHECK(!agent.start());
    CHECK(!host.start());
}

void UnpairedTransportIsRefused() {
    using namespace desklink;
    TransportPeerInfo peer;
    peer.authenticated = true;
    peer.encrypted = true;
    peer.identity = MakeIdentity(4, "Unknown PC");
    auto pair = make_in_memory_transport_pair(peer, peer);
    ManualClock clock;
    RecordingInjector injector;
    AgentCoordinator agent_core(clock, injector);
    HostCoordinator host_core(11);
    InMemoryTrustStore empty_trust;
    AgentSession agent(pair.b, agent_core, empty_trust, 11);
    HostSession host(pair.a, host_core, empty_trust, 11);
    CHECK(!agent.start());
    CHECK(!host.start());
}

void PinnedIdentityMismatchIsRefused() {
    using namespace desklink;
    TransportPeerInfo presented;
    presented.authenticated = true;
    presented.encrypted = true;
    presented.identity = MakeIdentity(5, "PC5");
    auto pair = make_in_memory_transport_pair(presented, presented);

    auto pinned = presented.identity;
    pinned.public_key_fingerprint = FormatFingerprint(MakeDigest(99));
    InMemoryTrustStore trust;
    SaveTrustedPeer(trust, pinned);

    ManualClock clock;
    RecordingInjector injector;
    AgentCoordinator agent_core(clock, injector);
    AgentSession agent(pair.b, agent_core, trust, 12);
    CHECK(!agent.start());
}

void PairingRequiresMatchingUserVerification() {
    using namespace desklink;
    ManualClock clock;
    DeterministicPairingCrypto first_crypto(10);
    DeterministicPairingCrypto second_crypto(80);
    InMemoryTrustStore first_trust;
    InMemoryTrustStore second_trust;
    PairingCoordinator first(
        MakeIdentity(1, "PC1"), MakeDigest(1), clock, first_crypto, first_trust);
    PairingCoordinator second(
        MakeIdentity(2, "PC2"), MakeDigest(2), clock, second_crypto, second_trust);

    CHECK(first.BeginPairing(std::chrono::seconds(60)));
    CHECK(second.BeginPairing(std::chrono::seconds(60)));
    const auto first_offer = first.CreateOffer();
    const auto second_offer = second.CreateOffer();
    CHECK(first_offer.has_value());
    CHECK(second_offer.has_value());

    const auto first_candidate = first.InspectOffer(*second_offer);
    const auto second_candidate = second.InspectOffer(*first_offer);
    CHECK(first_candidate.Status == PairingStatus::Ready);
    CHECK(second_candidate.Status == PairingStatus::Ready);
    CHECK(first_candidate.VerificationCode == second_candidate.VerificationCode);
    CHECK(first_candidate.VerificationCode.size() == 6);
    CHECK(!first.ConfirmOffer(*second_offer, "000000", CapabilitySet{}));

    CapabilitySet grant_to_second;
    grant_to_second.grant(Capability::InputInject);
    CHECK(first.ConfirmOffer(
        *second_offer, first_candidate.VerificationCode, grant_to_second));
    CHECK(second.ConfirmOffer(
        *first_offer, second_candidate.VerificationCode, CapabilitySet{}));
    CHECK(IsTrustedPeer(first_trust, first_candidate.Identity));
    CHECK(IsTrustedPeer(second_trust, second_candidate.Identity));
    CHECK(first_trust.GetPeer(second_offer->Machine)->Capabilities.contains(Capability::InputInject));
    CHECK(!first.IsPairingOpen());
    CHECK(!second.IsPairingOpen());
}

void PairingTranscriptDetectsPinTamperingAndExpiry() {
    using namespace desklink;
    ManualClock clock;
    DeterministicPairingCrypto first_crypto(1);
    DeterministicPairingCrypto second_crypto(2);
    InMemoryTrustStore first_trust;
    InMemoryTrustStore second_trust;
    PairingCoordinator first(
        MakeIdentity(20, "First"), MakeDigest(20), clock, first_crypto, first_trust);
    PairingCoordinator second(
        MakeIdentity(21, "Second"), MakeDigest(21), clock, second_crypto, second_trust);
    CHECK(first.BeginPairing(std::chrono::seconds(5)));
    CHECK(second.BeginPairing(std::chrono::seconds(5)));
    const auto first_offer = first.CreateOffer();
    const auto second_offer = second.CreateOffer();
    CHECK(first_offer.has_value() && second_offer.has_value());

    auto tampered_offer = *second_offer;
    tampered_offer.CertificatePin[0] ^= 0x5Au;
    const auto tampered_code = first.InspectOffer(tampered_offer).VerificationCode;
    const auto genuine_code = second.InspectOffer(*first_offer).VerificationCode;
    CHECK(tampered_code != genuine_code);

    clock.advance(std::chrono::milliseconds(5001));
    CHECK(!first.IsPairingOpen());
    CHECK(first.InspectOffer(*second_offer).Status == PairingStatus::WindowClosed);
    CHECK(!first.ConfirmOffer(*second_offer, genuine_code, CapabilitySet{}));
}

void PairingWireIsBoundedAndFragmentSafe() {
    using namespace desklink;
    PairingOffer Offer{
        MakeMachineId(31), "DeskLink peer", MakeDigest(31), {}};
    for (std::size_t Index = 0; Index < Offer.Nonce.size(); ++Index) {
        Offer.Nonce[Index] = static_cast<std::uint8_t>(Index + 1);
    }
    const auto Frame = EncodePairingOfferFrame(Offer);
    CHECK(Frame.has_value());
    CHECK(Frame->size() <= kMaxPairingFrameSize);
    const auto Decoded = DecodePairingOfferFrame(*Frame);
    CHECK(Decoded.has_value());
    CHECK(Decoded->Machine == Offer.Machine);
    CHECK(Decoded->DisplayName == Offer.DisplayName);
    CHECK(Decoded->CertificatePin == Offer.CertificatePin);
    CHECK(Decoded->Nonce == Offer.Nonce);

    PairingFrameDecoder Decoder;
    CHECK(Decoder.Push(ByteSpan{Frame->data(), 3}) == PairingWireStatus::Incomplete);
    CHECK(Decoder.Push(ByteSpan{Frame->data() + 3, Frame->size() - 3}) ==
          PairingWireStatus::Ready);
    CHECK(Decoder.ReadyType() == PairingWireFrameType::Offer);
    CHECK(Decoder.TakeOffer().has_value());
    CHECK(Decoder.Status() == PairingWireStatus::Incomplete);

    const auto Confirmation = EncodePairingConfirmationFrame();
    CHECK(Confirmation.size() == kPairingFrameHeaderSize);
    ByteBuffer Combined = *Frame;
    Combined.insert(Combined.end(), Confirmation.begin(), Confirmation.end());
    Decoder.Reset();
    CHECK(Decoder.Push(Combined) == PairingWireStatus::Ready);
    CHECK(Decoder.ReadyType() == PairingWireFrameType::Offer);
    CHECK(Decoder.TakeOffer().has_value());
    CHECK(Decoder.Status() == PairingWireStatus::Ready);
    CHECK(Decoder.ReadyType() == PairingWireFrameType::Confirmation);
    CHECK(Decoder.TakeConfirmation());
    CHECK(Decoder.Status() == PairingWireStatus::Incomplete);

    Decoder.Reset();
    CHECK(Decoder.Push(ByteSpan{Confirmation.data(), 2}) ==
          PairingWireStatus::Incomplete);
    CHECK(Decoder.Push(ByteSpan{Confirmation.data() + 2, Confirmation.size() - 2}) ==
          PairingWireStatus::Ready);
    CHECK(Decoder.TakeConfirmation());

    auto BadMagic = *Frame;
    BadMagic[0] ^= 0xFFu;
    CHECK(!DecodePairingOfferFrame(BadMagic));
    auto ExtraByte = *Frame;
    ExtraByte.push_back(0);
    CHECK(!DecodePairingOfferFrame(ExtraByte));
    auto ConfirmationWithBody = Confirmation;
    ConfirmationWithBody[7] = 1;
    ConfirmationWithBody.push_back(0);
    Decoder.Reset();
    CHECK(Decoder.Push(ConfirmationWithBody) == PairingWireStatus::InvalidFrame);
    auto InvalidUtf8 = Offer;
    InvalidUtf8.DisplayName = std::string{"\xC0\xAF", 2};
    CHECK(!EncodePairingOfferFrame(InvalidUtf8));
}

void AttemptRateLimiterIsBoundedAndExpires() {
    using namespace desklink;
    ManualClock Clock;
    AttemptRateLimiter Limiter(Clock, 2, std::chrono::seconds(1), 2);
    CHECK(Limiter.Allow("peer-a"));
    CHECK(Limiter.Allow("peer-a"));
    CHECK(!Limiter.Allow("peer-a"));
    CHECK(Limiter.Allow("peer-b"));
    CHECK(!Limiter.Allow("peer-c"));
    CHECK(Limiter.TrackedKeyCount() == 2);
    Clock.advance(std::chrono::milliseconds(1000));
    CHECK(Limiter.Allow("peer-c"));
    CHECK(Limiter.TrackedKeyCount() == 1);
    CHECK(!Limiter.Allow(""));
    CHECK(!Limiter.Allow(std::string(129, 'x')));
}

void CertificatePinsMatchOnlyTheStoredPeer() {
    using namespace desklink;
    DeterministicPairingCrypto crypto(7);
    const ByteBuffer certificate{1, 3, 3, 7, 9, 11};
    const auto digest = crypto.HashSha256(certificate);
    CHECK(digest.has_value());

    auto identity = MakeIdentity(60, "Pinned peer");
    identity.public_key_fingerprint = FormatFingerprint(*digest);
    InMemoryTrustStore trust;
    SaveTrustedPeer(trust, identity);

    const auto matched = MatchPeerCertificate(
        trust, crypto, certificate, &identity.machine_id);
    CHECK(matched.has_value());
    CHECK(matched->Identity == identity);

    auto tampered = certificate;
    tampered[0] ^= 0xFFu;
    CHECK(!MatchPeerCertificate(trust, crypto, tampered, &identity.machine_id));

    auto duplicate_identity = MakeIdentity(61, "Duplicate pin");
    duplicate_identity.public_key_fingerprint = identity.public_key_fingerprint;
    CHECK(!trust.SavePeer(TrustedPeer{duplicate_identity, CapabilitySet{}}));
}

void ForegroundProfilePolicyIsBoundedAndDeterministic() {
    using namespace desklink;

    ForegroundProfileEngine Engine;
    CHECK(Engine.SetRules({
        ForegroundProfileRule{"game.exe", DeskMode::Game, true},
        ForegroundProfileRule{"editor.exe", DeskMode::LockPc1, false},
    }));

    auto Decision = Engine.Decision();
    CHECK(Decision.Mode == DeskMode::LockPc1);
    CHECK(Decision.Source == ProfileModeSource::ForegroundUnavailable);

    Engine.SetForeground(ForegroundWindowSnapshot{
        10, "browser.exe", false, true});
    Decision = Engine.Decision();
    CHECK(Decision.Mode == DeskMode::Roam);
    CHECK(Decision.Source == ProfileModeSource::SystemDefault);

    Engine.SetForeground(ForegroundWindowSnapshot{
        11, "GAME.EXE", false, true});
    CHECK(Engine.Decision().Mode == DeskMode::Roam);

    Engine.SetForeground(ForegroundWindowSnapshot{
        11, "GAME.EXE", true, true});
    Decision = Engine.Decision();
    CHECK(Decision.Mode == DeskMode::Game);
    CHECK(Decision.Source == ProfileModeSource::ProfileRule);
    CHECK(Decision.RuleIndex == 0);

    CHECK(Engine.SetManualOverride(DeskMode::Roam));
    Decision = Engine.Decision();
    CHECK(Decision.Mode == DeskMode::Roam);
    CHECK(Decision.Source == ProfileModeSource::ManualOverride);

    Engine.EmergencyFailLocal();
    Decision = Engine.Decision();
    CHECK(Decision.Mode == DeskMode::LockPc1);
    CHECK(Decision.Source == ProfileModeSource::Emergency);
    Engine.ClearEmergency();
    CHECK(Engine.Decision().Source == ProfileModeSource::ManualOverride);
    Engine.ClearManualOverride();
    CHECK(Engine.Decision().Mode == DeskMode::Game);

    Engine.SetForeground(ForegroundWindowSnapshot{
        12, "", true, true});
    CHECK(Engine.Decision().Source ==
          ProfileModeSource::ForegroundUnavailable);

    CHECK(!Engine.SetRules({
        ForegroundProfileRule{"bad/path.exe", DeskMode::Game, false}}));
    CHECK(!Engine.SetRules({
        ForegroundProfileRule{"duplicate.exe", DeskMode::Game, false},
        ForegroundProfileRule{"DUPLICATE.EXE", DeskMode::Roam, false}}));

    std::vector<ForegroundProfileRule> TooManyRules;
    for (std::size_t Index = 0;
         Index <= kMaximumForegroundProfileRules; ++Index) {
        TooManyRules.push_back(ForegroundProfileRule{
            "game" + std::to_string(Index) + ".exe",
            DeskMode::Game, false});
    }
    CHECK(!Engine.SetRules(std::move(TooManyRules)));
}

class RecordingHostInputBackend final
    : public desklink::IHostInputLifecycleBackend {
public:
    void DisableCapture() noexcept override {
        Calls.push_back("disable-capture");
    }

    void StopCapture() noexcept override {
        Calls.push_back("stop-capture");
    }

    bool ReleaseFocus() noexcept override {
        Calls.push_back("release-focus");
        return ReleaseFocusResult;
    }

    bool SetDesiredMode(desklink::DeskMode Mode) noexcept override {
        Calls.push_back("set-mode-" +
                        std::to_string(static_cast<unsigned>(Mode)));
        return SetModeResult;
    }

    bool RequestFocus() noexcept override {
        Calls.push_back("request-focus");
        return RequestFocusResult;
    }

    bool SendInputStateSnapshot() noexcept override {
        Calls.push_back("send-snapshot");
        return SnapshotResult;
    }

    bool StartCapture() noexcept override {
        Calls.push_back("start-capture");
        return StartCaptureResult;
    }

    void EnableCapture() noexcept override {
        Calls.push_back("enable-capture");
    }

    std::vector<std::string> Calls;
    bool ReleaseFocusResult{true};
    bool SetModeResult{true};
    bool RequestFocusResult{true};
    bool SnapshotResult{true};
    bool StartCaptureResult{true};
};

void HostInputLifecycleRecreatesCaptureOnlyAfterFreshFocus() {
    using namespace desklink;

    RecordingHostInputBackend Backend;
    HostInputLifecycle Lifecycle(Backend, true);
    CHECK(Lifecycle.Start());
    CHECK((Backend.Calls == std::vector<std::string>{
        "set-mode-0", "request-focus"}));
    CHECK(Lifecycle.Status().State ==
          HostInputLifecycleState::AwaitingFocus);
    CHECK(!Lifecycle.Status().CaptureInstalled);

    Backend.Calls.clear();
    CHECK(Lifecycle.FocusReady());
    CHECK((Backend.Calls == std::vector<std::string>{
        "send-snapshot", "start-capture", "enable-capture"}));
    CHECK(Lifecycle.Status().State == HostInputLifecycleState::Remote);
    CHECK(Lifecycle.Status().CaptureInstalled);

    Backend.Calls.clear();
    CHECK(Lifecycle.ApplyMode(DeskMode::Game));
    CHECK((Backend.Calls == std::vector<std::string>{
        "disable-capture", "release-focus", "stop-capture", "set-mode-3"}));
    CHECK(Lifecycle.Status().State == HostInputLifecycleState::Local);
    CHECK(!Lifecycle.Status().CaptureInstalled);

    Backend.Calls.clear();
    CHECK(!Lifecycle.FocusReady());
    CHECK(Backend.Calls.empty());

    CHECK(Lifecycle.ApplyMode(DeskMode::Roam));
    CHECK((Backend.Calls == std::vector<std::string>{
        "set-mode-0", "request-focus"}));
    CHECK(!Lifecycle.Status().CaptureInstalled);

    Backend.Calls.clear();
    CHECK(Lifecycle.FocusReady());
    CHECK((Backend.Calls == std::vector<std::string>{
        "send-snapshot", "start-capture", "enable-capture"}));
    CHECK(Lifecycle.Status().CaptureInstalled);
}

void HostInputLifecycleFailuresRemainLocal() {
    using namespace desklink;

    RecordingHostInputBackend Backend;
    HostInputLifecycle Lifecycle(Backend, true);
    CHECK(Lifecycle.Start());
    Backend.Calls.clear();
    Backend.SnapshotResult = false;
    CHECK(!Lifecycle.FocusReady());
    CHECK((Backend.Calls == std::vector<std::string>{
        "send-snapshot", "disable-capture", "release-focus",
        "stop-capture", "set-mode-1"}));
    CHECK(Lifecycle.Status().Mode == DeskMode::LockPc1);
    CHECK(Lifecycle.Status().State == HostInputLifecycleState::Local);
    CHECK(!Lifecycle.Status().CaptureInstalled);

    RecordingHostInputBackend CaptureFailureBackend;
    HostInputLifecycle CaptureFailure(CaptureFailureBackend, true);
    CHECK(CaptureFailure.Start());
    CaptureFailureBackend.Calls.clear();
    CaptureFailureBackend.StartCaptureResult = false;
    CHECK(!CaptureFailure.FocusReady());
    CHECK((CaptureFailureBackend.Calls == std::vector<std::string>{
        "send-snapshot", "start-capture", "disable-capture",
        "release-focus", "stop-capture", "set-mode-1"}));
    CHECK(CaptureFailure.Status().Mode == DeskMode::LockPc1);
    CHECK(!CaptureFailure.Status().CaptureInstalled);

    RecordingHostInputBackend NoCaptureBackend;
    HostInputLifecycle NoCapture(NoCaptureBackend, false);
    CHECK(NoCapture.Start(DeskMode::LockPc2));
    NoCaptureBackend.Calls.clear();
    CHECK(NoCapture.FocusReady());
    CHECK((NoCaptureBackend.Calls == std::vector<std::string>{
        "send-snapshot"}));
    CHECK(!NoCapture.Status().CaptureInstalled);

    NoCaptureBackend.Calls.clear();
    CHECK(NoCapture.ApplyMode(DeskMode::Roam));
    CHECK((NoCaptureBackend.Calls == std::vector<std::string>{"set-mode-0"}));
    CHECK(!NoCapture.ApplyMode(static_cast<DeskMode>(0xffu)));
}

#ifdef _WIN32
void WindowsForegroundMonitorPublishesBoundedSnapshot() {
    using namespace desklink;

    std::mutex Mutex;
    std::condition_variable Changed;
    std::size_t CallbackCount{};
    bool Failed{};
    Win32ForegroundMonitor Monitor({
        [&](ForegroundWindowSnapshot Snapshot) {
            CHECK(IsValidForegroundWindowSnapshot(Snapshot));
            {
                std::scoped_lock Lock(Mutex);
                ++CallbackCount;
            }
            Changed.notify_all();
        },
        [&](std::string) {
            {
                std::scoped_lock Lock(Mutex);
                Failed = true;
            }
            Changed.notify_all();
        }});
    CHECK(Monitor.Start());
    {
        std::unique_lock Lock(Mutex);
        CHECK(Changed.wait_for(Lock, std::chrono::seconds(2), [&] {
            return CallbackCount != 0 || Failed;
        }));
        CHECK(!Failed);
        CHECK(CallbackCount != 0);
    }
    Monitor.Stop();
    CHECK(Monitor.Start());
    Monitor.Stop();
}

void WindowsCurrentUserControlPipeRoundTrip() {
    using namespace desklink;

    const auto Instance = L"test-" + std::to_wstring(GetCurrentProcessId());
    CHECK(!GetWin32ControlPipeName(L"invalid.instance").has_value());

    ControlState State;
    State.LocalMachine[0] = 0x31;
    State.Role = ControlRole::Agent;
    State.DesiredMode = DeskMode::Roam;
    State.ConnectedPeerCount = 2;
    Win32ControlPipeServer Server(
        [State](const ControlRequest& Request) {
            if (std::holds_alternative<GetStateControlRequest>(Request.Payload)) {
                return ControlResponse{
                    Request.RequestId, ControlStatus::Ok, State};
            }
            return ControlResponse{
                Request.RequestId, ControlStatus::Unsupported, std::nullopt};
        }, Instance);
    CHECK(Server.Start());
    CHECK(Server.Running());
    CHECK(!Server.PipeName().empty());
    Win32ControlPipeServer Duplicate(
        [](const ControlRequest& Request) {
            return ControlResponse{
                Request.RequestId, ControlStatus::Ok, std::nullopt};
        }, Instance);
    CHECK(!Duplicate.Start());

    const auto Response = Win32ControlPipeClient::Send(
        ControlRequest{101, GetStateControlRequest{}}, Instance);
    CHECK(Response.has_value());
    CHECK(Response->Status == ControlStatus::Ok);
    CHECK(Response->State.has_value());
    CHECK(Response->State->LocalMachine == State.LocalMachine);
    CHECK(Response->State->ConnectedPeerCount == 2);

    const auto Unsupported = Win32ControlPipeClient::Send(
        ControlRequest{102, ToggleAudioMuteControlRequest{}}, Instance);
    CHECK(Unsupported.has_value());
    CHECK(Unsupported->Status == ControlStatus::Unsupported);
    CHECK(!Unsupported->State.has_value());

    Server.Stop();
    CHECK(!Server.Running());
    CHECK(!Win32ControlPipeClient::Send(
        ControlRequest{103, GetStateControlRequest{}}, Instance,
        std::chrono::milliseconds{25}).has_value());
}

void WindowsDisplayTopologyEnumeratesWhenAvailable() {
    using namespace desklink;

    const auto Displays = EnumerateWin32Displays();
    if (!Displays) {
        std::cout << "[Display:Topology] active Windows display topology unavailable; "
                     "portable mapping tests still passed.\n";
        return;
    }

    DisplayTopologyMap Topology;
    CHECK(Topology.Update(*Displays) == DisplayTopologyUpdate::Changed);
    CHECK(!Topology.Current().Displays.empty());
    CHECK(Topology.Current().VirtualBounds.IsValid());
    for (const auto& Display : Topology.Current().Displays) {
        CHECK(Display.Id != kLegacyVirtualDesktopDisplayId);
        CHECK(!Display.StableIdentity.empty());
        CHECK(Display.Bounds.IsValid());
    }
    std::cout << "[Display:Topology] enumerated "
              << Topology.Current().Displays.size() << " active Windows display(s).\n";
}

void WindowsSuppressionGateFailsLocal() {
    using namespace desklink;
    constexpr std::uint32_t LeftControl = 0xA2u;
    constexpr std::uint32_t LeftAlt = 0xA4u;
    constexpr std::uint32_t Pause = 0x13u;
    constexpr std::uint32_t Cancel = 0x03u;

    Win32SuppressionGate Gate;
    CHECK(Gate.HandleKeyboard(LeftControl, true, false) ==
          Win32HookDecision::Pass);
    Gate.SetRemoteRouting(true);
    CHECK(Gate.HandleMouse(false) == Win32HookDecision::Suppress);
    CHECK(Gate.HandleMouse(true) == Win32HookDecision::Pass);
    CHECK(Gate.HandleKeyboard(0x20u, true, true) == Win32HookDecision::Pass);
    CHECK(Gate.HandleKeyboard(LeftControl, true, false) ==
          Win32HookDecision::Suppress);
    CHECK(Gate.HandleKeyboard(LeftAlt, true, false) ==
          Win32HookDecision::Suppress);
    CHECK(Gate.HandleKeyboard(Pause, true, false) ==
          Win32HookDecision::Emergency);
    CHECK(!Gate.RemoteRouting());
    CHECK(Gate.HandleMouse(false) == Win32HookDecision::Pass);

    Win32SuppressionGate BreakGate;
    BreakGate.SetRemoteRouting(true);
    CHECK(BreakGate.HandleKeyboard(LeftControl, true, false) ==
          Win32HookDecision::Suppress);
    CHECK(BreakGate.HandleKeyboard(LeftAlt, true, false) ==
          Win32HookDecision::Suppress);
    CHECK(BreakGate.HandleKeyboard(Cancel, true, false) ==
          Win32HookDecision::Emergency);
    CHECK(!BreakGate.RemoteRouting());
}

void WindowsCaptureSmokeIfRequested() {
    if (std::getenv("DESKLINK_CAPTURE_SMOKE") == nullptr) return;
    desklink::Win32InputCapture Capture({});
    CHECK(Capture.Start());
    CHECK(!Capture.RemoteRouting());
    Capture.Stop();
    CHECK(Capture.Start());
    CHECK(!Capture.RemoteRouting());
    Capture.Stop();
}

void WindowsCryptoAndDpapiTrustStoreWork() {
    using namespace desklink;
    BCryptPairingCrypto crypto;
    const std::string abc = "abc";
    const auto digest = crypto.HashSha256(ByteSpan{
        reinterpret_cast<const std::uint8_t*>(abc.data()), abc.size()});
    CHECK(digest.has_value());
    CHECK(FormatFingerprint(*digest) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    PairingNonce random{};
    CHECK(crypto.FillRandom(random));
    CHECK(random != PairingNonce{});

    const auto path = std::filesystem::temp_directory_path() /
        ("desklink-trust-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".bin");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    DpapiTrustStore first(path);
    CHECK(first.Load());
    CapabilitySet capabilities;
    capabilities.grant(Capability::InputInject);
    const auto identity = MakeIdentity(42, "DPAPI peer");
    CHECK(first.SavePeer(TrustedPeer{identity, capabilities}));

    DpapiTrustStore second(path);
    CHECK(second.Load());
    const auto restored = second.GetPeer(identity.machine_id);
    CHECK(restored.has_value());
    CHECK(restored->Identity == identity);
    CHECK(restored->Capabilities.contains(Capability::InputInject));
    CHECK(second.RemovePeer(identity.machine_id));
    std::filesystem::remove(path, ignored);

    const auto KeyName = std::wstring(L"DeskLink-Test-") + std::to_wstring(
        std::chrono::steady_clock::now().time_since_epoch().count());
    CHECK(Win32DeviceCertificate::Remove(KeyName));
    {
        auto FirstCertificate = Win32DeviceCertificate::LoadOrCreate(KeyName, crypto);
        CHECK(FirstCertificate.has_value());
        CHECK(!FirstCertificate->Der().empty());
        CHECK(FirstCertificate->CertificatePin() != Sha256Digest{});
        const auto Before = FirstCertificate->IdentitySnapshot(crypto);
        CHECK(Before.has_value());
        CHECK(Before->KeyName == KeyName);
        CHECK(Before->Provider == MS_KEY_STORAGE_PROVIDER);
        CHECK(Before->Algorithm == NCRYPT_RSA_ALGORITHM);
        CHECK(Before->ExportPolicy == 0);
        CHECK(!Before->PublicKeyDer.empty());
        CHECK(Before->CertificateDerHash == FirstCertificate->CertificatePin());
        CHECK(Before->DeskLinkIdentityPin == FirstCertificate->CertificatePin());
        auto ReloadedCertificate = Win32DeviceCertificate::Load(KeyName, crypto);
        CHECK(ReloadedCertificate.has_value());
        CHECK(ReloadedCertificate->CertificatePin() == FirstCertificate->CertificatePin());
        const auto After = ReloadedCertificate->IdentitySnapshot(crypto);
        CHECK(After.has_value());
        CHECK(*After == *Before);
    }
    CHECK(Win32DeviceCertificate::Remove(KeyName));
    CHECK(!Win32DeviceCertificate::Load(KeyName, crypto));
    CHECK(!Win32DeviceCertificate::LoadOrCreate(L"invalid key name!", crypto));
}
#endif

void DiscoveryPropertiesAreStrictAndRoundTrip() {
    using namespace desklink;
    const auto Advertisement = MakeDiscoveryAdvertisement();
    const auto Properties = EncodeDiscoveryProperties(Advertisement);
    CHECK(Properties.has_value());
    CHECK(Properties->size() == 6);

    const auto Decoded = DecodeDiscoveryProperties(
        *Properties, "DeskLink test PC._desklink._udp.local.",
        "desklink-test.local.", Advertisement.Port, 7);
    CHECK(Decoded.has_value());
    CHECK(Decoded->Advertisement.Machine == Advertisement.Machine);
    CHECK(Decoded->Advertisement.DisplayName == Advertisement.DisplayName);
    CHECK(Decoded->Advertisement.CapabilityHints ==
          Advertisement.CapabilityHints);
    CHECK(Decoded->Advertisement.PairingAvailable);
    CHECK(Decoded->InstanceName ==
          "DeskLink test PC._desklink._udp.local");
    CHECK(Decoded->HostName == "desklink-test.local");

    auto WithUnknown = *Properties;
    WithUnknown.emplace_back("future-key", "future-value");
    CHECK(DecodeDiscoveryProperties(
              WithUnknown, "DeskLink test PC._desklink._udp.local",
              "desklink-test.local", Advertisement.Port, 7)
              .has_value());

    auto Duplicate = *Properties;
    Duplicate.emplace_back("name", "spoofed");
    CHECK(!DecodeDiscoveryProperties(
               Duplicate, "DeskLink test PC._desklink._udp.local",
               "desklink-test.local", Advertisement.Port, 7)
               .has_value());

    auto WrongVersion = *Properties;
    WrongVersion[1].second = "2";
    CHECK(!DecodeDiscoveryProperties(
               WrongVersion, "DeskLink test PC._desklink._udp.local",
               "desklink-test.local", Advertisement.Port, 7)
               .has_value());

    auto InvalidMachine = *Properties;
    InvalidMachine[2].second = std::string(32, '0');
    CHECK(!DecodeDiscoveryProperties(
               InvalidMachine, "DeskLink test PC._desklink._udp.local",
               "desklink-test.local", Advertisement.Port, 7)
               .has_value());

    auto NonCanonicalMachine = *Properties;
    NonCanonicalMachine[2].second[0] = 'A';
    CHECK(!DecodeDiscoveryProperties(
               NonCanonicalMachine,
               "DeskLink test PC._desklink._udp.local",
               "desklink-test.local", Advertisement.Port, 7)
               .has_value());

    auto NonCanonicalCapabilities = *Properties;
    NonCanonicalCapabilities[4].second[15] = 'A';
    CHECK(!DecodeDiscoveryProperties(
               NonCanonicalCapabilities,
               "DeskLink test PC._desklink._udp.local",
               "desklink-test.local", Advertisement.Port, 7)
               .has_value());

    auto InvalidName = *Properties;
    InvalidName[3].second = "bad\nname";
    CHECK(!DecodeDiscoveryProperties(
               InvalidName, "DeskLink test PC._desklink._udp.local",
               "desklink-test.local", Advertisement.Port, 7)
               .has_value());

    auto Oversized = *Properties;
    Oversized.emplace_back("extra", std::string(256, 'x'));
    CHECK(!DecodeDiscoveryProperties(
               Oversized, "DeskLink test PC._desklink._udp.local",
               "desklink-test.local", Advertisement.Port, 7)
               .has_value());

    CHECK(!DecodeDiscoveryProperties(
               *Properties, "not-desklink._other._udp.local",
               "desklink-test.local", Advertisement.Port, 7)
               .has_value());
    CHECK(!DecodeDiscoveryProperties(
               *Properties, "DeskLink test PC._desklink._udp.local",
               "desklink-test.example", Advertisement.Port, 7)
               .has_value());
    CHECK(!DecodeDiscoveryProperties(
               *Properties, "DeskLink test PC._desklink._udp.local",
               "desklink-test.local", 0, 7)
               .has_value());
    CHECK(!DecodeDiscoveryProperties(
               *Properties, "DeskLink test PC._desklink._udp.local",
               "desklink-test.local", Advertisement.Port, 0)
               .has_value());
}

void DiscoveryCacheExpiresAndFlagsConflicts() {
    using namespace desklink;
    ManualClock Clock;
    DiscoveryCache Cache(Clock);

    auto First = MakeDiscoveryEndpoint(1, "z-host.local", 9);
    auto Second = MakeDiscoveryEndpoint(1, "a-host.local", 3);
    CHECK(Cache.Observe(First, std::chrono::seconds(30)));
    CHECK(Cache.Observe(Second, std::chrono::seconds(30)));
    auto Snapshot = Cache.Snapshot();
    CHECK(Snapshot.size() == 1);
    CHECK(Snapshot[0].EndpointCount == 2);
    CHECK(!Snapshot[0].Ambiguous);
    CHECK(Snapshot[0].Endpoint.HostName == "a-host.local");

    auto Conflict = MakeDiscoveryEndpoint(1, "b-host.local", 4);
    Conflict.Advertisement.DisplayName = "Conflicting name";
    CHECK(Cache.Observe(Conflict, std::chrono::seconds(30)));
    Snapshot = Cache.Snapshot();
    CHECK(Snapshot.size() == 1);
    CHECK(Snapshot[0].EndpointCount == 3);
    CHECK(Snapshot[0].Ambiguous);

    Cache.Remove(Conflict.InstanceName + ".", Conflict.InterfaceIndex);
    Snapshot = Cache.Snapshot();
    CHECK(Snapshot.size() == 1);
    CHECK(Snapshot[0].EndpointCount == 2);
    CHECK(!Snapshot[0].Ambiguous);

    Clock.advance(std::chrono::seconds(31));
    CHECK(Cache.Snapshot().empty());
    CHECK(!Cache.Observe(MakeDiscoveryEndpoint(),
                         std::chrono::milliseconds(0)));
}

void in_memory_transport_preserves_security_metadata() {
    using namespace desklink;
    TransportPeerInfo a_sees_b;
    a_sees_b.authenticated = true;
    a_sees_b.encrypted = true;
    a_sees_b.identity.display_name = "PC2";
    TransportPeerInfo b_sees_a;
    b_sees_a.authenticated = true;
    b_sees_a.encrypted = true;
    b_sees_a.identity.display_name = "PC1";

    auto pair = make_in_memory_transport_pair(a_sees_b, b_sees_a);
    CHECK(pair.a->peer_info().authenticated);
    CHECK(pair.a->peer_info().encrypted);
    CHECK(pair.a->peer_info().identity.display_name == "PC2");

    bool received = false;
    pair.b->set_reliable_handler([&](ByteBuffer bytes) {
        received = bytes.size() == 3 && bytes[0] == 1;
    });
    CHECK(pair.a->send_reliable(ByteBuffer{1, 2, 3}));
    CHECK(received);
}

} // namespace

int main() {
    ForegroundProfilePolicyIsBoundedAndDeterministic();
    HostInputLifecycleRecreatesCaptureOnlyAfterFreshFocus();
    HostInputLifecycleFailuresRemainLocal();
    DiscoveryPropertiesAreStrictAndRoundTrip();
    DiscoveryCacheExpiresAndFlagsConflicts();
    protocol_round_trip();
    ControlProtocolRoundTripAndValidation();
    MouseWheelRoundTripAndValidation();
    DisplayTopologyMappingIsStableAndInvalidates();
    DisplayTopologyRejectsAmbiguousStableIds();
    InputStateSnapshotRoundTripAndValidation();
    InputStateTransitionsReleaseBeforePress();
    rejects_wrong_lane_and_oversize();
    capability_and_lease_gate_input();
    DesiredModeControlIsCapabilityGatedAndFailsLocal();
    stale_epoch_rejected_after_refocus();
    host_agent_focus_transaction();
    jitter_buffer_reorders_and_conceals();
    out_of_order_pointer_rejected();
    stale_focus_ready_cannot_win_new_transaction();
    secure_session_end_to_end();
    insecure_transport_refused();
    UnpairedTransportIsRefused();
    PinnedIdentityMismatchIsRefused();
    PairingRequiresMatchingUserVerification();
    PairingTranscriptDetectsPinTamperingAndExpiry();
    PairingWireIsBoundedAndFragmentSafe();
    AttemptRateLimiterIsBoundedAndExpires();
    CertificatePinsMatchOnlyTheStoredPeer();
#ifdef _WIN32
    WindowsForegroundMonitorPublishesBoundedSnapshot();
    WindowsCurrentUserControlPipeRoundTrip();
    WindowsDisplayTopologyEnumeratesWhenAvailable();
    WindowsSuppressionGateFailsLocal();
    WindowsCaptureSmokeIfRequested();
    WindowsCryptoAndDpapiTrustStoreWork();
#endif
    in_memory_transport_preserves_security_metadata();
    std::cout << "All DeskLink foundation tests passed.\n";
    return 0;
}
