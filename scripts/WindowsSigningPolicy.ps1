function Get-DeskLinkNormalizedThumbprint([string] $Thumbprint) {
    return ($Thumbprint -replace '\s', '').ToUpperInvariant()
}

function Get-DeskLinkCertificateDerSha256(
    [Security.Cryptography.X509Certificates.X509Certificate2] $Certificate) {
    if (-not $Certificate) {
        throw 'The certificate is unavailable.'
    }
    $Sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        $Hash = $Sha256.ComputeHash($Certificate.RawData)
        return -join ($Hash | ForEach-Object { $_.ToString('X2') })
    } finally {
        $Sha256.Dispose()
    }
}

function Assert-DeskLinkReleaseCertificate(
    [Security.Cryptography.X509Certificates.X509Certificate2] $Certificate,
    [DateTimeOffset] $Now = [DateTimeOffset]::Now) {
    if (-not $Certificate) {
        throw 'The release certificate is unavailable.'
    }
    if (-not $Certificate.HasPrivateKey -or
        $Certificate.NotBefore -gt $Now.LocalDateTime -or
        $Certificate.NotAfter -le $Now.LocalDateTime) {
        throw 'The release certificate must have a usable private key and be currently valid.'
    }

    $CodeSigningOid = '1.3.6.1.5.5.7.3.3'
    $HasCodeSigningEku = $false
    foreach ($Extension in $Certificate.Extensions) {
        if ($Extension -is [Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]) {
            foreach ($Usage in $Extension.EnhancedKeyUsages) {
                if ($Usage.Value -eq $CodeSigningOid) {
                    $HasCodeSigningEku = $true
                }
            }
        }
    }
    if (-not $HasCodeSigningEku) {
        throw 'The release certificate must include the Code Signing enhanced key usage.'
    }
    return $Certificate
}

function Assert-DeskLinkDevelopmentCertificate(
    [Security.Cryptography.X509Certificates.X509Certificate2] $Certificate,
    [DateTimeOffset] $Now = [DateTimeOffset]::Now) {
    [void] (Assert-DeskLinkReleaseCertificate $Certificate $Now)
    if ($Certificate.Subject -ne 'CN=DeskLink Development Secure' -or
        $Certificate.Issuer -ne $Certificate.Subject -or
        $Certificate.SignatureAlgorithm.Value -ne '1.2.840.113549.1.1.11') {
        throw 'The development certificate must be the exact self-signed DeskLink Development Secure RSA/SHA-256 identity.'
    }

    $PrivateKey = [Security.Cryptography.X509Certificates.RSACertificateExtensions]::GetRSAPrivateKey(
        $Certificate)
    try {
        if ($PrivateKey -isnot [Security.Cryptography.RSACng] -or
            $PrivateKey.KeySize -lt 3072 -or
            $PrivateKey.Key.ExportPolicy -ne
                [Security.Cryptography.CngExportPolicies]::None) {
            throw 'The development signing key must be a non-exportable RSA-3072-or-stronger CNG key.'
        }
    } finally {
        if ($PrivateKey) { $PrivateKey.Dispose() }
    }
    return $Certificate
}

function Assert-DeskLinkDevelopmentPublicCertificate(
    [Security.Cryptography.X509Certificates.X509Certificate2] $Certificate,
    [DateTimeOffset] $Now = [DateTimeOffset]::Now) {
    if (-not $Certificate -or $Certificate.HasPrivateKey -or
        $Certificate.NotBefore -gt $Now.LocalDateTime -or
        $Certificate.NotAfter -le $Now.LocalDateTime -or
        $Certificate.Subject -ne 'CN=DeskLink Development Secure' -or
        $Certificate.Issuer -ne $Certificate.Subject -or
        $Certificate.SignatureAlgorithm.Value -ne '1.2.840.113549.1.1.11') {
        throw 'The public development certificate is missing, private, expired, or has the wrong identity.'
    }
    $CodeSigningOid = '1.3.6.1.5.5.7.3.3'
    $HasCodeSigningEku = $false
    foreach ($Extension in $Certificate.Extensions) {
        if ($Extension -is
            [Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]) {
            foreach ($Usage in $Extension.EnhancedKeyUsages) {
                if ($Usage.Value -eq $CodeSigningOid) {
                    $HasCodeSigningEku = $true
                }
            }
        }
    }
    $PublicKey = [Security.Cryptography.X509Certificates.RSACertificateExtensions]::GetRSAPublicKey(
        $Certificate)
    try {
        if (-not $HasCodeSigningEku -or -not $PublicKey -or
            $PublicKey.KeySize -lt 3072) {
            throw 'The public development certificate must have Code Signing EKU and an RSA-3072-or-stronger key.'
        }
    } finally {
        if ($PublicKey) { $PublicKey.Dispose() }
    }
    return $Certificate
}

function Get-DeskLinkCodeSigningCertificate([string] $Thumbprint) {
    $Normalized = Get-DeskLinkNormalizedThumbprint $Thumbprint
    if ($Normalized -notmatch '^[0-9A-F]{40}$') {
        throw 'The release certificate thumbprint is malformed.'
    }
    $CertificatePath = "Cert:\CurrentUser\My\$Normalized"
    if (-not (Test-Path -LiteralPath $CertificatePath -PathType Leaf)) {
        throw 'The release certificate was not found in the current-user My store.'
    }
    return Assert-DeskLinkReleaseCertificate `
        (Get-Item -LiteralPath $CertificatePath)
}

function Get-DeskLinkDevelopmentSigningCertificate([string] $Thumbprint) {
    $Normalized = Get-DeskLinkNormalizedThumbprint $Thumbprint
    if ($Normalized -notmatch '^[0-9A-F]{40}$') {
        throw 'The development certificate thumbprint is malformed.'
    }
    $CertificatePath = "Cert:\CurrentUser\My\$Normalized"
    if (-not (Test-Path -LiteralPath $CertificatePath -PathType Leaf)) {
        throw 'The development certificate was not found in the current-user My store.'
    }
    return Assert-DeskLinkDevelopmentCertificate `
        (Get-Item -LiteralPath $CertificatePath)
}

function Assert-DeskLinkTimestampUrl([uri] $TimestampUrl) {
    if (-not $TimestampUrl -or -not $TimestampUrl.IsAbsoluteUri -or
        $TimestampUrl.Scheme -notin 'http', 'https' -or
        [string]::IsNullOrWhiteSpace($TimestampUrl.DnsSafeHost) -or
        -not [string]::IsNullOrEmpty($TimestampUrl.UserInfo) -or
        -not [string]::IsNullOrEmpty($TimestampUrl.Fragment) -or
        $TimestampUrl.OriginalString -match '["\s]') {
        throw 'TimestampUrl must be an absolute HTTP(S) RFC 3161 endpoint without credentials, fragments, quotes, or whitespace.'
    }
    return $TimestampUrl
}
