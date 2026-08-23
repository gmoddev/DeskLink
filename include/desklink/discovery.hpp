#pragma once

#include "desklink/types.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace desklink {

inline constexpr std::string_view kDeskLinkDiscoveryServiceType =
    "_desklink._udp.local";
inline constexpr std::size_t kMaximumDiscoveryPropertyCount = 16;
inline constexpr std::size_t kMaximumDiscoveryPropertyBytes = 1'024;
inline constexpr std::size_t kMaximumDiscoveryDisplayNameBytes = 63;

using DiscoveryProperties =
    std::vector<std::pair<std::string, std::string>>;

struct DiscoveryAdvertisement {
    MachineId Machine{};
    std::string DisplayName;
    std::uint16_t ProtocolVersion{kProtocolVersion};
    std::uint16_t Port{};
    std::uint64_t CapabilityHints{};
    bool PairingAvailable{};
};

struct DiscoveryEndpoint {
    DiscoveryAdvertisement Advertisement;
    std::string InstanceName;
    std::string HostName;
    std::uint32_t InterfaceIndex{};
};

struct DiscoveredPeer {
    DiscoveryEndpoint Endpoint;
    std::size_t EndpointCount{};
    bool Ambiguous{};
};

[[nodiscard]] bool IsValidDiscoveryAdvertisement(
    const DiscoveryAdvertisement& Advertisement) noexcept;
[[nodiscard]] bool IsValidDiscoveryEndpoint(
    const DiscoveryEndpoint& Endpoint) noexcept;
[[nodiscard]] std::string FormatDiscoveryMachineId(
    const MachineId& Machine);
[[nodiscard]] std::optional<MachineId> ParseDiscoveryMachineId(
    std::string_view Text) noexcept;
[[nodiscard]] std::optional<DiscoveryProperties> EncodeDiscoveryProperties(
    const DiscoveryAdvertisement& Advertisement);
[[nodiscard]] std::optional<DiscoveryEndpoint> DecodeDiscoveryProperties(
    const DiscoveryProperties& Properties,
    std::string InstanceName,
    std::string HostName,
    std::uint16_t Port,
    std::uint32_t InterfaceIndex);

class DiscoveryCache final {
public:
    explicit DiscoveryCache(const IClock& Clock) noexcept;
    ~DiscoveryCache();

    DiscoveryCache(const DiscoveryCache&) = delete;
    DiscoveryCache& operator=(const DiscoveryCache&) = delete;

    [[nodiscard]] bool Observe(
        DiscoveryEndpoint Endpoint,
        std::chrono::milliseconds TimeToLive);
    void Remove(std::string_view InstanceName,
                std::uint32_t InterfaceIndex) noexcept;
    void PurgeExpired() noexcept;
    [[nodiscard]] std::vector<DiscoveredPeer> Snapshot();

private:
    struct Entry;

    const IClock& Clock_;
    std::vector<Entry> Entries_;
};

} // namespace desklink
