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
    void Reset() noexcept;
    [[nodiscard]] bool Failed() const noexcept;
    [[nodiscard]] AudioReceiverStats Stats() const noexcept;

private:
    RenderHandler Renderer_;
    AudioJitterBuffer Buffer_;
    AudioAdaptiveJitterController JitterController_;
    mutable std::mutex Mutex_;
    std::optional<std::uint32_t> StreamId_;
    AudioReceiverStats Stats_;
    bool Failed_{};
};

} // namespace desklink
