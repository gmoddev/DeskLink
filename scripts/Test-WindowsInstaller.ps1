[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $InstallerPath,

    [Parameter(Mandatory = $true)]
    [string] $UpgradeInstallerPath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string] $ExpectedUpgradeVersion,

    [switch] $AllowCurrentUserMutation
)

$ErrorActionPreference = 'Stop'
if (-not $AllowCurrentUserMutation) {
    throw 'Installer validation mutates the current-user install/registry state. Pass AllowCurrentUserMutation only on an isolated test account.'
}
$InstallerPath = (Resolve-Path -LiteralPath $InstallerPath).Path
$UpgradeInstallerPath = (Resolve-Path -LiteralPath $UpgradeInstallerPath).Path
$InstallPath = Join-Path $env:LOCALAPPDATA 'Programs\DeskLink'
$StatePath = Join-Path $env:LOCALAPPDATA 'DeskLink'
$RunKeyPath = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$UninstallKeyPath =
    'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{58944975-11A2-4DD6-B881-A0700574270F}_is1'
$MachineUninstallKeyPath =
    'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{58944975-11A2-4DD6-B881-A0700574270F}_is1'
$StatePathExisted = Test-Path -LiteralPath $StatePath -PathType Container
$SentinelName = 'installer-preservation-' + [guid]::NewGuid().ToString('N') + '.txt'
$SentinelPath = Join-Path $StatePath $SentinelName
$GateOutputPath = Join-Path $env:TEMP `
    ('DeskLinkInstallerGate-' + [guid]::NewGuid().ToString('N') + '.out')
$GateErrorPath = $GateOutputPath + '.err'
if ((Test-Path -LiteralPath $InstallPath) -or
    (Test-Path -LiteralPath $UninstallKeyPath) -or
    (Get-ItemProperty -LiteralPath $RunKeyPath -Name DeskLink `
        -ErrorAction SilentlyContinue)) {
    throw 'Installer validation requires an account with no existing DeskLink install or startup registration.'
}

function Invoke-Installer([string] $Path, [string] $Operation) {
    $Process = Start-Process -FilePath $Path -ArgumentList @(
        '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/NOCLOSEAPPLICATIONS'
    ) -Wait -PassThru
    if ($Process.ExitCode -ne 0) {
        throw "$Operation failed with exit code $($Process.ExitCode)"
    }
}

function Assert-InstalledPayload() {
    $ExpectedFiles = @(
        'desklink_alpha.exe',
        'desklink_pair.exe',
        'runtime\schannel\msquic.dll',
        'concrt140.dll',
        'msvcp140.dll',
        'msvcp140_1.dll',
        'msvcp140_2.dll',
        'msvcp140_atomic_wait.dll',
        'msvcp140_codecvt_ids.dll',
        'vcruntime140.dll',
        'vcruntime140_1.dll',
        'LICENSE',
        'ALPHA_WRAPPER.md'
    )
    foreach ($RelativePath in $ExpectedFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $InstallPath $RelativePath) `
                -PathType Leaf)) {
            throw "Installed payload is missing: $RelativePath"
        }
    }
}

try {
    New-Item -ItemType Directory -Path $StatePath -Force | Out-Null
    Set-Content -LiteralPath $SentinelPath -Value 'preserve-on-uninstall' `
        -Encoding utf8 -NoNewline

    $GateMutex = [Threading.Mutex]::new($false, 'Local\DeskLink.Alpha.v1')
    try {
        $Blocked = Start-Process -FilePath $InstallerPath -ArgumentList @(
            '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART'
        ) -Wait -PassThru
        if ($Blocked.ExitCode -eq 0) {
            throw 'Installer succeeded while the DeskLink lifecycle mutex was active.'
        }
    } finally {
        $GateMutex.Dispose()
    }

    Invoke-Installer $InstallerPath 'Initial installation'
    Assert-InstalledPayload
    if (-not (Test-Path -LiteralPath $UninstallKeyPath)) {
        throw 'DeskLink was not registered in the current-user uninstall registry.'
    }
    if (Test-Path -LiteralPath $MachineUninstallKeyPath) {
        throw 'DeskLink unexpectedly registered a machine-wide uninstall entry.'
    }

    $InstallGate = [Threading.Mutex]::new($false, 'Local\DeskLink.Install.v1')
    try {
        $BlockedRuntime = Start-Process `
            -FilePath (Join-Path $InstallPath 'desklink_pair.exe') `
            -ArgumentList 'identity' `
            -RedirectStandardOutput $GateOutputPath `
            -RedirectStandardError $GateErrorPath `
            -Wait -PassThru
        $GateError = Get-Content -Raw -LiteralPath $GateErrorPath
        if ($BlockedRuntime.ExitCode -eq 0 -or
            $GateError -notmatch 'installer operation is active') {
            throw 'DeskLink runtime did not fail closed while the installer mutex was active.'
        }
    } finally {
        $InstallGate.Dispose()
    }

    Invoke-Installer $UpgradeInstallerPath 'In-place upgrade'
    Assert-InstalledPayload
    $InstalledVersion = (Get-ItemProperty -LiteralPath $UninstallKeyPath).DisplayVersion
    if ($InstalledVersion -ne $ExpectedUpgradeVersion) {
        throw "Upgrade registered version $InstalledVersion instead of $ExpectedUpgradeVersion"
    }

    New-Item -Path $RunKeyPath -Force | Out-Null
    Set-ItemProperty -LiteralPath $RunKeyPath -Name DeskLink `
        -Value ('"' + (Join-Path $InstallPath 'desklink_alpha.exe') + '"')
    $Uninstaller = Join-Path $InstallPath 'unins000.exe'
    if (-not (Test-Path -LiteralPath $Uninstaller -PathType Leaf)) {
        throw 'The current-user uninstaller was not installed.'
    }
    $Uninstall = Start-Process -FilePath $Uninstaller -ArgumentList @(
        '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART'
    ) -Wait -PassThru
    if ($Uninstall.ExitCode -ne 0) {
        throw "Uninstall failed with exit code $($Uninstall.ExitCode)"
    }
    if (Test-Path -LiteralPath $UninstallKeyPath) {
        throw 'The current-user uninstall registration was not removed.'
    }
    if (Test-Path -LiteralPath (Join-Path $InstallPath 'desklink_alpha.exe')) {
        throw 'Uninstall left installer-owned DeskLink binaries behind.'
    }
    if (Get-ItemProperty -LiteralPath $RunKeyPath -Name DeskLink `
            -ErrorAction SilentlyContinue) {
        throw 'The DeskLink current-user startup value survived uninstall.'
    }
    if (-not (Test-Path -LiteralPath $SentinelPath -PathType Leaf) -or
        (Get-Content -Raw -LiteralPath $SentinelPath) -ne 'preserve-on-uninstall') {
        throw 'Uninstall modified or deleted current-user DeskLink state.'
    }
    Write-Host '[Packaging:Installer] current-user install, upgrade, startup cleanup, state preservation, and uninstall passed.'
} finally {
    $CleanupUninstaller = Join-Path $InstallPath 'unins000.exe'
    if (Test-Path -LiteralPath $CleanupUninstaller -PathType Leaf) {
        Start-Process -FilePath $CleanupUninstaller -ArgumentList @(
            '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART'
        ) -Wait -ErrorAction SilentlyContinue | Out-Null
    }
    if (Test-Path -LiteralPath $SentinelPath) {
        Remove-Item -LiteralPath $SentinelPath -Force
    }
    foreach ($GatePath in $GateOutputPath, $GateErrorPath) {
        if (Test-Path -LiteralPath $GatePath) {
            Remove-Item -LiteralPath $GatePath -Force
        }
    }
    if (-not $StatePathExisted -and
        (Test-Path -LiteralPath $StatePath -PathType Container) -and
        -not (Get-ChildItem -LiteralPath $StatePath -Force)) {
        Remove-Item -LiteralPath $StatePath -Force
    }
}
