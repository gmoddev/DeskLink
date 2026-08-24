#pragma once

#ifdef _WIN32

#include "desklink/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace desklink {

struct Win32WasapiCaptureHandlers {
    std::function<bool(AudioFrameMessage)> Frame;
    std::function<void(std::string)> Failed;
};

class Win32WasapiLoopbackCapture final {
public:
    struct State;

    explicit Win32WasapiLoopbackCapture(
        std::uint32_t StreamId, Win32WasapiCaptureHandlers Handlers);
    ~Win32WasapiLoopbackCapture();

    Win32WasapiLoopbackCapture(const Win32WasapiLoopbackCapture&) = delete;
    Win32WasapiLoopbackCapture& operator=(
        const Win32WasapiLoopbackCapture&) = delete;

    [[nodiscard]] bool Start();
    void Stop() noexcept;
    [[nodiscard]] bool Running() const noexcept;

private:
    std::unique_ptr<State> State_;
};

struct Win32WasapiRenderHandlers {
    std::function<void(std::string)> Failed;
};

class Win32WasapiRenderer final {
public:
    struct State;

    explicit Win32WasapiRenderer(Win32WasapiRenderHandlers Handlers = {});
    ~Win32WasapiRenderer();

    Win32WasapiRenderer(const Win32WasapiRenderer&) = delete;
    Win32WasapiRenderer& operator=(const Win32WasapiRenderer&) = delete;

    [[nodiscard]] bool Start();
    [[nodiscard]] bool Submit(AudioFrameMessage Frame);
    void Stop() noexcept;
    [[nodiscard]] bool Running() const noexcept;
    [[nodiscard]] std::size_t QueuedFrames() const noexcept;
    [[nodiscard]] std::uint64_t Underruns() const noexcept;

private:
    std::unique_ptr<State> State_;
};

} // namespace desklink

#endif
