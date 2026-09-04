[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $PackagePath,

    [switch] $RequireMicrosoftProductionSignature
)

$ErrorActionPreference = 'Stop'
$ExpectedFiles = @(
    'DeskLinkVirtualMicrophone.inf',
    'DeskLinkVirtualMicrophone.sys',
    'DeskLinkVirtualMicrophone.cat',
    'manifest.json'
)
$ExpectedHardwarePublisher =
    'Microsoft Windows Hardware Compatibility Publisher'

function Assert-PlainPath([string] $Path, [bool] $Directory) {
    $Item = Get-Item -LiteralPath $Path -Force
    if (($Item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
        $Item.PSIsContainer -ne $Directory) {
        throw "Virtual microphone package path has the wrong type or is redirected: $Path"
    }
}

function Get-PeMachine([string] $Path) {
    $Stream = [IO.File]::Open(
        $Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::Read)
    try {
        $Reader = [IO.BinaryReader]::new($Stream)
        if ($Reader.ReadUInt16() -ne 0x5A4D) {
            throw "Driver is not a PE image: $Path"
        }
        $Stream.Position = 0x3C
        $PeOffset = $Reader.ReadUInt32()
        if ($PeOffset -gt $Stream.Length - 6) {
            throw "Driver PE header is out of range: $Path"
        }
        $Stream.Position = $PeOffset
        if ($Reader.ReadUInt32() -ne 0x00004550) {
            throw "Driver PE signature is invalid: $Path"
        }
        return $Reader.ReadUInt16()
    } finally {
        $Stream.Dispose()
    }
}

$PackagePath = (Resolve-Path -LiteralPath $PackagePath).Path
Assert-PlainPath $PackagePath $true
$ActualFiles = @(Get-ChildItem -LiteralPath $PackagePath -Force)
if ($ActualFiles.Count -ne $ExpectedFiles.Count) {
    throw 'The driver package must contain exactly INF, SYS, CAT, and manifest.json.'
}
foreach ($File in $ActualFiles) {
    Assert-PlainPath $File.FullName $false
    if ($File.Name -notin $ExpectedFiles) {
        throw "Unexpected virtual microphone package file: $($File.Name)"
    }
}

$InfPath = Join-Path $PackagePath 'DeskLinkVirtualMicrophone.inf'
$SysPath = Join-Path $PackagePath 'DeskLinkVirtualMicrophone.sys'
$CatalogPath = Join-Path $PackagePath 'DeskLinkVirtualMicrophone.cat'
$ManifestPath = Join-Path $PackagePath 'manifest.json'
$InfBytes = [IO.File]::ReadAllBytes($InfPath)
if ($InfBytes.Length -lt 2 -or $InfBytes[0] -ne 0xFF -or
    $InfBytes[1] -ne 0xFE) {
    throw 'The driver INF must remain deterministic UTF-16LE with a BOM.'
}
$Inf = [Text.Encoding]::Unicode.GetString($InfBytes, 2, $InfBytes.Length - 2)
foreach ($Required in @(
        'DriverVer = 09/03/2026,0.1.0.0',
        'CatalogFile = DeskLinkVirtualMicrophone.cat',
        'ROOT\DeskLinkVirtualMicrophone',
        'DeskLinkVirtualMicrophone.sys',
        'DeskLink Microphone Feed',
        'DeskLink Remote Microphone',
        'NTamd64.10.0...20348',
        '{D21F0A7C-80DA-4E7E-A906-81DF3E2EA4B9},2')) {
    if ($Inf.IndexOf($Required, [StringComparison]::Ordinal) -lt 0) {
        throw "The driver INF lost required fixed identity: $Required"
    }
}
if ((Get-PeMachine $SysPath) -ne 0x8664) {
    throw 'The virtual microphone driver must be an x64 PE image.'
}

$Manifest = Get-Content -LiteralPath $ManifestPath -Raw |
    ConvertFrom-Json
if ($Manifest.schema -ne 1 -or $Manifest.architecture -ne 'x64' -or
    $Manifest.format -ne '48000 Hz mono PCM16' -or
    $Manifest.maximum_bridge_ms -ne 60 -or
    $Manifest.target_bridge_ms -ne 40 -or
    $Manifest.upstream_commit -ne
        '197ba2156a60e2b76fcd4820bae594223e91a1e9' -or
    $Manifest.wdk -ne '10.0.26100.6584') {
    throw 'The virtual microphone package manifest does not match the reviewed architecture.'
}
$ManifestFiles = @($Manifest.files)
if ($ManifestFiles.Count -ne 3) {
    throw 'The virtual microphone manifest must hash exactly INF, SYS, and CAT.'
}
foreach ($Name in $ExpectedFiles | Where-Object { $_ -ne 'manifest.json' }) {
    $Entries = @($ManifestFiles | Where-Object { $_.name -ceq $Name })
    if ($Entries.Count -ne 1 -or
        $Entries[0].sha256 -cnotmatch '^[0-9a-f]{64}$') {
        throw "The virtual microphone manifest has no unique SHA-256 for $Name"
    }
    $ActualHash = (Get-FileHash -Algorithm SHA256 `
        -LiteralPath (Join-Path $PackagePath $Name)).Hash.ToLowerInvariant()
    if ($Entries[0].sha256 -cne $ActualHash) {
        throw "The virtual microphone package hash does not match for $Name"
    }
}

$CatalogSignature = Get-AuthenticodeSignature -LiteralPath $CatalogPath
if ($RequireMicrosoftProductionSignature) {
    if ($Manifest.signature_state -ne 'microsoft-production-signed') {
        throw 'Production packaging requires a manifest marked microsoft-production-signed.'
    }
    $Publisher = if ($CatalogSignature.SignerCertificate) {
        $CatalogSignature.SignerCertificate.GetNameInfo(
            [Security.Cryptography.X509Certificates.X509NameType]::SimpleName,
            $false)
    }
    if ($CatalogSignature.Status -ne
            [Management.Automation.SignatureStatus]::Valid -or
        -not [string]::Equals(
            $Publisher, $ExpectedHardwarePublisher,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw 'The catalog lacks the expected valid Microsoft production hardware signature.'
    }
    $SdkBin = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $SignTool = Get-ChildItem -LiteralPath $SdkBin -Directory |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName 'x64\signtool.exe' } |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
    if (-not $SignTool) {
        throw 'A Windows SDK x64 signtool.exe is required for kernel-policy verification.'
    }
    $ToolSignature = Get-AuthenticodeSignature -LiteralPath $SignTool
    if ($ToolSignature.Status -ne
            [Management.Automation.SignatureStatus]::Valid -or
        -not $ToolSignature.SignerCertificate -or
        $ToolSignature.SignerCertificate.GetNameInfo(
            [Security.Cryptography.X509Certificates.X509NameType]::SimpleName,
            $false) -ne 'Microsoft Corporation') {
        throw 'signtool.exe does not have the expected Microsoft signature.'
    }
    foreach ($Member in @($InfPath, $SysPath)) {
        & $SignTool verify /kp /c $CatalogPath $Member
        if ($LASTEXITCODE -ne 0) {
            throw "Kernel-policy catalog verification failed for $Member"
        }
    }
} else {
    if ($Manifest.signature_state -ne 'unsigned-development-output' -or
        $CatalogSignature.Status -ne
            [Management.Automation.SignatureStatus]::NotSigned) {
        throw 'Development output must be explicitly marked and remain unsigned.'
    }
}

Write-Host ('[VirtualMicrophone:Package] validated {0} ({1})' -f `
    $PackagePath, $Manifest.signature_state)
