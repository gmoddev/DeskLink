# DeskLink Windows Installer

DeskLink's installer foundation is a 64-bit, current-user Inno Setup package
for the Windows 11 / Windows Server 2022+ production baseline. It installs to
`%LOCALAPPDATA%\Programs\DeskLink`, creates a current-user Start menu entry,
and registers its uninstaller only under HKCU. It does not request elevation,
install a service, add Firewall rules, or modify the Windows network profile.

In the production/current-user channel, the optional virtual-microphone feature
is a deliberately separate exception:
when and only when an externally Microsoft production-signed driver package is
supplied at release packaging time, Setup may carry that exact package and the
normal UI may explicitly launch a fixed UAC-elevated helper. The main DeskLink
application and every unrelated feature remain usable without it.

PR 9A makes `desklink.exe` the normal Start menu and post-install entry point.
The sign-in command uses `desklink.exe --background` only as a windowless,
short-lived bootstrap for the native broker; updates restart
`desklink_runtime.exe` directly without retaining WinUI. `desklink_alpha.exe`
remains for one migration release as the explicitly labeled **DeskLink
diagnostics (Alpha)** fallback.

A separate `-ExperimentalWindows10 -DevelopmentUnsigned` build mode creates an
unsigned Beta installer for Windows 10 22H2 build 19045. It includes both
provider graphs and the Windows 10-targeted product shell. It cannot be signed
through the production packaging path, does not alter Firewall policy, and is
not a supported or production artifact.

A second explicit mode, `-DevelopmentSelfSigned`, creates **DeskLink
Development Secure** for a small set of administrator-approved test PCs. It is
not the production installer: it uses a dedicated self-signed code-signing
certificate and therefore has no public trust. The exact public certificate
must be verified out of band and installed separately in LocalMachine `Root`
and `TrustedPublisher`; Setup never installs trust. This package installs under
`%ProgramFiles%\DeskLink Development Secure` and installs the separately signed,
networkless `DeskLinkSecureInput` LocalSystem broker plus its fixed helper and
administrator-only configurator. The service starts automatically but has no
input authority by default. Setup never enables the protected per-peer grant;
that requires a separate local **I know what I'm doing** confirmation for one
exact already paired identity and pin.

## Security and lifecycle contract

- The installer accepts only the exact allowlisted Alpha payload plus the
  validated WinUI self-contained graph. Unexpected files, reparse points,
  invalid Microsoft runtime signatures, or a Schannel `msquic.dll` whose
  SHA-256 differs from the reviewed 2.6.0 artifact stop the build.
- The pinned Windows App SDK license plus Runtime, WinUI, and C++/WinRT notices
  are hash-validated, installed with the self-contained payload, and the SDK
  terms are presented by Setup.
- Setup and Uninstall check `Local\DeskLink.Alpha.v1`,
  `Local\DeskLink.Shell.v1`,
  `Local\DeskLink.Runtime.v1`, and `Local\DeskLink.RuntimeBroker.v1`. An
  active UI, transport runtime, or broker blocks replacement and removal;
  Setup never force-closes or restarts DeskLink.
- Install/repair/upgrade modifies only installer-owned files. If the DeskLink
  Run value exactly names the installed Alpha executable, with either no
  argument or `--background`, upgrade migrates it to the product shell. Missing,
  unrelated, and malformed values are not adopted or rewritten. Uninstall also
  removes DeskLink's current-user Run value so an enabled sign-in launch cannot
  point at a deleted executable.
- `%LOCALAPPDATA%\DeskLink`, the current-user CNG key, certificate, trust
  records, application preferences, and roaming preferences are not installer
  payload and are not removed or migrated. The device private key remains
  non-exportable.
- `desklink_virtual_microphone_installer.exe` accepts exactly `install` or
  `uninstall`; it knows only the fixed sibling
  `driver\DeskLinkVirtualMicrophone` package and stable
  `ROOT\DeskLinkVirtualMicrophone` hardware ID. It rejects reparse points,
  extra/missing files, altered INF identity, failed catalog membership, and a
  signer other than Microsoft Windows Hardware Compatibility Publisher. The UI
  cannot provide an INF path.
- Installing or routing the virtual microphone never changes a Windows default
  audio endpoint. UAC denial leaves DeskLink and all other features intact.
  Driver removal is attempted during normal uninstall; denial or driver-removal
  failure is reported without corrupting the rest of application removal.
- Only Development Secure stages the secure-input service, helper, and
  configurator. Packaging signs and timestamps all three with the same exact
  leaf as `desklink_pair.exe`. Setup records a fully quoted fixed image path,
  LocalSystem account, delayed automatic start, restart recovery, and an
  administrator/SYSTEM-only service DACL; it validates the stored path/account
  before copying application files, then starts the broker after the signed
  payload is present. A registration failure aborts before file replacement; a
  startup failure leaves privileged input unavailable. Upgrade stops the broker
  before replacement. Uninstall stops/deletes it and removes its protected
  grant. Ordinary installers never reference the service.
- The packaged update coordinator performs `Return Local -> confirm no remote
  focus/capture -> stop runtime/UI -> update/validate -> optional restart` and
  invokes a prevalidated current-version installer on candidate failure. Setup
  and Uninstall cannot overlap its update gate. See
  [`WINDOWS_UPDATES.md`](WINDOWS_UPDATES.md).

## Build modes

CI creates an explicitly named `*-unsigned.exe` development installer. It is
for automated install/repair/upgrade/uninstall validation only and must never
be published as a production release.

The cross-version DeskLink Beta is the only unsigned package intended for a
GitHub prerelease. Its name, release title, Setup information page, and bundled
notice must all say Beta/unsigned. Packaging requires the exact
reviewed OpenSSL MsQuic, libcrypto, and libssl hashes in addition to Schannel.
The broker chooses by OS before loading and never falls back.

A production build has no unsigned fallback. It requires an Authenticode code
signing certificate in the current user's Windows certificate store, selected
by thumbprint, plus an RFC 3161 timestamp URL. The build signs every DeskLink
executable, the generated uninstaller, and Setup; it then verifies the signer
and timestamp before copying the artifact to its destination. The release
signing key is separate from DeskLink's device CNG identity. The build accepts
no PFX, PEM, private-key path, or exported DeskLink key.

### Development Secure trust channel

Create the development signing identity once, in the real signing user's
Windows profile. The script refuses to replace a still-valid identity and
exports only the public DER certificate plus a fingerprint manifest:

```powershell
.\scripts\New-DeskLinkDevelopmentCertificate.ps1 `
  -OutputDirectory .deploy\development-secure `
  -ConfirmDevelopmentSigningIdentity -Confirm
```

The private signing key is RSA-3072 CNG with export policy `None`; it is never
written to a PFX, PEM, installer, repository, or target PC. Before trusting a
target, compare the manifest's `CertificateDerSha256` through an independent
channel. Then, from an elevated PowerShell session on that target:

```powershell
.\scripts\Set-DeskLinkDevelopmentTrust.ps1 `
  -Action Install `
  -CertificatePath .\DeskLink-Development-Secure.cer `
  -ExpectedCertificateDerSha256 '<independently-approved-SHA-256>' `
  -ConfirmDevelopmentTrust -Confirm
```

Build only from the signing profile; sandbox, service-account, and target-PC
stores do not contain the private key:

```powershell
.\scripts\Build-WindowsInstaller.ps1 `
  -StagePath installer-stage `
  -OutputPath DeskLink-0.1.1-development-secure.exe `
  -IsccPath 'C:\Program Files\Inno Setup 7\ISCC.exe' `
  -AppVersion 0.1.1 `
  -DevelopmentSelfSigned `
  -CertificateThumbprint '<development-certificate-SHA-1>' `
  -TimestampUrl 'https://<approved-rfc3161-service>'
```

Packaging selects the exact thumbprint, revalidates subject, issuer, EKU,
RSA/SHA-256 signature, strength, CNG provider, and zero export policy, then
signs every DeskLink executable—including the secure-input service, helper,
and configurator—the generated uninstaller, and Setup. It
verifies the expected signer and RFC 3161 timestamp before publishing the
artifact. Development Secure never enables test-signing, weakens Secure Boot or
signature enforcement, or falls back to unsigned output. Removing the exact
certificate with the trust script revokes this private channel on a target.

To include the optional driver, first validate a Microsoft-signed package and
pass its directory explicitly:

```powershell
.\scripts\Test-VirtualMicrophonePackage.ps1 `
  -PackagePath '<signed-driver-package>' `
  -RequireMicrosoftProductionSignature
.\scripts\Build-WindowsInstaller.ps1 `
  -StagePath installer-stage `
  -VirtualMicrophonePackagePath '<signed-driver-package>' `
  -OutputPath DeskLink-0.1.0-windows-x64-setup.exe `
  -IsccPath 'C:\Program Files\Inno Setup 7\ISCC.exe' `
  -AppVersion 0.1.0 `
  -CertificateThumbprint '<current-user code-signing certificate SHA-1>' `
  -TimestampUrl 'https://<approved-rfc3161-service>'
```

An unsigned/test-signed package cannot satisfy the production mode. No mode
disables Secure Boot, enables test-signing, or weakens signature enforcement.
Development Secure distributes its public certificate separately and requires
explicit fingerprint verification; Setup never installs that trust root.

```powershell
cmake --install build-msquic --config Release `
  --prefix installer-stage --component Alpha

.\scripts\Build-WinUiShell.ps1 `
  -Configuration Release -LockedMode
.\scripts\Stage-WinUiShell.ps1 `
  -BuildPath build-winui\Release `
  -StagePath installer-stage\ui

.\scripts\Build-WindowsInstaller.ps1 `
  -StagePath installer-stage `
  -OutputPath DeskLink-0.1.0-windows-x64-setup.exe `
  -IsccPath 'C:\Program Files\Inno Setup 7\ISCC.exe' `
  -AppVersion 0.1.0 `
  -CertificateThumbprint '<current-user code-signing certificate SHA-1>' `
  -TimestampUrl 'https://<approved-rfc3161-service>'
```

The experimental package uses the dedicated staging helper and explicit
switches:

```powershell
.\scripts\Build-WinUiShell.ps1 -Configuration Release -LockedMode `
  -ExperimentalWindows10
.\scripts\Stage-Windows10DevelopmentAlpha.ps1 `
  -CMakeBuildPath out\windows10-compat\desklink `
  -WinUiBuildPath out\windows10-compat\desklink\product-ui\Release `
  -StagePath windows10-beta-stage
.\scripts\Build-WindowsInstaller.ps1 `
  -StagePath windows10-beta-stage `
  -OutputPath DeskLink-0.1.0-beta.1-windows-x64-unsigned.exe `
  -IsccPath '<verified Inno Setup 7.1.0>\ISCC.exe' `
  -AppVersion 0.1.0 -DevelopmentUnsigned -ExperimentalWindows10
```

The compiler is pinned to Inno Setup 7.1.0. CI downloads the immutable release,
checks SHA-256
`0362A383ED217D4C4239B5933866DD96D3EB2102737DA92F80F6057A4B40DF2F`,
and requires a valid `Pyrsys B.V.` Authenticode signature before execution.
Review Inno Setup's license requirements before commercial release.

## Automated validation

The Windows MsQuic job builds two development versions, proves that production
packaging without signing authority is rejected, and uses an isolated CI
account to verify:

1. an active DeskLink lifecycle mutex blocks Setup;
2. installation and same-version repair remain current-user, install the complete allowlisted
   runtime, diagnostic Alpha, and self-contained WinUI payload;
3. `desklink.exe --background` starts the fixed sibling broker and exits before
   constructing the WinUI tree; a normal launch supports secondary activation
   and bounded exit, and shell exit leaves the broker and tray responsive;
4. unsigned packages are rejected by the production updater before UI shutdown;
5. injected candidate health failure rolls back to the current version and
   restores the exact prior Alpha startup command;
6. a coordinated update advances the registered version and migrates that
   exact legacy command to `desklink.exe --background`;
7. the complete CNG identity snapshot and hashes of a real DPAPI trust record,
   schema-4 preferences, and saved roaming graph remain unchanged;
8. uninstall removes binaries, registration, and the startup value; and
9. a sentinel in `%LOCALAPPDATA%\DeskLink` survives unchanged.

The same exact installer artifacts and validation executable are downloaded by
a dependent disposable Windows Server 2022 job, which repeats the complete
install/repair/rollback/upgrade/uninstall sequence rather than merely launching
the shell. Separate contract checks reject invalid release certificate states,
unsafe timestamp endpoints, lost PerMonitorV2/accessibility/theme/UTF-8
metadata, elevation, or automatic Firewall changes. These tests do not make an
unsigned development artifact a release package.

The optional-driver CI job separately builds the x64 WaveRT driver from the
pinned Microsoft sample and WDK inputs, runs Inf2Cat, and validates the exact
unsigned development package. Installer validation also proves that this
package is rejected by `-VirtualMicrophonePackagePath`; physical installation
is reserved for a Microsoft production-signed package.

`Test-WindowsInstaller.ps1` refuses to run unless
`-AllowCurrentUserMutation` is supplied. Use that switch only on an isolated
test account or disposable Windows worker.

## Remaining release gates

- obtain and protect the production code-signing identity and approve its
  timestamp service;
- validate signed install, repair, upgrade, and uninstall on clean Windows 11
  and Windows Server 2022 systems (the unsigned disposable Server 2022 path is
  automated, but cannot substitute for the production signer);
- validate sign-in startup plus Private/Domain Firewall onboarding without
  automatic Firewall changes; and
- validate signed update/rollback plus process termination, power loss,
  disk-full, and restart failure on clean supported systems.
- obtain Microsoft production signing/certification for the virtual-microphone
  catalog, then validate install/update/uninstall, zero-physical-microphone
  capture, crash/revoke/disconnect silence, and Discord/OBS capture.
