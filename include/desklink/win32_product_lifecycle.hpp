#pragma once

#ifdef _WIN32

#include <chrono>
#include <filesystem>
#include <optional>

namespace desklink {

enum class Win32BrokerLaunchStatus {
    Ready,
    Started,
    LifecycleBlocked,
    BrokerUnavailable,
};

[[nodiscard]] bool IsWin32DeskLinkLifecycleOperationActive() noexcept;
[[nodiscard]] bool IsWin32DeskLinkUpdateValidationActive() noexcept;
[[nodiscard]] bool IsSafeWin32ProductFile(
    const std::filesystem::path& Path) noexcept;
[[nodiscard]] std::optional<std::filesystem::path>
GetWin32ProductShellExecutable(
    const std::filesystem::path& SiblingExecutable);
[[nodiscard]] Win32BrokerLaunchStatus EnsureWin32RuntimeBroker(
    const std::filesystem::path& FrontendExecutable,
    std::chrono::milliseconds Timeout = std::chrono::milliseconds{2'000});

} // namespace desklink

#endif
