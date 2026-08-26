Set-StrictMode -Version Latest

$DeskLinkWinUiLegalFileHashes = [ordered]@{
    'WindowsAppSDK-LICENSE.txt' =
        '5B11E6347756E40FE0274BC08C97F89201B94F0D50181A09A00F1F4740840501'
    'WindowsAppSDK-Runtime-NOTICE.txt' =
        '572B43D41DEA717DAE7DC5DE69ACB20A74DF025E8F5A3C0AA6F7BCA02615E23C'
    'WindowsAppSDK-WinUI-NOTICE.txt' =
        'E25393C0D340A1821827B093FA4DBBFCCCD8FEB7BF769E7FA773E3955CD5314B'
    'CppWinRT-LICENSE.txt' =
        'C2CFCCB812FE482101A8F04597DFC5A9991A6B2748266C47AC91B6A5AAE15383'
}

$DeskLinkWinUiRootFiles = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
foreach ($Name in @(
        'App.xbf',
        'CoreMessagingXP.dll',
        'CppWinRT-LICENSE.txt',
        'dcompi.dll',
        'desklink.exe',
        'desklink.pri',
        'DeskLink.Product.winmd',
        'dwmcorei.dll',
        'DwmSceneI.dll',
        'MainWindow.xbf',
        'marshal.dll',
        'Microsoft.DirectManipulation.dll',
        'Microsoft.Foundation.winmd',
        'Microsoft.Graphics.Display.dll',
        'Microsoft.Graphics.winmd',
        'Microsoft.InputStateManager.dll',
        'Microsoft.Internal.FrameworkUdk.dll',
        'Microsoft.Security.Authentication.OAuth.winmd',
        'Microsoft.UI.Composition.OSSupport.dll',
        'Microsoft.UI.Designer.dll',
        'Microsoft.UI.dll',
        'Microsoft.UI.Input.dll',
        'Microsoft.UI.pri',
        'Microsoft.UI.Text.winmd',
        'Microsoft.UI.Windowing.Core.dll',
        'Microsoft.UI.Windowing.dll',
        'Microsoft.UI.winmd',
        'Microsoft.UI.Xaml.Controls.dll',
        'Microsoft.UI.Xaml.Controls.pri',
        'Microsoft.ui.xaml.dll',
        'Microsoft.UI.Xaml.Internal.dll',
        'Microsoft.UI.Xaml.Phone.dll',
        'Microsoft.ui.xaml.resources.19h1.dll',
        'Microsoft.ui.xaml.resources.common.dll',
        'Microsoft.UI.Xaml.winmd',
        'Microsoft.Web.WebView2.Core.dll',
        'Microsoft.Web.WebView2.Core.Projection.dll',
        'Microsoft.Web.WebView2.Core.winmd',
        'Microsoft.Windows.ApplicationModel.Background.UniversalBGTask.winmd',
        'Microsoft.Windows.ApplicationModel.Background.winmd',
        'Microsoft.Windows.ApplicationModel.DynamicDependency.winmd',
        'Microsoft.Windows.ApplicationModel.Resources.dll',
        'Microsoft.Windows.ApplicationModel.Resources.winmd',
        'Microsoft.Windows.ApplicationModel.WindowsAppRuntime.winmd',
        'Microsoft.Windows.AppLifecycle.winmd',
        'Microsoft.Windows.AppNotifications.Builder.winmd',
        'Microsoft.Windows.AppNotifications.winmd',
        'Microsoft.Windows.BadgeNotifications.winmd',
        'Microsoft.Windows.Foundation.winmd',
        'Microsoft.Windows.Globalization.winmd',
        'Microsoft.Windows.Management.Deployment.winmd',
        'Microsoft.Windows.Media.Capture.winmd',
        'Microsoft.Windows.PushNotifications.winmd',
        'Microsoft.Windows.Security.AccessControl.winmd',
        'Microsoft.Windows.Storage.Pickers.winmd',
        'Microsoft.Windows.Storage.winmd',
        'Microsoft.Windows.System.Power.winmd',
        'Microsoft.Windows.System.winmd',
        'Microsoft.WindowsAppRuntime.Bootstrap.dll',
        'Microsoft.WindowsAppRuntime.dll',
        'Microsoft.WindowsAppRuntime.pri',
        'MRM.dll',
        'PushNotificationsLongRunningTask.ProxyStub.dll',
        'RestartAgent.exe',
        'WinUIEdit.dll',
        'WindowsAppSDK-LICENSE.txt',
        'WindowsAppSDK-Runtime-NOTICE.txt',
        'WindowsAppSDK-WinUI-NOTICE.txt',
        'wuceffectsi.dll')) {
    [void] $DeskLinkWinUiRootFiles.Add($Name)
}

$DeskLinkWinUiBuildFiles = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
foreach ($Name in 'desklink.exp', 'desklink.lib', 'desklink.pdb') {
    [void] $DeskLinkWinUiBuildFiles.Add($Name)
}

function Test-DeskLinkWinUiRuntimePath([string] $RelativePath) {
    if ([string]::IsNullOrWhiteSpace($RelativePath) -or
        [IO.Path]::IsPathRooted($RelativePath) -or
        $RelativePath.Contains('..')) {
        return $false
    }
    if ($DeskLinkWinUiRootFiles.Contains($RelativePath)) {
        return $true
    }
    if ($RelativePath -ieq 'Microsoft.UI.Xaml\Assets\map.html' -or
        $RelativePath -ieq
            'Microsoft.UI.Xaml\Assets\NoiseAsset_256X256_PNG.png') {
        return $true
    }
    return $RelativePath -match
        '^[a-z]{2,3}(?:-[a-z0-9]{2,8}){1,2}\\Microsoft\.(?:ui\.xaml|UI\.Xaml\.Phone)\.dll\.mui$'
}

function Get-DeskLinkWinUiRuntimeFiles([string] $RootPath) {
    $ResolvedRoot = (Resolve-Path -LiteralPath $RootPath).Path
    $Files = [Collections.Generic.List[IO.FileInfo]]::new()
    foreach ($File in Get-ChildItem -LiteralPath $ResolvedRoot -File -Recurse -Force) {
        $RelativePath = [IO.Path]::GetRelativePath($ResolvedRoot, $File.FullName)
        if (Test-DeskLinkWinUiRuntimePath $RelativePath) {
            $Files.Add($File)
            continue
        }
        if ($DeskLinkWinUiBuildFiles.Contains($RelativePath)) {
            continue
        }
        throw "The WinUI output contains an unexpected file: $RelativePath"
    }
    return $Files
}

function Assert-DeskLinkWinUiRuntimePayload([string] $RootPath) {
    $ResolvedRoot = (Resolve-Path -LiteralPath $RootPath).Path
    $Files = @(Get-DeskLinkWinUiRuntimeFiles $ResolvedRoot)
    foreach ($Required in $DeskLinkWinUiRootFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $ResolvedRoot $Required) `
                -PathType Leaf)) {
            throw "The WinUI payload is missing required file: $Required"
        }
    }
    foreach ($LegalFile in $DeskLinkWinUiLegalFileHashes.GetEnumerator()) {
        $Path = Join-Path $ResolvedRoot $LegalFile.Key
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash -ne
            $LegalFile.Value) {
            throw "The WinUI legal file did not match its reviewed hash: $($LegalFile.Key)"
        }
    }
    foreach ($Item in Get-ChildItem -LiteralPath $ResolvedRoot -Recurse -Force) {
        if (($Item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "The WinUI payload must not contain a reparse point: $($Item.FullName)"
        }
    }
    if ($Files.Count -lt $DeskLinkWinUiRootFiles.Count) {
        throw 'The WinUI payload did not contain its required runtime graph.'
    }
    return $Files
}
