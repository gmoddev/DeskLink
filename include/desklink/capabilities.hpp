#pragma once

#include <cstdint>

namespace desklink {

enum class Capability : std::uint64_t {
    None           = 0,
    CoreStateRead  = 1ull << 0,
    InputSend      = 1ull << 1,
    InputInject    = 1ull << 2,
    AudioSend      = 1ull << 3,
    AudioReceive   = 1ull << 4,
    ClipboardRead  = 1ull << 5,
    ClipboardWrite = 1ull << 6,
    SystemSleep    = 1ull << 7,
    SystemWake     = 1ull << 8,
    SystemLaunch   = 1ull << 9,
    FileSend       = 1ull << 10,
    FileReceive    = 1ull << 11,
    DisplayTopologyExchange = 1ull << 12,
};

inline constexpr std::uint64_t kKnownCapabilityBits = (1ull << 13u) - 1u;

class CapabilitySet {
public:
    constexpr CapabilitySet() noexcept = default;
    explicit constexpr CapabilitySet(std::uint64_t bits) noexcept : bits_(bits) {}

    constexpr void grant(Capability capability) noexcept {
        bits_ |= static_cast<std::uint64_t>(capability);
    }

    constexpr void revoke(Capability capability) noexcept {
        bits_ &= ~static_cast<std::uint64_t>(capability);
    }

    [[nodiscard]] constexpr bool contains(Capability capability) const noexcept {
        const auto bit = static_cast<std::uint64_t>(capability);
        return (bits_ & bit) == bit;
    }

    [[nodiscard]] constexpr std::uint64_t bits() const noexcept { return bits_; }

private:
    std::uint64_t bits_{};
};

} // namespace desklink
