#include "desklink/runtime_broker.hpp"

#include <algorithm>
#include <limits>

namespace desklink {
namespace {

constexpr std::uint16_t kMaximumRecordedRetryAttempt = 16;

std::uint64_t MixJitter(std::uint64_t Value) noexcept {
    Value ^= Value >> 30u;
    Value *= 0xbf58476d1ce4e5b9ull;
    Value ^= Value >> 27u;
    Value *= 0x94d049bb133111ebull;
    return Value ^ (Value >> 31u);
}

bool IsNonzeroMachine(const MachineId& Machine) noexcept {
    return std::any_of(Machine.begin(), Machine.end(),
                       [](std::uint8_t Byte) { return Byte != 0; });
}

bool HasOnlyKnownCapabilities(CapabilitySet Capabilities) noexcept {
    return (Capabilities.bits() & ~kKnownCapabilityBits) == 0;
}

bool IsSubset(CapabilitySet Candidate, CapabilitySet Existing) noexcept {
    return (Candidate.bits() & ~Existing.bits()) == 0;
}

bool MachineLess(const MachineId& Left, const MachineId& Right) noexcept {
    return std::lexicographical_compare(
        Left.begin(), Left.end(), Right.begin(), Right.end());
}

bool IsValidCandidate(const PairingCandidate& Candidate) noexcept {
    if (Candidate.Status != PairingStatus::Ready ||
        Candidate.Identity.display_name.empty() ||
        Candidate.Identity.display_name.size() > kMaxPairingDisplayName ||
        Candidate.VerificationCode.size() != 6 ||
        !std::all_of(Candidate.VerificationCode.begin(),
                     Candidate.VerificationCode.end(),
                     [](char Value) { return Value >= '0' && Value <= '9'; }) ||
        std::all_of(Candidate.TranscriptDigest.begin(),
                    Candidate.TranscriptDigest.end(),
                    [](std::uint8_t Byte) { return Byte == 0; })) {
        return false;
    }
    const auto Fingerprint = ParseFingerprint(
        Candidate.Identity.public_key_fingerprint);
    return Fingerprint &&
           Candidate.Identity.machine_id == DeriveMachineId(*Fingerprint);
}

} // namespace

std::chrono::milliseconds BrokerReconnectDelay(
    std::uint16_t Attempt, std::uint64_t JitterSeed) noexcept {
    const auto BoundedAttempt = std::clamp<std::uint16_t>(Attempt, 1, 16);
    const auto Shift = std::min<std::uint16_t>(BoundedAttempt - 1, 5);
    const auto Base = std::chrono::milliseconds{
        1'000ll << static_cast<unsigned>(Shift)};
    const auto Mixed = MixJitter(
        JitterSeed ^ (static_cast<std::uint64_t>(BoundedAttempt) *
                      0x9e3779b97f4a7c15ull));
    const auto Permille = 800ll + static_cast<std::int64_t>(Mixed % 401ull);
    const auto Jittered = std::chrono::milliseconds{
        (Base.count() * Permille) / 1'000ll};
    return std::clamp(Jittered, kBrokerReconnectMinimumDelay,
                      std::chrono::duration_cast<std::chrono::milliseconds>(
                          kBrokerReconnectMaximumDelay));
}

BrokerReconnectController::BrokerReconnectController(
    std::uint64_t JitterSeed) noexcept
    : JitterSeed_(JitterSeed) {}

void BrokerReconnectController::ResetForConfiguration(
    IClock::time_point Now) noexcept {
    State_.Failure = BrokerRuntimeFailure::None;
    State_.RetryAttempt = 0;
    State_.RetryAt = Now;
    State_.Phase = State_.Paused
        ? BrokerRuntimePhase::Paused
        : BrokerRuntimePhase::Stopped;
}

void BrokerReconnectController::Pause(IClock::time_point Now) noexcept {
    State_.Paused = true;
    State_.Phase = BrokerRuntimePhase::Paused;
    State_.Failure = BrokerRuntimeFailure::None;
    State_.RetryAttempt = 0;
    State_.RetryAt = Now;
}

void BrokerReconnectController::Resume(IClock::time_point Now) noexcept {
    State_.Paused = false;
    ResetForConfiguration(Now);
}

bool BrokerReconnectController::Begin(BrokerRuntimePhase Phase) noexcept {
    if (State_.Paused ||
        (Phase != BrokerRuntimePhase::Listening &&
         Phase != BrokerRuntimePhase::Discovering &&
         Phase != BrokerRuntimePhase::Connecting)) {
        return false;
    }
    State_.Phase = Phase;
    State_.Failure = BrokerRuntimeFailure::None;
    return true;
}

void BrokerReconnectController::ConnectedLocal() noexcept {
    if (State_.Paused) return;
    State_.Phase = BrokerRuntimePhase::ConnectedLocal;
    State_.Failure = BrokerRuntimeFailure::None;
    State_.RetryAttempt = 0;
}

void BrokerReconnectController::ProcessStopped(
    BrokerRuntimeFailure Failure, IClock::time_point Now) noexcept {
    if (State_.Paused) {
        State_.Phase = BrokerRuntimePhase::Paused;
        State_.Failure = BrokerRuntimeFailure::None;
        State_.RetryAttempt = 0;
        State_.RetryAt = Now;
        return;
    }
    State_.Failure = Failure;
    if (!IsRetryableBrokerRuntimeFailure(Failure)) {
        State_.Phase = BrokerRuntimePhase::ActionRequired;
        State_.RetryAttempt = 0;
        State_.RetryAt = {};
        return;
    }
    State_.RetryAttempt = std::min<std::uint16_t>(
        static_cast<std::uint16_t>(State_.RetryAttempt + 1u),
        kMaximumRecordedRetryAttempt);
    State_.RetryAt = Now + BrokerReconnectDelay(
        State_.RetryAttempt, JitterSeed_);
    State_.Phase = BrokerRuntimePhase::RetryWaiting;
}

void BrokerReconnectController::NetworkChanged(
    IClock::time_point Now) noexcept {
    if (!State_.Paused &&
        State_.Phase == BrokerRuntimePhase::RetryWaiting &&
        IsRetryableBrokerRuntimeFailure(State_.Failure)) {
        State_.RetryAt = Now;
    }
}

bool BrokerReconnectController::AttemptDue(
    IClock::time_point Now) const noexcept {
    return !State_.Paused &&
        (State_.Phase == BrokerRuntimePhase::Stopped ||
         (State_.Phase == BrokerRuntimePhase::RetryWaiting &&
          Now >= State_.RetryAt));
}

BrokerRuntimeSnapshot BrokerReconnectController::Snapshot() const noexcept {
    return State_;
}

RuntimeTrustAuthority::RuntimeTrustAuthority(
    ITrustStore& TrustStore,
    IRuntimeSafetyController& SafetyController,
    std::function<bool()> Reload) noexcept
    : TrustStore_(TrustStore),
      SafetyController_(SafetyController),
      Reload_(std::move(Reload)) {}

std::optional<std::vector<TrustedPeer>>
RuntimeTrustAuthority::ListTrustedPeers() const {
    std::scoped_lock Lock(Mutex_);
    auto Peers = TrustStore_.ListPeers();
    if (!Peers || Peers->size() > kMaxTrustedPeers) return std::nullopt;
    std::sort(Peers->begin(), Peers->end(),
              [](const TrustedPeer& Left, const TrustedPeer& Right) {
        if (Left.Identity.display_name != Right.Identity.display_name) {
            return Left.Identity.display_name < Right.Identity.display_name;
        }
        return MachineLess(Left.Identity.machine_id,
                           Right.Identity.machine_id);
    });
    return Peers;
}

TrustMutationStatus RuntimeTrustAuthority::RequestPermissionChange(
    const MachineId& Machine, CapabilitySet DesiredCapabilities) {
    if (!IsNonzeroMachine(Machine) ||
        !HasOnlyKnownCapabilities(DesiredCapabilities)) {
        return TrustMutationStatus::InvalidRequest;
    }

    std::scoped_lock Lock(Mutex_);
    const auto Existing = TrustStore_.GetPeer(Machine);
    if (!Existing) return TrustMutationStatus::PeerNotFound;
    if (DesiredCapabilities.bits() == Existing->Capabilities.bits()) {
        return TrustMutationStatus::NoChange;
    }
    if (!IsSubset(DesiredCapabilities, Existing->Capabilities)) {
        // Generic same-user IPC may request more authority but cannot approve
        // or persist it. A fresh, broker-owned local reauthorization flow is
        // required to cross this boundary.
        return TrustMutationStatus::ReauthorizationRequired;
    }
    if (!SafetyController_.ReturnLocalAndStopPeer(Machine)) {
        return TrustMutationStatus::CleanupFailed;
    }
    auto Updated = *Existing;
    Updated.Capabilities = DesiredCapabilities;
    return TrustStore_.SavePeer(std::move(Updated))
        ? TrustMutationStatus::Applied
        : TrustMutationStatus::StoreFailed;
}

TrustMutationStatus RuntimeTrustAuthority::ForgetPeer(
    const MachineId& Machine) {
    if (!IsNonzeroMachine(Machine)) {
        return TrustMutationStatus::InvalidRequest;
    }
    std::scoped_lock Lock(Mutex_);
    if (!TrustStore_.GetPeer(Machine)) {
        return TrustMutationStatus::PeerNotFound;
    }
    if (!SafetyController_.ReturnLocalAndStopPeer(Machine)) {
        return TrustMutationStatus::CleanupFailed;
    }
    return TrustStore_.RemovePeer(Machine)
        ? TrustMutationStatus::Applied
        : TrustMutationStatus::StoreFailed;
}

bool RuntimeTrustAuthority::ReloadAfterExternalPairing() {
    std::scoped_lock Lock(Mutex_);
    return Reload_ && Reload_();
}

bool BrokerPairingCandidateLease::Present(
    BrokerPairingCandidate Candidate, IClock::time_point Now) {
    if (Candidate.RequestId == 0 || Candidate.OwnerClient == 0 ||
        !IsValidCandidate(Candidate.Candidate) ||
        Candidate.ExpiresAt <= Now ||
        !HasOnlyKnownCapabilities(Candidate.RequestedCapabilities)) {
        return false;
    }
    std::scoped_lock Lock(Mutex_);
    ExpireLocked(Now);
    if (Candidate_) return false;
    Candidate_ = std::move(Candidate);
    return true;
}

std::optional<BrokerPairingCandidate> BrokerPairingCandidateLease::Current(
    IClock::time_point Now) {
    std::scoped_lock Lock(Mutex_);
    ExpireLocked(Now);
    return Candidate_;
}

std::optional<BrokerPairingCandidate>
BrokerPairingCandidateLease::ResolveLocally(
    BrokerPairingRequestId RequestId, bool Approved,
    IClock::time_point Now) {
    std::scoped_lock Lock(Mutex_);
    ExpireLocked(Now);
    if (!Candidate_ || Candidate_->RequestId != RequestId) {
        return std::nullopt;
    }
    auto Result = std::move(Candidate_);
    Candidate_.reset();
    return Approved ? Result : std::nullopt;
}

void BrokerPairingCandidateLease::Reject(
    BrokerPairingRequestId RequestId) noexcept {
    std::scoped_lock Lock(Mutex_);
    if (Candidate_ && Candidate_->RequestId == RequestId) Candidate_.reset();
}

void BrokerPairingCandidateLease::ClientDisconnected(
    BrokerClientId Client) noexcept {
    std::scoped_lock Lock(Mutex_);
    if (Candidate_ && Candidate_->OwnerClient == Client) Candidate_.reset();
}

void BrokerPairingCandidateLease::ExpireLocked(
    IClock::time_point Now) noexcept {
    if (Candidate_ && Candidate_->ExpiresAt <= Now) Candidate_.reset();
}

} // namespace desklink
