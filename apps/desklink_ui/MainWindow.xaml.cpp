#include "pch.h"
#include "MainWindow.xaml.h"
#include "MainWindow.g.cpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

namespace {

constexpr wchar_t kLifecycleWindowClass[] =
    L"DeskLinkShellLifecycleWindow.v1";
constexpr auto kFocusPresentationTimeout = std::chrono::seconds(4);

bool IsTerminalPairingPhase(
    desklink::ControlPairingPhase Phase) noexcept {
    return Phase == desklink::ControlPairingPhase::Succeeded ||
           Phase == desklink::ControlPairingPhase::Canceled ||
           Phase == desklink::ControlPairingPhase::TimedOut ||
           Phase == desklink::ControlPairingPhase::Failed;
}

const wchar_t* ProfileModeName(desklink::DeskMode Mode) noexcept {
    switch (Mode) {
        case desklink::DeskMode::LockPc1: return L"Keep on this PC";
        case desklink::DeskMode::Roam: return L"Allow roaming";
        case desklink::DeskMode::Game: return L"Game mode";
        case desklink::DeskMode::LockPc2: return L"Keep on paired PC";
    }
    return L"Invalid mode";
}

bool ReceivesPeerAudio(
    desklink::AudioRoutePreference Route) noexcept {
    return Route == desklink::AudioRoutePreference::PeerToLocal ||
           Route == desklink::AudioRoutePreference::Bidirectional;
}

bool SendsLocalAudio(
    desklink::AudioRoutePreference Route) noexcept {
    return Route == desklink::AudioRoutePreference::LocalToPeer ||
           Route == desklink::AudioRoutePreference::Bidirectional;
}

bool ReceivesPeerVoice(desklink::VoiceRoutePreference Route) noexcept {
    return Route == desklink::VoiceRoutePreference::PeerToLocal ||
           Route == desklink::VoiceRoutePreference::Bidirectional;
}

bool SendsLocalVoice(desklink::VoiceRoutePreference Route) noexcept {
    return Route == desklink::VoiceRoutePreference::LocalToPeer ||
           Route == desklink::VoiceRoutePreference::Bidirectional;
}

bool UsesVoiceMonitor(
    desklink::VoiceReceiveDestination Destination) noexcept {
    return Destination ==
               desklink::VoiceReceiveDestination::CommunicationsPlayback ||
           Destination == desklink::VoiceReceiveDestination::
               CommunicationsPlaybackAndVirtualMicrophone;
}

bool UsesVirtualMicrophone(
    desklink::VoiceReceiveDestination Destination) noexcept {
    return Destination == desklink::VoiceReceiveDestination::VirtualMicrophone ||
           Destination == desklink::VoiceReceiveDestination::
               CommunicationsPlaybackAndVirtualMicrophone;
}

std::optional<std::filesystem::path> GetExecutablePath() {
    std::wstring Buffer(32'768, L'\0');
    const auto Length = GetModuleFileNameW(
        nullptr, Buffer.data(), static_cast<DWORD>(Buffer.size()));
    if (Length == 0 || Length >= Buffer.size()) return std::nullopt;
    Buffer.resize(Length);
    return std::filesystem::path(std::move(Buffer));
}

std::optional<std::filesystem::path> GetDataDirectory() {
    PWSTR RawPath{};
    if (FAILED(SHGetKnownFolderPath(
            FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &RawPath)) ||
        !RawPath) {
        return std::nullopt;
    }
    std::filesystem::path Result(RawPath);
    CoTaskMemFree(RawPath);
    Result /= L"DeskLink";
    std::error_code Error;
    std::filesystem::create_directories(Result, Error);
    return Error ? std::nullopt : std::optional(Result);
}

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

const wchar_t* MonitorSideName(desklink::DisplayEdgeSide Side) noexcept {
    switch (Side) {
        case desklink::DisplayEdgeSide::Left: return L"left";
        case desklink::DisplayEdgeSide::Top: return L"top";
        case desklink::DisplayEdgeSide::Right: return L"right";
        case desklink::DisplayEdgeSide::Bottom: return L"bottom";
    }
    return L"unknown";
}

const wchar_t* DeveloperRoamingStateName(
    desklink::ControlRoamingState State) noexcept {
    switch (State) {
        case desklink::ControlRoamingState::Unavailable: return L"unavailable";
        case desklink::ControlRoamingState::Local: return L"local";
        case desklink::ControlRoamingState::EdgeCandidate: return L"edge-candidate";
        case desklink::ControlRoamingState::FocusPending: return L"focus-pending";
        case desklink::ControlRoamingState::RemoteReady: return L"remote-ready";
        case desklink::ControlRoamingState::Remote: return L"remote";
        case desklink::ControlRoamingState::ReturnPending: return L"return-pending";
        case desklink::ControlRoamingState::LocalCooldown: return L"local-cooldown";
    }
    return L"invalid";
}

const wchar_t* DeveloperPeerDirectionName(
    desklink::ControlPeerDirectionState State) noexcept {
    switch (State) {
        case desklink::ControlPeerDirectionState::Unavailable: return L"unavailable";
        case desklink::ControlPeerDirectionState::Local: return L"local";
        case desklink::ControlPeerDirectionState::OutgoingPending: return L"outgoing-pending";
        case desklink::ControlPeerDirectionState::OutgoingActive: return L"outgoing-active";
        case desklink::ControlPeerDirectionState::IncomingActive: return L"incoming-active";
    }
    return L"invalid";
}

bool HasRoamingDirection(
    const desklink::RoamingLink& Link,
    desklink::RoamingDirection Direction) noexcept {
    return Direction == desklink::RoamingDirection::AToB
        ? Link.Direction == desklink::RoamingDirectionMode::AToB ||
              Link.Direction == desklink::RoamingDirectionMode::Bidirectional
        : Link.Direction == desklink::RoamingDirectionMode::BToA ||
              Link.Direction == desklink::RoamingDirectionMode::Bidirectional;
}

winrt::hstring RefreshRateText(std::uint32_t MilliHertz) {
    if (MilliHertz == 0) return L"refresh unknown";
    std::wostringstream Output;
    if (MilliHertz % 1'000u == 0) {
        Output << MilliHertz / 1'000u;
    } else {
        Output.setf(std::ios::fixed);
        Output.precision(1);
        Output << static_cast<double>(MilliHertz) / 1'000.0;
    }
    Output << L" Hz";
    return winrt::hstring(Output.str());
}

bool SameMonitorEndpoint(
    const desklink::RoamingEndpoint& Left,
    const desklink::RoamingEndpoint& Right) noexcept {
    return Left.Machine == Right.Machine &&
           Left.StableDisplayIdentity == Right.StableDisplayIdentity &&
           Left.Side == Right.Side;
}

bool HasEquivalentMonitorConnection(
    const desklink::RoamingConfiguration& Configuration,
    const desklink::RoamingLink& Candidate) noexcept {
    return std::any_of(
        Configuration.Links.begin(), Configuration.Links.end(),
        [&](const desklink::RoamingLink& Existing) {
            return (SameMonitorEndpoint(
                        Existing.EndpointA, Candidate.EndpointA) &&
                    SameMonitorEndpoint(
                        Existing.EndpointB, Candidate.EndpointB)) ||
                   (SameMonitorEndpoint(
                        Existing.EndpointA, Candidate.EndpointB) &&
                    SameMonitorEndpoint(
                        Existing.EndpointB, Candidate.EndpointA));
        });
}

winrt::hstring MonitorTileLabel(
    const desklink::MonitorCanvasTile& Tile,
    std::size_t Index) {
    std::wstring Result = std::to_wstring(Index + 1u) + L"  " +
        ToHString(Tile.FriendlyName).c_str() + L"\n";
    if (Tile.Online) {
        Result += std::to_wstring(Tile.PixelWidth) + L" × " +
            std::to_wstring(Tile.PixelHeight) + L" · " +
            RefreshRateText(Tile.RefreshMilliHertz).c_str();
    } else {
        Result += L"Saved display identity unavailable";
    }
    Result += L"\n";
    Result += ToHString(Tile.MachineName).c_str();
    if (Tile.Primary) Result += L" · Primary";
    if (!Tile.Online) Result += L" · Offline";
    if (Tile.SizeEstimated) Result += L" · Size estimated";
    return winrt::hstring(Result);
}

winrt::hstring MonitorDisplayChoice(
    const desklink::MonitorCanvasTile& Tile,
    std::size_t Index) {
    std::wstring Result = std::to_wstring(Index + 1u) + L" — " +
        ToHString(Tile.MachineName).c_str() + L" — " +
        ToHString(Tile.FriendlyName).c_str();
    if (!Tile.Online) Result += L" (offline)";
    return winrt::hstring(Result);
}

std::optional<std::uint16_t> PercentToPermyriad(double Value) noexcept {
    if (!std::isfinite(Value) || Value < 0.0 || Value > 100.0) {
        return std::nullopt;
    }
    const auto Rounded = std::llround(Value * 100.0);
    if (Rounded < 0 || Rounded > 10'000) return std::nullopt;
    return static_cast<std::uint16_t>(Rounded);
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
    if (Capabilities.contains(desklink::Capability::VoiceSend)) {
        Lines.emplace_back(L"May send microphone voice into this PC");
    }
    if (Capabilities.contains(desklink::Capability::VoiceReceive)) {
        Lines.emplace_back(L"May receive this PC's microphone voice");
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

const wchar_t* RuntimePhaseName(
    desklink::BrokerRuntimePhase Phase) noexcept {
    switch (Phase) {
        case desklink::BrokerRuntimePhase::Stopped:
            return L"Stopped";
        case desklink::BrokerRuntimePhase::Paused:
            return L"Paused";
        case desklink::BrokerRuntimePhase::Listening:
            return L"Listening for an authenticated peer";
        case desklink::BrokerRuntimePhase::Discovering:
            return L"Looking for the paired PC";
        case desklink::BrokerRuntimePhase::Connecting:
            return L"Performing the secure connection handshake";
        case desklink::BrokerRuntimePhase::ConnectedLocal:
            return L"Connected; input is Local";
        case desklink::BrokerRuntimePhase::RetryWaiting:
            return L"Waiting to retry an ordinary availability failure";
        case desklink::BrokerRuntimePhase::ActionRequired:
            return L"Automatic retry stopped; input is Local";
    }
    return L"Unknown";
}

const wchar_t* RuntimeFailureName(
    desklink::BrokerRuntimeFailure Failure) noexcept {
    switch (Failure) {
        case desklink::BrokerRuntimeFailure::None:
            return L"None";
        case desklink::BrokerRuntimeFailure::OrdinaryUnavailable:
            return L"The paired PC or network is temporarily unavailable";
        case desklink::BrokerRuntimeFailure::Security:
            return L"Security validation failed";
        case desklink::BrokerRuntimeFailure::Identity:
            return L"The paired identity could not be verified";
        case desklink::BrokerRuntimeFailure::Credential:
            return L"The local device credential is unavailable";
        case desklink::BrokerRuntimeFailure::Signing:
            return L"The local device identity could not sign the handshake";
        case desklink::BrokerRuntimeFailure::Authentication:
            return L"Peer authentication failed";
        case desklink::BrokerRuntimeFailure::Capability:
            return L"The negotiated permissions are incompatible";
        case desklink::BrokerRuntimeFailure::Protocol:
            return L"The authenticated transport failed a protocol check";
        case desklink::BrokerRuntimeFailure::Unknown:
            return L"The managed connection process stopped unexpectedly";
    }
    return L"Unknown";
}

std::wstring RuntimeConnectionDetail(
    const desklink::ControlState& State) {
    if (State.RuntimePhase == desklink::BrokerRuntimePhase::RetryWaiting) {
        const auto Seconds =
            (State.RetryDelayMilliseconds + 999u) / 1'000u;
        std::wostringstream Output;
        Output << L"Retry attempt " << State.RetryAttempt << L" starts in "
               << Seconds << (Seconds == 1 ? L" second." : L" seconds.");
        return Output.str();
    }
    return RuntimePhaseName(State.RuntimePhase);
}

std::wstring RuntimeRetryStatus(const desklink::ControlState& State) {
    if (State.RuntimePhase == desklink::BrokerRuntimePhase::RetryWaiting) {
        return RuntimeConnectionDetail(State);
    }
    if (State.RuntimePhase == desklink::BrokerRuntimePhase::ActionRequired) {
        return L"Stopped until you select Retry now or change the connection settings.";
    }
    if (State.RuntimePhase == desklink::BrokerRuntimePhase::ConnectedLocal) {
        return L"Not needed; the authenticated session is connected.";
    }
    if (State.RuntimePhase == desklink::BrokerRuntimePhase::Paused) {
        return L"Disabled while DeskLink is paused.";
    }
    return L"No retry is currently scheduled.";
}

std::wstring RuntimeRecommendedAction(const desklink::ControlState& State) {
    if (State.RuntimePhase == desklink::BrokerRuntimePhase::ActionRequired) {
        if (State.RuntimeFailure == desklink::BrokerRuntimeFailure::Security ||
            State.RuntimeFailure == desklink::BrokerRuntimeFailure::Identity ||
            State.RuntimeFailure == desklink::BrokerRuntimeFailure::Credential ||
            State.RuntimeFailure == desklink::BrokerRuntimeFailure::Signing ||
            State.RuntimeFailure == desklink::BrokerRuntimeFailure::Authentication) {
            return L"Verify the paired PC and its identity before retrying. DeskLink will not bypass this failure.";
        }
        return L"Confirm the paired PC is running, then select Retry now. If this category returns, keep it for the bug report.";
    }
    if (State.RuntimePhase == desklink::BrokerRuntimePhase::RetryWaiting) {
        return L"No action is required. DeskLink will retry automatically; input remains on this PC.";
    }
    if (State.RuntimePhase == desklink::BrokerRuntimePhase::Listening) {
        return L"Start DeskLink on the paired main PC. This PC is listening securely.";
    }
    if (State.RuntimePhase == desklink::BrokerRuntimePhase::Discovering ||
        State.RuntimePhase == desklink::BrokerRuntimePhase::Connecting) {
        return L"Wait for the bounded connection attempt to finish.";
    }
    if (State.RuntimePhase == desklink::BrokerRuntimePhase::ConnectedLocal) {
        return L"No action is required.";
    }
    if (State.RuntimePhase == desklink::BrokerRuntimePhase::Paused) {
        return L"Resume DeskLink when you want automatic connections again.";
    }
    return L"Check that a paired PC and automatic connection role are configured.";
}

} // namespace

namespace winrt::DeskLink::Product::implementation {

MainWindow::MainWindow() : MainWindow(false) {}

MainWindow::MainWindow(bool DeveloperMode)
    : DeveloperMode_(DeveloperMode) {
    InitializeComponent();
    ContentReady_ = true;
    Title(L"DeskLink");
    Navigation().SelectedItem(HomeNavigation());
    if (!CreateLifecycleWindow()) {
        throw winrt::hresult_error(
            E_FAIL, L"Could not initialize DeskLink lifecycle controls.");
    }
    wchar_t ComputerName[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD ComputerNameLength = static_cast<DWORD>(std::size(ComputerName));
    if (GetComputerNameW(ComputerName, &ComputerNameLength)) {
        LocalPcName().Text(winrt::hstring(
            ComputerName, ComputerNameLength));
    }
    LoadVoiceInputDevices();
    ApplyState(State_);
    PollTimer_ = DispatcherQueue().CreateTimer();
    PollTimer_.Interval(std::chrono::milliseconds{750});
    PollTimer_.Tick([this](auto&&, auto&&) { PollBroker(); });
    PollTimer_.Start();
    if (DeveloperMode_) {
        DeveloperPollTimer_ = DispatcherQueue().CreateTimer();
        DeveloperPollTimer_.Interval(std::chrono::milliseconds{100});
        DeveloperPollTimer_.Tick(
            [this](auto&&, auto&&) { UpdateDeveloperInputStatus(); });
        DeveloperPollTimer_.Start();
    }
    PollBroker();
    UpdateDeveloperInputStatus();
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
    if (DeveloperPollTimer_) DeveloperPollTimer_.Stop();
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
    const auto Now = std::chrono::steady_clock::now();
    const auto Response = Send(
        desklink::GetStateControlRequest{},
        desklink::kProductBrokerStateTimeout);
    const bool Available = Response &&
        Response->Status == desklink::ControlStatus::Ok && Response->State;
    if (!Available) {
        if (!BrokerAvailability_.ObserveUnavailable(Now)) return;
        BrokerAvailable_ = false;
        BrokerUnavailableBar().IsOpen(true);
        RuntimeStateLoaded_ = false;
        ApplyState(desklink::ProductShellState::Offline);
        UpdateFeatureControls();
        if (PairingDialogActive_ && PairingDialog_) PairingDialog_.Hide();
        return;
    }
    BrokerAvailability_.ObserveAvailable(Now);
    BrokerAvailable_ = true;
    BrokerUnavailableBar().IsOpen(false);
    const bool FeatureStateChanged = !RuntimeStateLoaded_ ||
        RuntimeState_.ConnectedPeerCount !=
            Response->State->ConnectedPeerCount ||
        RuntimeState_.AudioGainPermyriad !=
            Response->State->AudioGainPermyriad ||
        RuntimeState_.AudioMuted != Response->State->AudioMuted ||
        RuntimeState_.VoiceGainPermyriad !=
            Response->State->VoiceGainPermyriad ||
        RuntimeState_.VoiceMuted != Response->State->VoiceMuted ||
        RuntimeState_.VoicePttReady != Response->State->VoicePttReady ||
        RuntimeState_.VoiceTransmitting !=
            Response->State->VoiceTransmitting ||
        RuntimeState_.VoiceInputUnavailable !=
            Response->State->VoiceInputUnavailable ||
        RuntimeState_.VoicePermissionMissing !=
            Response->State->VoicePermissionMissing;
    const bool ConnectionStateChanged = !RuntimeStateLoaded_ ||
        RuntimeState_.ConnectedPeerCount !=
            Response->State->ConnectedPeerCount;
    RuntimeState_ = *Response->State;
    RuntimeStateLoaded_ = true;
    LocalMachine_ = Response->State->LocalMachine;
    BrokerPaused_ = Response->State->RuntimePhase ==
        desklink::BrokerRuntimePhase::Paused;
    ApplyState(StateFromControl(*Response->State));
    UpdateFocusRequestPresentation(Now);
    if (FeatureStateChanged) UpdateFeatureControls();
    if (ConnectionStateChanged && DevicesLoaded_) {
        RenderDevices();
        RenderNearby();
    }
    PollPreferences();
    PollDevices();
    PollNearby();
    PollPairingOperation();
    PollPairingCandidate();
    PollPermissionCandidate();
}

void MainWindow::PollPreferences() {
    const auto Response = Send(desklink::GetProductPreferencesControlRequest{});
    if (!Response || Response->Status != desklink::ControlStatus::Ok ||
        !Response->Preferences) {
        return;
    }
    const bool WasLoaded = PreferencesLoaded_;
    const bool Changed = !WasLoaded || Preferences_ != *Response->Preferences;
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
    if (Changed) {
        UpdateFeatureControls();
        RenderProfiles();
        if (DevicesLoaded_) {
            RenderDevices();
            RenderNearby();
        }
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
        RenderNearby();
        UpdateHome();
        UpdateFeatureControls();
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
    // A missed control-pipe poll is not a pairing outcome. Keep the protected
    // comparison prompt open until the broker reports an explicit terminal
    // operation phase; otherwise a transient runtime heartbeat can silently
    // turn an in-progress approval into a cancellation.
}

void MainWindow::PollPairingOperation() {
    const auto Response = Send(
        desklink::GetPairingOperationControlRequest{});
    if (!Response || Response->Status != desklink::ControlStatus::Ok ||
        !Response->PairingOperation ||
        (PresentedPairingOperation_ &&
         *PresentedPairingOperation_ == *Response->PairingOperation)) {
        return;
    }
    PresentedPairingOperation_ = *Response->PairingOperation;
    using Microsoft::UI::Xaml::Controls::InfoBarSeverity;
    const auto Phase = Response->PairingOperation->Phase;
    switch (Phase) {
        case desklink::ControlPairingPhase::WaitingForPeer:
            PairingStatusBar().Title(L"Pairing started");
            ShowPairingStatus(
                L"Waiting for the secure handshake. This operation times out automatically.",
                InfoBarSeverity::Informational);
            break;
        case desklink::ControlPairingPhase::VerificationRequired:
            PairingStatusBar().Title(L"Verification required");
            ShowPairingStatus(
                L"Compare the code on both PCs and approve only matching codes.",
                InfoBarSeverity::Informational);
            break;
        case desklink::ControlPairingPhase::AwaitingPeer:
            PairingStatusBar().Title(L"Waiting for the other PC");
            ShowPairingStatus(
                L"This PC approved the code. Pairing completes only after the other PC approves and both trust records are stored.",
                InfoBarSeverity::Informational);
            break;
        case desklink::ControlPairingPhase::Succeeded:
            if (PairingDialogActive_ && PairingDialog_) PairingDialog_.Hide();
            PairingStatusBar().Title(L"Pairing complete");
            ShowPairingStatus(
                L"Both PCs stored the authenticated identity and permissions. DeskLink is reconnecting normally.",
                InfoBarSeverity::Success);
            PollDevices();
            break;
        case desklink::ControlPairingPhase::Canceled:
            if (PairingDialogActive_ && PairingDialog_) PairingDialog_.Hide();
            PairingStatusBar().Title(L"Pairing canceled");
            ShowPairingStatus(
                L"No new trust or permission was granted.",
                InfoBarSeverity::Warning);
            break;
        case desklink::ControlPairingPhase::TimedOut:
            if (PairingDialogActive_ && PairingDialog_) PairingDialog_.Hide();
            PairingStatusBar().Title(L"Pairing timed out");
            ShowPairingStatus(
                L"The handshake or two-PC approval did not finish before the five-minute deadline. No new trust was admitted.",
                InfoBarSeverity::Warning);
            break;
        case desklink::ControlPairingPhase::Failed:
            if (PairingDialogActive_ && PairingDialog_) PairingDialog_.Hide();
            PairingStatusBar().Title(L"Pairing failed");
            ShowPairingStatus(
                L"The secure handshake or trust persistence failed. No partially authenticated session was admitted.",
                InfoBarSeverity::Error);
            break;
    }
}

void MainWindow::PollPermissionCandidate() {
    const auto Response = Send(desklink::GetPermissionCandidateControlRequest{});
    if (!Response || Response->Status != desklink::ControlStatus::Ok ||
        !Response->PermissionCandidate || ModalDialogActive_ ||
        DisplayedPermissionOperation_ ==
            Response->PermissionCandidate->OperationId) {
        return;
    }
    DisplayedPermissionOperation_ =
        Response->PermissionCandidate->OperationId;
    (void)ShowPermissionCandidate(*Response->PermissionCandidate);
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
    DisplaysPage().Visibility(Tag == L"Displays" ? Visible : Collapsed);
    AdvancedPage().Visibility(Tag == L"Advanced" ? Visible : Collapsed);
    DiagnosticsPage().Visibility(Tag == L"Diagnostics" ? Visible : Collapsed);
    if (Tag == L"Displays" && !MonitorLayoutLoaded_) {
        LoadMonitorLayout();
    }
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

void MainWindow::OnOpenDisplays(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    Navigation().SelectedItem(DisplaysNavigation());
    NavigateTo(L"Displays");
}

void MainWindow::OnOpenDiagnostics(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    Navigation().SelectedItem(DiagnosticsNavigation());
    NavigateTo(L"Diagnostics");
    RunConnectionCheck();
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
    if (IsChecked(GrantVoiceSendCheck())) {
        Result.grant(desklink::Capability::VoiceSend);
    }
    if (IsChecked(GrantVoiceReceiveCheck())) {
        Result.grant(desklink::Capability::VoiceReceive);
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
            ? L"Waiting for a new PC for up to five minutes. A comparison code appears only after the secure handshake reaches both PCs."
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
            ? L"Waiting for the other PC to complete the secure handshake. The comparison code will appear here when both PCs are ready."
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
            ? L"Secure pairing started from an unverified nearby address. Wait for the comparison code to appear on both PCs."
            : L"That nearby record is no longer safe to use. Refresh the list or enter the address manually.",
        Response && Response->Status == desklink::ControlStatus::Ok
            ? Microsoft::UI::Xaml::Controls::InfoBarSeverity::Informational
            : Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error);
}

void MainWindow::OnTrustedConnect(
    Windows::Foundation::IInspectable const& Sender,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    const auto Button = Sender.try_as<Microsoft::UI::Xaml::Controls::Button>();
    const auto Machine = Button ? MachineFromTag(Button.Tag()) : std::nullopt;
    if (!Machine || !PreferencesLoaded_) return;

    if (Preferences_.Role == desklink::DeskRole::Companion) {
        Navigation().SelectedItem(DevicesNavigation());
        NavigateTo(L"Devices");
        DeviceStatusBar().Title(L"Already paired");
        DeviceStatusBar().Message(
            L"This Companion PC listens for its trusted Main PC automatically. Pairing is not required again.");
        DeviceStatusBar().Severity(
            Microsoft::UI::Xaml::Controls::InfoBarSeverity::Informational);
        DeviceStatusBar().IsOpen(true);
        return;
    }

    auto Updated = Preferences_;
    Updated.PreferredPeerMachine = *Machine;
    Updated.AutoStartRuntime = true;
    Updated.AutoConnect = true;
    const auto Response = Send(
        desklink::SetProductPreferencesControlRequest{Updated},
        std::chrono::milliseconds{2'500});
    if (!Response || Response->Status != desklink::ControlStatus::Ok) {
        DeviceStatusBar().Title(L"Connection not started");
        DeviceStatusBar().Message(
            L"DeskLink could not safely start the trusted connection. Pairing and permissions were left unchanged.");
        DeviceStatusBar().Severity(
            Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error);
        DeviceStatusBar().IsOpen(true);
        ShowPairingStatus(
            L"DeskLink could not safely start the trusted connection. Pairing and permissions were left unchanged.",
            Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error);
        return;
    }
    Preferences_ = Updated;
    DeviceStatusBar().Title(L"Connecting");
    DeviceStatusBar().Message(
        L"DeskLink is connecting with the stored authenticated identity. No pairing code is needed.");
    DeviceStatusBar().Severity(
        Microsoft::UI::Xaml::Controls::InfoBarSeverity::Informational);
    DeviceStatusBar().IsOpen(true);
    ShowPairingStatus(
        L"Connecting with the stored authenticated identity. No pairing code is needed.",
        Microsoft::UI::Xaml::Controls::InfoBarSeverity::Informational);
    RenderNearby();
    RenderDevices();
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

void MainWindow::OnSaveTrustedAddress(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    using Microsoft::UI::Xaml::Controls::InfoBarSeverity;
    const auto Device = PreferredDevice();
    const auto Host = ToUtf8(TrustedAddressHost().Text());
    const auto PortValue = TrustedAddressPort().Value();
    if (!PreferencesLoaded_ || !Device || !Host ||
        !std::isfinite(PortValue) || PortValue < 1 ||
        PortValue > 65'535 || std::floor(PortValue) != PortValue) {
        DeviceStatusBar().Title(L"Address not saved");
        DeviceStatusBar().Message(
            L"Select a paired PC and enter a valid host or IP address and UDP port.");
        DeviceStatusBar().Severity(InfoBarSeverity::Error);
        DeviceStatusBar().IsOpen(true);
        return;
    }

    desklink::ProductPeerEndpoint Endpoint{
        *Host, static_cast<std::uint16_t>(PortValue)};
    if (!desklink::IsValidProductPeerEndpoint(Endpoint)) {
        DeviceStatusBar().Title(L"Address not saved");
        DeviceStatusBar().Message(
            L"The address contains unsupported characters or is too long.");
        DeviceStatusBar().Severity(InfoBarSeverity::Error);
        DeviceStatusBar().IsOpen(true);
        return;
    }

    auto Updated = Preferences_;
    Updated.PreferredPeerMachine = Device->Machine;
    Updated.PreferredPeerEndpoint = std::move(Endpoint);
    Updated.AutoStartRuntime = true;
    Updated.AutoConnect = true;
    const auto Response = Send(
        desklink::SetProductPreferencesControlRequest{Updated},
        std::chrono::milliseconds{2'500});
    if (!Response || Response->Status != desklink::ControlStatus::Ok) {
        DeviceStatusBar().Title(L"Address not saved");
        DeviceStatusBar().Message(
            L"DeskLink could not safely reconcile the runtime. Pairing and trust were not changed.");
        DeviceStatusBar().Severity(InfoBarSeverity::Error);
        DeviceStatusBar().IsOpen(true);
        return;
    }
    Preferences_ = std::move(Updated);
    DeviceStatusBar().Title(L"Connecting by saved address");
    DeviceStatusBar().Message(
        L"Discovery remains preferred. If it misses, DeskLink will use this address only for the stored authenticated PC.");
    DeviceStatusBar().Severity(InfoBarSeverity::Success);
    DeviceStatusBar().IsOpen(true);
    RenderDevices();
}

void MainWindow::OnClearTrustedAddress(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    using Microsoft::UI::Xaml::Controls::InfoBarSeverity;
    if (!PreferencesLoaded_) return;
    auto Updated = Preferences_;
    Updated.PreferredPeerEndpoint.reset();
    const auto Response = Send(
        desklink::SetProductPreferencesControlRequest{Updated},
        std::chrono::milliseconds{2'500});
    if (!Response || Response->Status != desklink::ControlStatus::Ok) {
        DeviceStatusBar().Title(L"Address not cleared");
        DeviceStatusBar().Message(
            L"DeskLink could not safely reconcile the runtime. The saved setting is unchanged.");
        DeviceStatusBar().Severity(InfoBarSeverity::Error);
        DeviceStatusBar().IsOpen(true);
        return;
    }
    Preferences_ = std::move(Updated);
    TrustedAddressHost().Text(L"");
    TrustedAddressPort().Value(43'821);
    DeviceStatusBar().Title(L"Discovery only");
    DeviceStatusBar().Message(
        L"The explicit address was removed. The authenticated peer identity and permissions are unchanged.");
    DeviceStatusBar().Severity(InfoBarSeverity::Success);
    DeviceStatusBar().IsOpen(true);
    RenderDevices();
}

std::optional<desklink::MachineId> MainWindow::MachineFromTag(
    Windows::Foundation::IInspectable const& Tag) const {
    const auto Text = winrt::unbox_value_or<winrt::hstring>(Tag, {});
    return ParseMachineTag(Text);
}

void MainWindow::RenderNearby() {
    NearbyCards().Children().Clear();
    if (NearbyPeers_.empty() && TrustedDevices_.empty()) {
        NearbyStatus().Text(
            L"No DeskLink PCs were found. Open a pairing window on the other PC or use the manual address.");
        return;
    }
    NearbyStatus().Text(NearbyPeers_.empty()
        ? L"Your already-paired PCs are shown below and do not need another pairing code. Select Find nearby PCs to look for a new one."
        : L"New PCs are unverified. PCs with a matching stored machine identity are labeled Already paired and never enter pairing again.");

    auto VisiblePeers = NearbyPeers_;
    for (const auto& Device : TrustedDevices_) {
        const bool AlreadyVisible = std::any_of(
            VisiblePeers.begin(), VisiblePeers.end(),
            [&](const auto& Peer) { return Peer.Machine == Device.Machine; });
        if (!AlreadyVisible) {
            VisiblePeers.push_back({
                Device.Machine,
                Device.DisplayName,
                {},
                Device.Capabilities,
                0,
                desklink::kProtocolVersion,
                0,
                false,
                false});
        }
    }

    for (const auto& Peer : VisiblePeers) {
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
        const auto Trusted = std::find_if(
            TrustedDevices_.begin(), TrustedDevices_.end(),
            [&](const auto& Device) {
                return Device.Machine == Peer.Machine;
            });
        const bool ObservedNearby = std::any_of(
            NearbyPeers_.begin(), NearbyPeers_.end(),
            [&](const auto& Nearby) {
                return Nearby.Machine == Peer.Machine;
            });
        const bool AlreadyPaired = Trusted != TrustedDevices_.end();
        const bool Connected = AlreadyPaired && Trusted->Connected;
        if (AlreadyPaired && Connected) {
            Status.Text(ObservedNearby
                ? L"Nearby · Already paired · Connected now"
                : L"Already paired · Connected now");
        } else if (AlreadyPaired && Peer.Ambiguous) {
            Status.Text(L"Nearby · Already paired · Conflicting endpoints");
        } else if (AlreadyPaired) {
            Status.Text(ObservedNearby
                ? L"Nearby · Already paired · Not connected"
                : L"Already paired · Not connected");
        } else if (Peer.Ambiguous) {
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
        Connect.Content(winrt::box_value(
            AlreadyPaired
                ? Connected
                    ? L"Connected"
                    : Preferences_.Role == desklink::DeskRole::Companion
                        ? L"View"
                        : L"Connect"
                : L"Pair"));
        Connect.Tag(winrt::box_value(MachineTag(Peer.Machine)));
        Connect.IsEnabled(AlreadyPaired
            ? !Connected &&
                (Preferences_.Role == desklink::DeskRole::Companion ||
                 !Peer.Ambiguous)
            : !Peer.Ambiguous && Peer.PairingOpen &&
                Peer.EndpointCount != 0 &&
                Peer.ProtocolVersion == desklink::kProtocolVersion);
        if (AlreadyPaired) {
            Connect.Click({this, &MainWindow::OnTrustedConnect});
        } else {
            Connect.Click({this, &MainWindow::OnNearbyConnect});
        }
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
    const bool AddressAvailable = !TrustedDevices_.empty() &&
        (Preferences_.Role == desklink::DeskRole::Main ||
         Preferences_.Role == desklink::DeskRole::Flexible);
    TrustedAddressPanel().Visibility(AddressAvailable
        ? Microsoft::UI::Xaml::Visibility::Visible
        : Microsoft::UI::Xaml::Visibility::Collapsed);
    if (Preferences_.PreferredPeerEndpoint) {
        TrustedAddressHost().Text(ToHString(
            Preferences_.PreferredPeerEndpoint->Host));
        TrustedAddressPort().Value(
            Preferences_.PreferredPeerEndpoint->Port);
    }
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
        const bool Connected = Device.Connected;
        Trust.Text(Connected
            ? L"Paired · Connected now"
            : Preferences_.Role == desklink::DeskRole::Companion
                ? L"Paired · Waiting for trusted Main PC"
                : L"Paired · Not connected");
        Contents.Children().Append(Trust);
        TextBlock Permissions;
        Permissions.Text(CapabilitySummary(Device.Capabilities));
        Permissions.TextWrapping(TextWrapping::Wrap);
        Contents.Children().Append(Permissions);
        StackPanel Actions;
        Actions.Orientation(Orientation::Horizontal);
        Actions.Spacing(10);
        if (Preferences_.Role == desklink::DeskRole::Main ||
            Preferences_.Role == desklink::DeskRole::Flexible) {
            Button Connect;
            Connect.Content(winrt::box_value(
                Connected ? L"Connected" : L"Connect"));
            Connect.IsEnabled(!Connected);
            Connect.Tag(winrt::box_value(MachineTag(Device.Machine)));
            Connect.Click({this, &MainWindow::OnTrustedConnect});
            Actions.Children().Append(Connect);
        }
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

void MainWindow::ShowMonitorStatus(
    winrt::hstring const& Title,
    winrt::hstring const& Message,
    Microsoft::UI::Xaml::Controls::InfoBarSeverity Severity) {
    MonitorStatusBar().Title(Title);
    MonitorStatusBar().Message(Message);
    MonitorStatusBar().Severity(Severity);
    MonitorStatusBar().IsOpen(true);
}

void MainWindow::LoadMonitorLayout() {
    using namespace Microsoft::UI::Xaml::Controls;
    if (!BrokerAvailable_ ||
        !std::any_of(LocalMachine_.begin(), LocalMachine_.end(),
                     [](std::uint8_t Byte) { return Byte != 0; })) {
        ShowMonitorStatus(
            L"Runtime unavailable",
            L"DeskLink cannot establish the local machine identity. The saved layout was not opened.",
            InfoBarSeverity::Error);
        return;
    }

    if (!RoamingSettings_) {
        const auto Directory = GetDataDirectory();
        if (!Directory) {
            ShowMonitorStatus(
                L"Layout storage unavailable",
                L"DeskLink could not open its current-user data directory.",
                InfoBarSeverity::Error);
            return;
        }
        RoamingSettings_ =
            std::make_unique<desklink::Win32RoamingSettingsStore>(
                *Directory / L"roaming.settings");
    }
    if (!RoamingSettings_->Load()) {
        ShowMonitorStatus(
            L"Saved layout is invalid",
            L"DeskLink rejected the existing layout rather than guessing at its meaning. Input remains Local.",
            InfoBarSeverity::Error);
        return;
    }
    const auto Configuration = RoamingSettings_->Current();
    if (!Configuration) {
        ShowMonitorStatus(
            L"Saved layout unavailable",
            L"DeskLink could not read a validated roaming configuration.",
            InfoBarSeverity::Error);
        return;
    }

    desklink::ControlTopologyState TopologyState;
    const auto Response = Send(
        desklink::GetDisplayTopologiesControlRequest{},
        std::chrono::milliseconds{750});
    bool RemoteLayoutsAvailable = Response &&
        Response->Status == desklink::ControlStatus::Ok &&
        Response->Topologies;
    if (RemoteLayoutsAvailable) {
        TopologyState = *Response->Topologies;
    } else {
        desklink::Win32DisplayTopology LocalTopology;
        if (!LocalTopology.Refresh()) {
            ShowMonitorStatus(
                L"Display enumeration failed",
                L"No layout was changed. Reconnect the displays and try again.",
                InfoBarSeverity::Error);
            return;
        }
        TopologyState.Machines.push_back(desklink::ControlMachineTopology{
            LocalMachine_, desklink::DisplayTopologyExchangeStatus::Ready,
            LocalTopology.Current(), true, false});
    }

    MonitorMachines_.clear();
    const auto LocalName = ToUtf8(LocalPcName().Text()).value_or("This PC");
    for (auto& Entry : TopologyState.Machines) {
        std::string Name = LocalName;
        if (!Entry.Local) {
            const auto Trusted = std::find_if(
                TrustedDevices_.begin(), TrustedDevices_.end(),
                [&](const auto& Device) {
                    return Device.Machine == Entry.Machine;
                });
            Name = Trusted == TrustedDevices_.end()
                ? "Paired PC"
                : Trusted->DisplayName;
        }
        MonitorMachines_.push_back(desklink::MonitorCanvasMachine{
            Entry.Machine, std::move(Name), std::move(Entry.Topology),
            Entry.Status, Entry.Local, Entry.PeerInputAllowed});
    }
    const auto Model = desklink::BuildMonitorCanvasModel(
        MonitorMachines_, *Configuration);
    if (!Model) {
        ShowMonitorStatus(
            L"Layouts were rejected",
            L"The current and saved display records did not form a bounded, unambiguous canvas.",
            InfoBarSeverity::Error);
        return;
    }

    MonitorConfiguration_ = *Configuration;
    MonitorModel_ = *Model;
    MonitorLayoutLoaded_ = true;
    MonitorLayoutDirty_ = false;
    DraggingMonitorTile_.reset();
    RecomputeMonitorSuggestion(false);
    RenderMonitorEditors();
    RenderMonitorRoutes();
    RenderMonitorCanvas();
    MonitorUnsavedText().Text(L"No unsaved changes.");
    ShowMonitorStatus(
        RemoteLayoutsAvailable ? L"Layouts refreshed" : L"Local layout loaded",
        RemoteLayoutsAvailable
            ? L"Authenticated current topologies are shown. Offline saved displays remain visible."
            : L"The peer topology is unavailable. Saved peer displays remain offline and cannot produce a new snap proposal.",
        RemoteLayoutsAvailable
            ? InfoBarSeverity::Success
            : InfoBarSeverity::Warning);
}

void MainWindow::RenderMonitorCanvas() {
    using namespace Microsoft::UI;
    using namespace Microsoft::UI::Xaml;
    using namespace Microsoft::UI::Xaml::Automation;
    using namespace Microsoft::UI::Xaml::Controls;
    using namespace Microsoft::UI::Xaml::Media;

    MonitorCanvas().Children().Clear();
    MonitorTileElements_.clear();
    if (MonitorModel_.Tiles.empty()) {
        TextBlock Empty;
        Empty.Text(L"No current or saved displays are available.");
        Canvas::SetLeft(Empty, 24);
        Canvas::SetTop(Empty, 24);
        MonitorCanvas().Children().Append(Empty);
        return;
    }

    auto Left = MonitorModel_.Tiles.front().Rect.X;
    auto Top = MonitorModel_.Tiles.front().Rect.Y;
    auto Right = Left + MonitorModel_.Tiles.front().Rect.Width;
    auto Bottom = Top + MonitorModel_.Tiles.front().Rect.Height;
    for (const auto& Tile : MonitorModel_.Tiles) {
        Left = std::min(Left, Tile.Rect.X);
        Top = std::min(Top, Tile.Rect.Y);
        Right = std::max(Right, Tile.Rect.X + Tile.Rect.Width);
        Bottom = std::max(Bottom, Tile.Rect.Y + Tile.Rect.Height);
    }
    const auto Width = std::max<std::int32_t>(Right - Left, 1);
    const auto Height = std::max<std::int32_t>(Bottom - Top, 1);
    MonitorViewScale_ = std::clamp(
        std::min(1.0, std::min(1'400.0 / Width, 620.0 / Height)),
        0.10, 1.0);
    MonitorViewOriginX_ = Left;
    MonitorViewOriginY_ = Top;
    MonitorCanvas().Width(std::max(
        780.0, static_cast<double>(Width) * MonitorViewScale_ + 72.0));
    MonitorCanvas().Height(std::max(
        430.0, static_cast<double>(Height) * MonitorViewScale_ + 84.0));

    const auto LocalBrush = MonitorLocalPalette().Background();
    const auto PeerBrush = MonitorPeerPalette().Background();
    const auto OfflineBrush = MonitorOfflinePalette().Background();
    const auto NormalBorder = MonitorNormalBorderPalette().BorderBrush();
    const auto SuggestedBorder =
        MonitorSuggestedBorderPalette().BorderBrush();

    std::vector<desklink::MachineId> Groups;
    for (const auto& Tile : MonitorModel_.Tiles) {
        if (std::find(Groups.begin(), Groups.end(), Tile.Machine) ==
            Groups.end()) {
            Groups.push_back(Tile.Machine);
        }
    }
    for (const auto& Machine : Groups) {
        double GroupLeft = std::numeric_limits<double>::max();
        double GroupTop = std::numeric_limits<double>::max();
        double GroupRight = std::numeric_limits<double>::lowest();
        double GroupBottom = std::numeric_limits<double>::lowest();
        winrt::hstring GroupName;
        for (const auto& Tile : MonitorModel_.Tiles) {
            if (Tile.Machine != Machine) continue;
            GroupName = ToHString(Tile.MachineName);
            const auto X = 30.0 +
                (Tile.Rect.X - MonitorViewOriginX_) * MonitorViewScale_;
            const auto Y = 42.0 +
                (Tile.Rect.Y - MonitorViewOriginY_) * MonitorViewScale_;
            GroupLeft = std::min(GroupLeft, X);
            GroupTop = std::min(GroupTop, Y);
            GroupRight = std::max(
                GroupRight, X + std::max(
                    96.0, Tile.Rect.Width * MonitorViewScale_));
            GroupBottom = std::max(
                GroupBottom, Y + std::max(
                    68.0, Tile.Rect.Height * MonitorViewScale_));
        }
        Border Group;
        Group.Width(GroupRight - GroupLeft + 24.0);
        Group.Height(GroupBottom - GroupTop + 54.0);
        Group.CornerRadius(CornerRadius{8});
        Group.BorderThickness(Thickness{1});
        Group.BorderBrush(NormalBorder);
        Group.Background(PeerBrush);
        Group.IsHitTestVisible(false);
        TextBlock Name;
        Name.Text(GroupName);
        Name.Margin(Thickness{10, 7, 10, 0});
        Name.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        Name.VerticalAlignment(VerticalAlignment::Top);
        Group.Child(Name);
        Canvas::SetLeft(Group, GroupLeft - 12.0);
        Canvas::SetTop(Group, GroupTop - 34.0);
        MonitorCanvas().Children().Append(Group);
    }

    for (std::size_t Index = 0; Index < MonitorModel_.Tiles.size(); ++Index) {
        const auto& Tile = MonitorModel_.Tiles[Index];
        const bool Suggested = MonitorSuggestion_ &&
            (MonitorSuggestion_->TileA == Index ||
             MonitorSuggestion_->TileB == Index);
        Border Card;
        Card.Width(std::max(96.0, Tile.Rect.Width * MonitorViewScale_));
        Card.Height(std::max(68.0, Tile.Rect.Height * MonitorViewScale_));
        Card.Padding(Thickness{10});
        Card.CornerRadius(CornerRadius{6});
        Card.BorderThickness(Thickness{Suggested ? 3.0 : 1.5});
        Card.BorderBrush(Suggested ? SuggestedBorder : NormalBorder);
        Card.Background(!Tile.Online
            ? OfflineBrush
            : Tile.Local ? LocalBrush : PeerBrush);
        Card.Opacity(Tile.Online ? 1.0 : 0.68);
        Card.Tag(winrt::box_value(static_cast<std::uint64_t>(Index)));
        Card.PointerPressed({this, &MainWindow::OnMonitorTilePointerPressed});
        Card.PointerMoved({this, &MainWindow::OnMonitorTilePointerMoved});
        Card.PointerReleased({this, &MainWindow::OnMonitorTilePointerReleased});
        Card.PointerCanceled({this, &MainWindow::OnMonitorTilePointerReleased});
        const auto Label = MonitorTileLabel(Tile, Index);
        AutomationProperties::SetName(Card, Label);
        AutomationProperties::SetHelpText(
            Card,
            L"Drag for visual placement. Use the keyboard-accessible editor to create routing without dragging.");
        TextBlock Text;
        Text.Text(Label);
        Text.TextWrapping(TextWrapping::Wrap);
        Text.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        Card.Child(Text);
        Canvas::SetLeft(
            Card, 30.0 +
                (Tile.Rect.X - MonitorViewOriginX_) * MonitorViewScale_);
        Canvas::SetTop(
            Card, 42.0 +
                (Tile.Rect.Y - MonitorViewOriginY_) * MonitorViewScale_);
        MonitorCanvas().Children().Append(Card);
        MonitorTileElements_.push_back(Card);
    }
}

void MainWindow::RenderMonitorEditors() {
    const auto SourceSelection = AccessibleSourceDisplay().SelectedIndex();
    const auto TargetSelection = AccessibleTargetDisplay().SelectedIndex();
    AccessibleSourceDisplay().Items().Clear();
    AccessibleTargetDisplay().Items().Clear();
    for (std::size_t Index = 0; Index < MonitorModel_.Tiles.size(); ++Index) {
        const auto Text = MonitorDisplayChoice(MonitorModel_.Tiles[Index], Index);
        AccessibleSourceDisplay().Items().Append(winrt::box_value(Text));
        AccessibleTargetDisplay().Items().Append(winrt::box_value(Text));
    }
    if (MonitorModel_.Tiles.empty()) return;
    AccessibleSourceDisplay().SelectedIndex(
        SourceSelection >= 0 &&
                static_cast<std::size_t>(SourceSelection) < MonitorModel_.Tiles.size()
            ? SourceSelection
            : 0);
    AccessibleTargetDisplay().SelectedIndex(
        TargetSelection >= 0 &&
                static_cast<std::size_t>(TargetSelection) < MonitorModel_.Tiles.size()
            ? TargetSelection
            : MonitorModel_.Tiles.size() > 1 ? 1 : 0);
}

void MainWindow::RenderMonitorRoutes() {
    using namespace Microsoft::UI::Xaml;
    using namespace Microsoft::UI::Xaml::Controls;
    MonitorRouteCards().Children().Clear();
    NoMonitorRoutesText().Visibility(MonitorConfiguration_.Links.empty()
        ? Visibility::Visible
        : Visibility::Collapsed);

    std::vector<desklink::MachineDisplayTopology> Topologies;
    for (const auto& Machine : MonitorMachines_) {
        if (Machine.Topology) {
            Topologies.push_back({Machine.Machine, &*Machine.Topology});
        }
    }
    const auto EndpointName = [&](const desklink::RoamingEndpoint& Endpoint) {
        const auto Match = std::find_if(
            MonitorModel_.Tiles.begin(), MonitorModel_.Tiles.end(),
            [&](const auto& Tile) {
                return Tile.Machine == Endpoint.Machine &&
                       Tile.StableDisplayIdentity ==
                           Endpoint.StableDisplayIdentity;
            });
        return Match == MonitorModel_.Tiles.end()
            ? winrt::hstring(L"Offline display")
            : MonitorDisplayChoice(
                  *Match, static_cast<std::size_t>(
                      std::distance(MonitorModel_.Tiles.begin(), Match)));
    };

    for (std::size_t Index = 0;
         Index < MonitorConfiguration_.Links.size(); ++Index) {
        const auto& Link = MonitorConfiguration_.Links[Index];
        const auto Resolution = desklink::ResolveRoamingLink(Link, Topologies);
        const wchar_t* Direction =
            Link.Direction == desklink::RoamingDirectionMode::Bidirectional
                ? L" ↔ "
                : Link.Direction == desklink::RoamingDirectionMode::AToB
                    ? L" → "
                    : L" ← ";
        std::wstring Summary = EndpointName(Link.EndpointA).c_str();
        Summary += L" · ";
        Summary += MonitorSideName(Link.EndpointA.Side);
        Summary += Direction;
        Summary += EndpointName(Link.EndpointB).c_str();
        Summary += L" · ";
        Summary += MonitorSideName(Link.EndpointB.Side);
        Summary += Link.Enabled
            ? Resolution.Ready() ? L" · Ready" : L" · Display offline or missing"
            : L" · Disabled";

        Border Card;
        Card.Padding(Thickness{12});
        Card.CornerRadius(CornerRadius{6});
        Card.BorderThickness(Thickness{1});
        Grid Layout;
        Layout.ColumnSpacing(12);
        Layout.ColumnDefinitions().Append(ColumnDefinition{});
        ColumnDefinition ActionColumn;
        ActionColumn.Width(GridLengthHelper::Auto());
        Layout.ColumnDefinitions().Append(ActionColumn);
        TextBlock Text;
        Text.Text(winrt::hstring(Summary));
        Text.TextWrapping(TextWrapping::Wrap);
        Layout.Children().Append(Text);
        Button Remove;
        Remove.Content(winrt::box_value(L"Remove"));
        Remove.Tag(winrt::box_value(static_cast<std::uint64_t>(Index)));
        Remove.Click({this, &MainWindow::OnRemoveMonitorRoute});
        Grid::SetColumn(Remove, 1);
        Layout.Children().Append(Remove);
        Card.Child(Layout);
        MonitorRouteCards().Children().Append(Card);
    }
}

void MainWindow::RecomputeMonitorSuggestion(bool SnapDraggedTile) {
    MonitorSuggestion_.reset();
    for (std::size_t A = 0; A < MonitorModel_.Tiles.size(); ++A) {
        for (std::size_t B = A + 1; B < MonitorModel_.Tiles.size(); ++B) {
            if (SnapDraggedTile && DraggingMonitorTile_ &&
                A != *DraggingMonitorTile_ && B != *DraggingMonitorTile_) {
                continue;
            }
            auto Candidate = desklink::BuildRoamingLinkSuggestion(
                MonitorModel_.Tiles, A, B);
            if (!Candidate || HasEquivalentMonitorConnection(
                    MonitorConfiguration_, Candidate->Link)) {
                continue;
            }
            if (!MonitorSuggestion_ ||
                std::tie(Candidate->EdgeGapPixels,
                         Candidate->TileA, Candidate->TileB) <
                    std::tie(MonitorSuggestion_->EdgeGapPixels,
                             MonitorSuggestion_->TileA,
                             MonitorSuggestion_->TileB)) {
                MonitorSuggestion_ = std::move(Candidate);
            }
        }
    }

    if (SnapDraggedTile && DraggingMonitorTile_ && MonitorSuggestion_) {
        const auto MovingIndex = *DraggingMonitorTile_;
        auto& Moving = MonitorModel_.Tiles[MovingIndex].Rect;
        const auto OtherIndex = MonitorSuggestion_->TileA == MovingIndex
            ? MonitorSuggestion_->TileB
            : MonitorSuggestion_->TileA;
        const auto& Other = MonitorModel_.Tiles[OtherIndex].Rect;
        const auto Side = MonitorSuggestion_->TileA == MovingIndex
            ? MonitorSuggestion_->Link.EndpointA.Side
            : MonitorSuggestion_->Link.EndpointB.Side;
        switch (Side) {
            case desklink::DisplayEdgeSide::Left:
                Moving.X = Other.X + Other.Width;
                break;
            case desklink::DisplayEdgeSide::Top:
                Moving.Y = Other.Y + Other.Height;
                break;
            case desklink::DisplayEdgeSide::Right:
                Moving.X = Other.X - Moving.Width;
                break;
            case desklink::DisplayEdgeSide::Bottom:
                Moving.Y = Other.Y - Moving.Height;
                break;
        }
        Moving.X = std::clamp(
            Moving.X, -desklink::kMaximumCanvasCoordinate,
            desklink::kMaximumCanvasCoordinate);
        Moving.Y = std::clamp(
            Moving.Y, -desklink::kMaximumCanvasCoordinate,
            desklink::kMaximumCanvasCoordinate);
        MonitorSuggestion_ = desklink::BuildRoamingLinkSuggestion(
            MonitorModel_.Tiles,
            MonitorSuggestion_->TileA, MonitorSuggestion_->TileB);
    }

    if (!MonitorSuggestion_) {
        MonitorSuggestionCard().Visibility(
            Microsoft::UI::Xaml::Visibility::Collapsed);
        return;
    }
    const auto& A = MonitorModel_.Tiles[MonitorSuggestion_->TileA];
    const auto& B = MonitorModel_.Tiles[MonitorSuggestion_->TileB];
    std::wstring Text = MonitorDisplayChoice(
        A, MonitorSuggestion_->TileA).c_str();
    Text += L" ";
    Text += MonitorSideName(MonitorSuggestion_->Link.EndpointA.Side);
    Text += L" ↔ ";
    Text += MonitorDisplayChoice(B, MonitorSuggestion_->TileB).c_str();
    Text += L" ";
    Text += MonitorSideName(MonitorSuggestion_->Link.EndpointB.Side);
    MonitorSuggestionText().Text(winrt::hstring(Text));
    MonitorSuggestionCard().Visibility(
        Microsoft::UI::Xaml::Visibility::Visible);
}

void MainWindow::MarkMonitorDirty() {
    MonitorLayoutDirty_ = true;
    MonitorUnsavedText().Text(
        L"Unsaved changes. Input routing is unchanged until Save desk layout succeeds.");
}

std::optional<std::size_t> MainWindow::MonitorTileFromTag(
    Windows::Foundation::IInspectable const& Tag) const {
    const auto Index = winrt::unbox_value_or<std::uint64_t>(
        Tag, std::numeric_limits<std::uint64_t>::max());
    return Index < MonitorModel_.Tiles.size()
        ? std::optional<std::size_t>(static_cast<std::size_t>(Index))
        : std::nullopt;
}

std::optional<std::size_t> MainWindow::SelectedMonitorTile(
    Microsoft::UI::Xaml::Controls::ComboBox const& Selector) const {
    const auto Index = Selector.SelectedIndex();
    return Index >= 0 &&
            static_cast<std::size_t>(Index) < MonitorModel_.Tiles.size()
        ? std::optional<std::size_t>(static_cast<std::size_t>(Index))
        : std::nullopt;
}

std::optional<desklink::RoamingLink> MainWindow::ReadMonitorConnection(
    bool Advanced) {
    const auto Source = SelectedMonitorTile(AccessibleSourceDisplay());
    const auto Target = SelectedMonitorTile(AccessibleTargetDisplay());
    const auto SourceSide = AccessibleSourceSide().SelectedIndex();
    const auto TargetSide = AccessibleTargetSide().SelectedIndex();
    if (!Source || !Target || *Source == *Target ||
        SourceSide < 0 || SourceSide > 3 ||
        TargetSide < 0 || TargetSide > 3) {
        return std::nullopt;
    }
    const auto& A = MonitorModel_.Tiles[*Source];
    const auto& B = MonitorModel_.Tiles[*Target];
    if (!A.Online || !B.Online || A.Machine == B.Machine) {
        return std::nullopt;
    }
    const std::array Sides{
        desklink::DisplayEdgeSide::Left,
        desklink::DisplayEdgeSide::Top,
        desklink::DisplayEdgeSide::Right,
        desklink::DisplayEdgeSide::Bottom};
    const auto AStart = Advanced
        ? PercentToPermyriad(AdvancedSourceStart().Value())
        : std::optional<std::uint16_t>(static_cast<std::uint16_t>(0));
    const auto AEnd = Advanced
        ? PercentToPermyriad(AdvancedSourceEnd().Value())
        : std::optional<std::uint16_t>(static_cast<std::uint16_t>(10'000));
    const auto BStart = Advanced
        ? PercentToPermyriad(AdvancedTargetStart().Value())
        : std::optional<std::uint16_t>(static_cast<std::uint16_t>(0));
    const auto BEnd = Advanced
        ? PercentToPermyriad(AdvancedTargetEnd().Value())
        : std::optional<std::uint16_t>(static_cast<std::uint16_t>(10'000));
    if (!AStart || !AEnd || !BStart || !BEnd ||
        *AStart >= *AEnd || *BStart >= *BEnd) {
        return std::nullopt;
    }
    const std::array Directions{
        desklink::RoamingDirectionMode::Bidirectional,
        desklink::RoamingDirectionMode::AToB,
        desklink::RoamingDirectionMode::BToA};
    const auto Direction = Advanced ? AdvancedDirection().SelectedIndex() : 0;
    if (Direction < 0 || Direction > 2) return std::nullopt;
    desklink::RoamingLink Link;
    Link.EndpointA = {
        A.Machine, A.StableDisplayIdentity,
        Sides[static_cast<std::size_t>(SourceSide)], *AStart, *AEnd};
    Link.EndpointB = {
        B.Machine, B.StableDisplayIdentity,
        Sides[static_cast<std::size_t>(TargetSide)], *BStart, *BEnd};
    Link.Direction = Directions[static_cast<std::size_t>(Direction)];
    Link.AToB = MonitorConfiguration_.CrossingDefaults;
    Link.BToA = MonitorConfiguration_.CrossingDefaults;
    return Link;
}

void MainWindow::OnRefreshMonitorLayout(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    if (MonitorLayoutDirty_) {
        ShowMonitorStatus(
            L"Save or remove pending changes first",
            L"Refresh did not discard the unsaved desk layout.",
            Microsoft::UI::Xaml::Controls::InfoBarSeverity::Warning);
        return;
    }
    LoadMonitorLayout();
}

void MainWindow::OnIdentifyDisplays(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    if (!MainWindowHandle_ ||
        !desklink::ShowWin32DisplayIdentification(MainWindowHandle_)) {
        ShowMonitorStatus(
            L"Identify unavailable",
            L"DeskLink could not create the five-second local display overlays.",
            Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error);
        return;
    }
    const auto PeerTile = std::find_if(
        MonitorModel_.Tiles.begin(), MonitorModel_.Tiles.end(),
        [](const auto& Tile) { return Tile.Online && !Tile.Local; });
    if (PeerTile == MonitorModel_.Tiles.end()) {
        ShowMonitorStatus(
            L"Only this PC was identified",
            L"No online paired-PC display was present in the current desk layout.",
            Microsoft::UI::Xaml::Controls::InfoBarSeverity::Warning);
        return;
    }
    const auto FirstPeerDisplayNumber = static_cast<std::uint16_t>(
        std::distance(MonitorModel_.Tiles.begin(), PeerTile) + 1);
    const auto Response = Send(
        desklink::IdentifyPeerDisplaysControlRequest{
            FirstPeerDisplayNumber},
        std::chrono::milliseconds{1'000});
    if (!Response || Response->Status != desklink::ControlStatus::Ok) {
        ShowMonitorStatus(
            L"Only this PC was identified",
            L"The paired PC was unavailable or has not mutually allowed display-layout exchange.",
            Microsoft::UI::Xaml::Controls::InfoBarSeverity::Warning);
        return;
    }
    ShowMonitorStatus(
        L"Identifying the whole desk",
        L"Numbered overlays are shown for five seconds on this PC and the authenticated paired PC.",
        Microsoft::UI::Xaml::Controls::InfoBarSeverity::Success);
}

void MainWindow::OnMonitorTilePointerPressed(
    Windows::Foundation::IInspectable const& Sender,
    Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& Args) {
    const auto Card = Sender.try_as<Microsoft::UI::Xaml::Controls::Border>();
    const auto Index = Card ? MonitorTileFromTag(Card.Tag()) : std::nullopt;
    if (!Card || !Index) return;
    const auto Point = Args.GetCurrentPoint(MonitorCanvas()).Position();
    DraggingMonitorTile_ = *Index;
    DragStartRect_ = MonitorModel_.Tiles[*Index].Rect;
    DragStartPointerX_ = Point.X;
    DragStartPointerY_ = Point.Y;
    Card.CapturePointer(Args.Pointer());
    Args.Handled(true);
}

void MainWindow::OnMonitorTilePointerMoved(
    Windows::Foundation::IInspectable const& Sender,
    Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& Args) {
    const auto Card = Sender.try_as<Microsoft::UI::Xaml::Controls::Border>();
    const auto Index = Card ? MonitorTileFromTag(Card.Tag()) : std::nullopt;
    if (!Card || !Index || !DraggingMonitorTile_ ||
        *DraggingMonitorTile_ != *Index ||
        !Args.GetCurrentPoint(MonitorCanvas()).Properties().IsLeftButtonPressed()) {
        return;
    }
    const auto Point = Args.GetCurrentPoint(MonitorCanvas()).Position();
    auto& Rect = MonitorModel_.Tiles[*Index].Rect;
    Rect.X = std::clamp(
        DragStartRect_.X + static_cast<std::int32_t>(std::lround(
            (Point.X - DragStartPointerX_) / MonitorViewScale_)),
        -desklink::kMaximumCanvasCoordinate,
        desklink::kMaximumCanvasCoordinate);
    Rect.Y = std::clamp(
        DragStartRect_.Y + static_cast<std::int32_t>(std::lround(
            (Point.Y - DragStartPointerY_) / MonitorViewScale_)),
        -desklink::kMaximumCanvasCoordinate,
        desklink::kMaximumCanvasCoordinate);
    Microsoft::UI::Xaml::Controls::Canvas::SetLeft(
        Card, 30.0 +
            (Rect.X - MonitorViewOriginX_) * MonitorViewScale_);
    Microsoft::UI::Xaml::Controls::Canvas::SetTop(
        Card, 42.0 +
            (Rect.Y - MonitorViewOriginY_) * MonitorViewScale_);
    MarkMonitorDirty();
    Args.Handled(true);
}

void MainWindow::OnMonitorTilePointerReleased(
    Windows::Foundation::IInspectable const& Sender,
    Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& Args) {
    const auto Card = Sender.try_as<Microsoft::UI::Xaml::Controls::Border>();
    const auto Index = Card ? MonitorTileFromTag(Card.Tag()) : std::nullopt;
    if (!Card || !Index || !DraggingMonitorTile_ ||
        *DraggingMonitorTile_ != *Index) {
        return;
    }
    Card.ReleasePointerCapture(Args.Pointer());
    RecomputeMonitorSuggestion(true);
    DraggingMonitorTile_.reset();
    RenderMonitorCanvas();
    Args.Handled(true);
}

void MainWindow::OnAcceptMonitorSuggestion(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    if (!MonitorSuggestion_) return;
    auto Link = MonitorSuggestion_->Link;
    Link.AToB = MonitorConfiguration_.CrossingDefaults;
    Link.BToA = MonitorConfiguration_.CrossingDefaults;
    auto Candidate = MonitorConfiguration_;
    Candidate.Links.push_back(std::move(Link));
    if (!desklink::IsValidRoamingConfiguration(Candidate)) {
        ShowMonitorStatus(
            L"Connection rejected",
            L"The proposed edge duplicates or overlaps an active source edge.",
            Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error);
        return;
    }
    MonitorConfiguration_ = std::move(Candidate);
    MarkMonitorDirty();
    RecomputeMonitorSuggestion(false);
    RenderMonitorRoutes();
    RenderMonitorCanvas();
}

void MainWindow::OnAddAccessibleConnection(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    const auto Link = ReadMonitorConnection(false);
    if (!Link) {
        ShowMonitorStatus(
            L"Choose two online displays",
            L"Connections must join displays on different PCs with a valid edge on each side.",
            Microsoft::UI::Xaml::Controls::InfoBarSeverity::Warning);
        return;
    }
    auto Candidate = MonitorConfiguration_;
    Candidate.Links.push_back(*Link);
    if (!desklink::IsValidRoamingConfiguration(Candidate)) {
        ShowMonitorStatus(
            L"Connection rejected",
            L"The new connection duplicates or overlaps an active source edge.",
            Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error);
        return;
    }
    MonitorConfiguration_ = std::move(Candidate);
    MarkMonitorDirty();
    RecomputeMonitorSuggestion(false);
    RenderMonitorRoutes();
    RenderMonitorCanvas();
}

void MainWindow::OnAddAdvancedConnection(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    const auto Link = ReadMonitorConnection(true);
    if (!Link) {
        ShowMonitorStatus(
            L"Advanced connection is invalid",
            L"Choose different online PCs, valid edges, and percentages where start is less than end.",
            Microsoft::UI::Xaml::Controls::InfoBarSeverity::Warning);
        return;
    }
    auto Candidate = MonitorConfiguration_;
    Candidate.Links.push_back(*Link);
    if (!desklink::IsValidRoamingConfiguration(Candidate)) {
        ShowMonitorStatus(
            L"Advanced connection rejected",
            L"The new source segment duplicates or overlaps an existing active route.",
            Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error);
        return;
    }
    MonitorConfiguration_ = std::move(Candidate);
    MarkMonitorDirty();
    RecomputeMonitorSuggestion(false);
    RenderMonitorRoutes();
    RenderMonitorCanvas();
}

void MainWindow::OnRemoveMonitorRoute(
    Windows::Foundation::IInspectable const& Sender,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    const auto Button = Sender.try_as<Microsoft::UI::Xaml::Controls::Button>();
    const auto Index = Button
        ? winrt::unbox_value_or<std::uint64_t>(
              Button.Tag(), std::numeric_limits<std::uint64_t>::max())
        : std::numeric_limits<std::uint64_t>::max();
    if (Index >= MonitorConfiguration_.Links.size()) return;
    MonitorConfiguration_.Links.erase(
        MonitorConfiguration_.Links.begin() +
        static_cast<std::ptrdiff_t>(Index));
    MarkMonitorDirty();
    RecomputeMonitorSuggestion(false);
    RenderMonitorRoutes();
    RenderMonitorCanvas();
}

void MainWindow::OnNudgeMonitorTile(
    Windows::Foundation::IInspectable const& Sender,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    const auto Button = Sender.try_as<Microsoft::UI::Xaml::Controls::Button>();
    const auto Index = SelectedMonitorTile(AccessibleSourceDisplay());
    const auto Direction = Button
        ? winrt::unbox_value_or<winrt::hstring>(Button.Tag(), {})
        : winrt::hstring{};
    if (!Index || Direction.empty()) return;
    auto& Rect = MonitorModel_.Tiles[*Index].Rect;
    constexpr std::int32_t Step = 10;
    if (Direction == L"Left") Rect.X -= Step;
    else if (Direction == L"Right") Rect.X += Step;
    else if (Direction == L"Up") Rect.Y -= Step;
    else if (Direction == L"Down") Rect.Y += Step;
    else return;
    Rect.X = std::clamp(
        Rect.X, -desklink::kMaximumCanvasCoordinate,
        desklink::kMaximumCanvasCoordinate);
    Rect.Y = std::clamp(
        Rect.Y, -desklink::kMaximumCanvasCoordinate,
        desklink::kMaximumCanvasCoordinate);
    MarkMonitorDirty();
    RecomputeMonitorSuggestion(false);
    RenderMonitorCanvas();
}

void MainWindow::OnSaveMonitorLayout(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    SaveMonitorLayout();
}

void MainWindow::SaveMonitorLayout() {
    using Microsoft::UI::Xaml::Controls::InfoBarSeverity;
    if (!MonitorLayoutLoaded_ || !RoamingSettings_) return;
    auto Candidate = MonitorConfiguration_;
    Candidate.CanvasLayout.clear();
    if (MonitorModel_.Tiles.size() > desklink::kMaximumCanvasPlacements) {
        ShowMonitorStatus(
            L"Layout is too large",
            L"The bounded display-placement limit was exceeded.",
            InfoBarSeverity::Error);
        return;
    }
    for (const auto& Tile : MonitorModel_.Tiles) {
        Candidate.CanvasLayout.push_back({
            Tile.Machine, Tile.StableDisplayIdentity,
            Tile.Rect.X, Tile.Rect.Y});
    }
    if (!desklink::IsValidRoamingConfiguration(Candidate)) {
        ShowMonitorStatus(
            L"Layout rejected",
            L"The candidate graph failed strict validation. The active layout was not changed.",
            InfoBarSeverity::Error);
        return;
    }

    const auto StateResponse = Send(desklink::GetStateControlRequest{});
    if (!StateResponse || StateResponse->Status != desklink::ControlStatus::Ok ||
        !StateResponse->State) {
        ShowMonitorStatus(
            L"Cannot confirm Local",
            L"The runtime state could not be verified. The active layout was not changed.",
            InfoBarSeverity::Error);
        return;
    }
    const bool WasPaused = StateResponse->State->RuntimePhase ==
        desklink::BrokerRuntimePhase::Paused;
    const bool RuntimeRemote = StateResponse->State->RemoteFocused ||
        StateResponse->State->CaptureActive;
    const bool HasEnabledRoute = std::any_of(
        Candidate.Links.begin(), Candidate.Links.end(),
        [](const auto& Link) { return Link.Enabled; });
    auto UpdatedPreferences = Preferences_;
    UpdatedPreferences.InputRoamingDesired = HasEnabledRoute;
    const auto SaveStatus = desklink::ApplyProductMonitorLayout(
        RuntimeRemote,
        desklink::ProductMonitorSaveActions{
            [&] {
                const auto Response = Send(
                    desklink::GetStateControlRequest{});
                return Response &&
                    Response->Status == desklink::ControlStatus::Ok &&
                    Response->State && !Response->State->RemoteFocused &&
                    !Response->State->CaptureActive;
            },
            [&] {
                const auto Response = Send(
                    desklink::ReturnLocalControlRequest{},
                    std::chrono::milliseconds{2'500});
                return Response &&
                    Response->Status == desklink::ControlStatus::Ok;
            },
            [&] { return RoamingSettings_->Save(Candidate); },
            [&] {
                return PreferencesLoaded_ && SavePreferences(
                    UpdatedPreferences, L"");
            }});
    if (SaveStatus == desklink::ProductMonitorSaveStatus::CleanupFailed) {
        ShowMonitorStatus(
            L"Cannot confirm Local",
            L"DeskLink did not complete fail-local cleanup. The active layout was not changed.",
            InfoBarSeverity::Error);
        return;
    }
    if (SaveStatus == desklink::ProductMonitorSaveStatus::StoreFailed) {
        ShowMonitorStatus(
            L"Layout was not saved",
            L"Atomic replacement failed. The previous validated layout remains on disk.",
            InfoBarSeverity::Error);
        return;
    }

    MonitorConfiguration_ = std::move(Candidate);
    MonitorLayoutDirty_ = false;
    MonitorUnsavedText().Text(L"No unsaved changes.");
    RenderMonitorRoutes();
    if (SaveStatus ==
        desklink::ProductMonitorSaveStatus::PreferenceApplyFailed) {
        ShowMonitorStatus(
            L"Desk layout saved; activation pending",
            L"The validated graph was stored, but DeskLink could not apply its roaming setting. The authenticated session was not replaced and input remains Local.",
            InfoBarSeverity::Warning);
    } else if (WasPaused) {
        ShowMonitorStatus(
            L"Desk layout saved",
            HasEnabledRoute
                ? L"The graph was replaced atomically and edge roaming was enabled. DeskLink remains paused."
                : L"The graph was replaced atomically and edge roaming was turned off. DeskLink remains paused.",
            InfoBarSeverity::Success);
    } else {
        ShowMonitorStatus(
            L"Desk layout saved",
            HasEnabledRoute
                ? L"The graph was applied to the existing authenticated session and edge roaming is ready—no separate switch is required."
                : L"The graph was applied to the existing authenticated session and edge roaming was turned off.",
            InfoBarSeverity::Success);
    }
    PollBroker();
}

void MainWindow::UpdateHome() {
    const auto Device = PreferredDevice();
    if (!Device) {
        PeerPcName().Text(L"No paired PC");
        PeerStatusText().Text(L"Add a PC to begin");
    } else {
        PeerPcName().Text(ToHString(Device->DisplayName));
        PeerStatusText().Text(
            State_ == desklink::ProductShellState::ConnectedLocal ||
                    State_ == desklink::ProductShellState::RemoteFocus
                ? L"Paired · Connected now"
                : L"Paired · Offline");
    }
}

const desklink::ControlTrustedDevice* MainWindow::PreferredDevice() const {
    if (Preferences_.PreferredPeerMachine) {
        const auto Match = std::find_if(
            TrustedDevices_.begin(), TrustedDevices_.end(),
            [&](const auto& Device) {
                return Device.Machine == *Preferences_.PreferredPeerMachine;
            });
        if (Match != TrustedDevices_.end()) return &*Match;
    }
    return TrustedDevices_.size() == 1 ? &TrustedDevices_.front() : nullptr;
}

void MainWindow::ShowFeatureStatus(
    winrt::hstring const& Title,
    winrt::hstring const& Message,
    Microsoft::UI::Xaml::Controls::InfoBarSeverity Severity) {
    FeatureStatusBar().Title(Title);
    FeatureStatusBar().Message(Message);
    FeatureStatusBar().Severity(Severity);
    FeatureStatusBar().IsOpen(true);
}

bool MainWindow::SavePreferences(
    desklink::ProductPreferences const& Preferences,
    winrt::hstring const& SuccessMessage) {
    using Microsoft::UI::Xaml::Controls::InfoBarSeverity;
    if (!BrokerAvailable_ || !desklink::IsValidProductPreferences(Preferences)) {
        ShowFeatureStatus(
            L"Settings unchanged",
            L"The requested settings were invalid or the runtime was unavailable. Input remains Local.",
            InfoBarSeverity::Error);
        return false;
    }
    const auto Response = Send(
        desklink::SetProductPreferencesControlRequest{Preferences},
        std::chrono::milliseconds{5'000});
    if (!Response || Response->Status != desklink::ControlStatus::Ok) {
        ShowFeatureStatus(
            L"Settings unchanged",
            L"DeskLink could not safely reconcile the runtime and save this change. Input remains Local.",
            InfoBarSeverity::Error);
        return false;
    }
    Preferences_ = Preferences;
    PreferencesLoaded_ = true;
    UpdateFeatureControls();
    RenderProfiles();
    if (!SuccessMessage.empty()) {
        ShowFeatureStatus(
            L"Settings saved", SuccessMessage, InfoBarSeverity::Success);
    }
    return true;
}

void MainWindow::UpdateFeatureControls() {
    if (!ContentReady_) return;
    UpdatingFeatureControls_ = true;
    const auto Device = PreferredDevice();
    const auto PeerName = Device
        ? ToHString(Device->DisplayName)
        : winrt::hstring(L"paired PC");
    const auto ThisPcName = LocalPcName().Text();

    ClipboardIntentLabel().Text(Device
        ? JoinText(L"Share text clipboard with ", PeerName,
                   L". Both PCs must separately allow read and write access.")
        : winrt::hstring(L"Pair a PC to configure clipboard sharing."));
    ClipboardDesiredToggle().IsEnabled(Device != nullptr);
    ClipboardDesiredToggle().IsOn(Preferences_.ClipboardDesired);

    PeerAudioIntentLabel().Text(Device
        ? JoinText(L"Play ", PeerName,
                   JoinText(L" audio on ", ThisPcName, L". The other PC must separately allow capture."))
        : winrt::hstring(L"Pair a PC to configure its audio."));
    PeerAudioDesiredToggle().IsEnabled(Device != nullptr);
    PeerAudioDesiredToggle().IsOn(ReceivesPeerAudio(Preferences_.AudioRoute));
    LocalAudioIntentLabel().Text(Device
        ? JoinText(
              L"Share ", ThisPcName,
              JoinText(L" audio with ", PeerName,
                       L". The other PC must separately allow playback."))
        : winrt::hstring(L"Pair a PC to share this PC's audio."));
    LocalAudioDesiredToggle().IsEnabled(Device != nullptr);
    LocalAudioDesiredToggle().IsOn(SendsLocalAudio(Preferences_.AudioRoute));
    PeerAudioGainBox().Value(
        static_cast<double>(Preferences_.AudioGainPermyriad) / 100.0);
    PeerAudioGainBox().IsEnabled(Device != nullptr);
    PeerAudioMuteButton().IsEnabled(
        Device && RuntimeStateLoaded_ &&
        RuntimeState_.ConnectedPeerCount != 0 &&
        ReceivesPeerAudio(Preferences_.AudioRoute));
    PeerAudioMuteButton().Content(winrt::box_value(
        RuntimeStateLoaded_ && RuntimeState_.AudioMuted ? L"Unmute" : L"Mute"));

    PeerVoiceDesiredToggle().IsEnabled(Device != nullptr);
    PeerVoiceDesiredToggle().IsOn(ReceivesPeerVoice(Preferences_.VoiceRoute));
    VoiceReceiveDestinationBox().IsEnabled(Device != nullptr);
    VoiceReceiveDestinationBox().SelectedIndex(
        static_cast<int>(Preferences_.VoiceDestination));
    LocalVoiceDesiredToggle().IsEnabled(Device != nullptr);
    LocalVoiceDesiredToggle().IsOn(SendsLocalVoice(Preferences_.VoiceRoute));
    VoiceGainBox().Value(
        static_cast<double>(Preferences_.VoiceGainPermyriad) / 100.0);
    VoiceGainBox().IsEnabled(
        Device != nullptr && UsesVoiceMonitor(Preferences_.VoiceDestination));
    VoiceEchoGuardToggle().IsOn(Preferences_.VoiceEchoGuard);
    VoiceEchoGuardToggle().IsEnabled(Device != nullptr);
    VoiceInputDeviceBox().IsEnabled(Device != nullptr);
    auto VirtualMicrophoneState =
        desklink::ControlVirtualMicrophoneState::NotInstalled;
    const auto ProductExecutable = GetExecutablePath();
    const auto VirtualMicrophoneInstaller = ProductExecutable
        ? ProductExecutable->parent_path() /
              L"desklink_virtual_microphone_installer.exe"
        : std::filesystem::path{};
    const auto VirtualMicrophonePackage = ProductExecutable
        ? ProductExecutable->parent_path() / L"driver" /
              L"DeskLinkVirtualMicrophone"
        : std::filesystem::path{};
    const bool VirtualMicrophoneInstallAvailable = ProductExecutable &&
        std::filesystem::is_regular_file(VirtualMicrophoneInstaller) &&
        std::filesystem::is_regular_file(
            VirtualMicrophonePackage / L"DeskLinkVirtualMicrophone.inf") &&
        std::filesystem::is_regular_file(
            VirtualMicrophonePackage / L"DeskLinkVirtualMicrophone.sys") &&
        std::filesystem::is_regular_file(
            VirtualMicrophonePackage / L"DeskLinkVirtualMicrophone.cat") &&
        std::filesystem::is_regular_file(
            VirtualMicrophonePackage / L"manifest.json");
    if (RuntimeStateLoaded_) {
        VirtualMicrophoneState = RuntimeState_.VirtualMicrophoneState;
    } else {
        const auto LocalState =
            desklink::GetWin32VirtualMicrophoneComponentState();
        VirtualMicrophoneState = LocalState == desklink::
                Win32VirtualMicrophoneComponentState::Ready
            ? desklink::ControlVirtualMicrophoneState::Installed
            : LocalState == desklink::
                  Win32VirtualMicrophoneComponentState::NeedsRepair
                ? desklink::ControlVirtualMicrophoneState::NeedsRepair
                : desklink::ControlVirtualMicrophoneState::NotInstalled;
    }
    switch (VirtualMicrophoneState) {
        case desklink::ControlVirtualMicrophoneState::NotInstalled:
            VirtualMicrophoneStatusText().Text(
                !VirtualMicrophoneInstallAvailable
                    ? L"DeskLink Virtual Microphone is not bundled in this build. Microsoft production driver signing is required; other DeskLink features remain available."
                : UsesVirtualMicrophone(Preferences_.VoiceDestination)
                    ? L"DeskLink Virtual Microphone is required. Install it to use the paired PC's microphone in Discord, OBS, games, and other apps."
                    : L"DeskLink Virtual Microphone: not installed (optional)");
            break;
        case desklink::ControlVirtualMicrophoneState::Installed:
            VirtualMicrophoneStatusText().Text(
                L"DeskLink Virtual Microphone: installed. Select Microphone for apps or Both to start its feed.");
            break;
        case desklink::ControlVirtualMicrophoneState::FeedReady:
            VirtualMicrophoneStatusText().Text(
                L"DeskLink Virtual Microphone: feed ready. Apps can select “DeskLink Remote Microphone”.");
            break;
        case desklink::ControlVirtualMicrophoneState::Live:
            VirtualMicrophoneStatusText().Text(
                L"Paired PC is sending microphone audio to apps through “DeskLink Remote Microphone”.");
            break;
        case desklink::ControlVirtualMicrophoneState::Silent:
            VirtualMicrophoneStatusText().Text(
                L"DeskLink Remote Microphone is available to apps and currently silent.");
            break;
        case desklink::ControlVirtualMicrophoneState::Unavailable:
            VirtualMicrophoneStatusText().Text(
                L"DeskLink Virtual Microphone is installed, but its feed is unavailable. DeskLink will retry without stopping other features.");
            break;
        case desklink::ControlVirtualMicrophoneState::NeedsRepair:
            VirtualMicrophoneStatusText().Text(
                L"DeskLink Virtual Microphone needs repair. Reinstall the optional component.");
            break;
    }
    InstallVirtualMicrophoneButton().Visibility(
        VirtualMicrophoneInstallAvailable &&
            (VirtualMicrophoneState ==
                desklink::ControlVirtualMicrophoneState::NotInstalled ||
            VirtualMicrophoneState ==
                desklink::ControlVirtualMicrophoneState::NeedsRepair)
            ? Microsoft::UI::Xaml::Visibility::Visible
            : Microsoft::UI::Xaml::Visibility::Collapsed);
    InstallVirtualMicrophoneButton().Content(winrt::box_value(
        VirtualMicrophoneState ==
                desklink::ControlVirtualMicrophoneState::NeedsRepair
            ? L"Repair virtual microphone"
            : L"Install virtual microphone"));
    OpenSoundInputSettingsButton().IsEnabled(
        VirtualMicrophoneState !=
            desklink::ControlVirtualMicrophoneState::NotInstalled);
    for (std::uint32_t Index = 0;
         Index < VoiceInputDeviceBox().Items().Size(); ++Index) {
        const auto Item = VoiceInputDeviceBox().Items().GetAt(Index)
            .try_as<Microsoft::UI::Xaml::Controls::ComboBoxItem>();
        if (!Item) continue;
        const auto Endpoint = winrt::unbox_value_or<winrt::hstring>(
            Item.Tag(), L"");
        const auto Selected = Preferences_.VoiceInputEndpointId
            ? ToHString(*Preferences_.VoiceInputEndpointId) == Endpoint
            : Endpoint.empty();
        if (Selected) {
            VoiceInputDeviceBox().SelectedIndex(static_cast<int>(Index));
            break;
        }
    }
    const bool Connected = Device && RuntimeStateLoaded_ &&
        RuntimeState_.ConnectedPeerCount != 0;
    VoiceMuteButton().IsEnabled(Connected &&
        SendsLocalVoice(Preferences_.VoiceRoute));
    VoiceMuteButton().Content(winrt::box_value(
        RuntimeStateLoaded_ && RuntimeState_.VoiceMuted
            ? L"Unmute microphone" : L"Mute microphone"));
    VoicePttButton().IsEnabled(Connected && RuntimeState_.VoicePttReady);
    VoicePttButton().Content(winrt::box_value(
        RuntimeStateLoaded_ && RuntimeState_.VoiceTransmitting
            ? L"Transmitting — release to stop" : L"Hold to talk"));
    if (!SendsLocalVoice(Preferences_.VoiceRoute)) {
        VoiceStatusText().Text(
            L"Microphone sharing is off. Capture is closed.");
    } else if (!RuntimeStateLoaded_ || !Connected) {
        VoiceStatusText().Text(
            L"Voice is enabled, but the authenticated peer is unavailable. Capture is closed.");
    } else if (RuntimeState_.VoiceTransmitting) {
        VoiceStatusText().Text(JoinText(
            L"Microphone is being sent to ", PeerName));
    } else if (RuntimeState_.VoiceMuted) {
        VoiceStatusText().Text(L"Microphone muted. PTT cannot start capture.");
    } else if (RuntimeState_.VoicePermissionMissing) {
        VoiceStatusText().Text(
            L"Voice permission is missing on one PC. Microphone capture is closed.");
    } else if (RuntimeState_.VoiceInputUnavailable) {
        VoiceStatusText().Text(
            L"The selected microphone is unavailable. Choose a device and press PTT again.");
    } else {
        VoiceStatusText().Text(
            L"PTT ready. The microphone remains closed until you hold the button.");
    }

    GamingBehaviorToggle().IsOn(
        Preferences_.Gaming == desklink::GamingBehavior::KeepLocal);
    InputRoamingDesiredToggle().IsEnabled(
        Device != nullptr &&
        Preferences_.Role != desklink::DeskRole::Companion);
    InputRoamingDesiredToggle().IsOn(
        Preferences_.InputRoamingDesired);
    FocusPeerHotkeyBox().SelectedIndex(
        static_cast<int>(Preferences_.FocusPeerHotkey));
    ReturnLocalHotkeyBox().SelectedIndex(
        static_cast<int>(Preferences_.ReturnLocalHotkey));
    FocusPeerButton().IsEnabled(
        Device && RuntimeStateLoaded_ && RuntimeState_.ConnectedPeerCount != 0);
    FocusPeerButton().Content(winrt::box_value(Device
        ? JoinText(L"Focus ", PeerName)
        : winrt::hstring(L"Focus paired PC")));
    UpdatingFeatureControls_ = false;
}

void MainWindow::LoadVoiceInputDevices() {
    using Microsoft::UI::Xaml::Controls::ComboBoxItem;
    VoiceInputDevices_ = desklink::EnumerateWin32VoiceInputDevices();
    VoiceInputDeviceBox().Items().Clear();
    ComboBoxItem Default;
    Default.Content(winrt::box_value(L"Default communications microphone"));
    Default.Tag(winrt::box_value(winrt::hstring{}));
    VoiceInputDeviceBox().Items().Append(Default);
    for (const auto& Device : VoiceInputDevices_) {
        ComboBoxItem Item;
        auto Label = ToHString(Device.DisplayName);
        if (Device.IsDefaultCommunications) Label = JoinText(Label, L" (default)");
        Item.Content(winrt::box_value(Label));
        Item.Tag(winrt::box_value(ToHString(Device.EndpointId)));
        VoiceInputDeviceBox().Items().Append(Item);
    }
    VoiceInputDeviceBox().SelectedIndex(0);
}

void MainWindow::RenderProfiles() {
    if (!ContentReady_) return;
    using namespace Microsoft::UI::Xaml;
    using namespace Microsoft::UI::Xaml::Controls;
    ProfileCards().Children().Clear();
    NoProfilesText().Visibility(Preferences_.ProfileRules.empty()
        ? Visibility::Visible : Visibility::Collapsed);
    for (std::size_t Index = 0; Index < Preferences_.ProfileRules.size();
         ++Index) {
        const auto& Rule = Preferences_.ProfileRules[Index];
        Border Card;
        Card.Padding(Thickness{12});
        Card.CornerRadius(CornerRadius{6});
        Card.BorderThickness(Thickness{1});
        Grid Layout;
        Layout.ColumnSpacing(12);
        Layout.ColumnDefinitions().Append(ColumnDefinition{});
        ColumnDefinition ActionColumn;
        ActionColumn.Width(GridLengthHelper::Auto());
        Layout.ColumnDefinitions().Append(ActionColumn);
        StackPanel Details;
        Details.Spacing(3);
        TextBlock Name;
        Name.Text(ToHString(Rule.ExecutableName));
        Name.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        Details.Children().Append(Name);
        TextBlock Policy;
        std::wstring Summary(ProfileModeName(Rule.Mode));
        Summary += Rule.FullscreenOnly ? L" | Fullscreen only" : L" | Any window";
        Policy.Text(winrt::hstring(Summary));
        Details.Children().Append(Policy);
        Layout.Children().Append(Details);
        Button Remove;
        Remove.Content(winrt::box_value(L"Remove"));
        Remove.Tag(winrt::box_value(static_cast<std::uint64_t>(Index)));
        Remove.Click({this, &MainWindow::OnRemoveProfileRule});
        Grid::SetColumn(Remove, 1);
        Layout.Children().Append(Remove);
        Card.Child(Layout);
        ProfileCards().Children().Append(Card);
    }
}

void MainWindow::SetClipboardDesired(bool Desired) {
    using Microsoft::UI::Xaml::Controls::InfoBarSeverity;
    const auto Device = PreferredDevice();
    if (!Device) {
        UpdateFeatureControls();
        ShowFeatureStatus(
            L"Pair a PC first",
            L"Clipboard intent is stored per preferred paired PC.",
            InfoBarSeverity::Warning);
        return;
    }
    if (Desired && !desklink::CanEnableClipboardIntent(Device->Capabilities)) {
        UpdateFeatureControls();
        ShowFeatureStatus(
            L"Permission required on this PC",
            L"Allow the paired PC to read and replace this PC's text clipboard under Devices & permissions. The other PC must approve its side separately.",
            InfoBarSeverity::Warning);
        return;
    }
    auto Updated = Preferences_;
    Updated.ClipboardDesired = Desired;
    (void)SavePreferences(
        Updated,
        Desired
            ? L"Clipboard sharing is desired. It remains off until both stored permission sets and the authenticated module handshake agree."
            : L"Clipboard sharing is off for this PC.");
}

void MainWindow::SetPeerAudioDesired(bool Desired) {
    using Microsoft::UI::Xaml::Controls::InfoBarSeverity;
    const auto Device = PreferredDevice();
    if (!Device) {
        UpdateFeatureControls();
        ShowFeatureStatus(
            L"Pair a PC first",
            L"Audio intent is stored per preferred paired PC.",
            InfoBarSeverity::Warning);
        return;
    }
    if (Desired && !desklink::CanEnablePeerAudioIntent(Device->Capabilities)) {
        UpdateFeatureControls();
        ShowFeatureStatus(
            L"Permission required on this PC",
            L"Allow the paired PC to play audio into this PC under Devices & permissions. The paired PC must separately allow its audio to be captured.",
            InfoBarSeverity::Warning);
        return;
    }
    auto Updated = Preferences_;
    if (Desired) {
        Updated.AudioRoute = Updated.AudioRoute ==
                desklink::AudioRoutePreference::LocalToPeer
            ? desklink::AudioRoutePreference::Bidirectional
            : desklink::AudioRoutePreference::PeerToLocal;
    } else {
        Updated.AudioRoute = Updated.AudioRoute ==
                desklink::AudioRoutePreference::Bidirectional
            ? desklink::AudioRoutePreference::LocalToPeer
            : desklink::AudioRoutePreference::Off;
    }
    (void)SavePreferences(
        Updated,
        Desired
            ? L"Peer audio is desired. It remains off until both stored permission sets and authenticated audio admission agree."
            : L"Peer audio playback is off on this PC.");
}

void MainWindow::SetLocalAudioDesired(bool Desired) {
    using Microsoft::UI::Xaml::Controls::InfoBarSeverity;
    const auto Device = PreferredDevice();
    if (!Device) {
        UpdateFeatureControls();
        ShowFeatureStatus(
            L"Pair a PC first",
            L"Audio intent is stored per preferred paired PC.",
            InfoBarSeverity::Warning);
        return;
    }
    if (Desired && !desklink::CanEnableLocalAudioIntent(Device->Capabilities)) {
        UpdateFeatureControls();
        ShowFeatureStatus(
            L"Permission required on this PC",
            L"Allow the paired PC to receive audio captured on this PC under Devices & permissions. The paired PC must separately allow audio playback.",
            InfoBarSeverity::Warning);
        return;
    }
    auto Updated = Preferences_;
    if (Desired) {
        Updated.AudioRoute = Updated.AudioRoute ==
                desklink::AudioRoutePreference::PeerToLocal
            ? desklink::AudioRoutePreference::Bidirectional
            : desklink::AudioRoutePreference::LocalToPeer;
    } else {
        Updated.AudioRoute = Updated.AudioRoute ==
                desklink::AudioRoutePreference::Bidirectional
            ? desklink::AudioRoutePreference::PeerToLocal
            : desklink::AudioRoutePreference::Off;
    }
    (void)SavePreferences(
        Updated,
        Desired
            ? L"This PC's audio is shared only after both stored permission sets and authenticated audio admission agree."
            : L"Audio capture and sharing are off on this PC.");
}

void MainWindow::SetPeerVoiceDesired(bool Desired) {
    using Microsoft::UI::Xaml::Controls::InfoBarSeverity;
    const auto Device = PreferredDevice();
    if (!Device) {
        UpdateFeatureControls();
        ShowFeatureStatus(L"Pair a PC first",
                          L"Voice intent is stored per preferred paired PC.",
                          InfoBarSeverity::Warning);
        return;
    }
    if (Desired && !desklink::CanEnablePeerVoiceIntent(Device->Capabilities)) {
        UpdateFeatureControls();
        ShowFeatureStatus(
            L"Permission required on this PC",
            L"Allow the paired PC to play voice into this PC under Devices & permissions. The paired PC must separately allow its microphone to be sent.",
            InfoBarSeverity::Warning);
        return;
    }
    auto Updated = Preferences_;
    if (Desired) {
        Updated.VoiceRoute = Updated.VoiceRoute ==
                desklink::VoiceRoutePreference::LocalToPeer
            ? desklink::VoiceRoutePreference::Bidirectional
            : desklink::VoiceRoutePreference::PeerToLocal;
    } else {
        Updated.VoiceRoute = Updated.VoiceRoute ==
                desklink::VoiceRoutePreference::Bidirectional
            ? desklink::VoiceRoutePreference::LocalToPeer
            : desklink::VoiceRoutePreference::Off;
    }
    (void)SavePreferences(
        Updated,
        Desired
            ? L"Incoming voice is desired. Playback remains off until both stored permission sets and authenticated voice admission agree."
            : L"Incoming DeskLink voice playback is off on this PC.");
}

void MainWindow::SetLocalVoiceDesired(bool Desired) {
    using Microsoft::UI::Xaml::Controls::InfoBarSeverity;
    const auto Device = PreferredDevice();
    if (!Device) {
        UpdateFeatureControls();
        ShowFeatureStatus(L"Pair a PC first",
                          L"Voice intent is stored per preferred paired PC.",
                          InfoBarSeverity::Warning);
        return;
    }
    if (Desired && !desklink::CanEnableLocalVoiceIntent(Device->Capabilities)) {
        UpdateFeatureControls();
        ShowFeatureStatus(
            L"Permission required on this PC",
            L"Allow the paired PC to receive this PC's microphone under Devices & permissions. The paired PC must separately allow voice playback.",
            InfoBarSeverity::Warning);
        return;
    }
    auto Updated = Preferences_;
    if (Desired) {
        Updated.VoiceRoute = Updated.VoiceRoute ==
                desklink::VoiceRoutePreference::PeerToLocal
            ? desklink::VoiceRoutePreference::Bidirectional
            : desklink::VoiceRoutePreference::LocalToPeer;
    } else {
        Updated.VoiceRoute = Updated.VoiceRoute ==
                desklink::VoiceRoutePreference::Bidirectional
            ? desklink::VoiceRoutePreference::PeerToLocal
            : desklink::VoiceRoutePreference::Off;
    }
    (void)SavePreferences(
        Updated,
        Desired
            ? L"Microphone sharing is enabled for push-to-talk. Capture remains closed until PTT is held and both PCs admit the voice module."
            : L"Microphone sharing is off and capture is closed.");
}

void MainWindow::OnClipboardIntentToggled(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    if (ContentReady_ && !UpdatingFeatureControls_) {
        SetClipboardDesired(ClipboardDesiredToggle().IsOn());
    }
}

void MainWindow::OnPeerAudioIntentToggled(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    if (ContentReady_ && !UpdatingFeatureControls_) {
        SetPeerAudioDesired(PeerAudioDesiredToggle().IsOn());
    }
}

void MainWindow::OnLocalAudioIntentToggled(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    if (ContentReady_ && !UpdatingFeatureControls_) {
        SetLocalAudioDesired(LocalAudioDesiredToggle().IsOn());
    }
}

void MainWindow::OnApplyAudioGain(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    const auto Value = PeerAudioGainBox().Value();
    if (!std::isfinite(Value) || Value < 0.0 || Value > 100.0) {
        ShowFeatureStatus(
            L"Volume unchanged", L"Enter a value from 0 through 100 percent.",
            Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error);
        return;
    }
    auto Updated = Preferences_;
    Updated.AudioGainPermyriad = static_cast<std::uint16_t>(
        std::llround(Value * 100.0));
    (void)SavePreferences(
        Updated,
        L"The bounded peer-audio volume was saved. DeskLink never changes the Windows system mixer.");
}

void MainWindow::OnToggleAudioMute(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    const auto Response = Send(desklink::ToggleAudioMuteControlRequest{});
    if (!Response || Response->Status != desklink::ControlStatus::Ok) {
        ShowFeatureStatus(
            L"Audio unchanged",
            L"A validated active peer audio session was not available.",
            Microsoft::UI::Xaml::Controls::InfoBarSeverity::Warning);
        return;
    }
    RuntimeState_.AudioMuted = !RuntimeState_.AudioMuted;
    RuntimeStateLoaded_ = true;
    UpdateFeatureControls();
}

void MainWindow::OnPeerVoiceIntentToggled(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    if (ContentReady_ && !UpdatingFeatureControls_) {
        SetPeerVoiceDesired(PeerVoiceDesiredToggle().IsOn());
    }
}

void MainWindow::OnLocalVoiceIntentToggled(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    if (ContentReady_ && !UpdatingFeatureControls_) {
        SetLocalVoiceDesired(LocalVoiceDesiredToggle().IsOn());
    }
}

void MainWindow::OnVoiceInputChanged(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&) {
    if (!ContentReady_ || UpdatingFeatureControls_) return;
    const auto Item = VoiceInputDeviceBox().SelectedItem()
        .try_as<Microsoft::UI::Xaml::Controls::ComboBoxItem>();
    if (!Item) return;
    const auto Endpoint = winrt::unbox_value_or<winrt::hstring>(Item.Tag(), L"");
    auto Updated = Preferences_;
    Updated.VoiceInputEndpointId = Endpoint.empty()
        ? std::optional<std::string>{}
        : ToUtf8(Endpoint);
    (void)SavePreferences(
        Updated,
        L"Microphone selection saved. The device will be opened only while push-to-talk is held.");
}

void MainWindow::SetVoiceReceiveDestination(
    desklink::VoiceReceiveDestination Destination) {
    using Microsoft::UI::Xaml::Controls::InfoBarSeverity;
    if (!PreferencesLoaded_ || !PreferredDevice()) {
        ShowFeatureStatus(
            L"Pair a PC first",
            L"The received-microphone destination is stored locally for the preferred paired PC.",
            InfoBarSeverity::Informational);
        UpdateFeatureControls();
        return;
    }
    auto Updated = Preferences_;
    Updated.VoiceDestination = Destination;
    (void)SavePreferences(
        Updated,
        UsesVirtualMicrophone(Destination)
            ? L"Applications can use “DeskLink Remote Microphone” after the optional component is installed."
            : L"Paired-PC voice will play only through this PC's communications output.");
}

void MainWindow::OnVoiceReceiveDestinationChanged(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&) {
    if (UpdatingFeatureControls_) return;
    const auto Selection = VoiceReceiveDestinationBox().SelectedIndex();
    if (Selection < 0 || Selection > 2) {
        UpdateFeatureControls();
        return;
    }
    SetVoiceReceiveDestination(
        static_cast<desklink::VoiceReceiveDestination>(Selection));
}

void MainWindow::OnInstallVirtualMicrophone(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    using Microsoft::UI::Xaml::Controls::InfoBarSeverity;
    const auto Executable = GetExecutablePath();
    const auto Installer = Executable
        ? Executable->parent_path() /
              L"desklink_virtual_microphone_installer.exe"
        : std::filesystem::path{};
    if (!Executable || !std::filesystem::is_regular_file(Installer)) {
        ShowFeatureStatus(
            L"Virtual microphone package unavailable",
            L"This build does not contain a signed DeskLink Virtual Microphone package. Other DeskLink features remain available.",
            InfoBarSeverity::Warning);
        return;
    }
    const auto Result = reinterpret_cast<std::intptr_t>(ShellExecuteW(
        nullptr, L"runas", Installer.c_str(), L"install",
        Installer.parent_path().c_str(), SW_SHOWNORMAL));
    if (Result <= 32) {
        ShowFeatureStatus(
            L"Installation not started",
            L"Windows did not start the fixed DeskLink driver installer. No system audio settings were changed.",
            InfoBarSeverity::Error);
        return;
    }
    ShowFeatureStatus(
        L"Installation started",
        L"Approve the Windows administrator prompt, then return here. DeskLink will detect the component without changing your default microphone.",
        InfoBarSeverity::Informational);
}

void MainWindow::OnOpenSoundInputSettings(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    (void)ShellExecuteW(
        nullptr, L"open", L"ms-settings:sound", nullptr, nullptr,
        SW_SHOWNORMAL);
}

void MainWindow::OnApplyVoiceGain(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    const auto Value = VoiceGainBox().Value();
    if (!std::isfinite(Value) || Value < 0.0 || Value > 100.0) {
        ShowFeatureStatus(
            L"Volume unchanged", L"Enter a value from 0 through 100 percent.",
            Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error);
        return;
    }
    auto Updated = Preferences_;
    Updated.VoiceGainPermyriad = static_cast<std::uint16_t>(
        std::llround(Value * 100.0));
    (void)SavePreferences(
        Updated,
        L"The bounded incoming-voice volume was saved. DeskLink never changes the Windows system mixer.");
}

void MainWindow::OnToggleVoiceMute(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    const bool Muted = RuntimeStateLoaded_ && RuntimeState_.VoiceMuted;
    const auto Response = Send(desklink::SetVoiceMutedControlRequest{!Muted});
    if (!Response || Response->Status != desklink::ControlStatus::Ok) {
        ShowFeatureStatus(
            L"Microphone mute unchanged",
            L"A validated active voice session was not available.",
            Microsoft::UI::Xaml::Controls::InfoBarSeverity::Warning);
        return;
    }
    RuntimeState_.VoiceMuted = !Muted;
    RuntimeState_.VoiceTransmitting = false;
    RuntimeStateLoaded_ = true;
    UpdateFeatureControls();
}

void MainWindow::OnVoiceEchoGuardToggled(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    if (!ContentReady_ || UpdatingFeatureControls_) return;
    auto Updated = Preferences_;
    Updated.VoiceEchoGuard = VoiceEchoGuardToggle().IsOn();
    (void)SavePreferences(
        Updated,
        Updated.VoiceEchoGuard
            ? L"Echo guard enabled. Incoming DeskLink voice is muted while this PC transmits."
            : L"Echo guard disabled. This is not acoustic echo cancellation and may cause feedback.");
}

void MainWindow::OnVoicePttPressed(
    Windows::Foundation::IInspectable const& Sender,
    Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& Args) {
    const auto Button = Sender.try_as<Microsoft::UI::Xaml::Controls::Button>();
    if (!Button || !Button.CapturePointer(Args.Pointer())) return;
    const auto Response = Send(desklink::SetVoiceTransmitControlRequest{true});
    if (!Response || Response->Status != desklink::ControlStatus::Ok) {
        Button.ReleasePointerCapture(Args.Pointer());
        ShowFeatureStatus(
            L"Push-to-talk did not start",
            L"Voice permission, the authenticated session, or the selected microphone is unavailable. Capture remains closed.",
            Microsoft::UI::Xaml::Controls::InfoBarSeverity::Warning);
        return;
    }
    RuntimeState_.VoiceTransmitting = true;
    RuntimeState_.VoiceInputUnavailable = false;
    RuntimeStateLoaded_ = true;
    UpdateFeatureControls();
}

void MainWindow::OnVoicePttReleased(
    Windows::Foundation::IInspectable const& Sender,
    Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const& Args) {
    const auto Button = Sender.try_as<Microsoft::UI::Xaml::Controls::Button>();
    if (Button) Button.ReleasePointerCapture(Args.Pointer());
    (void)Send(desklink::SetVoiceTransmitControlRequest{false});
    RuntimeState_.VoiceTransmitting = false;
    RuntimeStateLoaded_ = true;
    UpdateFeatureControls();
}

void MainWindow::OnGamingBehaviorToggled(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    if (!ContentReady_ || UpdatingFeatureControls_) return;
    auto Updated = Preferences_;
    Updated.Gaming = GamingBehaviorToggle().IsOn()
        ? desklink::GamingBehavior::KeepLocal
        : desklink::GamingBehavior::FollowProfileRules;
    (void)SavePreferences(
        Updated,
        GamingBehaviorToggle().IsOn()
            ? L"Fullscreen applications keep keyboard and mouse on this PC. Uninspectable foreground state also stays Local."
            : L"Only the exact application profiles below affect foreground behavior.");
}

void MainWindow::SetInputRoamingDesired(bool Desired) {
    using Microsoft::UI::Xaml::Controls::InfoBarSeverity;
    if (!PreferredDevice()) {
        UpdateFeatureControls();
        ShowFeatureStatus(
            L"Pair a PC first",
            L"Display-edge roaming requires an authenticated paired PC and a saved display connection.",
            InfoBarSeverity::Warning);
        return;
    }
    auto Updated = Preferences_;
    Updated.InputRoamingDesired = Desired;
    (void)SavePreferences(
        Updated,
        Desired
            ? L"Edge roaming is enabled. DeskLink still waits for a connected, mutually authorized peer and a valid saved display route."
            : L"Edge roaming is off. Manual focus remains available.");
}

void MainWindow::OnInputRoamingToggled(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    if (ContentReady_ && !UpdatingFeatureControls_) {
        SetInputRoamingDesired(InputRoamingDesiredToggle().IsOn());
    }
}

void MainWindow::OnApplyCrossingPreset(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    if (!MonitorLayoutLoaded_) LoadMonitorLayout();
    if (!MonitorLayoutLoaded_ || !RoamingSettings_) {
        ShowFeatureStatus(
            L"Crossing unchanged",
            L"DeskLink could not load the validated display graph.",
            Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error);
        return;
    }
    const auto Selection = CrossingPresetBox().SelectedIndex();
    if (Selection < 0 || Selection > 2 ||
        !desklink::ApplyProductCrossingPreset(
            MonitorConfiguration_,
            static_cast<desklink::ProductCrossingPreset>(Selection))) {
        ShowFeatureStatus(
            L"Crossing unchanged", L"Select a valid crossing behavior.",
            Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error);
        return;
    }
    MarkMonitorDirty();
    SaveMonitorLayout();
    if (!MonitorLayoutDirty_) {
        ShowFeatureStatus(
            L"Crossing behavior saved",
            L"The preset was applied to new and existing directions after Local-first atomic graph replacement.",
            Microsoft::UI::Xaml::Controls::InfoBarSeverity::Success);
    }
}

void MainWindow::OnApplyHotkeys(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    using Microsoft::UI::Xaml::Controls::InfoBarSeverity;
    const auto Focus = FocusPeerHotkeyBox().SelectedIndex();
    const auto Return = ReturnLocalHotkeyBox().SelectedIndex();
    if (Focus < 0 || Focus > 4 || Return < 0 || Return > 4) {
        ShowFeatureStatus(
            L"Hotkeys unchanged", L"Select hotkeys from the bounded list.",
            InfoBarSeverity::Error);
        return;
    }
    auto Updated = Preferences_;
    Updated.FocusPeerHotkey = static_cast<desklink::ProductHotkey>(Focus);
    Updated.ReturnLocalHotkey = static_cast<desklink::ProductHotkey>(Return);
    if (!desklink::IsValidProductPreferences(Updated)) {
        UpdateFeatureControls();
        ShowFeatureStatus(
            L"Hotkeys unchanged",
            L"Focus and Return to this PC must use different shortcuts. Ctrl+Alt+Pause/Break remains reserved.",
            InfoBarSeverity::Error);
        return;
    }
    (void)SavePreferences(
        Updated,
        L"The background runtime saved and registered the local shortcuts. They request normal authenticated focus and never bypass admission.");
}

void MainWindow::FocusPreferredPeer() {
    const auto Device = PreferredDevice();
    const auto Response = Device
        ? Send(desklink::FocusMachineControlRequest{Device->Machine})
        : std::nullopt;
    if (!Response || Response->Status != desklink::ControlStatus::Ok) {
        FocusRequestStartedAt_.reset();
        ShowFeatureStatus(
            L"Focus stayed on this PC",
            L"The named peer was not connected and fully admitted for input. DeskLink did not bypass route, trust, capability, nonce, epoch, lease, topology, or peer validation checks.",
            Microsoft::UI::Xaml::Controls::InfoBarSeverity::Warning);
        return;
    }
    FocusRequestStartedAt_ = std::chrono::steady_clock::now();
    ShowFeatureStatus(
        L"Changing focus",
        L"The authenticated peer accepted the request. DeskLink is waiting for fresh focus admission and input-state reconciliation.",
        Microsoft::UI::Xaml::Controls::InfoBarSeverity::Informational);
}

void MainWindow::UpdateFocusRequestPresentation(
    std::chrono::steady_clock::time_point Now) {
    if (!FocusRequestStartedAt_) return;
    using Microsoft::UI::Xaml::Controls::InfoBarSeverity;
    if (RuntimeState_.RemoteFocused) {
        FocusRequestStartedAt_.reset();
        ShowFeatureStatus(
            L"Focus changed",
            L"Keyboard and mouse focus is active on the authenticated paired PC.",
            InfoBarSeverity::Success);
        return;
    }
    if (RuntimeState_.ConnectedPeerCount == 0) {
        FocusRequestStartedAt_.reset();
        ShowFeatureStatus(
            L"Focus stayed on this PC",
            L"The peer connection ended before focus admission completed.",
            InfoBarSeverity::Warning);
        return;
    }
    if (Now - *FocusRequestStartedAt_ >= kFocusPresentationTimeout) {
        FocusRequestStartedAt_.reset();
        ShowFeatureStatus(
            L"Focus request timed out",
            L"The peer did not complete focus admission and input-state reconciliation. Input stayed on this PC.",
            InfoBarSeverity::Warning);
    }
}

void MainWindow::ReturnLocal() {
    FocusRequestStartedAt_.reset();
    const auto Response = Send(desklink::ReturnLocalControlRequest{});
    if (Response && Response->Status == desklink::ControlStatus::Ok) {
        ApplyState(desklink::ProductShellState::ConnectedLocal);
    }
}

void MainWindow::OnFocusPreferredPeer(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    FocusPreferredPeer();
}

void MainWindow::OnAddProfileRule(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    using Microsoft::UI::Xaml::Controls::InfoBarSeverity;
    const auto Executable = ToUtf8(ProfileExecutableBox().Text());
    const auto Selection = ProfileModeBox().SelectedIndex();
    if (!Executable || Selection < 0 || Selection > 2) {
        ProfileStatusBar().Title(L"Profile unchanged");
        ProfileStatusBar().Message(
            L"Enter an exact executable name such as game.exe and choose a behavior.");
        ProfileStatusBar().Severity(InfoBarSeverity::Error);
        ProfileStatusBar().IsOpen(true);
        return;
    }
    const auto ModeName = Selection == 0 ? "lock-pc1" :
        Selection == 1 ? "roam" : "game";
    const auto Rule = desklink::ParseForegroundProfileRule(
        *Executable + "=" + ModeName,
        IsChecked(ProfileFullscreenOnlyCheck()));
    auto Updated = Preferences_;
    if (Rule) Updated.ProfileRules.push_back(*Rule);
    if (!Rule || !desklink::IsValidProductPreferences(Updated)) {
        ProfileStatusBar().Title(L"Profile unchanged");
        ProfileStatusBar().Message(
            L"Use a unique exact executable name without a path. The same name and fullscreen condition cannot be repeated.");
        ProfileStatusBar().Severity(InfoBarSeverity::Error);
        ProfileStatusBar().IsOpen(true);
        return;
    }
    if (SavePreferences(Updated, {})) {
        ProfileExecutableBox().Text(L"");
        ProfileFullscreenOnlyCheck().IsChecked(false);
        ProfileStatusBar().Title(L"Profile saved");
        ProfileStatusBar().Message(
            L"The bounded local profile will be applied on the next reconciled runtime session.");
        ProfileStatusBar().Severity(InfoBarSeverity::Success);
        ProfileStatusBar().IsOpen(true);
    }
}

void MainWindow::OnRemoveProfileRule(
    Windows::Foundation::IInspectable const& Sender,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    const auto Button = Sender.try_as<Microsoft::UI::Xaml::Controls::Button>();
    const auto Index = Button
        ? winrt::unbox_value_or<std::uint64_t>(
              Button.Tag(), std::numeric_limits<std::uint64_t>::max())
        : std::numeric_limits<std::uint64_t>::max();
    if (Index >= Preferences_.ProfileRules.size()) return;
    auto Updated = Preferences_;
    Updated.ProfileRules.erase(
        Updated.ProfileRules.begin() + static_cast<std::ptrdiff_t>(Index));
    if (SavePreferences(Updated, {})) {
        ProfileStatusBar().Title(L"Profile removed");
        ProfileStatusBar().Message(
            L"Foreground policy was reconciled and remains fail-local.");
        ProfileStatusBar().Severity(
            Microsoft::UI::Xaml::Controls::InfoBarSeverity::Success);
        ProfileStatusBar().IsOpen(true);
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
    ShowPairingStatus(
        L"Secure handshake ready. Compare the code shown by both PCs before allowing either side.",
        InfoBarSeverity::Informational);
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
    if (PresentedPairingOperation_ &&
        PresentedPairingOperation_->OperationId == Candidate.OperationId &&
        IsTerminalPairingPhase(PresentedPairingOperation_->Phase)) {
        // Polling may close the dialog after the broker has already reached a
        // terminal result. Do not overwrite that result with a stale resolve.
        DisplayedPairingOperation_ = 0;
        co_return;
    }
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
    AddPermission(L"May play voice into this PC",
                  desklink::Capability::VoiceSend);
    AddPermission(L"May receive microphone voice from this PC",
                  desklink::Capability::VoiceReceive);
    TextBlock Note;
    Note.Text(
        L"Removing permission takes effect immediately after returning input Local. Adding permission opens a separate local confirmation for this already-trusted identity; it does not pair the PCs again.");
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
    if (Response && Response->Status == desklink::ControlStatus::Ok &&
        Response->PermissionCandidate) {
        DisplayedPermissionOperation_ =
            Response->PermissionCandidate->OperationId;
        co_await ShowPermissionCandidate(*Response->PermissionCandidate);
        co_return;
    }
    if (Response && Response->Status == desklink::ControlStatus::Ok) {
        DeviceStatusBar().Title(L"Permissions updated");
        DeviceStatusBar().Message(
            L"The change was applied after fail-local cleanup. Pairing and the stored identity were unchanged.");
        DeviceStatusBar().Severity(InfoBarSeverity::Success);
    } else if (Response && Response->Status ==
        desklink::ControlStatus::ReauthorizationRequired) {
        DeviceStatusBar().Title(L"Permission review expired");
        DeviceStatusBar().Message(
            L"DeskLink did not add authority because the trusted identity or existing permissions changed during review. Pairing was not modified.");
        DeviceStatusBar().Severity(InfoBarSeverity::Warning);
    } else if (Response && Response->Status ==
        desklink::ControlStatus::NotReady) {
        DeviceStatusBar().Title(L"Finish the current permission review");
        DeviceStatusBar().Message(
            L"Only one protected permission review can be active. Any permission removals already completed remain in effect.");
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

Windows::Foundation::IAsyncAction MainWindow::ShowPermissionCandidate(
    desklink::ControlPermissionCandidate Candidate) {
    auto Lifetime = get_strong();
    ModalDialogActive_ = true;
    using namespace Microsoft::UI::Xaml::Controls;
    ContentDialog Dialog;
    Dialog.XamlRoot(Content().XamlRoot());
    Dialog.Title(winrt::box_value(JoinText(
        L"Allow more access for ", ToHString(Candidate.DisplayName))));
    Dialog.PrimaryButtonText(L"Allow new permissions");
    Dialog.CloseButtonText(L"Keep current permissions");
    Dialog.DefaultButton(ContentDialogButton::Close);

    const desklink::CapabilitySet Added{
        Candidate.DesiredCapabilities.bits() &
        ~Candidate.CurrentCapabilities.bits()};
    StackPanel Details;
    Details.Spacing(10);
    TextBlock Identity;
    Identity.Text(
        L"This changes permissions for the existing authenticated device. DeskLink will not replace its stored certificate pin or run pairing again.");
    Identity.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
    Details.Children().Append(Identity);
    TextBlock Consequences;
    Consequences.Text(JoinText(
        L"This PC would additionally allow:\n", CapabilitySummary(Added)));
    Consequences.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
    Details.Children().Append(Consequences);
    TextBlock Safety;
    Safety.Text(
        L"Allowing returns input Local, stops the active peer, stores the reviewed grant, and reconnects with a fresh session. Closing this dialog grants nothing. Any earlier removals remain revoked.");
    Safety.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
    Details.Children().Append(Safety);
    Dialog.Content(Details);

    const auto Result = co_await Dialog.ShowAsync();
    ModalDialogActive_ = false;
    const bool Approved = Result == ContentDialogResult::Primary;
    const auto Response = Send(
        desklink::ResolvePermissionCandidateControlRequest{
            Candidate.OperationId, Approved},
        desklink::kProductPermissionResolutionTimeout);
    DisplayedPermissionOperation_ = 0;

    bool DesiredPermissionsStored = false;
    if (Approved &&
        (!Response || Response->Status != desklink::ControlStatus::Ok)) {
        const auto Devices = Send(
            desklink::ListTrustedDevicesControlRequest{},
            std::chrono::milliseconds{2'000});
        if (Devices && Devices->Status == desklink::ControlStatus::Ok &&
            Devices->TrustedDevices) {
            const auto Stored = std::find_if(
                Devices->TrustedDevices->Devices.begin(),
                Devices->TrustedDevices->Devices.end(),
                [&](const auto& Device) {
                    return Device.Machine == Candidate.Machine;
                });
            DesiredPermissionsStored =
                Stored != Devices->TrustedDevices->Devices.end() &&
                Stored->Capabilities == Candidate.DesiredCapabilities;
        }
    }

    if (Approved && Response &&
        Response->Status == desklink::ControlStatus::CleanupFailed &&
        DesiredPermissionsStored) {
        DeviceStatusBar().Title(
            L"Permissions saved; connection needs attention");
        DeviceStatusBar().Message(
            L"The reviewed permissions are stored for the same identity, but DeskLink could not confirm the final runtime restart. Input remains Local and automatic reconnect is blocked until the runtime is repaired or restarted.");
        DeviceStatusBar().Severity(InfoBarSeverity::Warning);
    } else if (Approved && DesiredPermissionsStored) {
        DeviceStatusBar().Title(L"Permissions updated");
        DeviceStatusBar().Message(
            L"The reviewed permissions were verified in the protected trust record after the runtime handoff. DeskLink did not re-pair or replace the stored identity.");
        DeviceStatusBar().Severity(InfoBarSeverity::Success);
    } else if (!Response || Response->Status != desklink::ControlStatus::Ok) {
        DeviceStatusBar().Title(L"New permissions were not added");
        DeviceStatusBar().Message(
            L"The protected review expired, trust changed, or fail-local cleanup could not complete. The stored identity was not replaced and any earlier removals remain revoked.");
        DeviceStatusBar().Severity(InfoBarSeverity::Warning);
    } else if (Approved) {
        DeviceStatusBar().Title(L"Permissions updated");
        DeviceStatusBar().Message(
            L"The reviewed permission was added for the same stored identity. DeskLink did not re-pair the PCs.");
        DeviceStatusBar().Severity(InfoBarSeverity::Success);
    } else {
        DeviceStatusBar().Title(L"Current permissions kept");
        DeviceStatusBar().Message(
            L"No new authority was granted. Any permissions removed before this review remain revoked.");
        DeviceStatusBar().Severity(InfoBarSeverity::Informational);
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
    ReturnLocal();
}

void MainWindow::UpdateDeveloperInputStatus() {
    if (!DeveloperMode_) return;
    POINT Cursor{};
    if (!GetCursorPos(&Cursor)) {
        DeveloperInputStatus().Text(
            L"DEV cursor unavailable (GetCursorPos failed)");
        return;
    }

    const auto Now = std::chrono::steady_clock::now();
    if (!DeveloperRoamingSettings_) {
        const auto Directory = GetDataDirectory();
        if (Directory) {
            DeveloperRoamingSettings_ =
                std::make_unique<desklink::Win32RoamingSettingsStore>(
                    *Directory / L"roaming.settings");
        }
    }
    if (DeveloperRoamingSettings_ &&
        (LastDeveloperSettingsLoad_ ==
             std::chrono::steady_clock::time_point{} ||
         Now - LastDeveloperSettingsLoad_ >=
             std::chrono::milliseconds{500})) {
        LastDeveloperSettingsLoad_ = Now;
        DeveloperRoamingConfiguration_ =
            DeveloperRoamingSettings_->Load()
            ? DeveloperRoamingSettings_->Current()
            : std::nullopt;
    }

    std::wostringstream Output;
    Output << L"DEV cursor=(" << Cursor.x << L',' << Cursor.y << L')';
    if (!DeveloperDisplayTopology_.RefreshIfDue(
            std::chrono::milliseconds{250})) {
        Output << L" display=unavailable";
    } else {
        const auto& Topology = DeveloperDisplayTopology_.Current();
        const auto Display = std::find_if(
            Topology.Displays.begin(), Topology.Displays.end(),
            [&](const desklink::DisplayDescriptor& Value) {
                return Cursor.x >= Value.Bounds.Left &&
                    Cursor.x < Value.Bounds.Right &&
                    Cursor.y >= Value.Bounds.Top &&
                    Cursor.y < Value.Bounds.Bottom;
            });
        if (Display == Topology.Displays.end()) {
            Output << L" display=outside-current-topology";
        } else {
            Output << L" display=\""
                   << ToHString(Display->FriendlyName).c_str()
                   << L"\" bounds=[" << Display->Bounds.Left << L','
                   << Display->Bounds.Top << L".."
                   << Display->Bounds.Right - 1 << L','
                   << Display->Bounds.Bottom - 1 << L']';

            struct RouteCandidate {
                const desklink::RoamingEndpoint* Source{};
                const desklink::CrossingConfiguration* Crossing{};
                const desklink::RoamingLink* Link{};
                std::int64_t Distance{};
                std::uint16_t Along{};
                bool InSegment{};
            };
            std::optional<RouteCandidate> Candidate;
            const auto Consider = [&](const desklink::RoamingLink& Link,
                                      desklink::RoamingDirection Direction) {
                if (!Link.Enabled || !HasRoamingDirection(Link, Direction)) {
                    return;
                }
                const auto& Source = Direction ==
                        desklink::RoamingDirection::AToB
                    ? Link.EndpointA : Link.EndpointB;
                if (Source.Machine != LocalMachine_ ||
                    Source.StableDisplayIdentity != Display->StableIdentity) {
                    return;
                }
                const bool VerticalEdge =
                    Source.Side == desklink::DisplayEdgeSide::Left ||
                    Source.Side == desklink::DisplayEdgeSide::Right;
                const auto Length = VerticalEdge
                    ? Display->Bounds.Bottom - Display->Bounds.Top
                    : Display->Bounds.Right - Display->Bounds.Left;
                if (Length <= 1) return;
                const auto AlongPixel = VerticalEdge
                    ? Cursor.y - Display->Bounds.Top
                    : Cursor.x - Display->Bounds.Left;
                const auto BoundedAlong = std::clamp<std::int64_t>(
                    AlongPixel, 0, static_cast<std::int64_t>(Length - 1));
                const auto Along = static_cast<std::uint16_t>(
                    BoundedAlong * 10'000 /
                    (Length - 1));
                const bool InSegment =
                    Along >= Source.SegmentStartPermyriad &&
                    (Along < Source.SegmentEndPermyriad ||
                     Source.SegmentEndPermyriad == 10'000);
                std::int64_t Distance{};
                switch (Source.Side) {
                    case desklink::DisplayEdgeSide::Left:
                        Distance = Cursor.x - Display->Bounds.Left;
                        break;
                    case desklink::DisplayEdgeSide::Top:
                        Distance = Cursor.y - Display->Bounds.Top;
                        break;
                    case desklink::DisplayEdgeSide::Right:
                        Distance = Display->Bounds.Right - 1 - Cursor.x;
                        break;
                    case desklink::DisplayEdgeSide::Bottom:
                        Distance = Display->Bounds.Bottom - 1 - Cursor.y;
                        break;
                }
                const auto& Crossing = Direction ==
                        desklink::RoamingDirection::AToB
                    ? Link.AToB : Link.BToA;
                const RouteCandidate Next{
                    &Source, &Crossing, &Link,
                    std::max<std::int64_t>(0, Distance), Along, InSegment};
                if (!Candidate ||
                    (Next.InSegment && !Candidate->InSegment) ||
                    (Next.InSegment == Candidate->InSegment &&
                     Next.Distance < Candidate->Distance)) {
                    Candidate = Next;
                }
            };
            if (DeveloperRoamingConfiguration_) {
                for (const auto& Link :
                     DeveloperRoamingConfiguration_->Links) {
                    Consider(Link, desklink::RoamingDirection::AToB);
                    Consider(Link, desklink::RoamingDirection::BToA);
                }
            }

            Output << L"\nDEV route=";
            if (!Candidate) {
                Output << L"none on this display";
            } else {
                Output << MonitorSideName(Candidate->Source->Side)
                       << L" distance=" << Candidate->Distance
                       << L" px (need 0) segment="
                       << (Candidate->InSegment ? L"yes" : L"no")
                       << L" along=" << Candidate->Along / 100.0
                       << L"% reentry="
                       << Candidate->Link->ReentryDistancePixels << L" px ";
                switch (Candidate->Crossing->Policy) {
                    case desklink::CrossingPolicy::Push:
                        Output << L"trigger=first outward count";
                        break;
                    case desklink::CrossingPolicy::DwellAndPush:
                        Output << L"trigger=push "
                               << Candidate->Crossing->PushDistancePixels
                               << L" px + dwell "
                               << Candidate->Crossing->DwellMilliseconds
                               << L" ms";
                        break;
                    case desklink::CrossingPolicy::DoublePush:
                        Output << L"trigger=double push "
                               << Candidate->Crossing->PushDistancePixels
                               << L" px within "
                               << Candidate->Crossing
                                      ->DoublePushWindowMilliseconds
                               << L" ms";
                        break;
                }
            }
        }
    }
    Output << L"\nDEV runtime roaming="
           << DeveloperRoamingStateName(RuntimeState_.RoamingState)
           << L" direction="
           << DeveloperPeerDirectionName(RuntimeState_.PeerDirection)
           << L" observer="
           << (RuntimeState_.RoamingObserverActive ? L"on" : L"off")
           << L" ready_routes=" << RuntimeState_.ReadyRoamingRouteCount
           << L" capture="
           << (RuntimeState_.CaptureActive ? L"remote" : L"local");
    DeveloperInputStatus().Text(Output.str());
}

void MainWindow::ApplyState(desklink::ProductShellState State) {
    State_ = State;
    const auto Presentation = desklink::PresentProductShellState(State);
    StatusBadge().Text(Presentation.Badge);
    InputTitle().Text(Presentation.KeyboardAndMouseTitle);
    InputSummary().Text(Presentation.KeyboardAndMouseSummary);
    DeveloperInputStatus().Visibility(
        DeveloperMode_ && RuntimeState_.ConnectedPeerCount != 0
        ? Microsoft::UI::Xaml::Visibility::Visible
        : Microsoft::UI::Xaml::Visibility::Collapsed);
    UpdateDeveloperInputStatus();
    if (RuntimeStateLoaded_) {
        const auto Detail = RuntimeConnectionDetail(RuntimeState_);
        DiagnosticRuntimePhase().Text(Detail);
        DiagnosticRuntimeFailure().Text(
            RuntimeFailureName(RuntimeState_.RuntimeFailure));
        DiagnosticRetryStatus().Text(RuntimeRetryStatus(RuntimeState_));
        DiagnosticRecommendedAction().Text(
            RuntimeRecommendedAction(RuntimeState_));
        const bool CanRetry = RuntimeState_.RuntimePhase ==
            desklink::BrokerRuntimePhase::ActionRequired;
        DiagnosticRetryButton().Visibility(CanRetry
            ? Microsoft::UI::Xaml::Visibility::Visible
            : Microsoft::UI::Xaml::Visibility::Collapsed);
        if (State == desklink::ProductShellState::Connecting ||
            State == desklink::ProductShellState::ActionRequired) {
            InputSummary().Text(Detail);
        }
        if (State == desklink::ProductShellState::ActionRequired) {
            ActionRequiredBar().Message(
                std::wstring(RuntimeFailureName(RuntimeState_.RuntimeFailure)) +
                L". Input remains Local. Review Diagnostics or select Retry now.");
        }
    } else {
        DiagnosticRuntimePhase().Text(Presentation.ConnectionDetail);
        DiagnosticRuntimeFailure().Text(L"Runtime state unavailable");
        DiagnosticRetryStatus().Text(L"Unknown");
        DiagnosticRecommendedAction().Text(
            L"Start or repair the DeskLink runtime, then run the check again.");
        DiagnosticRetryButton().Visibility(
            Microsoft::UI::Xaml::Visibility::Collapsed);
    }
    ReturnLocalButton().Visibility(Presentation.ShowReturnLocal
        ? Microsoft::UI::Xaml::Visibility::Visible
        : Microsoft::UI::Xaml::Visibility::Collapsed);
    ActionRequiredBar().IsOpen(Presentation.ShowActionRequired);
    UpdateHome();
}

void MainWindow::OnRunConnectionCheck(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    RunConnectionCheck();
}

void MainWindow::RunConnectionCheck() {
    PollBroker();
    using Microsoft::UI::Xaml::Controls::InfoBarSeverity;
    DiagnosticResultBar().Title(L"Connection check complete");
    if (!BrokerAvailable_ || !RuntimeStateLoaded_) {
        DiagnosticResultBar().Severity(InfoBarSeverity::Error);
        DiagnosticResultBar().Message(
            L"The DeskLink broker did not answer. Input remains on this PC. Restart or repair DeskLink.");
    } else {
        const auto Failure = RuntimeFailureName(RuntimeState_.RuntimeFailure);
        const auto Action = RuntimeRecommendedAction(RuntimeState_);
        std::wstring Message = RuntimeConnectionDetail(RuntimeState_);
        Message += L" Last failure: ";
        Message += Failure;
        Message += L". ";
        Message += Action;
        DiagnosticResultBar().Message(Message);
        DiagnosticResultBar().Severity(
            RuntimeState_.RuntimePhase ==
                    desklink::BrokerRuntimePhase::ActionRequired
                ? InfoBarSeverity::Error
                : RuntimeState_.RuntimePhase ==
                          desklink::BrokerRuntimePhase::ConnectedLocal
                    ? InfoBarSeverity::Success
                    : InfoBarSeverity::Warning);
    }
    DiagnosticResultBar().IsOpen(true);
}

void MainWindow::OnRetryConnection(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
    using Microsoft::UI::Xaml::Controls::InfoBarSeverity;
    const auto Response = Send(
        desklink::ResumeDeskLinkControlRequest{},
        std::chrono::milliseconds{5'500});
    DiagnosticResultBar().Title(Response &&
            Response->Status == desklink::ControlStatus::Ok
        ? L"Retry started"
        : L"Retry could not start");
    DiagnosticResultBar().Severity(Response &&
            Response->Status == desklink::ControlStatus::Ok
        ? InfoBarSeverity::Informational
        : InfoBarSeverity::Error);
    DiagnosticResultBar().Message(Response &&
            Response->Status == desklink::ControlStatus::Ok
        ? L"DeskLink returned input Local and started a fresh authenticated connection attempt."
        : L"The broker could not safely restart the connection. Input remains Local; restart or repair DeskLink.");
    DiagnosticResultBar().IsOpen(true);
    PollBroker();
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

void MainWindow::ShowFromTray() {
    if (!MainWindowHandle_) return;
    ShowWindow(MainWindowHandle_, SW_SHOW);
    SetForegroundWindow(MainWindowHandle_);
}

void MainWindow::RequestExit() {
    if (ExplicitExit_) return;
    ExplicitExit_ = true;
    // The UI owns this PTT gesture. Never leave its local capture authority
    // asserted when the window/process lifetime ends.
    (void)Send(desklink::SetVoiceTransmitControlRequest{false});
    if (PollTimer_) PollTimer_.Stop();
    if (PairingDialog_) PairingDialog_.Hide();
    if (MainWindowHandle_) SendMessageW(MainWindowHandle_, WM_CLOSE, 0, 0);
    Microsoft::UI::Xaml::Application::Current().Exit();
}

LRESULT CALLBACK MainWindow::MainWindowSubclassProcedure(
    HWND Window, UINT Message, WPARAM WParam, LPARAM LParam,
    UINT_PTR, DWORD_PTR ReferenceData) {
    const auto Self = reinterpret_cast<MainWindow*>(ReferenceData);
    if (Message == WM_CLOSE && Self && !Self->ExplicitExit_) {
        Self->RequestExit();
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
    return DefWindowProcW(Window, Message, WParam, LParam);
}

} // namespace winrt::DeskLink::Product::implementation
