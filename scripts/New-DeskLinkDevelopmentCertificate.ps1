[CmdletBinding(SupportsShouldProcess, ConfirmImpact = 'High')]
param(
    [Parameter(Mandatory = $true)]
    [string] $OutputDirectory,

    [ValidateRange(1, 5)]
    [int] $ValidityYears = 3,

    [switch] $ConfirmDevelopmentSigningIdentity
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'WindowsSigningPolicy.ps1')

if (-not $ConfirmDevelopmentSigningIdentity) {
    throw 'Specify ConfirmDevelopmentSigningIdentity to create the private development signing identity.'
}

$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$Parent = Split-Path -Parent $OutputDirectory
if (-not (Test-Path -LiteralPath $Parent -PathType Container)) {
    throw 'The output directory parent must already exist.'
}
if (Test-Path -LiteralPath $OutputDirectory) {
    $OutputItem = Get-Item -LiteralPath $OutputDirectory -Force
    if (-not $OutputItem.PSIsContainer -or
        ($OutputItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'OutputDirectory must be a regular directory, not a reparse point.'
    }
}

$Existing = @(Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
    Where-Object {
        $_.Subject -eq 'CN=DeskLink Development Secure' -and
        $_.NotAfter -gt (Get-Date)
    })
if ($Existing.Count -ne 0) {
    throw 'A valid DeskLink Development Secure signing identity already exists. Reuse its thumbprint instead of creating another trust root.'
}

if (-not $PSCmdlet.ShouldProcess(
        'Cert:\CurrentUser\My',
        'Create a non-exportable DeskLink development code-signing identity')) {
    return
}

$Certificate = New-SelfSignedCertificate `
    -Type CodeSigningCert `
    -Subject 'CN=DeskLink Development Secure' `
    -FriendlyName 'DeskLink Development Secure signing identity' `
    -CertStoreLocation 'Cert:\CurrentUser\My' `
    -Provider 'Microsoft Software Key Storage Provider' `
    -KeyAlgorithm RSA `
    -KeyLength 3072 `
    -HashAlgorithm SHA256 `
    -KeyExportPolicy NonExportable `
    -KeyUsage DigitalSignature `
    -NotAfter (Get-Date).AddYears($ValidityYears)

try {
    [void] (Assert-DeskLinkDevelopmentCertificate $Certificate)
    [void] (New-Item -ItemType Directory -Path $OutputDirectory -Force)
    $PublicCertificatePath = Join-Path $OutputDirectory `
        'DeskLink-Development-Secure.cer'
    [void] (Export-Certificate -Cert $Certificate `
        -FilePath $PublicCertificatePath -Type CERT -Force)
    $PublicCertificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new(
        $PublicCertificatePath)
    try {
        [void] (Assert-DeskLinkDevelopmentPublicCertificate $PublicCertificate)
        $CertificateDerSha256 =
            Get-DeskLinkCertificateDerSha256 $PublicCertificate
    } finally {
        $PublicCertificate.Dispose()
    }

    $ManifestPath = Join-Path $OutputDirectory `
        'DeskLink-Development-Secure.json'
    [ordered]@{
        Schema = 1
        Channel = 'development-secure'
        Subject = $Certificate.Subject
        ThumbprintSha1 = Get-DeskLinkNormalizedThumbprint $Certificate.Thumbprint
        CertificateDerSha256 = $CertificateDerSha256
        NotBeforeUtc = $Certificate.NotBefore.ToUniversalTime().ToString('O')
        NotAfterUtc = $Certificate.NotAfter.ToUniversalTime().ToString('O')
        PrivateKeyExportable = $false
    } | ConvertTo-Json | Set-Content -LiteralPath $ManifestPath `
        -Encoding UTF8

    Write-Host '[Packaging:DevelopmentSigning] created non-exportable CNG signing identity.'
    Write-Host "[Packaging:DevelopmentSigning] thumbprint=$($Certificate.Thumbprint)"
    Write-Host "[Packaging:DevelopmentSigning] certificate_der_sha256=$CertificateDerSha256"
    Write-Host "[Packaging:DevelopmentSigning] public_certificate=$PublicCertificatePath"
    Write-Host "[Packaging:DevelopmentSigning] manifest=$ManifestPath"
} catch {
    Remove-Item -LiteralPath $Certificate.PSPath -Force -ErrorAction SilentlyContinue
    throw
}
