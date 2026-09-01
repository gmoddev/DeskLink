#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace desklink {

using ByteBuffer = std::vector<std::uint8_t>;
using ByteSpan = std::span<const std::uint8_t>;
using MachineId = std::array<std::uint8_t, 16>;

inline constexpr std::size_t kEnvelopeSize = 36;
inline constexpr std::size_t kMaxReliablePayload = 64 * 1024;
inline constexpr std::size_t kMaxEncodedDatagramSize = 1200;
inline constexpr std::size_t kMaxDatagramPayload =
    kMaxEncodedDatagramSize - kEnvelopeSize;
inline constexpr std::uint32_t kWireMagic = 0x444C4E4Bu; // "DLNK"
inline constexpr std::uint16_t kProtocolVersion = 3;

struct PeerIdentity {
    MachineId machine_id{};
    std::string display_name;
    std::string public_key_fingerprint;

    [[nodiscard]] bool operator==(const PeerIdentity& other) const noexcept {
        return machine_id == other.machine_id &&
               public_key_fingerprint == other.public_key_fingerprint;
    }
};

class IClock {
public:
    using time_point = std::chrono::steady_clock::time_point;
    virtual ~IClock() = default;
    [[nodiscard]] virtual time_point now() const noexcept = 0;
};

class SteadyClock final : public IClock {
public:
    [[nodiscard]] time_point now() const noexcept override {
        return std::chrono::steady_clock::now();
    }
};

} // namespace desklink
