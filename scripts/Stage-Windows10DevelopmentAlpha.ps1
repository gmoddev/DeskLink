[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $CMakeBuildPath,

    [Parameter(Mandatory = $true)]
    [string] $WinUiBuildPath,

    [Parameter(Mandatory = $true)]
    [string] $StagePath,

    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$CMakeBuildPath = (Resolve-Path -LiteralPath $CMakeBuildPath).Path
$WinUiBuildPath = (Resolve-Path -LiteralPath $WinUiBuildPath).Path
$StagePath = [IO.Path]::GetFullPath($StagePath)

if (Test-Path -LiteralPath $StagePath) {
    if ((Get-ChildItem -LiteralPath $StagePath -Force | Measure-Object).Count -ne 0) {
        throw 'StagePath must be absent or empty.'
    }
} else {
    New-Item -ItemType Directory -Path $StagePath | Out-Null
}

& cmake --install $CMakeBuildPath --config $Configuration `
    --prefix $StagePath --component Alpha
if ($LASTEXITCODE -ne 0) {
    throw "CMake Alpha staging failed with exit code $LASTEXITCODE"
}

$RuntimeCandidates = @(
    (Join-Path $CMakeBuildPath 'runtime\openssl'),
    (Join-Path $CMakeBuildPath "$Configuration\runtime\openssl")
)
$OpenSslRuntime = $RuntimeCandidates |
    Where-Object { Test-Path -LiteralPath $_ -PathType Container } |
    Select-Object -First 1
if (-not $OpenSslRuntime) {
    throw 'The compatibility build did not stage an OpenSSL runtime graph.'
}
$OpenSslStage = Join-Path $StagePath 'runtime\openssl'
New-Item -ItemType Directory -Path $OpenSslStage -Force | Out-Null
foreach ($Name in 'msquic.dll', 'libcrypto-3-x64.dll', 'libssl-3-x64.dll') {
    $Source = Join-Path $OpenSslRuntime $Name
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "The compatibility runtime is missing $Name"
    }
    Copy-Item -LiteralPath $Source -Destination (Join-Path $OpenSslStage $Name)
}

& (Join-Path $RepositoryRoot 'scripts\Stage-WinUiShell.ps1') `
    -BuildPath $WinUiBuildPath -StagePath (Join-Path $StagePath 'ui')
if ($LASTEXITCODE -ne 0) {
    throw "WinUI staging failed with exit code $LASTEXITCODE"
}
Copy-Item -LiteralPath (
    Join-Path $RepositoryRoot 'docs\WINDOWS10_DEVELOPMENT_ALPHA.md') `
    -Destination (Join-Path $StagePath 'WINDOWS10_DEVELOPMENT_ALPHA.md')

Write-Host "[Packaging:Windows10Alpha] staged $StagePath"
