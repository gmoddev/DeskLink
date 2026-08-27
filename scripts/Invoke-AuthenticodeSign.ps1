[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $SignToolPath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9A-Fa-f]{40}$')]
    [string] $CertificateThumbprint,

    [Parameter(Mandatory = $true)]
    [uri] $TimestampUrl,

    [Parameter(Mandatory = $true)]
    [string] $Path,

    [ValidateRange(5, 120)]
    [int] $TimeoutSeconds = 45
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'WindowsSigningPolicy.ps1')
$CertificateThumbprint =
    Get-DeskLinkNormalizedThumbprint $CertificateThumbprint
[void] (Get-DeskLinkCodeSigningCertificate $CertificateThumbprint)
[void] (Assert-DeskLinkTimestampUrl $TimestampUrl)
$SignToolPath = (Resolve-Path -LiteralPath $SignToolPath).Path
$Path = (Resolve-Path -LiteralPath $Path).Path
$OutputPath = Join-Path ([IO.Path]::GetTempPath()) `
    ('DeskLinkSign-' + [guid]::NewGuid().ToString('N') + '.out')
$ErrorPath = $OutputPath + '.err'
try {
    $Arguments = @(
        'sign',
        '/sha1', $CertificateThumbprint,
        '/s', 'My',
        '/fd', 'SHA256',
        '/tr', $TimestampUrl.AbsoluteUri,
        '/td', 'SHA256',
        '/d', 'DeskLink',
        ('"' + $Path + '"')
    )
    $Process = Start-Process -FilePath $SignToolPath -ArgumentList $Arguments `
        -RedirectStandardOutput $OutputPath -RedirectStandardError $ErrorPath `
        -PassThru
    if (-not $Process.WaitForExit($TimeoutSeconds * 1000)) {
        $Process.Kill()
        $Process.WaitForExit()
        throw "SignTool exceeded the $TimeoutSeconds-second fail-closed deadline."
    }
    $StandardOutput = Get-Content -Raw -LiteralPath $OutputPath
    $StandardError = Get-Content -Raw -LiteralPath $ErrorPath
    if ($Process.ExitCode -ne 0) {
        throw "SignTool failed with exit code $($Process.ExitCode). $StandardOutput $StandardError"
    }
    $Signature = Get-AuthenticodeSignature -LiteralPath $Path
    if ($Signature.Status -ne [Management.Automation.SignatureStatus]::Valid -or
        -not $Signature.SignerCertificate -or
        (Get-DeskLinkNormalizedThumbprint `
            $Signature.SignerCertificate.Thumbprint) -ne
        $CertificateThumbprint -or
        -not $Signature.TimeStamperCertificate) {
        throw 'The signed artifact did not verify with the expected signer and timestamp.'
    }
    Write-Host "[Packaging:Signing] signed and verified $Path"
} finally {
    foreach ($TemporaryPath in $OutputPath, $ErrorPath) {
        if (Test-Path -LiteralPath $TemporaryPath) {
            Remove-Item -LiteralPath $TemporaryPath -Force
        }
    }
}
