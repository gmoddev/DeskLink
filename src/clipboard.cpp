#include "desklink/clipboard.hpp"

#include <algorithm>
#include <utility>

namespace desklink {
namespace {

[[nodiscard]] bool NonzeroMachine(const MachineId& Machine) noexcept {
    return std::any_of(Machine.begin(), Machine.end(),
                       [](std::uint8_t Byte) { return Byte != 0; });
}

[[nodiscard]] bool IsContinuation(std::uint8_t Byte) noexcept {
    return (Byte & 0xC0u) == 0x80u;
}

[[nodiscard]] bool IsValidUtf8(std::string_view Text) noexcept {
    const auto* Bytes = reinterpret_cast<const std::uint8_t*>(Text.data());
    std::size_t Index{};
    while (Index < Text.size()) {
        const auto First = Bytes[Index];
        if (First == 0) return false;
        if (First <= 0x7Fu) {
            ++Index;
            continue;
        }
        if (First >= 0xC2u && First <= 0xDFu) {
            if (Index + 1 >= Text.size() ||
                !IsContinuation(Bytes[Index + 1])) {
                return false;
            }
            Index += 2;
            continue;
        }
        if (First >= 0xE0u && First <= 0xEFu) {
            if (Index + 2 >= Text.size() ||
                !IsContinuation(Bytes[Index + 1]) ||
                !IsContinuation(Bytes[Index + 2]) ||
                (First == 0xE0u && Bytes[Index + 1] < 0xA0u) ||
                (First == 0xEDu && Bytes[Index + 1] > 0x9Fu)) {
                return false;
            }
            Index += 3;
            continue;
        }
        if (First >= 0xF0u && First <= 0xF4u) {
            if (Index + 3 >= Text.size() ||
                !IsContinuation(Bytes[Index + 1]) ||
                !IsContinuation(Bytes[Index + 2]) ||
                !IsContinuation(Bytes[Index + 3]) ||
                (First == 0xF0u && Bytes[Index + 1] < 0x90u) ||
                (First == 0xF4u && Bytes[Index + 1] > 0x8Fu)) {
                return false;
            }
            Index += 4;
            continue;
        }
        return false;
    }
    return true;
}

} // namespace

bool IsValidClipboardHelloMessage(
    const ClipboardHelloMessage& Message) noexcept {
    return Message.Version == kClipboardProtocolVersion &&
           Message.MaximumTextBytes == kMaximumClipboardTextBytes;
}

bool IsValidClipboardTextMessage(
    const ClipboardTextMessage& Message) noexcept {
    return NonzeroMachine(Message.OriginMachine) &&
           Message.UpdateId != 0 &&
           Message.Text.size() <= kMaximumClipboardTextBytes &&
           IsValidUtf8(Message.Text);
}

ClipboardExchange::ClipboardExchange(const IClock* Clock) noexcept
    : Clock_(Clock) {}

void ClipboardExchange::Begin(
    const MachineId& LocalMachine,
    const MachineId& PeerMachine,
    std::uint64_t SessionNonce,
    bool Enabled,
    CapabilitySet LocalCapabilities) noexcept {
    Stop();
    LocalMachine_ = LocalMachine;
    PeerMachine_ = PeerMachine;
    SessionNonce_ = SessionNonce;
    LocalCapabilities_ = LocalCapabilities;
    Enabled_ = Enabled && Clock_ != nullptr && SessionNonce != 0 &&
        NonzeroMachine(LocalMachine) && NonzeroMachine(PeerMachine) &&
        LocalMachine != PeerMachine;
}

void ClipboardExchange::Stop() noexcept {
    LocalMachine_ = {};
    PeerMachine_ = {};
    SessionNonce_ = 0;
    LocalCapabilities_ = {};
    RemoteCapabilities_.reset();
    LastSentAt_.reset();
    LastReceivedAt_.reset();
    NextUpdateId_ = 1;
    LastReceivedUpdateId_ = 0;
    Enabled_ = false;
    HelloSent_ = false;
    PeerHelloReceived_ = false;
}

void ClipboardExchange::SetRemoteCapabilities(
    std::optional<CapabilitySet> Capabilities) noexcept {
    RemoteCapabilities_ = Capabilities;
    if (!RemoteCapabilities_) {
        HelloSent_ = false;
        PeerHelloReceived_ = false;
    }
}

bool ClipboardExchange::HasAnyDirection() const noexcept {
    if (!Enabled_ || !RemoteCapabilities_) return false;
    return (LocalCapabilities_.contains(Capability::ClipboardRead) &&
            RemoteCapabilities_->contains(Capability::ClipboardWrite)) ||
           (LocalCapabilities_.contains(Capability::ClipboardWrite) &&
            RemoteCapabilities_->contains(Capability::ClipboardRead));
}

bool ClipboardExchange::ShouldSendHello() const noexcept {
    return HasAnyDirection() && !HelloSent_;
}

bool ClipboardExchange::MarkHelloSent() noexcept {
    if (!ShouldSendHello()) return false;
    HelloSent_ = true;
    return true;
}

ClipboardAdmission ClipboardExchange::AdmitHello(
    const ClipboardHelloMessage& Message) noexcept {
    if (!Enabled_) return ClipboardAdmission::Disabled;
    if (!HasAnyDirection()) return ClipboardAdmission::CapabilityMissing;
    if (!IsValidClipboardHelloMessage(Message)) {
        return ClipboardAdmission::Invalid;
    }
    PeerHelloReceived_ = true;
    return ClipboardAdmission::Accepted;
}

bool ClipboardExchange::CanSend() const noexcept {
    return Enabled_ && HelloSent_ && PeerHelloReceived_ &&
        RemoteCapabilities_ &&
        LocalCapabilities_.contains(Capability::ClipboardRead) &&
        RemoteCapabilities_->contains(Capability::ClipboardWrite);
}

bool ClipboardExchange::CanReceive() const noexcept {
    return Enabled_ && HelloSent_ && PeerHelloReceived_ &&
        RemoteCapabilities_ &&
        LocalCapabilities_.contains(Capability::ClipboardWrite) &&
        RemoteCapabilities_->contains(Capability::ClipboardRead);
}

std::optional<ClipboardTextMessage> ClipboardExchange::BuildText(
    std::string Text) noexcept {
    ClipboardTextMessage Message{LocalMachine_, NextUpdateId_, std::move(Text)};
    if (!CanSend() || !IsValidClipboardTextMessage(Message) ||
        NextUpdateId_ == 0 || Clock_ == nullptr) {
        return std::nullopt;
    }
    const auto Now = Clock_->now();
    if (LastSentAt_ && Now - *LastSentAt_ <
            kClipboardMinimumUpdateInterval) {
        return std::nullopt;
    }
    LastSentAt_ = Now;
    ++NextUpdateId_;
    if (NextUpdateId_ == 0) Enabled_ = false;
    return Message;
}

ClipboardAdmission ClipboardExchange::AdmitText(
    std::uint64_t EnvelopeSessionNonce,
    const ClipboardTextMessage& Message) noexcept {
    if (!Enabled_) return ClipboardAdmission::Disabled;
    if (!CanReceive()) return HasAnyDirection()
        ? ClipboardAdmission::NotNegotiated
        : ClipboardAdmission::CapabilityMissing;
    if (EnvelopeSessionNonce != SessionNonce_) {
        return ClipboardAdmission::WrongSession;
    }
    if (!IsValidClipboardTextMessage(Message)) {
        return ClipboardAdmission::Invalid;
    }
    if (Message.OriginMachine != PeerMachine_) {
        return ClipboardAdmission::WrongPeer;
    }
    if (Message.UpdateId <= LastReceivedUpdateId_) {
        return ClipboardAdmission::StaleUpdate;
    }
    LastReceivedUpdateId_ = Message.UpdateId;
    if (Clock_ == nullptr) return ClipboardAdmission::Disabled;
    const auto Now = Clock_->now();
    if (LastReceivedAt_ && Now - *LastReceivedAt_ <
            kClipboardMinimumUpdateInterval) {
        return ClipboardAdmission::RateLimited;
    }
    LastReceivedAt_ = Now;
    return ClipboardAdmission::Accepted;
}

} // namespace desklink
