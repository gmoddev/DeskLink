#include "desklink/pairing_wire.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace desklink {
namespace {

constexpr std::uint8_t kOfferFrameType = 1;
constexpr std::uint8_t kConfirmationFrameType = 2;
constexpr std::size_t kFixedOfferBodySize =
    MachineId{}.size() + 1 + Sha256Digest{}.size() + PairingNonce{}.size();
constexpr std::size_t kMaximumBufferedPairingBytes =
    kMaxPairingFrameSize + kPairingFrameHeaderSize;
constexpr std::size_t kMaximumRateLimitKeySize = 128;

void AppendU16(ByteBuffer& Output, std::uint16_t Value) {
    Output.push_back(static_cast<std::uint8_t>(Value >> 8u));
    Output.push_back(static_cast<std::uint8_t>(Value));
}

void AppendU32(ByteBuffer& Output, std::uint32_t Value) {
    Output.push_back(static_cast<std::uint8_t>(Value >> 24u));
    Output.push_back(static_cast<std::uint8_t>(Value >> 16u));
    Output.push_back(static_cast<std::uint8_t>(Value >> 8u));
    Output.push_back(static_cast<std::uint8_t>(Value));
}

std::uint16_t ReadU16(ByteSpan Bytes, std::size_t Offset) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(Bytes[Offset]) << 8u) |
        static_cast<std::uint16_t>(Bytes[Offset + 1]));
}

std::uint32_t ReadU32(ByteSpan Bytes, std::size_t Offset) noexcept {
    return (static_cast<std::uint32_t>(Bytes[Offset]) << 24u) |
           (static_cast<std::uint32_t>(Bytes[Offset + 1]) << 16u) |
           (static_cast<std::uint32_t>(Bytes[Offset + 2]) << 8u) |
           static_cast<std::uint32_t>(Bytes[Offset + 3]);
}

bool IsSafeUtf8DisplayName(std::string_view Value) noexcept {
    if (Value.empty() || Value.size() > kMaxPairingDisplayName) return false;
    std::size_t Index = 0;
    while (Index < Value.size()) {
        const auto First = static_cast<std::uint8_t>(Value[Index]);
        if (First < 0x80u) {
            if (First < 0x20u || First == 0x7Fu) return false;
            ++Index;
            continue;
        }

        std::size_t Continuations = 0;
        std::uint32_t CodePoint = 0;
        std::uint32_t Minimum = 0;
        if ((First & 0xE0u) == 0xC0u) {
            Continuations = 1;
            CodePoint = First & 0x1Fu;
            Minimum = 0x80u;
        } else if ((First & 0xF0u) == 0xE0u) {
            Continuations = 2;
            CodePoint = First & 0x0Fu;
            Minimum = 0x800u;
        } else if ((First & 0xF8u) == 0xF0u) {
            Continuations = 3;
            CodePoint = First & 0x07u;
            Minimum = 0x10000u;
        } else {
            return false;
        }
        if (Index + Continuations >= Value.size()) return false;
        for (std::size_t Count = 0; Count < Continuations; ++Count) {
            const auto Next = static_cast<std::uint8_t>(Value[Index + Count + 1]);
            if ((Next & 0xC0u) != 0x80u) return false;
            CodePoint = (CodePoint << 6u) | (Next & 0x3Fu);
        }
        if (CodePoint < Minimum || CodePoint > 0x10FFFFu ||
            (CodePoint >= 0xD800u && CodePoint <= 0xDFFFu) ||
            (CodePoint >= 0x80u && CodePoint <= 0x9Fu)) {
            return false;
        }
        Index += Continuations + 1;
    }
    return true;
}

} // namespace

std::optional<ByteBuffer> EncodePairingOfferFrame(const PairingOffer& Offer) {
    if (!IsValidPairingOffer(Offer) || !IsSafeUtf8DisplayName(Offer.DisplayName)) {
        return std::nullopt;
    }
    const auto BodySize = kFixedOfferBodySize + Offer.DisplayName.size();
    if (BodySize > std::numeric_limits<std::uint16_t>::max()) return std::nullopt;

    ByteBuffer Frame;
    Frame.reserve(kPairingFrameHeaderSize + BodySize);
    AppendU32(Frame, kPairingWireMagic);
    Frame.push_back(kPairingWireVersion);
    Frame.push_back(kOfferFrameType);
    AppendU16(Frame, static_cast<std::uint16_t>(BodySize));
    Frame.insert(Frame.end(), Offer.Machine.begin(), Offer.Machine.end());
    Frame.push_back(static_cast<std::uint8_t>(Offer.DisplayName.size()));
    Frame.insert(Frame.end(), Offer.DisplayName.begin(), Offer.DisplayName.end());
    Frame.insert(Frame.end(), Offer.CertificatePin.begin(), Offer.CertificatePin.end());
    Frame.insert(Frame.end(), Offer.Nonce.begin(), Offer.Nonce.end());
    return Frame;
}

std::optional<PairingOffer> DecodePairingOfferFrame(ByteSpan Frame) {
    if (Frame.size() < kPairingFrameHeaderSize || Frame.size() > kMaxPairingFrameSize ||
        ReadU32(Frame, 0) != kPairingWireMagic || Frame[4] != kPairingWireVersion ||
        Frame[5] != kOfferFrameType) {
        return std::nullopt;
    }
    const auto BodySize = static_cast<std::size_t>(ReadU16(Frame, 6));
    if (BodySize < kFixedOfferBodySize ||
        Frame.size() != kPairingFrameHeaderSize + BodySize) {
        return std::nullopt;
    }

    PairingOffer Offer;
    std::size_t Offset = kPairingFrameHeaderSize;
    std::copy_n(Frame.begin() + static_cast<std::ptrdiff_t>(Offset),
                Offer.Machine.size(), Offer.Machine.begin());
    Offset += Offer.Machine.size();
    const auto NameSize = static_cast<std::size_t>(Frame[Offset++]);
    if (NameSize == 0 || NameSize > kMaxPairingDisplayName ||
        BodySize != kFixedOfferBodySize + NameSize) {
        return std::nullopt;
    }
    Offer.DisplayName.assign(
        reinterpret_cast<const char*>(Frame.data() + Offset), NameSize);
    if (!IsSafeUtf8DisplayName(Offer.DisplayName)) return std::nullopt;
    Offset += NameSize;
    std::copy_n(Frame.begin() + static_cast<std::ptrdiff_t>(Offset),
                Offer.CertificatePin.size(), Offer.CertificatePin.begin());
    Offset += Offer.CertificatePin.size();
    std::copy_n(Frame.begin() + static_cast<std::ptrdiff_t>(Offset),
                Offer.Nonce.size(), Offer.Nonce.begin());
    return IsValidPairingOffer(Offer) ? std::optional<PairingOffer>{std::move(Offer)}
                                      : std::nullopt;
}

ByteBuffer EncodePairingConfirmationFrame() {
    ByteBuffer Frame;
    Frame.reserve(kPairingFrameHeaderSize);
    AppendU32(Frame, kPairingWireMagic);
    Frame.push_back(kPairingWireVersion);
    Frame.push_back(kConfirmationFrameType);
    AppendU16(Frame, 0);
    return Frame;
}

PairingWireStatus PairingFrameDecoder::Push(ByteSpan Bytes) {
    if (Status_ == PairingWireStatus::InvalidFrame ||
        Status_ == PairingWireStatus::Ready ||
        Bytes.size() > kMaximumBufferedPairingBytes ||
        Buffer_.size() > kMaximumBufferedPairingBytes - Bytes.size()) {
        Status_ = PairingWireStatus::InvalidFrame;
        Offer_.reset();
        ReadyType_.reset();
        return Status_;
    }
    Buffer_.insert(Buffer_.end(), Bytes.begin(), Bytes.end());
    Advance();
    return Status_;
}

PairingWireStatus PairingFrameDecoder::Status() const noexcept {
    return Status_;
}

std::optional<PairingWireFrameType> PairingFrameDecoder::ReadyType() const noexcept {
    return ReadyType_;
}

void PairingFrameDecoder::Advance() {
    Status_ = PairingWireStatus::Incomplete;
    ReadyType_.reset();
    Offer_.reset();
    if (Buffer_.size() < kPairingFrameHeaderSize) return;
    if (ReadU32(Buffer_, 0) != kPairingWireMagic ||
        Buffer_[4] != kPairingWireVersion) {
        Status_ = PairingWireStatus::InvalidFrame;
        return;
    }
    const auto FrameType = Buffer_[5];
    if (FrameType != kOfferFrameType && FrameType != kConfirmationFrameType) {
        Status_ = PairingWireStatus::InvalidFrame;
        return;
    }
    const auto BodySize = static_cast<std::size_t>(ReadU16(Buffer_, 6));
    const auto ExpectedSize = kPairingFrameHeaderSize +
        BodySize;
    if (ExpectedSize > kMaxPairingFrameSize ||
        (FrameType == kConfirmationFrameType && BodySize != 0)) {
        Status_ = PairingWireStatus::InvalidFrame;
        return;
    }
    if (Buffer_.size() < ExpectedSize) return;

    if (FrameType == kOfferFrameType) {
        Offer_ = DecodePairingOfferFrame(
            ByteSpan{Buffer_.data(), ExpectedSize});
        if (!Offer_) {
            Status_ = PairingWireStatus::InvalidFrame;
            return;
        }
        ReadyType_ = PairingWireFrameType::Offer;
    } else {
        ReadyType_ = PairingWireFrameType::Confirmation;
    }
    Buffer_.erase(Buffer_.begin(),
                  Buffer_.begin() + static_cast<std::ptrdiff_t>(ExpectedSize));
    Status_ = PairingWireStatus::Ready;
}

std::optional<PairingOffer> PairingFrameDecoder::TakeOffer() {
    if (Status_ != PairingWireStatus::Ready ||
        ReadyType_ != PairingWireFrameType::Offer || !Offer_) {
        return std::nullopt;
    }
    auto Result = std::move(Offer_);
    Advance();
    return Result;
}

bool PairingFrameDecoder::TakeConfirmation() {
    if (Status_ != PairingWireStatus::Ready ||
        ReadyType_ != PairingWireFrameType::Confirmation) {
        return false;
    }
    Advance();
    return true;
}

void PairingFrameDecoder::Reset() noexcept {
    Buffer_.clear();
    Offer_.reset();
    ReadyType_.reset();
    Status_ = PairingWireStatus::Incomplete;
}

AttemptRateLimiter::AttemptRateLimiter(IClock& Clock,
                                       std::size_t MaximumAttempts,
                                       std::chrono::milliseconds Window,
                                       std::size_t MaximumKeys)
    : Clock_(Clock),
      MaximumAttempts_(MaximumAttempts),
      Window_(Window),
      MaximumKeys_(MaximumKeys) {}

bool AttemptRateLimiter::Allow(std::string_view Key) {
    if (Key.empty() || Key.size() > kMaximumRateLimitKeySize ||
        MaximumAttempts_ == 0 || Window_ <= std::chrono::milliseconds::zero() ||
        MaximumKeys_ == 0) {
        return false;
    }
    std::scoped_lock Lock(Mutex_);
    const auto Now = Clock_.now();
    RemoveExpired(Now);
    const auto Match = std::find_if(Entries_.begin(), Entries_.end(), [&](const Entry& Item) {
        return Item.Key == Key;
    });
    if (Match != Entries_.end()) {
        if (Match->Attempts >= MaximumAttempts_) return false;
        ++Match->Attempts;
        return true;
    }
    if (Entries_.size() >= MaximumKeys_) return false;
    Entries_.push_back(Entry{std::string(Key), Now, 1});
    return true;
}

std::size_t AttemptRateLimiter::TrackedKeyCount() const {
    std::scoped_lock Lock(Mutex_);
    return Entries_.size();
}

void AttemptRateLimiter::RemoveExpired(IClock::time_point Now) {
    std::erase_if(Entries_, [&](const Entry& Item) {
        return Now - Item.WindowStart >= Window_;
    });
}

} // namespace desklink
