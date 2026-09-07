[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ServicePath = Join-Path $RepositoryRoot 'apps\desklink_secure_service.cpp'
$HelperPath = Join-Path $RepositoryRoot 'apps\desklink_secure_input_helper.cpp'
$CMakePath = Join-Path $RepositoryRoot 'CMakeLists.txt'
$InstallScriptPath = Join-Path $RepositoryRoot `
    'scripts\Invoke-SecureInputRAndD.ps1'

function Get-StrictUtf8([string] $Path) {
    $Encoding = [Text.UTF8Encoding]::new($false, $true)
    $Text = $Encoding.GetString([IO.File]::ReadAllBytes($Path))
    if ($Text.Contains([char] 0xFFFD)) {
        throw "Invalid UTF-8 was found in $Path"
    }
    return $Text
}

$Service = Get-StrictUtf8 $ServicePath
$Helper = Get-StrictUtf8 $HelperPath
$CMake = Get-StrictUtf8 $CMakePath
$InstallScript = Get-StrictUtf8 $InstallScriptPath
$All = $Service + "`n" + $Helper + "`n" + $InstallScript

foreach ($Forbidden in @(
        'WinHttp', 'WinInet', 'WSAStartup', 'socket(', 'connect(',
        'NCryptExportKey', 'CryptExportKey', 'PFXExportCertStoreEx',
        'CreateRemoteThread', 'WriteProcessMemory', 'ShellExecute',
        'cmd.exe', 'powershell.exe', 'uiAccess=''true''', 'uiAccess="true"',
        'PromptOnSecureDesktop', 'EnableLUA', 'ConsentPromptBehaviorAdmin')) {
    if ($All.IndexOf($Forbidden, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
        throw "Secure-input R&D boundary references forbidden mechanism: $Forbidden"
    }
}

if ($CMake -notmatch
        'option\(DESKLINK_BUILD_SECURE_INPUT_RND[\s\S]+?OFF\)' -or
    $CMake -match
        'install\(TARGETS[^\)]*desklink_secure_(service|input_helper)') {
    throw 'Secure-input R&D targets must remain explicit, default-off, and absent from product packaging.'
}
if ($Service -notmatch 'CreateProcessAsUserW' -or
    $Service -notmatch 'TokenSessionId' -or
    $Service -notmatch 'SE_TCB_NAME' -or
    $Service -notmatch 'WTS_SESSIONSTATE_UNLOCK' -or
    $Service -notmatch 'WaitForSingleObject' -or
    $Service -notmatch 'GetExitCodeProcess' -or
    $Service -notmatch 'TerminalProbeError' -or
    $Service -notmatch 'FOLDERID_ProgramFiles' -or
    $Service -notmatch 'FILE_ATTRIBUTE_REPARSE_POINT' -or
    $Service -notmatch 'DeskLinkSecureInputRnd') {
    throw 'The service lost its fixed Program Files, active-session, or fixed-name boundary.'
}
if ($Helper -notmatch 'CheckTokenMembership' -or
    $Helper -notmatch 'WTSGetActiveConsoleSessionId' -or
    $Helper -notmatch 'WTS_SESSIONSTATE_UNLOCK' -or
    $Helper -notmatch 'OpenInputDesktop' -or
    $Helper -notmatch 'consent\.exe' -or
    $Helper -notmatch 'KEYEVENTF_SCANCODE' -or
    $Helper -notmatch 'ThreadDesktopMismatch = 16' -or
    $Helper -notmatch 'InputDesktopMismatch = 17' -or
    $Helper -notmatch 'DesktopTransitionTimedOut = 27' -or
    $Helper -notmatch 'ActiveInputDesktopName\(\)[\s\S]{0,100}L"Default"' -or
    $Helper -notmatch 'UOI_NAME' -or
    $Helper -notmatch 'secure-cancel' -or
    $Helper -match '(?i)username|password|credential|clipboard') {
    throw 'The helper lost its SYSTEM/session/desktop checks or gained sensitive-data behavior.'
}
if ($InstallScript -notmatch 'StartupType Manual' -or
    $InstallScript -notmatch 'DenyProductIntegration' -or
    $InstallScript -notmatch 'ConfirmExperimental' -or
    $InstallScript -notmatch 'sdset' -or
    $InstallScript -notmatch 'failed closed and stopped' -or
    $InstallScript -notmatch 'SERVICE_EXIT_CODE' -or
    $InstallScript -match '(?i)AutomaticDelayedStart|start=\s*auto') {
    throw 'The lab harness must remain explicit, manual-start, and R&D-only.'
}
if ($InstallScript -notmatch
        '\(A;;CCLCSWLORC;;;AU\)' -or
    $InstallScript -match '\(A;;[^\)]*CR[^\)]*;;;AU\)') {
    throw 'Authenticated users must not receive service user-defined-control access.'
}

Write-Host '[SecureInput:RAndD] privileged-boundary source contracts passed.'
