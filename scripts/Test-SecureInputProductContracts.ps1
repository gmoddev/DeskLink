[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Get-StrictUtf8([string] $Path) {
    $Encoding = [Text.UTF8Encoding]::new($false, $true)
    $Text = $Encoding.GetString([IO.File]::ReadAllBytes($Path))
    if ($Text.Contains([char] 0xFFFD)) {
        throw "Invalid UTF-8 was found in $Path"
    }
    return $Text
}

$Service = Get-StrictUtf8 (Join-Path $RepositoryRoot `
    'apps\desklink_secure_input_service.cpp')
$Helper = Get-StrictUtf8 (Join-Path $RepositoryRoot `
    'apps\desklink_secure_input_helper.cpp')
$Configurator = Get-StrictUtf8 (Join-Path $RepositoryRoot `
    'apps\desklink_secure_input_configurator.cpp')
$ProductShell = Get-StrictUtf8 (Join-Path $RepositoryRoot `
    'apps\desklink_ui\MainWindow.xaml.cpp')
$Broker = Get-StrictUtf8 (Join-Path $RepositoryRoot `
    'src\win32_secure_input.cpp')
$Runtime = Get-StrictUtf8 (Join-Path $RepositoryRoot `
    'apps\desklink_pair.cpp')
$Agent = Get-StrictUtf8 (Join-Path $RepositoryRoot 'src\agent.cpp')
$CMake = Get-StrictUtf8 (Join-Path $RepositoryRoot 'CMakeLists.txt')
$Installer = Get-StrictUtf8 (Join-Path $RepositoryRoot `
    'installer\DeskLink.iss')
$Builder = Get-StrictUtf8 (Join-Path $RepositoryRoot `
    'scripts\Build-WindowsInstaller.ps1')
$PrivilegedBoundary = $Service + "`n" + $Helper + "`n" + $Configurator
$All = $PrivilegedBoundary + "`n" + $Broker + "`n" + $Runtime +
    "`n" + $Agent + "`n" + $CMake + "`n" + $Installer + "`n" + $Builder

foreach ($Forbidden in @(
        'WinHttp', 'WinInet', 'WSAStartup',
        'NCryptExportKey', 'CryptExportKey', 'PFXExportCertStoreEx',
        'CreateRemoteThread', 'WriteProcessMemory',
        'uiAccess=''true''', 'uiAccess="true"',
        'PromptOnSecureDesktop', 'EnableLUA', 'ConsentPromptBehaviorAdmin')) {
    if ($All.IndexOf($Forbidden, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
        throw "Secure-input product boundary references forbidden mechanism: $Forbidden"
    }
}
foreach ($Forbidden in @(
        'WinHttp', 'WinInet', 'WSAStartup')) {
    if ($PrivilegedBoundary.IndexOf(
            $Forbidden, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
        throw "The privileged broker must remain networkless: $Forbidden"
    }
}
if ($PrivilegedBoundary -match
        '(?i)(?<![A-Za-z0-9_])(socket|connect)\s*\(') {
    throw 'The privileged broker must not call socket() or connect().'
}

if ($CMake -notmatch
        'option\(DESKLINK_BUILD_SECURE_INPUT_PRODUCT[\s\S]+?OFF\)' -or
    $CMake -notmatch 'desklink_secure_input_service' -or
    $CMake -notmatch 'desklink_secure_input_configurator' -or
    $CMake -notmatch "MANIFESTUAC:level='asInvoker' uiAccess='false'" -or
    $CMake -notmatch "MANIFESTUAC:level='requireAdministrator' uiAccess='false'") {
    throw 'Secure-input product targets must remain explicit, default-off, and non-UIAccess.'
}
foreach ($Required in @(
        'WinVerifyTrust', 'AuthenticodeSignerHash',
        'GetNamedPipeClientProcessId', 'PIPE_REJECT_REMOTE_CLIENTS',
        'WTSGetActiveConsoleSessionId', 'LoadProtectedGrant',
        'SecureInputAuthorizationGate', 'CreateProcessAsUserW',
        'PrepareHelper', 'kDiagnosticRegistryPath',
        'FOLDERID_ProgramFiles', 'FILE_ATTRIBUTE_REPARSE_POINT',
        'desklink_pair.exe')) {
    if ($Service.IndexOf($Required, [StringComparison]::Ordinal) -lt 0) {
        throw "Secure-input service lost required boundary: $Required"
    }
}
if ($Service -notmatch 'Operation::ReconcileState[\s\S]{0,100}SecureInputOperation::ReconcileState' -or
    $Service -notmatch 'DefaultResult\s*!=\s*Status::DesktopUnavailable' -or
    $Service -match '(?i)CreateProcessW\([^\)]*(request|payload)' -or
    $Service -match '(?i)ShellExecute') {
    throw 'The service must preserve exact operations and fixed helper launch behavior.'
}

foreach ($Required in @(
        'CheckTokenMembership', 'WTSGetActiveConsoleSessionId',
        'WTS_SESSIONSTATE_UNLOCK', 'OpenInputDesktop', 'consent.exe',
        'Ready.Result',
        'KEYEVENTF_SCANCODE', 'Operation::ReleaseOwnedState',
        'Operation::PointerMotion', 'Operation::PointerPosition',
        'MOUSEEVENTF_ABSOLUTE', 'MOUSEEVENTF_VIRTUALDESK',
        'Operation::MouseButton')) {
    if ($Helper.IndexOf($Required, [StringComparison]::Ordinal) -lt 0) {
        throw "Secure-input helper lost required constraint: $Required"
    }
}
if ($Helper -notmatch
        'SecureDesktop\s*&&[\s\S]{0,180}Operation::Key[\s\S]{0,180}Operation::ReconcileState[\s\S]{0,120}Status::SecureOperationBlocked') {
    throw 'The secure desktop must reject keyboard and reconciliation input.'
}
if ($Service -notmatch
        'ReadyWait\s*=\s*WaitForMultipleObjects\([\s\S]{0,300}FALSE,\s*500\)[\s\S]{0,2400}Ready\.Result\s*!=\s*Status::Ok') {
    throw 'The service must require a bounded helper-readiness acknowledgement.'
}
if ($Service -match 'PeekNamedPipe') {
    throw 'The secure-input hot path must use event-driven IPC instead of polling.'
}
if ($Service -notmatch 'FILE_FLAG_OVERLAPPED' -or
    $Service -notmatch 'TransferPipeWithStop' -or
    $Service -notmatch 'ConnectPipeWithStop') {
    throw 'The secure-input service must use cancellable overlapped pipe I/O.'
}
if ($Service -notmatch 'Message\.RequestedOperation\s*==\s*Operation::Authorize[\s\S]{0,1800}PrepareHelper\(false\)[\s\S]{0,1800}Gate_\.Authorize') {
    throw 'The Default helper must be ready before the secure-input authorization lease starts.'
}

if ($Configurator -notmatch 'RegSetKeySecurity' -or
    $Configurator -notmatch 'PeerCertificateDerHash' -or
    $Configurator -notmatch 'SetDword\(Key, L"Enabled", Disabled\)' -or
    $Configurator -notmatch 'SetDword\(Key, L"Enabled", Enabled\)') {
    throw 'The configurator lost its protected, fail-disabled exact-peer grant.'
}
foreach ($Required in @(
        'ShellExecuteExW', 'SEE_MASK_NOCLOSEPROCESS',
        'PollSecureInputConfiguration', 'WaitForSingleObject',
        'Elevated control enabled', 'Protected review canceled',
        'protected stored state did not match the exact requested peer')) {
    if ($ProductShell.IndexOf($Required, [StringComparison]::Ordinal) -lt 0) {
        throw "The product shell lost protected configurator completion handling: $Required"
    }
}
if ($Broker -notmatch 'kPipeName' -or
    $Broker -notmatch 'kRegistryPath' -or
    $Broker -notmatch 'PeerCertificateDerHash' -or
    $Broker -notmatch 'SessionNonce_' -or
    $Broker -notmatch 'GrantRevision_' -or
    $Broker -notmatch 'Sequence_') {
    throw 'The runtime broker lost its fixed local IPC or replay-bound identity envelope.'
}
if ($Broker -notmatch 'PreservesAuthorizationAfterForwardFailure' -or
    $Broker -match
        'Result->Result\s*==\s*Status::SecureOperationBlocked[\s\S]{0,120}Revoke\(') {
    throw 'Expected secure-desktop operation blocking must preserve the existing exact authorization.'
}
if ($Runtime -notmatch
        'PrivilegedInput\([\s\S]{0,220}Trusted\.SessionNonce' -or
    $Agent -notmatch 'PrivilegedInput_->Begin' -or
    $Agent -notmatch 'PrivilegedInput_->Forward' -or
    $Agent -notmatch 'PrivilegedInput_->Revoke') {
    throw 'The broker must remain downstream of an already trusted DeskLink session.'
}

foreach ($Required in @(
        '#ifdef DevelopmentSecure',
        'desklink_secure_input_service.exe',
        'desklink_secure_input_helper.exe',
        'desklink_secure_input_configurator.exe',
        'start= delayed-auto obj= LocalSystem',
        'sdset DeskLinkSecureInput',
        'failureflag DeskLinkSecureInput 1',
        'AfterInstall: InstallSecureInputService',
        'PrepareToInstall',
        'SecureInputServicePrepared',
        'RegDeleteKeyIncludingSubkeys')) {
    if ($Installer.IndexOf($Required, [StringComparison]::Ordinal) -lt 0) {
        throw "Development Secure installer lost required service contract: $Required"
    }
}
if ($Builder -notmatch
        'if \(\$DevelopmentSelfSigned\)[\s\S]{0,260}desklink_secure_input_service\.exe' -or
    $Builder -notmatch
        'Assert-AuthenticodeSignature[\s\S]{0,200}\$Certificate\.Thumbprint -RequireTimestamp') {
    throw 'Only the fully signed Development Secure package may stage the broker.'
}

Write-Host '[SecureInput:Product] explicit opt-in broker contracts passed.'
