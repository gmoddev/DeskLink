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
#include <new>
#include <optional>
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
void PublishFailure(const Handler& Failed, Win32WasapiFailureKind Kind,
                    std::string Message) noexcept {
    if (!Failed) return;
    try {
        Failed(Kind, std::move(Message));
    } catch (...) {
    }
}

class EndpointNotificationClient final : public IMMNotificationClient {
public:
    explicit EndpointNotificationClient(HANDLE ChangedEvent) noexcept
        : ChangedEvent_(ChangedEvent) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID InterfaceId, void** Object) noexcept override {
        if (Object == nullptr) return E_POINTER;
        *Object = nullptr;
        if (InterfaceId == __uuidof(IUnknown) ||
            InterfaceId == __uuidof(IMMNotificationClient)) {
            *Object = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override {
        return ++References_;
    }

    ULONG STDMETHODCALLTYPE Release() noexcept override {
        const auto Remaining = --References_;
        if (Remaining == 0) delete this;
        return Remaining;
    }

    void SetDeviceId(std::wstring DeviceId) noexcept {
        DeviceId_ = std::move(DeviceId);
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(
        LPCWSTR DeviceId, DWORD NewState) noexcept override {
        if (Matches(DeviceId) && (NewState & DEVICE_STATE_ACTIVE) == 0) {
            Signal();
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) noexcept override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(
        LPCWSTR DeviceId) noexcept override {
        if (Matches(DeviceId)) Signal();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(
        EDataFlow Flow, ERole Role, LPCWSTR) noexcept override {
        if (Flow == eRender && Role == eConsole) Signal();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(
        LPCWSTR DeviceId, const PROPERTYKEY) noexcept override {
        if (Matches(DeviceId)) Signal();
        return S_OK;
    }

private:
    [[nodiscard]] bool Matches(LPCWSTR DeviceId) const noexcept {
        return DeviceId != nullptr && !DeviceId_.empty() &&
            _wcsicmp(DeviceId_.c_str(), DeviceId) == 0;
    }

    void Signal() noexcept {
        if (ChangedEvent_) SetEvent(ChangedEvent_);
    }

    HANDLE ChangedEvent_{};
    std::wstring DeviceId_;
    std::atomic_ulong References_{1};
};

class EndpointNotificationOwner final {
public:
    explicit EndpointNotificationOwner(HANDLE ChangedEvent) noexcept
        : Client_(new (std::nothrow)
              EndpointNotificationClient(ChangedEvent)) {}
    ~EndpointNotificationOwner() {
        if (Client_) Client_->Release();
    }

    [[nodiscard]] EndpointNotificationClient* Get() const noexcept {
        return Client_;
    }

private:
    EndpointNotificationClient* Client_{};
};

class EndpointNotificationRegistration final {
public:
    EndpointNotificationRegistration() = default;
    ~EndpointNotificationRegistration() {
        if (Enumerator_ && Client_) {
            Enumerator_->UnregisterEndpointNotificationCallback(Client_);
        }
    }

    [[nodiscard]] bool Start(
        IMMDeviceEnumerator* Enumerator,
        IMMNotificationClient* Client) noexcept {
        if (!Enumerator || !Client || Enumerator_ || Client_) return false;
        if (FAILED(Enumerator->RegisterEndpointNotificationCallback(Client))) {
            return false;
        }
        Enumerator_ = Enumerator;
        Client_ = Client;
        return true;
    }

private:
    IMMDeviceEnumerator* Enumerator_{};
    IMMNotificationClient* Client_{};
};

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

std::optional<std::wstring> GetDeviceId(IMMDevice* Device) noexcept {
    if (!Device) return std::nullopt;
    LPWSTR RawDeviceId{};
    if (FAILED(Device->GetId(&RawDeviceId)) || RawDeviceId == nullptr) {
        return std::nullopt;
    }
    std::optional<std::wstring> Result;
    try {
        Result = std::wstring(RawDeviceId);
    } catch (...) {
    }
    CoTaskMemFree(RawDeviceId);
    return Result;
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
    HANDLE EndpointEvent{};
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
                                   Win32WasapiFailureKind::ClientRejected,
                                   "audio capture frame was rejected");
                    return false;
                }
            }
            return true;
        } catch (...) {
            PublishFailure(Handlers.Failed,
                           Win32WasapiFailureKind::ClientRejected,
                           "audio capture handler failed");
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
        EndpointNotificationOwner Notifications(EndpointEvent);
        EndpointNotificationRegistration NotificationRegistration;
        auto Format = DeskLinkWaveFormat();
        const DWORD Flags =
            AUDCLNT_STREAMFLAGS_LOOPBACK |
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        bool Initialized = Apartment.Ready() &&
            OpenDefaultRenderClient(Enumerator, Device, AudioClient);
        if (Initialized) {
            auto DeviceId = GetDeviceId(Device.Get());
            if (DeviceId && Notifications.Get()) {
                Notifications.Get()->SetDeviceId(std::move(*DeviceId));
            }
            Initialized = DeviceId.has_value() && Notifications.Get() &&
                NotificationRegistration.Start(
                Enumerator.Get(), Notifications.Get()) &&
                SUCCEEDED(AudioClient->Initialize(
                    AUDCLNT_SHAREMODE_SHARED, Flags,
                    kRequestedBufferDuration, 0, &Format, nullptr)) &&
                SUCCEEDED(AudioClient->SetEventHandle(AudioEvent)) &&
                SUCCEEDED(AudioClient->GetService(
                    __uuidof(IAudioCaptureClient),
                    reinterpret_cast<void**>(CaptureClient.Put()))) &&
                SUCCEEDED(AudioClient->Start());
        }
        FinishStart(Initialized);
        if (!Initialized) {
            PublishFailure(Handlers.Failed,
                           Win32WasapiFailureKind::EndpointUnavailable,
                           "WASAPI loopback initialization failed");
            return;
        }

        AudioFrameAssembler Assembler(StreamId);
        HANDLE WaitHandles[]{StopEvent, EndpointEvent, AudioEvent};
        bool Failed{};
        while (!Failed) {
            const auto WaitResult = WaitForMultipleObjects(
                3, WaitHandles, FALSE, INFINITE);
            if (WaitResult == WAIT_OBJECT_0) break;
            if (WaitResult == WAIT_OBJECT_0 + 1) {
                PublishFailure(Handlers.Failed,
                               Win32WasapiFailureKind::EndpointChanged,
                               "WASAPI render endpoint changed");
                break;
            }
            if (WaitResult != WAIT_OBJECT_0 + 2) {
                PublishFailure(Handlers.Failed,
                               Win32WasapiFailureKind::EndpointUnavailable,
                               "WASAPI capture wait failed");
                break;
            }

            UINT32 PacketFrames{};
            while (true) {
                const auto PacketSizeResult =
                    CaptureClient->GetNextPacketSize(&PacketFrames);
                if (FAILED(PacketSizeResult)) {
                    PublishFailure(Handlers.Failed,
                                   Win32WasapiFailureKind::EndpointUnavailable,
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
                                   Win32WasapiFailureKind::EndpointUnavailable,
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
                                   Win32WasapiFailureKind::EndpointUnavailable,
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
                                   Win32WasapiFailureKind::EndpointUnavailable,
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
    if (State_->Thread.joinable()) {
        if (State_->IsRunning.load()) return true;
        Stop();
    }
    State_->StopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    State_->EndpointEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    State_->AudioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!State_->StopEvent || !State_->EndpointEvent ||
        !State_->AudioEvent) {
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
    if (State_->EndpointEvent) CloseHandle(State_->EndpointEvent);
    if (State_->StopEvent) CloseHandle(State_->StopEvent);
    State_->AudioEvent = nullptr;
    State_->EndpointEvent = nullptr;
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
    HANDLE EndpointEvent{};
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
        EndpointNotificationOwner Notifications(EndpointEvent);
        EndpointNotificationRegistration NotificationRegistration;
        auto Format = DeskLinkWaveFormat();
        const DWORD Flags =
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
            AUDCLNT_STREAMFLAGS_NOPERSIST;
        UINT32 BufferFrames{};
        BYTE* InitialBuffer{};
        bool Initialized = Apartment.Ready() &&
            OpenDefaultRenderClient(Enumerator, Device, AudioClient);
        if (Initialized) {
            auto DeviceId = GetDeviceId(Device.Get());
            if (DeviceId && Notifications.Get()) {
                Notifications.Get()->SetDeviceId(std::move(*DeviceId));
            }
            Initialized = DeviceId.has_value() && Notifications.Get() &&
                NotificationRegistration.Start(
                Enumerator.Get(), Notifications.Get()) &&
                SUCCEEDED(AudioClient->Initialize(
                    AUDCLNT_SHAREMODE_SHARED, Flags,
                    kRequestedBufferDuration, 0, &Format, nullptr)) &&
                SUCCEEDED(AudioClient->SetEventHandle(AudioEvent)) &&
                SUCCEEDED(AudioClient->GetBufferSize(&BufferFrames)) &&
                BufferFrames != 0 &&
                SUCCEEDED(AudioClient->GetService(
                    __uuidof(IAudioRenderClient),
                    reinterpret_cast<void**>(RenderClient.Put()))) &&
                SUCCEEDED(RenderClient->GetBuffer(
                    BufferFrames, &InitialBuffer));
        }
        if (Initialized) {
            Initialized = SUCCEEDED(RenderClient->ReleaseBuffer(
                BufferFrames, AUDCLNT_BUFFERFLAGS_SILENT)) &&
                SUCCEEDED(AudioClient->Start());
        }
        FinishStart(Initialized);
        if (!Initialized) {
            PublishFailure(Handlers.Failed,
                           Win32WasapiFailureKind::EndpointUnavailable,
                           "WASAPI render initialization failed");
            return;
        }

        HANDLE WaitHandles[]{StopEvent, EndpointEvent, AudioEvent};
        while (true) {
            const auto WaitResult = WaitForMultipleObjects(
                3, WaitHandles, FALSE, INFINITE);
            if (WaitResult == WAIT_OBJECT_0) break;
            if (WaitResult == WAIT_OBJECT_0 + 1) {
                PublishFailure(Handlers.Failed,
                               Win32WasapiFailureKind::EndpointChanged,
                               "WASAPI render endpoint changed");
                break;
            }
            if (WaitResult != WAIT_OBJECT_0 + 2) {
                PublishFailure(Handlers.Failed,
                               Win32WasapiFailureKind::EndpointUnavailable,
                               "WASAPI render wait failed");
                break;
            }
            UINT32 Padding{};
            if (FAILED(AudioClient->GetCurrentPadding(&Padding)) ||
                Padding > BufferFrames) {
                PublishFailure(Handlers.Failed,
                               Win32WasapiFailureKind::EndpointUnavailable,
                               "WASAPI render padding failed");
                break;
            }
            const auto AvailableFrames = BufferFrames - Padding;
            if (AvailableFrames == 0) continue;
            BYTE* Buffer{};
            if (FAILED(RenderClient->GetBuffer(AvailableFrames, &Buffer))) {
                PublishFailure(Handlers.Failed,
                               Win32WasapiFailureKind::EndpointUnavailable,
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
                               Win32WasapiFailureKind::EndpointUnavailable,
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
    if (State_->Thread.joinable()) {
        if (State_->IsRunning.load()) return true;
        Stop();
    }
    State_->StopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    State_->EndpointEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    State_->AudioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!State_->StopEvent || !State_->EndpointEvent ||
        !State_->AudioEvent) {
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
    if (State_->EndpointEvent) CloseHandle(State_->EndpointEvent);
    if (State_->StopEvent) CloseHandle(State_->StopEvent);
    State_->AudioEvent = nullptr;
    State_->EndpointEvent = nullptr;
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
