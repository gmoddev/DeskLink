#pragma once

#include "desklink/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>

namespace desklink {

struct AudioPlayout {
    AudioFrameMessage frame;
    bool concealed{};
};

class AudioJitterBuffer {
public:
    explicit AudioJitterBuffer(std::size_t target_frames = 4, std::size_t max_frames = 20);

    [[nodiscard]] bool push(std::uint64_t sequence, AudioFrameMessage frame);
    [[nodiscard]] std::optional<AudioPlayout> pop();
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

} // namespace desklink
