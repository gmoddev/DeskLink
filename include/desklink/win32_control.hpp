#pragma once

#include "desklink/control.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace desklink {

[[nodiscard]] std::optional<std::wstring> GetWin32ControlPipeName(
    std::wstring_view Instance = {});

class Win32ControlPipeServer final {
public:
    using Handler = std::function<ControlResponse(const ControlRequest&)>;

    explicit Win32ControlPipeServer(Handler RequestHandler,
                                    std::wstring Instance = {});
    ~Win32ControlPipeServer();

    Win32ControlPipeServer(const Win32ControlPipeServer&) = delete;
    Win32ControlPipeServer& operator=(const Win32ControlPipeServer&) = delete;

    [[nodiscard]] bool Start();
    void Stop() noexcept;
    [[nodiscard]] bool Running() const noexcept;
    [[nodiscard]] std::wstring PipeName() const;

private:
    struct State;
    std::unique_ptr<State> State_;
};

class Win32ControlPipeClient final {
public:
    [[nodiscard]] static std::optional<ControlResponse> Send(
        const ControlRequest& Request,
        std::wstring_view Instance = {},
        std::chrono::milliseconds Timeout = std::chrono::milliseconds{2'000});
};

} // namespace desklink
