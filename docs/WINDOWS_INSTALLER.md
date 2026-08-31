# DeskLink Windows Installer

DeskLink's installer foundation is a 64-bit, current-user Inno Setup package
for the Windows 11 / Windows Server 2022+ production baseline. It installs to
`%LOCALAPPDATA%\Programs\DeskLink`, creates a current-user Start menu entry,
and registers its uninstaller only under HKCU. It does not request elevation,
install a service, add Firewall rules, or modify the Windows network profile.

PR 9A makes `desklink.exe` the normal Start menu, post-install, sign-in, and
updater-restart entry point. `desklink_alpha.exe` remains for one migration
release as the explicitly labeled **DeskLink diagnostics (Alpha)** fallback.

A separate `-ExperimentalWindows10 -DevelopmentUnsigned` build mode creates a
Development Alpha installer for Windows 10 22H2 build 19045. It includes both
provider graphs and the Windows 10-targeted product shell. It cannot be signed
through the production packaging path, does not alter Firewall policy, and is
not a supported or production artifact.

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
- The packaged update coordinator performs `Return Local -> confirm no remote
  focus/capture -> stop runtime/UI -> update/validate -> optional restart` and
  invokes a prevalidated current-version installer on candidate failure. Setup
  and Uninstall cannot overlap its update gate. See
  [`WINDOWS_UPDATES.md`](WINDOWS_UPDATES.md).

## Build modes

CI creates an explicitly named `*-unsigned.exe` development installer. It is
for automated install/repair/upgrade/uninstall validation only and must never
be published as a production release.

The Windows 10 Development Alpha is the only unsigned package intended for a
GitHub prerelease. Its name, release title, Setup information page, and bundled
notice must all say Development Alpha/unsigned. Packaging requires the exact
reviewed OpenSSL MsQuic, libcrypto, and libssl hashes in addition to Schannel.
The broker chooses by OS before loading and never falls back.

A production build has no unsigned fallback. It requires an Authenticode code
signing certificate in the current user's Windows certificate store, selected
by thumbprint, plus an RFC 3161 timestamp URL. The build signs every DeskLink
executable, the generated uninstaller, and Setup; it then verifies the signer
and timestamp before copying the artifact to its destination. The release
signing key is separate from DeskLink's device CNG identity. The build accepts
no PFX, PEM, private-key path, or exported DeskLink key.

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
  -StagePath windows10-alpha-stage
.\scripts\Build-WindowsInstaller.ps1 `
  -StagePath windows10-alpha-stage `
  -OutputPath DeskLink-0.1.0-alpha.1-windows-x64-unsigned.exe `
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
3. launching only the installed product shell starts the fixed sibling broker,
   secondary activation and bounded exit work, and shell exit leaves the broker
   responsive;
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
