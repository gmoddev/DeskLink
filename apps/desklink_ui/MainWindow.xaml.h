#pragma once

#include "MainWindow.g.h"

namespace winrt::DeskLink::Product::implementation {

struct MainWindow : MainWindowT<MainWindow> {
    MainWindow();
    ~MainWindow();

    void OnNavigationChanged(
        Microsoft::UI::Xaml::Controls::NavigationView const& Sender,
        Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const& Args);
    void OnSimulationChanged(
        Windows::Foundation::IInspectable const& Sender,
        Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& Args);
    void OnReturnLocal(
        Windows::Foundation::IInspectable const& Sender,
        Microsoft::UI::Xaml::RoutedEventArgs const& Args);

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
    HWND MainWindowHandle_{};
    HWND LifecycleWindow_{};
    NOTIFYICONDATAW TrayIcon_{};
    bool TrayActive_{};
    bool ExplicitExit_{};
    bool ContentReady_{};
    desklink::ProductShellState State_{
        desklink::ProductShellState::ConnectedLocal};
};

} // namespace winrt::DeskLink::Product::implementation

namespace winrt::DeskLink::Product::factory_implementation {

struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};

} // namespace winrt::DeskLink::Product::factory_implementation
