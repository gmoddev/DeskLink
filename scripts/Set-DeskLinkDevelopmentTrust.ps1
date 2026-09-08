[CmdletBinding(SupportsShouldProcess, ConfirmImpact = 'High')]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Install', 'Remove', 'Status')]
    [string] $Action,

    [Parameter(Mandatory = $true)]
    [string] $CertificatePath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9A-Fa-f]{64}$')]
    [string] $ExpectedCertificateDerSha256,

    [switch] $ConfirmDevelopmentTrust
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'WindowsSigningPolicy.ps1')

$CertificatePath = (Resolve-Path -LiteralPath $CertificatePath).Path
$CertificateItem = Get-Item -LiteralPath $CertificatePath -Force
if ($CertificateItem.PSIsContainer -or
    ($CertificateItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
    $CertificateItem.Extension -ine '.cer') {
    throw 'CertificatePath must identify a regular DER .cer file.'
}
$Certificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new(
    $CertificatePath)
try {
    [void] (Assert-DeskLinkDevelopmentPublicCertificate $Certificate)
    $ActualDerSha256 = Get-DeskLinkCertificateDerSha256 $Certificate
    if ($ActualDerSha256 -ne $ExpectedCertificateDerSha256.ToUpperInvariant()) {
        throw 'The development certificate DER SHA-256 does not match the explicitly approved fingerprint.'
    }
    $Thumbprint = Get-DeskLinkNormalizedThumbprint $Certificate.Thumbprint
    $StoreNames = @('Root', 'TrustedPublisher')

    if ($Action -eq 'Status') {
        foreach ($StoreName in $StoreNames) {
            $Store = [Security.Cryptography.X509Certificates.X509Store]::new(
                $StoreName,
                [Security.Cryptography.X509Certificates.StoreLocation]::LocalMachine)
            try {
                $Store.Open([Security.Cryptography.X509Certificates.OpenFlags]::ReadOnly)
                $Present = $Store.Certificates.Find(
                    [Security.Cryptography.X509Certificates.X509FindType]::FindByThumbprint,
                    $Thumbprint, $false).Count -eq 1
                [pscustomobject]@{
                    Store = "LocalMachine\\$StoreName"
                    Thumbprint = $Thumbprint
                    Present = $Present
                }
            } finally {
                $Store.Close()
            }
        }
        return
    }

    if (-not $ConfirmDevelopmentTrust) {
        throw 'Specify ConfirmDevelopmentTrust after independently verifying the certificate DER SHA-256 fingerprint.'
    }

    $AddedStores = [Collections.Generic.List[string]]::new()
    try {
        foreach ($StoreName in $StoreNames) {
            $Store = [Security.Cryptography.X509Certificates.X509Store]::new(
                $StoreName,
                [Security.Cryptography.X509Certificates.StoreLocation]::LocalMachine)
            try {
                $Store.Open(
                    [Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
                $Existing = $Store.Certificates.Find(
                    [Security.Cryptography.X509Certificates.X509FindType]::FindByThumbprint,
                    $Thumbprint, $false)
                if ($Action -eq 'Install') {
                    if ($Existing.Count -eq 0 -and $PSCmdlet.ShouldProcess(
                            "LocalMachine\\$StoreName",
                            "Trust DeskLink Development Secure $Thumbprint")) {
                        $Store.Add($Certificate)
                        $AddedStores.Add($StoreName)
                    }
                } elseif ($Existing.Count -eq 1 -and $PSCmdlet.ShouldProcess(
                        "LocalMachine\\$StoreName",
                        "Remove DeskLink Development Secure $Thumbprint")) {
                    $Store.Remove($Existing[0])
                }
            } finally {
                $Store.Close()
            }
        }
    } catch {
        if ($Action -eq 'Install') {
            foreach ($StoreName in $AddedStores) {
                $Store = [Security.Cryptography.X509Certificates.X509Store]::new(
                    $StoreName,
                    [Security.Cryptography.X509Certificates.StoreLocation]::LocalMachine)
                try {
                    $Store.Open(
                        [Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
                    foreach ($Match in $Store.Certificates.Find(
                            [Security.Cryptography.X509Certificates.X509FindType]::FindByThumbprint,
                            $Thumbprint, $false)) {
                        $Store.Remove($Match)
                    }
                } finally {
                    $Store.Close()
                }
            }
        }
        throw
    }

    Write-Host "[Packaging:DevelopmentTrust] action=$Action thumbprint=$Thumbprint"
    Write-Host "[Packaging:DevelopmentTrust] certificate_der_sha256=$ActualDerSha256"
} finally {
    $Certificate.Dispose()
}
