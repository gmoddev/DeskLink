[CmdletBinding(SupportsShouldProcess, ConfirmImpact = 'High')]
param(
    [Parameter(Mandatory)]
    [ValidateSet('Install', 'Uninstall', 'DefaultReleaseProbe',
        'SecureCancelProbe', 'Status')]
    [string] $Action,

    [string] $BuildPath,

    [switch] $ConfirmExperimental,

    # This literal switch is intentionally required by the source-contract
    # check. It documents that this harness cannot be called by product code.
    [switch] $DenyProductIntegration
)

$ErrorActionPreference = 'Stop'
$ServiceName = 'DeskLinkSecureInputRnd'
$InstallRoot = Join-Path $env:ProgramFiles 'DeskLink Secure Input R&D'
$ServiceFile = 'desklink_secure_service.exe'
$HelperFile = 'desklink_secure_input_helper.exe'

function Assert-Administrator {
    $Identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $Principal = [Security.Principal.WindowsPrincipal]::new($Identity)
    if (-not $Principal.IsInRole(
            [Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run this validation-only command from an elevated PowerShell window.'
    }
}

function Assert-ExperimentalConsent {
    if (-not $ConfirmExperimental -or -not $DenyProductIntegration) {
        throw 'Specify -ConfirmExperimental and -DenyProductIntegration. This is not a product installer.'
    }
}

function Get-ServiceIfPresent {
    return Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
}

function Invoke-ProbeControl([int] $Control, [string] $Description) {
    $Service = Get-ServiceIfPresent
    if (-not $Service -or $Service.Status -ne 'Running') {
        throw "$ServiceName must be installed and running before $Description."
    }
    & sc.exe control $ServiceName $Control | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "$Description could not be delivered to the service."
    }
    for ($Attempt = 0; $Attempt -lt 20; ++$Attempt) {
        Start-Sleep -Milliseconds 100
        $Service.Refresh()
        if ($Service.Status -ne 'Running') {
            throw "$Description failed closed and stopped the validation service."
        }
    }
}

switch ($Action) {
    'Status' {
        $Service = Get-ServiceIfPresent
        [pscustomobject]@{
            Installed = $null -ne $Service
            Status = if ($Service) { $Service.Status } else { 'Absent' }
            InstallRoot = $InstallRoot
        }
        break
    }
    'Install' {
        Assert-Administrator
        Assert-ExperimentalConsent
        if ([string]::IsNullOrWhiteSpace($BuildPath)) {
            throw '-BuildPath is required for Install.'
        }
        $ResolvedBuild = (Resolve-Path -LiteralPath $BuildPath).Path
        $SourceService = Join-Path $ResolvedBuild $ServiceFile
        $SourceHelper = Join-Path $ResolvedBuild $HelperFile
        foreach ($Source in $SourceService, $SourceHelper) {
            $Item = Get-Item -LiteralPath $Source -Force
            if (-not $Item -or $Item.PSIsContainer -or
                ($Item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                throw "Expected a regular, non-reparse build artifact: $Source"
            }
        }
        if (Get-ServiceIfPresent) {
            throw "$ServiceName is already installed. Uninstall it before replacing binaries."
        }
        if ($PSCmdlet.ShouldProcess($InstallRoot,
                'Install validation-only LocalSystem service and helper')) {
            [void] (New-Item -ItemType Directory -Path $InstallRoot -Force)
            Copy-Item -LiteralPath $SourceService -Destination `
                (Join-Path $InstallRoot $ServiceFile)
            Copy-Item -LiteralPath $SourceHelper -Destination `
                (Join-Path $InstallRoot $HelperFile)
            $InstalledService = Join-Path $InstallRoot $ServiceFile
            New-Service -Name $ServiceName -BinaryPathName `
                ('"{0}"' -f $InstalledService) -StartupType Manual `
                -DisplayName 'DeskLink Secure Input R&D' | Out-Null
            # SYSTEM and Administrators receive full service control. Ordinary
            # authenticated users may query/interrogate but cannot start, stop,
            # reconfigure, or send user-defined probe controls.
            $Sddl = 'D:(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;SY)(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)(A;;CCLCSWLORC;;;AU)'
            & sc.exe sdset $ServiceName $Sddl | Out-Null
            if ($LASTEXITCODE -ne 0) {
                throw 'The restricted service DACL could not be applied.'
            }
            Start-Service -Name $ServiceName
            Write-Host '[SecureInput:RAndD] validation service installed with Manual startup.'
        }
        break
    }
    'Uninstall' {
        Assert-Administrator
        Assert-ExperimentalConsent
        if ($PSCmdlet.ShouldProcess($InstallRoot,
                'Remove validation-only service and staged binaries')) {
            $Service = Get-ServiceIfPresent
            if ($Service) {
                if ($Service.Status -ne 'Stopped') {
                    Stop-Service -Name $ServiceName -Force
                }
                & sc.exe delete $ServiceName | Out-Null
                if ($LASTEXITCODE -ne 0) {
                    throw 'The validation service could not be deleted.'
                }
            }
            if (Test-Path -LiteralPath $InstallRoot) {
                $ResolvedRoot = (Resolve-Path -LiteralPath $InstallRoot).Path
                $ResolvedProgramFiles = (Resolve-Path -LiteralPath `
                    $env:ProgramFiles).Path
                if (-not $ResolvedRoot.StartsWith(
                        $ResolvedProgramFiles + '\',
                        [StringComparison]::OrdinalIgnoreCase) -or
                    [IO.Path]::GetFileName($ResolvedRoot) -ne
                        'DeskLink Secure Input R&D') {
                    throw "Refusing to remove unexpected path: $ResolvedRoot"
                }
                Remove-Item -LiteralPath $ResolvedRoot -Recurse -Force
            }
            Write-Host '[SecureInput:RAndD] validation service removed.'
        }
        break
    }
    'DefaultReleaseProbe' {
        Assert-Administrator
        Assert-ExperimentalConsent
        Invoke-ProbeControl 128 'Default-desktop release probe'
        Write-Host '[SecureInput:RAndD] Default-desktop release probe completed.'
        break
    }
    'SecureCancelProbe' {
        Assert-Administrator
        Assert-ExperimentalConsent
        Invoke-ProbeControl 129 'Secure-desktop cancel probe'
        Write-Host '[SecureInput:RAndD] secure-desktop cancel probe completed.'
        break
    }
}
