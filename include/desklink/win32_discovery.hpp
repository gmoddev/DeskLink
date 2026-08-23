#pragma once

#include "desklink/discovery.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace desklink {

struct Win32DiscoveryBrowseResult {
    std::vector<DiscoveredPeer> Peers;
    std::uint32_t StartStatus{};
    std::size_t BrowseFailures{};
    std::size_t ResolveFailures{};
    std::size_t MalformedRecords{};
};

class Win32MdnsAdvertiser final {
public:
    Win32MdnsAdvertiser();
    ~Win32MdnsAdvertiser();

    Win32MdnsAdvertiser(const Win32MdnsAdvertiser&) = delete;
    Win32MdnsAdvertiser& operator=(const Win32MdnsAdvertiser&) = delete;

    [[nodiscard]] bool Start(const DiscoveryAdvertisement& Advertisement);
    void Stop() noexcept;
    [[nodiscard]] bool Running() const noexcept;
    [[nodiscard]] std::uint32_t LastStatus() const noexcept;

private:
    struct Impl;
    std::shared_ptr<Impl> Impl_;
};

class Win32MdnsBrowser final {
public:
    [[nodiscard]] static Win32DiscoveryBrowseResult Browse(
        std::chrono::milliseconds Duration);
};

} // namespace desklink
