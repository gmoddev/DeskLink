#include "desklink/agent.hpp"
#include "desklink/audio.hpp"
#include "desklink/capabilities.hpp"
#include "desklink/control.hpp"
#include "desklink/display_topology.hpp"
#include "desklink/discovery.hpp"
#include "desklink/host.hpp"
#include "desklink/host_input_lifecycle.hpp"
#include "desklink/input.hpp"
#include "desklink/monitor_configurator.hpp"
#include "desklink/pairing.hpp"
#include "desklink/pairing_wire.hpp"
#include "desklink/profile.hpp"
#include "desklink/product.hpp"
#include "desklink/protocol.hpp"
#include "desklink/roaming.hpp"
#include "desklink/roaming_runtime.hpp"
#include "desklink/runtime_broker.hpp"
#include "desklink/product_shell.hpp"
#include "desklink/session.hpp"
#include "desklink/topology_exchange.hpp"
#include "desklink/transport.hpp"
#include "desklink/types.hpp"
#include "desklink/update.hpp"
#ifdef DESKLINK_BUILD_VOICE
#include "desklink/voice.hpp"
#endif
#ifdef _WIN32
#include "desklink/win32_audio.hpp"
#ifdef DESKLINK_BUILD_VOICE
#include "desklink/win32_voice.hpp"
#endif
#include "desklink/win32_application_settings.hpp"
#include "desklink/win32_capture.hpp"
#include "desklink/win32_clipboard.hpp"
#include "desklink/win32_control.hpp"
#include "desklink/win32_device_certificate.hpp"
#include "desklink/win32_discovery.hpp"
#include "desklink/win32_display_topology.hpp"
#include "desklink/win32_foreground.hpp"
#include "desklink/win32_launcher.hpp"
#include "desklink/win32_pairing.hpp"
#include "desklink/win32_roaming_settings.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
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
    bool ReadyForInput() const noexcept override { return Ready; }
    bool inject_key(const desklink::KeyEventMessage& event) override {
        keys.push_back(event); return InjectSucceeds;
    }
    bool inject_button(const desklink::MouseButtonMessage& event) override {
        buttons.push_back(event); return true;
    }
    bool inject_pointer(const desklink::PointerPositionMessage& event) override {
        pointers.push_back(event); return InjectSucceeds;
    }
    bool InjectPointerMotion(
        const desklink::PointerMotionMessage& Message) override {
        motions.push_back(Message); return InjectSucceeds;
    }
    bool InjectWheel(const desklink::MouseWheelMessage& Message) override {
        wheels.push_back(Message); return InjectSucceeds;
    }
    bool ReconcileState(const desklink::InputStateSnapshotMessage& Snapshot) override {
        snapshots.push_back(Snapshot); return ReconcileSucceeds;
    }
    std::optional<desklink::PointerPositionMessage>
    CurrentPointerPosition() override {
        return CurrentPointer;
    }
    bool release_owned_state() noexcept override {
        ++release_calls;
        return ReleaseSucceeds;
    }
    bool ParkPointer() noexcept override {
        ++park_calls;
        return ParkSucceeds;
    }

    std::vector<desklink::KeyEventMessage> keys;
    std::vector<desklink::MouseButtonMessage> buttons;
    std::vector<desklink::PointerPositionMessage> pointers;
    std::vector<desklink::PointerMotionMessage> motions;
    std::vector<desklink::MouseWheelMessage> wheels;
    std::vector<desklink::InputStateSnapshotMessage> snapshots;
    std::optional<desklink::PointerPositionMessage> CurrentPointer;
    bool ReconcileSucceeds{true};
    bool Ready{true};
    bool InjectSucceeds{true};
    bool ReleaseSucceeds{true};
    bool ParkSucceeds{true};
    int release_calls{};
    int park_calls{};
};

class PausableTransportEndpoint final
    : public desklink::ITransportEndpoint,
      public std::enable_shared_from_this<PausableTransportEndpoint> {
public:
    explicit PausableTransportEndpoint(desklink::TransportPeerInfo Peer)
        : Peer_(std::move(Peer)) {}

    void Connect(
        const std::shared_ptr<PausableTransportEndpoint>& PeerEndpoint) {
        PeerEndpoint_ = PeerEndpoint;
    }

    bool send_reliable(desklink::ByteBuffer Packet) override {
        {
            std::scoped_lock Lock(Mutex_);
            if (Closed_) return false;
            if (DropNextReliableType_ &&
                desklink::PeekMessageType(Packet) ==
                    DropNextReliableType_) {
                DropNextReliableType_.reset();
                return true;
            }
            if (ReliablePaused_) {
                ReliableQueue_.push_back(std::move(Packet));
                return true;
            }
        }
        return Deliver(std::move(Packet), false);
    }

    bool send_datagram(desklink::ByteBuffer Packet) override {
        return Deliver(std::move(Packet), true);
    }

    void set_reliable_handler(ReceiveHandler Handler) override {
        std::scoped_lock Lock(Mutex_);
        ReliableHandler_ = std::move(Handler);
    }

    void set_datagram_handler(ReceiveHandler Handler) override {
        std::scoped_lock Lock(Mutex_);
        DatagramHandler_ = std::move(Handler);
    }

    void set_close_handler(CloseHandler Handler) override {
        std::scoped_lock Lock(Mutex_);
        CloseHandler_ = std::move(Handler);
    }

    [[nodiscard]] desklink::TransportPeerInfo peer_info() const override {
        return Peer_;
    }

    void close() noexcept override {
        std::shared_ptr<PausableTransportEndpoint> PeerEndpoint;
        {
            std::scoped_lock Lock(Mutex_);
            if (Closed_) return;
            Closed_ = true;
            ReliableHandler_ = {};
            DatagramHandler_ = {};
            CloseHandler_ = {};
            ReliableQueue_.clear();
            PeerEndpoint = PeerEndpoint_.lock();
        }
        CloseHandler PeerHandler;
        if (PeerEndpoint) {
            std::scoped_lock Lock(PeerEndpoint->Mutex_);
            PeerHandler = PeerEndpoint->CloseHandler_;
        }
        if (PeerHandler) {
            PeerHandler(desklink::TransportCloseReason::Unavailable);
        }
    }

    void PauseReliable(bool Paused) noexcept {
        std::scoped_lock Lock(Mutex_);
        ReliablePaused_ = Paused;
    }

    void DropNextReliable(desklink::MessageType Type) noexcept {
        std::scoped_lock Lock(Mutex_);
        DropNextReliableType_ = Type;
    }

    [[nodiscard]] bool FlushOneReliable() {
        desklink::ByteBuffer Packet;
        {
            std::scoped_lock Lock(Mutex_);
            if (ReliableQueue_.empty()) return false;
            Packet = std::move(ReliableQueue_.front());
            ReliableQueue_.pop_front();
        }
        return Deliver(std::move(Packet), false);
    }

private:
    [[nodiscard]] bool Deliver(
        desklink::ByteBuffer Packet, bool Datagram) {
        const auto PeerEndpoint = PeerEndpoint_.lock();
        if (!PeerEndpoint) return false;
        ReceiveHandler Handler;
        {
            std::scoped_lock Lock(PeerEndpoint->Mutex_);
            if (PeerEndpoint->Closed_) return false;
            Handler = Datagram
                ? PeerEndpoint->DatagramHandler_
                : PeerEndpoint->ReliableHandler_;
        }
        if (!Handler) return false;
        Handler(std::move(Packet));
        return true;
    }

    desklink::TransportPeerInfo Peer_;
    std::weak_ptr<PausableTransportEndpoint> PeerEndpoint_;
    mutable std::mutex Mutex_;
    ReceiveHandler ReliableHandler_;
    ReceiveHandler DatagramHandler_;
    CloseHandler CloseHandler_;
    std::deque<desklink::ByteBuffer> ReliableQueue_;
    std::optional<desklink::MessageType> DropNextReliableType_;
    bool ReliablePaused_{};
    bool Closed_{};
};

struct PausableTransportPair {
    std::shared_ptr<PausableTransportEndpoint> A;
    std::shared_ptr<PausableTransportEndpoint> B;
};

PausableTransportPair MakePausableTransportPair(
    desklink::TransportPeerInfo AViewOfB,
    desklink::TransportPeerInfo BViewOfA) {
    auto A = std::make_shared<PausableTransportEndpoint>(
        std::move(AViewOfB));
    auto B = std::make_shared<PausableTransportEndpoint>(
        std::move(BViewOfA));
    A->Connect(B);
    B->Connect(A);
    return {std::move(A), std::move(B)};
}

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

desklink::RoamingConfiguration MakeRoamingConfiguration() {
    using namespace desklink;
    RoamingConfiguration Result;
    Result.CrossingDefaults = {
        CrossingPolicy::Push, 8, 120, 500};
    Result.Links.push_back(RoamingLink{
        {MakeMachineId(1), "display-a", DisplayEdgeSide::Right, 1'000, 9'000},
        {MakeMachineId(2), "display-b", DisplayEdgeSide::Left, 2'000, 8'000},
        RoamingDirectionMode::Bidirectional,
        {CrossingPolicy::DwellAndPush, 12, 250, 500},
        {CrossingPolicy::DoublePush, 9, 0, 600},
        12,
        24,
        24,
        true,
    });
    Result.CanvasLayout.push_back(
        {MakeMachineId(1), "display-a", -1'920, 75});
    Result.CanvasLayout.push_back(
        {MakeMachineId(2), "display-b", 0, -25});
    return Result;
}

desklink::DisplayTopologySnapshot MakeDisplayTopology(
    std::string StableIdentity,
    std::string FriendlyName,
    desklink::DisplayRect Bounds = {0, 0, 1920, 1080}) {
    desklink::DisplayTopologyMap Topology;
    CHECK(Topology.Update({desklink::DiscoveredDisplay{
        std::move(StableIdentity), std::move(FriendlyName), Bounds, true}}) ==
        desklink::DisplayTopologyUpdate::Changed);
    return Topology.Current();
}

desklink::DisplayTopologySnapshot MakePhysicalDisplayTopology(
    std::string StableIdentity,
    std::string FriendlyName,
    desklink::DisplayRect Bounds,
    desklink::PhysicalDisplaySize Physical,
    desklink::PhysicalSizeSource Source =
        desklink::PhysicalSizeSource::Edid,
    desklink::DisplayOrientation Orientation =
        desklink::DisplayOrientation::Landscape) {
    using namespace desklink;
    DiscoveredDisplay Display{
        std::move(StableIdentity), std::move(FriendlyName), Bounds, true};
    Display.PixelWidth = static_cast<std::uint32_t>(
        static_cast<std::int64_t>(Bounds.Right) - Bounds.Left);
    Display.PixelHeight = static_cast<std::uint32_t>(
        static_cast<std::int64_t>(Bounds.Bottom) - Bounds.Top);
    Display.RefreshMilliHertz = 60'000;
    Display.PhysicalWidthMillimeters = Physical.WidthMillimeters;
    Display.PhysicalHeightMillimeters = Physical.HeightMillimeters;
    Display.PhysicalSize = Source;
    Display.Orientation = Orientation;
    DisplayTopologyMap Topology;
    CHECK(Topology.Update({std::move(Display)}) ==
          DisplayTopologyUpdate::Changed);
    return Topology.Current();
}

struct RecordedPointerSample {
    std::uint16_t DelayMilliseconds{};
    desklink::LocalPointerObservation Observation;
};

std::vector<desklink::RoamingFocusRequest> ReplayRoamingTrace(
    desklink::RoamingRuntime& Runtime,
    ManualClock& Clock,
    std::span<const RecordedPointerSample> Trace) {
    std::vector<desklink::RoamingFocusRequest> Requests;
    for (const auto& Sample : Trace) {
        Clock.advance(std::chrono::milliseconds(Sample.DelayMilliseconds));
        const auto Request = Runtime.Observe(Sample.Observation);
        if (Request) Requests.push_back(*Request);
    }
    return Requests;
}

desklink::RoamingRuntimeContext MakeRoamingRuntimeContext(
    desklink::CrossingPolicy Policy = desklink::CrossingPolicy::Push) {
    using namespace desklink;
    auto Configuration = MakeRoamingConfiguration();
    Configuration.Links[0].Direction = RoamingDirectionMode::AToB;
    Configuration.Links[0].AToB = {Policy, 8, 100, 500};
    return RoamingRuntimeContext{
        MakeMachineId(1),
        MakeMachineId(2),
        std::move(Configuration),
        MakeDisplayTopology("display-a", "Local", {0, 0, 1920, 1080}),
        MakeDisplayTopology("display-b", "Peer", {0, 0, 2560, 1440}),
        PeerConnectionStatus::Connected,
        DisplayTopologyExchangeStatus::Ready,
        0x1234u,
        true,
        true,
        true,
    };
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
    Result.machine_id = desklink::DeriveMachineId(MakeDigest(Marker));
    Result.display_name = std::move(Name);
    Result.public_key_fingerprint = desklink::FormatFingerprint(MakeDigest(Marker));
    return Result;
}

void SaveTrustedPeer(desklink::InMemoryTrustStore& Store,
                      const desklink::PeerIdentity& Identity,
                      desklink::CapabilitySet Capabilities = {}) {
    const auto Fingerprint = desklink::ParseFingerprint(
        Identity.public_key_fingerprint);
    CHECK(Fingerprint.has_value());
    CHECK(Identity.machine_id == desklink::DeriveMachineId(*Fingerprint));
    CHECK(Store.SavePeer(desklink::TrustedPeer{Identity, Capabilities}));
}

class RecordingRuntimeSafetyController final
    : public desklink::IRuntimeSafetyController {
public:
    bool ReturnLocalForPeer(
        const desklink::MachineId& Machine) noexcept override {
        ReturnLocalCalls.push_back(Machine);
        return Succeeds;
    }

    bool RefreshPeerCapabilities(
        const desklink::MachineId& Machine) noexcept override {
        RefreshCalls.push_back(Machine);
        return Succeeds && RefreshSucceeds;
    }

    bool ReturnLocalAndStopPeer(
        const desklink::MachineId& Machine) noexcept override {
        StopCalls.push_back(Machine);
        return Succeeds;
    }

    bool Succeeds{true};
    bool RefreshSucceeds{true};
    std::vector<desklink::MachineId> ReturnLocalCalls;
    std::vector<desklink::MachineId> RefreshCalls;
    std::vector<desklink::MachineId> StopCalls;
};

void CallbackGateClosesAndDrainsAdmittedCallbacks() {
    using namespace desklink;
    auto Gate = std::make_shared<CallbackGate>();
    std::mutex Mutex;
    std::condition_variable Condition;
    bool Entered = false;
    bool Release = false;
    std::atomic_bool Drained{};
    std::atomic_bool DrainStarted{};

    std::thread Callback([&] {
        auto Guard = Gate->TryEnter();
        CHECK(static_cast<bool>(Guard));
        std::unique_lock Lock(Mutex);
        Entered = true;
        Condition.notify_all();
        Condition.wait(Lock, [&] { return Release; });
    });
    {
        std::unique_lock Lock(Mutex);
        CHECK(Condition.wait_for(
            Lock, std::chrono::seconds(1), [&] { return Entered; }));
    }
    Gate->Close();
    CHECK(!Gate->TryEnter());
    std::thread Drainer([&] {
        DrainStarted = true;
        Gate->Wait();
        Drained = true;
    });
    while (!DrainStarted.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(!Drained.load());
    {
        std::scoped_lock Lock(Mutex);
        Release = true;
    }
    Condition.notify_all();
    Callback.join();
    Drainer.join();
    CHECK(Drained.load());

    auto ReentrantGate = std::make_shared<CallbackGate>();
    auto Guard = ReentrantGate->TryEnter();
    CHECK(static_cast<bool>(Guard));
    bool OtherEntered = false;
    bool ReleaseOther = false;
    bool ReentrantWaitStarted = false;
    std::mutex ReentrantMutex;
    std::condition_variable ReentrantCondition;
    std::thread OtherCallback([&] {
        auto OtherGuard = ReentrantGate->TryEnter();
        CHECK(static_cast<bool>(OtherGuard));
        std::unique_lock Lock(ReentrantMutex);
        OtherEntered = true;
        ReentrantCondition.notify_all();
        ReentrantCondition.wait(Lock, [&] { return ReleaseOther; });
    });
    {
        std::unique_lock Lock(ReentrantMutex);
        ReentrantCondition.wait(Lock, [&] { return OtherEntered; });
    }
    std::thread Releaser([&] {
        std::unique_lock Lock(ReentrantMutex);
        ReentrantCondition.wait(Lock, [&] { return ReentrantWaitStarted; });
        ReleaseOther = true;
        ReentrantCondition.notify_all();
    });
    ReentrantGate->Close();
    {
        std::scoped_lock Lock(ReentrantMutex);
        ReentrantWaitStarted = true;
    }
    ReentrantCondition.notify_all();
    ReentrantGate->Wait();
    OtherCallback.join();
    Releaser.join();
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

    const auto GrantBytes = encode_packet(
        h, CapabilityGrantMessage{0x55, 7});
    const auto GrantDecoded = decode_packet(GrantBytes, false);
    CHECK(GrantDecoded.packet.has_value());
    const auto& Grant = std::get<CapabilityGrantMessage>(
        GrantDecoded.packet->message);
    CHECK(Grant.capabilities == 0x55);
    CHECK(Grant.revision == 7);

    const auto AckBytes = encode_packet(
        h, CapabilityGrantAckMessage{0x55, 7});
    const auto AckDecoded = decode_packet(AckBytes, false);
    CHECK(AckDecoded.packet.has_value());
    const auto& Ack = std::get<CapabilityGrantAckMessage>(
        AckDecoded.packet->message);
    CHECK(Ack.capabilities == 0x55);
    CHECK(Ack.revision == 7);

    const auto FeedbackBytes = encode_packet(
        h, PointerPositionFeedbackMessage{3, 0, 32'768});
    const auto FeedbackDecoded = decode_packet(FeedbackBytes, true);
    CHECK(FeedbackDecoded.packet.has_value());
    const auto& Feedback = std::get<PointerPositionFeedbackMessage>(
        FeedbackDecoded.packet->message);
    CHECK(Feedback.DisplayId == 3);
    CHECK(Feedback.NormalizedX == 0);
    CHECK(Feedback.NormalizedY == 32'768);
    CHECK(!decode_packet(FeedbackBytes, false).packet.has_value());

    const auto ClockRequestBytes = encode_packet(
        h, ClockSyncRequestMessage{7, 1'000});
    const auto ClockRequestDecoded = decode_packet(
        ClockRequestBytes, false);
    CHECK(ClockRequestDecoded.packet.has_value());
    const auto& ClockRequest = std::get<ClockSyncRequestMessage>(
        ClockRequestDecoded.packet->message);
    CHECK(ClockRequest.ProbeId == 7);
    CHECK(ClockRequest.OriginSendTimestampUs == 1'000);
    CHECK(!decode_packet(ClockRequestBytes, true).packet.has_value());

    const auto ClockResponseBytes = encode_packet(
        h, ClockSyncResponseMessage{7, 1'000, 1'250, 1'260});
    const auto ClockResponseDecoded = decode_packet(
        ClockResponseBytes, false);
    CHECK(ClockResponseDecoded.packet.has_value());
    const auto& ClockResponse = std::get<ClockSyncResponseMessage>(
        ClockResponseDecoded.packet->message);
    CHECK(ClockResponse.RemoteReceiveTimestampUs == 1'250);
    CHECK(ClockResponse.RemoteSendTimestampUs == 1'260);

    const auto InvalidClockResponse = encode_packet(
        h, ClockSyncResponseMessage{7, 1'000, 1'260, 1'250});
    CHECK(!decode_packet(
        InvalidClockResponse, false).packet.has_value());
}

void PointerMotionRoundTripAndValidation() {
    using namespace desklink;
    EnvelopeHeader Header;
    Header.session_nonce = 44;
    Header.epoch = 9;
    Header.sequence = 101;
    const auto Bytes = encode_packet(Header, PointerMotionMessage{-321, 654});
    const auto Decoded = decode_packet(Bytes, true);
    CHECK(Decoded.packet.has_value());
    const auto& Motion = std::get<PointerMotionMessage>(Decoded.packet->message);
    CHECK(Motion.DeltaX == -321);
    CHECK(Motion.DeltaY == 654);
    CHECK(!decode_packet(Bytes, false).packet.has_value());

    const auto Zero = encode_packet(Header, PointerMotionMessage{});
    CHECK(!decode_packet(Zero, true).packet.has_value());
    const auto Oversized = encode_packet(
        Header, PointerMotionMessage{kMaximumPointerMotionDelta + 1, 0});
    CHECK(!decode_packet(Oversized, true).packet.has_value());
}

void ControlProtocolRoundTripAndValidation() {
    using namespace desklink;

    ProductPreferences Preferences;
    Preferences.Role = DeskRole::Main;
    Preferences.PreferredPeerMachine = MakeMachineId(8);
    Preferences.PreferredPeerEndpoint =
        ProductPeerEndpoint{"192.168.0.108", 43'821};
    Preferences.AutoStartRuntime = true;
    Preferences.AutoConnect = true;
    Preferences.InputRoamingDesired = true;
    Preferences.AudioRoute = AudioRoutePreference::PeerToLocal;
    Preferences.AudioGainPermyriad = 7'500;
    Preferences.FocusPeerHotkey = ProductHotkey::CtrlAltF11;
    Preferences.ReturnLocalHotkey = ProductHotkey::CtrlAltF12;
    Preferences.ProfileRules.push_back(
        ForegroundProfileRule{"game.exe", DeskMode::Game, true});
    Preferences.FirstRunComplete = true;
    CapabilitySet RequestedCapabilities;
    RequestedCapabilities.grant(Capability::InputInject);
    RequestedCapabilities.grant(Capability::DisplayTopologyExchange);
    ControlPairingToken PairingToken{};
    PairingToken[0] = 0x41;
    const std::array<ControlRequest, 33> Requests{
        ControlRequest{1, GetStateControlRequest{}},
        ControlRequest{2, SetDesiredModeControlRequest{DeskMode::LockPc1}},
        ControlRequest{3, FocusMachineControlRequest{
            MachineId{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}}},
        ControlRequest{4, SetAudioGainControlRequest{7'500}},
        ControlRequest{5, ToggleAudioMuteControlRequest{}},
        ControlRequest{6, GetDisplayTopologiesControlRequest{}},
        ControlRequest{7, PrepareForUpdateControlRequest{}},
        ControlRequest{8, GetProductPreferencesControlRequest{}},
        ControlRequest{9, SetProductPreferencesControlRequest{Preferences}},
        ControlRequest{10, ListTrustedDevicesControlRequest{}},
        ControlRequest{11, RequestLocalPermissionChangeControlRequest{
            MakeMachineId(8), RequestedCapabilities}},
        ControlRequest{12, ForgetTrustedDeviceControlRequest{MakeMachineId(8)}},
        ControlRequest{13, ReturnLocalControlRequest{}},
        ControlRequest{14, GetPairingCandidateControlRequest{}},
        ControlRequest{15, PauseDeskLinkControlRequest{}},
        ControlRequest{16, ResumeDeskLinkControlRequest{}},
        ControlRequest{17, StartDiscoveryControlRequest{5}},
        ControlRequest{18, GetNearbyPeersControlRequest{}},
        ControlRequest{19, StopDiscoveryControlRequest{}},
        ControlRequest{20, OpenPairingWindowControlRequest{
            43'821, RequestedCapabilities}},
        ControlRequest{21, PairNearbyPeerControlRequest{
            MakeMachineId(9), RequestedCapabilities}},
        ControlRequest{22, PairManualAddressControlRequest{
            "192.168.0.108", 43'821, RequestedCapabilities}},
        ControlRequest{23, ResolvePairingCandidateControlRequest{44, true}},
        ControlRequest{24, PresentManagedPairingCandidateControlRequest{
            PairingToken,
            44,
            DeriveMachineId(MakeDigest(9)),
            "Nearby PC",
            "654321",
            FormatFingerprint(MakeDigest(9)),
            MakeDigest(72),
            RequestedCapabilities}},
        ControlRequest{25, GetManagedPairingDecisionControlRequest{
            PairingToken, 44}},
        ControlRequest{26, GetPermissionCandidateControlRequest{}},
        ControlRequest{27, ResolvePermissionCandidateControlRequest{
            45, true}},
        ControlRequest{28, GetPairingOperationControlRequest{}},
        ControlRequest{29, RefreshTrustedPeerCapabilitiesControlRequest{
            MakeMachineId(8)}},
        ControlRequest{30, ApplyManagedPreferencesControlRequest{
            Preferences}},
        ControlRequest{31, IdentifyPeerDisplaysControlRequest{4}},
        ControlRequest{32, SetVoiceTransmitControlRequest{true}},
        ControlRequest{33, SetVoiceMutedControlRequest{true}},
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
    const auto DecodedPreferenceRequest = DecodeControlRequest(
        *EncodeControlRequest(Requests[8]));
    CHECK(DecodedPreferenceRequest.Decoded.has_value());
    CHECK(std::get<SetProductPreferencesControlRequest>(
              DecodedPreferenceRequest.Decoded->Payload).Preferences ==
          Preferences);
    const auto DecodedPermissionRequest = DecodeControlRequest(
        *EncodeControlRequest(Requests[10]));
    CHECK(DecodedPermissionRequest.Decoded.has_value());
    const auto& PermissionRequest =
        std::get<RequestLocalPermissionChangeControlRequest>(
            DecodedPermissionRequest.Decoded->Payload);
    CHECK(PermissionRequest.Machine == MakeMachineId(8));
    CHECK(PermissionRequest.DesiredCapabilities == RequestedCapabilities);
    const auto DecodedIdentifyRequest = DecodeControlRequest(
        *EncodeControlRequest(Requests[30]));
    CHECK(DecodedIdentifyRequest.Decoded.has_value());
    CHECK(std::get<IdentifyPeerDisplaysControlRequest>(
              DecodedIdentifyRequest.Decoded->Payload)
              .FirstDisplayNumber == 4);

    ControlState State;
    State.LocalMachine[0] = 0x11;
    State.FocusedMachine[0] = 0x22;
    State.Role = ControlRole::Host;
    State.DesiredMode = DeskMode::Roam;
    State.ConnectedPeerCount = 1;
    State.AudioGainPermyriad = 8'000;
    State.VoiceGainPermyriad = 6'500;
    State.VoiceEnabled = true;
    State.VoicePttReady = true;
    State.VoiceTransmitting = true;
    State.RuntimePhase = BrokerRuntimePhase::ConnectedLocal;
    State.RoamingState = ControlRoamingState::Remote;
    State.PeerDirection = ControlPeerDirectionState::OutgoingActive;
    State.ReadyRoamingRouteCount = 1;
    State.RemoteFocused = true;
    State.CaptureActive = true;
    State.RoamingObserverActive = true;
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
    CHECK(Response.Decoded->State->RuntimePhase ==
          BrokerRuntimePhase::ConnectedLocal);
    CHECK(Response.Decoded->State->RoamingState ==
          ControlRoamingState::Remote);
    CHECK(Response.Decoded->State->PeerDirection ==
          ControlPeerDirectionState::OutgoingActive);
    CHECK(Response.Decoded->State->ReadyRoamingRouteCount == 1);
    CHECK(Response.Decoded->State->RoamingObserverActive);
    CHECK(Response.Decoded->State->VoiceGainPermyriad == 6'500);
    CHECK(Response.Decoded->State->VoiceEnabled);
    CHECK(Response.Decoded->State->VoicePttReady);
    CHECK(Response.Decoded->State->VoiceTransmitting);

    ControlTopologyState TopologyState;
    TopologyState.Machines.push_back(ControlMachineTopology{
        MakeMachineId(1), DisplayTopologyExchangeStatus::Ready,
        MakeDisplayTopology("control-local", "Local display"), true, false});
    TopologyState.Machines.push_back(ControlMachineTopology{
        MakeMachineId(2), DisplayTopologyExchangeStatus::Ready,
        MakeDisplayTopology("control-peer", "Peer display"), false, true});
    const auto TopologyResponseFrame = EncodeControlResponse(
        ControlResponse{11, ControlStatus::Ok, std::nullopt, TopologyState});
    CHECK(TopologyResponseFrame.has_value());
    const auto TopologyResponse = DecodeControlResponse(*TopologyResponseFrame);
    CHECK(TopologyResponse.Decoded.has_value());
    CHECK(!TopologyResponse.Decoded->State.has_value());
    CHECK(TopologyResponse.Decoded->Topologies == TopologyState);
    auto MissingLocal = TopologyState;
    MissingLocal.Machines.erase(MissingLocal.Machines.begin());
    CHECK(!IsValidControlTopologyState(MissingLocal));
    auto ReadyWithoutSnapshot = TopologyState;
    ReadyWithoutSnapshot.Machines[1].Topology.reset();
    CHECK(!IsValidControlTopologyState(ReadyWithoutSnapshot));
    CHECK(!EncodeControlResponse(ControlResponse{
        12, ControlStatus::Ok, State, TopologyState}).has_value());

    ControlResponse PreferencesResponse{15, ControlStatus::Ok};
    PreferencesResponse.Preferences = Preferences;
    const auto PreferencesFrame = EncodeControlResponse(PreferencesResponse);
    CHECK(PreferencesFrame.has_value());
    const auto DecodedPreferences = DecodeControlResponse(*PreferencesFrame);
    CHECK(DecodedPreferences.Decoded.has_value());
    CHECK(DecodedPreferences.Decoded->Preferences == Preferences);

    ControlTrustedDeviceList Devices;
    Devices.Devices.push_back(ControlTrustedDevice{
        MakeMachineId(8), "Companion PC", RequestedCapabilities, true});
    ControlResponse DevicesResponse{16, ControlStatus::Ok};
    DevicesResponse.TrustedDevices = Devices;
    const auto DevicesFrame = EncodeControlResponse(DevicesResponse);
    CHECK(DevicesFrame.has_value());
    const auto DecodedDevices = DecodeControlResponse(*DevicesFrame);
    CHECK(DecodedDevices.Decoded.has_value());
    CHECK(DecodedDevices.Decoded->TrustedDevices == Devices);
    CHECK(DecodedDevices.Decoded->TrustedDevices->Devices.front().Connected);

    ControlPairingCandidate PairingCandidate{
        44, MakeMachineId(9), "Nearby PC", "654321",
        RequestedCapabilities, ControlPairingSource::Nearby};
    ControlResponse PairingResponse{17, ControlStatus::Ok};
    PairingResponse.PairingCandidate = PairingCandidate;
    const auto PairingFrame = EncodeControlResponse(PairingResponse);
    CHECK(PairingFrame.has_value());
    const auto DecodedPairing = DecodeControlResponse(*PairingFrame);
    CHECK(DecodedPairing.Decoded.has_value());
    CHECK(DecodedPairing.Decoded->PairingCandidate == PairingCandidate);
    PairingCandidate.VerificationCode = "not-six";
    CHECK(!IsValidControlPairingCandidate(PairingCandidate));

    ControlNearbyPeerList Nearby;
    Nearby.Phase = ControlDiscoveryPhase::Complete;
    Nearby.Peers.push_back(ControlNearbyPeer{
        MakeMachineId(10), "Unverified PC", "desklink-peer.local",
        RequestedCapabilities, 43'821, kProtocolVersion, 1, true, false});
    ControlResponse NearbyResponse{18, ControlStatus::Ok};
    NearbyResponse.NearbyPeers = Nearby;
    const auto NearbyFrame = EncodeControlResponse(NearbyResponse);
    CHECK(NearbyFrame.has_value());
    const auto DecodedNearby = DecodeControlResponse(*NearbyFrame);
    CHECK(DecodedNearby.Decoded.has_value());
    CHECK(DecodedNearby.Decoded->NearbyPeers == Nearby);
    auto UnsafeNearby = Nearby;
    UnsafeNearby.Peers.front().HostName.assign(
        kMaximumControlHostName + 1, 'a');
    CHECK(!IsValidControlNearbyPeerList(UnsafeNearby));
    ControlResponse DecisionResponse{19, ControlStatus::Ok};
    DecisionResponse.PairingDecision =
        ControlManagedPairingDecision::Rejected;
    const auto DecisionFrame = EncodeControlResponse(DecisionResponse);
    CHECK(DecisionFrame.has_value());
    const auto DecodedDecision = DecodeControlResponse(*DecisionFrame);
    CHECK(DecodedDecision.Decoded.has_value());
    CHECK(DecodedDecision.Decoded->PairingDecision ==
          ControlManagedPairingDecision::Rejected);

    const ControlPairingOperation PairingOperation{
        44, ControlPairingPhase::Succeeded};
    ControlResponse OperationResponse{21, ControlStatus::Ok};
    OperationResponse.PairingOperation = PairingOperation;
    const auto OperationFrame = EncodeControlResponse(OperationResponse);
    CHECK(OperationFrame.has_value());
    const auto DecodedOperation = DecodeControlResponse(*OperationFrame);
    CHECK(DecodedOperation.Decoded.has_value());
    CHECK(DecodedOperation.Decoded->PairingOperation == PairingOperation);
    CHECK(IsValidControlPairingOperation(PairingOperation));
    CHECK(!IsValidControlPairingOperation(ControlPairingOperation{
        0, ControlPairingPhase::Succeeded}));

    CapabilitySet DesiredPermissions = RequestedCapabilities;
    DesiredPermissions.grant(Capability::ClipboardRead);
    ControlPermissionCandidate PermissionCandidate{
        45, MakeMachineId(8), "Companion PC", RequestedCapabilities,
        DesiredPermissions};
    ControlResponse PermissionResponse{20, ControlStatus::Ok};
    PermissionResponse.PermissionCandidate = PermissionCandidate;
    const auto PermissionFrame = EncodeControlResponse(PermissionResponse);
    CHECK(PermissionFrame.has_value());
    const auto DecodedPermission = DecodeControlResponse(*PermissionFrame);
    CHECK(DecodedPermission.Decoded.has_value());
    CHECK(DecodedPermission.Decoded->PermissionCandidate ==
          PermissionCandidate);
    PermissionCandidate.CurrentCapabilities = DesiredPermissions;
    CHECK(!IsValidControlPermissionCandidate(PermissionCandidate));

    auto DuplicateDevices = Devices;
    DuplicateDevices.Devices.push_back(Devices.Devices.front());
    CHECK(!IsValidControlTrustedDeviceList(DuplicateDevices));
    CHECK(IsValidControlTrustedDeviceList(ControlTrustedDeviceList{}));

    CHECK(!EncodeControlRequest(ControlRequest{}).has_value());
    CHECK(!EncodeControlRequest(ControlRequest{
        6, SetDesiredModeControlRequest{static_cast<DeskMode>(99)}}).has_value());
    CHECK(!EncodeControlRequest(ControlRequest{
        7, FocusMachineControlRequest{}}).has_value());
    CHECK(!EncodeControlRequest(ControlRequest{
        8, SetAudioGainControlRequest{10'001}}).has_value());
    CHECK(!EncodeControlRequest(ControlRequest{
        9, RequestLocalPermissionChangeControlRequest{
            MakeMachineId(8), CapabilitySet{1ull << 60u}}}).has_value());
    CHECK(!EncodeControlRequest(ControlRequest{
        10, ForgetTrustedDeviceControlRequest{}}).has_value());
    CHECK(!EncodeControlRequest(ControlRequest{
        11, StartDiscoveryControlRequest{31}}).has_value());
    CHECK(!EncodeControlRequest(ControlRequest{
        12, PairManualAddressControlRequest{
            "host with spaces", 43'821, {}}}).has_value());
    CHECK(!EncodeControlRequest(ControlRequest{
        13, GetManagedPairingDecisionControlRequest{}}).has_value());
    CHECK(!EncodeControlRequest(ControlRequest{
        14, ResolvePermissionCandidateControlRequest{}}).has_value());
    CHECK(!EncodeControlResponse(ControlResponse{
        10, ControlStatus::Failed, State}).has_value());
    auto InvalidRuntimeState = State;
    InvalidRuntimeState.RuntimePhase = BrokerRuntimePhase::ActionRequired;
    CHECK(!IsValidControlState(InvalidRuntimeState));
    InvalidRuntimeState.RemoteFocused = false;
    InvalidRuntimeState.CaptureActive = false;
    InvalidRuntimeState.FocusedMachine = {};
    InvalidRuntimeState.RuntimeFailure = BrokerRuntimeFailure::Protocol;
    CHECK(IsValidControlState(InvalidRuntimeState));
    InvalidRuntimeState.RuntimePhase = BrokerRuntimePhase::RetryWaiting;
    CHECK(!IsValidControlState(InvalidRuntimeState));
    InvalidRuntimeState.RuntimeFailure =
        BrokerRuntimeFailure::OrdinaryUnavailable;
    InvalidRuntimeState.RetryAttempt = 1;
    InvalidRuntimeState.RetryDelayMilliseconds = 800;
    CHECK(IsValidControlState(InvalidRuntimeState));
    InvalidRuntimeState.RetryDelayMilliseconds = 30'001;
    CHECK(!IsValidControlState(InvalidRuntimeState));
    InvalidRuntimeState.RetryDelayMilliseconds = 800;
    InvalidRuntimeState.RuntimePhase = BrokerRuntimePhase::ActionRequired;
    InvalidRuntimeState.RuntimeFailure = BrokerRuntimeFailure::Protocol;
    InvalidRuntimeState.RetryAttempt = 0;
    CHECK(!IsValidControlState(InvalidRuntimeState));

    auto WrongType = *EncodeControlRequest(Requests[0]);
    WrongType[7] = static_cast<std::uint8_t>(ControlFrameType::Response);
    CHECK(!DecodeControlRequest(WrongType).Decoded.has_value());
    CHECK(DecodeControlRequest(WrongType).Error ==
          ControlDecodeError::InvalidHeader);

    auto Oversized = *EncodeControlRequest(Requests[0]);
    const auto OversizedPayload = static_cast<std::uint32_t>(
        kMaximumControlPayload + 1);
    Oversized[16] = static_cast<std::uint8_t>(OversizedPayload >> 24u);
    Oversized[17] = static_cast<std::uint8_t>(OversizedPayload >> 16u);
    Oversized[18] = static_cast<std::uint8_t>(OversizedPayload >> 8u);
    Oversized[19] = static_cast<std::uint8_t>(OversizedPayload);
    CHECK(!DecodeControlRequest(Oversized).Decoded.has_value());
    CHECK(DecodeControlRequest(Oversized).Error == ControlDecodeError::Oversized);

    auto Trailing = *EncodeControlRequest(Requests[0]);
    Trailing.push_back(0);
    CHECK(!DecodeControlRequest(Trailing).Decoded.has_value());
    CHECK(DecodeControlRequest(Trailing).Error ==
          ControlDecodeError::InvalidPayload);
}

void RuntimeBrokerTrustAndPairingAuthorityAreFailClosed() {
    using namespace desklink;

    CHECK(!RuntimeOwnerMayBeActive(false, true));
    CHECK(RuntimeOwnerMayBeActive(false, false));
    CHECK(RuntimeOwnerMayBeActive(true, false));
    CHECK(RuntimeOwnerMayBeActive(true, true));

    for (const auto Phase : {
             BrokerRuntimePhase::Stopped,
             BrokerRuntimePhase::Paused,
             BrokerRuntimePhase::Listening,
             BrokerRuntimePhase::Discovering,
             BrokerRuntimePhase::Connecting,
             BrokerRuntimePhase::RetryWaiting,
             BrokerRuntimePhase::ActionRequired}) {
        CHECK(!ShouldQueryManagedRuntimeState(Phase));
    }
    CHECK(ShouldQueryManagedRuntimeState(
        BrokerRuntimePhase::ConnectedLocal));

    ManualClock ReconnectClock;
    BrokerReconnectController Reconnect(0x1234u);
    CHECK(Reconnect.AttemptDue(ReconnectClock.now()));
    CHECK(Reconnect.Begin(BrokerRuntimePhase::Discovering));
    CHECK(!Reconnect.Begin(BrokerRuntimePhase::ConnectedLocal));
    Reconnect.ProcessStopped(
        BrokerRuntimeFailure::OrdinaryUnavailable,
        ReconnectClock.now());
    auto ReconnectState = Reconnect.Snapshot();
    CHECK(ReconnectState.Phase == BrokerRuntimePhase::RetryWaiting);
    CHECK(ReconnectState.Failure ==
          BrokerRuntimeFailure::OrdinaryUnavailable);
    CHECK(ReconnectState.RetryAttempt == 1);
    const auto FirstDelay = std::chrono::duration_cast<
        std::chrono::milliseconds>(
        ReconnectState.RetryAt - ReconnectClock.now());
    CHECK(FirstDelay >= kBrokerReconnectMinimumDelay);
    CHECK(FirstDelay <= std::chrono::milliseconds{1'200});
    CHECK(!Reconnect.AttemptDue(ReconnectClock.now()));
    Reconnect.NetworkChanged(ReconnectClock.now());
    CHECK(Reconnect.AttemptDue(ReconnectClock.now()));
    CHECK(Reconnect.Begin(BrokerRuntimePhase::Connecting));
    Reconnect.ConnectedLocal();
    CHECK(Reconnect.Snapshot().Phase ==
          BrokerRuntimePhase::ConnectedLocal);
    CHECK(Reconnect.Snapshot().RetryAttempt == 0);

    Reconnect.ProcessStopped(
        BrokerRuntimeFailure::Authentication,
        ReconnectClock.now());
    CHECK(Reconnect.Snapshot().Phase ==
          BrokerRuntimePhase::ActionRequired);
    CHECK(!Reconnect.AttemptDue(ReconnectClock.now()));
    Reconnect.NetworkChanged(ReconnectClock.now());
    CHECK(!Reconnect.AttemptDue(ReconnectClock.now()));
    Reconnect.Pause(ReconnectClock.now());
    CHECK(Reconnect.Snapshot().Phase == BrokerRuntimePhase::Paused);
    CHECK(!Reconnect.AttemptDue(ReconnectClock.now()));
    Reconnect.Resume(ReconnectClock.now());
    CHECK(Reconnect.Snapshot().Phase == BrokerRuntimePhase::Stopped);
    CHECK(Reconnect.AttemptDue(ReconnectClock.now()));

    Reconnect.SystemSuspend(ReconnectClock.now());
    CHECK(Reconnect.Snapshot().SystemSuspended);
    CHECK(Reconnect.Snapshot().Paused);
    CHECK(!Reconnect.AttemptDue(ReconnectClock.now()));
    Reconnect.NetworkChanged(ReconnectClock.now());
    CHECK(!Reconnect.AttemptDue(ReconnectClock.now()));
    Reconnect.SystemResume(ReconnectClock.now());
    CHECK(!Reconnect.Snapshot().SystemSuspended);
    CHECK(!Reconnect.Snapshot().Paused);
    CHECK(Reconnect.Snapshot().Phase == BrokerRuntimePhase::Stopped);
    CHECK(Reconnect.AttemptDue(ReconnectClock.now()));

    Reconnect.Pause(ReconnectClock.now());
    Reconnect.SystemSuspend(ReconnectClock.now());
    Reconnect.SystemResume(ReconnectClock.now());
    CHECK(!Reconnect.Snapshot().SystemSuspended);
    CHECK(Reconnect.Snapshot().Paused);
    CHECK(Reconnect.Snapshot().Phase == BrokerRuntimePhase::Paused);
    Reconnect.Resume(ReconnectClock.now());

    Reconnect.ProcessStopped(
        BrokerRuntimeFailure::Authentication,
        ReconnectClock.now());
    Reconnect.SystemSuspend(ReconnectClock.now());
    Reconnect.SystemResume(ReconnectClock.now());
    CHECK(!Reconnect.Snapshot().SystemSuspended);
    CHECK(Reconnect.Snapshot().Phase ==
          BrokerRuntimePhase::ActionRequired);
    CHECK(Reconnect.Snapshot().Failure ==
          BrokerRuntimeFailure::Authentication);
    CHECK(!Reconnect.AttemptDue(ReconnectClock.now()));

    Reconnect.SystemSuspend(ReconnectClock.now());
    Reconnect.Resume(ReconnectClock.now());
    CHECK(Reconnect.Snapshot().SystemSuspended);
    CHECK(Reconnect.Snapshot().Paused);
    Reconnect.SystemResume(ReconnectClock.now());
    CHECK(!Reconnect.Snapshot().SystemSuspended);
    CHECK(!Reconnect.Snapshot().Paused);
    CHECK(Reconnect.AttemptDue(ReconnectClock.now()));

    CHECK(IsRetryableBrokerRuntimeFailure(
        BrokerRuntimeFailure::OrdinaryUnavailable));
    for (const auto Failure : {
             BrokerRuntimeFailure::Security,
             BrokerRuntimeFailure::Identity,
             BrokerRuntimeFailure::Credential,
             BrokerRuntimeFailure::Signing,
             BrokerRuntimeFailure::Authentication,
             BrokerRuntimeFailure::Capability,
             BrokerRuntimeFailure::Protocol,
             BrokerRuntimeFailure::Unknown}) {
        CHECK(!IsRetryableBrokerRuntimeFailure(Failure));
    }
    CHECK(BrokerReconnectDelay(16, 1) <=
          std::chrono::duration_cast<std::chrono::milliseconds>(
              kBrokerReconnectMaximumDelay));
    CHECK(ClassifyBrokerManagedProcessExit(
              kBrokerManagedRetryableProcessExit) ==
          BrokerRuntimeFailure::OrdinaryUnavailable);
    CHECK(ClassifyBrokerManagedProcessExit(
              kBrokerManagedActionRequiredProcessExit) ==
          BrokerRuntimeFailure::Unknown);
    CHECK(ClassifyBrokerManagedProcessExit(
              kBrokerManagedProtocolProcessExit) ==
          BrokerRuntimeFailure::Protocol);
    CHECK(ClassifyBrokerManagedProcessExit(1) ==
          BrokerRuntimeFailure::Unknown);

    InMemoryTrustStore Store;
    CapabilitySet Initial;
    Initial.grant(Capability::InputInject);
    Initial.grant(Capability::AudioSend);
    const auto Zulu = MakeIdentity(70, "Zulu PC");
    const auto Alpha = MakeIdentity(71, "Alpha PC");
    SaveTrustedPeer(Store, Zulu, Initial);
    SaveTrustedPeer(Store, Alpha, {});

    RecordingRuntimeSafetyController Safety;
    RuntimeTrustAuthority Authority(Store, Safety);
    CHECK(!Authority.ReloadAfterExternalPairing());
    bool Reloaded = false;
    RuntimeTrustAuthority ReloadingAuthority(
        Store, Safety, [&Reloaded] {
            Reloaded = true;
            return true;
        });
    CHECK(ReloadingAuthority.ReloadAfterExternalPairing());
    CHECK(Reloaded);
    const auto Listed = Authority.ListTrustedPeers();
    CHECK(Listed.has_value());
    CHECK(Listed->size() == 2);
    CHECK((*Listed)[0].Identity.display_name == "Alpha PC");
    CHECK((*Listed)[1].Identity.display_name == "Zulu PC");

    CapabilitySet Broader = Initial;
    Broader.grant(Capability::ClipboardRead);
    CHECK(Authority.RequestPermissionChange(Zulu.machine_id, Broader) ==
          TrustMutationStatus::ReauthorizationRequired);
    CHECK(Safety.ReturnLocalCalls.empty());
    CHECK(Safety.RefreshCalls.empty());
    CHECK(Safety.StopCalls.empty());
    CHECK(Store.GetPeer(Zulu.machine_id)->Capabilities.bits() ==
          Initial.bits());

    RecordingRuntimeSafetyController ReauthorizationSafety;
    RuntimeTrustAuthority ReauthorizationAuthority(
        Store, ReauthorizationSafety);
    const auto AlphaBefore = Store.GetPeer(Alpha.machine_id);
    CHECK(AlphaBefore.has_value());
    CapabilitySet AlphaDesired;
    AlphaDesired.grant(Capability::ClipboardRead);
    CHECK(ReauthorizationAuthority.ApplyReauthorizedPermissionChange(
              AlphaBefore->Identity, AlphaBefore->Capabilities,
              AlphaDesired) == TrustMutationStatus::Applied);
    CHECK(ReauthorizationSafety.ReturnLocalCalls.size() == 1);
    CHECK(ReauthorizationSafety.RefreshCalls.size() == 1);
    CHECK(ReauthorizationSafety.StopCalls.empty());
    const auto AlphaAfter = Store.GetPeer(Alpha.machine_id);
    CHECK(AlphaAfter.has_value());
    CHECK(AlphaAfter->Identity == AlphaBefore->Identity);
    CHECK(AlphaAfter->Capabilities == AlphaDesired);
    CHECK(ReauthorizationAuthority.ApplyReauthorizedPermissionChange(
              AlphaBefore->Identity, AlphaBefore->Capabilities,
              AlphaDesired) ==
          TrustMutationStatus::ReauthorizationRequired);
    CHECK(ReauthorizationSafety.ReturnLocalCalls.size() == 1);
    CHECK(ReauthorizationSafety.RefreshCalls.size() == 1);
    auto WrongAlphaIdentity = AlphaAfter->Identity;
    WrongAlphaIdentity.public_key_fingerprint =
        FormatFingerprint(MakeDigest(99));
    CapabilitySet AlphaBroader = AlphaDesired;
    AlphaBroader.grant(Capability::ClipboardWrite);
    CHECK(ReauthorizationAuthority.ApplyReauthorizedPermissionChange(
              WrongAlphaIdentity, AlphaDesired, AlphaBroader) ==
          TrustMutationStatus::InvalidRequest);
    CHECK(ReauthorizationSafety.ReturnLocalCalls.size() == 1);
    CHECK(ReauthorizationSafety.RefreshCalls.size() == 1);
    ReauthorizationSafety.Succeeds = false;
    CHECK(ReauthorizationAuthority.ApplyReauthorizedPermissionChange(
              AlphaAfter->Identity, AlphaDesired, AlphaBroader) ==
          TrustMutationStatus::CleanupFailed);
    CHECK(ReauthorizationSafety.ReturnLocalCalls.size() == 2);
    CHECK(ReauthorizationSafety.RefreshCalls.size() == 1);
    CHECK(ReauthorizationSafety.StopCalls.empty());
    CHECK(Store.GetPeer(Alpha.machine_id)->Capabilities == AlphaDesired);

    InMemoryTrustStore RefreshFailureStore;
    const auto RefreshFailurePeer = MakeIdentity(75, "Refresh failure PC");
    SaveTrustedPeer(RefreshFailureStore, RefreshFailurePeer, {});
    RecordingRuntimeSafetyController RefreshFailureSafety;
    RefreshFailureSafety.RefreshSucceeds = false;
    RuntimeTrustAuthority RefreshFailureAuthority(
        RefreshFailureStore, RefreshFailureSafety);
    CapabilitySet RefreshFailureDesired;
    RefreshFailureDesired.grant(Capability::ClipboardRead);
    CHECK(RefreshFailureAuthority.ApplyReauthorizedPermissionChange(
              RefreshFailurePeer, {}, RefreshFailureDesired) ==
          TrustMutationStatus::CleanupFailed);
    CHECK(RefreshFailureSafety.ReturnLocalCalls.size() == 1);
    CHECK(RefreshFailureSafety.RefreshCalls.size() == 1);
    CHECK(RefreshFailureSafety.StopCalls.empty());
    CHECK(RefreshFailureStore.GetPeer(RefreshFailurePeer.machine_id)
              ->Capabilities == RefreshFailureDesired);

    CapabilitySet Reduced;
    Reduced.grant(Capability::InputInject);
    CHECK(Authority.RequestPermissionChange(Zulu.machine_id, Reduced) ==
          TrustMutationStatus::Applied);
    CHECK(Safety.ReturnLocalCalls.size() == 1);
    CHECK(Safety.RefreshCalls.size() == 1);
    CHECK(Safety.StopCalls.empty());
    CHECK(Store.GetPeer(Zulu.machine_id)->Capabilities.bits() ==
          Reduced.bits());
    CHECK(Authority.RequestPermissionChange(Zulu.machine_id, Reduced) ==
          TrustMutationStatus::NoChange);
    CHECK(Safety.ReturnLocalCalls.size() == 1);
    CHECK(Safety.RefreshCalls.size() == 1);

    Safety.Succeeds = false;
    CHECK(Authority.RequestPermissionChange(Zulu.machine_id, {}) ==
          TrustMutationStatus::CleanupFailed);
    CHECK(Safety.ReturnLocalCalls.size() == 2);
    CHECK(Safety.RefreshCalls.size() == 1);
    CHECK(Store.GetPeer(Zulu.machine_id)->Capabilities.bits() ==
          Reduced.bits());
    CHECK(Authority.ForgetPeer(Zulu.machine_id) ==
          TrustMutationStatus::CleanupFailed);
    CHECK(Safety.StopCalls.size() == 1);
    CHECK(Store.GetPeer(Zulu.machine_id).has_value());

    Safety.Succeeds = true;
    CHECK(Authority.ForgetPeer(Zulu.machine_id) ==
          TrustMutationStatus::Applied);
    CHECK(Safety.StopCalls.size() == 2);
    CHECK(!Store.GetPeer(Zulu.machine_id).has_value());
    CHECK(Authority.ForgetPeer(Zulu.machine_id) ==
          TrustMutationStatus::PeerNotFound);
    CHECK(Authority.RequestPermissionChange(
              MachineId{}, CapabilitySet{}) ==
          TrustMutationStatus::InvalidRequest);
    CHECK(Authority.RequestPermissionChange(
              Alpha.machine_id,
              CapabilitySet{kKnownCapabilityBits | (1ull << 60u)}) ==
          TrustMutationStatus::InvalidRequest);

    ManualClock Clock;
    BrokerPairingCandidateLease Lease;
    PairingCandidate Candidate;
    Candidate.Status = PairingStatus::Ready;
    Candidate.Identity = MakeIdentity(72, "Pairing PC");
    Candidate.VerificationCode = "123456";
    Candidate.TranscriptDigest = MakeDigest(73);
    CHECK(Lease.Present(BrokerPairingCandidate{
        1, 55, Candidate, Reduced,
        Clock.now() + std::chrono::seconds(30)}, Clock.now()));
    CHECK(!Lease.Present(BrokerPairingCandidate{
        2, 55, Candidate, Reduced,
        Clock.now() + std::chrono::seconds(30)}, Clock.now()));
    Lease.ClientDisconnected(54);
    CHECK(Lease.Current(Clock.now()).has_value());
    Lease.ClientDisconnected(55);
    CHECK(!Lease.Current(Clock.now()).has_value());

    CHECK(Lease.Present(BrokerPairingCandidate{
        3, 56, Candidate, Reduced,
        Clock.now() + std::chrono::seconds(1)}, Clock.now()));
    Clock.advance(std::chrono::seconds(1));
    CHECK(!Lease.Current(Clock.now()).has_value());
    CHECK(!Lease.ResolveLocally(3, true, Clock.now()).has_value());

    CHECK(Lease.Present(BrokerPairingCandidate{
        4, 57, Candidate, Reduced,
        Clock.now() + std::chrono::seconds(30)}, Clock.now()));
    CHECK(!Lease.ResolveLocally(4, false, Clock.now()).has_value());
    CHECK(!Lease.Current(Clock.now()).has_value());
    CHECK(Lease.Present(BrokerPairingCandidate{
        5, 57, Candidate, Reduced,
        Clock.now() + std::chrono::seconds(30)}, Clock.now()));
    const auto Approved = Lease.ResolveLocally(5, true, Clock.now());
    CHECK(Approved.has_value());
    CHECK(Approved->Candidate.VerificationCode == "123456");
    CHECK(!Lease.Current(Clock.now()).has_value());

    BrokerPermissionCandidateLease PermissionLease;
    const auto PermissionIdentity = MakeIdentity(74, "Permission PC");
    CapabilitySet PermissionCurrent;
    PermissionCurrent.grant(Capability::InputInject);
    CapabilitySet PermissionDesired = PermissionCurrent;
    PermissionDesired.grant(Capability::ClipboardRead);
    BrokerPermissionCandidate PermissionCandidate{
        80, PermissionIdentity, PermissionCurrent, PermissionDesired,
        Clock.now() + std::chrono::seconds(30)};
    CHECK(PermissionLease.Present(PermissionCandidate, Clock.now()));
    CHECK(!PermissionLease.Present(PermissionCandidate, Clock.now()));
    CHECK(PermissionLease.Current(Clock.now()).has_value());
    CHECK(!PermissionLease.ResolveLocally(80, false, Clock.now()).has_value());
    CHECK(!PermissionLease.Current(Clock.now()).has_value());

    PermissionCandidate.RequestId = 81;
    PermissionCandidate.ExpiresAt =
        Clock.now() + std::chrono::seconds(1);
    CHECK(PermissionLease.Present(PermissionCandidate, Clock.now()));
    Clock.advance(std::chrono::seconds(1));
    CHECK(!PermissionLease.Current(Clock.now()).has_value());
    CHECK(!PermissionLease.ResolveLocally(81, true, Clock.now()).has_value());

    PermissionCandidate.RequestId = 82;
    PermissionCandidate.ExpiresAt =
        Clock.now() + std::chrono::seconds(30);
    CHECK(PermissionLease.Present(PermissionCandidate, Clock.now()));
    const auto PermissionApproved =
        PermissionLease.ResolveLocally(82, true, Clock.now());
    CHECK(PermissionApproved.has_value());
    CHECK(PermissionApproved->Identity == PermissionIdentity);
    CHECK(PermissionApproved->CurrentCapabilities == PermissionCurrent);
    CHECK(PermissionApproved->DesiredCapabilities == PermissionDesired);
    CHECK(!PermissionLease.Current(Clock.now()).has_value());

    PermissionCandidate.RequestId = 83;
    CHECK(PermissionLease.Present(PermissionCandidate, Clock.now()));
    PermissionLease.RejectAll();
    CHECK(!PermissionLease.Current(Clock.now()).has_value());
}

class RecordingUpdateBackend final : public desklink::IUpdateBackend {
public:
    std::vector<std::string> Calls;
    std::string FailAt;
    std::string ThrowAt;

    bool ValidatePackages() override { return Record("validate-packages"); }
    bool RequestReturnLocal() override { return Record("return-local"); }
    bool ConfirmLocal() override { return Record("confirm-local"); }
    bool RequestRuntimeShutdown() override {
        return Record("request-runtime-shutdown");
    }
    bool WaitForRuntimeShutdown() override {
        return Record("wait-runtime-shutdown");
    }
    bool RequestUiShutdown() override {
        return Record("request-ui-shutdown");
    }
    bool WaitForUiShutdown() override { return Record("wait-ui-shutdown"); }
    bool InstallCandidate() override { return Record("install-candidate"); }
    bool ValidateCandidate() override { return Record("validate-candidate"); }
    bool InstallRollback() override { return Record("install-rollback"); }
    bool ValidateRollback() override { return Record("validate-rollback"); }
    bool RestartApplication() override { return Record("restart"); }

private:
    bool Record(std::string Name) {
        Calls.push_back(Name);
        if (ThrowAt == Name) throw std::runtime_error("update backend failure");
        return FailAt != Name;
    }
};

void UpdateCoordinatorIsOrderedAndFailsLocal() {
    using namespace desklink;

    RecordingUpdateBackend SuccessBackend;
    UpdateCoordinator Success(SuccessBackend);
    const auto SuccessResult = Success.Run(true);
    CHECK(SuccessResult.State == UpdateState::Completed);
    CHECK(SuccessResult.Failure == UpdateFailure::None);
    CHECK(SuccessResult.CandidateInstalled);
    CHECK(!SuccessResult.RollbackInstalled);
    const std::vector<std::string> ExpectedSuccess{
        "validate-packages", "return-local", "confirm-local",
        "request-runtime-shutdown", "wait-runtime-shutdown",
        "request-ui-shutdown", "wait-ui-shutdown", "install-candidate",
        "validate-candidate", "restart"};
    CHECK(SuccessBackend.Calls == ExpectedSuccess);

    RecordingUpdateBackend InvalidPackage;
    InvalidPackage.FailAt = "validate-packages";
    const auto InvalidResult = UpdateCoordinator(InvalidPackage).Run(true);
    CHECK(InvalidResult.State == UpdateState::Failed);
    CHECK(InvalidResult.Failure == UpdateFailure::PackageValidationFailed);
    CHECK(InvalidPackage.Calls.size() == 1);

    RecordingUpdateBackend NotLocal;
    NotLocal.FailAt = "confirm-local";
    const auto NotLocalResult = UpdateCoordinator(NotLocal).Run(true);
    CHECK(NotLocalResult.State == UpdateState::Failed);
    CHECK(NotLocalResult.Failure == UpdateFailure::LocalConfirmationFailed);
    CHECK(std::find(NotLocal.Calls.begin(), NotLocal.Calls.end(),
                    "install-candidate") == NotLocal.Calls.end());

    RecordingUpdateBackend FailedInstall;
    FailedInstall.FailAt = "install-candidate";
    const auto RollbackResult = UpdateCoordinator(FailedInstall).Run(true);
    CHECK(RollbackResult.State == UpdateState::RolledBack);
    CHECK(RollbackResult.Failure == UpdateFailure::CandidateInstallFailed);
    CHECK(!RollbackResult.CandidateInstalled);
    CHECK(RollbackResult.RollbackInstalled);
    const std::vector<std::string> RollbackTail{
        "install-candidate", "install-rollback", "validate-rollback",
        "restart"};
    CHECK(FailedInstall.Calls.size() >= RollbackTail.size());
    CHECK(std::equal(RollbackTail.begin(), RollbackTail.end(),
                     FailedInstall.Calls.end() -
                         static_cast<std::ptrdiff_t>(RollbackTail.size())));

    RecordingUpdateBackend ValidationException;
    ValidationException.ThrowAt = "validate-candidate";
    const auto ExceptionResult =
        UpdateCoordinator(ValidationException).Run(false);
    CHECK(ExceptionResult.State == UpdateState::RolledBack);
    CHECK(ExceptionResult.Failure == UpdateFailure::CandidateValidationFailed);
    CHECK(ExceptionResult.CandidateInstalled);
    CHECK(ExceptionResult.RollbackInstalled);

    RecordingUpdateBackend FailedRollback;
    FailedRollback.FailAt = "install-candidate";
    FailedRollback.ThrowAt = "install-rollback";
    const auto FailedRollbackResult =
        UpdateCoordinator(FailedRollback).Run(true);
    CHECK(FailedRollbackResult.State == UpdateState::Failed);
    CHECK(FailedRollbackResult.Failure == UpdateFailure::RollbackInstallFailed);
    CHECK(!FailedRollbackResult.RollbackInstalled);
    CHECK(std::find(FailedRollback.Calls.begin(), FailedRollback.Calls.end(),
                    "restart") == FailedRollback.Calls.end());

    RecordingUpdateBackend FailedRestart;
    FailedRestart.FailAt = "restart";
    const auto FailedRestartResult = UpdateCoordinator(FailedRestart).Run(true);
    CHECK(FailedRestartResult.State == UpdateState::Failed);
    CHECK(FailedRestartResult.Failure == UpdateFailure::RestartFailed);
    CHECK(FailedRestartResult.CandidateInstalled);
    CHECK(!FailedRestartResult.RollbackInstalled);
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

void DisplayMetadataIsBoundedAndDoesNotChangeRoutingGeneration() {
    using namespace desklink;
    DisplayTopologyMap Topology;
    DiscoveredDisplay Display{
        "metadata-monitor", "Metadata monitor", {0, 0, 2560, 1440}, true};
    Display.PixelWidth = 2560;
    Display.PixelHeight = 1440;
    Display.RefreshMilliHertz = 177'000;
    Display.PhysicalWidthMillimeters = 597;
    Display.PhysicalHeightMillimeters = 336;
    Display.PhysicalSize = PhysicalSizeSource::Edid;
    Display.Orientation = DisplayOrientation::Landscape;
    CHECK(Topology.Update({Display}) == DisplayTopologyUpdate::Changed);
    const auto Generation = Topology.Current().Generation;
    const auto Id = DeriveStableDisplayId(Display.StableIdentity);
    CHECK(Topology.Current().FindStableIdentity(Display.StableIdentity) ==
          Topology.Current().Find(Id));
    CHECK(Topology.Current().Find(Id)->RefreshMilliHertz == 177'000);

    Display.RefreshMilliHertz = 165'000;
    Display.PhysicalWidthMillimeters = 600;
    Display.FriendlyName = "Renamed metadata monitor";
    CHECK(Topology.Update({Display}) == DisplayTopologyUpdate::Unchanged);
    CHECK(Topology.Current().Generation == Generation);
    CHECK(Topology.Current().Find(Id)->RefreshMilliHertz == 165'000);
    CHECK(Topology.Current().Find(Id)->PhysicalWidthMillimeters == 600);

    auto Invalid = Display;
    Invalid.PhysicalSize = PhysicalSizeSource::Unknown;
    CHECK(Topology.Update({Invalid}) == DisplayTopologyUpdate::Invalid);
    Invalid = Display;
    Invalid.RefreshMilliHertz = kMaximumDisplayRefreshMilliHertz + 1;
    CHECK(Topology.Update({Invalid}) == DisplayTopologyUpdate::Invalid);
    Invalid = Display;
    Invalid.RefreshMilliHertz = kMinimumDisplayRefreshMilliHertz - 1;
    CHECK(Topology.Update({Invalid}) == DisplayTopologyUpdate::Invalid);
    Invalid = Display;
    Invalid.PixelWidth = kMaximumDisplayPixelDimension + 1;
    CHECK(Topology.Update({Invalid}) == DisplayTopologyUpdate::Invalid);
    Invalid = Display;
    Invalid.Bounds.Right = static_cast<std::int32_t>(
        kMaximumDisplayPixelDimension + 1);
    CHECK(Topology.Update({Invalid}) == DisplayTopologyUpdate::Invalid);
    Invalid = Display;
    Invalid.Bounds = {
        std::numeric_limits<std::int32_t>::min(), 0,
        std::numeric_limits<std::int32_t>::max(), 1440};
    CHECK(Topology.Update({Invalid}) == DisplayTopologyUpdate::Invalid);
    Invalid = Display;
    Invalid.Bounds = {
        0, std::numeric_limits<std::int32_t>::min(), 2560,
        std::numeric_limits<std::int32_t>::max()};
    CHECK(Topology.Update({Invalid}) == DisplayTopologyUpdate::Invalid);
    Invalid = Display;
    Invalid.Orientation = static_cast<DisplayOrientation>(99);
    CHECK(Topology.Update({Invalid}) == DisplayTopologyUpdate::Invalid);
    CHECK(Topology.Current().Generation == Generation);

    DisplayTopologyMap BoundaryTopology;
    auto Boundary = Display;
    Boundary.Bounds = {
        -32'768, 0,
        static_cast<std::int32_t>(
            kMaximumDisplayPixelDimension - 32'768),
        1440};
    Boundary.PixelWidth = kMaximumDisplayPixelDimension;
    CHECK(BoundaryTopology.Update({Boundary}) ==
          DisplayTopologyUpdate::Changed);
}

void EdidPhysicalSizeParsingIsStrictAndBounded() {
    using namespace desklink;
    std::array<std::uint8_t, 128> Edid{};
    const std::array<std::uint8_t, 8> Header{
        0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    std::copy(Header.begin(), Header.end(), Edid.begin());
    Edid[21] = 60;
    Edid[22] = 34;
    constexpr std::size_t Descriptor = 54;
    Edid[Descriptor] = 1;
    Edid[Descriptor + 1] = 1;
    Edid[Descriptor + 12] = 0x55;
    Edid[Descriptor + 13] = 0x50;
    Edid[Descriptor + 14] = 0x21;
    const auto SetChecksum = [&Edid] {
        std::uint8_t Sum{};
        for (std::size_t Index = 0; Index < 127; ++Index) {
            Sum = static_cast<std::uint8_t>(Sum + Edid[Index]);
        }
        Edid[127] = static_cast<std::uint8_t>(0u - Sum);
    };
    SetChecksum();
    CHECK(ParseEdidPhysicalSize(Edid) ==
          (PhysicalDisplaySize{597, 336}));

    auto Corrupt = Edid;
    ++Corrupt[10];
    CHECK(!ParseEdidPhysicalSize(Corrupt));
    CHECK(!ParseEdidPhysicalSize(ByteSpan{Edid.data(), 127}));

    Edid[Descriptor] = 0;
    Edid[Descriptor + 1] = 0;
    SetChecksum();
    CHECK(ParseEdidPhysicalSize(Edid) ==
          (PhysicalDisplaySize{600, 340}));
    CHECK(OrientPhysicalDisplaySize(
              {600, 340}, DisplayOrientation::Landscape) ==
          (PhysicalDisplaySize{600, 340}));
    CHECK(OrientPhysicalDisplaySize(
              {600, 340}, DisplayOrientation::LandscapeFlipped) ==
          (PhysicalDisplaySize{600, 340}));
    CHECK(OrientPhysicalDisplaySize(
              {600, 340}, DisplayOrientation::Portrait) ==
          (PhysicalDisplaySize{340, 600}));
    CHECK(OrientPhysicalDisplaySize(
              {600, 340}, DisplayOrientation::PortraitFlipped) ==
          (PhysicalDisplaySize{340, 600}));
    CHECK(!OrientPhysicalDisplaySize(
        {600, 340}, static_cast<DisplayOrientation>(99)));
}

void RoamingGraphValidationAndCodecAreStrict() {
    using namespace desklink;
    const auto Configuration = MakeRoamingConfiguration();
    CHECK(IsValidRoamingConfiguration(Configuration));
    const auto Encoded = EncodeRoamingConfiguration(Configuration);
    CHECK(Encoded.has_value());
    CHECK(DecodeRoamingConfiguration(*Encoded) == Configuration);

    auto InvalidBytes = *Encoded;
    InvalidBytes.push_back(0);
    CHECK(!DecodeRoamingConfiguration(InvalidBytes));
    InvalidBytes = *Encoded;
    InvalidBytes[0] ^= 0xffu;
    CHECK(!DecodeRoamingConfiguration(InvalidBytes));
    InvalidBytes = *Encoded;
    InvalidBytes.resize(InvalidBytes.size() - 1);
    CHECK(!DecodeRoamingConfiguration(InvalidBytes));
    CHECK(!DecodeRoamingConfiguration(
        ByteBuffer(kMaximumRoamingSettingsBytes + 1)));

    auto Invalid = Configuration;
    Invalid.Links[0].EndpointA.Machine = {};
    CHECK(!IsValidRoamingConfiguration(Invalid));
    Invalid = Configuration;
    Invalid.Links[0].EndpointB.Machine = Invalid.Links[0].EndpointA.Machine;
    CHECK(!IsValidRoamingConfiguration(Invalid));
    Invalid = Configuration;
    Invalid.Links[0].EndpointA.SegmentEndPermyriad =
        Invalid.Links[0].EndpointA.SegmentStartPermyriad;
    CHECK(!IsValidRoamingConfiguration(Invalid));
    Invalid = Configuration;
    Invalid.Links[0].AToB.PushDistancePixels = 65;
    CHECK(!IsValidRoamingConfiguration(Invalid));
    Invalid = Configuration;
    Invalid.Links.push_back(Invalid.Links.front());
    CHECK(!IsValidRoamingConfiguration(Invalid));

    auto Touching = Configuration;
    auto Second = Touching.Links.front();
    Second.EndpointA.SegmentStartPermyriad = 9'000;
    Second.EndpointA.SegmentEndPermyriad = 10'000;
    Second.EndpointB.Machine = MakeMachineId(3);
    Second.EndpointB.StableDisplayIdentity = "display-c";
    Second.Direction = RoamingDirectionMode::AToB;
    Touching.Links.push_back(Second);
    CHECK(IsValidRoamingConfiguration(Touching));
    Touching.Links.back().EndpointA.SegmentStartPermyriad = 8'999;
    CHECK(!IsValidRoamingConfiguration(Touching));
    Touching.Links.back().Enabled = false;
    CHECK(IsValidRoamingConfiguration(Touching));

    Invalid = Configuration;
    Invalid.CanvasLayout.push_back(Invalid.CanvasLayout.front());
    CHECK(!IsValidRoamingConfiguration(Invalid));
    Invalid = Configuration;
    Invalid.CanvasLayout[0].StableDisplayIdentity.push_back('\0');
    CHECK(!IsValidRoamingConfiguration(Invalid));
}

void MonitorConfiguratorCanvasIsPresentationOnlyAndSuggestsExplicitLinks() {
    using namespace desklink;
    DisplayTopologyMap LocalTopology;
    DiscoveredDisplay LocalDisplay{
        "local-display", "LG UltraGear 27", {0, 0, 2560, 1440}, true};
    LocalDisplay.PixelWidth = 2560;
    LocalDisplay.PixelHeight = 1440;
    LocalDisplay.RefreshMilliHertz = 177'000;
    LocalDisplay.PhysicalWidthMillimeters = 600;
    LocalDisplay.PhysicalHeightMillimeters = 340;
    LocalDisplay.PhysicalSize = PhysicalSizeSource::Edid;
    CHECK(LocalTopology.Update({LocalDisplay}) ==
          DisplayTopologyUpdate::Changed);

    DisplayTopologyMap PeerTopology;
    DiscoveredDisplay PeerDisplay{
        "peer-display", "Peer monitor", {0, 0, 1920, 1080}, true};
    PeerDisplay.PixelWidth = 1920;
    PeerDisplay.PixelHeight = 1080;
    PeerDisplay.RefreshMilliHertz = 60'000;
    PeerDisplay.PhysicalWidthMillimeters = 510;
    PeerDisplay.PhysicalHeightMillimeters = 290;
    PeerDisplay.PhysicalSize = PhysicalSizeSource::RawDpiEstimate;
    CHECK(PeerTopology.Update({PeerDisplay}) ==
          DisplayTopologyUpdate::Changed);

    RoamingConfiguration Configuration;
    Configuration.CanvasLayout.push_back(
        {MakeMachineId(1), "local-display", 10, 20});
    Configuration.CanvasLayout.push_back(
        {MakeMachineId(3), "offline-display", 900, 40});
    const std::array Machines{
        MonitorCanvasMachine{
            MakeMachineId(1), "This PC", LocalTopology.Current(),
            DisplayTopologyExchangeStatus::Ready, true, false},
        MonitorCanvasMachine{
            MakeMachineId(2), "Peer PC", PeerTopology.Current(),
            DisplayTopologyExchangeStatus::Ready, false, true},
    };
    const auto Model = BuildMonitorCanvasModel(Machines, Configuration);
    CHECK(Model.has_value());
    CHECK(Model->Tiles.size() == 3);
    CHECK(Model->Tiles[0].Rect.X == 10);
    CHECK(Model->Tiles[0].Rect.Y == 20);
    CHECK(Model->Tiles[0].Rect.Width == 270);
    CHECK(!Model->Tiles[0].SizeEstimated);
    CHECK(Model->Tiles[1].SizeEstimated);
    CHECK(!Model->Tiles[2].Online);
    CHECK(Model->Tiles[2].FriendlyName == "Offline display");

    auto Adjacent = Model->Tiles;
    Adjacent[0].Rect = {10, 20, 270, 153};
    Adjacent[1].Rect = {284, 20, 230, 131};
    const auto Suggestion = BuildRoamingLinkSuggestion(Adjacent, 0, 1);
    CHECK(Suggestion.has_value());
    CHECK(Suggestion->Link.Direction == RoamingDirectionMode::Bidirectional);
    CHECK(Suggestion->Link.EndpointA.Side == DisplayEdgeSide::Right);
    CHECK(Suggestion->Link.EndpointB.Side == DisplayEdgeSide::Left);
    CHECK(Suggestion->Link.EndpointA.SegmentStartPermyriad == 0);
    CHECK(Suggestion->Link.EndpointA.SegmentEndPermyriad == 10'000);
    CHECK(IsValidRoamingConfiguration(
        RoamingConfiguration{{}, {Suggestion->Link}, {}}));

    // Canvas movement changes only the suggestion. Stable route resolution is
    // independent of every presentation coordinate.
    Adjacent[0].Rect.X = -500'000;
    CHECK(!BuildRoamingLinkSuggestion(Adjacent, 0, 1).has_value());
    std::array<MachineDisplayTopology, 2> Topologies{
        MachineDisplayTopology{MakeMachineId(1), &LocalTopology.Current()},
        MachineDisplayTopology{MakeMachineId(2), &PeerTopology.Current()},
    };
    CHECK(ResolveRoamingLink(Suggestion->Link, Topologies).Ready());
    CHECK(!BuildRoamingLinkSuggestion(Model->Tiles, 0, 2).has_value());
}

void RoamingEndpointsResolveAgainstCurrentStableTopologies() {
    using namespace desklink;
    auto Configuration = MakeRoamingConfiguration();
    DisplayTopologyMap TopologyA;
    DisplayTopologyMap TopologyB;
    CHECK(TopologyA.Update({
        {"display-a", "A", {0, 0, 1920, 1080}, true}}) ==
        DisplayTopologyUpdate::Changed);
    CHECK(TopologyB.Update({
        {"display-b", "B", {0, 0, 2560, 1440}, true}}) ==
        DisplayTopologyUpdate::Changed);
    std::array<MachineDisplayTopology, 2> Topologies{
        MachineDisplayTopology{MakeMachineId(1), &TopologyA.Current()},
        MachineDisplayTopology{MakeMachineId(2), &TopologyB.Current()},
    };
    const auto Ready = ResolveRoamingLink(Configuration.Links[0], Topologies);
    CHECK(Ready.Ready());
    CHECK(Ready.EndpointA.Endpoint->Display ==
          DeriveStableDisplayId("display-a"));
    CHECK(Ready.EndpointB.Endpoint->TopologyGeneration == 1);

    auto Missing = Configuration.Links[0].EndpointB;
    Missing.StableDisplayIdentity = "offline-display";
    CHECK(ResolveRoamingEndpoint(Missing, Topologies).Status ==
          RoamingEndpointResolution::DisplayMissing);
    Missing.Machine = MakeMachineId(9);
    CHECK(ResolveRoamingEndpoint(Missing, Topologies).Status ==
          RoamingEndpointResolution::MachineUnavailable);

    std::array<MachineDisplayTopology, 3> Ambiguous{
        Topologies[0], Topologies[1], Topologies[1]};
    CHECK(ResolveRoamingEndpoint(
              Configuration.Links[0].EndpointB, Ambiguous).Status ==
          RoamingEndpointResolution::AmbiguousMachine);

    auto DisplayA = DiscoveredDisplay{
        "display-a", "A renamed", {0, 0, 1920, 1080}, true};
    DisplayA.RefreshMilliHertz = 144'000;
    CHECK(TopologyA.Update({DisplayA}) == DisplayTopologyUpdate::Unchanged);
    CHECK(ResolveRoamingEndpoint(
              Configuration.Links[0].EndpointA, Topologies).Endpoint
              ->TopologyGeneration == 1);
    DisplayA.Bounds.Right = 2560;
    CHECK(TopologyA.Update({DisplayA}) == DisplayTopologyUpdate::Changed);
    CHECK(ResolveRoamingEndpoint(
              Configuration.Links[0].EndpointA, Topologies).Endpoint
              ->TopologyGeneration == 2);
}

void RoamingRuntimeRequiresAuthorizedStableContext() {
    using namespace desklink;
    ManualClock Clock;
    RoamingRuntime Runtime(Clock);

    auto Context = MakeRoamingRuntimeContext();
    auto Update = Runtime.UpdateContext(Context);
    CHECK(Update.Valid);
    CHECK(!Update.MustFailLocal);
    CHECK(Update.ReadyRouteCount == 1);

    auto Invalid = Context;
    Invalid.PeerValidated = false;
    Update = Runtime.UpdateContext(Invalid);
    CHECK(!Update.Valid);
    CHECK(Update.ReadyRouteCount == 0);

    Invalid = Context;
    Invalid.InputCapabilityGranted = false;
    Update = Runtime.UpdateContext(Invalid);
    CHECK(Update.Valid);
    CHECK(Update.ReadyRouteCount == 0);

    Invalid = Context;
    Invalid.DirectionSupported = false;
    Update = Runtime.UpdateContext(Invalid);
    CHECK(Update.ReadyRouteCount == 0);

    Invalid = Context;
    Invalid.PeerStatus = PeerConnectionStatus::Reconnecting;
    Update = Runtime.UpdateContext(Invalid);
    CHECK(Update.ReadyRouteCount == 0);

    Invalid = Context;
    Invalid.TopologyStatus = DisplayTopologyExchangeStatus::TimedOut;
    Update = Runtime.UpdateContext(Invalid);
    CHECK(Update.ReadyRouteCount == 0);

    Invalid = Context;
    Invalid.SessionNonce = 0;
    Update = Runtime.UpdateContext(Invalid);
    CHECK(!Update.Valid);

    Invalid = Context;
    Invalid.Configuration.Links[0].EndpointB.SegmentStartPermyriad = 0;
    Invalid.Configuration.Links[0].EndpointB.SegmentEndPermyriad = 300;
    Update = Runtime.UpdateContext(Invalid);
    CHECK(Update.Valid);
    CHECK(Update.ReadyRouteCount == 0);
}

void RoamingRuntimeCrossingPoliciesAndAdmissionAreFailClosed() {
    using namespace desklink;
    constexpr LocalPointerObservation Edge{1919, 540, 8, 0};

    ManualClock PushClock;
    RoamingRuntime PushRuntime(PushClock);
    const auto PushContext = MakeRoamingRuntimeContext();
    CHECK(PushRuntime.UpdateContext(PushContext).ReadyRouteCount == 1);
    CHECK(!PushRuntime.Observe({1918, 540, 8, 0}));
    CHECK(PushRuntime.State() == RoamingRuntimeState::Local);
    CHECK(!PushRuntime.Observe({1919, 50, 8, 0}));
    CHECK(PushRuntime.State() == RoamingRuntimeState::Local);
    CHECK(!PushRuntime.Observe({1919, 540, 0, 7}));
    CHECK(PushRuntime.State() == RoamingRuntimeState::EdgeCandidate);
    CHECK(!PushRuntime.Observe({1919, 540, -1, 0}));
    CHECK(PushRuntime.State() == RoamingRuntimeState::Local);
    const auto Request = PushRuntime.Observe({1919, 540, 1, 24});
    CHECK(Request.has_value());
    CHECK(Request->PeerMachine == MakeMachineId(2));
    CHECK(Request->SessionNonce == PushContext.SessionNonce);
    CHECK(Request->Landing.display_id ==
          DeriveStableDisplayId("display-b"));
    CHECK(Request->Landing.normalized_x > 0);
    CHECK(Request->Landing.normalized_x < 400);
    CHECK(Request->Landing.normalized_y > 32'000);
    CHECK(Request->Landing.normalized_y < 33'500);
    CHECK(Request->LocalReturnLanding.ScreenX == 1'895);
    CHECK(Request->LocalReturnLanding.ScreenY == 540);
    CHECK(PushRuntime.State() == RoamingRuntimeState::FocusPending);

    auto Stale = *Request;
    ++Stale.SessionNonce;
    CHECK(!PushRuntime.AdmitFocusReady(Stale));
    CHECK(PushRuntime.State() == RoamingRuntimeState::LocalCooldown);

    ManualClock TimeoutClock;
    RoamingRuntime TimeoutRuntime(TimeoutClock);
    CHECK(TimeoutRuntime.UpdateContext(PushContext).ReadyRouteCount == 1);
    CHECK(TimeoutRuntime.Observe(Edge).has_value());
    TimeoutClock.advance(kRoamingFocusTimeout -
                         std::chrono::milliseconds(1));
    CHECK(!TimeoutRuntime.ExpireFocusPending());
    TimeoutClock.advance(std::chrono::milliseconds(1));
    CHECK(TimeoutRuntime.ExpireFocusPending());
    CHECK(TimeoutRuntime.State() == RoamingRuntimeState::LocalCooldown);

    ManualClock AdmissionClock;
    RoamingRuntime AdmissionRuntime(AdmissionClock);
    CHECK(AdmissionRuntime.UpdateContext(PushContext).ReadyRouteCount == 1);
    const auto Admitted = AdmissionRuntime.Observe(Edge);
    CHECK(Admitted.has_value());
    CHECK(AdmissionRuntime.AdmitFocusReady(*Admitted));
    CHECK(AdmissionRuntime.State() == RoamingRuntimeState::RemoteReady);
    CHECK(AdmissionRuntime.AdmitRemoteInput(*Admitted));
    CHECK(AdmissionRuntime.State() == RoamingRuntimeState::Remote);
    const PointerPositionFeedbackMessage WrongDisplay{
        static_cast<std::uint16_t>(Admitted->Landing.display_id + 1u),
        0, 32'768};
    CHECK(!AdmissionRuntime.ObserveRemotePointer(WrongDisplay));
    const PointerPositionFeedbackMessage ReturnEdge{
        Admitted->Landing.display_id, 0, 32'768};
    CHECK(AdmissionRuntime.ObserveRemotePointer(ReturnEdge));
    AdmissionRuntime.BeginReturn();
    AdmissionRuntime.ReturnLocal();
    CHECK(AdmissionRuntime.State() == RoamingRuntimeState::LocalCooldown);
    CHECK(!AdmissionRuntime.ConfirmLocalReturnLanding({1'919, 540}));
    CHECK(AdmissionRuntime.State() == RoamingRuntimeState::LocalCooldown);
    CHECK(AdmissionRuntime.ConfirmLocalReturnLanding(
        Admitted->LocalReturnLanding));
    CHECK(AdmissionRuntime.State() == RoamingRuntimeState::Local);

    ManualClock DwellClock;
    RoamingRuntime DwellRuntime(DwellClock);
    CHECK(DwellRuntime.UpdateContext(
              MakeRoamingRuntimeContext(CrossingPolicy::DwellAndPush))
              .ReadyRouteCount == 1);
    CHECK(!DwellRuntime.Observe(Edge));
    DwellClock.advance(std::chrono::milliseconds(99));
    CHECK(!DwellRuntime.Observe({1919, 540, 0, 0}));
    DwellClock.advance(std::chrono::milliseconds(1));
    CHECK(DwellRuntime.Observe({1919, 540, 0, 0}).has_value());

    ManualClock DoubleClock;
    RoamingRuntime DoubleRuntime(DoubleClock);
    CHECK(DoubleRuntime.UpdateContext(
              MakeRoamingRuntimeContext(CrossingPolicy::DoublePush))
              .ReadyRouteCount == 1);
    CHECK(!DoubleRuntime.Observe(Edge));
    CHECK(!DoubleRuntime.Observe({1919, 540, -1, 0}));
    CHECK(DoubleRuntime.Observe(Edge).has_value());

    ManualClock ExpiredClock;
    RoamingRuntime ExpiredRuntime(ExpiredClock);
    CHECK(ExpiredRuntime.UpdateContext(
              MakeRoamingRuntimeContext(CrossingPolicy::DoublePush))
              .ReadyRouteCount == 1);
    CHECK(!ExpiredRuntime.Observe(Edge));
    ExpiredClock.advance(std::chrono::milliseconds(501));
    CHECK(!ExpiredRuntime.Observe({1919, 540, -1, 0}));
    CHECK(ExpiredRuntime.State() == RoamingRuntimeState::Local);

    ManualClock VerticalClock;
    RoamingRuntime VerticalRuntime(VerticalClock);
    auto VerticalContext = MakeRoamingRuntimeContext();
    auto& VerticalLink = VerticalContext.Configuration.Links[0];
    VerticalLink.EndpointA.Side = DisplayEdgeSide::Bottom;
    VerticalLink.EndpointA.SegmentStartPermyriad = 0;
    VerticalLink.EndpointA.SegmentEndPermyriad = 10'000;
    VerticalLink.EndpointB.Side = DisplayEdgeSide::Top;
    VerticalLink.EndpointB.SegmentStartPermyriad = 0;
    VerticalLink.EndpointB.SegmentEndPermyriad = 10'000;
    CHECK(VerticalRuntime.UpdateContext(VerticalContext).ReadyRouteCount == 1);
    const auto VerticalRequest = VerticalRuntime.Observe(
        {960, 1079, 0, 8});
    CHECK(VerticalRequest.has_value());
    CHECK(VerticalRequest->Landing.normalized_x > 32'000);
    CHECK(VerticalRequest->Landing.normalized_x < 33'500);
    CHECK(VerticalRequest->Landing.normalized_y > 0);
    CHECK(VerticalRequest->Landing.normalized_y < 700);

    ManualClock ReverseClock;
    RoamingRuntime ReverseRuntime(ReverseClock);
    auto ReverseContext = MakeRoamingRuntimeContext();
    std::swap(ReverseContext.LocalMachine, ReverseContext.PeerMachine);
    std::swap(ReverseContext.LocalTopology, ReverseContext.PeerTopology);
    ReverseContext.Configuration.Links[0].Direction =
        RoamingDirectionMode::BToA;
    ReverseContext.Configuration.Links[0].BToA = {
        CrossingPolicy::Push, 8, 100, 500};
    CHECK(ReverseRuntime.UpdateContext(ReverseContext).ReadyRouteCount == 1);
    const auto ReverseRequest = ReverseRuntime.Observe({0, 720, -8, 0});
    CHECK(ReverseRequest.has_value());
    CHECK(ReverseRequest->PeerMachine == MakeMachineId(1));
    CHECK(ReverseRequest->Landing.display_id ==
          DeriveStableDisplayId("display-a"));
    CHECK(ReverseRequest->Landing.normalized_x > 65'000);
    CHECK(ReverseRequest->LocalReturnLanding.ScreenX == 24);
    CHECK(ReverseRequest->LocalReturnLanding.ScreenY == 720);
}

void RoamingRuntimeInvalidatesActiveRoutesAndEnforcesCooldown() {
    using namespace desklink;
    constexpr LocalPointerObservation Edge{1919, 540, 8, 0};
    ManualClock Clock;
    RoamingRuntime Runtime(Clock);
    auto Context = MakeRoamingRuntimeContext();
    CHECK(Runtime.UpdateContext(Context).ReadyRouteCount == 1);
    const auto Request = Runtime.Observe(Edge);
    CHECK(Request.has_value());

    auto PresentationOnly = Context;
    PresentationOnly.Configuration.CanvasLayout[0].X += 50'000;
    const auto PresentationUpdate = Runtime.UpdateContext(PresentationOnly);
    CHECK(!PresentationUpdate.MustFailLocal);
    CHECK(Runtime.State() == RoamingRuntimeState::FocusPending);

    auto Mutated = PresentationOnly;
    ++Mutated.Configuration.Links[0].LandingInsetPixels;
    const auto MutationUpdate = Runtime.UpdateContext(Mutated);
    CHECK(MutationUpdate.MustFailLocal);
    CHECK(Runtime.State() == RoamingRuntimeState::LocalCooldown);

    CHECK(!Runtime.Observe({1919, 540, -23, 0}));
    CHECK(Runtime.State() == RoamingRuntimeState::LocalCooldown);
    CHECK(!Runtime.Observe({1919, 540, -1, 0}));
    CHECK(Runtime.State() == RoamingRuntimeState::Local);

    RoamingRuntime TopologyRuntime(Clock);
    CHECK(TopologyRuntime.UpdateContext(Context).ReadyRouteCount == 1);
    CHECK(TopologyRuntime.Observe(Edge).has_value());
    ++Context.PeerTopology->Generation;
    const auto TopologyUpdate = TopologyRuntime.UpdateContext(Context);
    CHECK(TopologyUpdate.MustFailLocal);
    CHECK(TopologyRuntime.State() == RoamingRuntimeState::LocalCooldown);

    RoamingRuntime LocalTopologyRuntime(Clock);
    Context = MakeRoamingRuntimeContext();
    CHECK(LocalTopologyRuntime.UpdateContext(Context).ReadyRouteCount == 1);
    CHECK(LocalTopologyRuntime.Observe(Edge).has_value());
    ++Context.LocalTopology->Generation;
    const auto LocalTopologyUpdate =
        LocalTopologyRuntime.UpdateContext(Context);
    CHECK(LocalTopologyUpdate.MustFailLocal);
    CHECK(LocalTopologyRuntime.State() ==
          RoamingRuntimeState::LocalCooldown);

    RoamingRuntime NonceRuntime(Clock);
    Context = MakeRoamingRuntimeContext();
    CHECK(NonceRuntime.UpdateContext(Context).ReadyRouteCount == 1);
    CHECK(NonceRuntime.Observe(Edge).has_value());
    ++Context.SessionNonce;
    const auto NonceUpdate = NonceRuntime.UpdateContext(Context);
    CHECK(NonceUpdate.MustFailLocal);

    const auto ExpectInvalidation = [&Clock](auto Mutate) {
        RoamingRuntime CheckedRuntime(Clock);
        auto CheckedContext = MakeRoamingRuntimeContext();
        CHECK(CheckedRuntime.UpdateContext(CheckedContext).ReadyRouteCount == 1);
        CHECK(CheckedRuntime.Observe({1919, 540, 8, 0}).has_value());
        Mutate(CheckedContext);
        const auto CheckedUpdate =
            CheckedRuntime.UpdateContext(std::move(CheckedContext));
        CHECK(CheckedUpdate.MustFailLocal);
        CHECK(CheckedRuntime.State() ==
              RoamingRuntimeState::LocalCooldown);
    };
    ExpectInvalidation([](RoamingRuntimeContext& Checked) {
        Checked.PeerValidated = false;
    });
    ExpectInvalidation([](RoamingRuntimeContext& Checked) {
        Checked.InputCapabilityGranted = false;
    });
    ExpectInvalidation([](RoamingRuntimeContext& Checked) {
        Checked.TopologyStatus = DisplayTopologyExchangeStatus::TimedOut;
    });
    ExpectInvalidation([](RoamingRuntimeContext& Checked) {
        Checked.Configuration.Links[0].Enabled = false;
    });

    RoamingRuntime InvalidCooldownRuntime(Clock);
    Context = MakeRoamingRuntimeContext();
    CHECK(InvalidCooldownRuntime.UpdateContext(Context).ReadyRouteCount == 1);
    CHECK(InvalidCooldownRuntime.Observe(Edge).has_value());
    Context.PeerValidated = false;
    CHECK(InvalidCooldownRuntime.UpdateContext(Context).MustFailLocal);
    CHECK(InvalidCooldownRuntime.State() ==
          RoamingRuntimeState::LocalCooldown);
    CHECK(!InvalidCooldownRuntime.Observe({1919, 540, 512, 0}));
    CHECK(InvalidCooldownRuntime.State() ==
          RoamingRuntimeState::LocalCooldown);
    CHECK(!InvalidCooldownRuntime.Observe({1919, 540, -24, 0}));
    CHECK(InvalidCooldownRuntime.State() == RoamingRuntimeState::Local);

    RoamingRuntime CandidateRuntime(Clock);
    Context = MakeRoamingRuntimeContext();
    CHECK(CandidateRuntime.UpdateContext(Context).ReadyRouteCount == 1);
    CHECK(!CandidateRuntime.Observe({1919, 540, 0, 3}));
    CHECK(CandidateRuntime.State() == RoamingRuntimeState::EdgeCandidate);
    ++Context.SessionNonce;
    CHECK(!CandidateRuntime.UpdateContext(Context).MustFailLocal);
    CHECK(CandidateRuntime.State() == RoamingRuntimeState::Local);
}

void RoamingRuntimeHandlesExtremeLocalPointerDeltas() {
    using namespace desklink;

    ManualClock Clock;
    RoamingRuntime AccumulationRuntime(Clock);
    auto Context = MakeRoamingRuntimeContext(CrossingPolicy::DwellAndPush);
    Context.Configuration.Links[0].AToB.DwellMilliseconds = 0;
    CHECK(AccumulationRuntime.UpdateContext(Context).ReadyRouteCount == 1);
    CHECK(!AccumulationRuntime.Observe({1919, 540, 3, 0}));
    const auto Accumulated = AccumulationRuntime.Observe({
        1919, 540, std::numeric_limits<std::int32_t>::max(), 0});
    CHECK(Accumulated.has_value());

    auto Stale = *Accumulated;
    ++Stale.SessionNonce;
    CHECK(!AccumulationRuntime.AdmitFocusReady(Stale));
    CHECK(AccumulationRuntime.State() ==
          RoamingRuntimeState::LocalCooldown);
    CHECK(!AccumulationRuntime.Observe({
        1919, 540, std::numeric_limits<std::int32_t>::min(), 0}));
    CHECK(AccumulationRuntime.State() == RoamingRuntimeState::Local);

    RoamingRuntime NegationRuntime(Clock);
    Context = MakeRoamingRuntimeContext();
    Context.Configuration.Links[0].EndpointA.Side =
        DisplayEdgeSide::Left;
    Context.Configuration.Links[0].EndpointB.Side =
        DisplayEdgeSide::Right;
    CHECK(NegationRuntime.UpdateContext(Context).ReadyRouteCount == 1);
    CHECK(NegationRuntime.Observe({
        0, 540, std::numeric_limits<std::int32_t>::min(), 0})
              .has_value());
}

void RoamingRuntimeUsesPhysicalLandingHintsOnlyWhenTrustworthy() {
    using namespace desklink;

    const auto BuildContext = [] {
        auto Context = MakeRoamingRuntimeContext();
        auto& Link = Context.Configuration.Links[0];
        Link.EndpointA.SegmentStartPermyriad = 0;
        Link.EndpointA.SegmentEndPermyriad = 5'000;
        Link.EndpointB.SegmentStartPermyriad = 0;
        Link.EndpointB.SegmentEndPermyriad = 7'000;
        Context.LocalTopology = MakePhysicalDisplayTopology(
            "display-a", "Local physical", {0, 0, 1'000, 1'000},
            {600, 400});
        Context.PeerTopology = MakePhysicalDisplayTopology(
            "display-b", "Peer physical", {0, 0, 1'000, 2'000},
            {500, 300});
        return Context;
    };
    const auto ObserveLanding = [](RoamingRuntimeContext Context) {
        ManualClock Clock;
        RoamingRuntime Runtime(Clock);
        CHECK(Runtime.UpdateContext(std::move(Context)).ReadyRouteCount == 1);
        const auto Request = Runtime.Observe({999, 250, 8, 0});
        CHECK(Request.has_value());
        return Request->Landing.normalized_y;
    };

    const auto PhysicalLanding = ObserveLanding(BuildContext());
    CHECK(PhysicalLanding > 21'700);
    CHECK(PhysicalLanding < 22'000);

    auto Estimated = BuildContext();
    Estimated.PeerTopology->Displays[0].PhysicalSize =
        PhysicalSizeSource::RawDpiEstimate;
    const auto EstimatedFallback = ObserveLanding(std::move(Estimated));
    CHECK(EstimatedFallback > 22'800);
    CHECK(EstimatedFallback < 23'100);

    auto Rotated = BuildContext();
    Rotated.PeerTopology->Displays[0].Orientation =
        DisplayOrientation::Portrait;
    CHECK(ObserveLanding(std::move(Rotated)) == EstimatedFallback);

    auto Contradictory = BuildContext();
    Contradictory.PeerTopology->Displays[0].PhysicalHeightMillimeters = 500;
    CHECK(ObserveLanding(std::move(Contradictory)) == EstimatedFallback);

    auto TooShort = BuildContext();
    TooShort.LocalTopology->Displays[0].PhysicalHeightMillimeters = 40;
    TooShort.PeerTopology->Displays[0].PhysicalHeightMillimeters = 30;
    CHECK(ObserveLanding(std::move(TooShort)) == EstimatedFallback);
}

void RoamingRuntimeRecordedTracesRespectCrossingPolicies() {
    using namespace desklink;

    ManualClock SkimClock;
    RoamingRuntime SkimRuntime(SkimClock);
    CHECK(SkimRuntime.UpdateContext(MakeRoamingRuntimeContext())
              .ReadyRouteCount == 1);
    const std::array SkimTrace{
        RecordedPointerSample{0, {1'919, 500, 0, 6}},
        RecordedPointerSample{1, {1'919, 506, 0, 6}},
        RecordedPointerSample{1, {1'919, 512, 0, 6}},
        RecordedPointerSample{1, {1'919, 518, 0, 6}},
        RecordedPointerSample{1, {1'919, 524, 0, 6}},
        RecordedPointerSample{1, {1'919, 530, 0, 6}},
    };
    CHECK(ReplayRoamingTrace(SkimRuntime, SkimClock, SkimTrace).empty());
    CHECK(SkimRuntime.State() != RoamingRuntimeState::FocusPending);

    ManualClock PollClock;
    RoamingRuntime PollRuntime(PollClock);
    CHECK(PollRuntime.UpdateContext(MakeRoamingRuntimeContext())
              .ReadyRouteCount == 1);
    const std::array FirstContactTrace{
        RecordedPointerSample{0, {1'919, 540, 1, 0}},
    };
    CHECK(ReplayRoamingTrace(
              PollRuntime, PollClock, FirstContactTrace).size() == 1);

    ManualClock DiagonalClock;
    RoamingRuntime DiagonalRuntime(DiagonalClock);
    CHECK(DiagonalRuntime.UpdateContext(MakeRoamingRuntimeContext())
              .ReadyRouteCount == 1);
    const std::array DiagonalTrace{
        RecordedPointerSample{0, {1'919, 540, 1, 12}},
    };
    CHECK(ReplayRoamingTrace(
              DiagonalRuntime, DiagonalClock, DiagonalTrace).size() == 1);

    ManualClock VelocityClock;
    RoamingRuntime VelocityRuntime(VelocityClock);
    CHECK(VelocityRuntime.UpdateContext(MakeRoamingRuntimeContext())
              .ReadyRouteCount == 1);
    const std::array VelocityTrace{
        RecordedPointerSample{0, {1'919, 540, 512, 2}},
    };
    CHECK(ReplayRoamingTrace(
              VelocityRuntime, VelocityClock, VelocityTrace).size() == 1);

    ManualClock PartialClock;
    RoamingRuntime PartialRuntime(PartialClock);
    CHECK(PartialRuntime.UpdateContext(MakeRoamingRuntimeContext())
              .ReadyRouteCount == 1);
    const std::array PartialEdgeTrace{
        RecordedPointerSample{0, {1'919, 50, 512, 0}},
        RecordedPointerSample{1, {1'919, 100, 512, 0}},
    };
    CHECK(ReplayRoamingTrace(
              PartialRuntime, PartialClock, PartialEdgeTrace).empty());

    ManualClock DwellClock;
    RoamingRuntime DwellRuntime(DwellClock);
    CHECK(DwellRuntime.UpdateContext(
              MakeRoamingRuntimeContext(CrossingPolicy::DwellAndPush))
              .ReadyRouteCount == 1);
    const std::array DwellTrace{
        RecordedPointerSample{0, {1'919, 540, 8, 0}},
        RecordedPointerSample{99, {1'919, 540, 0, 0}},
        RecordedPointerSample{1, {1'919, 540, 0, 0}},
    };
    CHECK(ReplayRoamingTrace(
              DwellRuntime, DwellClock, DwellTrace).size() == 1);

    ManualClock DoubleClock;
    RoamingRuntime DoubleRuntime(DoubleClock);
    CHECK(DoubleRuntime.UpdateContext(
              MakeRoamingRuntimeContext(CrossingPolicy::DoublePush))
              .ReadyRouteCount == 1);
    const std::array DoublePushTrace{
        RecordedPointerSample{0, {1'919, 540, 8, 0}},
        RecordedPointerSample{1, {1'919, 540, -1, 0}},
        RecordedPointerSample{1, {1'919, 540, 8, 0}},
    };
    CHECK(ReplayRoamingTrace(
              DoubleRuntime, DoubleClock, DoublePushTrace).size() == 1);

    ManualClock CooldownClock;
    RoamingRuntime CooldownRuntime(CooldownClock);
    CHECK(CooldownRuntime.UpdateContext(MakeRoamingRuntimeContext())
              .ReadyRouteCount == 1);
    const auto First = CooldownRuntime.Observe({1'919, 540, 8, 0});
    CHECK(First.has_value());
    CHECK(CooldownRuntime.AdmitFocusReady(*First));
    CHECK(CooldownRuntime.AdmitRemoteInput(*First));
    CooldownRuntime.ReturnLocal();
    CHECK(CooldownRuntime.State() == RoamingRuntimeState::LocalCooldown);
    CHECK(!CooldownRuntime.Observe({1'919, 540, 512, 0}));
    CHECK(CooldownRuntime.State() == RoamingRuntimeState::LocalCooldown);
    CHECK(!CooldownRuntime.Observe({1'919, 540, -24, 0}));
    CHECK(CooldownRuntime.State() == RoamingRuntimeState::Local);
    CHECK(CooldownRuntime.Observe({1'919, 540, 8, 0}).has_value());
}

void PeerDirectionArbiterRejectsCollisionsAndStaleTokens() {
    using namespace desklink;
    PeerDirectionArbiter Arbiter;
    const auto Peer = MakeMachineId(91);
    CHECK(Arbiter.BeginOutgoing().Outcome ==
          PeerDirectionOutcome::RejectedUnbound);
    CHECK(!Arbiter.BindSession({}, 5));
    CHECK(!Arbiter.BindSession(Peer, 0));
    CHECK(Arbiter.BindSession(Peer, 5));
    CHECK(Arbiter.BoundTo(Peer, 5));
    const auto Outgoing = Arbiter.BeginOutgoing();
    CHECK(Outgoing.Outcome == PeerDirectionOutcome::Admitted);
    CHECK(Outgoing.Token.has_value());
    CHECK(Arbiter.State() == PeerDirectionState::OutgoingPending);

    const auto Collision = Arbiter.BeginIncoming();
    CHECK(Collision.Outcome == PeerDirectionOutcome::CollisionFailLocal);
    CHECK(!Collision.Token);
    CHECK(Arbiter.State() == PeerDirectionState::Local);
    CHECK(!Arbiter.AdmitOutgoing(*Outgoing.Token));

    const auto Incoming = Arbiter.BeginIncoming();
    CHECK(Incoming.Outcome == PeerDirectionOutcome::Admitted);
    CHECK(Incoming.Token.has_value());
    CHECK(Arbiter.State() == PeerDirectionState::IncomingActive);
    CHECK(Arbiter.BeginOutgoing().Outcome ==
          PeerDirectionOutcome::RejectedBusy);
    CHECK(Arbiter.Release(*Incoming.Token));
    CHECK(Arbiter.State() == PeerDirectionState::Local);
    CHECK(!Arbiter.Release(*Incoming.Token));

    const auto Active = Arbiter.BeginOutgoing();
    CHECK(Active.Token.has_value());
    CHECK(Arbiter.AdmitOutgoing(*Active.Token));
    CHECK(Arbiter.State() == PeerDirectionState::OutgoingActive);
    CHECK(Arbiter.Release(*Active.Token));

    const auto ReconnectToken = Arbiter.BeginOutgoing();
    CHECK(ReconnectToken.Token.has_value());
    Arbiter.ResetSession();
    CHECK(!Arbiter.BoundTo(Peer, 5));
    CHECK(!Arbiter.AdmitOutgoing(*ReconnectToken.Token));
    CHECK(Arbiter.BindSession(Peer, 6));
    CHECK(!Arbiter.AdmitOutgoing(*ReconnectToken.Token));
    const auto Fresh = Arbiter.BeginOutgoing();
    CHECK(Fresh.Token.has_value());
    CHECK(Fresh.Token->PeerMachine == Peer);
    CHECK(Fresh.Token->SessionNonce == 6);
    CHECK(Arbiter.AdmitOutgoing(*Fresh.Token));
    auto Forged = *Fresh.Token;
    Forged.SessionNonce = 5;
    CHECK(!Arbiter.Release(Forged));
    CHECK(Arbiter.Release(*Fresh.Token));
}

void PeerSessionSupportsReciprocalFocusAndIndependentGrants() {
    using namespace desklink;
    constexpr std::uint64_t Nonce = 0x71'82'93u;
    const auto IdentityA = MakeIdentity(101, "Peer A");
    const auto IdentityB = MakeIdentity(102, "Peer B");
    TransportPeerInfo AViewOfB{IdentityB, true, true};
    TransportPeerInfo BViewOfA{IdentityA, true, true};
    auto Pair = make_in_memory_transport_pair(AViewOfB, BViewOfA);

    CapabilitySet GrantedToA;
    GrantedToA.grant(Capability::InputInject);
    GrantedToA.grant(Capability::DisplayTopologyExchange);
    CapabilitySet GrantedToB;
    GrantedToB.grant(Capability::InputInject);
    GrantedToB.grant(Capability::DisplayTopologyExchange);
    InMemoryTrustStore TrustA;
    InMemoryTrustStore TrustB;
    SaveTrustedPeer(TrustA, IdentityB, GrantedToB);
    SaveTrustedPeer(TrustB, IdentityA, GrantedToA);

    ManualClock Clock;
    RecordingInjector InjectorA;
    RecordingInjector InjectorB;
    AgentCoordinator IncomingA(Clock, InjectorA);
    AgentCoordinator IncomingB(Clock, InjectorB);
    HostCoordinator OutgoingA(Nonce);
    HostCoordinator OutgoingB(Nonce);
    std::size_t ReadyA{};
    std::size_t ReadyB{};
    std::size_t DirectionChanges{};
    std::vector<PointerPositionFeedbackMessage> FeedbackA;
    PeerSession SessionA(
        Pair.a, OutgoingA, IncomingA, TrustA, Nonce,
        PeerSessionHandlers{
            [&] { ++ReadyA; },
            [&] { ++DirectionChanges; },
            {}, {},
            [&](PointerPositionFeedbackMessage Position) {
                FeedbackA.push_back(Position);
            }});
    PeerSession SessionB(
        Pair.b, OutgoingB, IncomingB, TrustB, Nonce,
        PeerSessionHandlers{
            [&] { ++ReadyB; },
            [&] { ++DirectionChanges; },
            {}});
    CHECK(SessionA.Start());
    CHECK(SessionB.Start());
    CHECK(SessionA.PeerGrantedCapability(Capability::InputInject));
    CHECK(SessionB.PeerGrantedCapability(Capability::InputInject));
    CHECK(SessionA.GrantedToPeer(Capability::InputInject));
    CHECK(SessionB.GrantedToPeer(Capability::InputInject));
    CHECK(DirectionChanges >= 2);

    CHECK(SessionA.BeginOutgoingFocus(750));
    CHECK(ReadyA == 1);
    CHECK(SessionA.OutgoingFocused());
    CHECK(SessionB.IncomingFocused());
    CHECK(SessionA.DirectionState() ==
          PeerDirectionState::OutgoingActive);
    CHECK(SessionB.DirectionState() ==
          PeerDirectionState::IncomingActive);
    CHECK(!SessionB.BeginOutgoingFocus(750));
    CHECK(SessionA.SendKey(KeyEventMessage{0x30, false, true}));
    CHECK(SessionA.SendButton(
        MouseButtonMessage{MouseButtonId::Left, true}));
    InjectorB.CurrentPointer = PointerPositionMessage{7, 0, 32'768};
    CHECK(SessionA.SendPointerMotion(PointerMotionMessage{4, -2}));
    CHECK(InjectorB.keys.size() == 1);
    CHECK(InjectorB.buttons.size() == 1);
    CHECK(InjectorB.motions.size() == 1);
    CHECK(FeedbackA.size() == 1);
    CHECK(FeedbackA.front().DisplayId == 7);
    CHECK(FeedbackA.front().NormalizedX == 0);
    CHECK(SessionA.ReleaseOutgoingFocus());
    CHECK(SessionA.DirectionState() == PeerDirectionState::Local);
    CHECK(SessionB.DirectionState() == PeerDirectionState::Local);
    CHECK(InjectorB.release_calls == 1);
    CHECK(InjectorB.park_calls == 1);

    CHECK(SessionB.BeginOutgoingFocus(750));
    CHECK(ReadyB == 1);
    CHECK(SessionB.SendWheel(
        MouseWheelMessage{MouseWheelAxis::Vertical, 120}));
    CHECK(InjectorA.wheels.size() == 1);
    CHECK(SessionB.SetDesiredMode(DeskMode::Game));
    CHECK(SessionA.DirectionState() == PeerDirectionState::Local);
    CHECK(SessionB.DirectionState() == PeerDirectionState::Local);
    CHECK(InjectorA.release_calls == 1);
    CHECK(DirectionChanges >= 6);
    CHECK(SessionA.Stats().CapabilityGrantsReceived >= 1);
    CHECK(SessionB.Stats().CapabilityGrantsReceived >= 1);

    CHECK(SessionB.SetDesiredMode(DeskMode::Roam));
    CHECK(SessionB.BeginOutgoingFocus(750));
    CHECK(SessionA.IncomingFocused());
    CHECK(SessionB.SendKey(KeyEventMessage{0x31, false, true}));
    CHECK(SessionB.SendButton(
        MouseButtonMessage{MouseButtonId::Right, true}));
    const auto ReleasesBeforeClose = InjectorA.release_calls;
    Pair.b->close();
    CHECK(SessionA.DirectionState() == PeerDirectionState::Local);
    CHECK(!SessionA.IncomingFocused());
    CHECK(InjectorA.release_calls == ReleasesBeforeClose + 1);
}

void PeerSessionImmediatelyReacquiresAfterLostRelease() {
    using namespace desklink;
    constexpr std::uint64_t Nonce = 0x71'82'94u;
    const auto IdentityA = MakeIdentity(103, "Reacquire A");
    const auto IdentityB = MakeIdentity(104, "Reacquire B");
    auto Pair = MakePausableTransportPair(
        TransportPeerInfo{IdentityB, true, true},
        TransportPeerInfo{IdentityA, true, true});

    CapabilitySet InputCapability;
    InputCapability.grant(Capability::InputInject);
    InMemoryTrustStore TrustA;
    InMemoryTrustStore TrustB;
    SaveTrustedPeer(TrustA, IdentityB, InputCapability);
    SaveTrustedPeer(TrustB, IdentityA, InputCapability);

    ManualClock Clock;
    RecordingInjector InjectorA;
    RecordingInjector InjectorB;
    AgentCoordinator IncomingA(Clock, InjectorA);
    AgentCoordinator IncomingB(Clock, InjectorB);
    HostCoordinator OutgoingA(Nonce);
    HostCoordinator OutgoingB(Nonce);
    std::size_t ReadyA{};
    PeerSession SessionA(
        Pair.A, OutgoingA, IncomingA, TrustA, Nonce,
        PeerSessionHandlers{[&] { ++ReadyA; }});
    PeerSession SessionB(
        Pair.B, OutgoingB, IncomingB, TrustB, Nonce);
    CHECK(SessionA.Start());
    CHECK(SessionB.Start());

    CHECK(SessionA.BeginOutgoingFocus());
    CHECK(ReadyA == 1);
    CHECK(SessionB.IncomingFocused());
    CHECK(SessionA.SendKey(KeyEventMessage{0x30, false, true}));

    Pair.A->DropNextReliable(MessageType::FocusRelease);
    CHECK(SessionA.ReleaseOutgoingFocus());
    CHECK(SessionA.DirectionState() == PeerDirectionState::Local);
    CHECK(SessionB.DirectionState() ==
          PeerDirectionState::IncomingActive);

    CHECK(SessionA.BeginOutgoingFocus());
    CHECK(ReadyA == 2);
    CHECK(SessionA.DirectionState() ==
          PeerDirectionState::OutgoingActive);
    CHECK(SessionB.DirectionState() ==
          PeerDirectionState::IncomingActive);
    CHECK(InjectorB.release_calls == 1);
    CHECK(InjectorB.park_calls == 0);
    InjectorB.ParkSucceeds = false;
    CHECK(SessionA.ReleaseOutgoingFocus());
    CHECK(SessionA.DirectionState() == PeerDirectionState::Local);
    CHECK(SessionB.DirectionState() == PeerDirectionState::Local);
    CHECK(InjectorB.release_calls == 2);
    CHECK(InjectorB.park_calls == 1);
}

void PeerSessionRenegotiatesCapabilitiesWithoutDisconnecting() {
    using namespace desklink;
    constexpr std::uint64_t Nonce = 0x72'83'94u;
    const auto IdentityA = MakeIdentity(113, "Renegotiation A");
    const auto IdentityB = MakeIdentity(114, "Renegotiation B");
    auto Pair = make_in_memory_transport_pair(
        TransportPeerInfo{IdentityB, true, true},
        TransportPeerInfo{IdentityA, true, true});

    CapabilitySet InputCapability;
    InputCapability.grant(Capability::InputInject);
    InMemoryTrustStore TrustA;
    InMemoryTrustStore TrustB;
    SaveTrustedPeer(TrustA, IdentityB, InputCapability);
    SaveTrustedPeer(TrustB, IdentityA, InputCapability);
    ManualClock Clock;
    RecordingInjector InjectorA;
    RecordingInjector InjectorB;
    AgentCoordinator IncomingA(Clock, InjectorA);
    AgentCoordinator IncomingB(Clock, InjectorB);
    HostCoordinator OutgoingA(Nonce);
    HostCoordinator OutgoingB(Nonce);
    PeerSession SessionA(
        Pair.a, OutgoingA, IncomingA, TrustA, Nonce);
    PeerSession SessionB(
        Pair.b, OutgoingB, IncomingB, TrustB, Nonce);
    CHECK(SessionA.Start());
    CHECK(SessionB.Start());
    CHECK(SessionB.BeginOutgoingFocus());
    CHECK(SessionB.OutgoingFocused());
    CHECK(SessionA.IncomingFocused());

    SaveTrustedPeer(TrustA, IdentityB, CapabilitySet{});
    CHECK(SessionA.RefreshLocalCapabilities());
    CHECK(SessionA.DirectionState() == PeerDirectionState::Local);
    CHECK(SessionB.DirectionState() == PeerDirectionState::Local);
    CHECK(!SessionB.PeerGrantedCapability(Capability::InputInject));
    CHECK(!SessionB.BeginOutgoingFocus());
    CHECK(SessionA.PeerGrantedCapability(Capability::InputInject));
    CHECK(SessionA.BeginOutgoingFocus());
    CHECK(SessionA.OutgoingFocused());
    CHECK(SessionA.ReleaseOutgoingFocus());

    SaveTrustedPeer(TrustA, IdentityB, InputCapability);
    CHECK(SessionA.RefreshLocalCapabilities());
    CHECK(SessionB.PeerGrantedCapability(Capability::InputInject));
    CHECK(SessionB.BeginOutgoingFocus());
    CHECK(SessionB.OutgoingFocused());
    CHECK(SessionB.ReleaseOutgoingFocus());
    CHECK(SessionA.Stats().CapabilityGrantsSent >= 3);
    CHECK(SessionA.Stats().CapabilityGrantAcksReceived >= 3);
    CHECK(SessionB.Stats().CapabilityGrantsReceived >= 3);
    CHECK(SessionB.Stats().CapabilityGrantAcksSent >= 3);
}

void PeerSessionResolvesSimultaneousFocusToLocal() {
    using namespace desklink;
    constexpr std::uint64_t Nonce = 0x44'55'66u;
    const auto IdentityA = MakeIdentity(103, "Collision A");
    const auto IdentityB = MakeIdentity(104, "Collision B");
    auto Pair = MakePausableTransportPair(
        TransportPeerInfo{IdentityB, true, true},
        TransportPeerInfo{IdentityA, true, true});
    CapabilitySet InputCapability;
    InputCapability.grant(Capability::InputInject);
    InMemoryTrustStore TrustA;
    InMemoryTrustStore TrustB;
    SaveTrustedPeer(TrustA, IdentityB, InputCapability);
    SaveTrustedPeer(TrustB, IdentityA, InputCapability);
    ManualClock Clock;
    RecordingInjector InjectorA;
    RecordingInjector InjectorB;
    AgentCoordinator IncomingA(Clock, InjectorA);
    AgentCoordinator IncomingB(Clock, InjectorB);
    HostCoordinator OutgoingA(Nonce);
    HostCoordinator OutgoingB(Nonce);
    std::size_t CollisionsA{};
    std::size_t CollisionsB{};
    PeerSession SessionA(
        Pair.A, OutgoingA, IncomingA, TrustA, Nonce,
        PeerSessionHandlers{{}, {}, [&] { ++CollisionsA; }});
    PeerSession SessionB(
        Pair.B, OutgoingB, IncomingB, TrustB, Nonce,
        PeerSessionHandlers{{}, {}, [&] { ++CollisionsB; }});
    CHECK(SessionA.Start());
    CHECK(SessionB.Start());
    CHECK(SessionA.CanBeginOutgoing());
    CHECK(SessionB.CanBeginOutgoing());

    Pair.A->PauseReliable(true);
    Pair.B->PauseReliable(true);
    CHECK(SessionA.BeginOutgoingFocus(750));
    CHECK(SessionB.BeginOutgoingFocus(750));
    CHECK(SessionA.DirectionState() ==
          PeerDirectionState::OutgoingPending);
    CHECK(SessionB.DirectionState() ==
          PeerDirectionState::OutgoingPending);
    CHECK(Pair.A->FlushOneReliable());
    CHECK(Pair.B->FlushOneReliable());
    CHECK(SessionA.DirectionState() == PeerDirectionState::Local);
    CHECK(SessionB.DirectionState() == PeerDirectionState::Local);
    CHECK(!SessionA.OutgoingFocused());
    CHECK(!SessionB.OutgoingFocused());
    CHECK(CollisionsA == 1);
    CHECK(CollisionsB == 1);
    CHECK(SessionA.Stats().DirectionCollisions == 1);
    CHECK(SessionB.Stats().DirectionCollisions == 1);
}

void PeerSessionRequiresTheRemoteDirectionalGrant() {
    using namespace desklink;
    constexpr std::uint64_t Nonce = 0x99'88'77u;
    const auto IdentityA = MakeIdentity(105, "Grant A");
    const auto IdentityB = MakeIdentity(106, "Grant B");
    auto Pair = make_in_memory_transport_pair(
        TransportPeerInfo{IdentityB, true, true},
        TransportPeerInfo{IdentityA, true, true});
    CapabilitySet GrantedToB;
    GrantedToB.grant(Capability::InputInject);
    InMemoryTrustStore TrustA;
    InMemoryTrustStore TrustB;
    SaveTrustedPeer(TrustA, IdentityB, GrantedToB);
    SaveTrustedPeer(TrustB, IdentityA, CapabilitySet{});
    ManualClock Clock;
    RecordingInjector InjectorA;
    RecordingInjector InjectorB;
    AgentCoordinator IncomingA(Clock, InjectorA);
    AgentCoordinator IncomingB(Clock, InjectorB);
    HostCoordinator OutgoingA(Nonce);
    HostCoordinator OutgoingB(Nonce);
    PeerSession SessionA(
        Pair.a, OutgoingA, IncomingA, TrustA, Nonce);
    PeerSession SessionB(
        Pair.b, OutgoingB, IncomingB, TrustB, Nonce);
    CHECK(SessionA.Start());
    CHECK(SessionB.Start());
    CHECK(!SessionA.PeerGrantedCapability(Capability::InputInject));
    CHECK(SessionB.PeerGrantedCapability(Capability::InputInject));
    CHECK(!SessionA.BeginOutgoingFocus());
    CHECK(SessionB.BeginOutgoingFocus());
    CHECK(SessionA.IncomingFocused());
    CHECK(SessionB.ReleaseOutgoingFocus());

    EnvelopeHeader WrongNonce;
    WrongNonce.session_nonce = Nonce + 1;
    WrongNonce.sequence = 500;
    CHECK(Pair.a->send_reliable(encode_packet(
        WrongNonce, FocusRequestMessage{750, 42})));
    CHECK(SessionB.DirectionState() == PeerDirectionState::Local);
    CHECK(SessionB.Stats().session_rejected == 1);
}

void PeerSessionFailsLocalOnAnInvalidCapabilityReplay() {
    using namespace desklink;
    constexpr std::uint64_t Nonce = 0x12'34'56u;
    const auto IdentityA = MakeIdentity(107, "Grant replay A");
    const auto IdentityB = MakeIdentity(108, "Grant replay B");
    auto Pair = make_in_memory_transport_pair(
        TransportPeerInfo{IdentityB, true, true},
        TransportPeerInfo{IdentityA, true, true});
    CapabilitySet InputCapability;
    InputCapability.grant(Capability::InputInject);
    InMemoryTrustStore TrustA;
    InMemoryTrustStore TrustB;
    SaveTrustedPeer(TrustA, IdentityB, InputCapability);
    SaveTrustedPeer(TrustB, IdentityA, InputCapability);
    ManualClock Clock;
    RecordingInjector InjectorA;
    RecordingInjector InjectorB;
    AgentCoordinator IncomingA(Clock, InjectorA);
    AgentCoordinator IncomingB(Clock, InjectorB);
    HostCoordinator OutgoingA(Nonce);
    HostCoordinator OutgoingB(Nonce);
    std::size_t DirectionChanges{};
    PeerSession SessionA(
        Pair.a, OutgoingA, IncomingA, TrustA, Nonce);
    PeerSession SessionB(
        Pair.b, OutgoingB, IncomingB, TrustB, Nonce,
        PeerSessionHandlers{
            {}, [&] { ++DirectionChanges; }, {}});
    CHECK(SessionA.Start());
    CHECK(SessionB.Start());
    CHECK(SessionB.BeginOutgoingFocus());
    CHECK(SessionB.OutgoingFocused());
    CHECK(SessionA.IncomingFocused());

    EnvelopeHeader Header;
    Header.session_nonce = Nonce;
    Header.sequence = 900;
    CHECK(Pair.a->send_reliable(encode_packet(
        Header, CapabilityGrantMessage{std::uint64_t{1} << 63})));
    CHECK(SessionA.DirectionState() == PeerDirectionState::Local);
    CHECK(SessionB.DirectionState() == PeerDirectionState::Local);
    CHECK(!SessionB.PeerGrantedCapability(Capability::InputInject));
    CHECK(!SessionB.BeginOutgoingFocus());
    CHECK(SessionB.Stats().CapabilityGrantsRejected == 1);
    CHECK(DirectionChanges >= 1);
}

void PeerSessionDoesNotTreatRemoteGrantsAsLocalDisclosureConsent() {
    using namespace desklink;
    constexpr std::uint64_t Nonce = 0x65'43'21u;
    const auto IdentityA = MakeIdentity(109, "Disclosure A");
    const auto IdentityB = MakeIdentity(110, "Disclosure B");
    auto Pair = make_in_memory_transport_pair(
        TransportPeerInfo{IdentityB, true, true},
        TransportPeerInfo{IdentityA, true, true});
    CapabilitySet ReportedByB;
    ReportedByB.grant(Capability::AudioReceive);
    ReportedByB.grant(Capability::DisplayTopologyExchange);
    InMemoryTrustStore TrustA;
    InMemoryTrustStore TrustB;
    SaveTrustedPeer(TrustA, IdentityB, CapabilitySet{});
    SaveTrustedPeer(TrustB, IdentityA, ReportedByB);
    ManualClock Clock;
    RecordingInjector InjectorA;
    RecordingInjector InjectorB;
    AgentCoordinator IncomingA(Clock, InjectorA);
    AgentCoordinator IncomingB(Clock, InjectorB);
    HostCoordinator OutgoingA(Nonce);
    HostCoordinator OutgoingB(Nonce);
    PeerSession SessionA(
        Pair.a, OutgoingA, IncomingA, TrustA, Nonce, {}, nullptr,
        DisplayTopologyExchangeOptions{true, &Clock});
    PeerSession SessionB(
        Pair.b, OutgoingB, IncomingB, TrustB, Nonce, {}, nullptr,
        DisplayTopologyExchangeOptions{true, &Clock});
    CHECK(SessionA.Start());
    CHECK(SessionB.Start());
    CHECK(SessionA.PeerGrantedCapability(Capability::AudioReceive));
    CHECK(SessionA.PeerGrantedCapability(
        Capability::DisplayTopologyExchange));
    CHECK(!SessionA.GrantedToPeer(Capability::AudioReceive));
    CHECK(!SessionA.GrantedToPeer(
        Capability::DisplayTopologyExchange));
    CHECK(!SessionA.CanSendAudio());
    AudioFrameMessage Frame;
    Frame.stream_id = 1;
    Frame.pcm.assign(kDeskLinkAudioBytesPerBlock, 0x22);
    CHECK(!SessionA.SendAudioFrame(Frame));
    CHECK(!SessionA.PublishDisplayTopology(
        IdentityA.machine_id,
        MakeDisplayTopology("disclosure-a", "Disclosure A")));
    CHECK(!SessionA.RequestPeerDisplayIdentification(1));
}

void PeerSessionPreservesExplicitAudioAndTopologyExchange() {
    using namespace desklink;
    constexpr std::uint64_t Nonce = 0x24'68'10u;
    const auto IdentityA = MakeIdentity(111, "Modules A");
    const auto IdentityB = MakeIdentity(112, "Modules B");
    auto Pair = make_in_memory_transport_pair(
        TransportPeerInfo{IdentityB, true, true},
        TransportPeerInfo{IdentityA, true, true});
    CapabilitySet ModuleCapabilities;
    ModuleCapabilities.grant(Capability::AudioSend);
    ModuleCapabilities.grant(Capability::AudioReceive);
    ModuleCapabilities.grant(Capability::DisplayTopologyExchange);
    InMemoryTrustStore TrustA;
    InMemoryTrustStore TrustB;
    SaveTrustedPeer(TrustA, IdentityB, ModuleCapabilities);
    SaveTrustedPeer(TrustB, IdentityA, ModuleCapabilities);
    ManualClock Clock;
    RecordingInjector InjectorA;
    RecordingInjector InjectorB;
    AgentCoordinator IncomingA(Clock, InjectorA);
    AgentCoordinator IncomingB(Clock, InjectorB);
    HostCoordinator OutgoingA(Nonce);
    HostCoordinator OutgoingB(Nonce);
    AudioReceiver ReceiverA([](AudioFrameMessage) { return true; });
    AudioReceiver ReceiverB([](AudioFrameMessage) { return true; });
    std::uint64_t IdentifyA{};
    std::uint64_t IdentifyB{};
    std::uint64_t ClockSamplesA{};
    std::uint64_t AudioArrivalsA{};
    std::uint64_t AudioArrivalsB{};
    PeerSessionHandlers HandlersA;
    HandlersA.IdentifyDisplays = [&](std::uint16_t) { ++IdentifyA; };
    PeerSessionHandlers HandlersB;
    HandlersB.IdentifyDisplays = [&](std::uint16_t) { ++IdentifyB; };
    PeerSession SessionA(
        Pair.a, OutgoingA, IncomingA, TrustA, Nonce,
        std::move(HandlersA), &ReceiverA,
        DisplayTopologyExchangeOptions{true, &Clock}, {},
        LatencyDiagnosticOptions{
            true, &Clock,
            [&](const ClockSyncResponseMessage&, std::uint64_t) {
                ++ClockSamplesA;
            },
            [&](std::uint64_t, const AudioFrameMessage&, std::uint64_t) {
                ++AudioArrivalsA;
            }});
    PeerSession SessionB(
        Pair.b, OutgoingB, IncomingB, TrustB, Nonce,
        std::move(HandlersB), &ReceiverB,
        DisplayTopologyExchangeOptions{true, &Clock}, {},
        LatencyDiagnosticOptions{
            true, &Clock, {},
            [&](std::uint64_t, const AudioFrameMessage&, std::uint64_t) {
                ++AudioArrivalsB;
            }});
    CHECK(SessionA.Start());
    CHECK(SessionB.Start());
    CHECK(SessionA.CanSendAudio());
    CHECK(SessionA.CanReceiveAudio());
    CHECK(SessionB.CanSendAudio());
    CHECK(SessionB.CanReceiveAudio());
    CHECK(SessionA.SendClockSyncProbe(1));
    CHECK(ClockSamplesA == 1);
    CHECK(SessionA.Stats().ClockSyncSent == 1);
    CHECK(SessionA.Stats().ClockSyncReceived == 1);
    CHECK(SessionB.Stats().ClockSyncReceived == 1);
    AudioFrameMessage Frame;
    Frame.stream_id = 1;
    Frame.pcm.assign(kDeskLinkAudioBytesPerBlock, 0x33);
    CHECK(SessionA.SendAudioFrame(Frame));
    CHECK(SessionB.SendAudioFrame(Frame));
    CHECK(SessionA.Stats().AudioAccepted == 1);
    CHECK(SessionB.Stats().AudioAccepted == 1);
    CHECK(AudioArrivalsA == 1);
    CHECK(AudioArrivalsB == 1);

    const auto TopologyA = MakeDisplayTopology("modules-a", "Modules A");
    const auto TopologyB = MakeDisplayTopology("modules-b", "Modules B");
    CHECK(SessionA.PublishDisplayTopology(
        IdentityA.machine_id, TopologyA));
    CHECK(SessionB.PublishDisplayTopology(
        IdentityB.machine_id, TopologyB));
    CHECK(SessionA.DisplayTopologyStatus() ==
          DisplayTopologyExchangeStatus::Ready);
    CHECK(SessionB.DisplayTopologyStatus() ==
          DisplayTopologyExchangeStatus::Ready);
    CHECK(SessionA.RemoteDisplayTopology() == TopologyB);
    CHECK(SessionB.RemoteDisplayTopology() == TopologyA);
    CHECK(SessionA.RequestPeerDisplayIdentification(4));
    CHECK(IdentifyA == 0);
    CHECK(IdentifyB == 1);
    CHECK(SessionB.RequestPeerDisplayIdentification(1));
    CHECK(IdentifyA == 1);
    CHECK(IdentifyB == 1);
    CHECK(SessionA.Stats().DisplayIdentifySent == 1);
    CHECK(SessionA.Stats().DisplayIdentifyAccepted == 1);
    CHECK(SessionB.Stats().DisplayIdentifySent == 1);
    CHECK(SessionB.Stats().DisplayIdentifyAccepted == 1);

    EnvelopeHeader StaleIdentify;
    StaleIdentify.session_nonce = Nonce + 1;
    StaleIdentify.sequence = 88;
    CHECK(Pair.a->send_reliable(encode_packet(
        StaleIdentify, DisplayIdentifyRequestMessage{4})));
    CHECK(IdentifyB == 1);
    CHECK(SessionB.Stats().session_rejected >= 1);
}

void ClipboardProtocolIsUtf8BoundedAndReliableOnly() {
    using namespace desklink;
    EnvelopeHeader Header;
    Header.session_nonce = 0xC11F'B04Du;
    Header.sequence = 7;
    ClipboardTextMessage Message{
        MakeMachineId(31), 9, "DeskLink \xF0\x9F\x93\x8B"};
    const auto Encoded = encode_packet(Header, Message);
    CHECK(PeekMessageType(Encoded) == MessageType::ClipboardText);
    const auto Decoded = decode_packet(Encoded, false);
    CHECK(Decoded.packet.has_value());
    CHECK(std::get<ClipboardTextMessage>(
              Decoded.packet->message) == Message);
    CHECK(!decode_packet(Encoded, true).packet.has_value());
    for (std::size_t Length = 0; Length < Encoded.size(); ++Length) {
        CHECK(!decode_packet(
                   ByteBuffer(Encoded.begin(), Encoded.begin() + Length),
                   false).packet.has_value());
    }

    auto Invalid = Message;
    Invalid.OriginMachine = {};
    CHECK(!decode_packet(
               encode_packet(Header, Invalid), false).packet.has_value());
    Invalid = Message;
    Invalid.UpdateId = 0;
    CHECK(!decode_packet(
               encode_packet(Header, Invalid), false).packet.has_value());
    Invalid = Message;
    Invalid.Text = std::string{"\xC0\xAF", 2};
    CHECK(!decode_packet(
               encode_packet(Header, Invalid), false).packet.has_value());
    Invalid = Message;
    Invalid.Text = std::string{"a\0b", 3};
    CHECK(!decode_packet(
               encode_packet(Header, Invalid), false).packet.has_value());
    Invalid = Message;
    Invalid.Text.assign(kMaximumClipboardTextBytes + 1, 'x');
    CHECK(!decode_packet(
               encode_packet(Header, Invalid), false).packet.has_value());

    const auto Hello = encode_packet(Header, ClipboardHelloMessage{});
    CHECK(PeekMessageType(Hello) == MessageType::ClipboardHello);
    CHECK(decode_packet(Hello, false).packet.has_value());
    CHECK(!decode_packet(Hello, true).packet.has_value());
    CHECK(!decode_packet(
               encode_packet(
                   Header, ClipboardHelloMessage{
                       static_cast<std::uint16_t>(
                           kClipboardProtocolVersion + 1),
                       kMaximumClipboardTextBytes}),
               false).packet.has_value());
}

void ClipboardExchangeRequiresConsentNegotiationNonceAndRate() {
    using namespace desklink;
    ManualClock Clock;
    const auto Local = MakeMachineId(41);
    const auto Peer = MakeMachineId(42);
    CapabilitySet LocalCapabilities;
    LocalCapabilities.grant(Capability::ClipboardRead);
    LocalCapabilities.grant(Capability::ClipboardWrite);
    CapabilitySet RemoteCapabilities = LocalCapabilities;

    ClipboardExchange Exchange(&Clock);
    Exchange.Begin(Local, Peer, 88, true, LocalCapabilities);
    CHECK(!Exchange.CanSend());
    CHECK(!Exchange.CanReceive());
    Exchange.SetRemoteCapabilities(RemoteCapabilities);
    CHECK(Exchange.ShouldSendHello());
    CHECK(Exchange.MarkHelloSent());
    CHECK(Exchange.AdmitHello(ClipboardHelloMessage{}) ==
          ClipboardAdmission::Accepted);
    CHECK(Exchange.CanSend());
    CHECK(Exchange.CanReceive());

    const auto First = Exchange.BuildText("first");
    CHECK(First.has_value());
    CHECK(First->OriginMachine == Local);
    CHECK(First->UpdateId == 1);
    CHECK(!Exchange.BuildText("too fast").has_value());
    Clock.advance(kClipboardMinimumUpdateInterval);
    const auto Second = Exchange.BuildText("second");
    CHECK(Second.has_value());
    CHECK(Second->UpdateId == 2);

    ClipboardTextMessage Incoming{Peer, 1, "peer text"};
    CHECK(Exchange.AdmitText(87, Incoming) ==
          ClipboardAdmission::WrongSession);
    auto WrongPeer = Incoming;
    WrongPeer.OriginMachine = MakeMachineId(43);
    CHECK(Exchange.AdmitText(88, WrongPeer) ==
          ClipboardAdmission::WrongPeer);
    CHECK(Exchange.AdmitText(88, Incoming) ==
          ClipboardAdmission::Accepted);
    CHECK(Exchange.AdmitText(88, Incoming) ==
          ClipboardAdmission::StaleUpdate);
    Incoming.UpdateId = 2;
    CHECK(Exchange.AdmitText(88, Incoming) ==
          ClipboardAdmission::RateLimited);
    Clock.advance(kClipboardMinimumUpdateInterval);
    Incoming.UpdateId = 3;
    CHECK(Exchange.AdmitText(88, Incoming) ==
          ClipboardAdmission::Accepted);

    ClipboardExchange Disabled(&Clock);
    Disabled.Begin(Local, Peer, 88, false, LocalCapabilities);
    Disabled.SetRemoteCapabilities(RemoteCapabilities);
    CHECK(!Disabled.ShouldSendHello());
    CHECK(Disabled.AdmitHello(ClipboardHelloMessage{}) ==
          ClipboardAdmission::Disabled);
}

void PeerSessionClipboardIsComplementaryAndInputIndependent() {
    using namespace desklink;
    constexpr std::uint64_t Nonce = 0xC11F'0001u;
    const auto IdentityA = MakeIdentity(121, "Clipboard A");
    const auto IdentityB = MakeIdentity(122, "Clipboard B");
    auto Pair = make_in_memory_transport_pair(
        TransportPeerInfo{IdentityB, true, true},
        TransportPeerInfo{IdentityA, true, true});
    CapabilitySet Capabilities;
    Capabilities.grant(Capability::InputInject);
    Capabilities.grant(Capability::ClipboardRead);
    Capabilities.grant(Capability::ClipboardWrite);
    InMemoryTrustStore TrustA;
    InMemoryTrustStore TrustB;
    SaveTrustedPeer(TrustA, IdentityB, Capabilities);
    SaveTrustedPeer(TrustB, IdentityA, Capabilities);
    ManualClock Clock;
    RecordingInjector InjectorA;
    RecordingInjector InjectorB;
    AgentCoordinator IncomingA(Clock, InjectorA);
    AgentCoordinator IncomingB(Clock, InjectorB);
    HostCoordinator OutgoingA(Nonce);
    HostCoordinator OutgoingB(Nonce);
    std::vector<ClipboardTextMessage> AppliedA;
    std::vector<ClipboardTextMessage> AppliedB;
    PeerSession SessionA(
        Pair.a, OutgoingA, IncomingA, TrustA, Nonce, {}, nullptr, {},
        ClipboardSessionOptions{
            true, IdentityA.machine_id, &Clock,
            [&](ClipboardTextMessage Value) {
                AppliedA.push_back(std::move(Value));
                return true;
            }});
    PeerSession SessionB(
        Pair.b, OutgoingB, IncomingB, TrustB, Nonce, {}, nullptr, {},
        ClipboardSessionOptions{
            true, IdentityB.machine_id, &Clock,
            [&](ClipboardTextMessage Value) {
                AppliedB.push_back(std::move(Value));
                return true;
            }});
    CHECK(SessionA.Start());
    CHECK(SessionB.Start());
    CHECK(SessionA.CanSendClipboard());
    CHECK(SessionA.CanReceiveClipboard());
    CHECK(SessionB.CanSendClipboard());
    CHECK(SessionB.CanReceiveClipboard());
    CHECK(SessionA.PublishClipboardText("from A"));
    CHECK(AppliedB.size() == 1);
    CHECK(AppliedB.front().Text == "from A");
    CHECK(!SessionA.PublishClipboardText("too soon"));
    Clock.advance(kClipboardMinimumUpdateInterval);
    CHECK(SessionB.PublishClipboardText("from B"));
    CHECK(AppliedA.size() == 1);
    CHECK(AppliedA.front().Text == "from B");

    EnvelopeHeader ReplayHeader;
    ReplayHeader.session_nonce = Nonce;
    ReplayHeader.sequence = 700;
    CHECK(Pair.a->send_reliable(encode_packet(
        ReplayHeader,
        ClipboardTextMessage{IdentityA.machine_id, 1, "replayed"})));
    CHECK(AppliedB.size() == 1);
    CHECK(SessionB.Stats().ClipboardRejected >= 1);

    CHECK(SessionA.BeginOutgoingFocus());
    CHECK(SessionA.OutgoingFocused());
    CHECK(SessionB.IncomingFocused());
    CHECK(SessionA.SendKey(KeyEventMessage{0x20, false, true}));
    CHECK(InjectorB.keys.size() == 1);
    CHECK(SessionA.ReleaseOutgoingFocus());
}

void PeerSessionClipboardDefaultsOffAndRejectsOneSidedConsent() {
    using namespace desklink;
    constexpr std::uint64_t Nonce = 0xC11F'0002u;
    const auto IdentityA = MakeIdentity(123, "Clipboard default A");
    const auto IdentityB = MakeIdentity(124, "Clipboard default B");
    auto Pair = make_in_memory_transport_pair(
        TransportPeerInfo{IdentityB, true, true},
        TransportPeerInfo{IdentityA, true, true});
    CapabilitySet GrantedByA;
    GrantedByA.grant(Capability::InputInject);
    GrantedByA.grant(Capability::ClipboardRead);
    CapabilitySet GrantedByB;
    GrantedByB.grant(Capability::InputInject);
    InMemoryTrustStore TrustA;
    InMemoryTrustStore TrustB;
    SaveTrustedPeer(TrustA, IdentityB, GrantedByA);
    SaveTrustedPeer(TrustB, IdentityA, GrantedByB);
    ManualClock Clock;
    RecordingInjector InjectorA;
    RecordingInjector InjectorB;
    AgentCoordinator IncomingA(Clock, InjectorA);
    AgentCoordinator IncomingB(Clock, InjectorB);
    HostCoordinator OutgoingA(Nonce);
    HostCoordinator OutgoingB(Nonce);
    PeerSession SessionA(
        Pair.a, OutgoingA, IncomingA, TrustA, Nonce, {}, nullptr, {},
        ClipboardSessionOptions{
            true, IdentityA.machine_id, &Clock,
            [](ClipboardTextMessage) { return true; }});
    PeerSession SessionB(
        Pair.b, OutgoingB, IncomingB, TrustB, Nonce);
    CHECK(SessionA.Start());
    CHECK(SessionB.Start());
    CHECK(!SessionA.CanSendClipboard());
    CHECK(!SessionA.CanReceiveClipboard());
    CHECK(!SessionA.PublishClipboardText("must stay local"));
    CHECK(SessionA.CanBeginOutgoing());
    CHECK(SessionA.BeginOutgoingFocus());
    CHECK(SessionA.ReleaseOutgoingFocus());
}

void PeerSessionClipboardFailureAndReconnectStayFailClosed() {
    using namespace desklink;
    constexpr std::uint64_t FirstNonce = 0xC11F'0003u;
    constexpr std::uint64_t SecondNonce = 0xC11F'0004u;
    const auto IdentityA = MakeIdentity(125, "Clipboard reconnect A");
    const auto IdentityB = MakeIdentity(126, "Clipboard reconnect B");
    CapabilitySet Capabilities;
    Capabilities.grant(Capability::InputInject);
    Capabilities.grant(Capability::ClipboardRead);
    Capabilities.grant(Capability::ClipboardWrite);
    InMemoryTrustStore TrustA;
    InMemoryTrustStore TrustB;
    SaveTrustedPeer(TrustA, IdentityB, Capabilities);
    SaveTrustedPeer(TrustB, IdentityA, Capabilities);
    ManualClock Clock;

    {
        auto Pair = make_in_memory_transport_pair(
            TransportPeerInfo{IdentityB, true, true},
            TransportPeerInfo{IdentityA, true, true});
        RecordingInjector InjectorA;
        RecordingInjector InjectorB;
        AgentCoordinator IncomingA(Clock, InjectorA);
        AgentCoordinator IncomingB(Clock, InjectorB);
        HostCoordinator OutgoingA(FirstNonce);
        HostCoordinator OutgoingB(FirstNonce);
        PeerSession SessionA(
            Pair.a, OutgoingA, IncomingA, TrustA, FirstNonce, {}, nullptr, {},
            ClipboardSessionOptions{
                true, IdentityA.machine_id, &Clock,
                [](ClipboardTextMessage) { return true; }});
        PeerSession SessionB(
            Pair.b, OutgoingB, IncomingB, TrustB, FirstNonce, {}, nullptr, {},
            ClipboardSessionOptions{
                true, IdentityB.machine_id, &Clock,
                [](ClipboardTextMessage) -> bool {
                    throw std::runtime_error("clipboard owner rejected write");
                }});
        CHECK(SessionA.Start());
        CHECK(SessionB.Start());
        CHECK(SessionA.CanSendClipboard());
        CHECK(SessionA.PublishClipboardText("isolated failure"));
        CHECK(SessionB.Stats().ClipboardRejected >= 1);
        CHECK(SessionA.BeginOutgoingFocus());
        CHECK(SessionA.SendKey(KeyEventMessage{0x21, false, true}));
        CHECK(InjectorB.keys.size() == 1);
        CHECK(SessionA.ReleaseOutgoingFocus());
    }

    auto Pair = make_in_memory_transport_pair(
        TransportPeerInfo{IdentityB, true, true},
        TransportPeerInfo{IdentityA, true, true});
    RecordingInjector InjectorA;
    RecordingInjector InjectorB;
    AgentCoordinator IncomingA(Clock, InjectorA);
    AgentCoordinator IncomingB(Clock, InjectorB);
    HostCoordinator OutgoingA(SecondNonce);
    HostCoordinator OutgoingB(SecondNonce);
    std::vector<ClipboardTextMessage> AppliedB;
    PeerSession SessionA(
        Pair.a, OutgoingA, IncomingA, TrustA, SecondNonce, {}, nullptr, {},
        ClipboardSessionOptions{
            true, IdentityA.machine_id, &Clock,
            [](ClipboardTextMessage) { return true; }});
    PeerSession SessionB(
        Pair.b, OutgoingB, IncomingB, TrustB, SecondNonce, {}, nullptr, {},
        ClipboardSessionOptions{
            true, IdentityB.machine_id, &Clock,
            [&](ClipboardTextMessage Message) {
                AppliedB.push_back(std::move(Message));
                return true;
            }});
    CHECK(SessionA.Start());
    CHECK(SessionB.Start());

    EnvelopeHeader StaleHeader;
    StaleHeader.session_nonce = FirstNonce;
    StaleHeader.sequence = 91;
    CHECK(Pair.a->send_reliable(encode_packet(
        StaleHeader,
        ClipboardTextMessage{IdentityA.machine_id, 99, "stale session"})));
    CHECK(AppliedB.empty());
    CHECK(SessionB.Stats().session_rejected >= 1);
    CHECK(SessionA.PublishClipboardText("fresh session"));
    CHECK(AppliedB.size() == 1);
    CHECK(AppliedB.front().UpdateId == 1);
    CHECK(AppliedB.front().Text == "fresh session");
}

void DisplayTopologyProtocolIsBoundedAndStrict() {
    using namespace desklink;
    constexpr std::uint64_t SessionNonce = 0x9021u;
    DisplayTopologySnapshotMessage Message{
        MakeMachineId(1), SessionNonce,
        MakeDisplayTopology("display-a", "Primary display")};
    EnvelopeHeader Header;
    Header.session_nonce = SessionNonce;
    Header.sequence = 7;

    const auto Encoded = encode_packet(Header, Message);
    CHECK(PeekMessageType(Encoded) ==
          MessageType::DisplayTopologySnapshot);
    const auto Decoded = decode_packet(Encoded, false);
    CHECK(Decoded.packet.has_value());
    CHECK(std::get<DisplayTopologySnapshotMessage>(
              Decoded.packet->message) == Message);
    CHECK(!decode_packet(Encoded, true).packet.has_value());

    const auto Identify = encode_packet(
        Header, DisplayIdentifyRequestMessage{4});
    CHECK(PeekMessageType(Identify) ==
          MessageType::DisplayIdentifyRequest);
    const auto DecodedIdentify = decode_packet(Identify, false);
    CHECK(DecodedIdentify.packet.has_value());
    CHECK(std::holds_alternative<DisplayIdentifyRequestMessage>(
        DecodedIdentify.packet->message));
    CHECK(std::get<DisplayIdentifyRequestMessage>(
              DecodedIdentify.packet->message).FirstDisplayNumber == 4);
    CHECK(!decode_packet(Identify, true).packet.has_value());
    auto InvalidIdentify = Identify;
    InvalidIdentify.push_back(0);
    CHECK(!decode_packet(InvalidIdentify, false).packet.has_value());
    CHECK(!decode_packet(
        encode_packet(Header, DisplayIdentifyRequestMessage{0}),
        false).packet.has_value());
    CHECK(!decode_packet(
        encode_packet(
            Header,
            DisplayIdentifyRequestMessage{
                static_cast<std::uint16_t>(kMaxDisplayCount + 1u)}),
        false).packet.has_value());

    auto Truncated = Encoded;
    Truncated.pop_back();
    CHECK(PeekMessageType(Truncated) ==
          MessageType::DisplayTopologySnapshot);
    CHECK(!decode_packet(Truncated, false).packet.has_value());

    auto Invalid = Message;
    Invalid.Machine = {};
    CHECK(!decode_packet(
               encode_packet(Header, Invalid), false).packet.has_value());
    Invalid = Message;
    Invalid.SessionNonce = 0;
    CHECK(!decode_packet(
               encode_packet(Header, Invalid), false).packet.has_value());
    Invalid = Message;
    Invalid.Topology.Displays[0].StableIdentity.push_back('\0');
    CHECK(!decode_packet(
               encode_packet(Header, Invalid), false).packet.has_value());
    Invalid = Message;
    Invalid.Topology.Displays.push_back(Invalid.Topology.Displays.front());
    CHECK(!decode_packet(
               encode_packet(Header, Invalid), false).packet.has_value());
    Invalid = Message;
    Invalid.Topology.Displays[0].Id++;
    CHECK(!decode_packet(
               encode_packet(Header, Invalid), false).packet.has_value());

    std::vector<DiscoveredDisplay> DenseDisplays;
    DenseDisplays.reserve(kMaxDisplayCount);
    for (std::size_t Index = 0; Index < kMaxDisplayCount; ++Index) {
        DenseDisplays.push_back(DiscoveredDisplay{
            "dense-" + std::to_string(Index) + "-" +
                std::string(990, static_cast<char>('a' + Index % 26)),
            std::string(kMaxDisplayFriendlyNameLength, 'n'),
            {static_cast<std::int32_t>(Index * 10), 0,
             static_cast<std::int32_t>(Index * 10 + 10), 10},
            Index == 0});
    }
    DisplayTopologyMap DenseTopology;
    CHECK(DenseTopology.Update(std::move(DenseDisplays)) ==
          DisplayTopologyUpdate::Changed);
    const DisplayTopologySnapshotMessage Oversized{
        MakeMachineId(1), SessionNonce, DenseTopology.Current()};
    CHECK(IsValidDisplayTopologySnapshot(Oversized.Topology));
    CHECK(!IsValidDisplayTopologySnapshotMessage(Oversized));
    const auto OversizedDecoded = decode_packet(
        encode_packet(Header, Oversized), false);
    CHECK(!OversizedDecoded.packet.has_value());
    CHECK(OversizedDecoded.error == DecodeError::PayloadTooLarge);
}

void DisplayTopologyAdmissionFailsClosedAndRecoversOnReconnect() {
    using namespace desklink;
    constexpr std::uint64_t SessionNonce = 0x7700u;
    ManualClock Clock;
    DisplayTopologyExchangeTracker Tracker(&Clock);
    const auto Peer = MakeMachineId(2);
    const auto Topology = MakeDisplayTopology("display-b", "Peer display");
    EnvelopeHeader Header;
    Header.type = MessageType::DisplayTopologySnapshot;
    Header.session_nonce = SessionNonce;
    DisplayTopologySnapshotMessage Message{Peer, SessionNonce, Topology};

    Tracker.Begin(Peer, SessionNonce, false, true);
    CHECK(Tracker.Status() == DisplayTopologyExchangeStatus::Disabled);
    CHECK(!Tracker.Admit(Header, Message));
    Tracker.Begin(Peer, SessionNonce, true, false);
    CHECK(Tracker.Status() ==
          DisplayTopologyExchangeStatus::CapabilityMissing);
    CHECK(!Tracker.Admit(Header, Message));

    Tracker.Begin(Peer, SessionNonce, true, true);
    CHECK(Tracker.Status() ==
          DisplayTopologyExchangeStatus::Synchronizing);
    auto WrongPeer = Message;
    WrongPeer.Machine = MakeMachineId(9);
    CHECK(!Tracker.Admit(Header, WrongPeer));
    CHECK(Tracker.Status() == DisplayTopologyExchangeStatus::Rejected);
    CHECK(!Tracker.Snapshot().has_value());

    Tracker.Begin(Peer, SessionNonce, true, true);
    CHECK(Tracker.Admit(Header, Message));
    CHECK(Tracker.Status() == DisplayTopologyExchangeStatus::Ready);
    CHECK(Tracker.Snapshot() == Topology);

    auto GenerationTwo = Topology;
    GenerationTwo.Generation = 2;
    Message.Topology = GenerationTwo;
    CHECK(Tracker.Admit(Header, Message));
    Message.Topology = Topology;
    CHECK(!Tracker.Admit(Header, Message));
    CHECK(Tracker.Status() == DisplayTopologyExchangeStatus::Ready);
    CHECK(Tracker.Snapshot()->Generation == 2);

    Message.Topology = GenerationTwo;
    Message.Topology.Displays[0].FriendlyName = "Changed without generation";
    CHECK(IsValidDisplayTopologySnapshot(Message.Topology));
    CHECK(!Tracker.Admit(Header, Message));
    CHECK(Tracker.Status() == DisplayTopologyExchangeStatus::Rejected);
    CHECK(!Tracker.Snapshot().has_value());

    Tracker.Begin(Peer, SessionNonce, true, true);
    Message.Topology = Topology;
    CHECK(Tracker.Admit(Header, Message));
    Clock.advance(kDisplayTopologyExchangeTimeout);
    CHECK(Tracker.Status() == DisplayTopologyExchangeStatus::TimedOut);
    CHECK(!Tracker.Snapshot().has_value());
    CHECK(!Tracker.Admit(Header, Message));

    Tracker.Stop();
    CHECK(Tracker.Status() == DisplayTopologyExchangeStatus::Offline);
    Tracker.Begin(Peer, SessionNonce + 1, true, true);
    Header.session_nonce = SessionNonce + 1;
    Message.SessionNonce = SessionNonce + 1;
    CHECK(Tracker.Admit(Header, Message));
    CHECK(Tracker.Status() == DisplayTopologyExchangeStatus::Ready);
}

void RoamingRouteWaitsForAuthenticatedTopology() {
    using namespace desklink;
    const auto Configuration = MakeRoamingConfiguration();
    auto TopologyA = MakeDisplayTopology("display-a", "A");
    auto TopologyB = MakeDisplayTopology("display-b", "B",
                                         {0, 0, 2560, 1440});
    std::array<MachineDisplayTopology, 2> Topologies{
        MachineDisplayTopology{MakeMachineId(1), &TopologyA},
        MachineDisplayTopology{MakeMachineId(2), &TopologyB},
    };
    const auto& Link = Configuration.Links.front();
    CHECK(EvaluateRoamingRoute(
              Link, RoamingDirection::AToB,
              PeerConnectionStatus::Connected,
              DisplayTopologyExchangeStatus::Ready,
              true, true, Topologies) == RoamingRouteStatus::Ready);
    CHECK(EvaluateRoamingRoute(
              Link, RoamingDirection::AToB,
              PeerConnectionStatus::Authenticating,
              DisplayTopologyExchangeStatus::Ready,
              true, true, Topologies) ==
          RoamingRouteStatus::SynchronizingTopology);
    CHECK(EvaluateRoamingRoute(
              Link, RoamingDirection::AToB,
              PeerConnectionStatus::Connected,
              DisplayTopologyExchangeStatus::Synchronizing,
              true, true, Topologies) ==
          RoamingRouteStatus::SynchronizingTopology);
    CHECK(EvaluateRoamingRoute(
              Link, RoamingDirection::AToB,
              PeerConnectionStatus::Connected,
              DisplayTopologyExchangeStatus::CapabilityMissing,
              false, true, Topologies) ==
          RoamingRouteStatus::CapabilityMissing);
    CHECK(EvaluateRoamingRoute(
              Link, RoamingDirection::AToB,
              PeerConnectionStatus::Connected,
              DisplayTopologyExchangeStatus::Rejected,
              true, true, Topologies) == RoamingRouteStatus::Invalid);
    CHECK(EvaluateRoamingRoute(
              Link, RoamingDirection::AToB,
              PeerConnectionStatus::Connected,
              DisplayTopologyExchangeStatus::Ready,
              true, false, Topologies) ==
          RoamingRouteStatus::DirectionUnsupported);

    auto Missing = Topologies;
    Missing[1].Topology = nullptr;
    CHECK(EvaluateRoamingRoute(
              Link, RoamingDirection::AToB,
              PeerConnectionStatus::Connected,
              DisplayTopologyExchangeStatus::Ready,
              true, true, Missing) == RoamingRouteStatus::DisplayMissing);
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

void FailedInputCleanupIsRetriedAndBlocksReadmission() {
    using namespace desklink;
    ManualClock Clock;
    RecordingInjector Injector;
    AgentCoordinator Agent(Clock, Injector);
    CapabilitySet Capabilities;
    Capabilities.grant(Capability::InputInject);
    Agent.set_peer_capabilities(Capabilities);

    EnvelopeHeader Header;
    const auto Request = decode_packet(
        encode_packet(Header, FocusRequestMessage{750, 1}), false);
    CHECK(Request.packet.has_value());
    CHECK(Agent.handle(*Request.packet) == AgentDecision::Accepted);
    const auto Epoch = Agent.focus_state().epoch();

    Injector.ReleaseSucceeds = false;
    Header.epoch = Epoch;
    const auto Release = decode_packet(
        encode_packet(Header, FocusReleaseMessage{}), false);
    CHECK(Release.packet.has_value());
    CHECK(Agent.handle(*Release.packet) == AgentDecision::RejectedMalformed);
    CHECK(Agent.InputCleanupPending());
    CHECK(Agent.handle(*Request.packet) == AgentDecision::RejectedLease);

    Injector.ReleaseSucceeds = true;
    Agent.tick();
    CHECK(!Agent.InputCleanupPending());
    CHECK(Injector.release_calls >= 2);
    CHECK(Agent.handle(*Request.packet) == AgentDecision::Accepted);
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

    CHECK(!host.PointerMotion(PointerMotionMessage{}).has_value());
    auto MotionBytes = host.PointerMotion(PointerMotionMessage{25, -12});
    CHECK(MotionBytes.has_value());
    auto Motion = decode_packet(*MotionBytes, true);
    CHECK(Motion.packet.has_value());
    CHECK(agent.handle(*Motion.packet) == AgentDecision::Accepted);
    CHECK(injector.motions.size() == 1);

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

void AdaptiveJitterRaisesQuicklyAndLowersSlowly() {
    using namespace desklink;
    AudioAdaptiveJitterController Controller(4, 12);
    std::uint64_t Capture{};
    std::uint64_t Arrival{};
    Controller.Observe(1, Capture, Arrival);
    for (std::uint64_t Sequence = 2; Sequence <= 401; ++Sequence) {
        Capture += kDeskLinkAudioBlockDurationUs;
        Arrival += kDeskLinkAudioBlockDurationUs;
        Controller.Observe(Sequence, Capture, Arrival);
    }
    CHECK(Controller.TargetFrames() == 2);
    CHECK(Controller.TargetLowers() == 2);

    Capture += kDeskLinkAudioBlockDurationUs;
    Arrival += kDeskLinkAudioBlockDurationUs + 30'000;
    Controller.Observe(402, Capture, Arrival);
    CHECK(Controller.TargetFrames() == 8);
    CHECK(Controller.PeakTargetFrames() == 8);
    CHECK(Controller.TargetRaises() == 1);
    CHECK(Controller.EstimatedJitterUs() > 0);

    Controller.Observe(401, Capture, Arrival);
    CHECK(Controller.TargetFrames() == 8);
    Arrival += kDeskLinkAudioBlockDurationUs;
    Controller.Observe(403, 1, Arrival);
    CHECK(Controller.TargetFrames() == 8);
    Arrival += 6'000'000;
    Controller.Observe(404, 6'000'001, Arrival);
    CHECK(Controller.TargetFrames() == 8);
    for (int Index = 0; Index < 20; ++Index) {
        Controller.ObserveConcealment();
    }
    CHECK(Controller.TargetFrames() == 12);
    CHECK(Controller.PeakTargetFrames() == 12);

    Controller.Reset();
    CHECK(Controller.TargetFrames() == 4);
    CHECK(Controller.PeakTargetFrames() == 4);
    CHECK(Controller.EstimatedJitterUs() == 0);
    CHECK(Controller.TargetRaises() == 0);
}

void ClockDriftControllerIsBoundedAndSlewLimited() {
    using namespace desklink;
    AudioClockDriftController Controller(4, 100, 25);
    Controller.Observe(240, 240, false);
    for (int Window = 0; Window < 8; ++Window) {
        for (int Sample = 0; Sample < 4; ++Sample) {
            Controller.Observe(480, 240, true);
        }
    }
    CHECK(Controller.AppliedPpm() == 100);
    CHECK(Controller.Adjustments() == 4);

    for (int Sample = 0; Sample < 4; ++Sample) {
        Controller.Observe(0, 240, true);
    }
    CHECK(Controller.AppliedPpm() == 75);
    CHECK(Controller.Adjustments() == 5);

    Controller.Observe(240, 480, true);
    CHECK(Controller.AppliedPpm() == 0);
    CHECK(Controller.Discontinuities() == 1);
    Controller.Reset();
    CHECK(Controller.AppliedPpm() == 0);
    CHECK(Controller.Adjustments() == 0);
    CHECK(Controller.Discontinuities() == 0);
    Controller.Observe(240, 240, false);
    for (int Sample = 0; Sample < 4; ++Sample) {
        Controller.Observe(300, 240, true);
    }
    CHECK(Controller.AppliedPpm() == 0);
}

void ClockDriftResamplerIsExactAndBounded() {
    using namespace desklink;
    const auto MakeFrame = [](std::uint64_t Timestamp,
                              std::int16_t Base) {
        AudioFrameMessage Frame;
        Frame.stream_id = 7;
        Frame.capture_timestamp_us = Timestamp;
        Frame.pcm.resize(kDeskLinkAudioBytesPerBlock);
        for (std::size_t Index = 0;
             Index < kDeskLinkAudioFramesPerBlock; ++Index) {
            const auto Sample = static_cast<std::int16_t>(
                Base + static_cast<std::int16_t>(Index));
            std::uint16_t Bits{};
            std::memcpy(&Bits, &Sample, sizeof(Bits));
            for (std::size_t Channel = 0;
                 Channel < kDeskLinkAudioChannels; ++Channel) {
                const auto Offset = Index * kDeskLinkAudioBytesPerFrame +
                    Channel * kDeskLinkAudioBytesPerSample;
                Frame.pcm[Offset] = static_cast<std::uint8_t>(Bits & 0xffu);
                Frame.pcm[Offset + 1] =
                    static_cast<std::uint8_t>(Bits >> 8);
            }
        }
        return Frame;
    };

    AudioClockDriftResampler FasterSource;
    std::size_t CompressedBlocks{};
    for (std::size_t Index = 0; Index < 2'000; ++Index) {
        CHECK(FasterSource.Push(MakeFrame(
            10'000 + Index * kDeskLinkAudioBlockDurationUs,
            static_cast<std::int16_t>(Index))));
        while (auto Output = FasterSource.Pop(1'000)) {
            CHECK(IsDeskLinkAudioFrame(*Output));
            ++CompressedBlocks;
        }
        CHECK(FasterSource.BufferedSourceFrames() <=
              2 * kDeskLinkAudioFramesPerBlock);
    }
    CHECK(CompressedBlocks < 2'000);
    CHECK(CompressedBlocks >= 1'997);

    AudioClockDriftResampler SlowerSource;
    std::size_t ExpandedBlocks{};
    for (std::size_t Index = 0; Index < 2'000; ++Index) {
        CHECK(SlowerSource.Push(MakeFrame(
            20'000 + Index * kDeskLinkAudioBlockDurationUs,
            static_cast<std::int16_t>(Index))));
        while (auto Output = SlowerSource.Pop(-1'000)) {
            CHECK(IsDeskLinkAudioFrame(*Output));
            ++ExpandedBlocks;
        }
        CHECK(SlowerSource.BufferedSourceFrames() <=
              2 * kDeskLinkAudioFramesPerBlock);
    }
    CHECK(ExpandedBlocks > 2'000);
    CHECK(ExpandedBlocks <= 2'003);

    SlowerSource.Reset();
    CHECK(SlowerSource.BufferedSourceFrames() == 0);
    CHECK(!SlowerSource.Pop(0).has_value());
}

void JitterTargetIncreaseRebuffersWithinBounds() {
    using namespace desklink;
    AudioJitterBuffer Buffer(2, 8);
    const auto MakeFrame = [](std::uint8_t Marker) {
        AudioFrameMessage Frame;
        Frame.pcm.assign(kDeskLinkAudioBytesPerBlock, Marker);
        return Frame;
    };
    CHECK(Buffer.push(1, MakeFrame(1)));
    CHECK(Buffer.push(2, MakeFrame(2)));
    auto First = Buffer.pop();
    CHECK(First.has_value() && First->frame.pcm[0] == 1);

    Buffer.SetTargetFrames(4);
    CHECK(Buffer.TargetFrames() == 4);
    CHECK(Buffer.RebufferEvents() == 1);
    CHECK(!Buffer.pop().has_value());
    CHECK(Buffer.push(3, MakeFrame(3)));
    CHECK(Buffer.push(4, MakeFrame(4)));
    CHECK(!Buffer.pop().has_value());
    CHECK(Buffer.push(5, MakeFrame(5)));
    auto Second = Buffer.pop();
    CHECK(Second.has_value() && Second->frame.pcm[0] == 2);

    Buffer.SetTargetFrames(2);
    CHECK(Buffer.TargetFrames() == 2);
    CHECK(Buffer.RebufferEvents() == 1);
    Buffer.SetTargetFrames(100);
    CHECK(Buffer.TargetFrames() == 8);
    CHECK(Buffer.RebufferEvents() == 2);
}

void AudioFrameAssemblerProducesExactBoundedBlocks() {
    using namespace desklink;
    AudioFrameAssembler Assembler(17);
    std::vector<AudioFrameMessage> Output;

    ByteBuffer First(100 * kDeskLinkAudioBytesPerFrame, 0x11);
    ByteBuffer Second(140 * kDeskLinkAudioBytesPerFrame, 0x22);
    CHECK(Assembler.Push(First, 10'000, Output));
    CHECK(Output.empty());
    CHECK(Assembler.Push(Second, 12'083, Output));
    CHECK(Output.size() == 1);
    CHECK(IsDeskLinkAudioFrame(Output[0]));
    CHECK(Output[0].stream_id == 17);
    CHECK(Output[0].capture_timestamp_us == 10'000);
    CHECK(Output[0].pcm[0] == 0x11);
    CHECK(Output[0].pcm[First.size()] == 0x22);

    Output.clear();
    ByteBuffer TwoBlocks(2 * kDeskLinkAudioBytesPerBlock, 0x33);
    CHECK(Assembler.Push(TwoBlocks, 20'000, Output));
    CHECK(Output.size() == 2);
    CHECK(Output[0].capture_timestamp_us == 20'000);
    CHECK(Output[1].capture_timestamp_us == 25'000);

    Output.clear();
    CHECK(Assembler.PushSilence(kDeskLinkAudioFramesPerBlock,
                                30'000, Output));
    CHECK(Output.size() == 1);
    CHECK(std::all_of(Output[0].pcm.begin(), Output[0].pcm.end(),
                      [](std::uint8_t Value) { return Value == 0; }));

    Output.clear();
    CHECK(!Assembler.Push(ByteBuffer{1, 2, 3}, 40'000, Output));
    CHECK(Output.empty());
    CHECK(!Assembler.PushSilence(8'193, 40'000, Output));
    ByteBuffer Partial(10 * kDeskLinkAudioBytesPerFrame, 0x44);
    CHECK(Assembler.Push(Partial, 40'000, Output));
    Assembler.Reset();
    CHECK(Assembler.PushSilence(kDeskLinkAudioFramesPerBlock,
                                50'000, Output));
    CHECK(Output.size() == 1);
    CHECK(Output[0].capture_timestamp_us == 50'000);
    auto Invalid = AudioFrameMessage{};
    Invalid.pcm.assign(kDeskLinkAudioBytesPerBlock, 0);
    CHECK(IsDeskLinkAudioFrame(Invalid));
    Invalid.sample_rate = 44'100;
    CHECK(!IsDeskLinkAudioFrame(Invalid));
}

void AudioReceiverIsBoundedAndFailsClosed() {
    using namespace desklink;
    std::vector<AudioFrameMessage> Rendered;
    AudioReceiver Receiver(
        [&](AudioFrameMessage Frame) {
            Rendered.push_back(std::move(Frame));
            return true;
        },
        2, 4);
    const auto MakeFrame = [](std::uint8_t Marker,
                              std::uint32_t StreamId = 9) {
        AudioFrameMessage Frame;
        Frame.stream_id = StreamId;
        Frame.pcm.assign(kDeskLinkAudioBytesPerBlock, Marker);
        return Frame;
    };

    CHECK(Receiver.Push(10, MakeFrame(10)));
    CHECK(Receiver.Pump() == AudioPumpResult::Buffering);
    CHECK(Receiver.Push(12, MakeFrame(12)));
    CHECK(Receiver.Pump() == AudioPumpResult::Submitted);
    CHECK(Rendered.size() == 1 && Rendered.back().pcm[0] == 10);
    CHECK(Receiver.Push(13, MakeFrame(13)));
    CHECK(Receiver.Pump() == AudioPumpResult::Submitted);
    CHECK(Rendered.size() == 2 && Rendered.back().pcm[0] == 0);
    CHECK(Receiver.Pump() == AudioPumpResult::Buffering);
    CHECK(Receiver.Push(14, MakeFrame(14)));
    CHECK(Receiver.Pump() == AudioPumpResult::Submitted);
    CHECK(Rendered.size() == 3 && Rendered.back().pcm[0] == 12);
    CHECK(!Receiver.Push(9, MakeFrame(9)));
    CHECK(!Receiver.Push(0, MakeFrame(14)));
    CHECK(!Receiver.Push(15, MakeFrame(15, 10)));
    auto Invalid = MakeFrame(0);
    Invalid.sample_rate = 44'100;
    CHECK(!Receiver.Push(15, std::move(Invalid)));
    const auto Stats = Receiver.Stats();
    CHECK(Stats.Accepted == 4);
    CHECK(Stats.Submitted == 3);
    CHECK(Stats.Concealed == 1);
    CHECK(Stats.SequenceRejected == 2);
    CHECK(Stats.StreamRejected == 1);
    CHECK(Stats.FormatRejected == 1);
    CHECK(Stats.CurrentTargetFrames == 3);
    CHECK(Stats.PeakTargetFrames == 3);
    CHECK(Stats.TargetRaises == 1);
    CHECK(Stats.RebufferEvents == 1);

    std::size_t BatchedRenderCount{};
    AudioReceiver Batched(
        [&](AudioFrameMessage) {
            ++BatchedRenderCount;
            return true;
        },
        4, kDeskLinkAudioMaximumPumpBatch);
    for (std::uint64_t Sequence = 1;
         Sequence <= kDeskLinkAudioMaximumPumpBatch; ++Sequence) {
        CHECK(Batched.Push(Sequence, MakeFrame(
            static_cast<std::uint8_t>(Sequence))));
    }
    CHECK(Batched.PumpAvailable() == AudioPumpResult::Submitted);
    CHECK(BatchedRenderCount == kDeskLinkAudioMaximumPumpBatch);
    CHECK(Batched.Stats().SequenceRejected == 0);
    CHECK(Batched.Push(
        kDeskLinkAudioMaximumPumpBatch + 1,
        MakeFrame(21)));
    CHECK(Batched.PumpAvailable(0) == AudioPumpResult::Buffering);

    AudioReceiver Rejecting(
        [](AudioFrameMessage) { return false; }, 1, 2);
    CHECK(Rejecting.Push(1, MakeFrame(1)));
    CHECK(Rejecting.Pump() == AudioPumpResult::RenderRejected);
    CHECK(Rejecting.Failed());
    CHECK(!Rejecting.Push(2, MakeFrame(2)));
    CHECK(Rejecting.Stats().RenderRejected == 1);
    Rejecting.Reset();
    CHECK(!Rejecting.Failed());
    CHECK(Rejecting.Push(1, MakeFrame(1)));
}

void AudioReceiverGainAndMuteAreBoundedAndRamped() {
    using namespace desklink;
    std::vector<AudioFrameMessage> Rendered;
    AudioReceiver Receiver(
        [&](AudioFrameMessage Frame) {
            Rendered.push_back(std::move(Frame));
            return true;
        },
        1, 4);
    const auto MakeFrame = [](std::int16_t Sample) {
        AudioFrameMessage Frame;
        Frame.stream_id = 12;
        Frame.pcm.resize(kDeskLinkAudioBytesPerBlock);
        std::uint16_t Bits{};
        std::memcpy(&Bits, &Sample, sizeof(Bits));
        for (std::size_t Offset = 0; Offset < Frame.pcm.size(); Offset += 2) {
            Frame.pcm[Offset] = static_cast<std::uint8_t>(Bits & 0xffu);
            Frame.pcm[Offset + 1] = static_cast<std::uint8_t>(Bits >> 8);
        }
        return Frame;
    };
    const auto ReadSample = [](const AudioFrameMessage& Frame,
                               std::size_t FrameIndex) {
        const auto Offset = FrameIndex * kDeskLinkAudioBytesPerFrame;
        const auto Bits = static_cast<std::uint16_t>(Frame.pcm[Offset]) |
            (static_cast<std::uint16_t>(Frame.pcm[Offset + 1]) << 8);
        std::int16_t Sample{};
        std::memcpy(&Sample, &Bits, sizeof(Sample));
        return Sample;
    };

    CHECK(Receiver.GainPermyriad() == 10'000);
    CHECK(!Receiver.Muted());
    CHECK(!Receiver.SetGainPermyriad(10'001));
    CHECK(Receiver.SetGainPermyriad(5'000));
    CHECK(Receiver.Push(1, MakeFrame(10'000)));
    CHECK(Receiver.Pump() == AudioPumpResult::Submitted);
    CHECK(ReadSample(Rendered.back(), 0) > 5'000);
    CHECK(ReadSample(Rendered.back(), 0) < 10'000);
    CHECK(ReadSample(Rendered.back(), kDeskLinkAudioFramesPerBlock - 1) ==
          5'000);

    CHECK(Receiver.Push(2, MakeFrame(10'000)));
    CHECK(Receiver.Pump() == AudioPumpResult::Submitted);
    CHECK(ReadSample(Rendered.back(), 0) == 5'000);
    CHECK(ReadSample(Rendered.back(), kDeskLinkAudioFramesPerBlock - 1) ==
          5'000);

    CHECK(Receiver.ToggleMuted());
    CHECK(Receiver.Muted());
    CHECK(Receiver.Push(3, MakeFrame(10'000)));
    CHECK(Receiver.Pump() == AudioPumpResult::Submitted);
    CHECK(ReadSample(Rendered.back(), 0) < 5'000);
    CHECK(ReadSample(Rendered.back(), 0) > 0);
    CHECK(ReadSample(Rendered.back(), kDeskLinkAudioFramesPerBlock - 1) == 0);
    CHECK(Receiver.SetGainPermyriad(2'500));
    Receiver.Reset();
    CHECK(Receiver.GainPermyriad() == 2'500);
    CHECK(Receiver.Muted());
    CHECK(Receiver.Push(1, MakeFrame(10'000)));
    CHECK(Receiver.Pump() == AudioPumpResult::Submitted);
    CHECK(ReadSample(Rendered.back(), 0) == 0);

    CHECK(!Receiver.ToggleMuted());
    CHECK(!Receiver.Muted());
    CHECK(Receiver.Push(2, MakeFrame(10'000)));
    CHECK(Receiver.Pump() == AudioPumpResult::Submitted);
    CHECK(ReadSample(Rendered.back(), 0) > 0);
    CHECK(ReadSample(Rendered.back(), kDeskLinkAudioFramesPerBlock - 1) ==
          2'500);
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
    auto newest = decode_packet(
        encode_packet(newest_h, PointerMotionMessage{50, -20}), true);
    CHECK(newest.packet.has_value());
    CHECK(agent.handle(*newest.packet) == AgentDecision::Accepted);

    EnvelopeHeader old_h;
    old_h.epoch = epoch;
    old_h.sequence = 19;
    auto old = decode_packet(encode_packet(old_h, PointerPositionMessage{0, 100, 100}), true);
    CHECK(old.packet.has_value());
    CHECK(agent.handle(*old.packet) == AgentDecision::RejectedSequence);
    CHECK(injector.motions.size() == 1);
    CHECK(injector.pointers.empty());
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
    caps.grant(Capability::AudioReceive);
    CapabilitySet HostCapabilities;
    HostCapabilities.grant(Capability::AudioSend);
    InMemoryTrustStore host_trust;
    InMemoryTrustStore agent_trust;
    SaveTrustedPeer(host_trust, host_view.identity, HostCapabilities);
    SaveTrustedPeer(agent_trust, agent_view.identity, caps);
    HostCoordinator host_core(nonce);

    std::vector<AudioFrameMessage> RenderedAudio;
    AudioReceiver Audio(
        [&](AudioFrameMessage Frame) {
            RenderedAudio.push_back(std::move(Frame));
            return true;
        },
        2, 8);

    AgentSession agent(pair.b, agent_core, agent_trust, nonce);
    bool FocusReadyNotified = false;
    HostSession host(pair.a, host_core, host_trust, nonce, [&] {
        FocusReadyNotified = true;
    }, &Audio);
    CHECK(agent.start());
    CHECK(host.start());
    CHECK(agent.CanSendAudio());
    CHECK(host.CanReceiveAudio());
    AudioFrameMessage FirstAudio;
    FirstAudio.stream_id = 1;
    FirstAudio.pcm.assign(kDeskLinkAudioBytesPerBlock, 0x41);
    auto SecondAudio = FirstAudio;
    SecondAudio.capture_timestamp_us = 5'000;
    SecondAudio.pcm.assign(kDeskLinkAudioBytesPerBlock, 0x42);
    auto ThirdAudio = SecondAudio;
    ThirdAudio.capture_timestamp_us = 10'000;
    ThirdAudio.pcm.assign(kDeskLinkAudioBytesPerBlock, 0x43);
    CHECK(agent.SendAudioFrame(std::move(FirstAudio)));
    CHECK(agent.SendAudioFrame(std::move(SecondAudio)));
    CHECK(agent.SendAudioFrame(std::move(ThirdAudio)));
    CHECK(Audio.Pump() == AudioPumpResult::Submitted);
    CHECK(RenderedAudio.size() == 1);
    CHECK(RenderedAudio[0].pcm[0] == 0x41);
    CHECK(agent.stats().AudioSent == 3);
    CHECK(host.stats().AudioAccepted == 3);
    CHECK(host.focus_remote(750));
    CHECK(host_core.remote_focused());
    CHECK(host.RemoteFocused());
    CHECK(FocusReadyNotified);
    CHECK(host.send_key(KeyEventMessage{0x20, false, true}));
    CHECK(host.send_key(KeyEventMessage{0x1D, true, true}));
    CHECK(host.send_button(MouseButtonMessage{MouseButtonId::X1, true}));
    CHECK(host.SendInputStateSnapshot());
    CHECK(host.send_pointer(PointerPositionMessage{0, 30000, 31000}));
    CHECK(host.SendPointerMotion(PointerMotionMessage{18, -7}));
    CHECK(host.SendWheel(MouseWheelMessage{MouseWheelAxis::Horizontal, -120}));
    CHECK(injector.keys.size() == 2);
    CHECK(injector.buttons.size() == 1);
    CHECK(injector.snapshots.size() == 1);
    CHECK(InputSnapshotKeyDown(injector.snapshots.back(), 0x20, false));
    CHECK(InputSnapshotKeyDown(injector.snapshots.back(), 0x1D, true));
    CHECK(InputSnapshotButtonDown(injector.snapshots.back(), MouseButtonId::X1));
    CHECK(injector.pointers.size() == 1);
    CHECK(injector.motions.size() == 1);
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

void TopologySessionExchangeIsCapabilityAndNonceBound() {
    using namespace desklink;
    constexpr std::uint64_t SessionNonce = 0x61'72'83u;
    TransportPeerInfo HostView;
    HostView.authenticated = true;
    HostView.encrypted = true;
    HostView.identity = MakeIdentity(52, "Topology peer B");
    TransportPeerInfo AgentView;
    AgentView.authenticated = true;
    AgentView.encrypted = true;
    AgentView.identity = MakeIdentity(51, "Topology peer A");

    {
        auto Pair = make_in_memory_transport_pair(HostView, AgentView);
        ManualClock Clock;
        RecordingInjector Injector;
        AgentCoordinator AgentCore(Clock, Injector);
        HostCoordinator HostCore(SessionNonce);
        InMemoryTrustStore HostTrust;
        InMemoryTrustStore AgentTrust;
        SaveTrustedPeer(HostTrust, HostView.identity);
        SaveTrustedPeer(AgentTrust, AgentView.identity);
        AgentSession Agent(
            Pair.b, AgentCore, AgentTrust, SessionNonce,
            DisplayTopologyExchangeOptions{true, &Clock});
        HostSession Host(
            Pair.a, HostCore, HostTrust, SessionNonce, {}, nullptr,
            DisplayTopologyExchangeOptions{true, &Clock});
        CHECK(Agent.start());
        CHECK(Host.start());
        CHECK(Agent.DisplayTopologyStatus() ==
              DisplayTopologyExchangeStatus::CapabilityMissing);
        CHECK(Host.DisplayTopologyStatus() ==
              DisplayTopologyExchangeStatus::CapabilityMissing);
        CHECK(!Agent.PublishDisplayTopology(
            HostView.identity.machine_id,
            MakeDisplayTopology("display-b", "B")));
        CHECK(!Host.PublishDisplayTopology(
            AgentView.identity.machine_id,
            MakeDisplayTopology("display-a", "A")));
    }

    CapabilitySet TopologyCapability;
    TopologyCapability.grant(Capability::DisplayTopologyExchange);
    auto Pair = make_in_memory_transport_pair(HostView, AgentView);
    ManualClock Clock;
    RecordingInjector Injector;
    AgentCoordinator AgentCore(Clock, Injector);
    HostCoordinator HostCore(SessionNonce);
    InMemoryTrustStore HostTrust;
    InMemoryTrustStore AgentTrust;
    SaveTrustedPeer(HostTrust, HostView.identity, TopologyCapability);
    SaveTrustedPeer(AgentTrust, AgentView.identity, TopologyCapability);
    AgentSession Agent(
        Pair.b, AgentCore, AgentTrust, SessionNonce,
        DisplayTopologyExchangeOptions{true, &Clock});
    HostSession Host(
        Pair.a, HostCore, HostTrust, SessionNonce, {}, nullptr,
        DisplayTopologyExchangeOptions{true, &Clock});
    EnvelopeHeader PreAdmissionHeader;
    PreAdmissionHeader.session_nonce = SessionNonce;
    PreAdmissionHeader.sequence = 1;
    CHECK(!Pair.a->send_reliable(encode_packet(
        PreAdmissionHeader,
        DisplayTopologySnapshotMessage{
            AgentView.identity.machine_id, SessionNonce,
            MakeDisplayTopology("display-a", "A")})));
    CHECK(Agent.start());
    CHECK(Host.start());
    CHECK(Agent.DisplayTopologyStatus() ==
          DisplayTopologyExchangeStatus::Synchronizing);
    CHECK(Host.DisplayTopologyStatus() ==
          DisplayTopologyExchangeStatus::Synchronizing);

    const auto TopologyA = MakeDisplayTopology("display-a", "A");
    const auto TopologyB = MakeDisplayTopology("display-b", "B");
    CHECK(Host.PublishDisplayTopology(
        AgentView.identity.machine_id, TopologyA));
    CHECK(Agent.PublishDisplayTopology(
        HostView.identity.machine_id, TopologyB));
    CHECK(!Host.PublishDisplayTopology(
        MakeMachineId(99), TopologyA));
    CHECK(Agent.DisplayTopologyStatus() ==
          DisplayTopologyExchangeStatus::Ready);
    CHECK(Host.DisplayTopologyStatus() ==
          DisplayTopologyExchangeStatus::Ready);
    CHECK(Agent.RemoteDisplayTopology() == TopologyA);
    CHECK(Host.RemoteDisplayTopology() == TopologyB);
    CHECK(Agent.stats().TopologyAccepted == 1);
    CHECK(Host.stats().TopologyAccepted == 1);

    auto TopologyATwo = TopologyA;
    TopologyATwo.Generation = 2;
    CHECK(Host.PublishDisplayTopology(
        AgentView.identity.machine_id, TopologyATwo));
    CHECK(Agent.RemoteDisplayTopology()->Generation == 2);
    EnvelopeHeader Header;
    Header.session_nonce = SessionNonce;
    Header.sequence = 90;
    CHECK(Pair.a->send_reliable(encode_packet(
        Header,
        DisplayTopologySnapshotMessage{
            AgentView.identity.machine_id, SessionNonce, TopologyA})));
    CHECK(Agent.DisplayTopologyStatus() ==
          DisplayTopologyExchangeStatus::Ready);
    CHECK(Agent.RemoteDisplayTopology()->Generation == 2);

    auto Conflicting = TopologyATwo;
    Conflicting.Displays[0].FriendlyName = "Conflicting metadata";
    CHECK(IsValidDisplayTopologySnapshot(Conflicting));
    Header.sequence = 91;
    CHECK(Pair.a->send_reliable(encode_packet(
        Header,
        DisplayTopologySnapshotMessage{
            AgentView.identity.machine_id, SessionNonce, Conflicting})));
    CHECK(Agent.DisplayTopologyStatus() ==
          DisplayTopologyExchangeStatus::Rejected);
    CHECK(!Agent.RemoteDisplayTopology().has_value());

    Header.session_nonce = SessionNonce + 1;
    Header.sequence = 92;
    CHECK(Pair.b->send_reliable(encode_packet(
        Header,
        DisplayTopologySnapshotMessage{
            HostView.identity.machine_id, SessionNonce + 1, TopologyB})));
    CHECK(Host.DisplayTopologyStatus() ==
          DisplayTopologyExchangeStatus::Rejected);
    CHECK(!Host.RemoteDisplayTopology().has_value());
    CHECK(Agent.stats().TopologyRejected >= 2);
    CHECK(Host.stats().TopologyRejected == 1);

    Agent.stop();
    Host.stop();

    auto ReconnectPair = make_in_memory_transport_pair(HostView, AgentView);
    RecordingInjector ReconnectInjector;
    AgentCoordinator ReconnectAgentCore(Clock, ReconnectInjector);
    HostCoordinator ReconnectHostCore(SessionNonce + 1);
    AgentSession ReconnectAgent(
        ReconnectPair.b, ReconnectAgentCore, AgentTrust, SessionNonce + 1,
        DisplayTopologyExchangeOptions{true, &Clock});
    HostSession ReconnectHost(
        ReconnectPair.a, ReconnectHostCore, HostTrust, SessionNonce + 1,
        {}, nullptr, DisplayTopologyExchangeOptions{true, &Clock});
    CHECK(ReconnectAgent.start());
    CHECK(ReconnectHost.start());
    CHECK(ReconnectHost.PublishDisplayTopology(
        AgentView.identity.machine_id, TopologyA));
    CHECK(ReconnectAgent.PublishDisplayTopology(
        HostView.identity.machine_id, TopologyB));
    CHECK(ReconnectAgent.DisplayTopologyStatus() ==
          DisplayTopologyExchangeStatus::Ready);
    CHECK(ReconnectHost.DisplayTopologyStatus() ==
          DisplayTopologyExchangeStatus::Ready);

    auto Malformed = encode_packet(
        EnvelopeHeader{.session_nonce = SessionNonce + 1, .sequence = 100},
        DisplayTopologySnapshotMessage{
            AgentView.identity.machine_id, SessionNonce + 1, TopologyA});
    Malformed.pop_back();
    CHECK(ReconnectPair.a->send_reliable(std::move(Malformed)));
    CHECK(ReconnectAgent.DisplayTopologyStatus() ==
          DisplayTopologyExchangeStatus::Rejected);
    CHECK(!ReconnectAgent.RemoteDisplayTopology().has_value());
}

void AudioSessionRequiresCapabilitiesNonceAndFormat() {
    using namespace desklink;
    constexpr std::uint64_t Nonce = 0x1234'5678u;
    TransportPeerInfo HostView;
    HostView.authenticated = true;
    HostView.encrypted = true;
    HostView.identity = MakeIdentity(31, "Audio sender");
    TransportPeerInfo AgentView;
    AgentView.authenticated = true;
    AgentView.encrypted = true;
    AgentView.identity = MakeIdentity(32, "Audio receiver");

    {
        auto Pair = make_in_memory_transport_pair(HostView, AgentView);
        ManualClock Clock;
        RecordingInjector Injector;
        AgentCoordinator AgentCore(Clock, Injector);
        HostCoordinator HostCore(Nonce);
        InMemoryTrustStore HostTrust;
        InMemoryTrustStore AgentTrust;
        SaveTrustedPeer(HostTrust, HostView.identity);
        SaveTrustedPeer(AgentTrust, AgentView.identity);
        AudioReceiver Receiver(
            [](AudioFrameMessage) { return true; }, 1, 2);
        AgentSession Agent(Pair.b, AgentCore, AgentTrust, Nonce);
        HostSession Host(Pair.a, HostCore, HostTrust, Nonce, {}, &Receiver);
        CHECK(Agent.start());
        CHECK(Host.start());
        CHECK(!Agent.CanSendAudio());
        CHECK(!Host.CanReceiveAudio());
        AudioFrameMessage Frame;
        Frame.stream_id = 1;
        Frame.pcm.assign(kDeskLinkAudioBytesPerBlock, 0x11);
        CHECK(!Agent.SendAudioFrame(Frame));
        EnvelopeHeader Header;
        Header.session_nonce = Nonce;
        Header.sequence = 1;
        CHECK(Pair.b->send_datagram(encode_packet(Header, Frame)));
        CHECK(Host.stats().AudioRejected == 1);
        CHECK(Receiver.Stats().Accepted == 0);
    }

    auto Pair = make_in_memory_transport_pair(HostView, AgentView);
    CapabilitySet HostCapabilities;
    HostCapabilities.grant(Capability::AudioSend);
    InMemoryTrustStore HostTrust;
    SaveTrustedPeer(HostTrust, HostView.identity, HostCapabilities);
    HostCoordinator HostCore(Nonce);
    AudioReceiver Receiver(
        [](AudioFrameMessage) { return true; }, 1, 2);
    HostSession Host(Pair.a, HostCore, HostTrust, Nonce, {}, &Receiver);
    CHECK(Host.start());
    CHECK(Host.CanReceiveAudio());
    AudioFrameMessage Frame;
    Frame.stream_id = 7;
    Frame.pcm.assign(kDeskLinkAudioBytesPerBlock, 0x22);
    EnvelopeHeader Header;
    Header.session_nonce = Nonce + 1;
    Header.sequence = 1;
    CHECK(Pair.b->send_datagram(encode_packet(Header, Frame)));
    CHECK(Host.stats().session_rejected == 1);
    Header.session_nonce = Nonce;
    auto Invalid = Frame;
    Invalid.channels = 1;
    CHECK(Pair.b->send_datagram(encode_packet(Header, Invalid)));
    CHECK(Host.stats().decode_rejected == 1);
    CHECK(Host.stats().AudioRejected == 2);
    CHECK(Pair.b->send_datagram(encode_packet(Header, Frame)));
    CHECK(Host.stats().AudioAccepted == 1);
    Header.sequence = 2;
    Frame.stream_id = 8;
    CHECK(Pair.b->send_datagram(encode_packet(Header, Frame)));
    CHECK(Receiver.Stats().StreamRejected == 1);
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
    pinned.machine_id = DeriveMachineId(MakeDigest(99));
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

    const auto first_commitment = first.CreateCommitment(*first_offer, true);
    const auto second_commitment = second.CreateCommitment(*second_offer, false);
    CHECK(first_commitment.has_value() && second_commitment.has_value());
    CHECK(first.VerifyCommitment(*second_commitment, *second_offer, false));
    CHECK(second.VerifyCommitment(*first_commitment, *first_offer, true));
    CHECK(!first.VerifyCommitment(*second_commitment, *second_offer, true));
    const auto first_candidate = first.InspectOffer(*first_offer, *second_offer, true);
    const auto second_candidate = second.InspectOffer(*second_offer, *first_offer, false);
    CHECK(first_candidate.Status == PairingStatus::Ready);
    CHECK(second_candidate.Status == PairingStatus::Ready);
    CHECK(first_candidate.VerificationCode == second_candidate.VerificationCode);
    CHECK(first_candidate.VerificationCode.size() == 6);
    CHECK(!first.ConfirmOffer(
        *first_offer, *second_offer, true, "000000", CapabilitySet{}));

    CapabilitySet grant_to_second;
    grant_to_second.grant(Capability::InputInject);
    CHECK(first.ConfirmOffer(
        *first_offer, *second_offer, true,
        first_candidate.VerificationCode, grant_to_second));
    CHECK(second.ConfirmOffer(
        *second_offer, *first_offer, false,
        second_candidate.VerificationCode, CapabilitySet{}));
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
    const auto second_commitment = second.CreateCommitment(*second_offer, false);
    CHECK(second_commitment.has_value());
    CHECK(!first.VerifyCommitment(*second_commitment, tampered_offer, false));
    const auto genuine_code = second.InspectOffer(
        *second_offer, *first_offer, false).VerificationCode;

    clock.advance(std::chrono::milliseconds(5001));
    CHECK(!first.IsPairingOpen());
    CHECK(first.InspectOffer(*first_offer, *second_offer, true).Status ==
          PairingStatus::WindowClosed);
    CHECK(!first.ConfirmOffer(
        *first_offer, *second_offer, true, genuine_code, CapabilitySet{}));
}

void PairingWireIsBoundedAndFragmentSafe() {
    using namespace desklink;
    PairingOffer Offer{
        DeriveMachineId(MakeDigest(31)), "DeskLink peer", MakeDigest(31), {}};
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

    CapabilitySet ConfirmedCapabilities;
    ConfirmedCapabilities.grant(Capability::InputInject);
    const auto Confirmation = EncodePairingConfirmationFrame(
        MakeDigest(90), ConfirmedCapabilities);
    CHECK(Confirmation.size() ==
          kPairingFrameHeaderSize + kSha256DigestSize + sizeof(std::uint64_t));
    ByteBuffer Combined = *Frame;
    Combined.insert(Combined.end(), Confirmation.begin(), Confirmation.end());
    Decoder.Reset();
    CHECK(Decoder.Push(Combined) == PairingWireStatus::Ready);
    CHECK(Decoder.ReadyType() == PairingWireFrameType::Offer);
    CHECK(Decoder.TakeOffer().has_value());
    CHECK(Decoder.Status() == PairingWireStatus::Ready);
    CHECK(Decoder.ReadyType() == PairingWireFrameType::Confirmation);
    const auto DecodedConfirmation = Decoder.TakeConfirmation();
    CHECK(DecodedConfirmation.has_value());
    CHECK(DecodedConfirmation->TranscriptDigest == MakeDigest(90));
    CHECK(DecodedConfirmation->Capabilities.contains(Capability::InputInject));
    CHECK(Decoder.Status() == PairingWireStatus::Incomplete);

    Decoder.Reset();
    CHECK(Decoder.Push(ByteSpan{Confirmation.data(), 2}) ==
          PairingWireStatus::Incomplete);
    CHECK(Decoder.Push(ByteSpan{Confirmation.data() + 2, Confirmation.size() - 2}) ==
          PairingWireStatus::Ready);
    CHECK(Decoder.TakeConfirmation().has_value());

    auto BadMagic = *Frame;
    BadMagic[0] ^= 0xFFu;
    CHECK(!DecodePairingOfferFrame(BadMagic));
    auto VersionOne = *Frame;
    VersionOne[4] = 1;
    CHECK(!DecodePairingOfferFrame(VersionOne));
    auto ExtraByte = *Frame;
    ExtraByte.push_back(0);
    CHECK(!DecodePairingOfferFrame(ExtraByte));
    auto ConfirmationWithBody = Confirmation;
    ConfirmationWithBody[7] = static_cast<std::uint8_t>(
        ConfirmationWithBody[7] - 1);
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
    identity.machine_id = DeriveMachineId(*digest);
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

    CHECK(ParseDeskModeName("roam") == DeskMode::Roam);
    CHECK(ParseDeskModeName("lock-pc1") == DeskMode::LockPc1);
    CHECK(ParseDeskModeName("lock-pc2") == DeskMode::LockPc2);
    CHECK(ParseDeskModeName("game") == DeskMode::Game);
    CHECK(!ParseDeskModeName("lock"));
    const auto ParsedRule = ParseForegroundProfileRule(
        "GAME.EXE=game", true);
    CHECK(ParsedRule.has_value());
    CHECK(ParsedRule->ExecutableName == "game.exe");
    CHECK(ParsedRule->Mode == DeskMode::Game);
    CHECK(ParsedRule->FullscreenOnly);
    CHECK(!ParseForegroundProfileRule("game.exe"));
    CHECK(!ParseForegroundProfileRule("bad/path.exe=game"));
    CHECK(!ParseForegroundProfileRule("game.exe=unknown"));

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

    ForegroundProfileEngine FullscreenPolicy;
    FullscreenPolicy.SetKeepLocalWhenFullscreen(true);
    CHECK(FullscreenPolicy.RequiresForegroundObservation());
    CHECK(FullscreenPolicy.Decision().Mode == DeskMode::LockPc1);
    CHECK(FullscreenPolicy.Decision().Source ==
          ProfileModeSource::ForegroundUnavailable);
    FullscreenPolicy.SetForeground(ForegroundWindowSnapshot{
        20, "browser.exe", false, true});
    CHECK(FullscreenPolicy.Decision().Mode == DeskMode::Roam);
    FullscreenPolicy.SetForeground(ForegroundWindowSnapshot{
        21, "game.exe", true, true});
    CHECK(FullscreenPolicy.Decision().Mode == DeskMode::Game);
    CHECK(FullscreenPolicy.Decision().Source ==
          ProfileModeSource::FullscreenPolicy);
    CHECK(FullscreenPolicy.SetManualOverride(DeskMode::Roam));
    CHECK(FullscreenPolicy.Decision().Source ==
          ProfileModeSource::ManualOverride);

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

    Backend.Calls.clear();
    CHECK(Lifecycle.ReturnLocal());
    CHECK((Backend.Calls == std::vector<std::string>{
        "disable-capture", "release-focus", "stop-capture"}));
    CHECK(Lifecycle.Status().Mode == DeskMode::Roam);
    CHECK(Lifecycle.Status().State == HostInputLifecycleState::Local);

    Backend.Calls.clear();
    CHECK(Lifecycle.ApplyMode(DeskMode::Roam));
    CHECK((Backend.Calls == std::vector<std::string>{
        "set-mode-0", "request-focus"}));
    CHECK(Lifecycle.Status().State ==
          HostInputLifecycleState::AwaitingFocus);

    Backend.Calls.clear();
    CHECK(Lifecycle.ApplyMode(DeskMode::Roam));
    CHECK(Backend.Calls.empty());
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

void ProductPreferencesAndPlannerAreStrictAndFailLocal() {
    using namespace desklink;

    const MachineId Peer{1};
    ProductPreferences Preferences;
    Preferences.Role = DeskRole::Main;
    Preferences.PreferredPeerMachine = Peer;
    Preferences.PreferredPeerEndpoint =
        ProductPeerEndpoint{"192.168.0.108", 43'821};
    Preferences.AutoStartRuntime = true;
    Preferences.AutoConnect = true;
    Preferences.InputRoamingDesired = true;
    Preferences.ClipboardDesired = true;
    Preferences.AudioRoute = AudioRoutePreference::Bidirectional;
    Preferences.AudioGainPermyriad = 7'500;
    Preferences.VoiceRoute = VoiceRoutePreference::Bidirectional;
    Preferences.VoiceInputEndpointId = "communications-microphone";
    Preferences.VoiceGainPermyriad = 6'500;
    Preferences.VoiceEchoGuard = true;
    Preferences.FocusPeerHotkey = ProductHotkey::CtrlAltF11;
    Preferences.ReturnLocalHotkey = ProductHotkey::CtrlAltF12;
    Preferences.ProfileRules.push_back(
        ForegroundProfileRule{"game.exe", DeskMode::Game, true});

    CapabilitySet LocalFeatureGrants;
    CHECK(!CanEnableClipboardIntent(LocalFeatureGrants));
    CHECK(!CanEnablePeerAudioIntent(LocalFeatureGrants));
    CHECK(!CanEnableLocalAudioIntent(LocalFeatureGrants));
    CHECK(!CanEnablePeerVoiceIntent(LocalFeatureGrants));
    CHECK(!CanEnableLocalVoiceIntent(LocalFeatureGrants));
    LocalFeatureGrants.grant(Capability::ClipboardRead);
    LocalFeatureGrants.grant(Capability::ClipboardWrite);
    CHECK(CanEnableClipboardIntent(LocalFeatureGrants));
    LocalFeatureGrants.grant(Capability::AudioSend);
    CHECK(CanEnablePeerAudioIntent(LocalFeatureGrants));
    CHECK(!CanEnableLocalAudioIntent(LocalFeatureGrants));
    LocalFeatureGrants.grant(Capability::AudioReceive);
    CHECK(CanEnableLocalAudioIntent(LocalFeatureGrants));
    LocalFeatureGrants.grant(Capability::VoiceSend);
    CHECK(CanEnablePeerVoiceIntent(LocalFeatureGrants));
    CHECK(!CanEnableLocalVoiceIntent(LocalFeatureGrants));
    LocalFeatureGrants.grant(Capability::VoiceReceive);
    CHECK(CanEnableLocalVoiceIntent(LocalFeatureGrants));

    RuntimePlannerContext Ready;
    Ready.PreferredPeerTrusted = true;
    Ready.PeerValidated = true;
    Ready.RoamingRouteReady = true;
    for (const auto CapabilityValue : {
             Capability::DisplayTopologyExchange,
             Capability::ClipboardRead,
             Capability::ClipboardWrite,
             Capability::AudioSend,
             Capability::AudioReceive,
             Capability::VoiceSend,
             Capability::VoiceReceive}) {
        Ready.LocalGrantsToPeer.grant(CapabilityValue);
        Ready.PeerGrantsToLocal.grant(CapabilityValue);
    }
    Ready.PeerGrantsToLocal.grant(Capability::InputInject);

    const auto Main = PlanDesiredDeskConfiguration(Preferences, Ready);
    CHECK(Main.PreferencesValid);
    CHECK(Main.StartRuntime);
    CHECK(!Main.Listen);
    CHECK(Main.ConnectPreferredPeer);
    CHECK(Main.EnableInputRoaming);
    CHECK(Main.EnableClipboard);
    CHECK(Main.SendAudio);
    CHECK(Main.ReceiveAudio);
    CHECK(Main.SendVoice);
    CHECK(Main.ReceiveVoice);
    CHECK(Main.InitialMode == DeskMode::LockPc1);
    CHECK(Main.AudioGainPermyriad == 7'500);
    CHECK(Main.VoiceGainPermyriad == 6'500);
    CHECK(Main.Blockers == 0);

    Preferences.Role = DeskRole::Companion;
    const auto Companion = PlanDesiredDeskConfiguration(Preferences, Ready);
    CHECK(Companion.StartRuntime);
    CHECK(Companion.Listen);
    CHECK(!Companion.ConnectPreferredPeer);
    CHECK(Companion.InitialMode == DeskMode::LockPc1);

    Preferences.Role = DeskRole::Flexible;
    const auto Flexible = PlanDesiredDeskConfiguration(Preferences, Ready);
    CHECK(Flexible.Listen);
    CHECK(Flexible.ConnectPreferredPeer);
    CHECK(Flexible.InitialMode == DeskMode::LockPc1);
    CHECK(Ready.LocalGrantsToPeer.bits() ==
          (static_cast<std::uint64_t>(Capability::DisplayTopologyExchange) |
           static_cast<std::uint64_t>(Capability::ClipboardRead) |
           static_cast<std::uint64_t>(Capability::ClipboardWrite) |
           static_cast<std::uint64_t>(Capability::AudioSend) |
           static_cast<std::uint64_t>(Capability::AudioReceive) |
           static_cast<std::uint64_t>(Capability::VoiceSend) |
           static_cast<std::uint64_t>(Capability::VoiceReceive)));
    CHECK(Ready.PeerGrantsToLocal.contains(Capability::InputInject));

    Preferences.Role = DeskRole::Unconfigured;
    const auto Unconfigured =
        PlanDesiredDeskConfiguration(Preferences, Ready);
    CHECK(!Unconfigured.StartRuntime);
    CHECK(HasRuntimePlanBlocker(
        Unconfigured, RuntimePlanBlocker::RoleUnconfigured));

    Preferences.Role = DeskRole::Main;
    Preferences.AutoStartRuntime = false;
    const auto Disabled = PlanDesiredDeskConfiguration(Preferences, Ready);
    CHECK(!Disabled.StartRuntime);
    CHECK(!Disabled.ConnectPreferredPeer);
    CHECK(HasRuntimePlanBlocker(
        Disabled, RuntimePlanBlocker::RuntimeDisabled));

    Preferences.AutoStartRuntime = true;
    Preferences.PreferredPeerMachine.reset();
    Preferences.PreferredPeerEndpoint.reset();
    const auto MissingPeer = PlanDesiredDeskConfiguration(Preferences, Ready);
    CHECK(MissingPeer.StartRuntime);
    CHECK(!MissingPeer.ConnectPreferredPeer);
    CHECK(!MissingPeer.EnableInputRoaming);
    CHECK(HasRuntimePlanBlocker(
        MissingPeer, RuntimePlanBlocker::PreferredPeerMissing));

    Preferences.PreferredPeerMachine = Peer;
    Preferences.PreferredPeerEndpoint =
        ProductPeerEndpoint{"192.168.0.108", 43'821};
    auto UntrustedContext = Ready;
    UntrustedContext.PreferredPeerTrusted = false;
    const auto Untrusted =
        PlanDesiredDeskConfiguration(Preferences, UntrustedContext);
    CHECK(!Untrusted.ConnectPreferredPeer);
    CHECK(!Untrusted.EnableClipboard);
    CHECK(HasRuntimePlanBlocker(
        Untrusted, RuntimePlanBlocker::PeerNotTrusted));

    auto AuthenticatingContext = Ready;
    AuthenticatingContext.PeerValidated = false;
    const auto Authenticating =
        PlanDesiredDeskConfiguration(Preferences, AuthenticatingContext);
    CHECK(Authenticating.ConnectPreferredPeer);
    CHECK(!Authenticating.EnableInputRoaming);
    CHECK(!Authenticating.EnableClipboard);
    CHECK(!Authenticating.SendAudio);
    CHECK(!Authenticating.ReceiveAudio);
    CHECK(!Authenticating.SendVoice);
    CHECK(!Authenticating.ReceiveVoice);
    CHECK(HasRuntimePlanBlocker(
        Authenticating, RuntimePlanBlocker::PeerNotValidated));

    RuntimePlannerContext MissingGrants;
    MissingGrants.PreferredPeerTrusted = true;
    MissingGrants.PeerValidated = true;
    const auto Denied =
        PlanDesiredDeskConfiguration(Preferences, MissingGrants);
    CHECK(Denied.ConnectPreferredPeer);
    CHECK(!Denied.EnableInputRoaming);
    CHECK(!Denied.EnableClipboard);
    CHECK(!Denied.SendAudio);
    CHECK(!Denied.ReceiveAudio);
    CHECK(!Denied.SendVoice);
    CHECK(!Denied.ReceiveVoice);
    CHECK(HasRuntimePlanBlocker(
        Denied, RuntimePlanBlocker::InputCapabilityMissing));
    CHECK(HasRuntimePlanBlocker(
        Denied, RuntimePlanBlocker::TopologyCapabilityMissing));
    CHECK(HasRuntimePlanBlocker(
        Denied, RuntimePlanBlocker::RoamingRouteUnavailable));
    CHECK(HasRuntimePlanBlocker(
        Denied, RuntimePlanBlocker::ClipboardCapabilityMissing));
    CHECK(HasRuntimePlanBlocker(
        Denied, RuntimePlanBlocker::AudioCapabilityMissing));
    CHECK(HasRuntimePlanBlocker(
        Denied, RuntimePlanBlocker::VoiceCapabilityMissing));

    auto Malformed = Preferences;
    Malformed.Role = static_cast<DeskRole>(0xffu);
    const auto InvalidRole = PlanDesiredDeskConfiguration(Malformed, Ready);
    CHECK(!InvalidRole.PreferencesValid);
    CHECK(!InvalidRole.StartRuntime);
    CHECK(HasRuntimePlanBlocker(
        InvalidRole, RuntimePlanBlocker::InvalidPreferences));

    Malformed = Preferences;
    Malformed.PreferredPeerMachine = MachineId{};
    CHECK(!IsValidProductPreferences(Malformed));
    Malformed = Preferences;
    Malformed.PreferredPeerMachine.reset();
    CHECK(!IsValidProductPreferences(Malformed));
    Malformed = Preferences;
    Malformed.PreferredPeerEndpoint->Port = 0;
    CHECK(!IsValidProductPreferences(Malformed));
    Malformed = Preferences;
    Malformed.PreferredPeerEndpoint->Host = "host with spaces";
    CHECK(!IsValidProductPreferences(Malformed));
    Malformed = Preferences;
    Malformed.PreferredPeerEndpoint->Host = "192.168.0.108:43821";
    CHECK(!IsValidProductPreferences(Malformed));
    Malformed = Preferences;
    Malformed.PreferredPeerEndpoint->Host = "[2001:db8::1]";
    CHECK(IsValidProductPreferences(Malformed));
    Malformed = Preferences;
    Malformed.PreferredPeerEndpoint->Host.assign(
        kMaximumPreferredPeerHostBytes + 1, 'a');
    CHECK(!IsValidProductPreferences(Malformed));
    Malformed = Preferences;
    Malformed.AudioGainPermyriad = 10'001;
    CHECK(!IsValidProductPreferences(Malformed));
    Malformed = Preferences;
    Malformed.VoiceGainPermyriad = 10'001;
    CHECK(!IsValidProductPreferences(Malformed));
    Malformed = Preferences;
    Malformed.VoiceRoute = static_cast<VoiceRoutePreference>(0xffu);
    CHECK(!IsValidProductPreferences(Malformed));
    Malformed = Preferences;
    Malformed.VoiceInputEndpointId = std::string(
        kMaximumVoiceEndpointIdBytes + 1, 'a');
    CHECK(!IsValidProductPreferences(Malformed));
    Malformed = Preferences;
    Malformed.ReturnLocalHotkey = Malformed.FocusPeerHotkey;
    CHECK(!IsValidProductPreferences(Malformed));
    Malformed = Preferences;
    Malformed.FocusPeerHotkey = static_cast<ProductHotkey>(0xffu);
    CHECK(!IsValidProductPreferences(Malformed));
    Malformed = Preferences;
    Malformed.ProfileRules.push_back(
        ForegroundProfileRule{"GAME.EXE", DeskMode::Roam, true});
    CHECK(!IsValidProductPreferences(Malformed));

    RoamingConfiguration CrossingPresets;
    CrossingPresets.Links.push_back(RoamingLink{
        {MakeMachineId(1), "left", DisplayEdgeSide::Right, 0, 10'000},
        {MakeMachineId(2), "right", DisplayEdgeSide::Left, 0, 10'000}});
    CHECK(ApplyProductCrossingPreset(
        CrossingPresets, ProductCrossingPreset::CrossImmediately));
    CHECK(CrossingPresets.CrossingDefaults.Policy == CrossingPolicy::Push);
    CHECK(CrossingPresets.Links.front().AToB ==
          CrossingPresets.CrossingDefaults);
    CHECK(ApplyProductCrossingPreset(
        CrossingPresets, ProductCrossingPreset::PauseAndPush));
    CHECK(CrossingPresets.CrossingDefaults.Policy ==
          CrossingPolicy::DwellAndPush);
    CHECK(CrossingPresets.CrossingDefaults.DwellMilliseconds == 180);
    CHECK(ApplyProductCrossingPreset(
        CrossingPresets, ProductCrossingPreset::PushTwice));
    CHECK(CrossingPresets.CrossingDefaults.Policy ==
          CrossingPolicy::DoublePush);
    CHECK(CrossingPresets.CrossingDefaults.DoublePushWindowMilliseconds ==
          600);
    CHECK(!ApplyProductCrossingPreset(
        CrossingPresets, static_cast<ProductCrossingPreset>(0xffu)));
}

#ifdef _WIN32
void WindowsDiscoveryCancellationIsBounded() {
    using namespace desklink;

    std::stop_source StopSource;
    StopSource.request_stop();
    const auto Started = std::chrono::steady_clock::now();
    const auto Result = Win32MdnsBrowser::Browse(
        std::chrono::seconds(30), StopSource.get_token());
    const auto Elapsed = std::chrono::steady_clock::now() - Started;
    CHECK(Result.StartStatus == ERROR_CANCELLED);
    CHECK(Result.Peers.empty());
    CHECK(Elapsed < std::chrono::seconds(1));
}

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
    ControlTopologyState Topologies;
    Topologies.Machines.push_back(ControlMachineTopology{
        State.LocalMachine, DisplayTopologyExchangeStatus::Ready,
        MakeDisplayTopology("pipe-local", "Pipe local display"), true, false});
    Win32ControlPipeServer Server(
        [State, Topologies](const ControlRequest& Request) {
            if (std::holds_alternative<GetStateControlRequest>(Request.Payload)) {
                return ControlResponse{
                    Request.RequestId, ControlStatus::Ok, State};
            }
            if (std::holds_alternative<GetDisplayTopologiesControlRequest>(
                    Request.Payload)) {
                return ControlResponse{
                    Request.RequestId, ControlStatus::Ok, std::nullopt,
                    Topologies};
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

    const auto PipeName = GetWin32ControlPipeName(Instance);
    CHECK(PipeName.has_value());
    CHECK(WaitNamedPipeW(PipeName->c_str(), 1'000));
    const auto RawPipe = CreateFileW(
        PipeName->c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, 0, nullptr);
    CHECK(RawPipe != INVALID_HANDLE_VALUE);
    auto OversizedFrame = *EncodeControlRequest(
        ControlRequest{99, GetStateControlRequest{}});
    OversizedFrame.resize(kControlFrameHeaderSize);
    const auto OversizedPayload = static_cast<std::uint32_t>(
        kMaximumControlPayload + 1);
    OversizedFrame[16] = static_cast<std::uint8_t>(OversizedPayload >> 24u);
    OversizedFrame[17] = static_cast<std::uint8_t>(OversizedPayload >> 16u);
    OversizedFrame[18] = static_cast<std::uint8_t>(OversizedPayload >> 8u);
    OversizedFrame[19] = static_cast<std::uint8_t>(OversizedPayload);
    DWORD Written{};
    CHECK(WriteFile(RawPipe, OversizedFrame.data(),
                    static_cast<DWORD>(OversizedFrame.size()),
                    &Written, nullptr));
    CHECK(static_cast<std::size_t>(Written) == OversizedFrame.size());
    std::uint8_t Unexpected{};
    DWORD Read{};
    CHECK(!ReadFile(RawPipe, &Unexpected, 1, &Read, nullptr) || Read == 0);
    CloseHandle(RawPipe);

    const auto Response = Win32ControlPipeClient::Send(
        ControlRequest{101, GetStateControlRequest{}}, Instance);
    CHECK(Response.has_value());
    CHECK(Response->Status == ControlStatus::Ok);
    CHECK(Response->State.has_value());
    CHECK(Response->State->LocalMachine == State.LocalMachine);
    CHECK(Response->State->ConnectedPeerCount == 2);

    const auto TopologyResponse = Win32ControlPipeClient::Send(
        ControlRequest{104, GetDisplayTopologiesControlRequest{}}, Instance);
    CHECK(TopologyResponse.has_value());
    CHECK(TopologyResponse->Status == ControlStatus::Ok);
    CHECK(TopologyResponse->Topologies == Topologies);

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

void WindowsClipboardListenerLifecycleIsContentSilent() {
    using namespace desklink;
    std::atomic_uint32_t Published{};
    std::atomic_uint32_t Failed{};
    Win32ClipboardSynchronizer Clipboard(
        Win32ClipboardHandlers{
            [&](std::string) {
                ++Published;
                return true;
            },
            [&](std::string) { ++Failed; }});
    Clipboard.SetLocalPublishing(false);
    CHECK(Clipboard.Start());
    CHECK(Clipboard.Running());
    Clipboard.Stop();
    CHECK(!Clipboard.Running());
    const auto Statistics = Clipboard.Stats();
    CHECK(Published.load() == 0);
    CHECK(Failed.load() == 0);
    CHECK(Statistics.LocalPublished == 0);
    CHECK(Statistics.RemoteApplied == 0);
}

void WindowsAlphaLauncherCommandsAreBoundedAndProductionPinned() {
    using namespace desklink;

    LauncherRequest Focus;
    Focus.Operation = LauncherOperation::Focus;
    Focus.Host = L"desklink-peer.local";
    Focus.Port = 43'821;
    Focus.CaptureInput = true;
    Focus.ReceiveAudio = true;
    Focus.SyncClipboard = true;
    const auto FocusArguments = BuildLauncherArguments(Focus);
    CHECK(FocusArguments.has_value());
    const std::vector<std::wstring> ExpectedFocus{
        L"focus", L"desklink-peer.local", L"43821", L"--capture",
        L"--receive-audio", L"--sync-clipboard",
        L"--default-mode", L"lock-pc1",
        L"--tls-provider", L"schannel"};
    CHECK(*FocusArguments == ExpectedFocus);

    Focus.TlsProvider = LauncherTlsProvider::Auto;
    const auto AutoFocusArguments = BuildLauncherArguments(Focus);
    CHECK(AutoFocusArguments.has_value());
    CHECK(AutoFocusArguments->back() == L"auto");
    Focus.TlsProvider = LauncherTlsProvider::OpenSsl;
    const auto OpenSslFocusArguments = BuildLauncherArguments(Focus);
    CHECK(OpenSslFocusArguments.has_value());
    CHECK(OpenSslFocusArguments->back() == L"openssl");
    Focus.TlsProvider = LauncherTlsProvider::Schannel;
    const auto ProductFocusArguments =
        BuildProductLauncherArguments(Focus);
    CHECK(ProductFocusArguments.has_value());
    CHECK(ProductFocusArguments->back() == L"auto");

    Focus.ExpectedPeerMachine = MakeMachineId(8);
    Focus.BrokerManaged = true;
    const auto ManagedFocusArguments = BuildLauncherArguments(Focus);
    CHECK(ManagedFocusArguments.has_value());
    const auto ExpectedPeer = std::find(
        ManagedFocusArguments->begin(), ManagedFocusArguments->end(),
        L"--expected-peer");
    CHECK(ExpectedPeer != ManagedFocusArguments->end());
    CHECK(ExpectedPeer + 1 != ManagedFocusArguments->end());
    CHECK(*(ExpectedPeer + 1) ==
          L"080000000000000000000000000000ad");
    CHECK(std::find(
              ManagedFocusArguments->begin(),
              ManagedFocusArguments->end(),
              L"--broker-managed") != ManagedFocusArguments->end());
    Focus.ExpectedPeerMachine.reset();
    Focus.BrokerManaged = false;

    Focus.PointerCalibration.GainPercent = 175;
    Focus.PointerCalibration.SourceDpi = 1'600;
    const auto CalibratedFocusArguments = BuildLauncherArguments(Focus);
    CHECK(CalibratedFocusArguments.has_value());
    const std::vector<std::wstring> ExpectedCalibratedFocus{
        L"focus", L"desklink-peer.local", L"43821", L"--capture",
        L"--pointer-gain", L"175", L"--pointer-dpi", L"1600",
        L"--receive-audio", L"--sync-clipboard",
        L"--default-mode", L"lock-pc1",
        L"--tls-provider", L"schannel"};
    CHECK(*CalibratedFocusArguments == ExpectedCalibratedFocus);
    Focus.PointerCalibration = {};

    Focus.ProfileDefaultMode = DeskMode::Roam;
    Focus.KeepLocalWhenFullscreen = true;
    Focus.ProfileRules = {
        ForegroundProfileRule{"game.exe", DeskMode::Game, true},
        ForegroundProfileRule{"editor.exe", DeskMode::LockPc1, false}};
    const auto ProfileArguments = BuildLauncherArguments(Focus);
    CHECK(ProfileArguments.has_value());
    CHECK(std::find(
              ProfileArguments->begin(), ProfileArguments->end(),
              L"--keep-local-fullscreen") != ProfileArguments->end());
    const auto FullscreenProfile = std::find(
        ProfileArguments->begin(), ProfileArguments->end(),
        L"--profile-fullscreen");
    CHECK(FullscreenProfile != ProfileArguments->end());
    CHECK(FullscreenProfile + 1 != ProfileArguments->end());
    CHECK(*(FullscreenProfile + 1) == L"game.exe=game");
    const auto ExactProfile = std::find(
        ProfileArguments->begin(), ProfileArguments->end(), L"--profile");
    CHECK(ExactProfile != ProfileArguments->end());
    CHECK(ExactProfile + 1 != ProfileArguments->end());
    CHECK(*(ExactProfile + 1) == L"editor.exe=lock-pc1");
    Focus.ProfileDefaultMode = DeskMode::LockPc1;
    Focus.KeepLocalWhenFullscreen = false;
    Focus.ProfileRules.clear();

    Focus.EdgeRoamingSettingsPath =
        L"C:\\Users\\test\\AppData\\Local\\DeskLink\\roaming.settings";
    const auto EdgeArguments = BuildLauncherArguments(Focus);
    CHECK(EdgeArguments.has_value());
    CHECK(std::find(
              EdgeArguments->begin(), EdgeArguments->end(),
              L"--edge-roaming") != EdgeArguments->end());
    CHECK(EdgeArguments->back() == L"schannel");
    Focus.EdgeRoamingSettingsPath = L"relative\\roaming.settings";
    CHECK(!BuildLauncherArguments(Focus).has_value());
    Focus.EdgeRoamingSettingsPath =
        L"C:\\Users\\test\\AppData\\Local\\DeskLink\\roaming.settings";
    Focus.CaptureInput = false;
    CHECK(!BuildLauncherArguments(Focus).has_value());
    Focus.CaptureInput = true;
    Focus.EdgeRoamingSettingsPath.clear();

    LauncherRequest Serve;
    Serve.Operation = LauncherOperation::Serve;
    Serve.SendAudio = true;
    Serve.SyncClipboard = true;
    Serve.CaptureInput = true;
    Serve.PointerCalibration.GainPercent = 125;
    Serve.PointerCalibration.SourceDpi = 800;
    Serve.EdgeRoamingSettingsPath =
        L"C:\\Users\\test\\AppData\\Local\\DeskLink\\roaming.settings";
    const auto ReciprocalServeArguments = BuildLauncherArguments(Serve);
    CHECK(ReciprocalServeArguments.has_value());
    const std::vector<std::wstring> ExpectedReciprocalServe{
        L"serve", L"43821", L"--send-audio", L"--sync-clipboard",
        L"--capture",
        L"--pointer-gain", L"125", L"--pointer-dpi", L"800",
        L"--edge-roaming",
        L"C:\\Users\\test\\AppData\\Local\\DeskLink\\roaming.settings",
        L"--tls-provider", L"schannel"};
    CHECK(*ReciprocalServeArguments == ExpectedReciprocalServe);
    Serve.EdgeRoamingSettingsPath.clear();
    CHECK(!BuildLauncherArguments(Serve).has_value());
    Serve.CaptureInput = false;
    CHECK(!BuildLauncherArguments(Serve).has_value());
    Serve.PointerCalibration = {};
    CHECK(BuildLauncherArguments(Serve).has_value());
    Serve.BrokerManaged = true;
    const auto ManagedServeArguments = BuildLauncherArguments(Serve);
    CHECK(ManagedServeArguments.has_value());
    CHECK(std::find(
              ManagedServeArguments->begin(),
              ManagedServeArguments->end(),
              L"--broker-managed") != ManagedServeArguments->end());
    Serve.ExpectedPeerMachine = MakeMachineId(8);
    CHECK(!BuildLauncherArguments(Serve).has_value());
    Serve.ExpectedPeerMachine.reset();
    Serve.BrokerManaged = false;
    Serve.ProfileRules.push_back(
        ForegroundProfileRule{"game.exe", DeskMode::Game, true});
    CHECK(!BuildLauncherArguments(Serve).has_value());
    Serve.ProfileRules.clear();

    LauncherRequest Pair;
    Pair.Operation = LauncherOperation::PairListen;
    const auto DefaultGrants = DefaultManualPairingGrants();
    CHECK(!DefaultGrants.GrantInput);
    CHECK(!DefaultGrants.GrantAudioSend);
    CHECK(!DefaultGrants.GrantAudioReceive);
    CHECK(!DefaultGrants.GrantTopology);
    CHECK(!DefaultGrants.GrantClipboardRead);
    CHECK(!DefaultGrants.GrantClipboardWrite);
    const auto DefaultPairArguments = BuildLauncherArguments(Pair);
    CHECK(DefaultPairArguments.has_value());
    const std::vector<std::wstring> ExpectedDefaultPair{
        L"listen", L"43821", L"--tls-provider", L"schannel"};
    CHECK(*DefaultPairArguments == ExpectedDefaultPair);

    LauncherRequest DefaultConnect;
    DefaultConnect.Operation = LauncherOperation::PairConnect;
    DefaultConnect.Host = L"desklink-peer.local";
    const auto DefaultConnectArguments = BuildLauncherArguments(DefaultConnect);
    CHECK(DefaultConnectArguments.has_value());
    const std::vector<std::wstring> ExpectedDefaultConnect{
        L"pair", L"desklink-peer.local", L"43821",
        L"--tls-provider", L"schannel"};
    CHECK(*DefaultConnectArguments == ExpectedDefaultConnect);

    Pair.GrantInput = true;
    Pair.GrantAudioReceive = true;
    Pair.GrantTopology = true;
    Pair.GrantClipboardRead = true;
    Pair.GrantClipboardWrite = true;
    const auto PairArguments = BuildLauncherArguments(Pair);
    CHECK(PairArguments.has_value());
    const std::vector<std::wstring> ExpectedPair{
        L"listen", L"43821", L"--grant-input",
        L"--grant-audio-receive", L"--grant-topology",
        L"--grant-clipboard-read", L"--grant-clipboard-write",
        L"--tls-provider", L"schannel"};
    CHECK(*PairArguments == ExpectedPair);

    ControlPairingToken PairingToken{};
    PairingToken[0] = 0x12;
    PairingToken[15] = 0xab;
    Pair.BrokerPairingOperationId = 77;
    Pair.BrokerPairingToken = PairingToken;
    const auto BrokerPairArguments = BuildLauncherArguments(Pair);
    CHECK(BrokerPairArguments.has_value());
    const auto BrokerPairingFlag = std::find(
        BrokerPairArguments->begin(), BrokerPairArguments->end(),
        L"--broker-pairing");
    CHECK(BrokerPairingFlag != BrokerPairArguments->end());
    CHECK(BrokerPairingFlag + 2 < BrokerPairArguments->end());
    CHECK(*(BrokerPairingFlag + 1) == L"77");
    CHECK(*(BrokerPairingFlag + 2) ==
          L"120000000000000000000000000000ab");
    Pair.BrokerPairingToken.reset();
    CHECK(!BuildLauncherArguments(Pair).has_value());
    Pair.BrokerPairingOperationId.reset();
    Pair.BrokerPairingToken = ControlPairingToken{};
    Pair.BrokerPairingOperationId = 78;
    CHECK(!BuildLauncherArguments(Pair).has_value());
    Pair.BrokerPairingToken.reset();
    Pair.BrokerPairingOperationId.reset();

    Focus.Host = L"host with spaces";
    CHECK(!BuildLauncherArguments(Focus).has_value());
    Focus.Host = L"192.168.0.108:43821";
    CHECK(!BuildLauncherArguments(Focus).has_value());
    Focus.Host = L"[::1]:43821";
    CHECK(!BuildLauncherArguments(Focus).has_value());
    Focus.Host = L"[::1]";
    CHECK(BuildLauncherArguments(Focus).has_value());
    Focus.Host = L"desklink-peer.local";
    Focus.GrantInput = true;
    CHECK(!BuildLauncherArguments(Focus).has_value());
    Pair.SyncClipboard = true;
    CHECK(!BuildLauncherArguments(Pair).has_value());
    Pair.SyncClipboard = false;
    Pair.BrokerManaged = true;
    CHECK(!BuildLauncherArguments(Pair).has_value());
    LauncherRequest Identity;
    Identity.Operation = LauncherOperation::Identity;
    Identity.SyncClipboard = true;
    CHECK(!BuildLauncherArguments(Identity).has_value());

    CHECK(QuoteWindowsCommandArgument(L"plain") == L"plain");
    CHECK(QuoteWindowsCommandArgument(L"") == L"\"\"");
    CHECK(QuoteWindowsCommandArgument(L"two words") == L"\"two words\"");
    CHECK(QuoteWindowsCommandArgument(L"a\"b") == L"\"a\\\"b\"");
    CHECK(QuoteWindowsCommandArgument(L"C:\\Program Files\\") ==
          L"\"C:\\Program Files\\\\\"");
    const auto CommandLine = BuildWindowsCommandLine(
        L"C:\\Program Files\\DeskLink\\desklink_pair.exe",
        *PairArguments);
    CHECK(CommandLine.has_value());
    CHECK(CommandLine->find(L"--tls-provider schannel") != std::wstring::npos);
}

void WindowsPointerMotionScalingIsBoundedAndRetainsFractions() {
    using namespace desklink;
    std::optional<PointerMotionMessage> Motion;

    PointerMotionScaler Raw({100, 0});
    CHECK(Raw.Scale(12, -7, Motion));
    CHECK(Motion.has_value());
    CHECK(Motion->DeltaX == 12 && Motion->DeltaY == -7);

    PointerMotionScaler Doubled({200, 0});
    CHECK(Doubled.Scale(12, -7, Motion));
    CHECK(Motion->DeltaX == 24 && Motion->DeltaY == -14);

    PointerMotionScaler Normalized({100, 1'600});
    CHECK(Normalized.Scale(12, -8, Motion));
    CHECK(Motion->DeltaX == 6 && Motion->DeltaY == -4);

    PointerMotionScaler Fractional({25, 0});
    for (int Index = 0; Index < 3; ++Index) {
        CHECK(Fractional.Scale(1, 0, Motion));
        CHECK(!Motion.has_value());
    }
    CHECK(Fractional.Scale(1, 0, Motion));
    CHECK(Motion.has_value() && Motion->DeltaX == 1);

    PointerMotionScaler Invalid({24, 0});
    CHECK(!Invalid.Scale(1, 0, Motion));
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
        CHECK(Display.PixelWidth > 0);
        CHECK(Display.PixelHeight > 0);
        CHECK(Display.RefreshMilliHertz <=
              kMaximumDisplayRefreshMilliHertz);
        if (Display.PhysicalSize == PhysicalSizeSource::Unknown) {
            CHECK(Display.PhysicalWidthMillimeters == 0);
            CHECK(Display.PhysicalHeightMillimeters == 0);
        } else {
            CHECK(Display.PhysicalWidthMillimeters > 0);
            CHECK(Display.PhysicalHeightMillimeters > 0);
        }
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

    constexpr std::uint32_t LeftShift = 0xA0u;
    constexpr std::uint32_t F12 = 0x7Bu;
    Win32SuppressionGate ReturnGate;
    ReturnGate.SetReturnLocalHotkey(ProductHotkey::CtrlShiftF12);
    ReturnGate.SetRemoteRouting(true);
    CHECK(ReturnGate.HandleKeyboard(LeftControl, true, false) ==
          Win32HookDecision::Suppress);
    CHECK(ReturnGate.HandleKeyboard(LeftShift, true, false) ==
          Win32HookDecision::Suppress);
    CHECK(ReturnGate.HandleKeyboard(F12, true, false) ==
          Win32HookDecision::ReturnLocal);
    CHECK(!ReturnGate.RemoteRouting());
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

void WindowsWasapiFailureKindsAreExplicit() {
    using namespace desklink;
    CHECK(IsRecoverableWasapiFailure(
        Win32WasapiFailureKind::EndpointChanged));
    CHECK(IsRecoverableWasapiFailure(
        Win32WasapiFailureKind::EndpointUnavailable));
    CHECK(!IsRecoverableWasapiFailure(
        Win32WasapiFailureKind::ClientRejected));
}

void WindowsWasapiSmokeIfRequested() {
    if (std::getenv("DESKLINK_WASAPI_SMOKE") == nullptr) return;
    using namespace desklink;
    std::atomic_uint32_t Captured{};
    std::atomic_bool CaptureFailed{};
    Win32WasapiLoopbackCapture Capture(
        1, Win32WasapiCaptureHandlers{
            [&](AudioFrameMessage Frame) {
                CHECK(IsDeskLinkAudioFrame(Frame));
                ++Captured;
                return true;
            },
            [&](Win32WasapiFailureKind, std::string) {
                CaptureFailed.store(true);
            }});
    CHECK(Capture.Start());
    CHECK(Capture.Running());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Capture.Stop();
    CHECK(!Capture.Running());
    CHECK(!CaptureFailed.load());
    CHECK(Capture.Start());
    CHECK(Capture.Running());
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    Capture.Stop();
    CHECK(!Capture.Running());
    CHECK(!CaptureFailed.load());

    std::atomic_bool RenderFailed{};
    Win32WasapiRenderer Renderer(Win32WasapiRenderHandlers{
        [&](Win32WasapiFailureKind, std::string) {
            RenderFailed.store(true);
        }});
    CHECK(Renderer.Start());
    AudioFrameMessage Silence;
    Silence.stream_id = 1;
    Silence.pcm.assign(kDeskLinkAudioBytesPerBlock, 0);
    CHECK(Renderer.Submit(std::move(Silence)));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    Renderer.Stop();
    CHECK(!Renderer.Running());
    CHECK(!RenderFailed.load());
    CHECK(Renderer.QueuedFrames() == 0);
    CHECK(Renderer.Start());
    AudioFrameMessage RestartSilence;
    RestartSilence.stream_id = 1;
    RestartSilence.pcm.assign(kDeskLinkAudioBytesPerBlock, 0);
    CHECK(Renderer.Submit(std::move(RestartSilence)));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    Renderer.Stop();
    CHECK(!Renderer.Running());
    CHECK(!RenderFailed.load());
    CHECK(Renderer.QueuedFrames() == 0);
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
    const auto second_identity = MakeIdentity(43, "DPAPI peer two");
    CHECK(second.SavePeer(TrustedPeer{second_identity, CapabilitySet{}}));
    const auto listed = first.ListPeers();
    CHECK(listed.has_value());
    CHECK(listed->size() == 2);
    CHECK(first.GetPeer(second_identity.machine_id).has_value());
    CHECK(second.RemovePeer(identity.machine_id));
    CHECK(!first.GetPeer(identity.machine_id).has_value());
    CHECK(first.GetPeer(second_identity.machine_id).has_value());
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

void WindowsRoamingSettingsAreAtomicAndStrict() {
    using namespace desklink;
    const auto Directory = std::filesystem::temp_directory_path() /
        ("desklink-roaming-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto Path = Directory / "roaming.bin";
    std::error_code Ignored;
    std::filesystem::remove_all(Directory, Ignored);

    Win32RoamingSettingsStore First(Path);
    CHECK(First.Load());
    CHECK(First.IsLoaded());
    CHECK(First.Current() == RoamingConfiguration{});
    const auto Configuration = MakeRoamingConfiguration();
    CHECK(First.Save(Configuration));
    CHECK(First.Current() == Configuration);
    CHECK(std::filesystem::exists(Path));
    auto Temporary = Path;
    Temporary += L".tmp";
    CHECK(!std::filesystem::exists(Temporary));

    Win32RoamingSettingsStore Second(Path);
    CHECK(Second.Load());
    CHECK(Second.Current() == Configuration);
    auto Invalid = Configuration;
    Invalid.Links[0].EndpointA.Machine = {};
    CHECK(!Second.Save(Invalid));
    CHECK(Second.Current() == Configuration);

    {
        std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
        CHECK(Output.good());
        const std::array<char, 4> Malformed{'b', 'a', 'd', '!'};
        Output.write(Malformed.data(), Malformed.size());
        CHECK(Output.good());
    }
    Win32RoamingSettingsStore Malformed(Path);
    CHECK(!Malformed.Load());
    CHECK(!Malformed.IsLoaded());
    CHECK(!Malformed.Current().has_value());
    CHECK(!Second.Load());
    CHECK(!Second.IsLoaded());
    CHECK(!Second.Current().has_value());
    std::filesystem::remove_all(Directory, Ignored);
}

void WindowsApplicationSettingsAreAtomicAndStrict() {
    using namespace desklink;
    const auto Directory = std::filesystem::temp_directory_path() /
        ("desklink-application-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto Path = Directory / "application.bin";
    std::error_code Ignored;
    std::filesystem::remove_all(Directory, Ignored);

    Win32ProductPreferencesStore First(Path);
    CHECK(First.Load());
    CHECK(First.Current() == ProductPreferences{});
    ProductPreferences Settings;
    Settings.Role = DeskRole::Main;
    Settings.PreferredPeerMachine = MachineId{1};
    Settings.PreferredPeerEndpoint =
        ProductPeerEndpoint{"192.168.0.108", 43'821};
    Settings.CloseToTray = false;
    Settings.RunAtLogin = true;
    Settings.FirstRunComplete = true;
    Settings.AutoStartRuntime = true;
    Settings.AutoConnect = true;
    Settings.InputRoamingDesired = true;
    Settings.ClipboardDesired = true;
    Settings.AudioRoute = AudioRoutePreference::PeerToLocal;
    Settings.AudioGainPermyriad = 7'500;
    Settings.VoiceRoute = VoiceRoutePreference::Bidirectional;
    Settings.VoiceInputEndpointId = "test-microphone-endpoint";
    Settings.VoiceGainPermyriad = 6'500;
    Settings.VoiceEchoGuard = false;
    Settings.Gaming = GamingBehavior::FollowProfileRules;
    Settings.FocusPeerHotkey = ProductHotkey::CtrlAltF11;
    Settings.ReturnLocalHotkey = ProductHotkey::CtrlAltF12;
    Settings.ProfileRules = {
        ForegroundProfileRule{"game.exe", DeskMode::Game, true},
        ForegroundProfileRule{"editor.exe", DeskMode::LockPc1, false}};
    Settings.AdvancedModeEnabled = true;
    CHECK(First.Save(Settings));
    CHECK(First.Current() == Settings);
    auto Temporary = Path;
    Temporary += L".tmp";
    CHECK(!std::filesystem::exists(Temporary));

    Win32ProductPreferencesStore Second(Path);
    CHECK(Second.Load());
    CHECK(Second.Current() == Settings);

    const auto InvalidRolePath = Directory / "invalid-role.bin";
    CHECK(std::filesystem::copy_file(Path, InvalidRolePath));
    {
        std::fstream Output(
            InvalidRolePath, std::ios::binary | std::ios::in | std::ios::out);
        CHECK(Output.good());
        Output.seekp(6);
        Output.put(static_cast<char>(0xff));
    }
    Win32ProductPreferencesStore InvalidRole(InvalidRolePath);
    CHECK(!InvalidRole.Load());
    CHECK(!InvalidRole.Current().has_value());

    const auto ReservedPath = Directory / "reserved.bin";
    CHECK(std::filesystem::copy_file(Path, ReservedPath));
    {
        std::fstream Output(
            ReservedPath, std::ios::binary | std::ios::in | std::ios::out);
        CHECK(Output.good());
        Output.seekp(35);
        Output.put(static_cast<char>(0x80));
    }
    Win32ProductPreferencesStore Reserved(ReservedPath);
    CHECK(!Reserved.Load());
    CHECK(!Reserved.Current().has_value());

    {
        std::ofstream Output(Path, std::ios::binary | std::ios::app);
        CHECK(Output.good());
        Output.put('\0');
    }
    Win32ProductPreferencesStore Trailing(Path);
    CHECK(!Trailing.Load());
    CHECK(!Trailing.Current().has_value());

    const auto LegacyPath = Directory / "legacy.bin";
    const std::array<std::uint8_t, 12> Legacy{
        'D', 'L', 'A', 'S', 0, 1, 0, 7, 0, 0, 0, 0};
    {
        std::ofstream Output(LegacyPath, std::ios::binary);
        CHECK(Output.good());
        Output.write(
            reinterpret_cast<const char*>(Legacy.data()), Legacy.size());
    }
    Win32ProductPreferencesStore Migrated(LegacyPath);
    CHECK(Migrated.Load());
    const auto MigratedPreferences = Migrated.Current();
    CHECK(MigratedPreferences.has_value());
    CHECK(MigratedPreferences->Role == DeskRole::Unconfigured);
    CHECK(MigratedPreferences->CloseToTray);
    CHECK(MigratedPreferences->RunAtLogin);
    CHECK(MigratedPreferences->FirstRunComplete);
    CHECK(std::filesystem::file_size(LegacyPath) == 40);

    const auto Version2Path = Directory / "version-2.bin";
    std::array<std::uint8_t, 64> Version2{};
    Version2[0] = 'D';
    Version2[1] = 'L';
    Version2[2] = 'P';
    Version2[3] = 'P';
    Version2[5] = 2;
    Version2[6] = static_cast<std::uint8_t>(DeskRole::Main);
    Version2[7] = static_cast<std::uint8_t>(
        AudioRoutePreference::PeerToLocal);
    Version2[8] = static_cast<std::uint8_t>(GamingBehavior::KeepLocal);
    Version2[10] = 0;
    Version2[11] = 1;
    Version2[12] = 0x27;
    Version2[13] = 0x10;
    {
        std::ofstream Output(Version2Path, std::ios::binary);
        CHECK(Output.good());
        Output.write(
            reinterpret_cast<const char*>(Version2.data()), Version2.size());
    }
    Win32ProductPreferencesStore MigratedVersion2(Version2Path);
    CHECK(MigratedVersion2.Load());
    CHECK(MigratedVersion2.Current()->Role == DeskRole::Main);
    CHECK(MigratedVersion2.Current()->AudioGainPermyriad == 10'000);
    CHECK(std::filesystem::file_size(Version2Path) == 40);

    const auto Version3Path = Directory / "version-3.bin";
    std::array<std::uint8_t, 36> Version3{};
    Version3[0] = 'D';
    Version3[1] = 'L';
    Version3[2] = 'P';
    Version3[3] = 'P';
    Version3[5] = 3;
    Version3[6] = static_cast<std::uint8_t>(DeskRole::Main);
    Version3[7] = static_cast<std::uint8_t>(AudioRoutePreference::Off);
    Version3[8] = static_cast<std::uint8_t>(GamingBehavior::KeepLocal);
    Version3[11] = 1;
    Version3[12] = 0x27;
    Version3[13] = 0x10;
    {
        std::ofstream Output(Version3Path, std::ios::binary);
        CHECK(Output.good());
        Output.write(
            reinterpret_cast<const char*>(Version3.data()), Version3.size());
    }
    Win32ProductPreferencesStore MigratedVersion3(Version3Path);
    CHECK(MigratedVersion3.Load());
    CHECK(MigratedVersion3.Current()->Role == DeskRole::Main);
    CHECK(!MigratedVersion3.Current()->PreferredPeerEndpoint);
    CHECK(std::filesystem::file_size(Version3Path) == 40);
    {
        std::ifstream Input(Version3Path, std::ios::binary);
        std::array<std::uint8_t, 6> Header{};
        Input.read(reinterpret_cast<char*>(Header.data()), Header.size());
        CHECK(Input.good());
        CHECK(Header[4] == 0);
        CHECK(Header[5] == kProductPreferencesSchemaVersion);
    }

    auto Invalid = Settings;
    Invalid.AudioGainPermyriad = 10'001;
    CHECK(!Second.Save(Invalid));
    CHECK(Second.Current() == Settings);
    std::filesystem::remove_all(Directory, Ignored);
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
    WrongVersion[1].second = std::to_string(kProtocolVersion + 1u);
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

void ProductShellPresentationIsFailLocalAndBounded() {
    using namespace desklink;

    CHECK(kProductPermissionResolutionTimeout >=
          std::chrono::seconds(6));
    CHECK(kProductPermissionResolutionTimeout <=
          std::chrono::seconds(10));
    CHECK(kProductBrokerUnavailableGrace >= std::chrono::seconds(6));
    CHECK(kProductBrokerUnavailableGrace <= std::chrono::seconds(10));
    CHECK(kProductBrokerStateTimeout >
          std::chrono::milliseconds(500));
    CHECK(kProductBrokerStateTimeout <=
          std::chrono::seconds(1));

    ProductBrokerAvailability BrokerAvailability;
    const auto BrokerStart = std::chrono::steady_clock::time_point{};
    CHECK(!BrokerAvailability.ObserveUnavailable(BrokerStart));
    CHECK(!BrokerAvailability.ObserveUnavailable(
        BrokerStart + kProductBrokerUnavailableGrace -
        std::chrono::milliseconds{1}));
    CHECK(BrokerAvailability.ObserveUnavailable(
        BrokerStart + kProductBrokerUnavailableGrace));
    BrokerAvailability.ObserveAvailable(
        BrokerStart + kProductBrokerUnavailableGrace);
    CHECK(!BrokerAvailability.ObserveUnavailable(
        BrokerStart + kProductBrokerUnavailableGrace));

    constexpr std::array States{
        ProductShellState::Offline,
        ProductShellState::Connecting,
        ProductShellState::ConnectedLocal,
        ProductShellState::RemoteFocus,
        ProductShellState::ActionRequired,
        ProductShellState::Paused,
    };
    for (const auto State : States) {
        const auto Presentation = PresentProductShellState(State);
        CHECK(!Presentation.Badge.empty());
        CHECK(!Presentation.KeyboardAndMouseTitle.empty());
        CHECK(!Presentation.KeyboardAndMouseSummary.empty());
        CHECK(!Presentation.ConnectionDetail.empty());
        CHECK(Presentation.Badge.size() <= 32);
        CHECK(Presentation.KeyboardAndMouseSummary.size() <= 96);
    }

    const auto Remote =
        PresentProductShellState(ProductShellState::RemoteFocus);
    CHECK(Remote.ShowReturnLocal);
    CHECK(!Remote.ShowActionRequired);

    const auto Action =
        PresentProductShellState(ProductShellState::ActionRequired);
    CHECK(!Action.ShowReturnLocal);
    CHECK(Action.ShowActionRequired);
    CHECK(Action.ConnectionDetail.find(L"retry stopped") !=
          std::wstring_view::npos);

    for (const auto State : {ProductShellState::Offline,
                             ProductShellState::Connecting,
                             ProductShellState::ConnectedLocal,
                             ProductShellState::Paused}) {
        const auto Presentation = PresentProductShellState(State);
        CHECK(!Presentation.ShowReturnLocal);
        CHECK(!Presentation.ShowActionRequired);
    }
}

void ProductMonitorLayoutSaveIsOrderedAndFailsLocal() {
    using namespace desklink;

    std::vector<std::string> Events;
    const auto Actions = [&](bool Local, bool Return, bool Save, bool Apply) {
        return ProductMonitorSaveActions{
            [&, Local] {
                Events.emplace_back("local");
                return Local;
            },
            [&, Return] {
                Events.emplace_back("return");
                return Return;
            },
            [&, Save] {
                Events.emplace_back("save");
                return Save;
            },
            [&, Apply] {
                Events.emplace_back("apply");
                return Apply;
            }};
    };

    CHECK(ApplyProductMonitorLayout(
        false, Actions(true, true, true, true)) ==
        ProductMonitorSaveStatus::Applied);
    CHECK((Events == std::vector<std::string>{"local", "save", "apply"}));

    Events.clear();
    CHECK(ApplyProductMonitorLayout(
        true, Actions(true, false, true, true)) ==
        ProductMonitorSaveStatus::CleanupFailed);
    CHECK((Events == std::vector<std::string>{"return"}));

    Events.clear();
    CHECK(ApplyProductMonitorLayout(
        true, Actions(true, true, true, true)) ==
        ProductMonitorSaveStatus::Applied);
    CHECK((Events == std::vector<std::string>{
        "return", "local", "save", "apply"}));

    Events.clear();
    CHECK(ApplyProductMonitorLayout(
        false, Actions(true, true, false, true)) ==
        ProductMonitorSaveStatus::StoreFailed);
    CHECK((Events == std::vector<std::string>{"local", "save"}));

    Events.clear();
    CHECK(ApplyProductMonitorLayout(
        false, Actions(true, true, true, false)) ==
        ProductMonitorSaveStatus::PreferenceApplyFailed);
    CHECK((Events == std::vector<std::string>{"local", "save", "apply"}));

    Events.clear();
    CHECK(ApplyProductMonitorLayout(
        false, Actions(false, true, true, true)) ==
        ProductMonitorSaveStatus::CleanupFailed);
    CHECK((Events == std::vector<std::string>{"local"}));
}

void PeerSessionFailsBothSidesLocalWhenInputBecomesUnavailable() {
    using namespace desklink;
    constexpr std::uint64_t Nonce = 0x71'82'95u;
    const auto IdentityA = MakeIdentity(105, "Unavailable A");
    const auto IdentityB = MakeIdentity(106, "Unavailable B");
    auto Pair = make_in_memory_transport_pair(
        TransportPeerInfo{IdentityB, true, true},
        TransportPeerInfo{IdentityA, true, true});
    CapabilitySet Capabilities;
    Capabilities.grant(Capability::InputInject);
    InMemoryTrustStore TrustA;
    InMemoryTrustStore TrustB;
    SaveTrustedPeer(TrustA, IdentityB, Capabilities);
    SaveTrustedPeer(TrustB, IdentityA, Capabilities);
    ManualClock Clock;
    RecordingInjector InjectorA;
    RecordingInjector InjectorB;
    AgentCoordinator IncomingA(Clock, InjectorA);
    AgentCoordinator IncomingB(Clock, InjectorB);
    HostCoordinator OutgoingA(Nonce);
    HostCoordinator OutgoingB(Nonce);
    std::size_t ClosedA{};
    std::size_t ClosedB{};
    PeerSessionHandlers HandlersA;
    HandlersA.TransportClosed = [&](TransportCloseReason Reason) {
        CHECK(Reason == TransportCloseReason::Unavailable);
        ++ClosedA;
    };
    PeerSession SessionA(
        Pair.a, OutgoingA, IncomingA, TrustA, Nonce,
        std::move(HandlersA));
    PeerSession SessionB(
        Pair.b, OutgoingB, IncomingB, TrustB, Nonce,
        PeerSessionHandlers{
            {}, {}, {},
            [&](TransportCloseReason Reason) {
                CHECK(Reason == TransportCloseReason::Unavailable);
                ++ClosedB;
            }});
    CHECK(SessionA.Start());
    CHECK(SessionB.Start());
    CHECK(SessionA.BeginOutgoingFocus());
    CHECK(SessionA.OutgoingFocused());
    CHECK(SessionB.IncomingFocused());

    InjectorB.InjectSucceeds = false;
    CHECK(SessionA.SendPointerMotion(PointerMotionMessage{4, 2}));
    CHECK(!SessionB.IncomingFocused());
    SessionB.Tick();
    CHECK(!SessionA.OutgoingFocused());
    CHECK(SessionA.DirectionState() == PeerDirectionState::Local);
    CHECK(SessionB.DirectionState() == PeerDirectionState::Local);
    CHECK(ClosedA == 1);
    CHECK(ClosedB == 1);
}

void UnavailableInputFailsLocalBeforeAndAfterFocusAdmission() {
    using namespace desklink;
    ManualClock Clock;
    RecordingInjector Injector;
    AgentCoordinator Agent(Clock, Injector);
    CapabilitySet Capabilities;
    Capabilities.grant(Capability::InputInject);
    Agent.set_peer_capabilities(Capabilities);

    EnvelopeHeader Header;
    const auto Request = decode_packet(
        encode_packet(Header, FocusRequestMessage{750, 1}), false);
    CHECK(Request.packet.has_value());
    Injector.Ready = false;
    CHECK(Agent.handle(*Request.packet) ==
          AgentDecision::RejectedInputUnavailable);
    CHECK(!Agent.RemoteFocused());
    CHECK(Agent.InputUnavailable());

    Injector.Ready = true;
    CHECK(Agent.handle(*Request.packet) == AgentDecision::Accepted);
    CHECK(Agent.RemoteFocused());
    CHECK(!Agent.InputUnavailable());
    Header.epoch = Agent.focus_state().epoch();
    Header.sequence = 1;
    const auto Motion = decode_packet(
        encode_packet(Header, PointerMotionMessage{4, 2}), true);
    CHECK(Motion.packet.has_value());
    Injector.InjectSucceeds = false;
    CHECK(Agent.handle(*Motion.packet) ==
          AgentDecision::RejectedInputUnavailable);
    CHECK(!Agent.RemoteFocused());
    CHECK(Agent.InputUnavailable());
    CHECK(Injector.release_calls >= 2);
}

#ifdef DESKLINK_BUILD_VOICE
void VoiceProtocolCodecJitterAndBoundsAreStrict() {
    using namespace desklink;

    CHECK(kProtocolVersion == 5);
    CHECK((kKnownCapabilityBits &
        static_cast<std::uint64_t>(Capability::VoiceSend)) != 0);
    CHECK((kKnownCapabilityBits &
        static_cast<std::uint64_t>(Capability::VoiceReceive)) != 0);

    VoiceEncoder Encoder;
    VoiceDecoder Decoder;
    CHECK(Encoder.Ready());
    CHECK(Decoder.Ready());
    std::array<std::int16_t, kVoiceSamplesPerChannel> Samples{};
    for (std::size_t Index = 0; Index < Samples.size(); ++Index) {
        Samples[Index] = static_cast<std::int16_t>(
            8'000.0 * std::sin(2.0 * 3.141592653589793 *
                440.0 * static_cast<double>(Index) /
                static_cast<double>(kVoiceSampleRate)));
    }
    const auto Encoded = Encoder.Encode(Samples);
    CHECK(Encoded.has_value());
    CHECK(!Encoded->empty());
    CHECK(Encoded->size() <= kVoiceMaximumEncodedBytes);
    const auto Decoded = Decoder.Decode(*Encoded, false, 123'000);
    CHECK(Decoded.has_value());
    CHECK(Decoded->Samples.size() == kVoiceSamplesPerChannel);
    CHECK(!Decoder.Decode(ByteBuffer{0xff, 0xff, 0xff}, false, 0));

    VoiceFrameMessage Frame;
    Frame.StreamId = 7;
    Frame.CaptureTimestampUs = 123'000;
    Frame.Encoded = *Encoded;
    EnvelopeHeader Header;
    Header.session_nonce = 99;
    Header.sequence = 1;
    const auto Packet = encode_packet(Header, Frame);
    const auto RoundTrip = decode_packet(Packet, true);
    CHECK(RoundTrip.packet.has_value());
    CHECK(RoundTrip.packet->header.type == MessageType::VoiceFrame);
    const auto& RoundTripFrame = std::get<VoiceFrameMessage>(
        RoundTrip.packet->message);
    CHECK(RoundTripFrame.StreamId == 7);
    CHECK(RoundTripFrame.Encoded == Frame.Encoded);
    CHECK(!decode_packet(Packet, false).packet.has_value());

    for (const auto Invalid : {
             VoiceFrameMessage{0, kVoiceSampleRate,
                 kVoiceSamplesPerChannel, kVoiceChannels,
                 VoiceCodec::Opus, 0, *Encoded},
             VoiceFrameMessage{1, 44'100,
                 kVoiceSamplesPerChannel, kVoiceChannels,
                 VoiceCodec::Opus, 0, *Encoded},
             VoiceFrameMessage{1, kVoiceSampleRate,
                 480, kVoiceChannels, VoiceCodec::Opus, 0, *Encoded},
             VoiceFrameMessage{1, kVoiceSampleRate,
                 kVoiceSamplesPerChannel, 2,
                 VoiceCodec::Opus, 0, *Encoded},
             VoiceFrameMessage{1, kVoiceSampleRate,
                 kVoiceSamplesPerChannel, kVoiceChannels,
                 static_cast<VoiceCodec>(0xff), 0, *Encoded},
             VoiceFrameMessage{1, kVoiceSampleRate,
                 kVoiceSamplesPerChannel, kVoiceChannels,
                 VoiceCodec::Opus, 0, {}}}) {
        CHECK(!IsValidVoiceFrameMessage(Invalid));
    }
    Frame.Encoded.assign(kVoiceMaximumEncodedBytes + 1u, 0x11);
    CHECK(!IsValidVoiceFrameMessage(Frame));

    std::vector<VoicePcmFrame> Rendered;
    VoiceReceiver Receiver(
        [&](VoicePcmFrame Pcm) {
            Rendered.push_back(std::move(Pcm));
            return true;
        });
    Frame.Encoded = *Encoded;
    Frame.StreamId = 9;
    Frame.CaptureTimestampUs = 20'000;
    CHECK(Receiver.Push(1, Frame));
    Frame.CaptureTimestampUs = 60'000;
    CHECK(Receiver.Push(3, Frame));
    Frame.CaptureTimestampUs = 80'000;
    CHECK(Receiver.Push(4, Frame));
    CHECK(Receiver.Pump() == VoicePumpResult::Submitted);
    CHECK(Receiver.Pump() == VoicePumpResult::Submitted);
    CHECK(Rendered.size() == 2);
    CHECK(Rendered.back().Concealed);
    CHECK(Receiver.Stats().FecRecovered + Receiver.Stats().PlcGenerated == 1);
    CHECK(Receiver.Stats().CurrentJitterTarget == 3);
    CHECK(Receiver.Stats().PeakJitterTarget == 3);
    CHECK(!Receiver.Push(1, Frame));
    CHECK(Receiver.Stats().SequenceRejected >= 1);

    Receiver.Reset();
    Frame.StreamId = 10;
    for (std::uint64_t Sequence = 1;
         Sequence <= kVoiceMaximumQueuedPackets + 5u; ++Sequence) {
        Frame.CaptureTimestampUs = Sequence * 20'000;
        CHECK(Receiver.Push(Sequence, Frame));
    }
    CHECK(Receiver.Stats().DroppedForBound >= 1);
    Frame.StreamId = 11;
    CHECK(Receiver.Push(100, Frame));
    Frame.StreamId = 10;
    CHECK(!Receiver.Push(99, Frame));
    CHECK(Receiver.Stats().StreamRejected >= 1);

    VoiceReceiver Adaptive([](VoicePcmFrame) { return true; });
    Frame.StreamId = 12;
    for (const auto Sequence : {1u, 3u, 5u, 7u, 9u, 11u}) {
        Frame.CaptureTimestampUs = Sequence * 20'000u;
        CHECK(Adaptive.Push(Sequence, Frame));
    }
    CHECK(Adaptive.Stats().CurrentJitterTarget ==
          kVoiceMaximumJitterPackets);
    CHECK(Adaptive.Stats().PeakJitterTarget ==
          kVoiceMaximumJitterPackets);

    VoiceReceiver Muted([&](VoicePcmFrame Pcm) {
        Rendered.push_back(std::move(Pcm));
        return true;
    }, 1);
    Muted.SetMuted(true);
    Frame.StreamId = 13;
    Frame.CaptureTimestampUs = 20'000;
    CHECK(Muted.Push(1, Frame));
    CHECK(Muted.Pump() == VoicePumpResult::Submitted);
    CHECK(Rendered.back().Samples.back() == 0);
}

void PeerVoiceRequiresComplementaryAcknowledgedGrants() {
    using namespace desklink;
    constexpr std::uint64_t Nonce = 0x5151'7171u;
    const auto IdentityA = MakeIdentity(151, "Voice A");
    const auto IdentityB = MakeIdentity(152, "Voice B");
    auto Pair = make_in_memory_transport_pair(
        TransportPeerInfo{IdentityB, true, true},
        TransportPeerInfo{IdentityA, true, true});
    CapabilitySet Capabilities;
    Capabilities.grant(Capability::VoiceSend);
    Capabilities.grant(Capability::VoiceReceive);
    Capabilities.grant(Capability::AudioSend);
    Capabilities.grant(Capability::AudioReceive);
    InMemoryTrustStore TrustA;
    InMemoryTrustStore TrustB;
    SaveTrustedPeer(TrustA, IdentityB, Capabilities);
    SaveTrustedPeer(TrustB, IdentityA, Capabilities);
    ManualClock Clock;
    RecordingInjector InjectorA;
    RecordingInjector InjectorB;
    AgentCoordinator IncomingA(Clock, InjectorA);
    AgentCoordinator IncomingB(Clock, InjectorB);
    HostCoordinator OutgoingA(Nonce);
    HostCoordinator OutgoingB(Nonce);
    std::vector<VoicePcmFrame> RenderedA;
    std::vector<VoicePcmFrame> RenderedB;
    VoiceReceiver ReceiverA([&](VoicePcmFrame Frame) {
        RenderedA.push_back(std::move(Frame));
        return true;
    }, 1);
    VoiceReceiver ReceiverB([&](VoicePcmFrame Frame) {
        RenderedB.push_back(std::move(Frame));
        return true;
    }, 1);
    std::uint64_t AuthorizationChangesA{};
    PeerSessionHandlers HandlersA;
    HandlersA.VoiceAuthorizationChanged = [&] {
        ++AuthorizationChangesA;
    };
    PeerSession SessionA(
        Pair.a, OutgoingA, IncomingA, TrustA, Nonce,
        std::move(HandlersA), nullptr, {}, {}, {}, &ReceiverA);
    PeerSession SessionB(
        Pair.b, OutgoingB, IncomingB, TrustB, Nonce,
        {}, nullptr, {}, {}, {}, &ReceiverB);
    CHECK(SessionA.Start());
    CHECK(SessionB.Start());
    CHECK(SessionA.CanSendVoice());
    CHECK(SessionA.CanReceiveVoice());
    CHECK(SessionB.CanSendVoice());
    CHECK(SessionB.CanReceiveVoice());

    VoiceEncoder Encoder;
    std::array<std::int16_t, kVoiceSamplesPerChannel> Samples{};
    const auto Encoded = Encoder.Encode(Samples);
    CHECK(Encoded.has_value());
    VoiceFrameMessage Frame;
    Frame.StreamId = 1;
    Frame.CaptureTimestampUs = 20'000;
    Frame.Encoded = *Encoded;
    CHECK(SessionA.SendVoiceFrame(Frame));
    CHECK(SessionB.Stats().VoiceAccepted == 1);
    CHECK(ReceiverB.Pump() == VoicePumpResult::Submitted);
    CHECK(RenderedB.size() == 1);

    CapabilitySet Revoked = Capabilities;
    Revoked.revoke(Capability::VoiceReceive);
    SaveTrustedPeer(TrustA, IdentityB, Revoked);
    CHECK(SessionA.RefreshLocalCapabilities());
    CHECK(!SessionA.CanSendVoice());
    CHECK(SessionA.CanSendAudio());
    CHECK(!SessionA.SendVoiceFrame(Frame));
    CHECK(AuthorizationChangesA >= 1);
}
#endif

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
    std::optional<TransportCloseReason> ClosedReason;
    pair.b->set_close_handler([&](TransportCloseReason Reason) {
        ClosedReason = Reason;
    });
    pair.a->close();
    CHECK(ClosedReason == TransportCloseReason::Unavailable);

    auto late_pair = make_in_memory_transport_pair(a_sees_b, b_sees_a);
    late_pair.a->close();
    std::optional<TransportCloseReason> LateClosedReason;
    late_pair.b->set_close_handler([&](TransportCloseReason Reason) {
        LateClosedReason = Reason;
    });
    CHECK(LateClosedReason == TransportCloseReason::Unavailable);
}

} // namespace

int main() {
#ifdef DESKLINK_BUILD_VOICE
    VoiceProtocolCodecJitterAndBoundsAreStrict();
    PeerVoiceRequiresComplementaryAcknowledgedGrants();
#endif
    CallbackGateClosesAndDrainsAdmittedCallbacks();
    AudioFrameAssemblerProducesExactBoundedBlocks();
    AudioReceiverIsBoundedAndFailsClosed();
    AudioReceiverGainAndMuteAreBoundedAndRamped();
    AdaptiveJitterRaisesQuicklyAndLowersSlowly();
    ClockDriftControllerIsBoundedAndSlewLimited();
    ClockDriftResamplerIsExactAndBounded();
    JitterTargetIncreaseRebuffersWithinBounds();
    ForegroundProfilePolicyIsBoundedAndDeterministic();
    ProductPreferencesAndPlannerAreStrictAndFailLocal();
    HostInputLifecycleRecreatesCaptureOnlyAfterFreshFocus();
    HostInputLifecycleFailuresRemainLocal();
    DiscoveryPropertiesAreStrictAndRoundTrip();
    DiscoveryCacheExpiresAndFlagsConflicts();
    ProductShellPresentationIsFailLocalAndBounded();
    ProductMonitorLayoutSaveIsOrderedAndFailsLocal();
    protocol_round_trip();
    PointerMotionRoundTripAndValidation();
    ControlProtocolRoundTripAndValidation();
    RuntimeBrokerTrustAndPairingAuthorityAreFailClosed();
    UpdateCoordinatorIsOrderedAndFailsLocal();
    MouseWheelRoundTripAndValidation();
    DisplayTopologyMappingIsStableAndInvalidates();
    DisplayTopologyRejectsAmbiguousStableIds();
    DisplayMetadataIsBoundedAndDoesNotChangeRoutingGeneration();
    EdidPhysicalSizeParsingIsStrictAndBounded();
    RoamingGraphValidationAndCodecAreStrict();
    MonitorConfiguratorCanvasIsPresentationOnlyAndSuggestsExplicitLinks();
    RoamingEndpointsResolveAgainstCurrentStableTopologies();
    RoamingRuntimeRequiresAuthorizedStableContext();
    RoamingRuntimeCrossingPoliciesAndAdmissionAreFailClosed();
    RoamingRuntimeInvalidatesActiveRoutesAndEnforcesCooldown();
    RoamingRuntimeHandlesExtremeLocalPointerDeltas();
    RoamingRuntimeUsesPhysicalLandingHintsOnlyWhenTrustworthy();
    RoamingRuntimeRecordedTracesRespectCrossingPolicies();
    PeerDirectionArbiterRejectsCollisionsAndStaleTokens();
    PeerSessionSupportsReciprocalFocusAndIndependentGrants();
    PeerSessionImmediatelyReacquiresAfterLostRelease();
    PeerSessionFailsBothSidesLocalWhenInputBecomesUnavailable();
    PeerSessionRenegotiatesCapabilitiesWithoutDisconnecting();
    PeerSessionResolvesSimultaneousFocusToLocal();
    PeerSessionRequiresTheRemoteDirectionalGrant();
    PeerSessionFailsLocalOnAnInvalidCapabilityReplay();
    PeerSessionDoesNotTreatRemoteGrantsAsLocalDisclosureConsent();
    PeerSessionPreservesExplicitAudioAndTopologyExchange();
    ClipboardProtocolIsUtf8BoundedAndReliableOnly();
    ClipboardExchangeRequiresConsentNegotiationNonceAndRate();
    PeerSessionClipboardIsComplementaryAndInputIndependent();
    PeerSessionClipboardDefaultsOffAndRejectsOneSidedConsent();
    PeerSessionClipboardFailureAndReconnectStayFailClosed();
    DisplayTopologyProtocolIsBoundedAndStrict();
    DisplayTopologyAdmissionFailsClosedAndRecoversOnReconnect();
    RoamingRouteWaitsForAuthenticatedTopology();
    InputStateSnapshotRoundTripAndValidation();
    InputStateTransitionsReleaseBeforePress();
    rejects_wrong_lane_and_oversize();
    capability_and_lease_gate_input();
    DesiredModeControlIsCapabilityGatedAndFailsLocal();
    stale_epoch_rejected_after_refocus();
    FailedInputCleanupIsRetriedAndBlocksReadmission();
    UnavailableInputFailsLocalBeforeAndAfterFocusAdmission();
    host_agent_focus_transaction();
    jitter_buffer_reorders_and_conceals();
    out_of_order_pointer_rejected();
    stale_focus_ready_cannot_win_new_transaction();
    secure_session_end_to_end();
    TopologySessionExchangeIsCapabilityAndNonceBound();
    AudioSessionRequiresCapabilitiesNonceAndFormat();
    insecure_transport_refused();
    UnpairedTransportIsRefused();
    PinnedIdentityMismatchIsRefused();
    PairingRequiresMatchingUserVerification();
    PairingTranscriptDetectsPinTamperingAndExpiry();
    PairingWireIsBoundedAndFragmentSafe();
    AttemptRateLimiterIsBoundedAndExpires();
    CertificatePinsMatchOnlyTheStoredPeer();
#ifdef _WIN32
    WindowsDiscoveryCancellationIsBounded();
    WindowsForegroundMonitorPublishesBoundedSnapshot();
    WindowsCurrentUserControlPipeRoundTrip();
    WindowsClipboardListenerLifecycleIsContentSilent();
    WindowsAlphaLauncherCommandsAreBoundedAndProductionPinned();
    WindowsPointerMotionScalingIsBoundedAndRetainsFractions();
    WindowsDisplayTopologyEnumeratesWhenAvailable();
    WindowsSuppressionGateFailsLocal();
    WindowsCaptureSmokeIfRequested();
    WindowsWasapiFailureKindsAreExplicit();
    WindowsWasapiSmokeIfRequested();
    WindowsCryptoAndDpapiTrustStoreWork();
    WindowsRoamingSettingsAreAtomicAndStrict();
    WindowsApplicationSettingsAreAtomicAndStrict();
#endif
    in_memory_transport_preserves_security_metadata();
    std::cout << "All DeskLink foundation tests passed.\n";
    return 0;
}
