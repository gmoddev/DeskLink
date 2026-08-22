param(
    [Parameter(Mandatory = $true)]
    [string] $MsQuicSource,

    [Parameter(Mandatory = $true)]
    [string] $OpenSslRoot,

    [Parameter(Mandatory = $true)]
    [string] $SchannelRoot,

    [string] $BuildRoot = ''
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($BuildRoot)) {
    $BuildRoot = Join-Path $PSScriptRoot '..\out\windows10-compat'
}
$ExpectedMsQuicCommit = 'e7e7a114e20a55ec2d5f723cf6bdf3bfb7b0b24a'
$ExpectedOpenSslVersion = '3.5.7'
$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$MsQuicSource = (Resolve-Path $MsQuicSource).Path
$OpenSslRoot = (Resolve-Path $OpenSslRoot).Path
$SchannelRoot = (Resolve-Path $SchannelRoot).Path
$BuildRoot = [System.IO.Path]::GetFullPath($BuildRoot)
$PatchPath = Join-Path $RepositoryRoot `
    'third_party\msquic\patches\0001-Add-explicit-opaque-CNG-OpenSSL-credential-path.patch'

function Assert-LastExitCode([string] $Operation) {
    if ($LASTEXITCODE -ne 0) {
        throw "$Operation failed with exit code $LASTEXITCODE"
    }
}

$ActualMsQuicCommit = & git -C $MsQuicSource rev-parse HEAD
Assert-LastExitCode 'Reading the MsQuic revision'
$ActualMsQuicCommit = ($ActualMsQuicCommit | Out-String).Trim()
if ($ActualMsQuicCommit -ne $ExpectedMsQuicCommit) {
    throw "MsQuic must be exactly $ExpectedMsQuicCommit; found $ActualMsQuicCommit"
}

$OpenSslVersionHeader = Join-Path $OpenSslRoot 'include\openssl\opensslv.h'
$OpenSslVersionText = Get-Content -Raw -LiteralPath $OpenSslVersionHeader
$ExpectedOpenSslPattern = 'OPENSSL_VERSION_TEXT\s+"OpenSSL {0}' -f `
    [regex]::Escape($ExpectedOpenSslVersion)
if ($OpenSslVersionText -notmatch $ExpectedOpenSslPattern) {
    throw "OpenSSL root must contain OpenSSL $ExpectedOpenSslVersion"
}

$PreviousErrorActionPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& git -C $MsQuicSource apply --check $PatchPath 2>$null
$PatchCanApply = $LASTEXITCODE -eq 0
$ErrorActionPreference = $PreviousErrorActionPreference
if ($PatchCanApply) {
    & git -C $MsQuicSource apply $PatchPath
    Assert-LastExitCode 'Applying the DeskLink MsQuic patch'
} else {
    $ErrorActionPreference = 'Continue'
    & git -C $MsQuicSource apply --reverse --check $PatchPath 2>$null
    $PatchIsApplied = $LASTEXITCODE -eq 0
    $ErrorActionPreference = $PreviousErrorActionPreference
    if (-not $PatchIsApplied) {
        throw 'DeskLink MsQuic patch is neither cleanly applicable nor already applied'
    }
    Assert-LastExitCode 'Checking the previously applied DeskLink MsQuic patch'
}

$ForbiddenPrivateKeyApis = @(
    'NCryptExportKey',
    'CryptExportKey',
    'PFXExportCertStoreEx',
    'RSA_METHOD',
    'ENGINE'
)
$ProviderSources = @(
    (Join-Path $MsQuicSource 'src\platform\tls_cng_provider.c'),
    (Join-Path $MsQuicSource 'src\platform\tls_cng_provider.h')
)
$ScannedSources = @($PatchPath) + $ProviderSources
foreach ($ForbiddenApi in $ForbiddenPrivateKeyApis) {
    if (Select-String -LiteralPath $ScannedSources `
            -SimpleMatch $ForbiddenApi -Quiet) {
        throw "Compatibility patch contains prohibited API or mechanism: $ForbiddenApi"
    }
}

$MsQuicBuild = Join-Path $BuildRoot 'msquic'
$DeskLinkBuild = Join-Path $BuildRoot 'desklink'
& cmake -S $MsQuicSource -B $MsQuicBuild -A x64 `
    -DQUIC_TLS_LIB=openssl `
    "-DQUIC_OPENSSL_ROOT_DIR=$OpenSslRoot" `
    -DQUIC_BUILD_SHARED=ON `
    -DQUIC_BUILD_TEST=OFF `
    -DQUIC_BUILD_TOOLS=OFF `
    -DQUIC_BUILD_PERF=OFF `
    -DCMAKE_C_FLAGS=/wd28020
Assert-LastExitCode 'Configuring patched MsQuic'
& cmake --build $MsQuicBuild --config Release --target msquic --parallel 2
Assert-LastExitCode 'Building patched MsQuic'

& cmake -S $RepositoryRoot -B $DeskLinkBuild -A x64 `
    '-UDESKLINK_*_DLL' `
    -UDESKLINK_MSQUIC_INCLUDE_DIR `
    -DDESKLINK_BUILD_MSQUIC=ON `
    "-DDESKLINK_MSQUIC_SCHANNEL_ROOT=$SchannelRoot" `
    "-DDESKLINK_MSQUIC_OPENSSL_ROOT=$MsQuicBuild" `
    "-DDESKLINK_OPENSSL_ROOT=$OpenSslRoot" `
    "-DDESKLINK_MSQUIC_INCLUDE_ROOT=$MsQuicSource"
Assert-LastExitCode 'Configuring DeskLink compatibility tests'
& cmake --build $DeskLinkBuild --config Release --parallel 2
Assert-LastExitCode 'Building DeskLink compatibility tests'
& ctest --test-dir $DeskLinkBuild -C Release --output-on-failure
Assert-LastExitCode 'Running DeskLink compatibility tests'

Write-Host '[Transport:Windows10Compat] Stage 2 build and local security gates passed.'
