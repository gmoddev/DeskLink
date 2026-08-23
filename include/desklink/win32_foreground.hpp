#pragma once

#ifdef _WIN32

#include "desklink/profile.hpp"

#include <functional>
#include <memory>
#include <string>

namespace desklink {

using Win32ForegroundHandler =
    std::function<void(ForegroundWindowSnapshot)>;

struct Win32ForegroundHandlers {
    Win32ForegroundHandler Changed;
    std::function<void(std::string)> Failed;
};

[[nodiscard]] ForegroundWindowSnapshot ReadWin32ForegroundWindow(
    std::uint64_t WindowId = 0) noexcept;

class Win32ForegroundMonitor final {
public:
    struct State;

    explicit Win32ForegroundMonitor(Win32ForegroundHandlers Handlers);
    ~Win32ForegroundMonitor();

    Win32ForegroundMonitor(const Win32ForegroundMonitor&) = delete;
    Win32ForegroundMonitor& operator=(const Win32ForegroundMonitor&) = delete;

    [[nodiscard]] bool Start();
    void Stop() noexcept;

private:
    std::unique_ptr<State> State_;
};

} // namespace desklink

#endif
