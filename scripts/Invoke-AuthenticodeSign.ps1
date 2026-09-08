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

    [switch] $DevelopmentSelfSigned,

    [string] $FailureLogPath = '',

    [ValidateRange(5, 120)]
    [int] $TimeoutSeconds = 45
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'WindowsSigningPolicy.ps1')
$CertificateThumbprint =
    Get-DeskLinkNormalizedThumbprint $CertificateThumbprint
if ($DevelopmentSelfSigned) {
    [void] (Get-DeskLinkDevelopmentSigningCertificate $CertificateThumbprint)
} else {
    [void] (Get-DeskLinkCodeSigningCertificate $CertificateThumbprint)
}
[void] (Assert-DeskLinkTimestampUrl $TimestampUrl)
$SignToolPath = (Resolve-Path -LiteralPath $SignToolPath).Path
$Path = (Resolve-Path -LiteralPath $Path).Path
$Process = $null
try {
    $Arguments = @(
        'sign',
        '/sha1', $CertificateThumbprint,
        '/s', 'My',
        '/fd', 'SHA256',
        '/tr', $TimestampUrl.AbsoluteUri,
        '/td', 'SHA256',
        '/d', $(if ($DevelopmentSelfSigned) {
            'DeskLink-Development-Secure'
        } else {
            'DeskLink'
        }),
        $Path
    )
    foreach ($Argument in $Arguments) {
        if ($Argument -match '["\r\n]') {
            throw 'SignTool arguments cannot contain quotes or line breaks.'
        }
    }
    $StartInfo = [Diagnostics.ProcessStartInfo]::new()
    $StartInfo.FileName = $SignToolPath
    $StartInfo.Arguments = ($Arguments | ForEach-Object {
        '"' + $_ + '"'
    }) -join ' '
    $StartInfo.UseShellExecute = $false
    $StartInfo.CreateNoWindow = $true
    $StartInfo.RedirectStandardOutput = $true
    $StartInfo.RedirectStandardError = $true
    $Process = [Diagnostics.Process]::new()
    $Process.StartInfo = $StartInfo
    if (-not $Process.Start()) {
        throw 'SignTool did not start.'
    }
    $StandardOutputTask = $Process.StandardOutput.ReadToEndAsync()
    $StandardErrorTask = $Process.StandardError.ReadToEndAsync()
    if (-not $Process.WaitForExit($TimeoutSeconds * 1000)) {
        $Process.Kill()
        $Process.WaitForExit()
        throw "SignTool exceeded the $TimeoutSeconds-second fail-closed deadline."
    }
    $Process.WaitForExit()
    $ExitCode = $Process.ExitCode
    $StandardOutput = $StandardOutputTask.GetAwaiter().GetResult()
    $StandardError = $StandardErrorTask.GetAwaiter().GetResult()
    if ($ExitCode -ne 0) {
        throw "SignTool failed with exit code $ExitCode. $StandardOutput $StandardError"
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
} catch {
    if (-not [string]::IsNullOrWhiteSpace($FailureLogPath)) {
        $FailureLogPath = [IO.Path]::GetFullPath($FailureLogPath)
        [IO.File]::AppendAllText(
            $FailureLogPath,
            "[Packaging:Signing] $($_.Exception.Message)`r`n")
    }
    throw
} finally {
    if ($Process) {
        $Process.Dispose()
    }
}
