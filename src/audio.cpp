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
constexpr std::size_t kDriftOccupancyDeadbandFrames =
    kDeskLinkAudioFramesPerBlock / 4;
constexpr std::size_t kMaximumDriftSourceFrames =
    static_cast<std::size_t>(kDeskLinkAudioFramesPerBlock) * 4;
constexpr std::size_t kMaximumDriftObservationSamples = 10'000;
constexpr std::uint64_t kQ32One = std::uint64_t{1} << 32;

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

AudioClockDriftController::AudioClockDriftController(
    std::size_t ObservationSamples,
    std::int32_t MaximumPpm,
    std::int32_t AdjustmentPpm) noexcept
    : ObservationSamples_(std::clamp<std::size_t>(
          ObservationSamples, 1, kMaximumDriftObservationSamples)),
      MaximumPpm_(std::max<std::int32_t>(1, MaximumPpm)),
      AdjustmentPpm_(std::clamp<std::int32_t>(
          AdjustmentPpm, 1, MaximumPpm_)) {}

void AudioClockDriftController::Observe(
    std::size_t BufferedSourceFrames,
    std::size_t TargetSourceFrames,
    bool PlayoutStarted) noexcept {
    if (!LastTargetSourceFrames_) {
        LastTargetSourceFrames_ = TargetSourceFrames;
        ClearObservation();
        return;
    }
    if (*LastTargetSourceFrames_ != TargetSourceFrames) {
        Discontinuity();
        LastTargetSourceFrames_ = TargetSourceFrames;
        return;
    }
    if (!PlayoutStarted) {
        ClearObservation();
        return;
    }

    const auto Buffered = static_cast<std::int64_t>(BufferedSourceFrames);
    const auto Target = static_cast<std::int64_t>(TargetSourceFrames);
    const auto MaximumError = static_cast<std::int64_t>(
        kDeskLinkAudioFramesPerBlock);
    OccupancyErrorSum_ += std::clamp(
        Buffered - Target, -MaximumError, MaximumError);
    ++Samples_;
    if (Samples_ < ObservationSamples_) return;

    const auto Threshold = static_cast<std::int64_t>(
        ObservationSamples_ * kDriftOccupancyDeadbandFrames);
    auto UpdatedPpm = AppliedPpm_;
    if (OccupancyErrorSum_ > Threshold) {
        UpdatedPpm = std::min(
            MaximumPpm_, AppliedPpm_ + AdjustmentPpm_);
    } else if (OccupancyErrorSum_ < -Threshold) {
        UpdatedPpm = std::max(
            -MaximumPpm_, AppliedPpm_ - AdjustmentPpm_);
    }
    if (UpdatedPpm != AppliedPpm_) {
        AppliedPpm_ = UpdatedPpm;
        ++Adjustments_;
    }
    ClearObservation();
}

void AudioClockDriftController::Discontinuity() noexcept {
    AppliedPpm_ = 0;
    ClearObservation();
    ++Discontinuities_;
}

void AudioClockDriftController::Reset() noexcept {
    LastTargetSourceFrames_.reset();
    AppliedPpm_ = 0;
    Adjustments_ = 0;
    Discontinuities_ = 0;
    ClearObservation();
}

void AudioClockDriftController::ClearObservation() noexcept {
    OccupancyErrorSum_ = 0;
    Samples_ = 0;
}

bool AudioClockDriftResampler::Push(AudioFrameMessage Frame) {
    if (!IsDeskLinkAudioFrame(Frame)) return false;
    if (OutputModel_ &&
        Frame.stream_id != OutputModel_->stream_id) {
        return false;
    }
    Compact();
    if (BufferedSourceFrames() + kDeskLinkAudioFramesPerBlock >
        kMaximumDriftSourceFrames) {
        return false;
    }
    if (!OutputModel_) {
        OutputModel_ = Frame;
        OutputModel_->pcm.clear();
        NextOutputTimestampUs_ = Frame.capture_timestamp_us;
    }
    SourcePcm_.insert(SourcePcm_.end(),
                      Frame.pcm.begin(), Frame.pcm.end());
    return true;
}

std::optional<AudioFrameMessage> AudioClockDriftResampler::Pop(
    std::int32_t AppliedPpm) {
    if (!OutputModel_) return std::nullopt;
    const auto BoundedPpm = std::clamp(
        AppliedPpm, -kDeskLinkAudioMaximumClockDriftPpm,
        kDeskLinkAudioMaximumClockDriftPpm);
    const auto StepAdjustment =
        (static_cast<std::int64_t>(BoundedPpm) *
         static_cast<std::int64_t>(kQ32One)) / 1'000'000;
    const auto StepQ32 = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(kQ32One) + StepAdjustment);
    const auto LastPosition = PhaseQ32_ + StepQ32 *
        (static_cast<std::uint64_t>(kDeskLinkAudioFramesPerBlock) - 1);
    auto RequiredFrames = static_cast<std::size_t>(LastPosition >> 32) + 1;
    if ((LastPosition & (kQ32One - 1)) != 0) ++RequiredFrames;
    if (BufferedSourceFrames() < RequiredFrames) return std::nullopt;

    AudioFrameMessage Output = *OutputModel_;
    Output.capture_timestamp_us = NextOutputTimestampUs_;
    Output.pcm.resize(kDeskLinkAudioBytesPerBlock);
    auto Position = PhaseQ32_;
    for (std::size_t Frame = 0;
         Frame < kDeskLinkAudioFramesPerBlock; ++Frame) {
        const auto SourceFrame = static_cast<std::size_t>(Position >> 32);
        const auto Fraction = Position & (kQ32One - 1);
        for (std::size_t Channel = 0;
             Channel < kDeskLinkAudioChannels; ++Channel) {
            const auto First = static_cast<std::int64_t>(
                ReadSample(SourceFrame, Channel));
            auto Sample = First;
            if (Fraction != 0) {
                const auto Second = static_cast<std::int64_t>(
                    ReadSample(SourceFrame + 1, Channel));
                Sample += ((Second - First) *
                    static_cast<std::int64_t>(Fraction)) /
                    static_cast<std::int64_t>(kQ32One);
            }
            WriteSample(Output.pcm, Frame, Channel,
                        static_cast<std::int16_t>(Sample));
        }
        Position += StepQ32;
    }

    const auto EndPosition = PhaseQ32_ + StepQ32 *
        static_cast<std::uint64_t>(kDeskLinkAudioFramesPerBlock);
    SourceOffsetFrames_ += static_cast<std::size_t>(EndPosition >> 32);
    PhaseQ32_ = EndPosition & (kQ32One - 1);
    if (NextOutputTimestampUs_ <=
        std::numeric_limits<std::uint64_t>::max() -
            kDeskLinkAudioBlockDurationUs) {
        NextOutputTimestampUs_ += kDeskLinkAudioBlockDurationUs;
    }
    Compact();
    return Output;
}

void AudioClockDriftResampler::Reset() noexcept {
    SourcePcm_.clear();
    SourceOffsetFrames_ = 0;
    PhaseQ32_ = 0;
    OutputModel_.reset();
    NextOutputTimestampUs_ = 0;
}

std::size_t AudioClockDriftResampler::BufferedSourceFrames() const noexcept {
    const auto TotalFrames = SourcePcm_.size() /
        kDeskLinkAudioBytesPerFrame;
    return TotalFrames >= SourceOffsetFrames_
        ? TotalFrames - SourceOffsetFrames_
        : 0;
}

void AudioClockDriftResampler::Compact() {
    if (SourceOffsetFrames_ == 0) return;
    const auto OffsetBytes = SourceOffsetFrames_ *
        kDeskLinkAudioBytesPerFrame;
    SourcePcm_.erase(
        SourcePcm_.begin(),
        SourcePcm_.begin() + static_cast<std::ptrdiff_t>(OffsetBytes));
    SourceOffsetFrames_ = 0;
}

std::int16_t AudioClockDriftResampler::ReadSample(
    std::size_t Frame, std::size_t Channel) const noexcept {
    const auto Offset = (SourceOffsetFrames_ + Frame) *
        kDeskLinkAudioBytesPerFrame +
        Channel * kDeskLinkAudioBytesPerSample;
    const auto Bits = static_cast<std::uint16_t>(SourcePcm_[Offset]) |
        (static_cast<std::uint16_t>(SourcePcm_[Offset + 1]) << 8);
    std::int16_t Sample{};
    std::memcpy(&Sample, &Bits, sizeof(Sample));
    return Sample;
}

void AudioClockDriftResampler::WriteSample(
    ByteBuffer& Output, std::size_t Frame,
    std::size_t Channel, std::int16_t Sample) noexcept {
    std::uint16_t Bits{};
    std::memcpy(&Bits, &Sample, sizeof(Bits));
    const auto Offset = Frame * kDeskLinkAudioBytesPerFrame +
        Channel * kDeskLinkAudioBytesPerSample;
    Output[Offset] = static_cast<std::uint8_t>(Bits & 0xffu);
    Output[Offset + 1] = static_cast<std::uint8_t>(Bits >> 8);
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
        if (LastCaptureTimestampUs_ &&
            (CaptureTimestampUs <= *LastCaptureTimestampUs_ ||
             CaptureTimestampUs - *LastCaptureTimestampUs_ >
                 kMaximumJitterSampleIntervalUs)) {
            DriftController_.Discontinuity();
        }
        LastCaptureTimestampUs_ = CaptureTimestampUs;
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
    std::optional<AudioFrameMessage> Corrected;
    try {
        {
            std::scoped_lock Lock(Mutex_);
            if (Failed_) return AudioPumpResult::RenderRejected;
            const auto TargetSourceFrames =
                (JitterController_.TargetFrames() - 1) *
                static_cast<std::size_t>(kDeskLinkAudioFramesPerBlock);
            const auto BufferedSourceFrames =
                Buffer_.buffered() *
                    static_cast<std::size_t>(
                        kDeskLinkAudioFramesPerBlock) +
                DriftResampler_.BufferedSourceFrames();
            DriftController_.Observe(
                BufferedSourceFrames, TargetSourceFrames,
                PlayoutStarted_);
            Corrected = DriftResampler_.Pop(
                DriftController_.AppliedPpm());
            for (std::size_t Attempt = 0;
                 !Corrected && Attempt < 2; ++Attempt) {
                auto Playout = Buffer_.pop();
                if (!Playout) break;
                PlayoutStarted_ = true;
                if (Playout->concealed) {
                    ++Stats_.Concealed;
                    const auto PreviousTarget =
                        JitterController_.TargetFrames();
                    JitterController_.ObserveConcealment();
                    Buffer_.SetTargetFrames(
                        JitterController_.TargetFrames());
                    if (PreviousTarget !=
                        JitterController_.TargetFrames()) {
                        DriftController_.Discontinuity();
                    }
                }
                if (!DriftResampler_.Push(
                        std::move(Playout->frame))) {
                    ++Stats_.RenderRejected;
                    Failed_ = true;
                    Buffer_.Reset();
                    DriftController_.Discontinuity();
                    DriftResampler_.Reset();
                    return AudioPumpResult::RenderRejected;
                }
                Corrected = DriftResampler_.Pop(
                    DriftController_.AppliedPpm());
            }
            if (Corrected) ApplyGain(*Corrected);
        }
        if (!Corrected) return AudioPumpResult::Buffering;
        const bool Submitted = Renderer_ &&
            Renderer_(std::move(*Corrected));
        std::scoped_lock Lock(Mutex_);
        if (!Submitted) {
            ++Stats_.RenderRejected;
            Failed_ = true;
            Buffer_.Reset();
            DriftController_.Discontinuity();
            DriftResampler_.Reset();
            return AudioPumpResult::RenderRejected;
        }
        ++Stats_.Submitted;
        return AudioPumpResult::Submitted;
    } catch (...) {
        std::scoped_lock Lock(Mutex_);
        ++Stats_.RenderRejected;
        Failed_ = true;
        Buffer_.Reset();
        DriftController_.Discontinuity();
        DriftResampler_.Reset();
        return AudioPumpResult::RenderRejected;
    }
}

bool AudioReceiver::SetGainPermyriad(std::uint16_t Gain) noexcept {
    if (Gain > kDeskLinkAudioMaximumGainPermyriad) return false;
    std::scoped_lock Lock(Mutex_);
    GainPermyriad_ = Gain;
    return true;
}

bool AudioReceiver::ToggleMuted() noexcept {
    std::scoped_lock Lock(Mutex_);
    Muted_ = !Muted_;
    return Muted_;
}

void AudioReceiver::SetMuted(bool Muted) noexcept {
    std::scoped_lock Lock(Mutex_);
    Muted_ = Muted;
}

std::uint16_t AudioReceiver::GainPermyriad() const noexcept {
    std::scoped_lock Lock(Mutex_);
    return GainPermyriad_;
}

bool AudioReceiver::Muted() const noexcept {
    std::scoped_lock Lock(Mutex_);
    return Muted_;
}

void AudioReceiver::ApplyGain(AudioFrameMessage& Frame) noexcept {
    const auto TargetGain = Muted_ ? std::uint16_t{} : GainPermyriad_;
    const auto StartingGain = AppliedGainPermyriad_;
    const auto Delta = static_cast<std::int32_t>(TargetGain) -
        static_cast<std::int32_t>(StartingGain);
    for (std::size_t FrameIndex = 0;
         FrameIndex < kDeskLinkAudioFramesPerBlock; ++FrameIndex) {
        const auto Gain = static_cast<std::int32_t>(StartingGain) +
            (Delta * static_cast<std::int32_t>(FrameIndex + 1)) /
                static_cast<std::int32_t>(kDeskLinkAudioFramesPerBlock);
        for (std::size_t Channel = 0;
             Channel < kDeskLinkAudioChannels; ++Channel) {
            const auto Offset = FrameIndex * kDeskLinkAudioBytesPerFrame +
                Channel * kDeskLinkAudioBytesPerSample;
            const auto Bits = static_cast<std::uint16_t>(Frame.pcm[Offset]) |
                (static_cast<std::uint16_t>(Frame.pcm[Offset + 1]) << 8);
            std::int16_t Sample{};
            std::memcpy(&Sample, &Bits, sizeof(Sample));
            const auto Scaled = (static_cast<std::int32_t>(Sample) * Gain) /
                static_cast<std::int32_t>(kDeskLinkAudioMaximumGainPermyriad);
            const auto Output = static_cast<std::int16_t>(Scaled);
            std::uint16_t OutputBits{};
            std::memcpy(&OutputBits, &Output, sizeof(OutputBits));
            Frame.pcm[Offset] = static_cast<std::uint8_t>(OutputBits & 0xffu);
            Frame.pcm[Offset + 1] =
                static_cast<std::uint8_t>(OutputBits >> 8);
        }
    }
    AppliedGainPermyriad_ = TargetGain;
}

void AudioReceiver::Reset() noexcept {
    std::scoped_lock Lock(Mutex_);
    Buffer_.Reset();
    JitterController_.Reset();
    Buffer_.SetTargetFrames(JitterController_.TargetFrames());
    DriftController_.Reset();
    DriftResampler_.Reset();
    StreamId_.reset();
    LastCaptureTimestampUs_.reset();
    Stats_ = {};
    AppliedGainPermyriad_ = Muted_ ? std::uint16_t{} : GainPermyriad_;
    PlayoutStarted_ = false;
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
    Result.AppliedClockDriftPpm = DriftController_.AppliedPpm();
    Result.ClockDriftAdjustments = DriftController_.Adjustments();
    Result.ClockDriftDiscontinuities =
        DriftController_.Discontinuities();
    Result.DriftBufferedSourceFrames =
        DriftResampler_.BufferedSourceFrames();
    return Result;
}

} // namespace desklink
