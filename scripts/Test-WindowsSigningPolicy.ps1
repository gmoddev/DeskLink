[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'WindowsSigningPolicy.ps1')

function New-TestCertificate(
    [string] $EnhancedKeyUsage,
    [DateTimeOffset] $NotBefore,
    [DateTimeOffset] $NotAfter) {
    $Key = [Security.Cryptography.RSA]::Create(2048)
    $Request = [Security.Cryptography.X509Certificates.CertificateRequest]::new(
        'CN=DeskLink CI release-policy fixture', $Key,
        [Security.Cryptography.HashAlgorithmName]::SHA256,
        [Security.Cryptography.RSASignaturePadding]::Pkcs1)
    $Usages = [Security.Cryptography.OidCollection]::new()
    [void] $Usages.Add([Security.Cryptography.Oid]::new($EnhancedKeyUsage))
    $Request.CertificateExtensions.Add(
        [Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new(
            $Usages, $true))
    return [pscustomobject]@{
        Certificate = $Request.CreateSelfSigned($NotBefore, $NotAfter)
        Key = $Key
    }
}

function Assert-Rejected([scriptblock] $Operation, [string] $Description) {
    try {
        & $Operation | Out-Null
    } catch {
        Write-Host "[Packaging:Signing] expected refusal: $Description"
        return
    }
    throw "Signing policy unexpectedly accepted: $Description"
}

$Now = [DateTimeOffset]::Now
$Certificates = @()
try {
    $Valid = New-TestCertificate '1.3.6.1.5.5.7.3.3' `
        $Now.AddDays(-1) $Now.AddDays(30)
    $WrongEku = New-TestCertificate '1.3.6.1.5.5.7.3.1' `
        $Now.AddDays(-1) $Now.AddDays(30)
    $Future = New-TestCertificate '1.3.6.1.5.5.7.3.3' `
        $Now.AddDays(1) $Now.AddDays(30)
    $Expired = New-TestCertificate '1.3.6.1.5.5.7.3.3' `
        $Now.AddDays(-30) $Now.AddDays(-1)
    $Certificates = @($Valid, $WrongEku, $Future, $Expired)

    [void] (Assert-DeskLinkReleaseCertificate $Valid.Certificate $Now)
    $PublicOnly = [Security.Cryptography.X509Certificates.X509Certificate2]::new(
        $Valid.Certificate.RawData)
    try {
        Assert-Rejected {
            Assert-DeskLinkReleaseCertificate $PublicOnly $Now
        } 'certificate without a private key'
    } finally {
        $PublicOnly.Dispose()
    }
    Assert-Rejected {
        Assert-DeskLinkReleaseCertificate $WrongEku.Certificate $Now
    } 'certificate without Code Signing EKU'
    Assert-Rejected {
        Assert-DeskLinkReleaseCertificate $Future.Certificate $Now
    } 'not-yet-valid certificate'
    Assert-Rejected {
        Assert-DeskLinkReleaseCertificate $Expired.Certificate $Now
    } 'expired certificate'
    Assert-Rejected {
        Get-DeskLinkCodeSigningCertificate ('00' * 20)
    } 'certificate absent from CurrentUser/My'

    [void] (Assert-DeskLinkTimestampUrl ([uri] 'https://timestamp.example.test/rfc3161'))
    [void] (Assert-DeskLinkTimestampUrl ([uri] 'http://timestamp.example.test/rfc3161'))
    foreach ($RejectedUrl in @(
            'relative/path',
            'ftp://timestamp.example.test/rfc3161',
            'https://user:secret@timestamp.example.test/rfc3161',
            'https://timestamp.example.test/rfc3161#alternate',
            'https://timestamp.example.test/rfc 3161')) {
        Assert-Rejected {
            Assert-DeskLinkTimestampUrl ([uri] $RejectedUrl)
        } "timestamp URL $RejectedUrl"
    }
    Write-Host '[Packaging:Signing] release certificate and timestamp policy passed.'
} finally {
    foreach ($Fixture in $Certificates) {
        if ($Fixture.Certificate) { $Fixture.Certificate.Dispose() }
        if ($Fixture.Key) { $Fixture.Key.Dispose() }
    }
}
