[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Path
)

$ErrorActionPreference = 'Stop'
$Path = (Resolve-Path -LiteralPath $Path).Path
$Text = [IO.File]::ReadAllText($Path, [Text.Encoding]::Unicode)

function Replace-Expected {
    param(
        [Parameter(Mandatory = $true)] [string] $Old,
        [Parameter(Mandatory = $true)] [string] $New,
        [int] $Count = 1
    )
    $Found = 0
    $Offset = 0
    while (($Offset = $script:Text.IndexOf(
            $Old, $Offset, [StringComparison]::Ordinal)) -ge 0) {
        ++$Found
        $Offset += $Old.Length
    }
    if ($Found -ne $Count) {
        throw "Expected $Count exact INF occurrence(s), found ${Found}: $Old"
    }
    $script:Text = $script:Text.Replace($Old, $New)
}

Replace-Expected 'DriverVer   = 02/22/2016, 1.0.0.1' `
    'DriverVer   = 09/03/2026, 0.1.0.0'
Replace-Expected 'CatalogFile = SimpleAudioSample.cat' `
    'CatalogFile = DeskLinkVirtualMicrophone.cat'
Replace-Expected '222="SIMPLEAUDIOSAMPLE Driver Disk","",222' `
    '222="DeskLink Virtual Microphone Driver Disk","",222'
Replace-Expected 'simpleaudiosample.sys' 'DeskLinkVirtualMicrophone.sys' 5
Replace-Expected 'NT$ARCH$.10.0...22000' 'NT$ARCH$.10.0...20348' 2
Replace-Expected 'ROOT\SimpleAudioSample' 'ROOT\DeskLinkVirtualMicrophone'

$CapturePropertyAnchor =
    'HKR,,FriendlyName,,%SIMPLEAUDIOSAMPLE.TopologyMicArray1.szPname%' +
    "`r`n`r`n" +
    'HKR,EP\0,%PKEY_AudioEndpoint_Association%,,%KSNODETYPE_ANY%'
Replace-Expected $CapturePropertyAnchor ($CapturePropertyAnchor +
    "`r`nHKR,EP\0,%PKEY_DeskLinkVirtualAudioEndpointKind%,0x00010001,0x2")

$FeedPropertyAnchor =
    'HKR,,FriendlyName,,%SIMPLEAUDIOSAMPLE.TopologySpeaker.szPname%' +
    "`r`n`r`n" +
    'HKR,EP\0,%PKEY_AudioEndpoint_Association%,,%KSNODETYPE_ANY%'
Replace-Expected $FeedPropertyAnchor ($FeedPropertyAnchor +
    "`r`nHKR,EP\0,%PKEY_DeskLinkVirtualAudioEndpointKind%,0x00010001,0x1")

Replace-Expected `
    'AddService=SimpleAudioSample,0x00000002,SimpleAudioSample_Service_Inst' `
    'AddService=DeskLinkVirtualMicrophone,0x00000002,DeskLinkVirtualMicrophone_Service_Inst'
Replace-Expected '[SimpleAudioSample_Service_Inst]' `
    '[DeskLinkVirtualMicrophone_Service_Inst]'
Replace-Expected 'DisplayName=%SimpleAudioSample.SvcDesc%' `
    'DisplayName=%DeskLinkVirtualMicrophone.SvcDesc%'
Replace-Expected `
    'KmdfService = SimpleAudioSample, SIMPLEAUDIOSAMPLE_SA_WdfSect' `
    'KmdfService = DeskLinkVirtualMicrophone, SIMPLEAUDIOSAMPLE_SA_WdfSect'

$PropertyDefinition =
    'PKEY_AudioEndpoint_Supports_EventDriven_Mode = "{1DA5D803-D492-4EDD-8C23-E0C0FFEE7F0E},7"'
Replace-Expected $PropertyDefinition ($PropertyDefinition +
    "`r`nPKEY_DeskLinkVirtualAudioEndpointKind        = `"{D21F0A7C-80DA-4E7E-A906-81DF3E2EA4B9},2`"")

Replace-Expected 'ProviderName = "TODO-Set-Provider"' `
    'ProviderName = "DeskLink"'
Replace-Expected 'MfgName      = "TODO-Set-Manufacturer"' `
    'MfgName      = "DeskLink"'
Replace-Expected 'MsCopyRight  = "TODO-Set-Copyright"' `
    'MsCopyRight  = "Copyright (c) DeskLink contributors"'
Replace-Expected `
    'SIMPLEAUDIOSAMPLE_SA.DeviceDesc="Virtual Audio Device (WDM) - Simple Audio Sample"' `
    'SIMPLEAUDIOSAMPLE_SA.DeviceDesc="DeskLink Virtual Microphone"'
Replace-Expected `
    'SimpleAudioSample.SvcDesc="Virtual Audio Device (WDM) - Simple Audio Sample Driver"' `
    'DeskLinkVirtualMicrophone.SvcDesc="DeskLink Virtual Microphone Driver"'
Replace-Expected `
    'SIMPLEAUDIOSAMPLE.WaveSpeaker.szPname="Simple Audio Sample Wave Speaker"' `
    'SIMPLEAUDIOSAMPLE.WaveSpeaker.szPname="DeskLink Microphone Feed Wave"'
Replace-Expected `
    'SIMPLEAUDIOSAMPLE.TopologySpeaker.szPname="Simple Audio Sample Topology Speaker"' `
    'SIMPLEAUDIOSAMPLE.TopologySpeaker.szPname="DeskLink Microphone Feed"'
Replace-Expected `
    'SIMPLEAUDIOSAMPLE.WaveMicArray1.szPname="Simple Audio Sample Wave Microphone Array - Front"' `
    'SIMPLEAUDIOSAMPLE.WaveMicArray1.szPname="DeskLink Remote Microphone Wave"'
Replace-Expected `
    'SIMPLEAUDIOSAMPLE.TopologyMicArray1.szPname="Simple Audio Sample Topology Microphone Array - Front"' `
    'SIMPLEAUDIOSAMPLE.TopologyMicArray1.szPname="DeskLink Remote Microphone"'
Replace-Expected 'MicArray1CustomName= "Internal Microphone Array - Front"' `
    'MicArray1CustomName= "DeskLink Remote Microphone"'

$UnicodeWithBom = [Text.UnicodeEncoding]::new($false, $true)
[IO.File]::WriteAllText($Path, $Text, $UnicodeWithBom)
