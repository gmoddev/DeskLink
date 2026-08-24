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
    void Reset() noexcept;
    [[nodiscard]] std::size_t buffered() const noexcept { return frames_.size(); }
    [[nodiscard]] std::uint64_t dropped_late() const noexcept { return dropped_late_; }
    [[nodiscard]] std::uint64_t concealed_frames() const noexcept { return concealed_frames_; }

private:
    [[nodiscard]] AudioFrameMessage make_silence_like(const AudioFrameMessage& model) const;

    std::size_t target_frames_;
    std::size_t max_frames_;
    std::map<std::uint64_t, AudioFrameMessage> frames_;
    std::optional<std::uint64_t> next_sequence_;
    std::optional<AudioFrameMessage> last_model_;
    std::uint64_t dropped_late_{};
    std::uint64_t concealed_frames_{};
};

struct AudioReceiverStats {
    std::uint64_t Accepted{};
    std::uint64_t FormatRejected{};
    std::uint64_t StreamRejected{};
    std::uint64_t SequenceRejected{};
    std::uint64_t Submitted{};
    std::uint64_t Concealed{};
    std::uint64_t RenderRejected{};
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
                           std::size_t TargetFrames = 4,
                           std::size_t MaximumFrames = 20);

    [[nodiscard]] bool Push(std::uint64_t Sequence,
                            AudioFrameMessage Frame) noexcept;
    [[nodiscard]] AudioPumpResult Pump() noexcept;
    void Reset() noexcept;
    [[nodiscard]] bool Failed() const noexcept;
    [[nodiscard]] AudioReceiverStats Stats() const noexcept;

private:
    RenderHandler Renderer_;
    AudioJitterBuffer Buffer_;
    mutable std::mutex Mutex_;
    std::optional<std::uint32_t> StreamId_;
    AudioReceiverStats Stats_;
    bool Failed_{};
};

} // namespace desklink
