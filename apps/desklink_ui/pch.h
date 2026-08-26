#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <strsafe.h>

// windows.h defines this as a macro, which collides with a WinUI projection
// member. The WinUI API must retain its generated name.
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <array>
#include <string>
#include <string_view>

#include <microsoft.ui.xaml.window.h>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Interop.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.h>

#include "desklink/product_shell.hpp"
