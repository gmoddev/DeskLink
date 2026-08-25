#include "desklink/pairing.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <utility>

namespace desklink {
namespace {

constexpr std::chrono::seconds kMinimumPairingWindow{1};
constexpr std::chrono::seconds kMaximumPairingWindow{300};
constexpr std::string_view kPairingDomain = "DeskLink-Pairing-v1";

bool IsAllZero(ByteSpan Bytes) noexcept {
    std::uint8_t Combined = 0;
    for (const auto Byte : Bytes) Combined = static_cast<std::uint8_t>(Combined | Byte);
    return Combined == 0;
}

void AppendBytes(ByteBuffer& Output, ByteSpan Bytes) {
    Output.insert(Output.end(), Bytes.begin(), Bytes.end());
}

void AppendString(ByteBuffer& Output, std::string_view Value) {
    Output.push_back(static_cast<std::uint8_t>(Value.size()));
    Output.insert(Output.end(), Value.begin(), Value.end());
}

ByteBuffer EncodeOffer(const PairingOffer& Offer) {
    ByteBuffer Encoded;
    Encoded.reserve(1 + Offer.DisplayName.size() + Offer.Machine.size() +
                    Offer.CertificatePin.size() + Offer.Nonce.size());
    AppendBytes(Encoded, Offer.Machine);
    AppendString(Encoded, Offer.DisplayName);
    AppendBytes(Encoded, Offer.CertificatePin);
    AppendBytes(Encoded, Offer.Nonce);
    return Encoded;
}

std::optional<std::string> DeriveVerificationCode(const PairingOffer& First,
                                                  const PairingOffer& Second,
                                                  const IPairingCrypto& Crypto) {
    auto FirstEncoded = EncodeOffer(First);
    auto SecondEncoded = EncodeOffer(Second);
    if (SecondEncoded < FirstEncoded) std::swap(FirstEncoded, SecondEncoded);

    ByteBuffer Transcript;
    Transcript.reserve(kPairingDomain.size() + FirstEncoded.size() + SecondEncoded.size());
    AppendBytes(Transcript, ByteSpan{
        reinterpret_cast<const std::uint8_t*>(kPairingDomain.data()), kPairingDomain.size()});
    AppendBytes(Transcript, FirstEncoded);
    AppendBytes(Transcript, SecondEncoded);

    const auto Digest = Crypto.HashSha256(Transcript);
    if (!Digest) return std::nullopt;

    const auto Value = (static_cast<std::uint32_t>((*Digest)[0]) << 16u) |
                       (static_cast<std::uint32_t>((*Digest)[1]) << 8u) |
                       static_cast<std::uint32_t>((*Digest)[2]);
    std::ostringstream Code;
    Code << std::setfill('0') << std::setw(6) << (Value % 1'000'000u);
    return Code.str();
}

bool ConstantTimeEqual(std::string_view Left, std::string_view Right) noexcept {
    std::size_t Difference = Left.size() ^ Right.size();
    const auto Count = std::max(Left.size(), Right.size());
    for (std::size_t Index = 0; Index < Count; ++Index) {
        const auto LeftByte = Index < Left.size() ? static_cast<unsigned char>(Left[Index]) : 0u;
        const auto RightByte = Index < Right.size() ? static_cast<unsigned char>(Right[Index]) : 0u;
        Difference |= static_cast<std::size_t>(LeftByte ^ RightByte);
    }
    return Difference == 0;
}

} // namespace

bool IsValidPairingOffer(const PairingOffer& Offer) noexcept {
    return !IsAllZero(Offer.Machine) &&
           !Offer.DisplayName.empty() &&
           Offer.DisplayName.size() <= kMaxPairingDisplayName &&
           !IsAllZero(Offer.CertificatePin) &&
           !IsAllZero(Offer.Nonce);
}

std::optional<TrustedPeer> InMemoryTrustStore::GetPeer(const MachineId& Machine) const {
    std::scoped_lock Lock(Mutex_);
    const auto Match = std::find_if(Peers_.begin(), Peers_.end(), [&](const TrustedPeer& Peer) {
        return Peer.Identity.machine_id == Machine;
    });
    if (Match == Peers_.end()) return std::nullopt;
    return *Match;
}

std::optional<TrustedPeer> InMemoryTrustStore::FindPeerByFingerprint(
    std::string_view Fingerprint) const {
    std::scoped_lock Lock(Mutex_);
    const auto Parsed = ParseFingerprint(Fingerprint);
    if (!Parsed) return std::nullopt;
    const auto Canonical = FormatFingerprint(*Parsed);
    const auto Match = std::find_if(Peers_.begin(), Peers_.end(), [&](const TrustedPeer& Peer) {
        return Peer.Identity.public_key_fingerprint == Canonical;
    });
    if (Match == Peers_.end()) return std::nullopt;
    return *Match;
}

bool InMemoryTrustStore::SavePeer(TrustedPeer Peer) {
    std::scoped_lock Lock(Mutex_);
    const auto Fingerprint = ParseFingerprint(Peer.Identity.public_key_fingerprint);
    if (IsAllZero(Peer.Identity.machine_id) || Peer.Identity.display_name.empty() ||
        Peer.Identity.display_name.size() > kMaxPairingDisplayName || !Fingerprint ||
        IsAllZero(*Fingerprint) || (Peer.Capabilities.bits() & ~kKnownCapabilityBits) != 0) {
        return false;
    }
    Peer.Identity.public_key_fingerprint = FormatFingerprint(*Fingerprint);

    const auto DuplicatePin = std::find_if(
        Peers_.begin(), Peers_.end(), [&](const TrustedPeer& Existing) {
            return Existing.Identity.machine_id != Peer.Identity.machine_id &&
                   Existing.Identity.public_key_fingerprint ==
                       Peer.Identity.public_key_fingerprint;
        });
    if (DuplicatePin != Peers_.end()) return false;

    const auto Match = std::find_if(Peers_.begin(), Peers_.end(), [&](const TrustedPeer& Existing) {
        return Existing.Identity.machine_id == Peer.Identity.machine_id;
    });
    if (Match != Peers_.end()) {
        *Match = std::move(Peer);
        return true;
    }
    if (Peers_.size() >= kMaxTrustedPeers) return false;
    Peers_.push_back(std::move(Peer));
    return true;
}

bool InMemoryTrustStore::RemovePeer(const MachineId& Machine) {
    std::scoped_lock Lock(Mutex_);
    const auto PreviousSize = Peers_.size();
    std::erase_if(Peers_, [&](const TrustedPeer& Peer) {
        return Peer.Identity.machine_id == Machine;
    });
    return Peers_.size() != PreviousSize;
}

PairingCoordinator::PairingCoordinator(PeerIdentity LocalIdentity,
                                       Sha256Digest LocalCertificatePin,
                                       IClock& Clock,
                                       IPairingCrypto& Crypto,
                                       ITrustStore& TrustStore)
    : LocalIdentity_(std::move(LocalIdentity)),
      LocalCertificatePin_(LocalCertificatePin),
      Clock_(Clock),
      Crypto_(Crypto),
      TrustStore_(TrustStore) {}

bool PairingCoordinator::BeginPairing(std::chrono::seconds Duration) {
    std::scoped_lock Lock(Mutex_);
    if (Duration < kMinimumPairingWindow || Duration > kMaximumPairingWindow ||
        IsAllZero(LocalIdentity_.machine_id) || LocalIdentity_.display_name.empty() ||
        LocalIdentity_.display_name.size() > kMaxPairingDisplayName ||
        IsAllZero(LocalCertificatePin_)) {
        return false;
    }
    PairingNonce Nonce{};
    if (!Crypto_.FillRandom(Nonce) || IsAllZero(Nonce)) return false;
    LocalNonce_ = Nonce;
    PairingDeadline_ = Clock_.now() + Duration;
    PairingOpen_ = true;
    return true;
}

void PairingCoordinator::ClosePairing() {
    std::scoped_lock Lock(Mutex_);
    PairingOpen_ = false;
    LocalNonce_.fill(0);
}

bool PairingCoordinator::IsPairingOpen() const {
    std::scoped_lock Lock(Mutex_);
    return PairingOpen_ && Clock_.now() < PairingDeadline_;
}

std::optional<PairingOffer> PairingCoordinator::CreateOffer() const {
    std::scoped_lock Lock(Mutex_);
    if (!IsPairingOpen()) return std::nullopt;
    return PairingOffer{
        LocalIdentity_.machine_id,
        LocalIdentity_.display_name,
        LocalCertificatePin_,
        LocalNonce_};
}

PairingCandidate PairingCoordinator::InspectOffer(const PairingOffer& RemoteOffer) const {
    std::scoped_lock Lock(Mutex_);
    if (!IsPairingOpen()) return {PairingStatus::WindowClosed, {}, {}};
    if (!IsValidPairingOffer(RemoteOffer) || RemoteOffer.Machine == LocalIdentity_.machine_id) {
        return {PairingStatus::InvalidOffer, {}, {}};
    }
    const auto LocalOffer = CreateOffer();
    if (!LocalOffer) return {PairingStatus::WindowClosed, {}, {}};
    const auto Code = DeriveVerificationCode(*LocalOffer, RemoteOffer, Crypto_);
    if (!Code) return {PairingStatus::CryptoFailure, {}, {}};

    PeerIdentity Identity;
    Identity.machine_id = RemoteOffer.Machine;
    Identity.display_name = RemoteOffer.DisplayName;
    Identity.public_key_fingerprint = FormatFingerprint(RemoteOffer.CertificatePin);
    return {PairingStatus::Ready, std::move(Identity), *Code};
}

bool PairingCoordinator::ConfirmOffer(const PairingOffer& RemoteOffer,
                                      std::string_view VerificationCode,
                                      CapabilitySet Capabilities) {
    std::scoped_lock Lock(Mutex_);
    const auto Candidate = InspectOffer(RemoteOffer);
    if (Candidate.Status != PairingStatus::Ready ||
        !ConstantTimeEqual(Candidate.VerificationCode, VerificationCode)) {
        return false;
    }
    if (!TrustStore_.SavePeer(TrustedPeer{Candidate.Identity, Capabilities})) return false;
    ClosePairing();
    return true;
}

std::string FormatFingerprint(const Sha256Digest& Digest) {
    constexpr char Alphabet[] = "0123456789abcdef";
    std::string Result;
    Result.resize(Digest.size() * 2);
    for (std::size_t Index = 0; Index < Digest.size(); ++Index) {
        Result[Index * 2] = Alphabet[Digest[Index] >> 4u];
        Result[Index * 2 + 1] = Alphabet[Digest[Index] & 0x0Fu];
    }
    return Result;
}

std::optional<Sha256Digest> ParseFingerprint(std::string_view Fingerprint) {
    if (Fingerprint.size() != kSha256DigestSize * 2) return std::nullopt;
    const auto Nibble = [](char Value) -> std::optional<std::uint8_t> {
        if (Value >= '0' && Value <= '9') return static_cast<std::uint8_t>(Value - '0');
        if (Value >= 'a' && Value <= 'f') return static_cast<std::uint8_t>(Value - 'a' + 10);
        if (Value >= 'A' && Value <= 'F') return static_cast<std::uint8_t>(Value - 'A' + 10);
        return std::nullopt;
    };

    Sha256Digest Result{};
    for (std::size_t Index = 0; Index < Result.size(); ++Index) {
        const auto High = Nibble(Fingerprint[Index * 2]);
        const auto Low = Nibble(Fingerprint[Index * 2 + 1]);
        if (!High || !Low) return std::nullopt;
        Result[Index] = static_cast<std::uint8_t>((*High << 4u) | *Low);
    }
    return Result;
}

std::optional<TrustedPeer> MatchPeerCertificate(const ITrustStore& TrustStore,
                                                const IPairingCrypto& Crypto,
                                                ByteSpan CertificateDer,
                                                const MachineId* ExpectedMachine) {
    if (CertificateDer.empty()) return std::nullopt;
    const auto Digest = Crypto.HashSha256(CertificateDer);
    if (!Digest) return std::nullopt;
    const auto Fingerprint = FormatFingerprint(*Digest);
    const auto Match = ExpectedMachine
        ? TrustStore.GetPeer(*ExpectedMachine)
        : TrustStore.FindPeerByFingerprint(Fingerprint);
    if (!Match) return std::nullopt;
    const auto Presented = ParseFingerprint(Fingerprint);
    const auto Expected = ParseFingerprint(Match->Identity.public_key_fingerprint);
    if (!Presented || !Expected) return std::nullopt;
    std::uint8_t Difference = 0;
    for (std::size_t Index = 0; Index < Presented->size(); ++Index) {
        Difference = static_cast<std::uint8_t>(
            Difference | ((*Presented)[Index] ^ (*Expected)[Index]));
    }
    if (Difference != 0) return std::nullopt;
    return Match;
}

bool IsTrustedPeer(const ITrustStore& TrustStore, const PeerIdentity& Identity) noexcept {
    const auto Expected = TrustStore.GetPeer(Identity.machine_id);
    if (!Expected) return false;
    const auto ExpectedPin = ParseFingerprint(Expected->Identity.public_key_fingerprint);
    const auto PresentedPin = ParseFingerprint(Identity.public_key_fingerprint);
    if (!ExpectedPin || !PresentedPin) return false;

    std::uint8_t Difference = 0;
    for (std::size_t Index = 0; Index < ExpectedPin->size(); ++Index) {
        Difference = static_cast<std::uint8_t>(Difference | ((*ExpectedPin)[Index] ^ (*PresentedPin)[Index]));
    }
    return Difference == 0;
}

} // namespace desklink
