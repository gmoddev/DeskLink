#include "pch.h"
#include "MainWindow.xaml.h"
#include "MainWindow.g.cpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace {

constexpr wchar_t kLifecycleWindowClass[] =
    L"DeskLinkShellLifecycleWindow.v1";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kTrayOpen = 1;
constexpr UINT kTrayReturnLocal = 2;
constexpr UINT kTrayPause = 3;
constexpr UINT kTrayExit = 4;

UINT GetActivateMessage() noexcept {
    static const UINT Message =
        RegisterWindowMessageW(L"DeskLink.ActivateShell.v1");
    return Message;
}

UINT GetExitMessage() noexcept {
    static const UINT Message =
        RegisterWindowMessageW(L"DeskLink.ExitShell.v1");
    return Message;
}

UINT GetPrepareUpdateMessage() noexcept {
    static const UINT Message =
        RegisterWindowMessageW(L"DeskLink.PrepareUpdate.v1");
    return Message;
}

winrt::hstring ToHString(std::string_view Text) {
    if (Text.empty()) return {};
    const auto Required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, Text.data(),
        static_cast<int>(Text.size()), nullptr, 0);
    if (Required <= 0) return L"Invalid name";
    std::wstring Result(static_cast<std::size_t>(Required), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, Text.data(),
            static_cast<int>(Text.size()), Result.data(), Required) !=
        Required) {
        return L"Invalid name";
    }
    return winrt::hstring(Result);
}

winrt::hstring JoinText(
    std::wstring_view Prefix,
    winrt::hstring const& Value,
    std::wstring_view Suffix = {}) {
    std::wstring Result(Prefix);
    Result.append(Value.c_str(), Value.size());
    Result.append(Suffix);
    return winrt::hstring(Result);
}

std::optional<std::string> ToUtf8(winrt::hstring const& Text) {
    if (Text.empty()) return std::string{};
    const auto Required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, Text.c_str(),
        static_cast<int>(Text.size()), nullptr, 0, nullptr, nullptr);
    if (Required <= 0) return std::nullopt;
    std::string Result(static_cast<std::size_t>(Required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, Text.c_str(),
            static_cast<int>(Text.size()), Result.data(), Required,
            nullptr, nullptr) != Required) {
        return std::nullopt;
    }
    return Result;
}

winrt::hstring MachineTag(const desklink::MachineId& Machine) {
    constexpr wchar_t Hex[] = L"0123456789abcdef";
    std::wstring Result;
    Result.reserve(Machine.size() * 2u);
    for (const auto Byte : Machine) {
        Result.push_back(Hex[Byte >> 4u]);
        Result.push_back(Hex[Byte & 0x0fu]);
    }
    return winrt::hstring(Result);
}

std::optional<desklink::MachineId> ParseMachineTag(
    std::wstring_view Text) noexcept {
    if (Text.size() != desklink::MachineId{}.size() * 2u) {
        return std::nullopt;
    }
    auto Nibble = [](wchar_t Character) -> std::optional<std::uint8_t> {
        if (Character >= L'0' && Character <= L'9') {
            return static_cast<std::uint8_t>(Character - L'0');
        }
        if (Character >= L'a' && Character <= L'f') {
            return static_cast<std::uint8_t>(Character - L'a' + 10);
        }
        return std::nullopt;
    };
    desklink::MachineId Result{};
    for (std::size_t Index = 0; Index < Result.size(); ++Index) {
        const auto High = Nibble(Text[Index * 2u]);
        const auto Low = Nibble(Text[Index * 2u + 1u]);
        if (!High || !Low) return std::nullopt;
        Result[Index] = static_cast<std::uint8_t>((*High << 4u) | *Low);
    }
    return std::any_of(Result.begin(), Result.end(),
                       [](std::uint8_t Byte) { return Byte != 0; })
        ? std::optional<desklink::MachineId>(Result)
        : std::nullopt;
}

bool IsChecked(
    winrt::Microsoft::UI::Xaml::Controls::CheckBox const& CheckBox) noexcept {
    const auto Value = CheckBox.IsChecked();
    return Value && Value.Value();
}

winrt::hstring RoleName(desklink::DeskRole Role) {
    switch (Role) {
        case desklink::DeskRole::Main: return L"Main PC";
        case desklink::DeskRole::Companion: return L"Companion PC";
        case desklink::DeskRole::Flexible: return L"Flexible role";
        default: return L"Not configured";
    }
}

winrt::hstring CapabilitySummary(desklink::CapabilitySet Capabilities) {
    std::vector<std::wstring> Lines;
    if (Capabilities.contains(desklink::Capability::InputInject)) {
        Lines.emplace_back(L"Can control this PC");
    }
    if (Capabilities.contains(
            desklink::Capability::DisplayTopologyExchange)) {
        Lines.emplace_back(L"Display layout shared");
    }
    if (Capabilities.contains(desklink::Capability::ClipboardRead)) {
        Lines.emplace_back(L"May read text clipboard");
    }
    if (Capabilities.contains(desklink::Capability::ClipboardWrite)) {
        Lines.emplace_back(L"May replace text clipboard");
    }
    if (Capabilities.contains(desklink::Capability::AudioSend)) {
        Lines.emplace_back(L"May play audio into this PC");
    }
    if (Capabilities.contains(desklink::Capability::AudioReceive)) {
        Lines.emplace_back(L"May receive audio from this PC");
    }
    if (Lines.empty()) return L"No sensitive permissions";
    std::wstring Result;
    for (std::size_t Index = 0; Index < Lines.size(); ++Index) {
        if (Index != 0) Result += L"\n";
        Result += L"• ";
        Result += Lines[Index];
    }
    return winrt::hstring(Result);
}

desklink::ProductShellState StateFromControl(
    const desklink::ControlState& State) noexcept {
    if (State.RuntimePhase == desklink::BrokerRuntimePhase::Paused) {
        return desklink::ProductShellState::Paused;
    }
    if (State.RuntimePhase ==
        desklink::BrokerRuntimePhase::ActionRequired) {
        return desklink::ProductShellState::ActionRequired;
    }
    if (State.RemoteFocused) {
        return desklink::ProductShellState::RemoteFocus;
    }
    if (State.ConnectedPeerCount != 0) {
        return desklink::ProductShellState::ConnectedLocal;
    }
    if (State.RuntimePhase == desklink::BrokerRuntimePhase::Discovering ||
        State.RuntimePhase == desklink::BrokerRuntimePhase::Connecting ||
        State.RuntimePhase == desklink::BrokerRuntimePhase::RetryWaiting) {
        return desklink::ProductShellState::Connecting;
    }
    return desklink::ProductShellState::Offline;
}

} // namespace

namespace winrt::DeskLink::Product::implementation {

MainWindow::MainWindow() {
    InitializeComponent();
    ContentReady_ = true;
    Title(L"DeskLink");
    Navigation().SelectedItem(HomeNavigation());
    if (!CreateLifecycleWindow() || !AddTrayIcon()) {
        throw winrt::hresult_error(
            E_FAIL, L"Could not initialize DeskLink lifecycle controls.");
    }
    wchar_t ComputerName[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD ComputerNameLength = static_cast<DWORD>(std::size(ComputerName));
    if (GetComputerNameW(ComputerName, &ComputerNameLength)) {
        LocalPcName().Text(winrt::hstring(
            ComputerName, ComputerNameLength));
    }
    ApplyState(State_);
    PollTimer_ = DispatcherQueue().CreateTimer();
    PollTimer_.Interval(std::chrono::milliseconds{750});
    PollTimer_.Tick([this](auto&&, auto&&) { PollBroker(); });
    PollTimer_.Start();
    PollBroker();
}

void MainWindow::InitializeWindowLifecycle() {
    const Microsoft::UI::Xaml::Window ProductWindow = *this;
    winrt::check_hresult(
        ProductWindow.as<IWindowNative>()->get_WindowHandle(
            &MainWindowHandle_));
    if (!MainWindowHandle_ || !SetWindowSubclass(
            MainWindowHandle_, MainWindowSubclassProcedure, 1,
            reinterpret_cast<DWORD_PTR>(this))) {
        throw winrt::hresult_error(
            E_UNEXPECTED,
            L"DeskLink window activation did not create lifecycle controls.");
    }
    SetWindowPos(
        MainWindowHandle_, nullptr, 0, 0, 1'040, 760,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

MainWindow::~MainWindow() {
    if (PollTimer_) PollTimer_.Stop();
    RemoveTrayIcon();
    if (MainWindowHandle_) {
        RemoveWindowSubclass(
            MainWindowHandle_, MainWindowSubclassProcedure, 1);
    }
    if (LifecycleWindow_) DestroyWindow(LifecycleWindow_);
}

std::optional<desklink::ControlResponse> MainWindow::Send(
    desklink::ControlRequestPayload Payload,
    std::chrono::milliseconds Timeout) {
    return desklink::Win32ControlPipeClient::Send(
        desklink::ControlRequest{
            NextRequestId_.fetch_add(1), std::move(Payload)},
        L"broker", Timeout);
}

void MainWindow::PollBroker() {
    if (!ContentReady_ || ExplicitExit_) return;
    const auto Response = Send(
        desklink::GetStateControlRequest{},
        std::chrono::milliseconds{200});
    BrokerAvailable_ = Response &&
        Response->Status == desklink::ControlStatus::Ok && Response->State;
    BrokerUnavailableBar().IsOpen(!BrokerAvailable_);
    if (!BrokerAvailable_) {
        ApplyState(desklink::ProductShellState::Offline);
        if (PairingDialogActive_ && PairingDialog_) PairingDialog_.Hide();
        return;
    }
    ApplyState(StateFromControl(*Response->State));
    PollPreferences();
    PollDevices();
    PollNearby();
    PollPairingCandidate();
}

void MainWindow::PollPreferences() {
    const auto Response = Send(desklink::GetProductPreferencesControlRequest{});
    if (!Response || Response->Status != desklink::ControlStatus::Ok ||
        !Response->Preferences) {
        return;
    }
    const bool WasLoaded = PreferencesLoaded_;
    Preferences_ = *Response->Preferences;
    PreferencesLoaded_ = true;
    LocalRoleText().Text(RoleName(Preferences_.Role));
    FinishSetupButton().Visibility(!Preferences_.FirstRunComplete &&
            Preferences_.Role != desklink::DeskRole::Unconfigured
        ? Microsoft::UI::Xaml::Visibility::Visible
        : Microsoft::UI::Xaml::Visibility::Collapsed);
    if (!WasLoaded &&
        (!Preferences_.FirstRunComplete ||
         Preferences_.Role == desklink::DeskRole::Unconfigured)) {
        NavigateTo(L"Onboarding");
    }
}

void MainWindow::PollNearby() {
    if (!DiscoveryActive_) return;
    const auto Response = Send(desklink::GetNearbyPeersControlRequest{});
    if (!Response || Response->Status != desklink::ControlStatus::Ok ||
        !Response->NearbyPeers) {
        DiscoveryActive_ = false;
        DiscoveryProgress().IsActive(false);
        NearbyStatus().Text(L"Nearby scan failed. Manual pairing remains available.");
        return;
    }
    if (Response->NearbyPeers->Phase ==
        desklink::ControlDiscoveryPhase::Searching) {
        DiscoveryProgress().IsActive(true);
        NearbyStatus().Text(L"Looking for DeskLink PCs on this local network…");
        return;
    }
    DiscoveryActive_ = false;
    DiscoveryProgress().IsActive(false);
    NearbyPeers_ = Response->NearbyPeers->Peers;
    RenderNearby();
}

void MainWindow::PollDevices() {
    const auto Response = Send(desklink::ListTrustedDevicesControlRequest{});
    if (!Response || Response->Status != desklink::ControlStatus::Ok ||
        !Response->TrustedDevices) {
        return;
    }
    if (TrustedDevices_ != Response->TrustedDevices->Devices) {
        const bool AddedDevice = DevicesLoaded_ && std::any_of(
            Response->TrustedDevices->Devices.begin(),
            Response->TrustedDevices->Devices.end(),
            [&](const auto& Device) {
                return std::none_of(
                    TrustedDevices_.begin(), TrustedDevices_.end(),
                    [&](const auto& Existing) {
                        return Existing.Machine == Device.Machine;
                    });
            });
        TrustedDevices_ = Response->TrustedDevices->Devices;
        RenderDevices();
        UpdateHome();
        if (AddedDevice && !Preferences_.FirstRunComplete) {
            ShowPairingStatus(
                L"Pairing completed and trust was stored on this PC. Finish setup when ready.",
                Microsoft::UI::Xaml::Controls::InfoBarSeverity::Success);
        }
    }
    DevicesLoaded_ = true;
}

void MainWindow::PollPairingCandidate() {
    const auto Response = Send(desklink::GetPairingCandidateControlRequest{});
    if (Response && Response->Status == desklink::ControlStatus::Ok &&
        Response->PairingCandidate) {
        if (DisplayedPairingOperation_ ==
                Response->PairingCandidate->OperationId ||
            ModalDialogActive_) {
            return;
        }
        DisplayedPairingOperation_ =
            Response->PairingCandidate->OperationId;
        (void)ShowPairingCandidate(*Response->PairingCandidate);
        return;
    }
    if (PairingDialogActive_ && PairingDialog_) {
        PairingDialog_.Hide();
    }
}

void MainWindow::OnNavigationChanged(
    Microsoft::UI::Xaml::Controls::NavigationView const&,
    Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const& Args) {
    const auto Item = Args.SelectedItemContainer();
    const auto Tag = Item
        ? winrt::unbox_value_or<winrt::hstring>(Item.Tag(), {})
        : winrt::hstring{};
    if (!Tag.empty()) NavigateTo(Tag);
}

void MainWindow::NavigateTo(winrt::hstring const& Tag) {
    const auto Visible = Microsoft::UI::Xaml::Visibility::Visible;
    const auto Collapsed = Microsoft::UI::Xaml::Visibility::Collapsed;
    OnboardingPage().Visibility(Tag == L"Onboarding" ? Visible : Collapsed);
    HomePage().Visibility(Tag == L"Home" ? Visible : Collapsed);
    AddPcPage().Visibility(Tag == L"AddPc" ? Visible : Collapsed);
    DevicesPage().Visibility(Tag == L"Devices" ? Visible : Collapsed);
    AdvancedPage().Visibility(Tag == L"Advanced" ? Visible : Collapsed);
    DiagnosticsPage().Visibility(Tag == L"Diagnostics" ? Visible : Collapsed);
}

void MainWindow::ChooseRole(desklink::DeskRole Role) {
    if (!PreferencesLoaded_ || !BrokerAvailable_) return;
    auto Updated = Preferences_;
    Updated.Role = Role;
    Updated.FirstRunComplete = false;
    Updated.AutoStartRuntime = false;
    Updated.AutoConnect = false;
    Updated.InputRoamingDesired = false;
    const auto Response = Send(
        desklink::SetProductPreferencesControlRequest{Updated});
    if (!Response || Response->Status != desklink::ControlStatus::Ok) {
        ShowPairingStatus(
            L"The role could not be saved. Input remains on this PC.",
            Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error);
        return;
    }
    Preferences_ = Updated;
    LocalRoleText().Text(RoleName(Role));
    FinishSetupButton().Visibility(
        Microsoft::UI::Xaml::Visibility::Visible);
    Navigation().SelectedItem(AddPcNavigation());
    NavigateTo(L"AddPc");
}

void MainWindow::OnChooseMain(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    ChooseRole(desklink::DeskRole::Main);
}

void MainWindow::OnChooseCompanion(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    ChooseRole(desklink::DeskRole::Companion);
}

void MainWindow::OnChooseFlexible(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    ChooseRole(desklink::DeskRole::Flexible);
}

void MainWindow::OnOpenDevices(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    Navigation().SelectedItem(DevicesNavigation());
    NavigateTo(L"Devices");
    PollDevices();
}

void MainWindow::OnOpenAddPc(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    Navigation().SelectedItem(AddPcNavigation());
    NavigateTo(L"AddPc");
}

desklink::CapabilitySet MainWindow::SelectedPairingCapabilities() {
    desklink::CapabilitySet Result;
    if (IsChecked(GrantInputCheck())) {
        Result.grant(desklink::Capability::InputInject);
    }
    if (IsChecked(GrantTopologyCheck())) {
        Result.grant(desklink::Capability::DisplayTopologyExchange);
    }
    if (IsChecked(GrantClipboardReadCheck())) {
        Result.grant(desklink::Capability::ClipboardRead);
    }
    if (IsChecked(GrantClipboardWriteCheck())) {
        Result.grant(desklink::Capability::ClipboardWrite);
    }
    if (IsChecked(GrantAudioSendCheck())) {
        Result.grant(desklink::Capability::AudioSend);
    }
    if (IsChecked(GrantAudioReceiveCheck())) {
        Result.grant(desklink::Capability::AudioReceive);
    }
    return Result;
}

void MainWindow::OnRefreshNearby(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    NearbyPeers_.clear();
    NearbyCards().Children().Clear();
    const auto Response = Send(
        desklink::StartDiscoveryControlRequest{5});
    if (!Response || Response->Status != desklink::ControlStatus::Ok) {
        NearbyStatus().Text(
            L"A scan is already running or discovery is unavailable. Manual pairing remains available.");
        return;
    }
    DiscoveryActive_ = true;
    DiscoveryProgress().IsActive(true);
    NearbyStatus().Text(L"Looking for DeskLink PCs on this local network…");
}

void MainWindow::OnOpenPairingWindow(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    const auto Response = Send(desklink::OpenPairingWindowControlRequest{
        43'821, SelectedPairingCapabilities()});
    ShowPairingStatus(
        Response && Response->Status == desklink::ControlStatus::Ok
            ? L"Pairing window is open for five minutes. Keep this screen open and compare the code on both PCs."
            : L"The pairing window could not open. Another runtime operation may be active.",
        Response && Response->Status == desklink::ControlStatus::Ok
            ? Microsoft::UI::Xaml::Controls::InfoBarSeverity::Success
            : Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error);
}

void MainWindow::OnPairManual(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    const auto Host = ToUtf8(ManualHost().Text());
    const auto PortValue = ManualPort().Value();
    if (!Host || Host->empty() || !std::isfinite(PortValue) ||
        PortValue < 1 || PortValue > 65'535 ||
        std::floor(PortValue) != PortValue) {
        ShowPairingStatus(
            L"Enter a host or IP address and a valid UDP port.",
            Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error);
        return;
    }
    const auto Response = Send(desklink::PairManualAddressControlRequest{
        *Host,
        static_cast<std::uint16_t>(PortValue),
        SelectedPairingCapabilities()});
    ShowPairingStatus(
        Response && Response->Status == desklink::ControlStatus::Ok
            ? L"Pairing started. Compare the code shown on both PCs before allowing."
            : L"Pairing could not start. Verify the address and open a pairing window on the other PC.",
        Response && Response->Status == desklink::ControlStatus::Ok
            ? Microsoft::UI::Xaml::Controls::InfoBarSeverity::Informational
            : Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error);
}

void MainWindow::OnNearbyConnect(
    Windows::Foundation::IInspectable const& Sender,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    const auto Button = Sender.try_as<Microsoft::UI::Xaml::Controls::Button>();
    const auto Machine = Button ? MachineFromTag(Button.Tag()) : std::nullopt;
    if (!Machine) return;
    const auto Response = Send(desklink::PairNearbyPeerControlRequest{
        *Machine, SelectedPairingCapabilities()});
    ShowPairingStatus(
        Response && Response->Status == desklink::ControlStatus::Ok
            ? L"Pairing started from an unverified nearby address. Compare the code on both PCs."
            : L"That nearby record is no longer safe to use. Refresh the list or enter the address manually.",
        Response && Response->Status == desklink::ControlStatus::Ok
            ? Microsoft::UI::Xaml::Controls::InfoBarSeverity::Informational
            : Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error);
}

void MainWindow::OnFinishOnboarding(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    if (!PreferencesLoaded_) return;
    auto Updated = Preferences_;
    Updated.FirstRunComplete = true;
    Updated.AutoStartRuntime =
        Updated.Role == desklink::DeskRole::Companion ||
        Updated.Role == desklink::DeskRole::Flexible ||
        !TrustedDevices_.empty();
    if (!TrustedDevices_.empty() &&
        (Updated.Role == desklink::DeskRole::Main ||
         Updated.Role == desklink::DeskRole::Flexible)) {
        Updated.PreferredPeerMachine = TrustedDevices_.front().Machine;
        Updated.AutoConnect = true;
    }
    const auto Response = Send(
        desklink::SetProductPreferencesControlRequest{Updated});
    if (!Response || Response->Status != desklink::ControlStatus::Ok) {
        ShowPairingStatus(
            L"Setup could not be saved. Input remains on this PC.",
            Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error);
        return;
    }
    Preferences_ = Updated;
    Navigation().SelectedItem(HomeNavigation());
    NavigateTo(L"Home");
    UpdateHome();
}

void MainWindow::OnRefreshDevices(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    PollDevices();
}

std::optional<desklink::MachineId> MainWindow::MachineFromTag(
    Windows::Foundation::IInspectable const& Tag) const {
    const auto Text = winrt::unbox_value_or<winrt::hstring>(Tag, {});
    return ParseMachineTag(Text);
}

void MainWindow::RenderNearby() {
    NearbyCards().Children().Clear();
    if (NearbyPeers_.empty()) {
        NearbyStatus().Text(
            L"No DeskLink PCs were found. Open a pairing window on the other PC or use the manual address.");
        return;
    }
    NearbyStatus().Text(
        L"Nearby results are unverified. Connect is disabled for ambiguous, incompatible, or closed records.");
    for (const auto& Peer : NearbyPeers_) {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        Border Card;
        Card.Padding(Thickness{16});
        Card.CornerRadius(CornerRadius{8});
        Card.BorderThickness(Thickness{1});
        Grid Layout;
        Layout.ColumnSpacing(12);
        Layout.ColumnDefinitions().Append(ColumnDefinition{});
        ColumnDefinition ButtonColumn;
        ButtonColumn.Width(GridLengthHelper::Auto());
        Layout.ColumnDefinitions().Append(ButtonColumn);
        StackPanel Text;
        Text.Spacing(3);
        TextBlock Name;
        Name.Text(ToHString(Peer.DisplayName));
        Name.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        Text.Children().Append(Name);
        TextBlock Status;
        if (Peer.Ambiguous) {
            Status.Text(L"Nearby · Unverified · Conflicting endpoints");
        } else if (!Peer.PairingOpen) {
            Status.Text(L"Nearby · Unverified · Pairing window closed");
        } else if (Peer.ProtocolVersion != desklink::kProtocolVersion) {
            Status.Text(L"Nearby · Unverified · Incompatible version");
        } else {
            Status.Text(L"Nearby · Unverified · Ready to compare code");
        }
        Text.Children().Append(Status);
        Layout.Children().Append(Text);
        Button Connect;
        Connect.Content(winrt::box_value(L"Connect"));
        Connect.Tag(winrt::box_value(MachineTag(Peer.Machine)));
        Connect.IsEnabled(!Peer.Ambiguous && Peer.PairingOpen &&
                          Peer.EndpointCount != 0 &&
                          Peer.ProtocolVersion == desklink::kProtocolVersion);
        Connect.Click({this, &MainWindow::OnNearbyConnect});
        Grid::SetColumn(Connect, 1);
        Layout.Children().Append(Connect);
        Card.Child(Layout);
        NearbyCards().Children().Append(Card);
    }
}

void MainWindow::RenderDevices() {
    DeviceCards().Children().Clear();
    NoDevicesText().Visibility(TrustedDevices_.empty()
        ? Microsoft::UI::Xaml::Visibility::Visible
        : Microsoft::UI::Xaml::Visibility::Collapsed);
    for (const auto& Device : TrustedDevices_) {
        using namespace Microsoft::UI::Xaml;
        using namespace Microsoft::UI::Xaml::Controls;
        Border Card;
        Card.Padding(Thickness{16});
        Card.CornerRadius(CornerRadius{8});
        Card.BorderThickness(Thickness{1});
        StackPanel Contents;
        Contents.Spacing(8);
        TextBlock Name;
        Name.Text(ToHString(Device.DisplayName));
        Name.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        Contents.Children().Append(Name);
        TextBlock Trust;
        Trust.Text(L"Paired · Authenticated stored identity");
        Contents.Children().Append(Trust);
        TextBlock Permissions;
        Permissions.Text(CapabilitySummary(Device.Capabilities));
        Permissions.TextWrapping(TextWrapping::Wrap);
        Contents.Children().Append(Permissions);
        StackPanel Actions;
        Actions.Orientation(Orientation::Horizontal);
        Actions.Spacing(10);
        Button Change;
        Change.Content(winrt::box_value(L"Change permissions"));
        Change.Tag(winrt::box_value(MachineTag(Device.Machine)));
        Change.Click({this, &MainWindow::OnDevicePermissions});
        Actions.Children().Append(Change);
        Button Forget;
        Forget.Content(winrt::box_value(L"Forget this PC"));
        Forget.Tag(winrt::box_value(MachineTag(Device.Machine)));
        Forget.Click({this, &MainWindow::OnDeviceForget});
        Actions.Children().Append(Forget);
        Contents.Children().Append(Actions);
        Card.Child(Contents);
        DeviceCards().Children().Append(Card);
    }
}

void MainWindow::UpdateHome() {
    if (TrustedDevices_.empty()) {
        PeerPcName().Text(L"No paired PC");
        PeerStatusText().Text(L"Add a PC to begin");
    } else {
        PeerPcName().Text(ToHString(TrustedDevices_.front().DisplayName));
        PeerStatusText().Text(
            State_ == desklink::ProductShellState::ConnectedLocal ||
                    State_ == desklink::ProductShellState::RemoteFocus
                ? L"Paired · Connected now"
                : L"Paired · Offline");
    }
}

void MainWindow::ShowPairingStatus(
    winrt::hstring const& Message,
    Microsoft::UI::Xaml::Controls::InfoBarSeverity Severity) {
    PairingStatusBar().Message(Message);
    PairingStatusBar().Severity(Severity);
    PairingStatusBar().IsOpen(true);
}

Windows::Foundation::IAsyncAction MainWindow::ShowPairingCandidate(
    desklink::ControlPairingCandidate Candidate) {
    auto Lifetime = get_strong();
    ModalDialogActive_ = true;
    PairingDialogActive_ = true;
    using namespace Microsoft::UI::Xaml::Controls;
    ContentDialog Dialog;
    PairingDialog_ = Dialog;
    Dialog.XamlRoot(Content().XamlRoot());
    Dialog.Title(winrt::box_value(L"Compare this code on both PCs"));
    Dialog.PrimaryButtonText(L"Allow");
    Dialog.CloseButtonText(L"Cancel");
    Dialog.DefaultButton(ContentDialogButton::Close);
    StackPanel Details;
    Details.Spacing(10);
    TextBlock Code;
    Code.Text(ToHString(Candidate.VerificationCode));
    Code.FontSize(36);
    Code.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
    Code.HorizontalAlignment(Microsoft::UI::Xaml::HorizontalAlignment::Center);
    Details.Children().Append(Code);
    TextBlock Peer;
    Peer.Text(JoinText(L"Remote PC: ", ToHString(Candidate.DisplayName)));
    Details.Children().Append(Peer);
    TextBlock Source;
    switch (Candidate.Source) {
        case desklink::ControlPairingSource::Nearby:
            Source.Text(L"Address source: untrusted Nearby discovery");
            break;
        case desklink::ControlPairingSource::Manual:
            Source.Text(L"Address source: manual entry");
            break;
        default:
            Source.Text(L"Address source: incoming pairing request");
            break;
    }
    Details.Children().Append(Source);
    TextBlock Consequences;
    Consequences.Text(JoinText(
        L"If you allow, this PC grants:\n",
        CapabilitySummary(Candidate.RequestedCapabilities)));
    Consequences.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
    Details.Children().Append(Consequences);
    TextBlock Warning;
    Warning.Text(
        L"Select Allow only if the other PC shows the same code and these local consequences are correct.");
    Warning.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
    Details.Children().Append(Warning);
    Dialog.Content(Details);
    const auto Result = co_await Dialog.ShowAsync();
    PairingDialogActive_ = false;
    PairingDialog_ = nullptr;
    ModalDialogActive_ = false;
    const bool Approved = Result == ContentDialogResult::Primary;
    const auto Response = Send(
        desklink::ResolvePairingCandidateControlRequest{
            Candidate.OperationId, Approved});
    if (!Response || Response->Status != desklink::ControlStatus::Ok) {
        ShowPairingStatus(
            L"The pairing request expired or was rejected. No permission was granted.",
            InfoBarSeverity::Warning);
    } else {
        ShowPairingStatus(
            Approved
                ? L"Local approval sent. Pairing is not complete until both PCs persist trust."
                : L"Pairing canceled. No permission was granted.",
            Approved ? InfoBarSeverity::Informational
                     : InfoBarSeverity::Warning);
    }
    DisplayedPairingOperation_ = 0;
}

void MainWindow::OnDevicePermissions(
    Windows::Foundation::IInspectable const& Sender,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    if (ModalDialogActive_) return;
    const auto Button = Sender.try_as<Microsoft::UI::Xaml::Controls::Button>();
    const auto Machine = Button ? MachineFromTag(Button.Tag()) : std::nullopt;
    if (!Machine) return;
    const auto Device = std::find_if(
        TrustedDevices_.begin(), TrustedDevices_.end(),
        [&](const auto& Value) { return Value.Machine == *Machine; });
    if (Device != TrustedDevices_.end()) {
        (void)ShowPermissionEditor(*Device);
    }
}

Windows::Foundation::IAsyncAction MainWindow::ShowPermissionEditor(
    desklink::ControlTrustedDevice Device) {
    auto Lifetime = get_strong();
    ModalDialogActive_ = true;
    using namespace Microsoft::UI::Xaml::Controls;
    ContentDialog Dialog;
    Dialog.XamlRoot(Content().XamlRoot());
    Dialog.Title(winrt::box_value(JoinText(
        L"Permissions for ", ToHString(Device.DisplayName))));
    Dialog.PrimaryButtonText(L"Apply");
    Dialog.CloseButtonText(L"Cancel");
    Dialog.DefaultButton(ContentDialogButton::Close);
    StackPanel Controls;
    Controls.Spacing(8);
    auto AddPermission = [&](winrt::hstring const& Text,
                             desklink::Capability Capability) {
        CheckBox Box;
        Box.Content(winrt::box_value(Text));
        Box.IsChecked(Device.Capabilities.contains(Capability));
        Box.Tag(winrt::box_value(static_cast<std::uint64_t>(Capability)));
        Controls.Children().Append(Box);
    };
    AddPermission(L"Can control this PC", desklink::Capability::InputInject);
    AddPermission(L"Display layout shared",
                  desklink::Capability::DisplayTopologyExchange);
    AddPermission(L"May read text clipboard",
                  desklink::Capability::ClipboardRead);
    AddPermission(L"May replace text clipboard",
                  desklink::Capability::ClipboardWrite);
    AddPermission(L"May play audio into this PC",
                  desklink::Capability::AudioSend);
    AddPermission(L"May receive audio from this PC",
                  desklink::Capability::AudioReceive);
    TextBlock Note;
    Note.Text(
        L"Removing a permission returns input Local and stops the active peer before saving. Adding authority requires a new two-PC pairing approval.");
    Note.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
    Controls.Children().Append(Note);
    Dialog.Content(Controls);
    const auto Result = co_await Dialog.ShowAsync();
    ModalDialogActive_ = false;
    if (Result != ContentDialogResult::Primary) co_return;
    desklink::CapabilitySet Desired;
    for (const auto& Child : Controls.Children()) {
        const auto Box = Child.try_as<CheckBox>();
        if (!Box || !IsChecked(Box)) continue;
        const auto Raw = winrt::unbox_value_or<std::uint64_t>(Box.Tag(), 0);
        Desired.grant(static_cast<desklink::Capability>(Raw));
    }
    const auto Response = Send(
        desklink::RequestLocalPermissionChangeControlRequest{
            Device.Machine, Desired});
    if (Response && Response->Status == desklink::ControlStatus::Ok) {
        DeviceStatusBar().Title(L"Permissions updated");
        DeviceStatusBar().Message(
            L"The reduction was applied after fail-local cleanup.");
        DeviceStatusBar().Severity(InfoBarSeverity::Success);
    } else if (Response && Response->Status ==
        desklink::ControlStatus::ReauthorizationRequired) {
        DeviceStatusBar().Title(L"Pair again to add permission");
        DeviceStatusBar().Message(
            L"DeskLink did not add authority. Open Add a PC and approve the new local consequences on both PCs.");
        DeviceStatusBar().Severity(InfoBarSeverity::Warning);
    } else {
        DeviceStatusBar().Title(L"Permissions unchanged");
        DeviceStatusBar().Message(
            L"Fail-local cleanup or the protected trust update did not complete.");
        DeviceStatusBar().Severity(InfoBarSeverity::Error);
    }
    DeviceStatusBar().IsOpen(true);
    PollDevices();
}

void MainWindow::OnDeviceForget(
    Windows::Foundation::IInspectable const& Sender,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    if (ModalDialogActive_) return;
    const auto Button = Sender.try_as<Microsoft::UI::Xaml::Controls::Button>();
    const auto Machine = Button ? MachineFromTag(Button.Tag()) : std::nullopt;
    if (!Machine) return;
    const auto Device = std::find_if(
        TrustedDevices_.begin(), TrustedDevices_.end(),
        [&](const auto& Value) { return Value.Machine == *Machine; });
    if (Device != TrustedDevices_.end()) (void)ConfirmForget(*Device);
}

Windows::Foundation::IAsyncAction MainWindow::ConfirmForget(
    desklink::ControlTrustedDevice Device) {
    auto Lifetime = get_strong();
    ModalDialogActive_ = true;
    using namespace Microsoft::UI::Xaml::Controls;
    ContentDialog Dialog;
    Dialog.XamlRoot(Content().XamlRoot());
    Dialog.Title(winrt::box_value(JoinText(
        L"Forget ", ToHString(Device.DisplayName), L"?")));
    Dialog.Content(winrt::box_value(
        L"DeskLink will return input to this PC, stop the active peer, and remove its stored trust. A nearby advertisement will remain unverified."));
    Dialog.PrimaryButtonText(L"Forget this PC");
    Dialog.CloseButtonText(L"Cancel");
    Dialog.DefaultButton(ContentDialogButton::Close);
    const auto Result = co_await Dialog.ShowAsync();
    ModalDialogActive_ = false;
    if (Result != ContentDialogResult::Primary) co_return;
    const auto Response = Send(
        desklink::ForgetTrustedDeviceControlRequest{Device.Machine});
    DeviceStatusBar().IsOpen(true);
    if (Response && Response->Status == desklink::ControlStatus::Ok) {
        DeviceStatusBar().Title(L"PC forgotten");
        DeviceStatusBar().Message(
            L"Trust was removed after fail-local cleanup.");
        DeviceStatusBar().Severity(InfoBarSeverity::Success);
    } else {
        DeviceStatusBar().Title(L"PC was not forgotten");
        DeviceStatusBar().Message(
            L"Cleanup or the protected trust update did not complete.");
        DeviceStatusBar().Severity(InfoBarSeverity::Error);
    }
    PollDevices();
}

void MainWindow::OnReturnLocal(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    const auto Response = Send(desklink::ReturnLocalControlRequest{});
    if (Response && Response->Status == desklink::ControlStatus::Ok) {
        ApplyState(desklink::ProductShellState::ConnectedLocal);
    }
}

void MainWindow::ApplyState(desklink::ProductShellState State) {
    State_ = State;
    const auto Presentation = desklink::PresentProductShellState(State);
    StatusBadge().Text(Presentation.Badge);
    InputTitle().Text(Presentation.KeyboardAndMouseTitle);
    InputSummary().Text(Presentation.KeyboardAndMouseSummary);
    DiagnosticRuntimePhase().Text(Presentation.ConnectionDetail);
    ReturnLocalButton().Visibility(Presentation.ShowReturnLocal
        ? Microsoft::UI::Xaml::Visibility::Visible
        : Microsoft::UI::Xaml::Visibility::Collapsed);
    ActionRequiredBar().IsOpen(Presentation.ShowActionRequired);
    UpdateHome();
    if (TrayActive_) {
        StringCchPrintfW(
            TrayIcon_.szTip, ARRAYSIZE(TrayIcon_.szTip),
            L"DeskLink — %.*s",
            static_cast<int>(Presentation.Badge.size()),
            Presentation.Badge.data());
        Shell_NotifyIconW(NIM_MODIFY, &TrayIcon_);
    }
}

bool MainWindow::CreateLifecycleWindow() {
    WNDCLASSEXW Class{sizeof(Class)};
    Class.lpfnWndProc = LifecycleWindowProcedure;
    Class.hInstance = GetModuleHandleW(nullptr);
    Class.lpszClassName = kLifecycleWindowClass;
    if (!RegisterClassExW(&Class) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    LifecycleWindow_ = CreateWindowExW(
        0, kLifecycleWindowClass, L"DeskLink product shell lifecycle", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), this);
    return LifecycleWindow_ != nullptr;
}

bool MainWindow::AddTrayIcon() {
    TrayIcon_.cbSize = sizeof(TrayIcon_);
    TrayIcon_.hWnd = LifecycleWindow_;
    TrayIcon_.uID = 1;
    TrayIcon_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    TrayIcon_.uCallbackMessage = kTrayMessage;
    TrayIcon_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    StringCchCopyW(
        TrayIcon_.szTip, ARRAYSIZE(TrayIcon_.szTip), L"DeskLink — Starting");
    TrayIcon_.uVersion = NOTIFYICON_VERSION_4;
    if (!Shell_NotifyIconW(NIM_ADD, &TrayIcon_)) return false;
    Shell_NotifyIconW(NIM_SETVERSION, &TrayIcon_);
    TrayActive_ = true;
    return true;
}

void MainWindow::RemoveTrayIcon() noexcept {
    if (!TrayActive_) return;
    Shell_NotifyIconW(NIM_DELETE, &TrayIcon_);
    TrayActive_ = false;
}

void MainWindow::HideToTray() {
    if (MainWindowHandle_) ShowWindow(MainWindowHandle_, SW_HIDE);
}

void MainWindow::ShowFromTray() {
    if (!MainWindowHandle_) return;
    ShowWindow(MainWindowHandle_, SW_SHOW);
    SetForegroundWindow(MainWindowHandle_);
}

void MainWindow::TogglePaused() {
    const bool Resume = State_ == desklink::ProductShellState::Paused;
    const auto Response = Send(Resume
        ? desklink::ControlRequestPayload(
              desklink::ResumeDeskLinkControlRequest{})
        : desklink::ControlRequestPayload(
              desklink::PauseDeskLinkControlRequest{}));
    if (Response && Response->Status == desklink::ControlStatus::Ok) {
        ApplyState(Resume ? desklink::ProductShellState::Offline
                          : desklink::ProductShellState::Paused);
    }
}

void MainWindow::ShowTrayMenu() {
    const auto Menu = CreatePopupMenu();
    if (!Menu) return;
    AppendMenuW(Menu, MF_STRING, kTrayOpen, L"Open DeskLink");
    AppendMenuW(Menu, MF_STRING, kTrayReturnLocal, L"Return to this PC");
    AppendMenuW(
        Menu, MF_STRING, kTrayPause,
        State_ == desklink::ProductShellState::Paused
            ? L"Resume DeskLink" : L"Pause DeskLink");
    AppendMenuW(Menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(Menu, MF_STRING, kTrayExit, L"Exit");
    POINT Cursor{};
    GetCursorPos(&Cursor);
    SetForegroundWindow(LifecycleWindow_);
    const auto Command = TrackPopupMenu(
        Menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
        Cursor.x, Cursor.y, 0, LifecycleWindow_, nullptr);
    DestroyMenu(Menu);
    switch (Command) {
        case kTrayOpen: ShowFromTray(); break;
        case kTrayReturnLocal:
            (void)Send(desklink::ReturnLocalControlRequest{});
            PollBroker();
            break;
        case kTrayPause: TogglePaused(); break;
        case kTrayExit: RequestExit(); break;
        default: break;
    }
}

void MainWindow::RequestExit() {
    if (ExplicitExit_) return;
    ExplicitExit_ = true;
    if (PollTimer_) PollTimer_.Stop();
    if (PairingDialog_) PairingDialog_.Hide();
    RemoveTrayIcon();
    if (MainWindowHandle_) SendMessageW(MainWindowHandle_, WM_CLOSE, 0, 0);
    Microsoft::UI::Xaml::Application::Current().Exit();
}

LRESULT CALLBACK MainWindow::MainWindowSubclassProcedure(
    HWND Window, UINT Message, WPARAM WParam, LPARAM LParam,
    UINT_PTR, DWORD_PTR ReferenceData) {
    const auto Self = reinterpret_cast<MainWindow*>(ReferenceData);
    if (Message == WM_CLOSE && Self && !Self->ExplicitExit_) {
        Self->HideToTray();
        return 0;
    }
    if (Message == WM_NCDESTROY) {
        RemoveWindowSubclass(Window, MainWindowSubclassProcedure, 1);
        if (Self) Self->MainWindowHandle_ = nullptr;
    }
    return DefSubclassProc(Window, Message, WParam, LParam);
}

LRESULT CALLBACK MainWindow::LifecycleWindowProcedure(
    HWND Window, UINT Message, WPARAM WParam, LPARAM LParam) {
    auto Self = reinterpret_cast<MainWindow*>(
        GetWindowLongPtrW(Window, GWLP_USERDATA));
    if (Message == WM_NCCREATE) {
        const auto Create = reinterpret_cast<CREATESTRUCTW*>(LParam);
        Self = static_cast<MainWindow*>(Create->lpCreateParams);
        SetWindowLongPtrW(
            Window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(Self));
    }
    if (!Self) return DefWindowProcW(Window, Message, WParam, LParam);
    if (Message == GetActivateMessage()) {
        Self->ShowFromTray();
        return 0;
    }
    if (Message == GetExitMessage() ||
        Message == GetPrepareUpdateMessage()) {
        Self->RequestExit();
        return 0;
    }
    if (Message == kTrayMessage) {
        switch (LOWORD(LParam)) {
            case WM_LBUTTONDBLCLK:
                Self->ShowFromTray();
                return 0;
            case WM_CONTEXTMENU:
                Self->ShowTrayMenu();
                return 0;
            default: break;
        }
    }
    return DefWindowProcW(Window, Message, WParam, LParam);
}

} // namespace winrt::DeskLink::Product::implementation
