#include "desklink/win32_voice.hpp"

#if defined(_WIN32) && defined(DESKLINK_BUILD_VOICE)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <propsys.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <string_view>
#include <thread>
#include <utility>

namespace desklink {
namespace {

constexpr REFERENCE_TIME kVoiceBufferDuration = 400'000; // 40 ms.
constexpr std::size_t kMaximumCaptureChunkFrames = 8'192;
constexpr std::size_t kMaximumVoiceRenderQueueFrames = 12;
constexpr std::size_t kMaximumVirtualMicrophoneQueueFrames = 3; // 60 ms.
constexpr PROPERTYKEY kDeskLinkEndpointKindProperty{
    {0xd21f0a7c, 0x80da, 0x4e7e,
     {0xa9, 0x06, 0x81, 0xdf, 0x3e, 0x2e, 0xa4, 0xb9}},
    2};

template <typename Interface>
class ComPtr final {
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    [[nodiscard]] Interface* Get() const noexcept { return Value_; }
    [[nodiscard]] Interface** Put() noexcept { Reset(); return &Value_; }
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
    ~ComApartment() { if (SUCCEEDED(Result_)) CoUninitialize(); }
    [[nodiscard]] bool Ready() const noexcept { return SUCCEEDED(Result_); }
private:
    HRESULT Result_{};
};

class MmcssRegistration final {
public:
    MmcssRegistration() noexcept {
        DWORD TaskIndex{};
        Handle_ = AvSetMmThreadCharacteristicsW(L"Audio", &TaskIndex);
    }
    ~MmcssRegistration() {
        if (Handle_) AvRevertMmThreadCharacteristics(Handle_);
    }
private:
    HANDLE Handle_{};
};

WAVEFORMATEX VoiceWaveFormat() noexcept {
    WAVEFORMATEX Format{};
    Format.wFormatTag = WAVE_FORMAT_PCM;
    Format.nChannels = kVoiceChannels;
    Format.nSamplesPerSec = kVoiceSampleRate;
    Format.wBitsPerSample = 16;
    Format.nBlockAlign = 2;
    Format.nAvgBytesPerSec = Format.nSamplesPerSec * Format.nBlockAlign;
    return Format;
}

std::uint64_t SteadyTimestampUs() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::optional<std::wstring> Utf8ToWide(std::string_view Value) {
    if (Value.empty()) return std::nullopt;
    const auto Count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, Value.data(),
        static_cast<int>(Value.size()), nullptr, 0);
    if (Count <= 0) return std::nullopt;
    std::wstring Result(static_cast<std::size_t>(Count), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, Value.data(),
            static_cast<int>(Value.size()), Result.data(), Count) != Count) {
        return std::nullopt;
    }
    return Result;
}

std::optional<std::string> WideToUtf8(std::wstring_view Value) {
    if (Value.empty()) return std::nullopt;
    const auto Count = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, Value.data(),
        static_cast<int>(Value.size()), nullptr, 0, nullptr, nullptr);
    if (Count <= 0) return std::nullopt;
    std::string Result(static_cast<std::size_t>(Count), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, Value.data(),
            static_cast<int>(Value.size()), Result.data(), Count,
            nullptr, nullptr) != Count) {
        return std::nullopt;
    }
    return Result;
}

std::optional<std::wstring> DeviceId(IMMDevice* Device) noexcept {
    if (!Device) return std::nullopt;
    LPWSTR Raw{};
    if (FAILED(Device->GetId(&Raw)) || !Raw) return std::nullopt;
    std::optional<std::wstring> Result;
    try { Result = std::wstring(Raw); } catch (...) {}
    CoTaskMemFree(Raw);
    return Result;
}

template <typename Handler>
void PublishFailure(const Handler& Failed, Win32WasapiFailureKind Kind,
                    std::string Message) noexcept {
    if (!Failed) return;
    try { Failed(Kind, std::move(Message)); } catch (...) {}
}

void ApplyCommunicationsCategory(IAudioClient* Client) noexcept {
    if (!Client) return;
    ComPtr<IAudioClient2> Client2;
    if (FAILED(Client->QueryInterface(
            __uuidof(IAudioClient2),
            reinterpret_cast<void**>(Client2.Put())))) {
        return;
    }
    AudioClientProperties Properties{};
    Properties.cbSize = sizeof(Properties);
    Properties.eCategory = AudioCategory_Communications;
    (void)Client2->SetClientProperties(&Properties);
}

class EndpointNotifications final : public IMMNotificationClient {
public:
    EndpointNotifications(HANDLE Event, EDataFlow Flow,
                          bool FollowDefault) noexcept
        : Event_(Event), Flow_(Flow), FollowDefault_(FollowDefault) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID Id, void** Object) noexcept override {
        if (!Object) return E_POINTER;
        *Object = nullptr;
        if (Id == __uuidof(IUnknown) || Id == __uuidof(IMMNotificationClient)) {
            *Object = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++Refs_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        const auto Remaining = --Refs_;
        if (!Remaining) delete this;
        return Remaining;
    }
    void SetDeviceId(std::wstring Id) { DeviceId_ = std::move(Id); }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(
        LPCWSTR Id, DWORD State) noexcept override {
        if (Matches(Id) && (State & DEVICE_STATE_ACTIVE) == 0) Signal();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) noexcept override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR Id) noexcept override {
        if (Matches(Id)) Signal();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(
        EDataFlow Flow, ERole Role, LPCWSTR) noexcept override {
        if (FollowDefault_ && Flow == Flow_ && Role == eCommunications) {
            Signal();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(
        LPCWSTR Id, const PROPERTYKEY) noexcept override {
        if (Matches(Id)) Signal();
        return S_OK;
    }
private:
    [[nodiscard]] bool Matches(LPCWSTR Id) const noexcept {
        return Id && !DeviceId_.empty() &&
            _wcsicmp(DeviceId_.c_str(), Id) == 0;
    }
    void Signal() noexcept { if (Event_) SetEvent(Event_); }
    HANDLE Event_{};
    EDataFlow Flow_{};
    bool FollowDefault_{};
    std::wstring DeviceId_;
    std::atomic_ulong Refs_{1};
};

class NotificationRegistration final {
public:
    ~NotificationRegistration() {
        if (Enumerator_ && Client_) {
            Enumerator_->UnregisterEndpointNotificationCallback(Client_);
        }
        if (Client_) Client_->Release();
    }
    [[nodiscard]] bool Start(IMMDeviceEnumerator* Enumerator,
                             EndpointNotifications* Client) noexcept {
        if (!Enumerator || !Client ||
            FAILED(Enumerator->RegisterEndpointNotificationCallback(Client))) {
            if (Client) Client->Release();
            return false;
        }
        Enumerator_ = Enumerator;
        Client_ = Client;
        return true;
    }
private:
    IMMDeviceEnumerator* Enumerator_{};
    EndpointNotifications* Client_{};
};

bool OpenEnumerator(ComPtr<IMMDeviceEnumerator>& Enumerator) noexcept {
    return SUCCEEDED(CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(Enumerator.Put())));
}

bool OpenVoiceDevice(IMMDeviceEnumerator* Enumerator, EDataFlow Flow,
                     const std::optional<std::string>& EndpointId,
                     ComPtr<IMMDevice>& Device) {
    if (!Enumerator) return false;
    if (!EndpointId) {
        return SUCCEEDED(Enumerator->GetDefaultAudioEndpoint(
            Flow, eCommunications, Device.Put()));
    }
    const auto Wide = Utf8ToWide(*EndpointId);
    if (!Wide || FAILED(Enumerator->GetDevice(Wide->c_str(), Device.Put()))) {
        return false;
    }
    DWORD State{};
    return SUCCEEDED(Device->GetState(&State)) &&
        (State & DEVICE_STATE_ACTIVE) != 0;
}

bool ActivateAudioClient(IMMDevice* Device,
                         ComPtr<IAudioClient>& Client) noexcept {
    return Device && SUCCEEDED(Device->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(Client.Put())));
}

std::optional<DeskLinkVirtualAudioEndpointKind> DeskLinkEndpointKind(
    IMMDevice* Device) noexcept {
    if (!Device) return std::nullopt;
    ComPtr<IPropertyStore> Properties;
    if (FAILED(Device->OpenPropertyStore(STGM_READ, Properties.Put()))) {
        return std::nullopt;
    }
    PROPVARIANT Value;
    PropVariantInit(&Value);
    const auto Result = Properties->GetValue(
        kDeskLinkEndpointKindProperty, &Value);
    std::optional<DeskLinkVirtualAudioEndpointKind> Kind;
    if (SUCCEEDED(Result) && Value.vt == VT_UI4 &&
        Value.ulVal <= static_cast<std::uint32_t>(
            DeskLinkVirtualAudioEndpointKind::RemoteMicrophone)) {
        Kind = static_cast<DeskLinkVirtualAudioEndpointKind>(Value.ulVal);
    }
    PropVariantClear(&Value);
    return Kind;
}

bool OpenDeskLinkEndpoint(
    IMMDeviceEnumerator* Enumerator, EDataFlow Flow,
    DeskLinkVirtualAudioEndpointKind ExpectedKind,
    ComPtr<IMMDevice>& Device) noexcept {
    if (!Enumerator) return false;
    ComPtr<IMMDeviceCollection> Devices;
    if (FAILED(Enumerator->EnumAudioEndpoints(
            Flow, DEVICE_STATE_ACTIVE, Devices.Put()))) {
        return false;
    }
    UINT Count{};
    if (FAILED(Devices->GetCount(&Count))) return false;
    Count = std::min(Count, UINT{128});
    bool Found{};
    for (UINT Index = 0; Index < Count; ++Index) {
        ComPtr<IMMDevice> Candidate;
        if (FAILED(Devices->Item(Index, Candidate.Put())) ||
            DeskLinkEndpointKind(Candidate.Get()) != ExpectedKind) {
            continue;
        }
        if (Found) return false;
        auto* Raw = Candidate.Get();
        Raw->AddRef();
        *Device.Put() = Raw;
        Found = true;
    }
    return Found;
}

struct DeskLinkEndpointCounts {
    std::uint32_t Feed{};
    std::uint32_t Capture{};
    std::uint32_t ActiveFeed{};
    std::uint32_t ActiveCapture{};
};

DeskLinkEndpointCounts CountDeskLinkEndpoints(
    IMMDeviceEnumerator* Enumerator) noexcept {
    DeskLinkEndpointCounts Result;
    if (!Enumerator) return Result;
    for (const auto Flow : {eRender, eCapture}) {
        ComPtr<IMMDeviceCollection> Devices;
        if (FAILED(Enumerator->EnumAudioEndpoints(
                Flow, DEVICE_STATEMASK_ALL, Devices.Put()))) {
            continue;
        }
        UINT Count{};
        if (FAILED(Devices->GetCount(&Count))) continue;
        Count = std::min(Count, UINT{128});
        for (UINT Index = 0; Index < Count; ++Index) {
            ComPtr<IMMDevice> Device;
            DWORD State{};
            if (FAILED(Devices->Item(Index, Device.Put())) ||
                FAILED(Device->GetState(&State))) {
                continue;
            }
            const auto Kind = DeskLinkEndpointKind(Device.Get());
            if (Flow == eRender &&
                Kind == DeskLinkVirtualAudioEndpointKind::MicrophoneFeed) {
                ++Result.Feed;
                if ((State & DEVICE_STATE_ACTIVE) != 0) ++Result.ActiveFeed;
            } else if (Flow == eCapture &&
                Kind == DeskLinkVirtualAudioEndpointKind::RemoteMicrophone) {
                ++Result.Capture;
                if ((State & DEVICE_STATE_ACTIVE) != 0) ++Result.ActiveCapture;
            }
        }
    }
    return Result;
}

} // namespace

Win32VirtualMicrophoneComponentState
GetWin32VirtualMicrophoneComponentState() {
    ComApartment Apartment;
    ComPtr<IMMDeviceEnumerator> Enumerator;
    if (!Apartment.Ready() || !OpenEnumerator(Enumerator)) {
        return Win32VirtualMicrophoneComponentState::NeedsRepair;
    }
    const auto Counts = CountDeskLinkEndpoints(Enumerator.Get());
    if (Counts.Feed == 0 && Counts.Capture == 0) {
        return Win32VirtualMicrophoneComponentState::NotInstalled;
    }
    if (Counts.Feed == 1 && Counts.Capture == 1 &&
        Counts.ActiveFeed == 1 && Counts.ActiveCapture == 1) {
        return Win32VirtualMicrophoneComponentState::Ready;
    }
    return Win32VirtualMicrophoneComponentState::NeedsRepair;
}

std::vector<VoiceInputDevice> EnumerateWin32VoiceInputDevices() {
    std::vector<VoiceInputDevice> Result;
    ComApartment Apartment;
    ComPtr<IMMDeviceEnumerator> Enumerator;
    ComPtr<IMMDeviceCollection> Devices;
    if (!Apartment.Ready() || !OpenEnumerator(Enumerator) ||
        FAILED(Enumerator->EnumAudioEndpoints(
            eCapture, DEVICE_STATE_ACTIVE, Devices.Put()))) {
        return Result;
    }
    ComPtr<IMMDevice> Default;
    std::optional<std::wstring> DefaultId;
    if (SUCCEEDED(Enumerator->GetDefaultAudioEndpoint(
            eCapture, eCommunications, Default.Put()))) {
        DefaultId = DeviceId(Default.Get());
    }
    UINT Count{};
    if (FAILED(Devices->GetCount(&Count))) return Result;
    Count = std::min(Count, UINT{128});
    for (UINT Index = 0; Index < Count; ++Index) {
        ComPtr<IMMDevice> Device;
        ComPtr<IPropertyStore> Properties;
        if (FAILED(Devices->Item(Index, Device.Put())) ||
            FAILED(Device->OpenPropertyStore(STGM_READ, Properties.Put()))) {
            continue;
        }
        if (IsDeskLinkVirtualMicrophoneSource(
                DeskLinkEndpointKind(Device.Get()).value_or(
                    DeskLinkVirtualAudioEndpointKind::None))) {
            continue;
        }
        const auto WideId = DeviceId(Device.Get());
        if (!WideId) continue;
        PROPVARIANT Name;
        PropVariantInit(&Name);
        const auto NameResult = Properties->GetValue(
            PKEY_Device_FriendlyName, &Name);
        const auto Id = WideToUtf8(*WideId);
        const auto DisplayName = SUCCEEDED(NameResult) &&
            Name.vt == VT_LPWSTR && Name.pwszVal
            ? WideToUtf8(Name.pwszVal) : std::nullopt;
        PropVariantClear(&Name);
        if (!Id || !DisplayName) continue;
        Result.push_back(VoiceInputDevice{
            *Id, *DisplayName,
            DefaultId && _wcsicmp(DefaultId->c_str(), WideId->c_str()) == 0});
    }
    std::sort(Result.begin(), Result.end(),
              [](const auto& Left, const auto& Right) {
                  if (Left.IsDefaultCommunications !=
                      Right.IsDefaultCommunications) {
                      return Left.IsDefaultCommunications;
                  }
                  return Left.DisplayName < Right.DisplayName;
              });
    return Result;
}

struct Win32WasapiMicrophoneCapture::State {
    State(std::optional<std::string> OwnedEndpoint,
          Win32WasapiMicrophoneHandlers OwnedHandlers)
        : EndpointId(std::move(OwnedEndpoint)),
          Handlers(std::move(OwnedHandlers)) {}

    std::optional<std::string> EndpointId;
    Win32WasapiMicrophoneHandlers Handlers;
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
            StartComplete = true;
            StartSucceeded = Success;
        }
        IsRunning.store(Success);
        Started.notify_all();
    }

    bool Deliver(VoicePcmFrame Frame) noexcept {
        if (!Handlers.Frame) return false;
        try { return Handlers.Frame(std::move(Frame)); }
        catch (...) { return false; }
    }

    void Run() noexcept {
        ComApartment Apartment;
        MmcssRegistration Mmcss;
        ComPtr<IMMDeviceEnumerator> Enumerator;
        ComPtr<IMMDevice> Device;
        ComPtr<IAudioClient> AudioClient;
        ComPtr<IAudioCaptureClient> CaptureClient;
        NotificationRegistration Notifications;
        auto Format = VoiceWaveFormat();
        const DWORD Flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
            AUDCLNT_STREAMFLAGS_NOPERSIST;
        bool Initialized = Apartment.Ready() && OpenEnumerator(Enumerator) &&
            OpenVoiceDevice(Enumerator.Get(), eCapture, EndpointId, Device) &&
            !IsDeskLinkVirtualMicrophoneSource(
                DeskLinkEndpointKind(Device.Get()).value_or(
                    DeskLinkVirtualAudioEndpointKind::None)) &&
            ActivateAudioClient(Device.Get(), AudioClient);
        if (Initialized) {
            ApplyCommunicationsCategory(AudioClient.Get());
            const auto Id = DeviceId(Device.Get());
            auto* Notification = Id
                ? new (std::nothrow) EndpointNotifications(
                    EndpointEvent, eCapture, !EndpointId.has_value())
                : nullptr;
            if (Notification && Id) Notification->SetDeviceId(*Id);
            Initialized = Id && Notification &&
                Notifications.Start(Enumerator.Get(), Notification) &&
                SUCCEEDED(AudioClient->Initialize(
                    AUDCLNT_SHAREMODE_SHARED, Flags,
                    kVoiceBufferDuration, 0, &Format, nullptr)) &&
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
                "voice microphone initialization failed");
            return;
        }

        std::array<std::int16_t, kVoiceSamplesPerChannel> Pending{};
        std::size_t PendingFrames{};
        std::uint64_t PendingTimestampUs{};
        HANDLE Handles[]{StopEvent, EndpointEvent, AudioEvent};
        bool Failed{};
        while (!Failed) {
            const auto Wait = WaitForMultipleObjects(3, Handles, FALSE, INFINITE);
            if (Wait == WAIT_OBJECT_0) break;
            if (Wait == WAIT_OBJECT_0 + 1) {
                PublishFailure(Handlers.Failed,
                    Win32WasapiFailureKind::EndpointChanged,
                    EndpointId ? "selected microphone became unavailable"
                               : "default communications microphone changed");
                break;
            }
            if (Wait != WAIT_OBJECT_0 + 2) {
                PublishFailure(Handlers.Failed,
                    Win32WasapiFailureKind::EndpointUnavailable,
                    "voice microphone wait failed");
                break;
            }
            UINT32 PacketFrames{};
            while (!Failed) {
                if (FAILED(CaptureClient->GetNextPacketSize(&PacketFrames))) {
                    Failed = true;
                    break;
                }
                if (!PacketFrames) break;
                BYTE* Data{};
                DWORD PacketFlags{};
                UINT64 QpcPosition{};
                if (FAILED(CaptureClient->GetBuffer(
                        &Data, &PacketFrames, &PacketFlags,
                        nullptr, &QpcPosition)) ||
                    PacketFrames > kMaximumCaptureChunkFrames) {
                    Failed = true;
                    break;
                }
                const bool Silent =
                    (PacketFlags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                if (!Silent && !Data) {
                    CaptureClient->ReleaseBuffer(PacketFrames);
                    Failed = true;
                    break;
                }
                if ((PacketFlags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0) {
                    PendingFrames = 0;
                }
                const auto Timestamp =
                    (PacketFlags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0
                    ? SteadyTimestampUs()
                    : static_cast<std::uint64_t>(QpcPosition / 10u);
                const auto* Samples = reinterpret_cast<const std::int16_t*>(Data);
                for (UINT32 Index = 0; Index < PacketFrames; ++Index) {
                    if (PendingFrames == 0) PendingTimestampUs = Timestamp;
                    Pending[PendingFrames++] = Silent ? 0 : Samples[Index];
                    if (PendingFrames == Pending.size()) {
                        VoicePcmFrame Frame;
                        Frame.Samples = Pending;
                        Frame.CaptureTimestampUs = PendingTimestampUs;
                        PendingFrames = 0;
                        if (!Deliver(std::move(Frame))) {
                            Failed = true;
                            break;
                        }
                    }
                }
                if (FAILED(CaptureClient->ReleaseBuffer(PacketFrames))) {
                    Failed = true;
                }
            }
        }
        (void)AudioClient->Stop();
        IsRunning.store(false);
        if (Failed) {
            PublishFailure(Handlers.Failed,
                Win32WasapiFailureKind::EndpointUnavailable,
                "voice microphone capture failed");
        }
    }
};

Win32WasapiMicrophoneCapture::Win32WasapiMicrophoneCapture(
    std::optional<std::string> EndpointId,
    Win32WasapiMicrophoneHandlers Handlers)
    : State_(std::make_unique<State>(
          std::move(EndpointId), std::move(Handlers))) {}

Win32WasapiMicrophoneCapture::~Win32WasapiMicrophoneCapture() { Stop(); }

bool Win32WasapiMicrophoneCapture::Start() {
    if (State_->Thread.joinable()) Stop();
    State_->StopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    State_->EndpointEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    State_->AudioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!State_->StopEvent || !State_->EndpointEvent || !State_->AudioEvent) {
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

void Win32WasapiMicrophoneCapture::Stop() noexcept {
    if (State_->StopEvent) SetEvent(State_->StopEvent);
    if (State_->Thread.joinable() &&
        State_->Thread.get_id() != std::this_thread::get_id()) {
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

bool Win32WasapiMicrophoneCapture::Running() const noexcept {
    return State_->IsRunning.load();
}

struct Win32WasapiVoiceRenderer::State {
    explicit State(Win32WasapiVoiceRenderHandlers OwnedHandlers)
        : Handlers(std::move(OwnedHandlers)) {}
    Win32WasapiVoiceRenderHandlers Handlers;
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
    std::deque<VoicePcmFrame> Queue;
    std::size_t SampleOffset{};
    std::atomic_uint64_t UnderrunCount{};

    void FinishStart(bool Success) noexcept {
        {
            std::scoped_lock Lock(StartMutex);
            StartComplete = true;
            StartSucceeded = Success;
        }
        IsRunning.store(Success);
        Started.notify_all();
    }
    bool Fill(std::int16_t* Destination, std::size_t Samples) noexcept {
        std::fill_n(Destination, Samples, std::int16_t{0});
        std::scoped_lock Lock(QueueMutex);
        std::size_t Written{};
        while (Written < Samples && !Queue.empty()) {
            auto& Frame = Queue.front();
            const auto Available = Frame.Samples.size() - SampleOffset;
            const auto Count = std::min(Available, Samples - Written);
            std::copy_n(Frame.Samples.data() + SampleOffset, Count,
                        Destination + Written);
            Written += Count;
            SampleOffset += Count;
            if (SampleOffset == Frame.Samples.size()) {
                Queue.pop_front();
                SampleOffset = 0;
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
        NotificationRegistration Notifications;
        auto Format = VoiceWaveFormat();
        const DWORD Flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
            AUDCLNT_STREAMFLAGS_NOPERSIST;
        UINT32 BufferFrames{};
        BYTE* Initial{};
        bool Initialized = Apartment.Ready() && OpenEnumerator(Enumerator) &&
            OpenVoiceDevice(Enumerator.Get(), eRender, std::nullopt, Device) &&
            ActivateAudioClient(Device.Get(), AudioClient);
        if (Initialized) {
            ApplyCommunicationsCategory(AudioClient.Get());
            const auto Id = DeviceId(Device.Get());
            auto* Notification = Id
                ? new (std::nothrow) EndpointNotifications(
                    EndpointEvent, eRender, true)
                : nullptr;
            if (Notification && Id) Notification->SetDeviceId(*Id);
            Initialized = Id && Notification &&
                Notifications.Start(Enumerator.Get(), Notification) &&
                SUCCEEDED(AudioClient->Initialize(
                    AUDCLNT_SHAREMODE_SHARED, Flags,
                    kVoiceBufferDuration, 0, &Format, nullptr)) &&
                SUCCEEDED(AudioClient->SetEventHandle(AudioEvent)) &&
                SUCCEEDED(AudioClient->GetBufferSize(&BufferFrames)) &&
                BufferFrames != 0 &&
                SUCCEEDED(AudioClient->GetService(
                    __uuidof(IAudioRenderClient),
                    reinterpret_cast<void**>(RenderClient.Put()))) &&
                SUCCEEDED(RenderClient->GetBuffer(BufferFrames, &Initial));
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
                "voice communications renderer initialization failed");
            return;
        }
        HANDLE Handles[]{StopEvent, EndpointEvent, AudioEvent};
        while (true) {
            const auto Wait = WaitForMultipleObjects(3, Handles, FALSE, INFINITE);
            if (Wait == WAIT_OBJECT_0) break;
            if (Wait == WAIT_OBJECT_0 + 1) {
                PublishFailure(Handlers.Failed,
                    Win32WasapiFailureKind::EndpointChanged,
                    "voice communications output changed");
                break;
            }
            if (Wait != WAIT_OBJECT_0 + 2) break;
            UINT32 Padding{};
            if (FAILED(AudioClient->GetCurrentPadding(&Padding)) ||
                Padding > BufferFrames) break;
            const auto Available = BufferFrames - Padding;
            if (!Available) continue;
            BYTE* Buffer{};
            if (FAILED(RenderClient->GetBuffer(Available, &Buffer))) break;
            const auto HasAudio = Fill(
                reinterpret_cast<std::int16_t*>(Buffer), Available);
            if (!HasAudio) ++UnderrunCount;
            if (FAILED(RenderClient->ReleaseBuffer(
                    Available, HasAudio ? 0 : AUDCLNT_BUFFERFLAGS_SILENT))) {
                break;
            }
        }
        (void)AudioClient->Stop();
        IsRunning.store(false);
    }
};

Win32WasapiVoiceRenderer::Win32WasapiVoiceRenderer(
    Win32WasapiVoiceRenderHandlers Handlers)
    : State_(std::make_unique<State>(std::move(Handlers))) {}
Win32WasapiVoiceRenderer::~Win32WasapiVoiceRenderer() { Stop(); }

bool Win32WasapiVoiceRenderer::Start() {
    if (State_->Thread.joinable()) Stop();
    State_->StopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    State_->EndpointEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    State_->AudioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!State_->StopEvent || !State_->EndpointEvent || !State_->AudioEvent) {
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

bool Win32WasapiVoiceRenderer::Submit(VoicePcmFrame Frame) {
    if (!State_->IsRunning.load()) return false;
    std::scoped_lock Lock(State_->QueueMutex);
    if (State_->Queue.size() >= kMaximumVoiceRenderQueueFrames) return false;
    State_->Queue.push_back(std::move(Frame));
    return true;
}

void Win32WasapiVoiceRenderer::Reset() noexcept {
    std::scoped_lock Lock(State_->QueueMutex);
    State_->Queue.clear();
    State_->SampleOffset = 0;
}

void Win32WasapiVoiceRenderer::Stop() noexcept {
    if (State_->StopEvent) SetEvent(State_->StopEvent);
    if (State_->Thread.joinable() &&
        State_->Thread.get_id() != std::this_thread::get_id()) {
        State_->Thread.join();
    }
    State_->IsRunning.store(false);
    {
        std::scoped_lock Lock(State_->QueueMutex);
        State_->Queue.clear();
        State_->SampleOffset = 0;
    }
    if (State_->AudioEvent) CloseHandle(State_->AudioEvent);
    if (State_->EndpointEvent) CloseHandle(State_->EndpointEvent);
    if (State_->StopEvent) CloseHandle(State_->StopEvent);
    State_->AudioEvent = nullptr;
    State_->EndpointEvent = nullptr;
    State_->StopEvent = nullptr;
}

bool Win32WasapiVoiceRenderer::Running() const noexcept {
    return State_->IsRunning.load();
}
std::size_t Win32WasapiVoiceRenderer::QueuedFrames() const noexcept {
    std::scoped_lock Lock(State_->QueueMutex);
    return State_->Queue.size();
}
std::uint64_t Win32WasapiVoiceRenderer::Underruns() const noexcept {
    return State_->UnderrunCount.load();
}

struct Win32VirtualMicrophoneFeed::State {
    explicit State(Win32WasapiVoiceRenderHandlers OwnedHandlers)
        : Handlers(std::move(OwnedHandlers)) {}
    Win32WasapiVoiceRenderHandlers Handlers;
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
    std::deque<VoicePcmFrame> Queue;
    std::size_t SampleOffset{};
    std::atomic_uint64_t StaleDropCount{};
    std::atomic_uint64_t UnderrunCount{};

    void FinishStart(bool Success) noexcept {
        {
            std::scoped_lock Lock(StartMutex);
            StartComplete = true;
            StartSucceeded = Success;
        }
        IsRunning.store(Success);
        Started.notify_all();
    }

    bool Fill(std::int16_t* Destination, std::size_t Samples) noexcept {
        std::fill_n(Destination, Samples, std::int16_t{0});
        std::scoped_lock Lock(QueueMutex);
        std::size_t Written{};
        while (Written < Samples && !Queue.empty()) {
            auto& Frame = Queue.front();
            const auto Available = Frame.Samples.size() - SampleOffset;
            const auto Count = std::min(Available, Samples - Written);
            std::copy_n(Frame.Samples.data() + SampleOffset, Count,
                        Destination + Written);
            Written += Count;
            SampleOffset += Count;
            if (SampleOffset == Frame.Samples.size()) {
                Queue.pop_front();
                SampleOffset = 0;
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
        NotificationRegistration Notifications;
        auto Format = VoiceWaveFormat();
        const DWORD Flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
            AUDCLNT_STREAMFLAGS_NOPERSIST;
        UINT32 BufferFrames{};
        BYTE* Initial{};
        bool Initialized = Apartment.Ready() && OpenEnumerator(Enumerator) &&
            OpenDeskLinkEndpoint(
                Enumerator.Get(), eRender,
                DeskLinkVirtualAudioEndpointKind::MicrophoneFeed, Device) &&
            ActivateAudioClient(Device.Get(), AudioClient);
        if (Initialized) {
            ApplyCommunicationsCategory(AudioClient.Get());
            const auto Id = DeviceId(Device.Get());
            auto* Notification = Id
                ? new (std::nothrow) EndpointNotifications(
                    EndpointEvent, eRender, false)
                : nullptr;
            if (Notification && Id) Notification->SetDeviceId(*Id);
            Initialized = Id && Notification &&
                Notifications.Start(Enumerator.Get(), Notification) &&
                SUCCEEDED(AudioClient->Initialize(
                    AUDCLNT_SHAREMODE_SHARED, Flags,
                    kVoiceBufferDuration, 0, &Format, nullptr)) &&
                SUCCEEDED(AudioClient->SetEventHandle(AudioEvent)) &&
                SUCCEEDED(AudioClient->GetBufferSize(&BufferFrames)) &&
                BufferFrames != 0 &&
                SUCCEEDED(AudioClient->GetService(
                    __uuidof(IAudioRenderClient),
                    reinterpret_cast<void**>(RenderClient.Put()))) &&
                SUCCEEDED(RenderClient->GetBuffer(BufferFrames, &Initial));
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
                "DeskLink virtual microphone feed is unavailable");
            return;
        }

        HANDLE Handles[]{StopEvent, EndpointEvent, AudioEvent};
        bool Failed{};
        while (!Failed) {
            const auto Wait = WaitForMultipleObjects(3, Handles, FALSE, INFINITE);
            if (Wait == WAIT_OBJECT_0) break;
            if (Wait == WAIT_OBJECT_0 + 1) {
                PublishFailure(Handlers.Failed,
                    Win32WasapiFailureKind::EndpointChanged,
                    "DeskLink virtual microphone endpoint changed");
                break;
            }
            if (Wait != WAIT_OBJECT_0 + 2) break;
            UINT32 Padding{};
            if (FAILED(AudioClient->GetCurrentPadding(&Padding)) ||
                Padding > BufferFrames) break;
            const auto Available = BufferFrames - Padding;
            if (!Available) continue;
            BYTE* Buffer{};
            if (FAILED(RenderClient->GetBuffer(Available, &Buffer))) break;
            const auto HasAudio = Fill(
                reinterpret_cast<std::int16_t*>(Buffer), Available);
            if (!HasAudio) ++UnderrunCount;
            if (FAILED(RenderClient->ReleaseBuffer(
                    Available,
                    HasAudio ? 0 : AUDCLNT_BUFFERFLAGS_SILENT))) {
                Failed = true;
            }
        }
        (void)AudioClient->Stop();
        (void)AudioClient->Reset();
        IsRunning.store(false);
        {
            std::scoped_lock Lock(QueueMutex);
            Queue.clear();
            SampleOffset = 0;
        }
        if (Failed) {
            PublishFailure(Handlers.Failed,
                Win32WasapiFailureKind::EndpointUnavailable,
                "DeskLink virtual microphone feed failed");
        }
    }
};

Win32VirtualMicrophoneFeed::Win32VirtualMicrophoneFeed(
    Win32WasapiVoiceRenderHandlers Handlers)
    : State_(std::make_unique<State>(std::move(Handlers))) {}

Win32VirtualMicrophoneFeed::~Win32VirtualMicrophoneFeed() { Stop(); }

bool Win32VirtualMicrophoneFeed::Start() {
    if (State_->Thread.joinable()) Stop();
    State_->StopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    State_->EndpointEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    State_->AudioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!State_->StopEvent || !State_->EndpointEvent || !State_->AudioEvent) {
        Stop();
        return false;
    }
    {
        std::scoped_lock Lock(State_->StartMutex);
        State_->StartComplete = false;
        State_->StartSucceeded = false;
    }
    State_->StaleDropCount.store(0);
    State_->UnderrunCount.store(0);
    State_->Thread = std::thread([State = State_.get()] { State->Run(); });
    std::unique_lock Lock(State_->StartMutex);
    State_->Started.wait(Lock, [&] { return State_->StartComplete; });
    if (State_->StartSucceeded) return true;
    Lock.unlock();
    Stop();
    return false;
}

bool Win32VirtualMicrophoneFeed::Submit(VoicePcmFrame Frame) {
    if (!State_->IsRunning.load()) return false;
    std::scoped_lock Lock(State_->QueueMutex);
    while (State_->Queue.size() >= kMaximumVirtualMicrophoneQueueFrames) {
        State_->Queue.pop_front();
        State_->SampleOffset = 0;
        ++State_->StaleDropCount;
    }
    State_->Queue.push_back(std::move(Frame));
    return true;
}

void Win32VirtualMicrophoneFeed::Reset() noexcept {
    // Closing the render client is the driver-level authorization boundary:
    // its pin leaves RUN and the bridge must flush immediately to silence.
    Stop();
}

void Win32VirtualMicrophoneFeed::Stop() noexcept {
    if (State_->StopEvent) SetEvent(State_->StopEvent);
    if (State_->Thread.joinable() &&
        State_->Thread.get_id() != std::this_thread::get_id()) {
        State_->Thread.join();
    }
    State_->IsRunning.store(false);
    {
        std::scoped_lock Lock(State_->QueueMutex);
        State_->Queue.clear();
        State_->SampleOffset = 0;
    }
    if (State_->AudioEvent) CloseHandle(State_->AudioEvent);
    if (State_->EndpointEvent) CloseHandle(State_->EndpointEvent);
    if (State_->StopEvent) CloseHandle(State_->StopEvent);
    State_->AudioEvent = nullptr;
    State_->EndpointEvent = nullptr;
    State_->StopEvent = nullptr;
}

bool Win32VirtualMicrophoneFeed::Running() const noexcept {
    return State_->IsRunning.load();
}

std::size_t Win32VirtualMicrophoneFeed::QueuedFrames() const noexcept {
    std::scoped_lock Lock(State_->QueueMutex);
    return State_->Queue.size();
}

std::uint64_t Win32VirtualMicrophoneFeed::StaleFramesDropped() const noexcept {
    return State_->StaleDropCount.load();
}

std::uint64_t Win32VirtualMicrophoneFeed::Underruns() const noexcept {
    return State_->UnderrunCount.load();
}

} // namespace desklink

#endif
