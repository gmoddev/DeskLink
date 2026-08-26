#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <strsafe.h>

// windows.h defines this as a macro, which collides with a WinUI projection
// member. The WinUI API must retain its generated name.
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <microsoft.ui.xaml.window.h>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Interop.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>

#include "desklink/control.hpp"
#include "desklink/monitor_configurator.hpp"
#include "desklink/product_shell.hpp"
#include "desklink/win32_control.hpp"
#include "desklink/win32_display_topology.hpp"
#include "desklink/win32_monitor_configurator.hpp"
#include "desklink/win32_product_lifecycle.hpp"
#include "desklink/win32_roaming_settings.hpp"
