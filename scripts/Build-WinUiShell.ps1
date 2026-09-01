[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',

    [string] $OutputPath = '',

    [string] $IntermediatePath = '',

    [switch] $LockedMode,

    [switch] $ExperimentalWindows10
)

$ErrorActionPreference = 'Stop'
$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot 'WinUiPayload.ps1')
$ProjectPath = Join-Path $RepositoryRoot 'apps\desklink_ui\DeskLink.vcxproj'
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $RepositoryRoot "build-winui\$Configuration"
}
if ([string]::IsNullOrWhiteSpace($IntermediatePath)) {
    $IntermediatePath = Join-Path $RepositoryRoot "build-winui\obj\$Configuration"
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
$IntermediatePath = [IO.Path]::GetFullPath($IntermediatePath)

$VsWhere = Join-Path ${env:ProgramFiles(x86)} `
    'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $VsWhere -PathType Leaf)) {
    throw 'Visual Studio vswhere.exe was not found.'
}
$VisualStudioRoots = @(& $VsWhere -products '*' `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath)
$VisualStudioRoot = $VisualStudioRoots |
    Where-Object {
        Test-Path -LiteralPath (Join-Path $_ `
            'MSBuild\Microsoft\WindowsXaml\v17.0\Microsoft.Windows.UI.Xaml.Cpp.targets') `
            -PathType Leaf
    } |
    Select-Object -First 1
if ([string]::IsNullOrWhiteSpace($VisualStudioRoot)) {
    throw 'A Visual Studio installation with the x64 C++ and Windows XAML build tools was not found.'
}
$MsBuild = Join-Path $VisualStudioRoot 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path -LiteralPath $MsBuild -PathType Leaf)) {
    throw "MSBuild.exe was not found at $MsBuild"
}

New-Item -ItemType Directory -Path $OutputPath, $IntermediatePath -Force |
    Out-Null
$Arguments = @(
    $ProjectPath,
    '/restore',
    '/m:2',
    '/nologo',
    '/verbosity:minimal',
    "/p:Configuration=$Configuration",
    '/p:Platform=x64',
    '/p:VisualStudioVersion=17.0',
    "/p:DeskLinkUiOutput=$OutputPath",
    "/p:DeskLinkUiIntermediate=$IntermediatePath",
    '/p:RestorePackagesWithLockFile=true'
)
if ($LockedMode) {
    $Arguments += '/p:RestoreLockedMode=true'
}
if ($ExperimentalWindows10) {
    $Arguments += '/p:DeskLinkExperimentalWindows10=true'
    $Arguments += '/p:DeskLinkWindowsTargetPlatformMinVersion=10.0.19041.0'
}

& $MsBuild @Arguments
if ($LASTEXITCODE -ne 0) {
    throw "WinUI shell build failed with exit code $LASTEXITCODE"
}

$NuGetRoot = if (-not [string]::IsNullOrWhiteSpace($env:NUGET_PACKAGES)) {
    [IO.Path]::GetFullPath($env:NUGET_PACKAGES)
} else {
    Join-Path $env:USERPROFILE '.nuget\packages'
}
$LegalFiles = @(
    @{
        Source = 'microsoft.windowsappsdk.runtime\2.4.0\license.txt'
        Destination = 'WindowsAppSDK-LICENSE.txt'
    },
    @{
        Source = 'microsoft.windowsappsdk.runtime\2.4.0\NOTICE.txt'
        Destination = 'WindowsAppSDK-Runtime-NOTICE.txt'
    },
    @{
        Source = 'microsoft.windowsappsdk.winui\2.3.6\NOTICE.txt'
        Destination = 'WindowsAppSDK-WinUI-NOTICE.txt'
    },
    @{
        Source = 'microsoft.windows.cppwinrt\3.0.260818.1\LICENSE'
        Destination = 'CppWinRT-LICENSE.txt'
    }
)
foreach ($LegalFile in $LegalFiles) {
    $Source = Join-Path $NuGetRoot $LegalFile.Source
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf) -or
        (Get-FileHash -Algorithm SHA256 -LiteralPath $Source).Hash -ne
            $DeskLinkWinUiLegalFileHashes[$LegalFile.Destination]) {
        throw "Pinned package legal file was missing or changed: $($LegalFile.Source)"
    }
    Copy-Item -LiteralPath $Source -Destination (
        Join-Path $OutputPath $LegalFile.Destination)
}

$Executable = Join-Path $OutputPath 'desklink.exe'
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
    throw "WinUI shell build did not produce $Executable"
}
Write-Host "[ProductUI:Build] created $Executable"
Write-Host ('[ProductUI:Build] mode=' + $(
    if ($ExperimentalWindows10) { 'windows10-experimental' } else { 'production' }))
