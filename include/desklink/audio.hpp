#pragma once

#include "desklink/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <array>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace desklink {

inline constexpr std::uint32_t kDeskLinkAudioSampleRate = 48'000;
inline constexpr std::uint16_t kDeskLinkAudioFramesPerBlock = 240;
inline constexpr std::uint8_t kDeskLinkAudioChannels = 2;
inline constexpr std::uint8_t kDeskLinkAudioBytesPerSample = 2;
inline constexpr std::size_t kDeskLinkAudioBytesPerFrame =
    static_cast<std::size_t>(kDeskLinkAudioChannels) *
    kDeskLinkAudioBytesPerSample;
inline constexpr std::size_t kDeskLinkAudioBytesPerBlock =
    static_cast<std::size_t>(kDeskLinkAudioFramesPerBlock) *
    kDeskLinkAudioBytesPerFrame;
inline constexpr std::uint64_t kDeskLinkAudioBlockDurationUs = 5'000;
inline constexpr std::size_t kDeskLinkAudioDefaultTargetFrames = 4;
inline constexpr std::size_t kDeskLinkAudioMaximumAdaptiveTargetFrames = 12;
inline constexpr std::int32_t kDeskLinkAudioMaximumClockDriftPpm = 1'000;
inline constexpr std::size_t kDeskLinkAudioDriftObservationSamples = 400;
inline constexpr std::int32_t kDeskLinkAudioDriftAdjustmentPpm = 50;
inline constexpr std::uint16_t kDeskLinkAudioMaximumGainPermyriad = 10'000;

[[nodiscard]] bool IsDeskLinkAudioFrame(
    const AudioFrameMessage& Frame) noexcept;

class AudioFrameAssembler final {
public:
    explicit AudioFrameAssembler(std::uint32_t StreamId) noexcept;

    [[nodiscard]] bool Push(
        ByteSpan Pcm16Stereo, std::uint64_t CaptureTimestampUs,
        std::vector<AudioFrameMessage>& Output);
    [[nodiscard]] bool PushSilence(
        std::size_t FrameCount, std::uint64_t CaptureTimestampUs,
        std::vector<AudioFrameMessage>& Output);
    void Reset() noexcept;

private:
    [[nodiscard]] bool Append(
        const std::uint8_t* Data, std::size_t FrameCount, bool Silent,
        std::uint64_t CaptureTimestampUs,
        std::vector<AudioFrameMessage>& Output);

    std::uint32_t StreamId_{};
    std::array<std::uint8_t, kDeskLinkAudioBytesPerBlock> Pending_{};
    std::size_t PendingFrames_{};
    std::uint64_t PendingTimestampUs_{};
};

struct AudioPlayout {
    AudioFrameMessage frame;
    bool concealed{};
};

class AudioJitterBuffer {
public:
    explicit AudioJitterBuffer(std::size_t target_frames = 4, std::size_t max_frames = 20);

    [[nodiscard]] bool push(std::uint64_t sequence, AudioFrameMessage frame);
    [[nodiscard]] std::optional<AudioPlayout> pop();
    void SetTargetFrames(std::size_t TargetFrames) noexcept;
    void Reset() noexcept;
    [[nodiscard]] std::size_t buffered() const noexcept { return frames_.size(); }
    [[nodiscard]] std::size_t TargetFrames() const noexcept { return target_frames_; }
    [[nodiscard]] std::uint64_t RebufferEvents() const noexcept { return RebufferEvents_; }
    [[nodiscard]] std::uint64_t dropped_late() const noexcept { return dropped_late_; }
    [[nodiscard]] std::uint64_t concealed_frames() const noexcept { return concealed_frames_; }

private:
    [[nodiscard]] AudioFrameMessage make_silence_like(const AudioFrameMessage& model) const;

    std::size_t target_frames_;
    std::size_t max_frames_;
    std::map<std::uint64_t, AudioFrameMessage> frames_;
    std::optional<std::uint64_t> next_sequence_;
    std::optional<AudioFrameMessage> last_model_;
    bool Rebuffering_{};
    std::uint64_t RebufferEvents_{};
    std::uint64_t dropped_late_{};
    std::uint64_t concealed_frames_{};
};

class AudioAdaptiveJitterController final {
public:
    explicit AudioAdaptiveJitterController(
        std::size_t InitialTargetFrames = kDeskLinkAudioDefaultTargetFrames,
        std::size_t MaximumTargetFrames =
            kDeskLinkAudioMaximumAdaptiveTargetFrames) noexcept;

    void Observe(std::uint64_t Sequence,
                 std::uint64_t CaptureTimestampUs,
                 std::uint64_t ArrivalTimestampUs) noexcept;
    void ObserveConcealment() noexcept;
    void Reset() noexcept;

    [[nodiscard]] std::size_t TargetFrames() const noexcept {
        return TargetFrames_;
    }
    [[nodiscard]] std::size_t PeakTargetFrames() const noexcept {
        return PeakTargetFrames_;
    }
    [[nodiscard]] std::uint64_t EstimatedJitterUs() const noexcept;
    [[nodiscard]] std::uint64_t TargetRaises() const noexcept {
        return TargetRaises_;
    }
    [[nodiscard]] std::uint64_t TargetLowers() const noexcept {
        return TargetLowers_;
    }

private:
    std::size_t InitialTargetFrames_{};
    std::size_t MinimumTargetFrames_{};
    std::size_t MaximumTargetFrames_{};
    std::size_t TargetFrames_{};
    std::size_t PeakTargetFrames_{};
    std::optional<std::uint64_t> LastSequence_;
    std::uint64_t LastCaptureTimestampUs_{};
    std::uint64_t LastArrivalTimestampUs_{};
    std::uint64_t JitterScaled_{};
    std::size_t StableSamples_{};
    std::uint64_t TargetRaises_{};
    std::uint64_t TargetLowers_{};
};

class AudioClockDriftController final {
public:
    explicit AudioClockDriftController(
        std::size_t ObservationSamples =
            kDeskLinkAudioDriftObservationSamples,
        std::int32_t MaximumPpm =
            kDeskLinkAudioMaximumClockDriftPpm,
        std::int32_t AdjustmentPpm =
            kDeskLinkAudioDriftAdjustmentPpm) noexcept;

    void Observe(std::size_t BufferedSourceFrames,
                 std::size_t TargetSourceFrames,
                 bool PlayoutStarted) noexcept;
    void Discontinuity() noexcept;
    void Reset() noexcept;

    [[nodiscard]] std::int32_t AppliedPpm() const noexcept {
        return AppliedPpm_;
    }
    [[nodiscard]] std::uint64_t Adjustments() const noexcept {
        return Adjustments_;
    }
    [[nodiscard]] std::uint64_t Discontinuities() const noexcept {
        return Discontinuities_;
    }

private:
    void ClearObservation() noexcept;

    std::size_t ObservationSamples_{};
    std::int32_t MaximumPpm_{};
    std::int32_t AdjustmentPpm_{};
    std::optional<std::size_t> LastTargetSourceFrames_;
    std::int64_t OccupancyErrorSum_{};
    std::size_t Samples_{};
    std::int32_t AppliedPpm_{};
    std::uint64_t Adjustments_{};
    std::uint64_t Discontinuities_{};
};

class AudioClockDriftResampler final {
public:
    [[nodiscard]] bool Push(AudioFrameMessage Frame);
    [[nodiscard]] std::optional<AudioFrameMessage> Pop(
        std::int32_t AppliedPpm);
    void Reset() noexcept;

    [[nodiscard]] std::size_t BufferedSourceFrames() const noexcept;

private:
    void Compact();
    [[nodiscard]] std::int16_t ReadSample(
        std::size_t Frame, std::size_t Channel) const noexcept;
    static void WriteSample(ByteBuffer& Output, std::size_t Frame,
                            std::size_t Channel,
                            std::int16_t Sample) noexcept;

    ByteBuffer SourcePcm_;
    std::size_t SourceOffsetFrames_{};
    std::uint64_t PhaseQ32_{};
    std::optional<AudioFrameMessage> OutputModel_;
    std::uint64_t NextOutputTimestampUs_{};
};

struct AudioReceiverStats {
    std::uint64_t Accepted{};
    std::uint64_t FormatRejected{};
    std::uint64_t StreamRejected{};
    std::uint64_t SequenceRejected{};
    std::uint64_t Submitted{};
    std::uint64_t Concealed{};
    std::uint64_t RenderRejected{};
    std::uint64_t CurrentTargetFrames{};
    std::uint64_t PeakTargetFrames{};
    std::uint64_t EstimatedJitterUs{};
    std::uint64_t TargetRaises{};
    std::uint64_t TargetLowers{};
    std::uint64_t RebufferEvents{};
    std::int32_t AppliedClockDriftPpm{};
    std::uint64_t ClockDriftAdjustments{};
    std::uint64_t ClockDriftDiscontinuities{};
    std::uint64_t DriftBufferedSourceFrames{};
};

enum class AudioPumpResult {
    Buffering,
    Submitted,
    RenderRejected,
};

class AudioReceiver final {
public:
    using RenderHandler = std::function<bool(AudioFrameMessage)>;

    explicit AudioReceiver(RenderHandler Renderer,
                           std::size_t TargetFrames =
                               kDeskLinkAudioDefaultTargetFrames,
                           std::size_t MaximumFrames = 20);

    [[nodiscard]] bool Push(std::uint64_t Sequence,
                            AudioFrameMessage Frame) noexcept;
    [[nodiscard]] bool PushAt(std::uint64_t Sequence,
                              AudioFrameMessage Frame,
                              std::uint64_t ArrivalTimestampUs) noexcept;
    [[nodiscard]] AudioPumpResult Pump() noexcept;
    [[nodiscard]] bool SetGainPermyriad(std::uint16_t Gain) noexcept;
    [[nodiscard]] bool ToggleMuted() noexcept;
    void SetMuted(bool Muted) noexcept;
    [[nodiscard]] std::uint16_t GainPermyriad() const noexcept;
    [[nodiscard]] bool Muted() const noexcept;
    void Reset() noexcept;
    [[nodiscard]] bool Failed() const noexcept;
    [[nodiscard]] AudioReceiverStats Stats() const noexcept;

private:
    void ApplyGain(AudioFrameMessage& Frame) noexcept;

    RenderHandler Renderer_;
    AudioJitterBuffer Buffer_;
    AudioAdaptiveJitterController JitterController_;
    AudioClockDriftController DriftController_;
    AudioClockDriftResampler DriftResampler_;
    mutable std::mutex Mutex_;
    std::optional<std::uint32_t> StreamId_;
    std::optional<std::uint64_t> LastCaptureTimestampUs_;
    AudioReceiverStats Stats_;
    std::uint16_t GainPermyriad_{kDeskLinkAudioMaximumGainPermyriad};
    std::uint16_t AppliedGainPermyriad_{kDeskLinkAudioMaximumGainPermyriad};
    bool Muted_{};
    bool PlayoutStarted_{};
    bool Failed_{};
};

} // namespace desklink
