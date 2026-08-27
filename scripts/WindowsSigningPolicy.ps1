function Get-DeskLinkNormalizedThumbprint([string] $Thumbprint) {
    return ($Thumbprint -replace '\s', '').ToUpperInvariant()
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
