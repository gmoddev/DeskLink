#pragma once

#include "desklink/protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>

namespace desklink {

inline constexpr std::uint32_t kVoiceBitrate = 32'000;
inline constexpr std::uint32_t kVoicePacketLossPercent = 10;
inline constexpr std::size_t kVoiceInitialJitterPackets = 2;
inline constexpr std::size_t kVoiceMaximumJitterPackets = 6;
inline constexpr std::size_t kVoiceMaximumQueuedPackets = 12;
inline constexpr std::uint64_t kVoiceMaximumFutureSequenceGap = 64;
inline constexpr std::uint16_t kVoiceMaximumGainPermyriad = 10'000;

struct VoicePcmFrame {
    std::array<std::int16_t, kVoiceSamplesPerChannel> Samples{};
    std::uint64_t CaptureTimestampUs{};
    bool Concealed{};
};

class VoiceEncoder final {
public:
    struct State;

    VoiceEncoder();
    ~VoiceEncoder();
    VoiceEncoder(VoiceEncoder&&) noexcept;
    VoiceEncoder& operator=(VoiceEncoder&&) noexcept;
    VoiceEncoder(const VoiceEncoder&) = delete;
    VoiceEncoder& operator=(const VoiceEncoder&) = delete;

    [[nodiscard]] bool Ready() const noexcept;
    [[nodiscard]] std::optional<ByteBuffer> Encode(
        std::span<const std::int16_t> Samples);
    [[nodiscard]] bool Reset() noexcept;

private:
    std::unique_ptr<State> State_;
};

class VoiceDecoder final {
public:
    struct State;

    VoiceDecoder();
    ~VoiceDecoder();
    VoiceDecoder(VoiceDecoder&&) noexcept;
    VoiceDecoder& operator=(VoiceDecoder&&) noexcept;
    VoiceDecoder(const VoiceDecoder&) = delete;
    VoiceDecoder& operator=(const VoiceDecoder&) = delete;

    [[nodiscard]] bool Ready() const noexcept;
    [[nodiscard]] std::optional<VoicePcmFrame> Decode(
        ByteSpan Encoded, bool DecodeFec, std::uint64_t CaptureTimestampUs);
    [[nodiscard]] std::optional<VoicePcmFrame> Conceal(
        std::uint64_t CaptureTimestampUs);
    [[nodiscard]] bool Reset() noexcept;

private:
    std::unique_ptr<State> State_;
};

struct VoiceReceiverStats {
    std::uint64_t PacketsAccepted{};
    std::uint64_t FormatRejected{};
    std::uint64_t StreamResets{};
    std::uint64_t StreamRejected{};
    std::uint64_t SequenceRejected{};
    std::uint64_t Duplicates{};
    std::uint64_t Reordered{};
    std::uint64_t FecRecovered{};
    std::uint64_t PlcGenerated{};
    std::uint64_t DecodeRejected{};
    std::uint64_t Submitted{};
    std::uint64_t RenderRejected{};
    std::uint64_t DroppedForBound{};
    std::uint64_t CurrentJitterTarget{kVoiceInitialJitterPackets};
    std::uint64_t PeakJitterTarget{kVoiceInitialJitterPackets};
};

enum class VoicePumpResult {
    Buffering,
    Submitted,
    DecodeRejected,
    RenderRejected,
};

class VoiceReceiver final {
public:
    using RenderHandler = std::function<bool(VoicePcmFrame)>;

    explicit VoiceReceiver(
        RenderHandler Renderer,
        std::size_t TargetPackets = kVoiceInitialJitterPackets,
        std::size_t MaximumPackets = kVoiceMaximumQueuedPackets);

    [[nodiscard]] bool Push(
        std::uint64_t Sequence, VoiceFrameMessage Frame);
    [[nodiscard]] VoicePumpResult Pump();
    [[nodiscard]] std::size_t PumpAvailable(
        std::size_t MaximumFrames = kVoiceMaximumQueuedPackets);
    [[nodiscard]] bool SetGainPermyriad(std::uint16_t Gain) noexcept;
    void SetMuted(bool Muted) noexcept;
    [[nodiscard]] bool Muted() const noexcept;
    [[nodiscard]] std::uint16_t GainPermyriad() const noexcept;
    void Reset() noexcept;
    [[nodiscard]] VoiceReceiverStats Stats() const noexcept;

private:
    void ResetStreamLocked(std::uint32_t StreamId,
                           std::uint64_t Sequence) noexcept;
    void ApplyGainLocked(VoicePcmFrame& Frame) noexcept;

    RenderHandler Renderer_;
    VoiceDecoder Decoder_;
    mutable std::mutex Mutex_;
    std::map<std::uint64_t, VoiceFrameMessage> Packets_;
    std::optional<std::uint32_t> StreamId_;
    std::optional<std::uint64_t> NextSequence_;
    std::uint64_t HighestSequence_{};
    std::size_t TargetPackets_{};
    std::size_t MaximumPackets_{};
    std::uint16_t GainPermyriad_{kVoiceMaximumGainPermyriad};
    std::uint16_t AppliedGainPermyriad_{kVoiceMaximumGainPermyriad};
    bool Muted_{};
    bool Started_{};
    VoiceReceiverStats Stats_;
};

} // namespace desklink
