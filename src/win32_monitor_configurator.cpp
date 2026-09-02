#ifdef _WIN32

#include "desklink/win32_monitor_configurator.hpp"

#include "desklink/monitor_configurator.hpp"
#include "desklink/win32_display_topology.hpp"
#include "desklink/win32_roaming_settings.hpp"

#include <windowsx.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace desklink {
namespace {

constexpr wchar_t kConfiguratorClass[] = L"DeskLink.MonitorConfigurator.v1";
constexpr wchar_t kCanvasClass[] = L"DeskLink.MonitorCanvas.v1";
constexpr wchar_t kIdentifyClass[] = L"DeskLink.IdentifyOverlay.v1";
constexpr UINT kTopologyReadyMessage = WM_APP + 40;
constexpr UINT_PTR kIdentifyTimer = 1;
constexpr UINT kIdentifyDurationMilliseconds = 5'000;

enum ConfiguratorControlId : int {
    RefreshLayouts = 4100,
    IdentifyDisplays,
    SourceDisplay,
    SourceSide,
    SourceStart,
    SourceEnd,
    TargetDisplay,
    TargetSide,
    TargetStart,
    TargetEnd,
    Direction,
    AddRoute,
    UseSuggestion,
    RemoveRoute,
    RouteList,
    SaveConfiguration,
    CloseConfigurator,
};

struct IdentifyPayload {
    std::wstring Text;
    HFONT LargeFont{};
    HFONT SmallFont{};

    ~IdentifyPayload() {
        if (LargeFont) DeleteObject(LargeFont);
        if (SmallFont) DeleteObject(SmallFont);
    }
};

[[nodiscard]] std::wstring ToWide(std::string_view Value) {
    if (Value.empty()) return {};
    const auto Length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, Value.data(),
        static_cast<int>(Value.size()), nullptr, 0);
    if (Length <= 0) return L"Invalid UTF-8";
    std::wstring Result(static_cast<std::size_t>(Length), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, Value.data(),
            static_cast<int>(Value.size()), Result.data(), Length) != Length) {
        return L"Invalid UTF-8";
    }
    return Result;
}

[[nodiscard]] std::wstring FormatMachine(const MachineId& Machine) {
    constexpr wchar_t Digits[] = L"0123456789abcdef";
    std::wstring Result;
    Result.reserve(19);
    for (std::size_t Index = 0; Index < 6; ++Index) {
        if (Index != 0 && Index % 2 == 0) Result.push_back(L':');
        Result.push_back(Digits[(Machine[Index] >> 4u) & 0x0fu]);
        Result.push_back(Digits[Machine[Index] & 0x0fu]);
    }
    return Result;
}

[[nodiscard]] std::wstring TopologyStatusText(
    DisplayTopologyExchangeStatus Status) {
    switch (Status) {
        case DisplayTopologyExchangeStatus::Offline: return L"Offline";
        case DisplayTopologyExchangeStatus::Disabled: return L"Disabled";
        case DisplayTopologyExchangeStatus::CapabilityMissing:
            return L"Topology permission missing";
        case DisplayTopologyExchangeStatus::Synchronizing:
            return L"Synchronizing topology";
        case DisplayTopologyExchangeStatus::Ready: return L"Connected";
        case DisplayTopologyExchangeStatus::TimedOut: return L"Topology timed out";
        case DisplayTopologyExchangeStatus::Rejected: return L"Topology rejected";
    }
    return L"Invalid";
}

[[nodiscard]] const wchar_t* SideText(DisplayEdgeSide Side) noexcept {
    switch (Side) {
        case DisplayEdgeSide::Left: return L"Left";
        case DisplayEdgeSide::Top: return L"Top";
        case DisplayEdgeSide::Right: return L"Right";
        case DisplayEdgeSide::Bottom: return L"Bottom";
    }
    return L"Invalid";
}

[[nodiscard]] std::wstring RefreshText(std::uint32_t MilliHertz) {
    if (MilliHertz == 0) return L"unknown Hz";
    if (MilliHertz % 1'000u == 0) {
        return std::to_wstring(MilliHertz / 1'000u) + L" Hz";
    }
    const auto Tenths = (MilliHertz + 50u) / 100u;
    return std::to_wstring(Tenths / 10u) + L"." +
           std::to_wstring(Tenths % 10u) + L" Hz";
}

[[nodiscard]] std::wstring TileText(
    const MonitorCanvasTile& Tile, std::size_t Index) {
    if (!Tile.Online) {
        return std::to_wstring(Index + 1) + L" · Offline display\r\n" +
               ToWide(Tile.MachineName) + L" · unavailable";
    }
    std::wstring Result = std::to_wstring(Tile.PixelWidth) + L"×" +
        std::to_wstring(Tile.PixelHeight) + L" · " +
        RefreshText(Tile.RefreshMilliHertz) + L"\r\n" +
        std::to_wstring(Index + 1) + L" · " + ToWide(Tile.FriendlyName);
    if (Tile.Primary) Result += L" · Primary";
    Result += L"\r\n" + ToWide(Tile.MachineName);
    if (Tile.SizeEstimated) Result += L" · size estimated";
    if (!Tile.Local) {
        Result += Tile.PeerInputAllowed
            ? L"\r\nPeer → this PC input granted"
            : L"\r\nPeer → this PC input not granted";
    }
    return Result;
}

LRESULT CALLBACK IdentifyProcedure(
    HWND Window, UINT Message, WPARAM WParam, LPARAM LParam) {
    auto* Payload = reinterpret_cast<IdentifyPayload*>(
        GetWindowLongPtrW(Window, GWLP_USERDATA));
    if (Message == WM_NCCREATE) {
        const auto* Create = reinterpret_cast<const CREATESTRUCTW*>(LParam);
        Payload = static_cast<IdentifyPayload*>(Create->lpCreateParams);
        SetWindowLongPtrW(
            Window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(Payload));
    }
    switch (Message) {
        case WM_CREATE:
            SetTimer(Window, kIdentifyTimer, kIdentifyDurationMilliseconds, nullptr);
            return 0;
        case WM_TIMER:
            if (WParam == kIdentifyTimer) DestroyWindow(Window);
            return 0;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT Paint{};
            const auto Dc = BeginPaint(Window, &Paint);
            RECT Bounds{};
            GetClientRect(Window, &Bounds);
            const auto Background = CreateSolidBrush(RGB(20, 24, 32));
            FillRect(Dc, &Bounds, Background);
            DeleteObject(Background);
            SetBkMode(Dc, TRANSPARENT);
            SetTextColor(Dc, RGB(245, 248, 255));
            Bounds.left += 22;
            Bounds.top += 18;
            if (Payload) {
                const auto Separator = Payload->Text.find(L'\n');
                auto First = Payload->Text.substr(0, Separator);
                auto Rest = Separator == std::wstring::npos
                    ? std::wstring{}
                    : Payload->Text.substr(Separator + 1);
                SelectObject(Dc, Payload->LargeFont);
                DrawTextW(Dc, First.data(), static_cast<int>(First.size()),
                          &Bounds, DT_LEFT | DT_TOP | DT_SINGLELINE);
                Bounds.top += 58;
                SelectObject(Dc, Payload->SmallFont);
                DrawTextW(Dc, Rest.data(), static_cast<int>(Rest.size()),
                          &Bounds, DT_LEFT | DT_TOP | DT_NOPREFIX);
            }
            EndPaint(Window, &Paint);
            return 0;
        }
        case WM_NCDESTROY:
            SetWindowLongPtrW(Window, GWLP_USERDATA, 0);
            delete Payload;
            return 0;
        default:
            return DefWindowProcW(Window, Message, WParam, LParam);
    }
}

[[nodiscard]] bool RegisterIdentifyClass(HINSTANCE Instance) {
    WNDCLASSEXW Class{};
    Class.cbSize = sizeof(Class);
    Class.hInstance = Instance;
    Class.lpfnWndProc = IdentifyProcedure;
    Class.lpszClassName = kIdentifyClass;
    Class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    Class.hbrBackground = nullptr;
    return RegisterClassExW(&Class) != 0 ||
           GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

class ConfiguratorWindow final {
public:
    ConfiguratorWindow(
        HWND Owner, std::filesystem::path SettingsPath,
        Win32MonitorConfiguratorCallbacks Callbacks)
        : Owner_(Owner),
          Instance_(reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(
              Owner, GWLP_HINSTANCE))),
          Settings_(std::move(SettingsPath)),
          Callbacks_(std::move(Callbacks)) {}

    ~ConfiguratorWindow() {
        if (RefreshWorker_.joinable()) RefreshWorker_.join();
        if (Font_) DeleteObject(Font_);
        if (TitleFont_) DeleteObject(TitleFont_);
        if (Owner_) EnableWindow(Owner_, TRUE);
    }

    [[nodiscard]] bool Show() {
        if (!Settings_.Load()) {
            MessageBoxW(
                Owner_, L"DeskLink could not load monitor settings.",
                L"Arrange monitors", MB_OK | MB_ICONERROR);
            return false;
        }
        Configuration_ = Settings_.Current().value_or(RoamingConfiguration{});
        if (!BuildModel()) return false;
        if (!RegisterClasses()) return false;
        EnableWindow(Owner_, FALSE);
        Window_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME, kConfiguratorClass,
            L"DeskLink — Arrange monitors",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
            CW_USEDEFAULT, CW_USEDEFAULT, 1180, 790, Owner_, nullptr,
            Instance_, this);
        if (!Window_) {
            EnableWindow(Owner_, TRUE);
            return false;
        }
        ShowWindow(Window_, SW_SHOW);
        UpdateWindow(Window_);
        BeginRefresh();
        MSG Message{};
        while (IsWindow(Window_) && GetMessageW(&Message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(Window_, &Message)) {
                TranslateMessage(&Message);
                DispatchMessageW(&Message);
            }
        }
        if (Owner_) {
            EnableWindow(Owner_, TRUE);
            SetForegroundWindow(Owner_);
        }
        return Saved_;
    }

private:
    [[nodiscard]] bool RegisterClasses() {
        WNDCLASSEXW WindowClass{};
        WindowClass.cbSize = sizeof(WindowClass);
        WindowClass.hInstance = Instance_;
        WindowClass.lpfnWndProc = WindowProcedure;
        WindowClass.lpszClassName = kConfiguratorClass;
        WindowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        WindowClass.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
        WindowClass.hbrBackground =
            reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        if (!RegisterClassExW(&WindowClass) &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
        WNDCLASSEXW CanvasClass{};
        CanvasClass.cbSize = sizeof(CanvasClass);
        CanvasClass.hInstance = Instance_;
        CanvasClass.lpfnWndProc = CanvasProcedure;
        CanvasClass.lpszClassName = kCanvasClass;
        CanvasClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32646));
        CanvasClass.hbrBackground = nullptr;
        return RegisterClassExW(&CanvasClass) != 0 ||
               GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    static LRESULT CALLBACK WindowProcedure(
        HWND Window, UINT Message, WPARAM WParam, LPARAM LParam) {
        auto* Self = reinterpret_cast<ConfiguratorWindow*>(
            GetWindowLongPtrW(Window, GWLP_USERDATA));
        if (Message == WM_NCCREATE) {
            const auto* Create = reinterpret_cast<const CREATESTRUCTW*>(LParam);
            Self = static_cast<ConfiguratorWindow*>(Create->lpCreateParams);
            Self->Window_ = Window;
            SetWindowLongPtrW(
                Window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(Self));
        }
        return Self ? Self->HandleMessage(Message, WParam, LParam)
                    : DefWindowProcW(Window, Message, WParam, LParam);
    }

    static LRESULT CALLBACK CanvasProcedure(
        HWND Window, UINT Message, WPARAM WParam, LPARAM LParam) {
        auto* Self = reinterpret_cast<ConfiguratorWindow*>(
            GetWindowLongPtrW(Window, GWLP_USERDATA));
        if (Message == WM_NCCREATE) {
            const auto* Create = reinterpret_cast<const CREATESTRUCTW*>(LParam);
            Self = static_cast<ConfiguratorWindow*>(Create->lpCreateParams);
            SetWindowLongPtrW(
                Window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(Self));
        }
        if (!Self) return DefWindowProcW(Window, Message, WParam, LParam);
        switch (Message) {
            case WM_ERASEBKGND: return 1;
            case WM_PAINT: Self->PaintCanvas(); return 0;
            case WM_LBUTTONDOWN:
                Self->BeginDrag(GET_X_LPARAM(LParam), GET_Y_LPARAM(LParam));
                return 0;
            case WM_MOUSEMOVE:
                if ((WParam & MK_LBUTTON) != 0) {
                    Self->ContinueDrag(
                        GET_X_LPARAM(LParam), GET_Y_LPARAM(LParam));
                }
                return 0;
            case WM_LBUTTONUP:
                Self->EndDrag(GET_X_LPARAM(LParam), GET_Y_LPARAM(LParam));
                return 0;
            default:
                return DefWindowProcW(Window, Message, WParam, LParam);
        }
    }

    LRESULT HandleMessage(UINT Message, WPARAM WParam, LPARAM LParam) {
        switch (Message) {
            case WM_CREATE: return CreateControls() ? 0 : -1;
            case WM_COMMAND:
                HandleCommand(LOWORD(WParam));
                return 0;
            case kTopologyReadyMessage: {
                FinishRefresh();
                return 0;
            }
            case WM_CLOSE:
                Close();
                return 0;
            case WM_DESTROY:
                Window_ = nullptr;
                return 0;
            default:
                return DefWindowProcW(Window_, Message, WParam, LParam);
        }
    }

    HWND CreateControl(
        const wchar_t* ClassName, const wchar_t* Text, DWORD Style,
        int X, int Y, int Width, int Height, int Id = 0,
        DWORD ExtendedStyle = 0) {
        const auto Control = CreateWindowExW(
            ExtendedStyle, ClassName, Text, WS_CHILD | WS_VISIBLE | Style,
            X, Y, Width, Height, Window_,
            Id == 0 ? nullptr : reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(Id)), Instance_, nullptr);
        if (Control && Font_) {
            SendMessageW(
                Control, WM_SETFONT, reinterpret_cast<WPARAM>(Font_), TRUE);
        }
        return Control;
    }

    [[nodiscard]] bool CreateControls() {
        Font_ = CreateFontW(
            -16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        TitleFont_ = CreateFontW(
            -25, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        const auto Title = CreateControl(
            L"STATIC", L"Arrange monitors", SS_LEFT, 18, 12, 300, 34);
        if (Title && TitleFont_) {
            SendMessageW(
                Title, WM_SETFONT, reinterpret_cast<WPARAM>(TitleFont_), TRUE);
        }
        Status_ = CreateControl(
            L"STATIC", L"Loading local and authenticated peer layouts…",
            SS_LEFT, 335, 19, 470, 24);
        CreateControl(
            L"BUTTON", L"Refresh layouts", BS_PUSHBUTTON,
            825, 12, 145, 32, RefreshLayouts);
        CreateControl(
            L"BUTTON", L"Identify displays", BS_PUSHBUTTON,
            985, 12, 160, 32, IdentifyDisplays);
        Canvas_ = CreateWindowExW(
            WS_EX_CLIENTEDGE, kCanvasClass, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            18, 55, 785, 475, Window_, nullptr, Instance_, this);

        CreateControl(
            L"BUTTON", L"Edge connection", BS_GROUPBOX,
            815, 52, 330, 478);
        CreateControl(L"STATIC", L"From display", SS_LEFT, 832, 80, 105, 22);
        SourceDisplay_ = CreateControl(
            L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP,
            832, 103, 292, 180, SourceDisplay);
        SourceSide_ = CreateControl(
            L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP,
            832, 141, 126, 150, SourceSide);
        SourceStart_ = CreateControl(
            L"EDIT", L"0", ES_NUMBER | WS_TABSTOP,
            970, 141, 58, 26, SourceStart, WS_EX_CLIENTEDGE);
        SourceEnd_ = CreateControl(
            L"EDIT", L"100", ES_NUMBER | WS_TABSTOP,
            1038, 141, 58, 26, SourceEnd, WS_EX_CLIENTEDGE);
        CreateControl(
            L"STATIC", L"Side          Start %   End %",
            SS_LEFT, 832, 171, 270, 20);

        CreateControl(L"STATIC", L"To display", SS_LEFT, 832, 201, 105, 22);
        TargetDisplay_ = CreateControl(
            L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP,
            832, 224, 292, 180, TargetDisplay);
        TargetSide_ = CreateControl(
            L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP,
            832, 262, 126, 150, TargetSide);
        TargetStart_ = CreateControl(
            L"EDIT", L"0", ES_NUMBER | WS_TABSTOP,
            970, 262, 58, 26, TargetStart, WS_EX_CLIENTEDGE);
        TargetEnd_ = CreateControl(
            L"EDIT", L"100", ES_NUMBER | WS_TABSTOP,
            1038, 262, 58, 26, TargetEnd, WS_EX_CLIENTEDGE);
        CreateControl(
            L"STATIC", L"Side          Start %   End %",
            SS_LEFT, 832, 292, 270, 20);

        CreateControl(L"STATIC", L"Direction", SS_LEFT, 832, 322, 90, 22);
        Direction_ = CreateControl(
            L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP,
            925, 318, 199, 120, Direction);
        CreateControl(
            L"BUTTON", L"Add connection", BS_PUSHBUTTON,
            832, 360, 140, 32, AddRoute);
        SuggestionButton_ = CreateControl(
            L"BUTTON", L"Connect suggestion", BS_PUSHBUTTON,
            984, 360, 140, 32, UseSuggestion);
        SuggestionText_ = CreateControl(
            L"STATIC",
            L"Drag displays next to another PC to receive a connection suggestion.",
            SS_LEFT, 832, 402, 292, 58);
        CreateControl(
            L"STATIC",
            L"Connections default to both directions. Partial edges and one-way routes are advanced options above.",
            SS_LEFT, 832, 470, 292, 48);

        CreateControl(
            L"BUTTON", L"Saved connections", BS_GROUPBOX,
            18, 540, 960, 150);
        RouteList_ = CreateControl(
            L"LISTBOX", L"", LBS_NOTIFY | WS_VSCROLL | WS_TABSTOP,
            34, 568, 770, 105, RouteList, WS_EX_CLIENTEDGE);
        CreateControl(
            L"BUTTON", L"Remove selected", BS_PUSHBUTTON,
            820, 568, 140, 32, RemoveRoute);
        CreateControl(
            L"STATIC",
            L"Canvas position and physical size are visualization only; routing uses stable display identities and explicit edge segments.",
            SS_LEFT, 34, 704, 760, 38);
        CreateControl(
            L"BUTTON", L"Save", BS_DEFPUSHBUTTON,
            922, 704, 105, 34, SaveConfiguration);
        CreateControl(
            L"BUTTON", L"Close", BS_PUSHBUTTON,
            1040, 704, 105, 34, CloseConfigurator);

        for (const auto Side : {DisplayEdgeSide::Left, DisplayEdgeSide::Top,
                                DisplayEdgeSide::Right, DisplayEdgeSide::Bottom}) {
            SendMessageW(SourceSide_, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(SideText(Side)));
            SendMessageW(TargetSide_, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(SideText(Side)));
        }
        SendMessageW(SourceSide_, CB_SETCURSEL, 2, 0);
        SendMessageW(TargetSide_, CB_SETCURSEL, 0, 0);
        for (const auto* Text : {L"Both directions", L"From → To", L"To → From"}) {
            SendMessageW(Direction_, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(Text));
        }
        SendMessageW(Direction_, CB_SETCURSEL, 0, 0);
        UpdateControls();
        return Canvas_ && SourceDisplay_ && TargetDisplay_ && RouteList_ &&
               Status_;
    }

    void HandleCommand(int Id) {
        switch (Id) {
            case RefreshLayouts: BeginRefresh(); break;
            case IdentifyDisplays:
                if (!ShowWin32DisplayIdentification(Window_)) {
                    MessageBoxW(
                        Window_, L"Active local displays could not be identified.",
                        L"Identify displays", MB_OK | MB_ICONERROR);
                }
                break;
            case AddRoute: AddManualRoute(); break;
            case UseSuggestion: AcceptSuggestion(); break;
            case RemoveRoute: RemoveSelectedRoute(); break;
            case SaveConfiguration: Save(); break;
            case CloseConfigurator: Close(); break;
            default: break;
        }
    }

    [[nodiscard]] bool BuildModel() {
        const auto Model = BuildMonitorCanvasModel(Machines_, Configuration_);
        if (!Model) return false;
        Model_ = *Model;
        RecomputeSuggestion();
        return true;
    }

    void BeginRefresh() {
        if (!Callbacks_.GetTopologies) return;
        if (RefreshWorker_.joinable()) RefreshWorker_.join();
        {
            std::scoped_lock Lock(RefreshResultMutex_);
            PendingRefreshResult_.reset();
            RefreshFinished_ = false;
        }
        EnableWindow(GetDlgItem(Window_, RefreshLayouts), FALSE);
        SetWindowTextW(Status_, L"Refreshing authenticated layouts…");
        const auto Window = Window_;
        const auto Callback = Callbacks_.GetTopologies;
        RefreshWorker_ = std::jthread([this, Window, Callback] {
            auto Result = Callback();
            {
                std::scoped_lock Lock(RefreshResultMutex_);
                PendingRefreshResult_ = std::move(Result);
                RefreshFinished_ = true;
            }
            (void)PostMessageW(Window, kTopologyReadyMessage, 0, 0);
        });
    }

    void FinishRefresh() {
        if (RefreshWorker_.joinable()) RefreshWorker_.join();
        std::optional<ControlTopologyState> State;
        {
            std::scoped_lock Lock(RefreshResultMutex_);
            if (RefreshFinished_) {
                State = std::move(PendingRefreshResult_);
                RefreshFinished_ = false;
            }
        }
        EnableWindow(GetDlgItem(Window_, RefreshLayouts), TRUE);
        if (!State || !IsValidControlTopologyState(*State)) {
            SetWindowTextW(
                Status_, L"Layouts unavailable; saved offline displays remain visible.");
            return;
        }
        std::wstring StatusText = L"Layouts current";
        std::vector<MonitorCanvasMachine> Machines;
        Machines.reserve(State->Machines.size());
        for (auto& Entry : State->Machines) {
            std::string Name = Entry.Local ? "This PC" : "Peer ";
            if (!Entry.Local) {
                const auto Wide = FormatMachine(Entry.Machine);
                const auto Length = WideCharToMultiByte(
                    CP_UTF8, 0, Wide.data(), static_cast<int>(Wide.size()),
                    nullptr, 0, nullptr, nullptr);
                if (Length > 0) {
                    std::string Suffix(static_cast<std::size_t>(Length), '\0');
                    (void)WideCharToMultiByte(
                        CP_UTF8, 0, Wide.data(), static_cast<int>(Wide.size()),
                        Suffix.data(), Length, nullptr, nullptr);
                    Name += Suffix;
                }
            }
            Machines.push_back(MonitorCanvasMachine{
                Entry.Machine, std::move(Name), std::move(Entry.Topology),
                Entry.Status, Entry.Local, Entry.PeerInputAllowed});
            if (!Entry.Local) {
                StatusText += L" · " + FormatMachine(Entry.Machine) + L": " +
                    TopologyStatusText(Entry.Status);
            }
        }
        Machines_ = std::move(Machines);
        if (!BuildModel()) {
            SetWindowTextW(Status_, L"Received layouts were not usable.");
            return;
        }
        StatusText += L". Drag cards to approximate your desk.";
        SetWindowTextW(Status_, StatusText.c_str());
        UpdateControls();
        InvalidateRect(Canvas_, nullptr, FALSE);
    }

    void UpdateControls() {
        if (!SourceDisplay_ || !TargetDisplay_) return;
        SendMessageW(SourceDisplay_, CB_RESETCONTENT, 0, 0);
        SendMessageW(TargetDisplay_, CB_RESETCONTENT, 0, 0);
        for (std::size_t Index = 0; Index < Model_.Tiles.size(); ++Index) {
            const auto& Tile = Model_.Tiles[Index];
            std::wstring Text = std::to_wstring(Index + 1) + L" — " +
                ToWide(Tile.MachineName) + L" — " + ToWide(Tile.FriendlyName);
            if (!Tile.Online) Text += L" (offline)";
            const auto SourceIndex = SendMessageW(
                SourceDisplay_, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(Text.c_str()));
            const auto TargetIndex = SendMessageW(
                TargetDisplay_, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(Text.c_str()));
            SendMessageW(SourceDisplay_, CB_SETITEMDATA, SourceIndex, Index);
            SendMessageW(TargetDisplay_, CB_SETITEMDATA, TargetIndex, Index);
        }
        if (!Model_.Tiles.empty()) {
            SendMessageW(SourceDisplay_, CB_SETCURSEL, 0, 0);
            SendMessageW(TargetDisplay_, CB_SETCURSEL,
                         Model_.Tiles.size() > 1 ? 1 : 0, 0);
        }
        SendMessageW(RouteList_, LB_RESETCONTENT, 0, 0);
        std::vector<MachineDisplayTopology> Topologies;
        for (const auto& Machine : Machines_) {
            if (Machine.Topology) {
                Topologies.push_back({Machine.Machine, &*Machine.Topology});
            }
        }
        for (const auto& Link : Configuration_.Links) {
            const auto Resolution = ResolveRoamingLink(Link, Topologies);
            const wchar_t* Direction =
                Link.Direction == RoamingDirectionMode::Bidirectional
                    ? L" ↔ "
                    : Link.Direction == RoamingDirectionMode::AToB
                        ? L" → "
                        : L" ← ";
            std::wstring Text = FormatMachine(Link.EndpointA.Machine) + L" " +
                SideText(Link.EndpointA.Side) + Direction +
                FormatMachine(Link.EndpointB.Machine) + L" " +
                SideText(Link.EndpointB.Side) + L" — " +
                (Link.Enabled
                    ? Resolution.Ready() ? L"Ready" : L"Display offline/missing"
                    : L"Disabled");
            SendMessageW(RouteList_, LB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(Text.c_str()));
        }
        EnableWindow(SuggestionButton_, Suggestion_.has_value());
        if (Suggestion_) {
            const auto& A = Model_.Tiles[Suggestion_->TileA];
            const auto& B = Model_.Tiles[Suggestion_->TileB];
            const auto Text = L"Suggestion: connect " + ToWide(A.MachineName) +
                L" " + SideText(Suggestion_->Link.EndpointA.Side) + L" ↔ " +
                ToWide(B.MachineName) + L" " +
                SideText(Suggestion_->Link.EndpointB.Side) +
                L" (both directions).";
            SetWindowTextW(SuggestionText_, Text.c_str());
        } else {
            SetWindowTextW(
                SuggestionText_,
                L"Drag displays next to another PC to receive a connection suggestion.");
        }
    }

    [[nodiscard]] std::optional<std::size_t> SelectedTile(HWND Combo) const {
        const auto Selection = SendMessageW(Combo, CB_GETCURSEL, 0, 0);
        if (Selection == CB_ERR) return std::nullopt;
        const auto Data = SendMessageW(Combo, CB_GETITEMDATA, Selection, 0);
        if (Data == CB_ERR || static_cast<std::size_t>(Data) >= Model_.Tiles.size()) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(Data);
    }

    [[nodiscard]] std::optional<std::uint16_t> ReadPercent(HWND Edit) const {
        wchar_t Text[4]{};
        const auto Length = GetWindowTextW(Edit, Text, static_cast<int>(std::size(Text)));
        if (Length <= 0 || Length > 3) return std::nullopt;
        unsigned Value = 0;
        for (int Index = 0; Index < Length; ++Index) {
            if (Text[Index] < L'0' || Text[Index] > L'9') return std::nullopt;
            Value = Value * 10u + static_cast<unsigned>(Text[Index] - L'0');
        }
        if (Value > 100u) return std::nullopt;
        return static_cast<std::uint16_t>(Value * 100u);
    }

    [[nodiscard]] std::optional<RoamingLink> ReadManualRoute() const {
        const auto Source = SelectedTile(SourceDisplay_);
        const auto Target = SelectedTile(TargetDisplay_);
        const auto AStart = ReadPercent(SourceStart_);
        const auto AEnd = ReadPercent(SourceEnd_);
        const auto BStart = ReadPercent(TargetStart_);
        const auto BEnd = ReadPercent(TargetEnd_);
        const auto SideA = SendMessageW(SourceSide_, CB_GETCURSEL, 0, 0);
        const auto SideB = SendMessageW(TargetSide_, CB_GETCURSEL, 0, 0);
        const auto Direction = SendMessageW(Direction_, CB_GETCURSEL, 0, 0);
        if (!Source || !Target || *Source == *Target || !AStart || !AEnd ||
            !BStart || !BEnd || *AStart >= *AEnd || *BStart >= *BEnd ||
            SideA < 0 || SideA > 3 || SideB < 0 || SideB > 3 ||
            Direction < 0 || Direction > 2) {
            return std::nullopt;
        }
        const auto& A = Model_.Tiles[*Source];
        const auto& B = Model_.Tiles[*Target];
        if (A.Machine == B.Machine) return std::nullopt;
        const std::array Sides{
            DisplayEdgeSide::Left, DisplayEdgeSide::Top,
            DisplayEdgeSide::Right, DisplayEdgeSide::Bottom};
        const std::array Directions{
            RoamingDirectionMode::Bidirectional,
            RoamingDirectionMode::AToB,
            RoamingDirectionMode::BToA};
        RoamingLink Link;
        Link.EndpointA = {
            A.Machine, A.StableDisplayIdentity,
            Sides[static_cast<std::size_t>(SideA)], *AStart, *AEnd};
        Link.EndpointB = {
            B.Machine, B.StableDisplayIdentity,
            Sides[static_cast<std::size_t>(SideB)], *BStart, *BEnd};
        Link.Direction = Directions[static_cast<std::size_t>(Direction)];
        return Link;
    }

    void AddManualRoute() {
        const auto Link = ReadManualRoute();
        if (!Link) {
            MessageBoxW(
                Window_,
                L"Choose displays on different PCs and valid edge segments. Start must be less than end.",
                L"Add connection", MB_OK | MB_ICONERROR);
            return;
        }
        auto Candidate = Configuration_;
        Candidate.Links.push_back(*Link);
        if (!IsValidRoamingConfiguration(Candidate)) {
            MessageBoxW(
                Window_,
                L"That connection duplicates or overlaps an existing active source edge.",
                L"Add connection", MB_OK | MB_ICONERROR);
            return;
        }
        Configuration_ = std::move(Candidate);
        Dirty_ = true;
        UpdateControls();
    }

    void AcceptSuggestion() {
        if (!Suggestion_) return;
        if (MessageBoxW(
                Window_,
                L"Create the suggested connection in both directions? No input is enabled until Phase 4 roaming is explicitly activated.",
                L"Connect displays", MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return;
        }
        auto Candidate = Configuration_;
        Candidate.Links.push_back(Suggestion_->Link);
        if (!IsValidRoamingConfiguration(Candidate)) {
            MessageBoxW(
                Window_, L"The suggested edge conflicts with an existing connection.",
                L"Connect displays", MB_OK | MB_ICONERROR);
            return;
        }
        Configuration_ = std::move(Candidate);
        Dirty_ = true;
        UpdateControls();
    }

    void RemoveSelectedRoute() {
        const auto Selection = SendMessageW(RouteList_, LB_GETCURSEL, 0, 0);
        if (Selection == LB_ERR ||
            static_cast<std::size_t>(Selection) >= Configuration_.Links.size()) {
            return;
        }
        Configuration_.Links.erase(
            Configuration_.Links.begin() + Selection);
        Dirty_ = true;
        UpdateControls();
    }

    void Save() {
        auto Candidate = Configuration_;
        Candidate.CanvasLayout.clear();
        if (Model_.Tiles.size() > kMaximumCanvasPlacements) {
            MessageBoxW(
                Window_, L"Too many display placements are present to save safely.",
                L"Save monitor layout", MB_OK | MB_ICONERROR);
            return;
        }
        for (const auto& Tile : Model_.Tiles) {
            Candidate.CanvasLayout.push_back({
                Tile.Machine, Tile.StableDisplayIdentity,
                Tile.Rect.X, Tile.Rect.Y});
        }
        if (!IsValidRoamingConfiguration(Candidate)) {
            MessageBoxW(
                Window_, L"The candidate monitor graph is invalid and was not saved.",
                L"Save monitor layout", MB_OK | MB_ICONERROR);
            return;
        }
        if (Callbacks_.EnsureLocal && !Callbacks_.EnsureLocal()) {
            MessageBoxW(
                Window_,
                L"DeskLink could not confirm that input returned Local. The active configuration was not changed.",
                L"Save monitor layout", MB_OK | MB_ICONERROR);
            return;
        }
        if (!Settings_.Save(Candidate)) {
            MessageBoxW(
                Window_, L"The monitor layout could not be replaced atomically.",
                L"Save monitor layout", MB_OK | MB_ICONERROR);
            return;
        }
        Configuration_ = std::move(Candidate);
        Dirty_ = false;
        Saved_ = true;
        SetWindowTextW(Status_, L"Monitor layout saved. Edge roaming remains disabled.");
    }

    void Close() {
        if (Dirty_ && MessageBoxW(
                Window_, L"Discard unsaved monitor layout changes?",
                L"Arrange monitors", MB_YESNO | MB_ICONQUESTION |
                MB_DEFBUTTON2) != IDYES) {
            return;
        }
        DestroyWindow(Window_);
    }

    void RecomputeSuggestion() {
        Suggestion_.reset();
        for (std::size_t A = 0; A < Model_.Tiles.size(); ++A) {
            for (std::size_t B = A + 1; B < Model_.Tiles.size(); ++B) {
                auto Candidate = BuildRoamingLinkSuggestion(Model_.Tiles, A, B);
                if (Candidate && (!Suggestion_ ||
                        Candidate->EdgeGapPixels < Suggestion_->EdgeGapPixels)) {
                    Suggestion_ = std::move(Candidate);
                }
            }
        }
    }

    void CalculateView() {
        if (Model_.Tiles.empty()) {
            ViewOriginX_ = 0;
            ViewOriginY_ = 0;
            ViewScale_ = 1.0;
            return;
        }
        auto Left = Model_.Tiles.front().Rect.X;
        auto Top = Model_.Tiles.front().Rect.Y;
        auto Right = Left + Model_.Tiles.front().Rect.Width;
        auto Bottom = Top + Model_.Tiles.front().Rect.Height;
        for (const auto& Tile : Model_.Tiles) {
            Left = std::min(Left, Tile.Rect.X);
            Top = std::min(Top, Tile.Rect.Y);
            Right = std::max(Right, Tile.Rect.X + Tile.Rect.Width);
            Bottom = std::max(Bottom, Tile.Rect.Y + Tile.Rect.Height);
        }
        RECT Client{};
        GetClientRect(Canvas_, &Client);
        const auto Width = std::max<std::int32_t>(Right - Left, 1);
        const auto Height = std::max<std::int32_t>(Bottom - Top, 1);
        const auto ScaleX = static_cast<double>(
            std::max<LONG>(Client.right - Client.left - 60, 1)) / Width;
        const auto ScaleY = static_cast<double>(
            std::max<LONG>(Client.bottom - Client.top - 80, 1)) / Height;
        ViewScale_ = std::clamp(std::min({1.0, ScaleX, ScaleY}), 0.10, 1.0);
        ViewOriginX_ = Left;
        ViewOriginY_ = Top;
    }

    [[nodiscard]] RECT ScreenRect(const MonitorCanvasTile& Tile) const {
        return {
            30 + static_cast<LONG>(std::lround(
                (Tile.Rect.X - ViewOriginX_) * ViewScale_)),
            45 + static_cast<LONG>(std::lround(
                (Tile.Rect.Y - ViewOriginY_) * ViewScale_)),
            30 + static_cast<LONG>(std::lround(
                (Tile.Rect.X - ViewOriginX_ + Tile.Rect.Width) * ViewScale_)),
            45 + static_cast<LONG>(std::lround(
                (Tile.Rect.Y - ViewOriginY_ + Tile.Rect.Height) * ViewScale_)),
        };
    }

    void PaintCanvas() {
        PAINTSTRUCT Paint{};
        const auto Dc = BeginPaint(Canvas_, &Paint);
        RECT Client{};
        GetClientRect(Canvas_, &Client);
        const auto Background = CreateSolidBrush(RGB(245, 247, 251));
        FillRect(Dc, &Client, Background);
        DeleteObject(Background);
        SetBkMode(Dc, TRANSPARENT);
        SelectObject(Dc, Font_);
        CalculateView();
        if (Model_.Tiles.empty()) {
            SetTextColor(Dc, RGB(80, 88, 102));
            DrawTextW(
                Dc, L"No current or saved displays. Start a topology-enabled session and refresh.",
                -1, &Client, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            EndPaint(Canvas_, &Paint);
            return;
        }

        std::vector<MachineId> Groups;
        for (const auto& Tile : Model_.Tiles) {
            if (std::find(Groups.begin(), Groups.end(), Tile.Machine) ==
                Groups.end()) {
                Groups.push_back(Tile.Machine);
            }
        }
        for (const auto& Machine : Groups) {
            RECT Group{};
            bool First = true;
            std::wstring Name;
            for (const auto& Tile : Model_.Tiles) {
                if (Tile.Machine != Machine) continue;
                const auto Rect = ScreenRect(Tile);
                if (First) {
                    Group = Rect;
                    Name = ToWide(Tile.MachineName);
                    First = false;
                } else {
                    Group.left = std::min(Group.left, Rect.left);
                    Group.top = std::min(Group.top, Rect.top);
                    Group.right = std::max(Group.right, Rect.right);
                    Group.bottom = std::max(Group.bottom, Rect.bottom);
                }
            }
            InflateRect(&Group, 10, 10);
            Group.top -= 24;
            const auto Brush = CreateSolidBrush(RGB(231, 237, 247));
            FillRect(Dc, &Group, Brush);
            DeleteObject(Brush);
            FrameRect(Dc, &Group, GetSysColorBrush(COLOR_3DSHADOW));
            auto Label = Group;
            Label.left += 8;
            Label.top += 3;
            SetTextColor(Dc, RGB(45, 60, 85));
            DrawTextW(Dc, Name.data(), static_cast<int>(Name.size()), &Label,
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        }

        for (std::size_t Index = 0; Index < Model_.Tiles.size(); ++Index) {
            const auto& Tile = Model_.Tiles[Index];
            auto Rect = ScreenRect(Tile);
            const auto Fill = CreateSolidBrush(Tile.Online
                ? Tile.Local ? RGB(219, 239, 255) : RGB(225, 247, 232)
                : RGB(226, 226, 226));
            FillRect(Dc, &Rect, Fill);
            DeleteObject(Fill);
            const auto Pen = CreatePen(
                Tile.SizeEstimated ? PS_DOT : PS_SOLID,
                SelectedTile_ && *SelectedTile_ == Index ? 3 : 2,
                Tile.Online ? RGB(35, 85, 135) : RGB(115, 115, 115));
            const auto OldPen = SelectObject(Dc, Pen);
            const auto OldBrush = SelectObject(Dc, GetStockObject(NULL_BRUSH));
            Rectangle(Dc, Rect.left, Rect.top, Rect.right, Rect.bottom);
            SelectObject(Dc, OldBrush);
            SelectObject(Dc, OldPen);
            DeleteObject(Pen);
            InflateRect(&Rect, -9, -8);
            SetTextColor(Dc, Tile.Online ? RGB(20, 34, 50) : RGB(90, 90, 90));
            const auto Text = TileText(Tile, Index);
            DrawTextW(Dc, Text.data(), static_cast<int>(Text.size()), &Rect,
                      DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
        }
        EndPaint(Canvas_, &Paint);
    }

    [[nodiscard]] std::optional<std::size_t> HitTest(int X, int Y) {
        CalculateView();
        for (std::size_t Index = Model_.Tiles.size(); Index > 0; --Index) {
            const auto Rect = ScreenRect(Model_.Tiles[Index - 1]);
            POINT Point{X, Y};
            if (PtInRect(&Rect, Point)) return Index - 1;
        }
        return std::nullopt;
    }

    void BeginDrag(int X, int Y) {
        const auto Hit = HitTest(X, Y);
        SelectedTile_ = Hit;
        if (!Hit) {
            InvalidateRect(Canvas_, nullptr, FALSE);
            return;
        }
        const auto Rect = ScreenRect(Model_.Tiles[*Hit]);
        DragOffsetX_ = X - Rect.left;
        DragOffsetY_ = Y - Rect.top;
        SetCapture(Canvas_);
        InvalidateRect(Canvas_, nullptr, FALSE);
    }

    void ContinueDrag(int X, int Y) {
        if (!SelectedTile_ || GetCapture() != Canvas_) return;
        const auto NewX = ViewOriginX_ + static_cast<std::int32_t>(std::lround(
            (X - DragOffsetX_ - 30) / ViewScale_));
        const auto NewY = ViewOriginY_ + static_cast<std::int32_t>(std::lround(
            (Y - DragOffsetY_ - 45) / ViewScale_));
        auto& Rect = Model_.Tiles[*SelectedTile_].Rect;
        Rect.X = std::clamp(
            NewX, -kMaximumCanvasCoordinate, kMaximumCanvasCoordinate);
        Rect.Y = std::clamp(
            NewY, -kMaximumCanvasCoordinate, kMaximumCanvasCoordinate);
        Dirty_ = true;
        InvalidateRect(Canvas_, nullptr, FALSE);
    }

    void EndDrag(int X, int Y) {
        ContinueDrag(X, Y);
        if (GetCapture() == Canvas_) ReleaseCapture();
        RecomputeSuggestion();
        UpdateControls();
        InvalidateRect(Canvas_, nullptr, FALSE);
    }

    HWND Owner_{};
    HINSTANCE Instance_{};
    HWND Window_{};
    HWND Canvas_{};
    HWND Status_{};
    HWND SourceDisplay_{};
    HWND SourceSide_{};
    HWND SourceStart_{};
    HWND SourceEnd_{};
    HWND TargetDisplay_{};
    HWND TargetSide_{};
    HWND TargetStart_{};
    HWND TargetEnd_{};
    HWND Direction_{};
    HWND SuggestionButton_{};
    HWND SuggestionText_{};
    HWND RouteList_{};
    HFONT Font_{};
    HFONT TitleFont_{};
    Win32RoamingSettingsStore Settings_;
    Win32MonitorConfiguratorCallbacks Callbacks_;
    RoamingConfiguration Configuration_;
    std::vector<MonitorCanvasMachine> Machines_;
    MonitorCanvasModel Model_;
    std::optional<RoamingLinkSuggestion> Suggestion_;
    std::optional<std::size_t> SelectedTile_;
    std::jthread RefreshWorker_;
    std::mutex RefreshResultMutex_;
    std::optional<ControlTopologyState> PendingRefreshResult_;
    std::int32_t ViewOriginX_{};
    std::int32_t ViewOriginY_{};
    double ViewScale_{1.0};
    int DragOffsetX_{};
    int DragOffsetY_{};
    bool Dirty_{};
    bool Saved_{};
    bool RefreshFinished_{};
};

} // namespace

bool ShowWin32MonitorConfigurator(
    HWND Owner, std::filesystem::path SettingsPath,
    Win32MonitorConfiguratorCallbacks Callbacks) {
    if (!Owner || !Callbacks.GetTopologies || !Callbacks.EnsureLocal) {
        return false;
    }
    ConfiguratorWindow Window(
        Owner, std::move(SettingsPath), std::move(Callbacks));
    return Window.Show();
}

bool ShowWin32DisplayIdentification(
    HWND Owner, std::uint16_t FirstDisplayNumber) {
    Win32DisplayTopology Topology;
    if (FirstDisplayNumber == 0 || FirstDisplayNumber > kMaxDisplayCount ||
        !Topology.Refresh() || Topology.Current().Displays.empty() ||
        Topology.Current().Displays.size() >
            kMaxDisplayCount - FirstDisplayNumber + 1u) {
        return false;
    }
    const auto Instance = Owner
        ? reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(
              Owner, GWLP_HINSTANCE))
        : GetModuleHandleW(nullptr);
    if (!Instance) return false;
    if (!RegisterIdentifyClass(Instance)) return false;
    wchar_t ComputerName[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD ComputerNameLength = static_cast<DWORD>(std::size(ComputerName));
    if (!GetComputerNameW(ComputerName, &ComputerNameLength)) {
        std::copy_n(L"This PC", 8, ComputerName);
        ComputerNameLength = 7;
    }

    std::size_t Created = 0;
    for (std::size_t Index = 0;
         Index < Topology.Current().Displays.size(); ++Index) {
        const auto& Display = Topology.Current().Displays[Index];
        const auto Width = std::max<std::int32_t>(
            1, Display.Bounds.Right - Display.Bounds.Left);
        const auto Height = std::max<std::int32_t>(
            1, Display.Bounds.Bottom - Display.Bounds.Top);
        auto Payload = std::make_unique<IdentifyPayload>();
        Payload->Text = std::to_wstring(FirstDisplayNumber + Index) + L"\n" +
            std::wstring(ComputerName, ComputerNameLength) + L"\r\n" +
            std::to_wstring(Display.PixelWidth) + L"×" +
            std::to_wstring(Display.PixelHeight) + L" · " +
            RefreshText(Display.RefreshMilliHertz);
        if (Display.PhysicalSize != PhysicalSizeSource::Edid) {
            Payload->Text += L" · size estimated";
        }
        Payload->LargeFont = CreateFontW(
            -50, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        Payload->SmallFont = CreateFontW(
            -20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        const auto OverlayWidth = std::min<std::int32_t>(440, Width - 40);
        const auto OverlayHeight = std::min<std::int32_t>(160, Height - 40);
        const auto Window = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT |
                WS_EX_TOOLWINDOW | WS_EX_LAYERED,
            kIdentifyClass, L"", WS_POPUP,
            Display.Bounds.Left + 20, Display.Bounds.Top + 20,
            std::max<std::int32_t>(OverlayWidth, 160),
            std::max<std::int32_t>(OverlayHeight, 100),
            Owner, nullptr, Instance, Payload.get());
        if (!Window) continue;
        (void)Payload.release();
        SetLayeredWindowAttributes(Window, 0, 232, LWA_ALPHA);
        ShowWindow(Window, SW_SHOWNOACTIVATE);
        UpdateWindow(Window);
        ++Created;
    }
    return Created == Topology.Current().Displays.size();
}

bool RunWin32DisplayIdentification(
    std::uint16_t FirstDisplayNumber, std::stop_token StopToken) {
    if (!ShowWin32DisplayIdentification(nullptr, FirstDisplayNumber)) {
        return false;
    }
    const auto Deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(kIdentifyDurationMilliseconds + 250u);
    MSG Message{};
    while (!StopToken.stop_requested() &&
           std::chrono::steady_clock::now() < Deadline) {
        while (PeekMessageW(&Message, nullptr, 0, 0, PM_REMOVE)) {
            if (Message.message == WM_QUIT) return true;
            TranslateMessage(&Message);
            DispatchMessageW(&Message);
        }
        (void)MsgWaitForMultipleObjectsEx(
            0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND Window, LPARAM) -> BOOL {
            wchar_t ClassName[64]{};
            if (GetClassNameW(
                    Window, ClassName,
                    static_cast<int>(std::size(ClassName))) > 0 &&
                std::wstring_view(ClassName) == kIdentifyClass) {
                DestroyWindow(Window);
            }
            return TRUE;
        },
        0);
    while (PeekMessageW(&Message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&Message);
        DispatchMessageW(&Message);
    }
    return true;
}

} // namespace desklink

#endif
