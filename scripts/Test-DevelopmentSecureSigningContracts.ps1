[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Get-StrictUtf8([string] $Path) {
    $Encoding = [Text.UTF8Encoding]::new($false, $true)
    return $Encoding.GetString([IO.File]::ReadAllBytes($Path))
}

$Generator = Get-StrictUtf8 (Join-Path $RepositoryRoot `
    'scripts\New-DeskLinkDevelopmentCertificate.ps1')
$Trust = Get-StrictUtf8 (Join-Path $RepositoryRoot `
    'scripts\Set-DeskLinkDevelopmentTrust.ps1')
$Policy = Get-StrictUtf8 (Join-Path $RepositoryRoot `
    'scripts\WindowsSigningPolicy.ps1')
$Signer = Get-StrictUtf8 (Join-Path $RepositoryRoot `
    'scripts\Invoke-AuthenticodeSign.ps1')
$Builder = Get-StrictUtf8 (Join-Path $RepositoryRoot `
    'scripts\Build-WindowsInstaller.ps1')
$Installer = Get-StrictUtf8 (Join-Path $RepositoryRoot `
    'installer\DeskLink.iss')
$All = $Generator + "`n" + $Trust + "`n" + $Policy + "`n" +
    $Signer + "`n" + $Builder + "`n" + $Installer

foreach ($Forbidden in @(
        'PFXExportCertStoreEx', 'Export-PfxCertificate',
        'NCryptExportKey', 'CryptExportKey', 'testsigning',
        'nointegritychecks', 'Disable-LocalUser', 'EnableLUA',
        'PromptOnSecureDesktop', 'SecureBoot')) {
    if ($All.IndexOf($Forbidden, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
        throw "Development Secure path references forbidden mechanism: $Forbidden"
    }
}

foreach ($Required in @(
        '-Type CodeSigningCert',
        "-Subject 'CN=DeskLink Development Secure'",
        "-Provider 'Microsoft Software Key Storage Provider'",
        '-KeyAlgorithm RSA', '-KeyLength 3072', '-HashAlgorithm SHA256',
        '-KeyExportPolicy NonExportable', '-KeyUsage DigitalSignature',
        'ConfirmDevelopmentSigningIdentity',
        'Assert-DeskLinkDevelopmentCertificate')) {
    if ($Generator.IndexOf($Required, [StringComparison]::Ordinal) -lt 0) {
        throw "Development certificate generator lost required contract: $Required"
    }
}
if ($Generator -match '(?i)password|pfx|pem|privatekeypath') {
    throw 'Development certificate generator must not accept or emit portable private-key material.'
}

foreach ($Required in @(
        "@('Root', 'TrustedPublisher')",
        'StoreLocation]::LocalMachine',
        'ConfirmDevelopmentTrust',
        'ExpectedCertificateDerSha256',
        'Assert-DeskLinkDevelopmentPublicCertificate',
        'ReparsePoint',
        "ValidateSet('Install', 'Remove', 'Status')")) {
    if ($Trust.IndexOf($Required, [StringComparison]::Ordinal) -lt 0) {
        throw "Development trust tool lost required contract: $Required"
    }
}
if ($Trust -match '(?i)DownloadString|Invoke-WebRequest|Start-BitsTransfer|http://|https://') {
    throw 'Development trust must not download certificates or trust material.'
}

if ($Policy -notmatch
        'ExportPolicy -ne\s+\[Security\.Cryptography\.CngExportPolicies\]::None' -or
    $Policy -notmatch 'RSACng' -or
    $Policy -notmatch 'KeySize -lt 3072' -or
    $Policy -notmatch "SignatureAlgorithm.Value -ne '1.2.840.113549.1.1.11'") {
    throw 'Development signing policy lost its exact CNG, export, strength, or SHA-256 checks.'
}
if ($Builder -notmatch '\[switch\] \$DevelopmentSelfSigned' -or
    $Builder -notmatch 'DevelopmentUnsigned and DevelopmentSelfSigned are mutually exclusive' -or
    $Builder -notmatch 'development-secure' -or
    $Builder -notmatch '/DDevelopmentSecure=1' -or
    $Builder -notmatch '/sha1 \{1\} /s My /fd SHA256' -or
    $Builder -notmatch '/tr \{2\} /td SHA256' -or
    $Builder -notmatch 'Assert-AuthenticodeSignature \$BuiltInstaller \$Certificate\.Thumbprint -RequireTimestamp' -or
    $Signer -notmatch '\[switch\] \$DevelopmentSelfSigned' -or
    $Signer -notmatch 'Get-DeskLinkDevelopmentSigningCertificate') {
    throw 'The packaging path no longer distinguishes self-signed development from unsigned and production modes.'
}
if ($Installer -notmatch '(?m)^#ifdef DevelopmentSecure\r?$' -or
    $Installer -notmatch 'AppName=DeskLink Development Secure' -or
    $Installer -notmatch 'DefaultDirName=\{autopf\}\\DeskLink Development Secure' -or
    $Installer -notmatch '(?m)^PrivilegesRequired=admin\r?$') {
    throw 'The Development Secure installer must remain labeled and machine-wide under Program Files.'
}

Write-Host '[Packaging:DevelopmentSigning] source contracts passed.'
