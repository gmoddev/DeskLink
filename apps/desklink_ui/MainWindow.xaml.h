#pragma once

#include "MainWindow.g.h"

namespace winrt::DeskLink::Product::implementation {

struct MainWindow : MainWindowT<MainWindow> {
    MainWindow();
    ~MainWindow();

    void OnNavigationChanged(
        Microsoft::UI::Xaml::Controls::NavigationView const& Sender,
        Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const& Args);
    void OnReturnLocal(
        Windows::Foundation::IInspectable const& Sender,
        Microsoft::UI::Xaml::RoutedEventArgs const& Args);
    void OnChooseMain(
        Windows::Foundation::IInspectable const& Sender,
        Microsoft::UI::Xaml::RoutedEventArgs const& Args);
    void OnChooseCompanion(
        Windows::Foundation::IInspectable const& Sender,
        Microsoft::UI::Xaml::RoutedEventArgs const& Args);
    void OnChooseFlexible(
        Windows::Foundation::IInspectable const& Sender,
        Microsoft::UI::Xaml::RoutedEventArgs const& Args);
    void OnOpenDevices(
        Windows::Foundation::IInspectable const& Sender,
        Microsoft::UI::Xaml::RoutedEventArgs const& Args);
    void OnOpenAddPc(
        Windows::Foundation::IInspectable const& Sender,
        Microsoft::UI::Xaml::RoutedEventArgs const& Args);
    void OnOpenDisplays(
        Windows::Foundation::IInspectable const& Sender,
        Microsoft::UI::Xaml::RoutedEventArgs const& Args);
    void OnRefreshNearby(
        Windows::Foundation::IInspectable const& Sender,
        Microsoft::UI::Xaml::RoutedEventArgs const& Args);
    void OnOpenPairingWindow(
        Windows::Foundation::IInspectable const& Sender,
        Microsoft::UI::Xaml::RoutedEventArgs const& Args);
    void OnPairManual(
        Windows::Foundation::IInspectable const& Sender,
        Microsoft::UI::Xaml::RoutedEventArgs const& Args);
    void OnNearbyConnect(
        Windows::Foundation::IInspectable const& Sender,
        Microsoft::UI::Xaml::RoutedEventArgs const& Args);
    void OnFinishOnboarding(
        Windows::Foundation::IInspectable const& Sender,
        Microsoft::UI::Xaml::RoutedEventArgs const& Args);
    void OnRefreshDevices(
        Windows::Foundation::IInspectable const& Sender,
        Microsoft::UI::Xaml::RoutedEventArgs const& Args);
    void OnDevicePermissions(
        Windows::Foundation::IInspectable const& Sender,
        Microsoft::UI::Xaml::RoutedEventArgs const& Args);
    void OnDeviceForget(
        Windows::Foundation::IInspectable const& Sender,
        Microsoft::UI::Xaml::RoutedEventArgs const& Args);
    void OnRefreshMonitorLayout(
        Windows::Foundation::IInspectable const& Sender,
        Microsoft::UI::Xaml::RoutedEventArgs const& Args);
    void OnIdentifyDisplays(
        Windows::Foundation::IInspectable const& Sender,
        Microsoft::UI::Xaml::RoutedEventArgs const& Args);
    void OnAcceptMonitorSuggestion(
        Windows::Foundation::IInspectable const& Sender,
        Microsoft::UI::Xaml::RoutedEventArgs const& Args);
    void OnAddAccessibleConnection(
        Windows::Foundation::IInspectable const& Sender,
        Microsoft::UI::Xaml::RoutedEventArgs const& Args);
    void OnAddAdvancedConnection(
        Windows::Foundation::IInspectable const& Sender,
        Microsoft::UI::Xaml::RoutedEventArgs const& Args);
    void OnRemoveMonitorRoute(
        Windows::Foundation::IInspectable const& Sender,
        Microsoft::UI::Xaml::RoutedEventArgs const& Args);
    void OnNudgeMonitorTile(
        Windows::Foundation::IInspectable const& Sender,
        Microsoft::UI::Xaml::RoutedEventArgs const& Args);
    void OnSaveMonitorLayout(
        Windows::Foundation::IInspectable const& Sender,
        Microsoft::UI::Xaml::RoutedEventArgs const& Args);
    void OnMonitorTilePointerPressed(
        Windows::Foundation::IInspectable const& Sender,
        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& Args);
    void OnMonitorTilePointerMoved(
        Windows::Foundation::IInspectable const& Sender,
        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& Args);
    void OnMonitorTilePointerReleased(
        Windows::Foundation::IInspectable const& Sender,
        Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& Args);

    void HideToTray();
    void InitializeWindowLifecycle();
    void RequestExit();

private:
    static LRESULT CALLBACK LifecycleWindowProcedure(
        HWND Window, UINT Message, WPARAM WParam, LPARAM LParam);
    static LRESULT CALLBACK MainWindowSubclassProcedure(
        HWND Window,
        UINT Message,
        WPARAM WParam,
        LPARAM LParam,
        UINT_PTR SubclassId,
        DWORD_PTR ReferenceData);
    bool CreateLifecycleWindow();
    bool AddTrayIcon();
    void RemoveTrayIcon() noexcept;
    void ShowTrayMenu();
    void ShowFromTray();
    void TogglePaused();
    void ApplyState(desklink::ProductShellState State);
    void PollBroker();
    void PollPreferences();
    void PollNearby();
    void PollDevices();
    void PollPairingCandidate();
    void ChooseRole(desklink::DeskRole Role);
    void NavigateTo(winrt::hstring const& Tag);
    void RenderNearby();
    void RenderDevices();
    void LoadMonitorLayout();
    void RenderMonitorCanvas();
    void RenderMonitorEditors();
    void RenderMonitorRoutes();
    void RecomputeMonitorSuggestion(bool SnapDraggedTile);
    void MarkMonitorDirty();
    void ShowMonitorStatus(
        winrt::hstring const& Title,
        winrt::hstring const& Message,
        Microsoft::UI::Xaml::Controls::InfoBarSeverity Severity);
    void UpdateHome();
    void ShowPairingStatus(
        winrt::hstring const& Message,
        Microsoft::UI::Xaml::Controls::InfoBarSeverity Severity);
    [[nodiscard]] desklink::CapabilitySet SelectedPairingCapabilities();
    [[nodiscard]] std::optional<desklink::ControlResponse> Send(
        desklink::ControlRequestPayload Payload,
        std::chrono::milliseconds Timeout = std::chrono::milliseconds{500});
    [[nodiscard]] std::optional<desklink::MachineId> MachineFromTag(
        Windows::Foundation::IInspectable const& Tag) const;
    [[nodiscard]] std::optional<std::size_t> MonitorTileFromTag(
        Windows::Foundation::IInspectable const& Tag) const;
    [[nodiscard]] std::optional<std::size_t> SelectedMonitorTile(
        Microsoft::UI::Xaml::Controls::ComboBox const& Selector) const;
    [[nodiscard]] std::optional<desklink::RoamingLink> ReadMonitorConnection(
        bool Advanced);
    [[nodiscard]] Windows::Foundation::IAsyncAction ShowPairingCandidate(
        desklink::ControlPairingCandidate Candidate);
    [[nodiscard]] Windows::Foundation::IAsyncAction ShowPermissionEditor(
        desklink::ControlTrustedDevice Device);
    [[nodiscard]] Windows::Foundation::IAsyncAction ConfirmForget(
        desklink::ControlTrustedDevice Device);

    HWND MainWindowHandle_{};
    HWND LifecycleWindow_{};
    NOTIFYICONDATAW TrayIcon_{};
    Microsoft::UI::Dispatching::DispatcherQueueTimer PollTimer_{nullptr};
    Microsoft::UI::Xaml::Controls::ContentDialog PairingDialog_{nullptr};
    desklink::ProductPreferences Preferences_;
    std::vector<desklink::ControlNearbyPeer> NearbyPeers_;
    std::vector<desklink::ControlTrustedDevice> TrustedDevices_;
    std::unique_ptr<desklink::Win32RoamingSettingsStore> RoamingSettings_;
    desklink::RoamingConfiguration MonitorConfiguration_;
    std::vector<desklink::MonitorCanvasMachine> MonitorMachines_;
    desklink::MonitorCanvasModel MonitorModel_;
    std::vector<Microsoft::UI::Xaml::Controls::Border> MonitorTileElements_;
    std::optional<desklink::RoamingLinkSuggestion> MonitorSuggestion_;
    std::optional<std::size_t> DraggingMonitorTile_;
    desklink::MonitorCanvasRect DragStartRect_;
    double DragStartPointerX_{};
    double DragStartPointerY_{};
    std::int32_t MonitorViewOriginX_{};
    std::int32_t MonitorViewOriginY_{};
    double MonitorViewScale_{1.0};
    desklink::MachineId LocalMachine_{};
    std::atomic_uint64_t NextRequestId_{1};
    std::uint64_t DisplayedPairingOperation_{};
    bool TrayActive_{};
    bool ExplicitExit_{};
    bool ContentReady_{};
    bool PreferencesLoaded_{};
    bool DevicesLoaded_{};
    bool BrokerAvailable_{};
    bool PairingDialogActive_{};
    bool ModalDialogActive_{};
    bool DiscoveryActive_{};
    bool BrokerPaused_{};
    bool MonitorLayoutLoaded_{};
    bool MonitorLayoutDirty_{};
    desklink::ProductShellState State_{
        desklink::ProductShellState::Offline};
};

} // namespace winrt::DeskLink::Product::implementation

namespace winrt::DeskLink::Product::factory_implementation {

struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};

} // namespace winrt::DeskLink::Product::factory_implementation
