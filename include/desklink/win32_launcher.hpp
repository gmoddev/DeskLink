#pragma once

#include "desklink/win32_capture.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace desklink {

enum class LauncherOperation {
    Identity,
    Discover,
    PairListen,
    PairConnect,
    Serve,
    Focus,
};

struct LauncherRequest {
    LauncherOperation Operation{LauncherOperation::Identity};
    std::wstring Host;
    std::uint16_t Port{43'821};
    std::uint8_t DiscoverySeconds{5};
    bool GrantInput{};
    bool GrantAudioSend{};
    bool GrantAudioReceive{};
    bool GrantTopology{};
    bool CaptureInput{};
    bool SendAudio{};
    bool ReceiveAudio{};
    std::filesystem::path EdgeRoamingSettingsPath;
    Win32PointerCalibration PointerCalibration;
};

// Produces arguments for desklink_pair.exe. Network operations are deliberately
// pinned to Schannel because the alpha wrapper supports the production Windows
// 11 / Server 2022+ path only.
[[nodiscard]] std::optional<std::vector<std::wstring>>
BuildLauncherArguments(const LauncherRequest& Request);

// Implements the quoting rules used by CommandLineToArgvW and the Microsoft C
// runtime. CreateProcessW still receives the executable through lpApplicationName.
[[nodiscard]] std::wstring QuoteWindowsCommandArgument(
    std::wstring_view Argument);

[[nodiscard]] std::optional<std::wstring> BuildWindowsCommandLine(
    std::wstring_view Application,
    const std::vector<std::wstring>& Arguments);

} // namespace desklink
