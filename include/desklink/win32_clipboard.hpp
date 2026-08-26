#pragma once

#ifdef _WIN32

#include "desklink/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace desklink {

struct Win32ClipboardHandlers {
    std::function<bool(std::string)> LocalText;
    std::function<void(std::string)> Failed;
};

struct Win32ClipboardStats {
    std::uint64_t LocalObserved{};
    std::uint64_t LocalPublished{};
    std::uint64_t LocalRejected{};
    std::uint64_t RemoteQueued{};
    std::uint64_t RemoteApplied{};
    std::uint64_t RemoteRejected{};
    std::uint64_t LoopsSuppressed{};
};

// A text-only, current-session adapter. It registers one message-only window,
// never persists clipboard content, and bounds remote work independently from
// input/focus lifecycles.
class Win32ClipboardSynchronizer final {
public:
    struct State;

    explicit Win32ClipboardSynchronizer(Win32ClipboardHandlers Handlers);
    ~Win32ClipboardSynchronizer();

    Win32ClipboardSynchronizer(const Win32ClipboardSynchronizer&) = delete;
    Win32ClipboardSynchronizer& operator=(
        const Win32ClipboardSynchronizer&) = delete;

    [[nodiscard]] bool Start();
    void Stop() noexcept;
    void SetLocalPublishing(bool Enabled) noexcept;
    [[nodiscard]] bool ApplyRemote(ClipboardTextMessage Message) noexcept;
    [[nodiscard]] bool Running() const noexcept;
    [[nodiscard]] Win32ClipboardStats Stats() const noexcept;

private:
    std::unique_ptr<State> State_;
};

} // namespace desklink

#endif
