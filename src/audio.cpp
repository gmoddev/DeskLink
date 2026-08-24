#include "desklink/audio.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace desklink {
namespace {

constexpr std::size_t kMaximumSourceFramesPerPush = 8'192;

std::optional<std::uint64_t> AddFrameDuration(
    std::uint64_t TimestampUs, std::size_t Frames) noexcept {
    constexpr auto Scale = std::uint64_t{1'000'000};
    const auto WholeSeconds = Frames / kDeskLinkAudioSampleRate;
    const auto RemainingFrames = Frames % kDeskLinkAudioSampleRate;
    if (WholeSeconds >
        (std::numeric_limits<std::uint64_t>::max() - TimestampUs) / Scale) {
        return std::nullopt;
    }
    auto Result = TimestampUs + WholeSeconds * Scale;
    const auto Fraction =
        (static_cast<std::uint64_t>(RemainingFrames) * Scale) /
        kDeskLinkAudioSampleRate;
    if (Fraction > std::numeric_limits<std::uint64_t>::max() - Result) {
        return std::nullopt;
    }
    Result += Fraction;
    return Result;
}

} // namespace

bool IsDeskLinkAudioFrame(const AudioFrameMessage& Frame) noexcept {
    return Frame.sample_rate == kDeskLinkAudioSampleRate &&
           Frame.frames_per_channel == kDeskLinkAudioFramesPerBlock &&
           Frame.channels == kDeskLinkAudioChannels &&
           Frame.bytes_per_sample == kDeskLinkAudioBytesPerSample &&
           Frame.pcm.size() == kDeskLinkAudioBytesPerBlock;
}

AudioFrameAssembler::AudioFrameAssembler(std::uint32_t StreamId) noexcept
    : StreamId_(StreamId) {}

bool AudioFrameAssembler::Push(
    ByteSpan Pcm16Stereo, std::uint64_t CaptureTimestampUs,
    std::vector<AudioFrameMessage>& Output) {
    if (Pcm16Stereo.size() % kDeskLinkAudioBytesPerFrame != 0) return false;
    const auto FrameCount = Pcm16Stereo.size() / kDeskLinkAudioBytesPerFrame;
    return Append(Pcm16Stereo.data(), FrameCount, false,
                  CaptureTimestampUs, Output);
}

bool AudioFrameAssembler::PushSilence(
    std::size_t FrameCount, std::uint64_t CaptureTimestampUs,
    std::vector<AudioFrameMessage>& Output) {
    return Append(nullptr, FrameCount, true, CaptureTimestampUs, Output);
}

void AudioFrameAssembler::Reset() noexcept {
    std::fill(Pending_.begin(), Pending_.end(), std::uint8_t{0});
    PendingFrames_ = 0;
    PendingTimestampUs_ = 0;
}

bool AudioFrameAssembler::Append(
    const std::uint8_t* Data, std::size_t FrameCount, bool Silent,
    std::uint64_t CaptureTimestampUs,
    std::vector<AudioFrameMessage>& Output) {
    if (FrameCount > kMaximumSourceFramesPerPush ||
        (!Silent && FrameCount != 0 && Data == nullptr)) {
        return false;
    }
    std::size_t ConsumedFrames{};
    while (ConsumedFrames < FrameCount) {
        if (PendingFrames_ == 0) {
            const auto Timestamp = AddFrameDuration(
                CaptureTimestampUs, ConsumedFrames);
            if (!Timestamp) return false;
            PendingTimestampUs_ = *Timestamp;
        }
        const auto AvailableFrames =
            static_cast<std::size_t>(kDeskLinkAudioFramesPerBlock) -
            PendingFrames_;
        const auto CopyFrames = std::min(
            AvailableFrames, FrameCount - ConsumedFrames);
        const auto DestinationOffset =
            PendingFrames_ * kDeskLinkAudioBytesPerFrame;
        const auto CopyBytes = CopyFrames * kDeskLinkAudioBytesPerFrame;
        if (Silent) {
            std::fill_n(Pending_.begin() +
                            static_cast<std::ptrdiff_t>(DestinationOffset),
                        static_cast<std::ptrdiff_t>(CopyBytes),
                        std::uint8_t{0});
        } else {
            std::memcpy(Pending_.data() + DestinationOffset,
                        Data + ConsumedFrames * kDeskLinkAudioBytesPerFrame,
                        CopyBytes);
        }
        PendingFrames_ += CopyFrames;
        ConsumedFrames += CopyFrames;
        if (PendingFrames_ != kDeskLinkAudioFramesPerBlock) continue;

        AudioFrameMessage Frame;
        Frame.stream_id = StreamId_;
        Frame.sample_rate = kDeskLinkAudioSampleRate;
        Frame.frames_per_channel = kDeskLinkAudioFramesPerBlock;
        Frame.channels = kDeskLinkAudioChannels;
        Frame.bytes_per_sample = kDeskLinkAudioBytesPerSample;
        Frame.capture_timestamp_us = PendingTimestampUs_;
        Frame.pcm.assign(Pending_.begin(), Pending_.end());
        Output.push_back(std::move(Frame));
        PendingFrames_ = 0;
    }
    return true;
}

AudioJitterBuffer::AudioJitterBuffer(std::size_t target_frames, std::size_t max_frames)
    : target_frames_(std::max<std::size_t>(1, target_frames)),
      max_frames_(std::max(target_frames_, max_frames)) {}

bool AudioJitterBuffer::push(std::uint64_t sequence, AudioFrameMessage frame) {
    if (next_sequence_.has_value() && sequence < *next_sequence_) {
        ++dropped_late_;
        return false;
    }
    if (frames_.contains(sequence)) return false;
    if (frames_.size() >= max_frames_) return false;

    last_model_ = frame;
    frames_.emplace(sequence, std::move(frame));
    if (!next_sequence_.has_value() && frames_.size() >= target_frames_) {
        next_sequence_ = frames_.begin()->first;
    }
    return true;
}

AudioFrameMessage AudioJitterBuffer::make_silence_like(const AudioFrameMessage& model) const {
    AudioFrameMessage silence = model;
    std::fill(silence.pcm.begin(), silence.pcm.end(), std::uint8_t{0});
    return silence;
}

std::optional<AudioPlayout> AudioJitterBuffer::pop() {
    if (!next_sequence_.has_value()) {
        if (frames_.size() < target_frames_) return std::nullopt;
        next_sequence_ = frames_.begin()->first;
    }

    const auto expected = *next_sequence_;
    auto it = frames_.find(expected);
    if (it != frames_.end()) {
        AudioPlayout out{std::move(it->second), false};
        last_model_ = out.frame;
        frames_.erase(it);
        ++(*next_sequence_);
        return out;
    }

    if (frames_.empty()) return std::nullopt;

    // Wait until there is enough future data to prove this packet is not merely reordered.
    if (frames_.size() < target_frames_) return std::nullopt;

    const AudioFrameMessage& model = last_model_.has_value() ? *last_model_ : frames_.begin()->second;
    AudioPlayout out{make_silence_like(model), true};
    ++concealed_frames_;
    ++(*next_sequence_);
    return out;
}

} // namespace desklink
