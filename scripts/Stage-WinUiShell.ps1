[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $BuildPath,

    [Parameter(Mandatory = $true)]
    [string] $StagePath
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'WinUiPayload.ps1')

$BuildPath = (Resolve-Path -LiteralPath $BuildPath).Path
$StagePath = [IO.Path]::GetFullPath($StagePath)
if (Test-Path -LiteralPath $StagePath) {
    if (-not (Test-Path -LiteralPath $StagePath -PathType Container) -or
        (Get-ChildItem -LiteralPath $StagePath -Force | Select-Object -First 1)) {
        throw 'StagePath must not exist or must identify an empty directory.'
    }
} else {
    New-Item -ItemType Directory -Path $StagePath -Force | Out-Null
}

$RuntimeFiles = @(Get-DeskLinkWinUiRuntimeFiles $BuildPath)
foreach ($File in $RuntimeFiles) {
    $RelativePath = [IO.Path]::GetRelativePath($BuildPath, $File.FullName)
    $Destination = Join-Path $StagePath $RelativePath
    $DestinationDirectory = Split-Path -Parent $Destination
    New-Item -ItemType Directory -Path $DestinationDirectory -Force |
        Out-Null
    Copy-Item -LiteralPath $File.FullName -Destination $Destination
}

$StagedFiles = @(Assert-DeskLinkWinUiRuntimePayload $StagePath)
foreach ($File in $StagedFiles |
        Where-Object { $_.Extension -in '.dll', '.exe' -and
                       $_.Name -ine 'desklink.exe' }) {
    $Signature = Get-AuthenticodeSignature -LiteralPath $File.FullName
    if ($Signature.Status -ne
            [Management.Automation.SignatureStatus]::Valid -or
        -not $Signature.SignerCertificate) {
        throw "A staged Windows App SDK binary has an invalid signature: $($File.FullName)"
    }
}

Write-Host "[ProductUI:Stage] staged $($StagedFiles.Count) locked runtime files at $StagePath"
