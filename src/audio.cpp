#include "desklink/audio.hpp"

#include <algorithm>

namespace desklink {

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
    std::fill(silence.pcm.begin(), silence.pcm.end(), 0);
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
