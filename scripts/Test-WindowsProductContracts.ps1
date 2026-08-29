[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$XamlPath = Join-Path $RepositoryRoot 'apps\desklink_ui\MainWindow.xaml'
$AppXamlPath = Join-Path $RepositoryRoot 'apps\desklink_ui\App.xaml'
$ManifestPath = Join-Path $RepositoryRoot 'apps\desklink_ui\app.manifest'
$ProjectPath = Join-Path $RepositoryRoot 'apps\desklink_ui\DeskLink.vcxproj'
$InstallerPath = Join-Path $RepositoryRoot 'installer\DeskLink.iss'
$BrokerPath = Join-Path $RepositoryRoot 'apps\desklink_runtime.cpp'

function Get-StrictUtf8([string] $Path) {
    $Encoding = [Text.UTF8Encoding]::new($false, $true)
    $Text = $Encoding.GetString([IO.File]::ReadAllBytes($Path))
    if ($Text.Contains([char] 0xFFFD) -or
        $Text -match 'â€|Ã.|Â.') {
        throw "Malformed or mojibake text was found in $Path"
    }
    return $Text
}

$XamlText = Get-StrictUtf8 $XamlPath
[void] (Get-StrictUtf8 $AppXamlPath)
$ManifestText = Get-StrictUtf8 $ManifestPath
$ProjectText = Get-StrictUtf8 $ProjectPath
$InstallerText = Get-StrictUtf8 $InstallerPath
$BrokerText = Get-StrictUtf8 $BrokerPath

[xml] $Xaml = $XamlText
[xml] $Manifest = $ManifestText
[xml] $Project = $ProjectText

$InteractiveNames = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::Ordinal)
foreach ($Name in @(
        'Button', 'ToggleButton', 'CheckBox', 'RadioButton', 'ToggleSwitch',
        'ComboBox', 'NumberBox', 'TextBox', 'NavigationViewItem', 'Expander')) {
    [void] $InteractiveNames.Add($Name)
}
foreach ($Node in $Xaml.SelectNodes('//*')) {
    if (-not $InteractiveNames.Contains($Node.LocalName)) { continue }
    $AccessibleName = $Node.GetAttribute('AutomationProperties.Name')
    $Label = $Node.GetAttribute('Content')
    if ([string]::IsNullOrWhiteSpace($Label)) {
        $Label = $Node.GetAttribute('Header')
    }
    if ([string]::IsNullOrWhiteSpace($Label)) {
        $Label = $Node.GetAttribute('PlaceholderText')
    }
    if ([string]::IsNullOrWhiteSpace($AccessibleName) -and
        [string]::IsNullOrWhiteSpace($Label)) {
        throw "Interactive $($Node.LocalName) lacks an accessible name."
    }
}

foreach ($Node in $Xaml.SelectNodes("//*[local-name()='NavigationViewItem']")) {
    if ([string]::IsNullOrWhiteSpace($Node.GetAttribute('AccessKey'))) {
        throw "Navigation item '$($Node.GetAttribute('Content'))' lacks an access key."
    }
}
foreach ($Name in @(
        'BrokerUnavailableBar', 'ActionRequiredBar', 'FeatureStatusBar',
        'PairingStatusBar', 'DeviceStatusBar', 'MonitorStatusBar',
        'ProfileStatusBar')) {
    $Node = $Xaml.SelectSingleNode(
        "//*[@*[local-name()='Name' and .='$Name']]")
    if (-not $Node -or
        [string]::IsNullOrWhiteSpace(
            $Node.GetAttribute('AutomationProperties.LiveSetting'))) {
        throw "Dynamic status surface $Name is not announced to assistive technology."
    }
}

$Canvas = $Xaml.SelectSingleNode(
    "//*[local-name()='Canvas' and @*[local-name()='Name' and .='MonitorCanvas']]")
$KeyboardEditor = $Xaml.SelectSingleNode(
    "//*[local-name()='Expander' and @Header='Keyboard-accessible connection editor']")
if (-not $Canvas -or
    [string]::IsNullOrWhiteSpace(
        $Canvas.GetAttribute('AutomationProperties.Name')) -or
    -not $KeyboardEditor) {
    throw 'The visual monitor canvas must retain its named keyboard-accessible equivalent.'
}

foreach ($Node in $Xaml.SelectNodes('//*')) {
    foreach ($AttributeName in 'Background', 'Foreground', 'BorderBrush') {
        $Value = $Node.GetAttribute($AttributeName)
        if (-not [string]::IsNullOrWhiteSpace($Value) -and
            $Value -ne 'Transparent' -and
            $Value -notmatch '^\{(ThemeResource|StaticResource) .+\}$') {
            throw "Hard-coded $AttributeName '$Value' bypasses theme/high-contrast resources."
        }
    }
}

$RequestedExecution = $Manifest.SelectSingleNode(
    "//*[local-name()='requestedExecutionLevel']")
$DpiAwareness = $Manifest.SelectSingleNode("//*[local-name()='dpiAwareness']")
$DpiAware = $Manifest.SelectSingleNode("//*[local-name()='dpiAware']")
if (-not $RequestedExecution -or
    $RequestedExecution.level -ne 'asInvoker' -or
    $RequestedExecution.uiAccess -ne 'false' -or
    -not $DpiAwareness -or $DpiAwareness.InnerText -notmatch '^PerMonitorV2' -or
    -not $DpiAware -or $DpiAware.InnerText -ne 'true/pm') {
    throw 'The product shell must remain asInvoker, uiAccess=false, and PerMonitorV2 DPI aware.'
}
if ($ProjectText -notmatch '<DefaultLanguage>en-US</DefaultLanguage>' -or
    $ProjectText -notmatch '<DeskLinkWindowsTargetPlatformMinVersion[^>]*>10\.0\.20348\.0</DeskLinkWindowsTargetPlatformMinVersion>' -or
    $ProjectText -notmatch '<WindowsTargetPlatformMinVersion>\$\(DeskLinkWindowsTargetPlatformMinVersion\)</WindowsTargetPlatformMinVersion>' -or
    $ProjectText -notmatch '/utf-8') {
    throw 'The product project lost its supported baseline, fallback language, or strict UTF-8 compiler contract.'
}
$WinUiBuildScript = Get-StrictUtf8 (
    Join-Path $RepositoryRoot 'scripts\Build-WinUiShell.ps1')
if ($WinUiBuildScript -notmatch '\[switch\] \$ExperimentalWindows10' -or
    $WinUiBuildScript -notmatch '10\.0\.19041\.0') {
    throw 'The product build lost its explicit Windows 10 experimental target.'
}
if (($BrokerText | Select-String -Pattern 'BuildProductLauncherArguments' -AllMatches).Matches.Count -ne 2 -or
    $BrokerText -match 'BuildLauncherArguments\(') {
    throw 'Every product-broker transport launch must use the fail-closed product provider policy.'
}
if ($InstallerText -notmatch '(?m)^PrivilegesRequired=lowest\r?$' -or
    $InstallerText -notmatch '(?m)^MinVersion=10\.0\.20348\r?$' -or
    $InstallerText -notmatch '(?m)^MinVersion=10\.0\.19045\r?$' -or
    $InstallerText -notmatch '(?m)^#ifdef ExperimentalWindows10\r?$' -or
    $InstallerText -match '(?i)netsh|New-NetFirewallRule|FirewallException|WindowsFirewall|EnableRule') {
    throw 'The installer must remain current-user, supported-baseline only, and free of automatic Firewall changes.'
}

Write-Host '[Product:Qualification] DPI, keyboard, assistive-technology, theme, Unicode, and installer policy contracts passed.'
