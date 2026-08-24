#include "desklink/audio.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <utility>

namespace desklink {
namespace {

constexpr std::size_t kMaximumSourceFramesPerPush = 8'192;
constexpr std::uint64_t kMaximumJitterSampleIntervalUs = 5'000'000;
constexpr std::uint64_t kMaximumJitterVariationUs = 100'000;
constexpr std::size_t kStableSamplesBeforeTargetDecrease = 200;

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

std::uint64_t SteadyTimestampUs() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
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

    if (Rebuffering_) {
        if (frames_.size() < target_frames_) return std::nullopt;
        Rebuffering_ = false;
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

void AudioJitterBuffer::SetTargetFrames(std::size_t TargetFrames) noexcept {
    const auto NewTarget = std::clamp(
        TargetFrames, std::size_t{1}, max_frames_);
    if (NewTarget > target_frames_ && next_sequence_.has_value() &&
        frames_.size() < NewTarget && !Rebuffering_) {
        Rebuffering_ = true;
        ++RebufferEvents_;
    }
    target_frames_ = NewTarget;
    if (Rebuffering_ && frames_.size() >= target_frames_) {
        Rebuffering_ = false;
    }
}

void AudioJitterBuffer::Reset() noexcept {
    frames_.clear();
    next_sequence_.reset();
    last_model_.reset();
    Rebuffering_ = false;
    RebufferEvents_ = 0;
    dropped_late_ = 0;
    concealed_frames_ = 0;
}

AudioAdaptiveJitterController::AudioAdaptiveJitterController(
    std::size_t InitialTargetFrames,
    std::size_t MaximumTargetFrames) noexcept
    : InitialTargetFrames_(std::max<std::size_t>(1, InitialTargetFrames)),
      MinimumTargetFrames_(std::min<std::size_t>(InitialTargetFrames_, 2)),
      MaximumTargetFrames_(std::max(
          InitialTargetFrames_, MaximumTargetFrames)),
      TargetFrames_(InitialTargetFrames_),
      PeakTargetFrames_(InitialTargetFrames_) {}

void AudioAdaptiveJitterController::Observe(
    std::uint64_t Sequence,
    std::uint64_t CaptureTimestampUs,
    std::uint64_t ArrivalTimestampUs) noexcept {
    if (Sequence == 0) return;
    if (!LastSequence_.has_value()) {
        LastSequence_ = Sequence;
        LastCaptureTimestampUs_ = CaptureTimestampUs;
        LastArrivalTimestampUs_ = ArrivalTimestampUs;
        return;
    }
    if (Sequence <= *LastSequence_) return;

    const bool TimestampAdvanced =
        CaptureTimestampUs > LastCaptureTimestampUs_ &&
        ArrivalTimestampUs >= LastArrivalTimestampUs_;
    const auto CaptureDelta = TimestampAdvanced
        ? CaptureTimestampUs - LastCaptureTimestampUs_
        : std::uint64_t{};
    const auto ArrivalDelta = TimestampAdvanced
        ? ArrivalTimestampUs - LastArrivalTimestampUs_
        : std::uint64_t{};
    LastSequence_ = Sequence;
    LastCaptureTimestampUs_ = CaptureTimestampUs;
    LastArrivalTimestampUs_ = ArrivalTimestampUs;
    if (!TimestampAdvanced || CaptureDelta > kMaximumJitterSampleIntervalUs ||
        ArrivalDelta > kMaximumJitterSampleIntervalUs) {
        StableSamples_ = 0;
        return;
    }

    const auto RawVariation = CaptureDelta > ArrivalDelta
        ? CaptureDelta - ArrivalDelta
        : ArrivalDelta - CaptureDelta;
    const auto Variation = std::min(
        RawVariation, kMaximumJitterVariationUs);
    JitterScaled_ = JitterScaled_ - (JitterScaled_ / 16) + Variation;
    const auto Estimated = EstimatedJitterUs();
    const auto BudgetUs = std::max(Variation, Estimated * 4);
    const auto ExtraFrames = static_cast<std::size_t>(
        (BudgetUs + kDeskLinkAudioBlockDurationUs - 1) /
        kDeskLinkAudioBlockDurationUs);
    const auto AvailableGrowth =
        MaximumTargetFrames_ - MinimumTargetFrames_;
    const auto DesiredTarget = MinimumTargetFrames_ +
        std::min(ExtraFrames, AvailableGrowth);
    if (DesiredTarget > TargetFrames_) {
        TargetFrames_ = DesiredTarget;
        PeakTargetFrames_ = std::max(PeakTargetFrames_, TargetFrames_);
        StableSamples_ = 0;
        ++TargetRaises_;
        return;
    }
    if (DesiredTarget < TargetFrames_) {
        ++StableSamples_;
        if (StableSamples_ >= kStableSamplesBeforeTargetDecrease) {
            --TargetFrames_;
            StableSamples_ = 0;
            ++TargetLowers_;
        }
        return;
    }
    StableSamples_ = 0;
}

void AudioAdaptiveJitterController::ObserveConcealment() noexcept {
    StableSamples_ = 0;
    if (TargetFrames_ >= MaximumTargetFrames_) return;
    ++TargetFrames_;
    PeakTargetFrames_ = std::max(PeakTargetFrames_, TargetFrames_);
    ++TargetRaises_;
}

void AudioAdaptiveJitterController::Reset() noexcept {
    TargetFrames_ = InitialTargetFrames_;
    PeakTargetFrames_ = InitialTargetFrames_;
    LastSequence_.reset();
    LastCaptureTimestampUs_ = 0;
    LastArrivalTimestampUs_ = 0;
    JitterScaled_ = 0;
    StableSamples_ = 0;
    TargetRaises_ = 0;
    TargetLowers_ = 0;
}

std::uint64_t AudioAdaptiveJitterController::EstimatedJitterUs() const noexcept {
    return (JitterScaled_ + 8) / 16;
}

AudioReceiver::AudioReceiver(RenderHandler Renderer,
                             std::size_t TargetFrames,
                             std::size_t MaximumFrames)
    : Renderer_(std::move(Renderer)),
      Buffer_(TargetFrames, MaximumFrames),
      JitterController_(
          TargetFrames,
          std::min(MaximumFrames,
                   kDeskLinkAudioMaximumAdaptiveTargetFrames)) {}

bool AudioReceiver::Push(std::uint64_t Sequence,
                         AudioFrameMessage Frame) noexcept {
    return PushAt(Sequence, std::move(Frame), SteadyTimestampUs());
}

bool AudioReceiver::PushAt(std::uint64_t Sequence,
                           AudioFrameMessage Frame,
                           std::uint64_t ArrivalTimestampUs) noexcept {
    try {
        std::scoped_lock Lock(Mutex_);
        if (Failed_) return false;
        if (!IsDeskLinkAudioFrame(Frame)) {
            ++Stats_.FormatRejected;
            return false;
        }
        if (Sequence == 0) {
            ++Stats_.SequenceRejected;
            return false;
        }
        if (Frame.stream_id == 0 ||
            (StreamId_.has_value() && Frame.stream_id != *StreamId_)) {
            ++Stats_.StreamRejected;
            return false;
        }
        const auto StreamId = Frame.stream_id;
        const auto CaptureTimestampUs = Frame.capture_timestamp_us;
        if (!Buffer_.push(Sequence, std::move(Frame))) {
            ++Stats_.SequenceRejected;
            return false;
        }
        if (!StreamId_) StreamId_ = StreamId;
        JitterController_.Observe(
            Sequence, CaptureTimestampUs, ArrivalTimestampUs);
        Buffer_.SetTargetFrames(JitterController_.TargetFrames());
        ++Stats_.Accepted;
        return true;
    } catch (...) {
        return false;
    }
}

AudioPumpResult AudioReceiver::Pump() noexcept {
    std::optional<AudioPlayout> Playout;
    try {
        {
            std::scoped_lock Lock(Mutex_);
            if (Failed_) return AudioPumpResult::RenderRejected;
            Playout = Buffer_.pop();
        }
        if (!Playout) return AudioPumpResult::Buffering;
        const bool Submitted = Renderer_ &&
            Renderer_(std::move(Playout->frame));
        std::scoped_lock Lock(Mutex_);
        if (!Submitted) {
            ++Stats_.RenderRejected;
            Failed_ = true;
            Buffer_.Reset();
            return AudioPumpResult::RenderRejected;
        }
        ++Stats_.Submitted;
        if (Playout->concealed) {
            ++Stats_.Concealed;
            JitterController_.ObserveConcealment();
            Buffer_.SetTargetFrames(JitterController_.TargetFrames());
        }
        return AudioPumpResult::Submitted;
    } catch (...) {
        std::scoped_lock Lock(Mutex_);
        ++Stats_.RenderRejected;
        Failed_ = true;
        Buffer_.Reset();
        return AudioPumpResult::RenderRejected;
    }
}

void AudioReceiver::Reset() noexcept {
    std::scoped_lock Lock(Mutex_);
    Buffer_.Reset();
    JitterController_.Reset();
    Buffer_.SetTargetFrames(JitterController_.TargetFrames());
    StreamId_.reset();
    Stats_ = {};
    Failed_ = false;
}

bool AudioReceiver::Failed() const noexcept {
    std::scoped_lock Lock(Mutex_);
    return Failed_;
}

AudioReceiverStats AudioReceiver::Stats() const noexcept {
    std::scoped_lock Lock(Mutex_);
    auto Result = Stats_;
    Result.CurrentTargetFrames = JitterController_.TargetFrames();
    Result.PeakTargetFrames = JitterController_.PeakTargetFrames();
    Result.EstimatedJitterUs = JitterController_.EstimatedJitterUs();
    Result.TargetRaises = JitterController_.TargetRaises();
    Result.TargetLowers = JitterController_.TargetLowers();
    Result.RebufferEvents = Buffer_.RebufferEvents();
    return Result;
}

} // namespace desklink
