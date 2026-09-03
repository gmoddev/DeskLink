#include "desklink/voice.hpp"

#include <opus.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace desklink {

struct VoiceEncoder::State {
    OpusEncoder* Encoder{};
};

VoiceEncoder::VoiceEncoder() : State_(std::make_unique<State>()) {
    int Error = OPUS_OK;
    State_->Encoder = opus_encoder_create(
        static_cast<opus_int32>(kVoiceSampleRate), kVoiceChannels,
        OPUS_APPLICATION_VOIP, &Error);
    if (Error != OPUS_OK || State_->Encoder == nullptr) return;
    if (opus_encoder_ctl(State_->Encoder, OPUS_SET_BITRATE(
            static_cast<opus_int32>(kVoiceBitrate))) != OPUS_OK ||
        opus_encoder_ctl(State_->Encoder, OPUS_SET_VBR(1)) != OPUS_OK ||
        opus_encoder_ctl(State_->Encoder, OPUS_SET_INBAND_FEC(1)) != OPUS_OK ||
        opus_encoder_ctl(State_->Encoder, OPUS_SET_PACKET_LOSS_PERC(
            static_cast<int>(kVoicePacketLossPercent))) != OPUS_OK ||
        opus_encoder_ctl(State_->Encoder, OPUS_SET_DTX(0)) != OPUS_OK) {
        opus_encoder_destroy(State_->Encoder);
        State_->Encoder = nullptr;
    }
}

VoiceEncoder::~VoiceEncoder() {
    if (State_ && State_->Encoder) opus_encoder_destroy(State_->Encoder);
}

VoiceEncoder::VoiceEncoder(VoiceEncoder&&) noexcept = default;
VoiceEncoder& VoiceEncoder::operator=(VoiceEncoder&&) noexcept = default;

bool VoiceEncoder::Ready() const noexcept {
    return State_ && State_->Encoder;
}

std::optional<ByteBuffer> VoiceEncoder::Encode(
    std::span<const std::int16_t> Samples) {
    if (!Ready() || Samples.size() != kVoiceSamplesPerChannel) {
        return std::nullopt;
    }
    ByteBuffer Output(kVoiceMaximumEncodedBytes);
    const auto Written = opus_encode(
        State_->Encoder, Samples.data(), kVoiceSamplesPerChannel,
        Output.data(), static_cast<opus_int32>(Output.size()));
    if (Written <= 0 ||
        Written > static_cast<opus_int32>(kVoiceMaximumEncodedBytes)) {
        return std::nullopt;
    }
    Output.resize(static_cast<std::size_t>(Written));
    return Output;
}

bool VoiceEncoder::Reset() noexcept {
    return Ready() && opus_encoder_ctl(State_->Encoder, OPUS_RESET_STATE) == OPUS_OK;
}

struct VoiceDecoder::State {
    OpusDecoder* Decoder{};
};

VoiceDecoder::VoiceDecoder() : State_(std::make_unique<State>()) {
    int Error = OPUS_OK;
    State_->Decoder = opus_decoder_create(
        static_cast<opus_int32>(kVoiceSampleRate), kVoiceChannels, &Error);
    if (Error != OPUS_OK) State_->Decoder = nullptr;
}

VoiceDecoder::~VoiceDecoder() {
    if (State_ && State_->Decoder) opus_decoder_destroy(State_->Decoder);
}

VoiceDecoder::VoiceDecoder(VoiceDecoder&&) noexcept = default;
VoiceDecoder& VoiceDecoder::operator=(VoiceDecoder&&) noexcept = default;

bool VoiceDecoder::Ready() const noexcept {
    return State_ && State_->Decoder;
}

std::optional<VoicePcmFrame> VoiceDecoder::Decode(
    ByteSpan Encoded, bool DecodeFec, std::uint64_t CaptureTimestampUs) {
    if (!Ready() || Encoded.empty() ||
        Encoded.size() > kVoiceMaximumEncodedBytes) {
        return std::nullopt;
    }
    VoicePcmFrame Result;
    Result.CaptureTimestampUs = CaptureTimestampUs;
    Result.Concealed = DecodeFec;
    const auto Decoded = opus_decode(
        State_->Decoder, Encoded.data(),
        static_cast<opus_int32>(Encoded.size()), Result.Samples.data(),
        kVoiceSamplesPerChannel, DecodeFec ? 1 : 0);
    if (Decoded != static_cast<int>(kVoiceSamplesPerChannel)) {
        return std::nullopt;
    }
    return Result;
}

std::optional<VoicePcmFrame> VoiceDecoder::Conceal(
    std::uint64_t CaptureTimestampUs) {
    if (!Ready()) return std::nullopt;
    VoicePcmFrame Result;
    Result.CaptureTimestampUs = CaptureTimestampUs;
    Result.Concealed = true;
    const auto Decoded = opus_decode(
        State_->Decoder, nullptr, 0, Result.Samples.data(),
        kVoiceSamplesPerChannel, 0);
    if (Decoded != static_cast<int>(kVoiceSamplesPerChannel)) {
        return std::nullopt;
    }
    return Result;
}

bool VoiceDecoder::Reset() noexcept {
    return Ready() && opus_decoder_ctl(State_->Decoder, OPUS_RESET_STATE) == OPUS_OK;
}

VoiceReceiver::VoiceReceiver(
    RenderHandler Renderer, std::size_t TargetPackets,
    std::size_t MaximumPackets)
    : Renderer_(std::move(Renderer)),
      InitialTargetPackets_(std::clamp(
          TargetPackets, std::size_t{1}, kVoiceMaximumJitterPackets)),
      TargetPackets_(InitialTargetPackets_),
      MaximumPackets_(std::clamp(
          MaximumPackets, TargetPackets_, kVoiceMaximumQueuedPackets)) {
    Stats_.CurrentJitterTarget = TargetPackets_;
    Stats_.PeakJitterTarget = TargetPackets_;
}

void VoiceReceiver::ResetStreamLocked(
    std::uint32_t StreamId, std::uint64_t Sequence) noexcept {
    Packets_.clear();
    StreamId_ = StreamId;
    NextSequence_ = Sequence;
    Started_ = false;
    StablePacketCount_ = 0;
    AppliedGainPermyriad_ = Muted_ ? 0 : GainPermyriad_;
    (void)Decoder_.Reset();
    ++Stats_.StreamResets;
}

bool VoiceReceiver::Push(
    std::uint64_t Sequence, VoiceFrameMessage Frame) {
    std::scoped_lock Lock(Mutex_);
    if (Sequence == 0 || !IsValidVoiceFrameMessage(Frame)) {
        ++Stats_.FormatRejected;
        return false;
    }
    if (!StreamId_) {
        ResetStreamLocked(Frame.StreamId, Sequence);
    } else if (Frame.StreamId != *StreamId_) {
        if (Sequence <= HighestSequence_) {
            ++Stats_.StreamRejected;
            return false;
        }
        ResetStreamLocked(Frame.StreamId, Sequence);
    }
    if (NextSequence_ && Sequence < *NextSequence_) {
        ++Stats_.SequenceRejected;
        return false;
    }
    if (NextSequence_ && Sequence - *NextSequence_ >
            kVoiceMaximumFutureSequenceGap) {
        ++Stats_.SequenceRejected;
        return false;
    }
    if (Packets_.contains(Sequence)) {
        ++Stats_.Duplicates;
        return false;
    }
    const bool Reordered = Sequence < HighestSequence_;
    const bool Gap = HighestSequence_ != 0 && Sequence > HighestSequence_ + 1u;
    if (Reordered) ++Stats_.Reordered;
    ObserveJitterLocked(Reordered || Gap);
    HighestSequence_ = std::max(HighestSequence_, Sequence);
    Packets_.emplace(Sequence, std::move(Frame));
    while (Packets_.size() > MaximumPackets_) {
        Packets_.erase(Packets_.begin());
        ++Stats_.DroppedForBound;
        if (NextSequence_ && !Packets_.empty() &&
            *NextSequence_ < Packets_.begin()->first) {
            *NextSequence_ = Packets_.begin()->first;
            Started_ = false;
            (void)Decoder_.Reset();
            ++Stats_.StreamResets;
        }
    }
    ++Stats_.PacketsAccepted;
    return true;
}

VoicePumpResult VoiceReceiver::Pump() {
    std::optional<VoicePcmFrame> Output;
    {
        std::scoped_lock Lock(Mutex_);
        if (!NextSequence_ || Packets_.empty()) {
            return VoicePumpResult::Buffering;
        }
        if (!Started_) {
            if (Packets_.size() < TargetPackets_) {
                return VoicePumpResult::Buffering;
            }
            Started_ = true;
        }

        const auto Exact = Packets_.find(*NextSequence_);
        if (Exact != Packets_.end()) {
            const auto Timestamp = Exact->second.CaptureTimestampUs;
            Output = Decoder_.Decode(
                Exact->second.Encoded, false, Timestamp);
            Packets_.erase(Exact);
            ++*NextSequence_;
        } else {
            const auto Future = Packets_.lower_bound(*NextSequence_ + 1u);
            if (Future == Packets_.end()) {
                return VoicePumpResult::Buffering;
            }
            const auto MissingTimestamp = Future->second.CaptureTimestampUs >= 20'000
                ? Future->second.CaptureTimestampUs - 20'000 : 0;
            if (Future->first == *NextSequence_ + 1u) {
                Output = Decoder_.Decode(
                    Future->second.Encoded, true, MissingTimestamp);
                if (Output) ++Stats_.FecRecovered;
            }
            if (!Output) {
                Output = Decoder_.Conceal(MissingTimestamp);
                if (Output) ++Stats_.PlcGenerated;
            }
            ++*NextSequence_;
        }
        if (!Output) {
            ++Stats_.DecodeRejected;
            return VoicePumpResult::DecodeRejected;
        }
        ApplyGainLocked(*Output);
    }

    bool Accepted = false;
    try {
        Accepted = Renderer_ && Renderer_(std::move(*Output));
    } catch (...) {
        Accepted = false;
    }
    std::scoped_lock Lock(Mutex_);
    if (!Accepted) {
        ++Stats_.RenderRejected;
        return VoicePumpResult::RenderRejected;
    }
    ++Stats_.Submitted;
    return VoicePumpResult::Submitted;
}

std::size_t VoiceReceiver::PumpAvailable(std::size_t MaximumFrames) {
    std::size_t Submitted{};
    MaximumFrames = std::min(MaximumFrames, kVoiceMaximumQueuedPackets);
    for (std::size_t Index = 0; Index < MaximumFrames; ++Index) {
        const auto Result = Pump();
        if (Result == VoicePumpResult::Submitted) {
            ++Submitted;
            continue;
        }
        if (Result == VoicePumpResult::Buffering) break;
    }
    return Submitted;
}

bool VoiceReceiver::SetGainPermyriad(std::uint16_t Gain) noexcept {
    if (Gain > kVoiceMaximumGainPermyriad) return false;
    std::scoped_lock Lock(Mutex_);
    GainPermyriad_ = Gain;
    return true;
}

void VoiceReceiver::SetMuted(bool Muted) noexcept {
    std::scoped_lock Lock(Mutex_);
    Muted_ = Muted;
}

bool VoiceReceiver::Muted() const noexcept {
    std::scoped_lock Lock(Mutex_);
    return Muted_;
}

std::uint16_t VoiceReceiver::GainPermyriad() const noexcept {
    std::scoped_lock Lock(Mutex_);
    return GainPermyriad_;
}

void VoiceReceiver::Reset() noexcept {
    std::scoped_lock Lock(Mutex_);
    Packets_.clear();
    StreamId_.reset();
    NextSequence_.reset();
    HighestSequence_ = 0;
    Started_ = false;
    TargetPackets_ = InitialTargetPackets_;
    StablePacketCount_ = 0;
    Stats_.CurrentJitterTarget = TargetPackets_;
    (void)Decoder_.Reset();
}

VoiceReceiverStats VoiceReceiver::Stats() const noexcept {
    std::scoped_lock Lock(Mutex_);
    return Stats_;
}

void VoiceReceiver::ObserveJitterLocked(bool Unstable) noexcept {
    const auto MaximumTarget = std::min(
        MaximumPackets_, kVoiceMaximumJitterPackets);
    if (Unstable) {
        StablePacketCount_ = 0;
        if (TargetPackets_ < MaximumTarget) {
            ++TargetPackets_;
            Started_ = false;
        }
    } else if (TargetPackets_ > InitialTargetPackets_) {
        ++StablePacketCount_;
        if (StablePacketCount_ >= 100) {
            --TargetPackets_;
            StablePacketCount_ = 0;
        }
    }
    Stats_.CurrentJitterTarget = TargetPackets_;
    Stats_.PeakJitterTarget = std::max<std::uint64_t>(
        Stats_.PeakJitterTarget, TargetPackets_);
}

void VoiceReceiver::ApplyGainLocked(VoicePcmFrame& Frame) noexcept {
    const auto Target = static_cast<std::uint16_t>(Muted_ ? 0 : GainPermyriad_);
    const auto Start = static_cast<std::int32_t>(AppliedGainPermyriad_);
    const auto Delta = static_cast<std::int32_t>(Target) - Start;
    for (std::size_t Index = 0; Index < Frame.Samples.size(); ++Index) {
        const auto Gain = Start + static_cast<std::int32_t>(
            (static_cast<std::int64_t>(Delta) *
             static_cast<std::int64_t>(Index + 1u)) /
            static_cast<std::int64_t>(Frame.Samples.size()));
        const auto Scaled = (static_cast<std::int64_t>(Frame.Samples[Index]) *
            Gain) / kVoiceMaximumGainPermyriad;
        Frame.Samples[Index] = static_cast<std::int16_t>(std::clamp(
            Scaled,
            static_cast<std::int64_t>(std::numeric_limits<std::int16_t>::min()),
            static_cast<std::int64_t>(std::numeric_limits<std::int16_t>::max())));
    }
    AppliedGainPermyriad_ = Target;
}

} // namespace desklink
