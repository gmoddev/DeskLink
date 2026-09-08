#include "desklink/secure_input.hpp"

#include <algorithm>

namespace desklink {
namespace {

template <std::size_t Size>
bool HasNonzeroByte(const std::array<std::uint8_t, Size>& Value) noexcept {
    return std::any_of(Value.begin(), Value.end(),
        [](std::uint8_t Byte) { return Byte != 0; });
}

} // namespace

SecureInputAuthorizationGate::SecureInputAuthorizationGate(
    const IClock& Clock) noexcept : Clock_(Clock) {}

bool IsValidSecureInputGrant(const SecureInputGrant& Grant) noexcept {
    return HasNonzeroByte(Grant.PeerMachine) &&
        HasNonzeroByte(Grant.PeerCertificateDerHash) &&
        Grant.SessionNonce != 0 && Grant.Epoch != 0 &&
        Grant.GrantRevision != 0 && Grant.AllowSecureDesktopInput;
}

bool IsValidSecureInputOperation(SecureInputOperation Operation) noexcept {
    switch (Operation) {
        case SecureInputOperation::ReleaseOwnedState:
        case SecureInputOperation::Key:
        case SecureInputOperation::MouseButton:
        case SecureInputOperation::PointerMotion:
        case SecureInputOperation::Wheel:
        case SecureInputOperation::ReconcileState:
            return true;
    }
    return false;
}

bool SecureInputAuthorizationGate::Authorize(
    const SecureInputGrant& Grant,
    std::chrono::milliseconds Lease) noexcept {
    constexpr auto MinimumLease = std::chrono::milliseconds(100);
    constexpr auto MaximumLease = std::chrono::milliseconds(2'000);
    if (!IsValidSecureInputGrant(Grant) || Lease < MinimumLease ||
        Lease > MaximumLease || Grant.GrantRevision <= HighestGrantRevision_) {
        Revoke();
        return false;
    }

    Grant_ = Grant;
    ExpiresAt_ = Clock_.now() + Lease;
    LastAcceptedSequence_ = 0;
    HighestGrantRevision_ = Grant.GrantRevision;
    Active_ = true;
    return true;
}

SecureInputDecision SecureInputAuthorizationGate::Admit(
    const SecureInputEnvelope& Envelope) noexcept {
    if (!Active_) return SecureInputDecision::RejectedNoGrant;
    if (Expired()) {
        Revoke();
        return SecureInputDecision::RejectedExpired;
    }
    if (!Grant_.AllowSecureDesktopInput ||
        Envelope.GrantRevision != Grant_.GrantRevision) {
        return SecureInputDecision::RejectedGrant;
    }
    if (Envelope.PeerMachine != Grant_.PeerMachine ||
        Envelope.PeerCertificateDerHash != Grant_.PeerCertificateDerHash) {
        return SecureInputDecision::RejectedIdentity;
    }
    if (Envelope.SessionNonce != Grant_.SessionNonce) {
        return SecureInputDecision::RejectedSession;
    }
    if (Envelope.Epoch != Grant_.Epoch) {
        return SecureInputDecision::RejectedEpoch;
    }
    if (Envelope.Sequence == 0 ||
        Envelope.Sequence <= LastAcceptedSequence_) {
        return SecureInputDecision::RejectedSequence;
    }
    if (!IsValidSecureInputOperation(Envelope.Operation)) {
        return SecureInputDecision::RejectedOperation;
    }
    LastAcceptedSequence_ = Envelope.Sequence;
    return SecureInputDecision::Accepted;
}

void SecureInputAuthorizationGate::Revoke() noexcept {
    Grant_ = {};
    ExpiresAt_ = {};
    LastAcceptedSequence_ = 0;
    Active_ = false;
}

bool SecureInputAuthorizationGate::Expired() const noexcept {
    return Active_ && Clock_.now() >= ExpiresAt_;
}

bool SecureInputAuthorizationGate::Authorized() const noexcept {
    return Active_ && !Expired();
}

} // namespace desklink
