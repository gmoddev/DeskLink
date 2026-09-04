#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "desklink/win32_voice.hpp"

#include <windows.h>
#include <audioclient.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <propsys.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr PROPERTYKEY kDeskLinkEndpointKindProperty{
    {0xd21f0a7c, 0x80da, 0x4e7e,
     {0xa9, 0x06, 0x81, 0xdf, 0x3e, 0x2e, 0xa4, 0xb9}},
    2};
constexpr std::uint32_t kRemoteMicrophoneKind = 2;
constexpr double kToneFrequency = 997.0;
constexpr double kPi = 3.14159265358979323846;

template <typename Interface>
class ComPtr final {
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    [[nodiscard]] Interface* Get() const noexcept { return Value_; }
    [[nodiscard]] Interface** Put() noexcept {
        Reset();
        return &Value_;
    }
    [[nodiscard]] Interface* operator->() const noexcept { return Value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return Value_ != nullptr;
    }
    void Reset() noexcept {
        if (Value_) Value_->Release();
        Value_ = nullptr;
    }
private:
    Interface* Value_{};
};

std::optional<std::uint32_t> EndpointKind(IMMDevice* Device) noexcept {
    if (!Device) return std::nullopt;
    ComPtr<IPropertyStore> Properties;
    if (FAILED(Device->OpenPropertyStore(STGM_READ, Properties.Put()))) {
        return std::nullopt;
    }
    PROPVARIANT Value;
    PropVariantInit(&Value);
    const auto Status = Properties->GetValue(
        kDeskLinkEndpointKindProperty, &Value);
    std::optional<std::uint32_t> Result;
    if (SUCCEEDED(Status) && Value.vt == VT_UI4) Result = Value.ulVal;
    PropVariantClear(&Value);
    return Result;
}

std::optional<std::wstring> FriendlyName(IMMDevice* Device) noexcept {
    if (!Device) return std::nullopt;
    ComPtr<IPropertyStore> Properties;
    if (FAILED(Device->OpenPropertyStore(STGM_READ, Properties.Put()))) {
        return std::nullopt;
    }
    PROPVARIANT Value;
    PropVariantInit(&Value);
    const auto Status = Properties->GetValue(PKEY_Device_FriendlyName, &Value);
    std::optional<std::wstring> Result;
    try {
        if (SUCCEEDED(Status) && Value.vt == VT_LPWSTR && Value.pwszVal) {
            Result = Value.pwszVal;
        }
    } catch (...) {}
    PropVariantClear(&Value);
    return Result;
}

bool FindRemoteMicrophone(IMMDeviceEnumerator* Enumerator,
                          ComPtr<IMMDevice>& Output,
                          std::uint32_t& OtherCaptureEndpoints) noexcept {
    OtherCaptureEndpoints = 0;
    ComPtr<IMMDeviceCollection> Collection;
    if (!Enumerator || FAILED(Enumerator->EnumAudioEndpoints(
            eCapture, DEVICE_STATE_ACTIVE, Collection.Put()))) {
        return false;
    }
    UINT Count{};
    if (FAILED(Collection->GetCount(&Count)) || Count > 128) return false;
    std::uint32_t Matches{};
    for (UINT Index = 0; Index < Count; ++Index) {
        ComPtr<IMMDevice> Candidate;
        if (FAILED(Collection->Item(Index, Candidate.Put()))) continue;
        if (EndpointKind(Candidate.Get()) == kRemoteMicrophoneKind) {
            ++Matches;
            auto* Raw = Candidate.Get();
            Raw->AddRef();
            *Output.Put() = Raw;
        } else {
            ++OtherCaptureEndpoints;
        }
    }
    return Matches == 1 && FriendlyName(Output.Get()) ==
        std::optional<std::wstring>(L"DeskLink Remote Microphone");
}

class CaptureProbe final {
public:
    ~CaptureProbe() { Stop(); }

    bool Start(IMMDevice* Device) noexcept {
        Event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!Event_ || !Device || FAILED(Device->Activate(
                __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(Client_.Put())))) {
            return false;
        }
        WAVEFORMATEX Format{};
        Format.wFormatTag = WAVE_FORMAT_PCM;
        Format.nChannels = 1;
        Format.nSamplesPerSec = desklink::kVoiceSampleRate;
        Format.wBitsPerSample = 16;
        Format.nBlockAlign = 2;
        Format.nAvgBytesPerSec = Format.nSamplesPerSec * Format.nBlockAlign;
        const auto Flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
            AUDCLNT_STREAMFLAGS_NOPERSIST;
        if (FAILED(Client_->Initialize(
                AUDCLNT_SHAREMODE_SHARED, Flags, 400'000, 0, &Format,
                nullptr)) ||
            FAILED(Client_->SetEventHandle(Event_)) ||
            FAILED(Client_->GetService(
                __uuidof(IAudioCaptureClient),
                reinterpret_cast<void**>(Capture_.Put()))) ||
            FAILED(Client_->Start())) {
            Stop();
            return false;
        }
        Running_ = true;
        return true;
    }

    void Stop() noexcept {
        if (Client_ && Running_) (void)Client_->Stop();
        Running_ = false;
        Capture_.Reset();
        Client_.Reset();
        if (Event_) CloseHandle(Event_);
        Event_ = nullptr;
    }

    std::optional<std::vector<std::int16_t>> ReadFor(
        std::chrono::milliseconds Duration) noexcept {
        if (!Running_) return std::nullopt;
        std::vector<std::int16_t> Result;
        try {
            Result.reserve(static_cast<std::size_t>(
                desklink::kVoiceSampleRate * Duration.count() / 1'000 +
                desklink::kVoiceSampleRate / 10));
        } catch (...) {
            return std::nullopt;
        }
        const auto Deadline = std::chrono::steady_clock::now() + Duration;
        while (std::chrono::steady_clock::now() < Deadline) {
            const auto Wait = WaitForSingleObject(Event_, 50);
            if (Wait == WAIT_TIMEOUT) continue;
            if (Wait != WAIT_OBJECT_0) return std::nullopt;
            UINT32 PacketFrames{};
            while (SUCCEEDED(Capture_->GetNextPacketSize(&PacketFrames)) &&
                   PacketFrames != 0) {
                BYTE* Data{};
                UINT32 Frames{};
                DWORD Flags{};
                if (FAILED(Capture_->GetBuffer(
                        &Data, &Frames, &Flags, nullptr, nullptr))) {
                    return std::nullopt;
                }
                try {
                    if ((Flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !Data) {
                        Result.insert(Result.end(), Frames, 0);
                    } else {
                        const auto* Samples =
                            reinterpret_cast<const std::int16_t*>(Data);
                        Result.insert(Result.end(), Samples, Samples + Frames);
                    }
                } catch (...) {
                    (void)Capture_->ReleaseBuffer(Frames);
                    return std::nullopt;
                }
                if (FAILED(Capture_->ReleaseBuffer(Frames))) {
                    return std::nullopt;
                }
                if (FAILED(Capture_->GetNextPacketSize(&PacketFrames))) {
                    return std::nullopt;
                }
            }
        }
        return Result;
    }

private:
    ComPtr<IAudioClient> Client_;
    ComPtr<IAudioCaptureClient> Capture_;
    HANDLE Event_{};
    bool Running_{};
};

desklink::VoicePcmFrame ToneFrame(std::uint64_t FrameIndex) noexcept {
    desklink::VoicePcmFrame Frame;
    const auto FirstSample = FrameIndex * desklink::kVoiceSamplesPerChannel;
    for (std::size_t Index = 0; Index < Frame.Samples.size(); ++Index) {
        const auto Sample = FirstSample + Index;
        Frame.Samples[Index] = static_cast<std::int16_t>(
            std::sin(2.0 * kPi * kToneFrequency *
                     static_cast<double>(Sample) /
                     static_cast<double>(desklink::kVoiceSampleRate)) *
            10'000.0);
    }
    return Frame;
}

class ToneFeeder final {
public:
    explicit ToneFeeder(desklink::Win32VirtualMicrophoneFeed& Feed)
        : Feed_(Feed), Thread_([this](std::stop_token Stop) {
              const auto Start = std::chrono::steady_clock::now();
              std::uint64_t Frame{};
              while (!Stop.stop_requested()) {
                  (void)Feed_.Submit(ToneFrame(Frame));
                  ++Frame;
                  std::this_thread::sleep_until(Start + 20ms * Frame);
              }
          }) {}
    ~ToneFeeder() {
        Thread_.request_stop();
        Thread_.join();
    }
private:
    desklink::Win32VirtualMicrophoneFeed& Feed_;
    std::jthread Thread_;
};

bool HasTone(std::span<const std::int16_t> Samples) noexcept {
    constexpr std::size_t Window = desklink::kVoiceSampleRate / 5;
    if (Samples.size() < Window) return false;
    Samples = Samples.last(Window);
    double SinProjection{};
    double CosProjection{};
    double SquareSum{};
    for (std::size_t Index = 0; Index < Samples.size(); ++Index) {
        const auto Angle = 2.0 * kPi * kToneFrequency *
            static_cast<double>(Index) /
            static_cast<double>(desklink::kVoiceSampleRate);
        const auto Value = static_cast<double>(Samples[Index]);
        SinProjection += Value * std::sin(Angle);
        CosProjection += Value * std::cos(Angle);
        SquareSum += Value * Value;
    }
    const auto Amplitude = 2.0 * std::hypot(
        SinProjection, CosProjection) / static_cast<double>(Samples.size());
    const auto Rms = std::sqrt(
        SquareSum / static_cast<double>(Samples.size()));
    const auto ToneScore = Rms > 0.0
        ? Amplitude / (Rms * std::sqrt(2.0)) : 0.0;
    return Rms >= 1'000.0 && ToneScore >= 0.65;
}

bool IsSilentAfter(std::span<const std::int16_t> Samples,
                   std::chrono::milliseconds Grace) noexcept {
    const auto Skip = static_cast<std::size_t>(
        desklink::kVoiceSampleRate * Grace.count() / 1'000);
    if (Samples.size() < Skip + desklink::kVoiceSampleRate / 50) return false;
    return std::all_of(Samples.begin() + static_cast<std::ptrdiff_t>(Skip),
                       Samples.end(), [](std::int16_t Sample) {
                           return Sample == 0;
                       });
}

std::optional<std::wstring> ExecutablePath() {
    std::wstring Value(32'768, L'\0');
    const auto Length = GetModuleFileNameW(
        nullptr, Value.data(), static_cast<DWORD>(Value.size()));
    if (!Length || Length >= Value.size()) return std::nullopt;
    Value.resize(Length);
    return Value;
}

bool ValidateCrashSilence(CaptureProbe& Capture) {
    const auto Executable = ExecutablePath();
    if (!Executable) return false;
    std::wstring Command = L"\"" + *Executable + L"\" --feed-child";
    STARTUPINFOW Startup{sizeof(STARTUPINFOW)};
    PROCESS_INFORMATION Process{};
    if (!CreateProcessW(
            Executable->c_str(), Command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &Startup, &Process)) {
        return false;
    }
    CloseHandle(Process.hThread);
    bool Result{};
    const auto Before = Capture.ReadFor(700ms);
    if (Before && HasTone(*Before) &&
        TerminateProcess(Process.hProcess, 197)) {
        (void)WaitForSingleObject(Process.hProcess, 5'000);
        const auto After = Capture.ReadFor(140ms);
        Result = After && IsSilentAfter(*After, 60ms);
    } else {
        (void)TerminateProcess(Process.hProcess, 198);
        (void)WaitForSingleObject(Process.hProcess, 5'000);
    }
    CloseHandle(Process.hProcess);
    return Result;
}

int FeedChild() {
    desklink::Win32VirtualMicrophoneFeed Feed;
    if (!Feed.Start()) return 2;
    ToneFeeder Feeder(Feed);
    std::this_thread::sleep_for(60s);
    return 0;
}

bool HasArgument(int Count, wchar_t** Arguments,
                 std::wstring_view Expected) noexcept {
    for (int Index = 1; Index < Count; ++Index) {
        if (Expected == Arguments[Index]) return true;
    }
    return false;
}

} // namespace

int wmain(int ArgumentCount, wchar_t** Arguments) {
    if (HasArgument(ArgumentCount, Arguments, L"--feed-child")) {
        return FeedChild();
    }
    const auto ComResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(ComResult)) {
        std::cerr << "[VirtualMicrophone:Validation] COM initialization failed\n";
        return 1;
    }
    int ExitCode = 1;
    {
        ComPtr<IMMDeviceEnumerator> Enumerator;
        ComPtr<IMMDevice> RemoteMicrophone;
        std::uint32_t OtherCaptureEndpoints{};
        if (FAILED(CoCreateInstance(
                __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                __uuidof(IMMDeviceEnumerator),
                reinterpret_cast<void**>(Enumerator.Put()))) ||
            !FindRemoteMicrophone(
                Enumerator.Get(), RemoteMicrophone, OtherCaptureEndpoints)) {
            std::cerr
                << "[VirtualMicrophone:Validation] exactly one stable DeskLink Remote Microphone endpoint is required\n";
        } else if (HasArgument(
                       ArgumentCount, Arguments,
                       L"--require-no-physical-microphones") &&
                   OtherCaptureEndpoints != 0) {
            std::cerr
                << "[VirtualMicrophone:Validation] non-DeskLink capture endpoints are present; zero-microphone qualification refused\n";
        } else {
            CaptureProbe Capture;
            desklink::Win32VirtualMicrophoneFeed Feed;
            const auto Initial = Capture.Start(RemoteMicrophone.Get())
                ? Capture.ReadFor(120ms) : std::nullopt;
            bool Passed = Initial && IsSilentAfter(*Initial, 0ms);
            if (!Passed) {
                std::cerr
                    << "[VirtualMicrophone:Validation] idle capture was not hard silence\n";
            }

            if (Passed && Feed.Start()) {
                {
                    ToneFeeder Feeder(Feed);
                    const auto Signal = Capture.ReadFor(700ms);
                    Passed = Signal && HasTone(*Signal);
                }
                if (!Passed) {
                    std::cerr
                        << "[VirtualMicrophone:Validation] deterministic feed signal was not captured\n";
                }
            } else if (Passed) {
                Passed = false;
                std::cerr
                    << "[VirtualMicrophone:Validation] DeskLink feed endpoint could not start\n";
            }

            Feed.Stop();
            if (Passed) {
                const auto Silence = Capture.ReadFor(140ms);
                Passed = Silence && IsSilentAfter(*Silence, 60ms);
                if (!Passed) {
                    std::cerr
                        << "[VirtualMicrophone:Validation] stop did not become silent within 60 ms\n";
                }
            }

            if (Passed && Feed.Start()) {
                ToneFeeder Feeder(Feed);
                const auto Restarted = Capture.ReadFor(500ms);
                Passed = Restarted && HasTone(*Restarted);
                if (!Passed) {
                    std::cerr
                        << "[VirtualMicrophone:Validation] fresh signal did not resume after restart\n";
                }
            } else if (Passed) {
                Passed = false;
            }
            Feed.Stop();

            if (Passed) {
                Passed = ValidateCrashSilence(Capture);
                if (!Passed) {
                    std::cerr
                        << "[VirtualMicrophone:Validation] killed feed process did not become silent within 60 ms\n";
                }
            }
            Capture.Stop();
            if (Passed) {
                std::cout
                    << "[VirtualMicrophone:Validation] property identity, idle silence, 997 Hz signal, <=60 ms stop/crash silence, and fresh restart passed; other_capture_endpoints="
                    << OtherCaptureEndpoints << '\n';
                ExitCode = 0;
            }
        }
    }
    CoUninitialize();
    return ExitCode;
}
