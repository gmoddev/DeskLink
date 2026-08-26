#pragma once

#include "desklink/pairing.hpp"

#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace desklink {

inline constexpr std::uint32_t kPairingWireMagic = 0x444C5032u; // "DLP2"
inline constexpr std::uint8_t kPairingWireVersion = 2;
inline constexpr std::size_t kPairingFrameHeaderSize = 8;
inline constexpr std::size_t kMaxPairingFrameSize =
    kPairingFrameHeaderSize + 16 + 1 + kMaxPairingDisplayName +
    kSha256DigestSize + kPairingNonceSize;

[[nodiscard]] ByteBuffer EncodePairingCommitmentFrame(
    const PairingCommitment& Commitment);
[[nodiscard]] std::optional<ByteBuffer> EncodePairingOfferFrame(const PairingOffer& Offer);
[[nodiscard]] std::optional<PairingOffer> DecodePairingOfferFrame(ByteSpan Frame);
[[nodiscard]] ByteBuffer EncodePairingConfirmationFrame(
    const Sha256Digest& TranscriptDigest,
    CapabilitySet Capabilities);
[[nodiscard]] ByteBuffer EncodePairingCompletionFrame(
    const Sha256Digest& TranscriptDigest);

struct PairingConfirmation {
    Sha256Digest TranscriptDigest{};
    CapabilitySet Capabilities;
};

enum class PairingWireFrameType {
    Commitment,
    Offer,
    Confirmation,
    Completion,
};

enum class PairingWireStatus {
    Incomplete,
    Ready,
    InvalidFrame,
};

class PairingFrameDecoder final {
public:
    [[nodiscard]] PairingWireStatus Push(ByteSpan Bytes);
    [[nodiscard]] PairingWireStatus Status() const noexcept;
    [[nodiscard]] std::optional<PairingWireFrameType> ReadyType() const noexcept;
    [[nodiscard]] std::optional<PairingCommitment> TakeCommitment();
    [[nodiscard]] std::optional<PairingOffer> TakeOffer();
    [[nodiscard]] std::optional<PairingConfirmation> TakeConfirmation();
    [[nodiscard]] std::optional<Sha256Digest> TakeCompletion();
    void Reset() noexcept;

private:
    void Advance();

    ByteBuffer Buffer_;
    std::optional<PairingCommitment> Commitment_;
    std::optional<PairingOffer> Offer_;
    std::optional<PairingConfirmation> Confirmation_;
    std::optional<Sha256Digest> Completion_;
    std::optional<PairingWireFrameType> ReadyType_;
    PairingWireStatus Status_{PairingWireStatus::Incomplete};
};

class AttemptRateLimiter final {
public:
    AttemptRateLimiter(IClock& Clock,
                       std::size_t MaximumAttempts,
                       std::chrono::milliseconds Window,
                       std::size_t MaximumKeys = 256);

    [[nodiscard]] bool Allow(std::string_view Key);
    [[nodiscard]] std::size_t TrackedKeyCount() const;

private:
    struct Entry {
        std::string Key;
        IClock::time_point WindowStart{};
        std::size_t Attempts{};
    };

    void RemoveExpired(IClock::time_point Now);

    IClock& Clock_;
    std::size_t MaximumAttempts_{};
    std::chrono::milliseconds Window_{};
    std::size_t MaximumKeys_{};
    mutable std::mutex Mutex_;
    std::vector<Entry> Entries_;
};

} // namespace desklink
