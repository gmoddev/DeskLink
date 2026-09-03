[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $StagePath,

    [Parameter(Mandatory = $true)]
    [string] $OutputPath,

    [Parameter(Mandatory = $true)]
    [string] $IsccPath,

    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string] $AppVersion = '0.1.0',

    [switch] $DevelopmentUnsigned,

    [switch] $ExperimentalWindows10,

    [ValidatePattern('^[0-9A-Fa-f ]{40,59}$')]
    [string] $CertificateThumbprint = '',

    [uri] $TimestampUrl
)

$ErrorActionPreference = 'Stop'
$ExpectedInnoVersion = '7.1.0'
$ExpectedInnoPublisher = 'Pyrsys B.V.'
$ExpectedMsQuicHash =
    'C981E61CD207F42D46B54EF7DBF1049F1F836424C3BA981F4469AC2B2BEA9610'
$ExpectedOpenSslMsQuicHash =
    '392743C217AFB9F7E6A34F7C62E5FADB89D1908334A43D10EB74F6AB8040F238'
$ExpectedOpenSslCryptoHash =
    '2E9FE207B6FC33B8A9CFBB261FCDD03253DF9666E2D73D83D57E2A5D4F97E48D'
$ExpectedOpenSslSslHash =
    '2B10D4D3641A07A85EDBDC446E8F5830EE531D5B91026DAEC4D80860EA868C4C'
$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$InstallerScript = Join-Path $RepositoryRoot 'installer\DeskLink.iss'
$SigningScript = Join-Path $RepositoryRoot 'scripts\Invoke-AuthenticodeSign.ps1'
. (Join-Path $RepositoryRoot 'scripts\WinUiPayload.ps1')
. (Join-Path $RepositoryRoot 'scripts\WindowsSigningPolicy.ps1')

function Assert-LastExitCode([string] $Operation) {
    if ($LASTEXITCODE -ne 0) {
        throw "$Operation failed with exit code $LASTEXITCODE"
    }
}

function Assert-NoReparsePoint([string] $Path, [string] $Description) {
    $Item = Get-Item -LiteralPath $Path -Force
    if (($Item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$Description must not be a reparse point: $Path"
    }
}

function Assert-AuthenticodeSignature(
    [string] $Path,
    [string] $ExpectedThumbprint,
    [switch] $RequireTimestamp) {
    $Signature = Get-AuthenticodeSignature -LiteralPath $Path
    if ($Signature.Status -ne [Management.Automation.SignatureStatus]::Valid -or
        -not $Signature.SignerCertificate) {
        throw "Authenticode validation failed for ${Path}: $($Signature.StatusMessage)"
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedThumbprint) -and
        (Get-DeskLinkNormalizedThumbprint $Signature.SignerCertificate.Thumbprint) -ne
        (Get-DeskLinkNormalizedThumbprint $ExpectedThumbprint)) {
        throw "Unexpected Authenticode signer for $Path"
    }
    if ($RequireTimestamp -and -not $Signature.TimeStamperCertificate) {
        throw "The production signature is missing an RFC 3161 timestamp: $Path"
    }
}

if ($DevelopmentUnsigned) {
    if (-not [string]::IsNullOrWhiteSpace($CertificateThumbprint) -or $TimestampUrl) {
        throw 'DevelopmentUnsigned cannot be combined with release signing parameters.'
    }
} elseif ([string]::IsNullOrWhiteSpace($CertificateThumbprint) -or
          -not $TimestampUrl) {
    throw 'Production installer builds require a current-user certificate thumbprint and timestamp URL.'
} else {
    [void] (Assert-DeskLinkTimestampUrl $TimestampUrl)
}
if ($ExperimentalWindows10 -and -not $DevelopmentUnsigned) {
    throw 'ExperimentalWindows10 packages must remain explicitly unsigned development builds.'
}

$StagePath = (Resolve-Path -LiteralPath $StagePath).Path
$IsccPath = (Resolve-Path -LiteralPath $IsccPath).Path
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
if ([IO.Path]::GetExtension($OutputPath) -ine '.exe') {
    throw 'OutputPath must end in .exe.'
}
$OutputName = [IO.Path]::GetFileNameWithoutExtension($OutputPath)
if ($DevelopmentUnsigned -and $OutputName -notmatch '(?i)unsigned') {
    throw 'An unsigned development installer must include "unsigned" in its file name.'
}
if (-not $DevelopmentUnsigned -and $OutputName -match '(?i)unsigned') {
    throw 'A signed production installer must not be named as unsigned.'
}

Assert-NoReparsePoint $StagePath 'StagePath'
Assert-NoReparsePoint $IsccPath 'ISCC.exe'
if ([IO.Path]::GetFileName($IsccPath) -ine 'ISCC.exe') {
    throw 'IsccPath must identify ISCC.exe.'
}
$IsccSignature = Get-AuthenticodeSignature -LiteralPath $IsccPath
if ($IsccSignature.Status -ne [Management.Automation.SignatureStatus]::Valid -or
    -not $IsccSignature.SignerCertificate -or
    $IsccSignature.SignerCertificate.GetNameInfo(
        [Security.Cryptography.X509Certificates.X509NameType]::SimpleName,
        $false) -ne $ExpectedInnoPublisher) {
    throw 'ISCC.exe must have a valid Pyrsys B.V. Authenticode signature.'
}
$IsccVersion = (& $IsccPath --version | Out-String).Trim()
Assert-LastExitCode 'Reading the Inno Setup compiler version'
if ($IsccVersion -ne $ExpectedInnoVersion) {
    throw "ISCC.exe must be Inno Setup $ExpectedInnoVersion; found $IsccVersion"
}

$RequiredFiles = @(
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
    'LICENSE',
    'ALPHA_WRAPPER.md'
)
if ($ExperimentalWindows10) {
    if ($OutputName -notmatch '(?i)beta') {
        throw 'An experimental Windows 10 beta installer must include "beta" in its file name.'
    }
    $RequiredFiles += @(
        'runtime\openssl\msquic.dll',
        'runtime\openssl\libcrypto-3-x64.dll',
        'runtime\openssl\libssl-3-x64.dll',
        'WINDOWS10_BETA_NOTICE.md'
    )
}
$ExpectedRelativeFiles = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
foreach ($RelativePath in $RequiredFiles) {
    [void] $ExpectedRelativeFiles.Add($RelativePath)
    $FullPath = Join-Path $StagePath $RelativePath
    if (-not (Test-Path -LiteralPath $FullPath -PathType Leaf)) {
        throw "The staging directory is missing required file: $RelativePath"
    }
    Assert-NoReparsePoint $FullPath "Staged file $RelativePath"
}
$WinUiRoot = Join-Path $StagePath 'ui'
$WinUiFiles = @(Assert-DeskLinkWinUiRuntimePayload $WinUiRoot)
foreach ($File in $WinUiFiles) {
    $WinUiRelativePath = [IO.Path]::GetRelativePath($WinUiRoot, $File.FullName)
    [void] $ExpectedRelativeFiles.Add((Join-Path 'ui' $WinUiRelativePath))
    if ($File.Extension -in '.dll', '.exe' -and
        $File.Name -ine 'desklink.exe') {
        Assert-AuthenticodeSignature $File.FullName ''
    }
}
foreach ($File in Get-ChildItem -LiteralPath $StagePath -File -Recurse -Force) {
    $RelativePath = [IO.Path]::GetRelativePath($StagePath, $File.FullName)
    if (-not $ExpectedRelativeFiles.Contains($RelativePath)) {
        throw "The staging directory contains an unexpected file: $RelativePath"
    }
}
$MsQuicPath = Join-Path $StagePath 'runtime\schannel\msquic.dll'
if ((Get-FileHash -Algorithm SHA256 -LiteralPath $MsQuicPath).Hash -ne
    $ExpectedMsQuicHash) {
    throw 'The staged Schannel MsQuic runtime does not match the reviewed 2.6.0 artifact.'
}
if ($ExperimentalWindows10) {
    $OpenSslHashes = [ordered]@{
        'runtime\openssl\msquic.dll' = $ExpectedOpenSslMsQuicHash
        'runtime\openssl\libcrypto-3-x64.dll' = $ExpectedOpenSslCryptoHash
        'runtime\openssl\libssl-3-x64.dll' = $ExpectedOpenSslSslHash
    }
    foreach ($Runtime in $OpenSslHashes.GetEnumerator()) {
        $RuntimePath = Join-Path $StagePath $Runtime.Key
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath $RuntimePath).Hash -ne
            $Runtime.Value) {
            throw "The staged Windows 10 runtime did not match the reviewed artifact: $($Runtime.Key)"
        }
    }
}
$UpdaterBytes = [IO.File]::ReadAllBytes(
    (Join-Path $StagePath 'desklink_update.exe'))
$UpdaterUtf16 = [Text.Encoding]::Unicode.GetString($UpdaterBytes)
$UpdaterAscii = [Text.Encoding]::ASCII.GetString($UpdaterBytes)
if ($UpdaterUtf16.Contains('development-allow-unsigned') -or
    $UpdaterAscii.Contains('development-allow-unsigned')) {
    throw 'The staged production updater contains a validation-only unsigned bypass.'
}

$Certificate = $null
if (-not $DevelopmentUnsigned) {
    $Certificate = Get-DeskLinkCodeSigningCertificate $CertificateThumbprint
}

$TemporaryRoot = Join-Path ([IO.Path]::GetTempPath()) `
    ('DeskLinkInstaller-' + [guid]::NewGuid().ToString('N'))
$TemporaryStage = Join-Path $TemporaryRoot 'stage'
$TemporaryOutput = Join-Path $TemporaryRoot 'output'
try {
    New-Item -ItemType Directory -Path $TemporaryStage, $TemporaryOutput -Force | Out-Null
    foreach ($RelativePath in ($ExpectedRelativeFiles | Sort-Object)) {
        $Destination = Join-Path $TemporaryStage $RelativePath
        $DestinationDirectory = Split-Path -Parent $Destination
        New-Item -ItemType Directory -Path $DestinationDirectory -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $StagePath $RelativePath) `
            -Destination $Destination
    }

    $VersionInfoVersion = "$AppVersion.0"
    $Arguments = @(
        '/Qp',
        "/DStagePath=$TemporaryStage",
        "/DAppVersion=$AppVersion",
        "/DVersionInfoVersion=$VersionInfoVersion",
        "/DOutputPath=$TemporaryOutput",
        "/DOutputName=$OutputName"
    )
    if (-not $DevelopmentUnsigned) {
        $NormalizedThumbprint = Get-DeskLinkNormalizedThumbprint $CertificateThumbprint
        $SignToolPath = Join-Path (Split-Path -Parent $IsccPath) 'signtool.exe'
        if (-not (Test-Path -LiteralPath $SignToolPath -PathType Leaf)) {
            $WindowsSdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
            $SignToolPath = Get-ChildItem -LiteralPath $WindowsSdkRoot -Directory |
                Sort-Object Name -Descending |
                ForEach-Object { Join-Path $_.FullName 'x64\signtool.exe' } |
                Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
                Select-Object -First 1
        }
        if (-not $SignToolPath) {
            throw 'A Windows SDK x64 signtool.exe was not found.'
        }
        Assert-AuthenticodeSignature $SignToolPath ''
        $WindowsPowerShell = Join-Path $env:SystemRoot `
            'System32\WindowsPowerShell\v1.0\powershell.exe'
        Assert-AuthenticodeSignature $WindowsPowerShell ''
        $Timestamp = $TimestampUrl.AbsoluteUri
        if ($Timestamp -match '["\s]') {
            throw 'TimestampUrl cannot contain quotes or whitespace.'
        }
        $SignCommand = ('$q{0}$q -NoProfile -NonInteractive ' +
            '-ExecutionPolicy Bypass -File $q{1}$q ' +
            '-SignToolPath $q{2}$q -CertificateThumbprint {3} ' +
            '-TimestampUrl {4} -Path $f') -f $WindowsPowerShell,
            $SigningScript, $SignToolPath, $NormalizedThumbprint, $Timestamp
        $Arguments += "/SDeskLinkReleaseSign=$SignCommand"
        $Arguments += '/DSignedBuild=1'
    }
    if ($ExperimentalWindows10) {
        $Arguments += '/DExperimentalWindows10=1'
    }
    $Arguments += $InstallerScript
    & $IsccPath @Arguments
    Assert-LastExitCode 'Compiling the DeskLink installer'

    $BuiltInstaller = Join-Path $TemporaryOutput "$OutputName.exe"
    if (-not (Test-Path -LiteralPath $BuiltInstaller -PathType Leaf)) {
        throw 'Inno Setup did not produce the expected installer.'
    }
    if ($DevelopmentUnsigned) {
        $Signature = Get-AuthenticodeSignature -LiteralPath $BuiltInstaller
        if ($Signature.Status -ne [Management.Automation.SignatureStatus]::NotSigned) {
            throw 'The explicit unsigned development installer unexpectedly has a signature.'
        }
    } else {
        Assert-AuthenticodeSignature $BuiltInstaller $Certificate.Thumbprint -RequireTimestamp
        foreach ($Executable in 'ui\desklink.exe', 'desklink_runtime.exe',
                'desklink_pair.exe', 'desklink_update.exe',
                'desklink_alpha.exe') {
            Assert-AuthenticodeSignature `
                (Join-Path $TemporaryStage $Executable) $Certificate.Thumbprint -RequireTimestamp
        }
    }

    $OutputDirectory = Split-Path -Parent $OutputPath
    New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
    Copy-Item -LiteralPath $BuiltInstaller -Destination $OutputPath -Force
    $Hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $OutputPath).Hash
    Write-Host "[Packaging:Installer] created $OutputPath"
    Write-Host "[Packaging:Installer] SHA256=$Hash"
} finally {
    if (Test-Path -LiteralPath $TemporaryRoot) {
        Remove-Item -LiteralPath $TemporaryRoot -Recurse -Force
    }
}
