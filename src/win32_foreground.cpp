#include "desklink/win32_foreground.hpp"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace desklink {
namespace {

constexpr UINT kStopForegroundMonitor = WM_APP + 0x61u;
constexpr DWORD kMaximumImagePathCharacters = 32'768u;

std::atomic<Win32ForegroundMonitor::State*> ActiveMonitor{};

class UniqueHandle final {
public:
    explicit UniqueHandle(HANDLE Handle = nullptr) noexcept : Handle_(Handle) {}
    ~UniqueHandle() {
        if (Handle_) CloseHandle(Handle_);
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    [[nodiscard]] HANDLE Get() const noexcept { return Handle_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return Handle_ != nullptr;
    }

private:
    HANDLE Handle_{};
};

std::optional<std::string> ToUtf8(std::wstring_view Value) noexcept {
    if (Value.empty() ||
        Value.size() > kMaximumExecutableNameBytes) {
        return std::nullopt;
    }
    const auto Size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, Value.data(),
        static_cast<int>(Value.size()), nullptr, 0, nullptr, nullptr);
    if (Size <= 0 ||
        static_cast<std::size_t>(Size) > kMaximumExecutableNameBytes) {
        return std::nullopt;
    }
    std::string Result(static_cast<std::size_t>(Size), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, Value.data(),
            static_cast<int>(Value.size()), Result.data(), Size,
            nullptr, nullptr) != Size) {
        return std::nullopt;
    }
    return Result;
}

bool IsFullscreen(HWND Window) noexcept {
    RECT WindowRectangle{};
    if (!GetWindowRect(Window, &WindowRectangle)) return false;
    const auto Monitor = MonitorFromWindow(Window, MONITOR_DEFAULTTONULL);
    if (!Monitor) return false;
    MONITORINFO Information{};
    Information.cbSize = sizeof(Information);
    if (!GetMonitorInfoW(Monitor, &Information)) return false;
    return WindowRectangle.left <= Information.rcMonitor.left &&
           WindowRectangle.top <= Information.rcMonitor.top &&
           WindowRectangle.right >= Information.rcMonitor.right &&
           WindowRectangle.bottom >= Information.rcMonitor.bottom;
}

} // namespace

struct Win32ForegroundMonitor::State {
    explicit State(Win32ForegroundHandlers OwnedHandlers)
        : Handlers(std::move(OwnedHandlers)) {}

    Win32ForegroundHandlers Handlers;
    std::thread Thread;
    std::mutex StartMutex;
    std::condition_variable Started;
    DWORD ThreadId{};
    HWINEVENTHOOK Hook{};
    bool StartComplete{};
    bool StartSucceeded{};

    void Publish(HWND Window) noexcept {
        if (!Handlers.Changed) return;
        try {
            Handlers.Changed(ReadWin32ForegroundWindow(
                static_cast<std::uint64_t>(
                    reinterpret_cast<std::uintptr_t>(Window))));
        } catch (...) {
            if (Handlers.Failed) {
                try {
                    Handlers.Failed("foreground-window handler failed");
                } catch (...) {
                }
            }
        }
    }
};

namespace {

void CALLBACK ForegroundChanged(HWINEVENTHOOK Hook, DWORD Event,
                                HWND Window, LONG, LONG, DWORD, DWORD) {
    auto* State = ActiveMonitor.load();
    if (!State || State->Hook != Hook || Event != EVENT_SYSTEM_FOREGROUND) {
        return;
    }
    State->Publish(Window);
}

} // namespace

ForegroundWindowSnapshot ReadWin32ForegroundWindow(
    std::uint64_t WindowId) noexcept {
    const auto Window = WindowId == 0
        ? GetForegroundWindow()
        : reinterpret_cast<HWND>(static_cast<std::uintptr_t>(WindowId));
    ForegroundWindowSnapshot Snapshot;
    Snapshot.WindowId = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(Window));
    if (!Window || !IsWindow(Window)) return Snapshot;

    DWORD ProcessId{};
    if (GetWindowThreadProcessId(Window, &ProcessId) == 0 || ProcessId == 0) {
        return Snapshot;
    }
    UniqueHandle Process(OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ProcessId));
    if (!Process) return Snapshot;

    std::vector<wchar_t> ImagePath(kMaximumImagePathCharacters);
    DWORD Size = static_cast<DWORD>(ImagePath.size());
    if (!QueryFullProcessImageNameW(
            Process.Get(), 0, ImagePath.data(), &Size) || Size == 0 ||
        Size >= ImagePath.size()) {
        return Snapshot;
    }
    const std::wstring_view Path(ImagePath.data(), Size);
    const auto Separator = Path.find_last_of(L"\\/");
    const auto Name = Separator == std::wstring_view::npos
        ? Path
        : Path.substr(Separator + 1);
    const auto Utf8Name = ToUtf8(Name);
    if (!Utf8Name) return Snapshot;

    Snapshot.ExecutableName = *Utf8Name;
    Snapshot.Fullscreen = IsFullscreen(Window);
    Snapshot.Inspectable = true;
    if (!IsValidForegroundWindowSnapshot(Snapshot)) {
        Snapshot = {};
        Snapshot.WindowId = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(Window));
    }
    return Snapshot;
}

Win32ForegroundMonitor::Win32ForegroundMonitor(
    Win32ForegroundHandlers Handlers)
    : State_(std::make_unique<State>(std::move(Handlers))) {}

Win32ForegroundMonitor::~Win32ForegroundMonitor() {
    Stop();
}

bool Win32ForegroundMonitor::Start() {
    if (State_->Thread.joinable()) return State_->StartSucceeded;
    {
        std::scoped_lock Lock(State_->StartMutex);
        State_->ThreadId = 0;
        State_->Hook = nullptr;
        State_->StartComplete = false;
        State_->StartSucceeded = false;
    }
    State_->Thread = std::thread([State = State_.get()] {
        State->ThreadId = GetCurrentThreadId();
        MSG Message{};
        PeekMessageW(&Message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

        Win32ForegroundMonitor::State* Expected = nullptr;
        bool Success = ActiveMonitor.compare_exchange_strong(Expected, State);
        if (Success) {
            State->Hook = SetWinEventHook(
                EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                ForegroundChanged, 0, 0,
                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
            Success = State->Hook != nullptr;
        }
        {
            std::scoped_lock Lock(State->StartMutex);
            State->StartSucceeded = Success;
            State->StartComplete = true;
        }
        State->Started.notify_all();

        if (Success) {
            State->Publish(GetForegroundWindow());
            while (GetMessageW(&Message, nullptr, 0, 0) > 0) {
                if (Message.message == kStopForegroundMonitor) break;
                TranslateMessage(&Message);
                DispatchMessageW(&Message);
            }
            UnhookWinEvent(State->Hook);
            State->Hook = nullptr;
        }
        auto* ExpectedState = State;
        ActiveMonitor.compare_exchange_strong(ExpectedState, nullptr);
    });

    std::unique_lock Lock(State_->StartMutex);
    State_->Started.wait(Lock, [&] { return State_->StartComplete; });
    if (!State_->StartSucceeded) {
        Lock.unlock();
        Stop();
        return false;
    }
    return true;
}

void Win32ForegroundMonitor::Stop() noexcept {
    if (!State_->Thread.joinable()) return;
    PostThreadMessageW(State_->ThreadId, kStopForegroundMonitor, 0, 0);
    if (GetCurrentThreadId() == State_->ThreadId) return;
    State_->Thread.join();
}

} // namespace desklink

#endif
