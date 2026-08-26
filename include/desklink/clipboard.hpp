#pragma once

#include "desklink/capabilities.hpp"
#include "desklink/types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace desklink {

inline constexpr std::uint16_t kClipboardProtocolVersion = 1;
inline constexpr std::uint32_t kMaximumClipboardTextBytes = 48 * 1024;
inline constexpr std::chrono::milliseconds kClipboardMinimumUpdateInterval{50};
inline constexpr std::size_t kMaximumPendingClipboardUpdates = 8;

struct ClipboardHelloMessage {
    std::uint16_t Version{kClipboardProtocolVersion};
    std::uint32_t MaximumTextBytes{kMaximumClipboardTextBytes};

    [[nodiscard]] bool operator==(
        const ClipboardHelloMessage&) const noexcept = default;
};

struct ClipboardTextMessage {
    MachineId OriginMachine{};
    std::uint64_t UpdateId{};
    std::string Text;

    [[nodiscard]] bool operator==(
        const ClipboardTextMessage&) const noexcept = default;
};

[[nodiscard]] bool IsValidClipboardHelloMessage(
    const ClipboardHelloMessage& Message) noexcept;
[[nodiscard]] bool IsValidClipboardTextMessage(
    const ClipboardTextMessage& Message) noexcept;

enum class ClipboardAdmission : std::uint8_t {
    Accepted,
    Disabled,
    CapabilityMissing,
    NotNegotiated,
    Invalid,
    WrongSession,
    WrongPeer,
    StaleUpdate,
    RateLimited,
};

// Owns the session-scoped clipboard negotiation and replay/rate gates. It does
// not touch an operating-system clipboard and never changes focus/input state.
class ClipboardExchange final {
public:
    explicit ClipboardExchange(const IClock* Clock = nullptr) noexcept;

    void Begin(const MachineId& LocalMachine,
               const MachineId& PeerMachine,
               std::uint64_t SessionNonce,
               bool Enabled,
               CapabilitySet LocalCapabilities) noexcept;
    void Stop() noexcept;
    void SetRemoteCapabilities(
        std::optional<CapabilitySet> Capabilities) noexcept;

    [[nodiscard]] bool ShouldSendHello() const noexcept;
    [[nodiscard]] bool MarkHelloSent() noexcept;
    [[nodiscard]] ClipboardAdmission AdmitHello(
        const ClipboardHelloMessage& Message) noexcept;
    [[nodiscard]] bool CanSend() const noexcept;
    [[nodiscard]] bool CanReceive() const noexcept;
    [[nodiscard]] std::optional<ClipboardTextMessage> BuildText(
        std::string Text) noexcept;
    [[nodiscard]] ClipboardAdmission AdmitText(
        std::uint64_t EnvelopeSessionNonce,
        const ClipboardTextMessage& Message) noexcept;

private:
    [[nodiscard]] bool HasAnyDirection() const noexcept;

    const IClock* Clock_{};
    MachineId LocalMachine_{};
    MachineId PeerMachine_{};
    std::uint64_t SessionNonce_{};
    CapabilitySet LocalCapabilities_;
    std::optional<CapabilitySet> RemoteCapabilities_;
    std::optional<IClock::time_point> LastSentAt_;
    std::optional<IClock::time_point> LastReceivedAt_;
    std::uint64_t NextUpdateId_{1};
    std::uint64_t LastReceivedUpdateId_{};
    bool Enabled_{};
    bool HelloSent_{};
    bool PeerHelloReceived_{};
};

} // namespace desklink
