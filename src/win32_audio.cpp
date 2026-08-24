#include "desklink/win32_audio.hpp"

#ifdef _WIN32

#include "desklink/audio.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace desklink {
namespace {

constexpr REFERENCE_TIME kRequestedBufferDuration = 200'000; // 20 ms.
constexpr std::size_t kMaximumRenderQueueFrames = 64;
constexpr std::size_t kMaximumCaptureChunkFrames = 8'192;

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

class ComApartment final {
public:
    ComApartment() noexcept
        : Result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComApartment() {
        if (SUCCEEDED(Result_)) CoUninitialize();
    }
    [[nodiscard]] bool Ready() const noexcept { return SUCCEEDED(Result_); }

private:
    HRESULT Result_{};
};

class MmcssRegistration final {
public:
    MmcssRegistration() noexcept {
        DWORD TaskIndex{};
        Handle_ = AvSetMmThreadCharacteristicsW(L"Pro Audio", &TaskIndex);
    }
    ~MmcssRegistration() {
        if (Handle_) AvRevertMmThreadCharacteristics(Handle_);
    }

private:
    HANDLE Handle_{};
};

WAVEFORMATEX DeskLinkWaveFormat() noexcept {
    WAVEFORMATEX Format{};
    Format.wFormatTag = WAVE_FORMAT_PCM;
    Format.nChannels = kDeskLinkAudioChannels;
    Format.nSamplesPerSec = kDeskLinkAudioSampleRate;
    Format.wBitsPerSample =
        static_cast<WORD>(kDeskLinkAudioBytesPerSample * 8u);
    Format.nBlockAlign = static_cast<WORD>(kDeskLinkAudioBytesPerFrame);
    Format.nAvgBytesPerSec =
        Format.nSamplesPerSec * Format.nBlockAlign;
    return Format;
}

std::uint64_t SteadyTimestampUs() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

template <typename Handler>
void PublishFailure(const Handler& Failed, std::string Message) noexcept {
    if (!Failed) return;
    try {
        Failed(std::move(Message));
    } catch (...) {
    }
}

bool OpenDefaultRenderClient(
    ComPtr<IMMDeviceEnumerator>& Enumerator, ComPtr<IMMDevice>& Device,
    ComPtr<IAudioClient>& AudioClient) noexcept {
    if (FAILED(CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(Enumerator.Put())))) {
        return false;
    }
    if (FAILED(Enumerator->GetDefaultAudioEndpoint(
            eRender, eConsole, Device.Put()))) {
        return false;
    }
    return SUCCEEDED(Device->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(AudioClient.Put())));
}

} // namespace

struct Win32WasapiLoopbackCapture::State {
    State(std::uint32_t OwnedStreamId, Win32WasapiCaptureHandlers OwnedHandlers)
        : StreamId(OwnedStreamId), Handlers(std::move(OwnedHandlers)) {}

    std::uint32_t StreamId{};
    Win32WasapiCaptureHandlers Handlers;
    std::thread Thread;
    std::mutex StartMutex;
    std::condition_variable Started;
    HANDLE StopEvent{};
    HANDLE AudioEvent{};
    bool StartComplete{};
    bool StartSucceeded{};
    std::atomic_bool IsRunning{};

    void FinishStart(bool Success) noexcept {
        {
            std::scoped_lock Lock(StartMutex);
            StartSucceeded = Success;
            StartComplete = true;
        }
        IsRunning.store(Success);
        Started.notify_all();
    }

    bool Deliver(std::vector<AudioFrameMessage>& Frames) noexcept {
        if (!Handlers.Frame) return true;
        try {
            for (auto& Frame : Frames) {
                if (!Handlers.Frame(std::move(Frame))) {
                    PublishFailure(Handlers.Failed,
                                   "audio capture frame was rejected");
                    return false;
                }
            }
            return true;
        } catch (...) {
            PublishFailure(Handlers.Failed, "audio capture handler failed");
            return false;
        }
    }

    void Run() noexcept {
        ComApartment Apartment;
        MmcssRegistration Mmcss;
        ComPtr<IMMDeviceEnumerator> Enumerator;
        ComPtr<IMMDevice> Device;
        ComPtr<IAudioClient> AudioClient;
        ComPtr<IAudioCaptureClient> CaptureClient;
        auto Format = DeskLinkWaveFormat();
        const DWORD Flags =
            AUDCLNT_STREAMFLAGS_LOOPBACK |
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        const bool Initialized = Apartment.Ready() &&
            OpenDefaultRenderClient(Enumerator, Device, AudioClient) &&
            SUCCEEDED(AudioClient->Initialize(
                AUDCLNT_SHAREMODE_SHARED, Flags, kRequestedBufferDuration,
                0, &Format, nullptr)) &&
            SUCCEEDED(AudioClient->SetEventHandle(AudioEvent)) &&
            SUCCEEDED(AudioClient->GetService(
                __uuidof(IAudioCaptureClient),
                reinterpret_cast<void**>(CaptureClient.Put()))) &&
            SUCCEEDED(AudioClient->Start());
        FinishStart(Initialized);
        if (!Initialized) {
            PublishFailure(Handlers.Failed,
                           "WASAPI loopback initialization failed");
            return;
        }

        AudioFrameAssembler Assembler(StreamId);
        HANDLE WaitHandles[]{StopEvent, AudioEvent};
        bool Failed{};
        while (!Failed) {
            const auto WaitResult = WaitForMultipleObjects(
                2, WaitHandles, FALSE, INFINITE);
            if (WaitResult == WAIT_OBJECT_0) break;
            if (WaitResult != WAIT_OBJECT_0 + 1) {
                PublishFailure(Handlers.Failed, "WASAPI capture wait failed");
                break;
            }

            UINT32 PacketFrames{};
            while (true) {
                const auto PacketSizeResult =
                    CaptureClient->GetNextPacketSize(&PacketFrames);
                if (FAILED(PacketSizeResult)) {
                    PublishFailure(Handlers.Failed,
                                   "WASAPI capture packet query failed");
                    Failed = true;
                    break;
                }
                if (PacketFrames == 0) break;
                BYTE* Data{};
                DWORD PacketFlags{};
                UINT64 QpcPosition{};
                const auto GetResult = CaptureClient->GetBuffer(
                    &Data, &PacketFrames, &PacketFlags, nullptr, &QpcPosition);
                if (FAILED(GetResult)) {
                    PublishFailure(Handlers.Failed,
                                   "WASAPI capture packet rejected");
                    Failed = true;
                    break;
                }
                const bool Silent =
                    (PacketFlags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                if (PacketFrames > kMaximumCaptureChunkFrames ||
                    (!Silent && Data == nullptr)) {
                    CaptureClient->ReleaseBuffer(PacketFrames);
                    PublishFailure(Handlers.Failed,
                                   "WASAPI capture packet rejected");
                    Failed = true;
                    break;
                }
                if ((PacketFlags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0) {
                    Assembler.Reset();
                }
                const auto TimestampUs =
                    (PacketFlags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0
                    ? SteadyTimestampUs()
                    : static_cast<std::uint64_t>(QpcPosition / 10u);
                std::vector<AudioFrameMessage> Frames;
                const bool Accepted = Silent
                    ? Assembler.PushSilence(PacketFrames, TimestampUs, Frames)
                    : Assembler.Push(
                        ByteSpan(Data,
                            static_cast<std::size_t>(PacketFrames) *
                            kDeskLinkAudioBytesPerFrame),
                        TimestampUs, Frames);
                const auto ReleaseResult =
                    CaptureClient->ReleaseBuffer(PacketFrames);
                if (!Accepted || FAILED(ReleaseResult)) {
                    PublishFailure(Handlers.Failed,
                                   "WASAPI capture packet processing failed");
                    Failed = true;
                    break;
                }
                if (!Deliver(Frames)) {
                    Failed = true;
                    break;
                }
            }
        }
        AudioClient->Stop();
        IsRunning.store(false);
    }
};

Win32WasapiLoopbackCapture::Win32WasapiLoopbackCapture(
    std::uint32_t StreamId, Win32WasapiCaptureHandlers Handlers)
    : State_(std::make_unique<State>(StreamId, std::move(Handlers))) {}

Win32WasapiLoopbackCapture::~Win32WasapiLoopbackCapture() {
    Stop();
}

bool Win32WasapiLoopbackCapture::Start() {
    if (State_->Thread.joinable()) return State_->StartSucceeded;
    State_->StopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    State_->AudioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!State_->StopEvent || !State_->AudioEvent) {
        Stop();
        return false;
    }
    {
        std::scoped_lock Lock(State_->StartMutex);
        State_->StartComplete = false;
        State_->StartSucceeded = false;
    }
    State_->Thread = std::thread([State = State_.get()] { State->Run(); });
    std::unique_lock Lock(State_->StartMutex);
    State_->Started.wait(Lock, [&] { return State_->StartComplete; });
    if (State_->StartSucceeded) return true;
    Lock.unlock();
    Stop();
    return false;
}

void Win32WasapiLoopbackCapture::Stop() noexcept {
    if (State_->StopEvent) SetEvent(State_->StopEvent);
    if (State_->Thread.joinable() &&
        State_->Thread.get_id() == std::this_thread::get_id()) {
        State_->IsRunning.store(false);
        return;
    }
    if (State_->Thread.joinable()) {
        State_->Thread.join();
    }
    State_->IsRunning.store(false);
    if (State_->AudioEvent) CloseHandle(State_->AudioEvent);
    if (State_->StopEvent) CloseHandle(State_->StopEvent);
    State_->AudioEvent = nullptr;
    State_->StopEvent = nullptr;
}

bool Win32WasapiLoopbackCapture::Running() const noexcept {
    return State_->IsRunning.load();
}

struct Win32WasapiRenderer::State {
    explicit State(Win32WasapiRenderHandlers OwnedHandlers)
        : Handlers(std::move(OwnedHandlers)) {}

    Win32WasapiRenderHandlers Handlers;
    std::thread Thread;
    std::mutex StartMutex;
    std::condition_variable Started;
    HANDLE StopEvent{};
    HANDLE AudioEvent{};
    bool StartComplete{};
    bool StartSucceeded{};
    std::atomic_bool IsRunning{};
    mutable std::mutex QueueMutex;
    std::deque<AudioFrameMessage> Queue;
    std::size_t CurrentOffset{};
    std::atomic_uint64_t UnderrunCount{};

    void FinishStart(bool Success) noexcept {
        {
            std::scoped_lock Lock(StartMutex);
            StartSucceeded = Success;
            StartComplete = true;
        }
        IsRunning.store(Success);
        Started.notify_all();
    }

    bool Fill(std::uint8_t* Destination, std::size_t Bytes) noexcept {
        std::fill_n(Destination, static_cast<std::ptrdiff_t>(Bytes),
                    std::uint8_t{0});
        std::scoped_lock Lock(QueueMutex);
        std::size_t Written{};
        while (Written < Bytes && !Queue.empty()) {
            auto& Frame = Queue.front();
            const auto Available = Frame.pcm.size() - CurrentOffset;
            const auto CopyBytes = std::min(Available, Bytes - Written);
            std::memcpy(Destination + Written,
                        Frame.pcm.data() + CurrentOffset, CopyBytes);
            Written += CopyBytes;
            CurrentOffset += CopyBytes;
            if (CurrentOffset == Frame.pcm.size()) {
                Queue.pop_front();
                CurrentOffset = 0;
            }
        }
        return Written != 0;
    }

    void Run() noexcept {
        ComApartment Apartment;
        MmcssRegistration Mmcss;
        ComPtr<IMMDeviceEnumerator> Enumerator;
        ComPtr<IMMDevice> Device;
        ComPtr<IAudioClient> AudioClient;
        ComPtr<IAudioRenderClient> RenderClient;
        auto Format = DeskLinkWaveFormat();
        const DWORD Flags =
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
            AUDCLNT_STREAMFLAGS_NOPERSIST;
        UINT32 BufferFrames{};
        BYTE* InitialBuffer{};
        bool Initialized = Apartment.Ready() &&
            OpenDefaultRenderClient(Enumerator, Device, AudioClient) &&
            SUCCEEDED(AudioClient->Initialize(
                AUDCLNT_SHAREMODE_SHARED, Flags, kRequestedBufferDuration,
                0, &Format, nullptr)) &&
            SUCCEEDED(AudioClient->SetEventHandle(AudioEvent)) &&
            SUCCEEDED(AudioClient->GetBufferSize(&BufferFrames)) &&
            BufferFrames != 0 &&
            SUCCEEDED(AudioClient->GetService(
                __uuidof(IAudioRenderClient),
                reinterpret_cast<void**>(RenderClient.Put()))) &&
            SUCCEEDED(RenderClient->GetBuffer(BufferFrames, &InitialBuffer));
        if (Initialized) {
            Initialized = SUCCEEDED(RenderClient->ReleaseBuffer(
                BufferFrames, AUDCLNT_BUFFERFLAGS_SILENT)) &&
                SUCCEEDED(AudioClient->Start());
        }
        FinishStart(Initialized);
        if (!Initialized) {
            PublishFailure(Handlers.Failed,
                           "WASAPI render initialization failed");
            return;
        }

        HANDLE WaitHandles[]{StopEvent, AudioEvent};
        while (true) {
            const auto WaitResult = WaitForMultipleObjects(
                2, WaitHandles, FALSE, INFINITE);
            if (WaitResult == WAIT_OBJECT_0) break;
            if (WaitResult != WAIT_OBJECT_0 + 1) {
                PublishFailure(Handlers.Failed, "WASAPI render wait failed");
                break;
            }
            UINT32 Padding{};
            if (FAILED(AudioClient->GetCurrentPadding(&Padding)) ||
                Padding > BufferFrames) {
                PublishFailure(Handlers.Failed,
                               "WASAPI render padding failed");
                break;
            }
            const auto AvailableFrames = BufferFrames - Padding;
            if (AvailableFrames == 0) continue;
            BYTE* Buffer{};
            if (FAILED(RenderClient->GetBuffer(AvailableFrames, &Buffer))) {
                PublishFailure(Handlers.Failed,
                               "WASAPI render buffer failed");
                break;
            }
            const auto HasAudio = Fill(
                Buffer, static_cast<std::size_t>(AvailableFrames) *
                    kDeskLinkAudioBytesPerFrame);
            const auto ReleaseResult = RenderClient->ReleaseBuffer(
                AvailableFrames,
                HasAudio ? 0 : AUDCLNT_BUFFERFLAGS_SILENT);
            if (!HasAudio) ++UnderrunCount;
            if (FAILED(ReleaseResult)) {
                PublishFailure(Handlers.Failed,
                               "WASAPI render release failed");
                break;
            }
        }
        AudioClient->Stop();
        IsRunning.store(false);
    }
};

Win32WasapiRenderer::Win32WasapiRenderer(Win32WasapiRenderHandlers Handlers)
    : State_(std::make_unique<State>(std::move(Handlers))) {}

Win32WasapiRenderer::~Win32WasapiRenderer() {
    Stop();
}

bool Win32WasapiRenderer::Start() {
    if (State_->Thread.joinable()) return State_->StartSucceeded;
    State_->StopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    State_->AudioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!State_->StopEvent || !State_->AudioEvent) {
        Stop();
        return false;
    }
    {
        std::scoped_lock Lock(State_->StartMutex);
        State_->StartComplete = false;
        State_->StartSucceeded = false;
    }
    State_->UnderrunCount.store(0);
    State_->Thread = std::thread([State = State_.get()] { State->Run(); });
    std::unique_lock Lock(State_->StartMutex);
    State_->Started.wait(Lock, [&] { return State_->StartComplete; });
    if (State_->StartSucceeded) return true;
    Lock.unlock();
    Stop();
    return false;
}

bool Win32WasapiRenderer::Submit(AudioFrameMessage Frame) {
    if (!State_->IsRunning.load() || !IsDeskLinkAudioFrame(Frame)) return false;
    std::scoped_lock Lock(State_->QueueMutex);
    if (State_->Queue.size() >= kMaximumRenderQueueFrames) return false;
    State_->Queue.push_back(std::move(Frame));
    return true;
}

void Win32WasapiRenderer::Stop() noexcept {
    if (State_->StopEvent) SetEvent(State_->StopEvent);
    if (State_->Thread.joinable() &&
        State_->Thread.get_id() == std::this_thread::get_id()) {
        State_->IsRunning.store(false);
        return;
    }
    if (State_->Thread.joinable()) {
        State_->Thread.join();
    }
    State_->IsRunning.store(false);
    {
        std::scoped_lock Lock(State_->QueueMutex);
        State_->Queue.clear();
        State_->CurrentOffset = 0;
    }
    if (State_->AudioEvent) CloseHandle(State_->AudioEvent);
    if (State_->StopEvent) CloseHandle(State_->StopEvent);
    State_->AudioEvent = nullptr;
    State_->StopEvent = nullptr;
}

bool Win32WasapiRenderer::Running() const noexcept {
    return State_->IsRunning.load();
}

std::size_t Win32WasapiRenderer::QueuedFrames() const noexcept {
    std::scoped_lock Lock(State_->QueueMutex);
    return State_->Queue.size();
}

std::uint64_t Win32WasapiRenderer::Underruns() const noexcept {
    return State_->UnderrunCount.load();
}

} // namespace desklink

#endif
