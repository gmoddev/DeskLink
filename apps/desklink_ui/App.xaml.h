#pragma once

#include "App.xaml.g.h"

namespace winrt::DeskLink::Product::implementation {

struct App : AppT<App> {
    App();
    ~App();

    void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const& Args);

private:
    bool IsSecondaryInstance() noexcept;
    void RedirectToPrimary() noexcept;

    HANDLE InstanceMutex_{};
    Microsoft::UI::Xaml::Window Window_{nullptr};
};

} // namespace winrt::DeskLink::Product::implementation
