[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $InstallerPath,

    [Parameter(Mandatory = $true)]
    [string] $UpgradeInstallerPath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string] $ExpectedUpgradeVersion,

    [Parameter(Mandatory = $true)]
    [string] $UpdateValidationPath,

    [switch] $AllowCurrentUserMutation
)

$ErrorActionPreference = 'Stop'
if (-not $AllowCurrentUserMutation) {
    throw 'Installer validation mutates the current-user install/registry state. Pass AllowCurrentUserMutation only on an isolated test account.'
}
$InstallerPath = (Resolve-Path -LiteralPath $InstallerPath).Path
$UpgradeInstallerPath = (Resolve-Path -LiteralPath $UpgradeInstallerPath).Path
$UpdateValidationPath = (Resolve-Path -LiteralPath $UpdateValidationPath).Path
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
        'desklink.exe',
        'desklink_alpha.exe',
        'desklink_pair.exe',
        'desklink_runtime.exe',
        'desklink_update.exe',
        'runtime\schannel\msquic.dll',
        'concrt140.dll',
        'msvcp140.dll',
        'msvcp140_1.dll',
        'msvcp140_2.dll',
        'msvcp140_atomic_wait.dll',
        'msvcp140_codecvt_ids.dll',
        'vcruntime140.dll',
        'vcruntime140_1.dll',
        'Microsoft.WindowsAppRuntime.dll',
        'Microsoft.ui.xaml.dll',
        'App.xbf',
        'MainWindow.xbf',
        'CppWinRT-LICENSE.txt',
        'WindowsAppSDK-LICENSE.txt',
        'WindowsAppSDK-Runtime-NOTICE.txt',
        'WindowsAppSDK-WinUI-NOTICE.txt',
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

function Wait-ForBrokerReady() {
    $Deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        $Probe = Start-Process `
            -FilePath (Join-Path $InstallPath 'desklink_pair.exe') `
            -ArgumentList @('control', 'state') `
            -RedirectStandardOutput $GateOutputPath `
            -RedirectStandardError $GateErrorPath `
            -Wait -PassThru
        if ($Probe.ExitCode -eq 0) { return }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $Deadline)

    $Failure = if (Test-Path -LiteralPath $GateErrorPath) {
        (Get-Content -Raw -LiteralPath $GateErrorPath).Trim()
    } else {
        'no control-client diagnostics were produced'
    }
    throw "Installed runtime broker did not become ready: $Failure"
}

function Invoke-CoordinatedUpdate(
    [string] $Coordinator,
    [switch] $AllowUnsigned,
    [switch] $FailCandidateValidation,
    [string] $ExpectedState,
    [string] $ExpectedFailure) {
    $TransactionsPath = Join-Path $StatePath 'UpdateTransactions'
    $Before = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    if (Test-Path -LiteralPath $TransactionsPath -PathType Container) {
        foreach ($Directory in Get-ChildItem -LiteralPath $TransactionsPath `
                -Directory -Filter 'tx-*') {
            [void] $Before.Add($Directory.Name)
        }
    }
    $Arguments = @(
        'apply', '--candidate', $UpgradeInstallerPath,
        '--candidate-sha256',
        (Get-FileHash -Algorithm SHA256 -LiteralPath $UpgradeInstallerPath).Hash,
        '--rollback', $InstallerPath,
        '--rollback-sha256',
        (Get-FileHash -Algorithm SHA256 -LiteralPath $InstallerPath).Hash
    )
    if ($AllowUnsigned) { $Arguments += '--development-allow-unsigned' }
    if ($FailCandidateValidation) {
        $Arguments += '--development-fail-candidate-validation'
    }
    $Starter = Start-Process -FilePath $Coordinator -ArgumentList $Arguments `
        -Wait -PassThru
    if ($Starter.ExitCode -ne 0) {
        throw "Update coordinator starter failed with exit code $($Starter.ExitCode)"
    }

    $Transaction = $null
    $DiscoveryDeadline = [DateTime]::UtcNow.AddSeconds(10)
    do {
        if (Test-Path -LiteralPath $TransactionsPath -PathType Container) {
            $Transaction = Get-ChildItem -LiteralPath $TransactionsPath `
                -Directory -Filter 'tx-*' |
                Where-Object { -not $Before.Contains($_.Name) } |
                Sort-Object LastWriteTimeUtc -Descending |
                Select-Object -First 1
        }
        if (-not $Transaction) { Start-Sleep -Milliseconds 100 }
    } while (-not $Transaction -and [DateTime]::UtcNow -lt $DiscoveryDeadline)
    if (-not $Transaction) {
        throw 'Update coordinator did not create a bounded transaction directory.'
    }

    $ResultPath = Join-Path $Transaction.FullName 'result.txt'
    $ResultDeadline = [DateTime]::UtcNow.AddMinutes(2)
    while (-not (Test-Path -LiteralPath $ResultPath -PathType Leaf) -and
           [DateTime]::UtcNow -lt $ResultDeadline) {
        Start-Sleep -Milliseconds 100
    }
    if (-not (Test-Path -LiteralPath $ResultPath -PathType Leaf)) {
        throw 'Update coordinator did not finish within two minutes.'
    }
    $Result = ConvertFrom-StringData (Get-Content -Raw -LiteralPath $ResultPath)
    if ($Result.state -ne $ExpectedState -or
        $Result.failure -ne $ExpectedFailure) {
        throw "Unexpected update result state=$($Result.state) failure=$($Result.failure)"
    }
    return $Result
}

try {
    New-Item -ItemType Directory -Path $StatePath -Force | Out-Null
    Set-Content -LiteralPath $SentinelPath -Value 'preserve-on-uninstall' `
        -Encoding utf8 -NoNewline

    foreach ($MutexName in
            'Local\DeskLink.Alpha.v1', 'Local\DeskLink.Shell.v1') {
        $GateMutex = [Threading.Mutex]::new($false, $MutexName)
        try {
            $Blocked = Start-Process -FilePath $InstallerPath -ArgumentList @(
                '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART'
            ) -Wait -PassThru
            if ($Blocked.ExitCode -eq 0) {
                throw "Installer succeeded while lifecycle mutex $MutexName was active."
            }
        } finally {
            $GateMutex.Dispose()
        }
    }

    $Impersonated = Start-Process -FilePath $InstallerPath -ArgumentList @(
        '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART',
        '/DESKLINKCOORDINATED'
    ) -Wait -PassThru
    if ($Impersonated.ExitCode -eq 0) {
        throw 'Setup impersonated a coordinated update without the update gate.'
    }

    Invoke-Installer $InstallerPath 'Initial installation'
    Assert-InstalledPayload
    if (-not (Test-Path -LiteralPath $UninstallKeyPath)) {
        throw 'DeskLink was not registered in the current-user uninstall registry.'
    }
    if (Test-Path -LiteralPath $MachineUninstallKeyPath) {
        throw 'DeskLink unexpectedly registered a machine-wide uninstall entry.'
    }

    $Alpha = Start-Process -FilePath (Join-Path $InstallPath 'desklink_alpha.exe') `
        -ArgumentList '--background' -PassThru
    Start-Sleep -Seconds 1
    if ($Alpha.HasExited) {
        throw 'Installed Alpha UI did not remain active for update coordination.'
    }
    Wait-ForBrokerReady

    $Shell = Start-Process -FilePath (Join-Path $InstallPath 'desklink.exe') `
        -ArgumentList '--background' -PassThru
    Start-Sleep -Seconds 2
    if ($Shell.HasExited) {
        throw 'Installed product shell did not remain active in the notification area.'
    }
    $Activation = Start-Process `
        -FilePath (Join-Path $InstallPath 'desklink.exe') -Wait -PassThru
    if ($Activation.ExitCode -ne 0 -or $Shell.HasExited) {
        throw 'Single-instance product-shell activation failed.'
    }
    $ExitActivation = Start-Process `
        -FilePath (Join-Path $InstallPath 'desklink.exe') `
        -ArgumentList '--request-exit' -Wait -PassThru
    if ($ExitActivation.ExitCode -ne 0 -or
        -not $Shell.WaitForExit(15000)) {
        throw 'The product shell did not honor bounded explicit exit.'
    }
    Wait-ForBrokerReady

    $ShellForUpdate = Start-Process `
        -FilePath (Join-Path $InstallPath 'desklink.exe') `
        -ArgumentList '--background' -PassThru
    Start-Sleep -Seconds 2
    if ($ShellForUpdate.HasExited) {
        throw 'The product shell did not start for update coordination.'
    }

    $Rejected = Invoke-CoordinatedUpdate `
        -Coordinator (Join-Path $InstallPath 'desklink_update.exe') `
        -ExpectedState failed -ExpectedFailure package-validation
    if ($Alpha.HasExited) {
        throw 'Production package rejection disturbed the running Alpha UI.'
    }
    if ($ShellForUpdate.HasExited) {
        throw 'Production package rejection disturbed the product shell.'
    }
    $RejectedVersion =
        (Get-ItemProperty -LiteralPath $UninstallKeyPath).DisplayVersion
    if ($RejectedVersion -ne '0.1.0') {
        throw 'Rejected production update changed the installed version.'
    }

    $RolledBack = Invoke-CoordinatedUpdate `
        -Coordinator $UpdateValidationPath -AllowUnsigned `
        -FailCandidateValidation -ExpectedState rolled-back `
        -ExpectedFailure candidate-validation
    [void] $Alpha.WaitForExit(30000)
    if (-not $Alpha.HasExited) {
        throw 'Coordinated update did not stop the Alpha UI.'
    }
    [void] $ShellForUpdate.WaitForExit(30000)
    if (-not $ShellForUpdate.HasExited) {
        throw 'Coordinated update did not stop the product shell.'
    }
    $RollbackVersion =
        (Get-ItemProperty -LiteralPath $UninstallKeyPath).DisplayVersion
    if ($RollbackVersion -ne '0.1.0') {
        throw "Rollback restored version $RollbackVersion instead of 0.1.0"
    }
    Assert-InstalledPayload

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
            $GateError -notmatch 'install or update operation is active') {
            throw 'DeskLink runtime did not fail closed while the installer mutex was active.'
        }
    } finally {
        $InstallGate.Dispose()
    }

    $UpdateGate = [Threading.Mutex]::new($false, 'Local\DeskLink.Update.v1')
    try {
        $BlockedSetup = Start-Process -FilePath $UpgradeInstallerPath `
            -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART') `
            -Wait -PassThru
        if ($BlockedSetup.ExitCode -eq 0) {
            throw 'Ordinary Setup overlapped an active coordinated update.'
        }
        $BlockedRuntime = Start-Process `
            -FilePath (Join-Path $InstallPath 'desklink_pair.exe') `
            -ArgumentList 'identity' `
            -RedirectStandardOutput $GateOutputPath `
            -RedirectStandardError $GateErrorPath `
            -Wait -PassThru
        $GateError = Get-Content -Raw -LiteralPath $GateErrorPath
        if ($BlockedRuntime.ExitCode -eq 0 -or
            $GateError -notmatch 'install or update operation is active') {
            throw 'DeskLink runtime did not fail closed while the update gate was active.'
        }
    } finally {
        $UpdateGate.Dispose()
    }

    $Updated = Invoke-CoordinatedUpdate `
        -Coordinator $UpdateValidationPath -AllowUnsigned `
        -ExpectedState completed -ExpectedFailure none
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
    if (Test-Path -LiteralPath (Join-Path $InstallPath 'desklink.exe')) {
        throw 'Uninstall left the DeskLink product shell behind.'
    }
    if (Get-ItemProperty -LiteralPath $RunKeyPath -Name DeskLink `
            -ErrorAction SilentlyContinue) {
        throw 'The DeskLink current-user startup value survived uninstall.'
    }
    if (-not (Test-Path -LiteralPath $SentinelPath -PathType Leaf) -or
        (Get-Content -Raw -LiteralPath $SentinelPath) -ne 'preserve-on-uninstall') {
        throw 'Uninstall modified or deleted current-user DeskLink state.'
    }
    Write-Host '[Packaging:Installer] current-user install, fail-closed update rejection, rollback, upgrade, startup cleanup, state preservation, and uninstall passed.'
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
