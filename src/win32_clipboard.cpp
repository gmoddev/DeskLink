#include "desklink/win32_clipboard.hpp"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <future>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>

namespace desklink {
namespace {

constexpr wchar_t kClipboardWindowClass[] =
    L"DeskLinkClipboardMessageWindowV1";
constexpr UINT kApplyRemoteMessage = WM_APP + 1;
constexpr std::size_t kMaximumClipboardWideCharacters =
    kMaximumClipboardTextBytes + 1;
constexpr std::size_t kClipboardOpenAttempts = 5;

void PublishFailure(const Win32ClipboardHandlers& Handlers,
                    std::string Message) noexcept {
    if (!Handlers.Failed) return;
    try {
        Handlers.Failed(std::move(Message));
    } catch (...) {
    }
}

[[nodiscard]] bool OpenClipboardBounded(HWND Owner) noexcept {
    for (std::size_t Attempt = 0;
         Attempt < kClipboardOpenAttempts; ++Attempt) {
        if (OpenClipboard(Owner)) return true;
        if (Attempt + 1 < kClipboardOpenAttempts) Sleep(5);
    }
    return false;
}

[[nodiscard]] std::optional<std::string> ToUtf8(
    std::wstring_view Text) noexcept {
    if (Text.empty()) return std::string{};
    if (Text.size() > static_cast<std::size_t>(INT_MAX)) return std::nullopt;
    const auto SourceLength = static_cast<int>(Text.size());
    const auto Length = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, Text.data(), SourceLength,
        nullptr, 0, nullptr, nullptr);
    if (Length <= 0 ||
        static_cast<std::size_t>(Length) > kMaximumClipboardTextBytes) {
        return std::nullopt;
    }
    std::string Result(static_cast<std::size_t>(Length), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, Text.data(), SourceLength,
            Result.data(), Length, nullptr, nullptr) != Length) {
        return std::nullopt;
    }
    return Result;
}

[[nodiscard]] std::optional<std::wstring> ToWide(
    std::string_view Text) noexcept {
    if (Text.empty()) return std::wstring{};
    if (Text.size() > static_cast<std::size_t>(INT_MAX)) return std::nullopt;
    const auto SourceLength = static_cast<int>(Text.size());
    const auto Length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, Text.data(), SourceLength,
        nullptr, 0);
    if (Length <= 0 ||
        static_cast<std::size_t>(Length) >=
            kMaximumClipboardWideCharacters) {
        return std::nullopt;
    }
    std::wstring Result(static_cast<std::size_t>(Length), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, Text.data(), SourceLength,
            Result.data(), Length) != Length) {
        return std::nullopt;
    }
    return Result;
}

} // namespace

struct Win32ClipboardSynchronizer::State {
    explicit State(Win32ClipboardHandlers Value)
        : Handlers(std::move(Value)) {}

    [[nodiscard]] bool Start() {
        std::promise<bool> Promise;
        auto Future = Promise.get_future();
        {
            std::scoped_lock Lock(Mutex);
            if (Worker.joinable()) return Running;
            StopRequested = false;
            try {
                Worker = std::thread(
                    [this, Promise = std::move(Promise)]() mutable {
                        Run(std::move(Promise));
                    });
            } catch (...) {
                return false;
            }
        }
        const bool Started = Future.get();
        if (!Started && Worker.joinable()) Worker.join();
        return Started;
    }

    void Stop() noexcept {
        HWND WindowToClose{};
        {
            std::scoped_lock Lock(Mutex);
            StopRequested = true;
            WindowToClose = Window;
        }
        if (WindowToClose) PostMessageW(WindowToClose, WM_CLOSE, 0, 0);
        try {
            if (Worker.joinable() &&
                Worker.get_id() != std::this_thread::get_id()) {
                Worker.join();
            }
        } catch (...) {
        }
        std::scoped_lock Lock(Mutex);
        Window = nullptr;
        Running = false;
        RemoteQueue.clear();
        SuppressedText.reset();
        SuppressedSequence = 0;
    }

    [[nodiscard]] bool ApplyRemote(ClipboardTextMessage Message) noexcept {
        if (!IsValidClipboardTextMessage(Message)) return false;
        std::scoped_lock Lock(Mutex);
        if (!Running || StopRequested || !Window ||
            RemoteQueue.size() >= kMaximumPendingClipboardUpdates) {
            ++Statistics.RemoteRejected;
            return false;
        }
        RemoteQueue.push_back(std::move(Message));
        if (!PostMessageW(Window, kApplyRemoteMessage, 0, 0)) {
            RemoteQueue.pop_back();
            ++Statistics.RemoteRejected;
            return false;
        }
        ++Statistics.RemoteQueued;
        return true;
    }

    static LRESULT CALLBACK WindowProcedure(
        HWND Window, UINT Message, WPARAM WParam, LPARAM LParam) noexcept {
        State* Self = reinterpret_cast<State*>(
            GetWindowLongPtrW(Window, GWLP_USERDATA));
        if (Message == WM_NCCREATE) {
            const auto* Create = reinterpret_cast<CREATESTRUCTW*>(LParam);
            Self = static_cast<State*>(Create->lpCreateParams);
            SetWindowLongPtrW(
                Window, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(Self));
        }
        if (!Self) {
            return DefWindowProcW(Window, Message, WParam, LParam);
        }
        switch (Message) {
            case WM_CLIPBOARDUPDATE:
                Self->ObserveLocal();
                return 0;
            case kApplyRemoteMessage:
                Self->DrainRemote();
                return 0;
            case WM_CLOSE:
                DestroyWindow(Window);
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            default:
                return DefWindowProcW(Window, Message, WParam, LParam);
        }
    }

    void Run(std::promise<bool> Promise) noexcept {
        bool PromiseSet = false;
        const auto CompleteStart = [&](bool Started) {
            if (PromiseSet) return;
            PromiseSet = true;
            try {
                Promise.set_value(Started);
            } catch (...) {
            }
        };
        const auto Instance = GetModuleHandleW(nullptr);
        WNDCLASSW WindowClass{};
        WindowClass.lpfnWndProc = WindowProcedure;
        WindowClass.hInstance = Instance;
        WindowClass.lpszClassName = kClipboardWindowClass;
        if (!RegisterClassW(&WindowClass) &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            CompleteStart(false);
            return;
        }
        const auto Created = CreateWindowExW(
            0, kClipboardWindowClass, L"", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, Instance, this);
        if (!Created || !AddClipboardFormatListener(Created)) {
            if (Created) DestroyWindow(Created);
            CompleteStart(false);
            return;
        }
        {
            std::scoped_lock Lock(Mutex);
            Window = Created;
            Running = true;
        }
        CompleteStart(true);
        if (LocalPublishing.load()) {
            PostMessageW(Created, WM_CLIPBOARDUPDATE, 0, 0);
        }
        MSG Message{};
        while (GetMessageW(&Message, nullptr, 0, 0) > 0) {
            TranslateMessage(&Message);
            DispatchMessageW(&Message);
        }
        RemoveClipboardFormatListener(Created);
        {
            std::scoped_lock Lock(Mutex);
            Window = nullptr;
            Running = false;
        }
        CompleteStart(false);
    }

    void ObserveLocal() noexcept {
        if (!LocalPublishing.load()) return;
        HWND Owner{};
        {
            std::scoped_lock Lock(Mutex);
            Owner = Window;
            ++Statistics.LocalObserved;
        }
        if (!Owner || !OpenClipboardBounded(Owner)) {
            PublishFailure(Handlers, "clipboard was busy during bounded read");
            return;
        }
        std::optional<std::string> Text;
        if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
            const auto Handle = GetClipboardData(CF_UNICODETEXT);
            const auto Bytes = Handle ? GlobalSize(Handle) : 0;
            const auto* Data = Handle
                ? static_cast<const wchar_t*>(GlobalLock(Handle)) : nullptr;
            if (Data && Bytes >= sizeof(wchar_t)) {
                const auto Characters = std::min<std::size_t>(
                    Bytes / sizeof(wchar_t),
                    kMaximumClipboardWideCharacters);
                const auto* End = std::find(Data, Data + Characters, L'\0');
                if (End != Data + Characters) {
                    Text = ToUtf8(std::wstring_view(
                        Data, static_cast<std::size_t>(End - Data)));
                }
            }
            if (Data) GlobalUnlock(Handle);
        }
        CloseClipboard();
        if (!Text) return;

        const auto Sequence = GetClipboardSequenceNumber();
        {
            std::scoped_lock Lock(Mutex);
            if (SuppressedText && SuppressedSequence == Sequence &&
                *SuppressedText == *Text) {
                // Windows may emit duplicate notifications for one sequence.
                // Retain the marker until a genuinely new sequence appears.
                ++Statistics.LoopsSuppressed;
                return;
            }
            if (SuppressedSequence != Sequence) {
                SuppressedText.reset();
                SuppressedSequence = 0;
            }
        }

        bool Published = false;
        try {
            Published = Handlers.LocalText &&
                Handlers.LocalText(std::move(*Text));
        } catch (...) {
            Published = false;
        }
        std::scoped_lock Lock(Mutex);
        if (Published) ++Statistics.LocalPublished;
        else ++Statistics.LocalRejected;
    }

    void DrainRemote() noexcept {
        for (;;) {
            ClipboardTextMessage Message;
            {
                std::scoped_lock Lock(Mutex);
                if (RemoteQueue.empty()) return;
                Message = std::move(RemoteQueue.front());
                RemoteQueue.pop_front();
            }
            if (!WriteRemote(Message.Text)) {
                std::scoped_lock Lock(Mutex);
                ++Statistics.RemoteRejected;
            } else {
                std::scoped_lock Lock(Mutex);
                ++Statistics.RemoteApplied;
            }
        }
    }

    [[nodiscard]] bool WriteRemote(const std::string& Text) noexcept {
        const auto Wide = ToWide(Text);
        if (!Wide) return false;
        const auto Bytes = (Wide->size() + 1) * sizeof(wchar_t);
        const auto Memory = GlobalAlloc(GMEM_MOVEABLE, Bytes);
        if (!Memory) return false;
        const auto Buffer = static_cast<wchar_t*>(GlobalLock(Memory));
        if (!Buffer) {
            GlobalFree(Memory);
            return false;
        }
        if (!Wide->empty()) {
            std::memcpy(Buffer, Wide->data(), Wide->size() * sizeof(wchar_t));
        }
        Buffer[Wide->size()] = L'\0';
        GlobalUnlock(Memory);

        HWND Owner{};
        {
            std::scoped_lock Lock(Mutex);
            Owner = Window;
        }
        if (!Owner || !OpenClipboardBounded(Owner)) {
            GlobalFree(Memory);
            PublishFailure(Handlers, "clipboard was busy during bounded write");
            return false;
        }
        const bool Emptied = EmptyClipboard() != FALSE;
        const bool Stored = Emptied &&
            SetClipboardData(CF_UNICODETEXT, Memory) != nullptr;
        CloseClipboard();
        if (!Stored) {
            GlobalFree(Memory);
            PublishFailure(Handlers, "clipboard text write failed");
            return false;
        }
        {
            std::scoped_lock Lock(Mutex);
            SuppressedText = Text;
            SuppressedSequence = GetClipboardSequenceNumber();
        }
        return true;
    }

    Win32ClipboardHandlers Handlers;
    mutable std::mutex Mutex;
    std::thread Worker;
    HWND Window{};
    std::deque<ClipboardTextMessage> RemoteQueue;
    std::optional<std::string> SuppressedText;
    DWORD SuppressedSequence{};
    Win32ClipboardStats Statistics;
    std::atomic_bool LocalPublishing{};
    bool Running{};
    bool StopRequested{};
};

Win32ClipboardSynchronizer::Win32ClipboardSynchronizer(
    Win32ClipboardHandlers Handlers)
    : State_(std::make_unique<State>(std::move(Handlers))) {}

Win32ClipboardSynchronizer::~Win32ClipboardSynchronizer() { Stop(); }

bool Win32ClipboardSynchronizer::Start() { return State_->Start(); }

void Win32ClipboardSynchronizer::Stop() noexcept { State_->Stop(); }

void Win32ClipboardSynchronizer::SetLocalPublishing(bool Enabled) noexcept {
    const bool WasEnabled = State_->LocalPublishing.exchange(Enabled);
    if (!Enabled || WasEnabled) return;
    HWND Window{};
    {
        std::scoped_lock Lock(State_->Mutex);
        Window = State_->Window;
    }
    if (Window) PostMessageW(Window, WM_CLIPBOARDUPDATE, 0, 0);
}

bool Win32ClipboardSynchronizer::ApplyRemote(
    ClipboardTextMessage Message) noexcept {
    return State_->ApplyRemote(std::move(Message));
}

bool Win32ClipboardSynchronizer::Running() const noexcept {
    std::scoped_lock Lock(State_->Mutex);
    return State_->Running;
}

Win32ClipboardStats Win32ClipboardSynchronizer::Stats() const noexcept {
    std::scoped_lock Lock(State_->Mutex);
    return State_->Statistics;
}

} // namespace desklink

#endif
