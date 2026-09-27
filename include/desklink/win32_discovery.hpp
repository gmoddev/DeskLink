#pragma once

#include "desklink/discovery.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <vector>

namespace desklink {

struct Win32DiscoveryBrowseResult {
    std::vector<DiscoveredPeer> Peers;
    std::uint32_t StartStatus{};
    std::size_t BrowseFailures{};
    std::size_t ResolveFailures{};
    std::size_t MalformedRecords{};
    std::uint32_t ResolveStatus{};
    std::vector<std::string> ServiceNames;
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
    [[nodiscard]] bool StartInterface(const DiscoveryAdvertisement& Advertisement,
                                      std::uint32_t InterfaceIndex);
    struct Impl;
    mutable std::recursive_mutex Mutex_;
    std::shared_ptr<Impl> Impl_;
    std::vector<std::unique_ptr<Win32MdnsAdvertiser>> Interfaces_;
};

class Win32MdnsBrowser final {
public:
    [[nodiscard]] static Win32DiscoveryBrowseResult Browse(
        std::chrono::milliseconds Duration,
        std::stop_token StopToken = {});
};

} // namespace desklink
