#pragma once

#ifdef _WIN32

#include "desklink/voice.hpp"
#include "desklink/win32_audio.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace desklink {

struct VoiceInputDevice {
    std::string EndpointId;
    std::string DisplayName;
    bool IsDefaultCommunications{};

    [[nodiscard]] bool operator==(
        const VoiceInputDevice&) const noexcept = default;
};

enum class DeskLinkVirtualAudioEndpointKind : std::uint32_t {
    None = 0,
    MicrophoneFeed = 1,
    RemoteMicrophone = 2,
};

enum class Win32VirtualMicrophoneComponentState : std::uint8_t {
    NotInstalled = 0,
    Ready = 1,
    NeedsRepair = 2,
};

[[nodiscard]] constexpr bool IsDeskLinkVirtualMicrophoneSource(
    DeskLinkVirtualAudioEndpointKind Kind) noexcept {
    return Kind == DeskLinkVirtualAudioEndpointKind::RemoteMicrophone;
}

[[nodiscard]] Win32VirtualMicrophoneComponentState
GetWin32VirtualMicrophoneComponentState();

[[nodiscard]] std::vector<VoiceInputDevice>
EnumerateWin32VoiceInputDevices();

struct Win32WasapiMicrophoneHandlers {
    std::function<bool(VoicePcmFrame)> Frame;
    std::function<void(Win32WasapiFailureKind, std::string)> Failed;
};

class Win32WasapiMicrophoneCapture final {
public:
    struct State;

    // nullopt selects and follows eCapture/eCommunications. A value selects
    // only that exact endpoint and never falls back to another microphone.
    explicit Win32WasapiMicrophoneCapture(
        std::optional<std::string> EndpointId,
        Win32WasapiMicrophoneHandlers Handlers);
    ~Win32WasapiMicrophoneCapture();

    Win32WasapiMicrophoneCapture(
        const Win32WasapiMicrophoneCapture&) = delete;
    Win32WasapiMicrophoneCapture& operator=(
        const Win32WasapiMicrophoneCapture&) = delete;

    [[nodiscard]] bool Start();
    void Stop() noexcept;
    [[nodiscard]] bool Running() const noexcept;

private:
    std::unique_ptr<State> State_;
};

struct Win32WasapiVoiceRenderHandlers {
    std::function<void(Win32WasapiFailureKind, std::string)> Failed;
};

class Win32WasapiVoiceRenderer final {
public:
    struct State;

    explicit Win32WasapiVoiceRenderer(
        Win32WasapiVoiceRenderHandlers Handlers = {});
    ~Win32WasapiVoiceRenderer();

    Win32WasapiVoiceRenderer(const Win32WasapiVoiceRenderer&) = delete;
    Win32WasapiVoiceRenderer& operator=(
        const Win32WasapiVoiceRenderer&) = delete;

    [[nodiscard]] bool Start();
    [[nodiscard]] bool Submit(VoicePcmFrame Frame);
    void Reset() noexcept;
    void Stop() noexcept;
    [[nodiscard]] bool Running() const noexcept;
    [[nodiscard]] std::size_t QueuedFrames() const noexcept;
    [[nodiscard]] std::uint64_t Underruns() const noexcept;

private:
    std::unique_ptr<State> State_;
};

// Writes already-authorized, canonical 48 kHz mono PCM16 to the private
// DeskLink driver feed endpoint. Discovery is property-based and never falls
// back to an arbitrary render endpoint.
class Win32VirtualMicrophoneFeed final {
public:
    struct State;

    explicit Win32VirtualMicrophoneFeed(
        Win32WasapiVoiceRenderHandlers Handlers = {});
    ~Win32VirtualMicrophoneFeed();

    Win32VirtualMicrophoneFeed(const Win32VirtualMicrophoneFeed&) = delete;
    Win32VirtualMicrophoneFeed& operator=(
        const Win32VirtualMicrophoneFeed&) = delete;

    [[nodiscard]] bool Start();
    [[nodiscard]] bool Submit(VoicePcmFrame Frame);
    void Reset() noexcept;
    void Stop() noexcept;
    [[nodiscard]] bool Running() const noexcept;
    [[nodiscard]] std::size_t QueuedFrames() const noexcept;
    [[nodiscard]] std::uint64_t StaleFramesDropped() const noexcept;
    [[nodiscard]] std::uint64_t Underruns() const noexcept;

private:
    std::unique_ptr<State> State_;
};

} // namespace desklink

#endif
