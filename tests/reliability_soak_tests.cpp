#include "desklink/audio.hpp"
#include "desklink/input.hpp"
#include "desklink/protocol.hpp"
#include "desklink/roaming_runtime.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

constexpr std::uint64_t kDefaultSeed = 0xD35C'1A2B'4E5F'6071ull;
constexpr std::uint64_t kDefaultIterations = 2'000;
constexpr std::uint64_t kMaximumIterations = 1'000'000;

class ManualClock final : public desklink::IClock {
public:
    time_point now() const noexcept override { return Current_; }
    void Advance(std::chrono::milliseconds Delta) noexcept { Current_ += Delta; }

private:
    time_point Current_{};
};

enum class FaultScenario : std::uint8_t {
    GracefulReturn,
    CableRemoval,
    NonceRotation,
    PeerTopologyChange,
    LocalMonitorHotplug,
    CapabilityRevocation,
    FocusTimeout,
    SleepWake,
    ProcessTermination,
    RdpDetach,
    Count,
};

struct HarnessOptions {
    std::uint64_t Iterations{kDefaultIterations};
    std::uint64_t Seed{kDefaultSeed};
};

[[nodiscard]] desklink::MachineId MakeMachine(std::uint8_t Marker) noexcept {
    desklink::MachineId Result{};
    Result.front() = Marker;
    Result.back() = static_cast<std::uint8_t>(Marker ^ 0xA5u);
    return Result;
}

[[nodiscard]] desklink::DisplayTopologySnapshot MakeTopology(
    std::string StableIdentity,
    desklink::DisplayRect Bounds) {
    desklink::DisplayTopologyMap Map;
    if (Map.Update({desklink::DiscoveredDisplay{
            std::move(StableIdentity), "Soak display", Bounds, true}}) !=
        desklink::DisplayTopologyUpdate::Changed) {
        return {};
    }
    return Map.Current();
}

[[nodiscard]] desklink::RoamingRuntimeContext MakeContext(
    std::uint64_t SessionNonce,
    bool Reverse) {
    using namespace desklink;
    RoamingConfiguration Configuration;
    Configuration.CrossingDefaults = {
        CrossingPolicy::Push, 8, 120, 500};
    Configuration.Links.push_back(RoamingLink{
        {MakeMachine(1), "soak-local", DisplayEdgeSide::Right, 0, 10'000},
        {MakeMachine(2), "soak-peer", DisplayEdgeSide::Left, 0, 10'000},
        RoamingDirectionMode::Bidirectional,
        {CrossingPolicy::Push, 8, 120, 500},
        {CrossingPolicy::Push, 8, 120, 500},
        12,
        24,
        24,
        true,
    });
    RoamingRuntimeContext Context{
        MakeMachine(1),
        MakeMachine(2),
        std::move(Configuration),
        MakeTopology("soak-local", {0, 0, 1'920, 1'080}),
        MakeTopology("soak-peer", {0, 0, 2'560, 1'440}),
        PeerConnectionStatus::Connected,
        DisplayTopologyExchangeStatus::Ready,
        SessionNonce,
        true,
        true,
        true,
    };
    if (Reverse) {
        std::swap(Context.LocalMachine, Context.PeerMachine);
        std::swap(Context.LocalTopology, Context.PeerTopology);
    }
    return Context;
}

[[nodiscard]] std::uint64_t NextRandom(std::uint64_t& State) noexcept {
    State ^= State << 13u;
    State ^= State >> 7u;
    State ^= State << 17u;
    return State;
}

[[nodiscard]] bool Fail(std::uint64_t Iteration,
                        FaultScenario Scenario,
                        std::string_view Detail) {
    std::cerr << "[Validation:Soak] failed iteration=" << Iteration
              << " scenario=" << static_cast<unsigned>(Scenario)
              << " detail=" << Detail << '\n';
    return false;
}

[[nodiscard]] bool IsReleased(
    const desklink::InputStateTransition& Transition) noexcept {
    return std::visit([](const auto& Message) { return !Message.down; },
                      Transition);
}

[[nodiscard]] bool ExerciseHeldInputCleanup(
    std::uint64_t Iteration, FaultScenario Scenario) {
    using namespace desklink;
    InputStateSnapshotMessage Held;
    if (!SetInputSnapshotKey(Held, 0x1D, false, true) ||
        !SetInputSnapshotKey(Held, 0x38, false, true) ||
        !SetInputSnapshotKey(Held, 0x1D, true, true) ||
        !SetInputSnapshotButton(Held, MouseButtonId::Left, true) ||
        !SetInputSnapshotButton(Held, MouseButtonId::Right, true)) {
        return Fail(Iteration, Scenario, "could not construct held-input chord");
    }
    const auto Cleanup = BuildInputStateTransitions(
        Held, InputStateSnapshotMessage{});
    if (Cleanup.size() != 5 ||
        !std::all_of(Cleanup.begin(), Cleanup.end(), IsReleased)) {
        return Fail(Iteration, Scenario,
                    "held key/button cleanup was incomplete or pressed state");
    }
    return true;
}

[[nodiscard]] bool ExerciseWheelPacket(
    std::uint64_t Iteration,
    FaultScenario Scenario,
    std::uint64_t SessionNonce,
    std::uint64_t Random) {
    using namespace desklink;
    const auto Axis = (Random & 1u) == 0
        ? MouseWheelAxis::Vertical : MouseWheelAxis::Horizontal;
    const auto Magnitude = static_cast<std::int16_t>(
        120 * (1 + static_cast<int>((Random >> 1u) % 10u)));
    const auto Delta = (Random & 0x20u) == 0
        ? Magnitude : static_cast<std::int16_t>(-Magnitude);
    const MouseWheelMessage Wheel{Axis, Delta};
    const auto Encoded = encode_packet(
        EnvelopeHeader{
            .session_nonce = SessionNonce,
            .epoch = Iteration + 1,
            .sequence = Iteration + 1},
        Message{Wheel});
    const auto Decoded = decode_packet(Encoded, false);
    if (!Decoded.packet ||
        !std::holds_alternative<MouseWheelMessage>(Decoded.packet->message)) {
        return Fail(Iteration, Scenario, "wheel packet did not round trip");
    }
    const auto& RoundTrip = std::get<MouseWheelMessage>(
        Decoded.packet->message);
    if (RoundTrip.Axis != Axis || RoundTrip.Delta != Delta ||
        Decoded.packet->header.session_nonce != SessionNonce) {
        return Fail(Iteration, Scenario, "wheel packet binding changed");
    }
    return true;
}

[[nodiscard]] bool ExerciseAudioPacket(
    std::uint64_t Iteration,
    FaultScenario Scenario,
    std::uint64_t SessionNonce,
    std::uint64_t Random,
    desklink::AudioReceiver& Receiver) {
    using namespace desklink;
    AudioFrameMessage Frame;
    Frame.stream_id = 7;
    Frame.capture_timestamp_us = Iteration * kDeskLinkAudioBlockDurationUs;
    Frame.pcm.resize(kDeskLinkAudioBytesPerBlock);
    for (std::size_t Index = 0; Index < Frame.pcm.size(); ++Index) {
        Frame.pcm[Index] = static_cast<std::uint8_t>(
            Random + Index + Iteration);
    }
    const auto Encoded = encode_packet(
        EnvelopeHeader{
            .session_nonce = SessionNonce,
            .epoch = Iteration + 1,
            .sequence = Iteration + 1},
        Message{Frame});
    const auto Decoded = decode_packet(Encoded, true);
    if (!Decoded.packet ||
        !std::holds_alternative<AudioFrameMessage>(Decoded.packet->message)) {
        return Fail(Iteration, Scenario, "audio packet did not round trip");
    }
    const auto& RoundTrip = std::get<AudioFrameMessage>(
        Decoded.packet->message);
    if (!IsDeskLinkAudioFrame(RoundTrip) ||
        Decoded.packet->header.session_nonce != SessionNonce ||
        !Receiver.Push(Iteration + 1, RoundTrip)) {
        return Fail(Iteration, Scenario, "audio load was rejected");
    }
    (void)Receiver.Pump();
    if (Receiver.Failed()) {
        return Fail(Iteration, Scenario, "audio receiver failed");
    }
    return true;
}

[[nodiscard]] std::optional<desklink::RoamingFocusRequest> CrossEdge(
    desklink::RoamingRuntime& Runtime,
    bool HighPollRate,
    bool Reverse,
    std::uint64_t Iteration,
    FaultScenario Scenario) {
    using namespace desklink;
    const auto ScreenX = Reverse ? 0 : 1'919;
    const auto ScreenY = Reverse ? 720 : 540;
    const auto OutwardDelta = Reverse ? -512 : 512;
    const auto ThresholdDelta = Reverse ? -1 : 1;
    if (!HighPollRate) {
        return Runtime.Observe(
            {ScreenX, ScreenY, OutwardDelta, 0});
    }
    const auto Request = Runtime.Observe(
        {ScreenX, ScreenY, ThresholdDelta, 0});
    if (!Request) {
        (void)Fail(Iteration, Scenario,
                   "high-poll-rate trace did not cross on first contact");
    }
    return Request;
}

[[nodiscard]] bool ExerciseRoamingFault(
    std::uint64_t Iteration,
    FaultScenario Scenario,
    std::uint64_t Random,
    ManualClock& Clock) {
    using namespace desklink;
    const auto SessionNonce = Iteration + 1;
    const auto Reverse = (Random & 2u) != 0;
    auto Context = MakeContext(SessionNonce, Reverse);
    RoamingRuntime Runtime(Clock);
    const auto Update = Runtime.UpdateContext(Context);
    if (!Update.Valid || Update.ReadyRouteCount != 1) {
        return Fail(Iteration, Scenario, "initial route was not ready");
    }
    const auto Request = CrossEdge(
        Runtime, (Random & 1u) != 0, Reverse, Iteration, Scenario);
    if (!Request || Runtime.State() != RoamingRuntimeState::FocusPending) {
        return Fail(Iteration, Scenario, "edge crossing was not requested");
    }

    const auto AdmitRemote = [&]() {
        return Runtime.AdmitFocusReady(*Request) &&
               Runtime.AdmitRemoteInput(*Request) &&
               Runtime.State() == RoamingRuntimeState::Remote;
    };
    switch (Scenario) {
        case FaultScenario::GracefulReturn:
            if (!AdmitRemote()) {
                return Fail(Iteration, Scenario, "normal focus was not admitted");
            }
            Runtime.BeginReturn();
            Runtime.ReturnLocal();
            break;
        case FaultScenario::CableRemoval:
            if (!AdmitRemote()) {
                return Fail(Iteration, Scenario, "pre-cable focus was not admitted");
            }
            Context.PeerStatus = PeerConnectionStatus::Offline;
            Context.PeerValidated = false;
            if (!Runtime.UpdateContext(Context).MustFailLocal) {
                return Fail(Iteration, Scenario, "cable removal stayed remote");
            }
            break;
        case FaultScenario::NonceRotation:
            ++Context.SessionNonce;
            if (!Runtime.UpdateContext(Context).MustFailLocal ||
                Runtime.AdmitFocusReady(*Request)) {
                return Fail(Iteration, Scenario,
                            "prior-session focus survived nonce rotation");
            }
            break;
        case FaultScenario::PeerTopologyChange:
            if (!AdmitRemote()) {
                return Fail(Iteration, Scenario, "pre-topology focus was not admitted");
            }
            ++Context.PeerTopology->Generation;
            if (!Runtime.UpdateContext(Context).MustFailLocal) {
                return Fail(Iteration, Scenario,
                            "peer topology change stayed remote");
            }
            break;
        case FaultScenario::LocalMonitorHotplug:
            if (!AdmitRemote()) {
                return Fail(Iteration, Scenario, "pre-hotplug focus was not admitted");
            }
            ++Context.LocalTopology->Generation;
            if (!Runtime.UpdateContext(Context).MustFailLocal) {
                return Fail(Iteration, Scenario,
                            "local monitor hotplug stayed remote");
            }
            break;
        case FaultScenario::CapabilityRevocation:
            Context.InputCapabilityGranted = false;
            if (!Runtime.UpdateContext(Context).MustFailLocal) {
                return Fail(Iteration, Scenario,
                            "capability revocation stayed pending");
            }
            break;
        case FaultScenario::FocusTimeout:
            Clock.Advance(kRoamingFocusTimeout);
            if (!Runtime.ExpireFocusPending()) {
                return Fail(Iteration, Scenario, "focus timeout did not fail local");
            }
            break;
        case FaultScenario::SleepWake:
            if (!AdmitRemote()) {
                return Fail(Iteration, Scenario, "pre-sleep focus was not admitted");
            }
            Context.PeerTopology.reset();
            Context.TopologyStatus = DisplayTopologyExchangeStatus::Offline;
            if (!Runtime.UpdateContext(Context).MustFailLocal) {
                return Fail(Iteration, Scenario, "sleep transition stayed remote");
            }
            break;
        case FaultScenario::ProcessTermination:
            if (!AdmitRemote()) {
                return Fail(Iteration, Scenario, "pre-kill focus was not admitted");
            }
            Runtime.FailLocal();
            break;
        case FaultScenario::RdpDetach:
            if (!AdmitRemote()) {
                return Fail(Iteration, Scenario, "pre-RDP focus was not admitted");
            }
            Context.DirectionSupported = false;
            if (!Runtime.UpdateContext(Context).MustFailLocal) {
                return Fail(Iteration, Scenario, "RDP detach stayed remote");
            }
            break;
        case FaultScenario::Count:
            return Fail(Iteration, Scenario, "invalid scenario");
    }

    if (Runtime.State() != RoamingRuntimeState::LocalCooldown &&
        Runtime.State() != RoamingRuntimeState::Local) {
        return Fail(Iteration, Scenario, "fault did not reach a Local state");
    }
    if (Runtime.ActiveRequest()) {
        return Fail(Iteration, Scenario, "fault retained an active request");
    }
    if (Runtime.State() == RoamingRuntimeState::LocalCooldown) {
        const auto ScreenX = Reverse ? 0 : 1'919;
        const auto ScreenY = Reverse ? 720 : 540;
        const auto OutwardDelta = Reverse ? -512 : 512;
        const auto InwardDelta = Reverse ? 24 : -24;
        if (Runtime.Observe(
                {ScreenX, ScreenY, OutwardDelta, 0}) ||
            Runtime.State() != RoamingRuntimeState::LocalCooldown) {
            return Fail(Iteration, Scenario, "cooldown admitted outward motion");
        }
        if (Runtime.Observe(
                {ScreenX, ScreenY, InwardDelta, 0}) ||
            Runtime.State() != RoamingRuntimeState::Local) {
            return Fail(Iteration, Scenario, "cooldown did not release inward");
        }
    }
    return true;
}

[[nodiscard]] std::optional<std::uint64_t> ParseUnsigned(
    std::string_view Text) noexcept {
    if (Text.empty()) return std::nullopt;
    std::uint64_t Value{};
    const auto Result = std::from_chars(
        Text.data(), Text.data() + Text.size(), Value);
    if (Result.ec != std::errc{} ||
        Result.ptr != Text.data() + Text.size()) {
        return std::nullopt;
    }
    return Value;
}

[[nodiscard]] std::optional<HarnessOptions> ParseOptions(
    int ArgumentCount, char** Arguments) noexcept {
    HarnessOptions Options;
    for (int Index = 1; Index < ArgumentCount; ++Index) {
        const std::string_view Argument(Arguments[Index]);
        if ((Argument == "--iterations" || Argument == "--seed") &&
            Index + 1 < ArgumentCount) {
            const auto Value = ParseUnsigned(Arguments[++Index]);
            if (!Value) return std::nullopt;
            if (Argument == "--iterations") {
                if (*Value == 0 || *Value > kMaximumIterations) {
                    return std::nullopt;
                }
                Options.Iterations = *Value;
            } else {
                if (*Value == 0) return std::nullopt;
                Options.Seed = *Value;
            }
            continue;
        }
        return std::nullopt;
    }
    return Options;
}

} // namespace

int main(int ArgumentCount, char** Arguments) {
    using namespace desklink;
    const auto Options = ParseOptions(ArgumentCount, Arguments);
    if (!Options) {
        std::cerr << "[Validation:Soak] usage: desklink_reliability_soak_tests "
                     "[--iterations 1..1000000] [--seed nonzero-decimal]\n";
        return 2;
    }

    std::uint64_t RandomState = Options->Seed;
    std::uint64_t RenderedAudioFrames{};
    AudioReceiver Receiver(
        [&](AudioFrameMessage Frame) {
            if (!IsDeskLinkAudioFrame(Frame)) return false;
            ++RenderedAudioFrames;
            return true;
        },
        2, 8);
    ManualClock Clock;
    for (std::uint64_t Iteration = 0;
         Iteration < Options->Iterations; ++Iteration) {
        const auto Random = NextRandom(RandomState);
        const auto Scenario = static_cast<FaultScenario>(
            Iteration % static_cast<std::uint64_t>(FaultScenario::Count));
        const auto SessionNonce = Iteration + 1;
        if (!ExerciseRoamingFault(
                Iteration, Scenario, Random, Clock) ||
            !ExerciseHeldInputCleanup(Iteration, Scenario) ||
            !ExerciseWheelPacket(
                Iteration, Scenario, SessionNonce, Random) ||
            !ExerciseAudioPacket(
                Iteration, Scenario, SessionNonce, Random, Receiver)) {
            return 1;
        }
        if ((Iteration + 1) % 127u == 0) Receiver.Reset();
        Clock.Advance(std::chrono::milliseconds(1));
    }

    std::cout << "[Validation:Soak] passed iterations="
              << Options->Iterations << " seed=" << Options->Seed
              << " scenarios="
              << static_cast<unsigned>(FaultScenario::Count)
              << " rendered_audio_frames=" << RenderedAudioFrames << '\n';
    return 0;
}
