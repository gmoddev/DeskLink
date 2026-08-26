# DeskLink Windows Installer

DeskLink's installer foundation is a 64-bit, current-user Inno Setup package
for the Windows 11 / Windows Server 2022+ production baseline. It installs to
`%LOCALAPPDATA%\Programs\DeskLink`, creates a current-user Start menu entry,
and registers its uninstaller only under HKCU. It does not request elevation,
install a service, add Firewall rules, or modify the Windows network profile.

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
- Install/repair/upgrade modifies only installer-owned files. Uninstall also
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
2. installation remains current-user and installs the complete allowlisted
   Alpha and self-contained WinUI payload;
3. product-shell secondary activation and bounded exit work, and shell exit
   leaves the broker responsive;
4. unsigned packages are rejected by the production updater before UI shutdown;
5. injected candidate health failure rolls back to the current version;
6. a coordinated update advances the registered version;
7. uninstall removes binaries, registration, and the startup value; and
8. a sentinel in `%LOCALAPPDATA%\DeskLink` survives unchanged.

`Test-WindowsInstaller.ps1` refuses to run unless
`-AllowCurrentUserMutation` is supplied. Use that switch only on an isolated
test account or disposable Windows worker.

## Remaining release gates

- obtain and protect the production code-signing identity and approve its
  timestamp service;
- validate signed install, repair, upgrade, and uninstall on clean Windows 11
  and Windows Server 2022 systems;
- validate sign-in startup plus Private/Domain Firewall onboarding without
  automatic Firewall changes; and
- validate signed update/rollback plus process termination, power loss,
  disk-full, and restart failure on clean supported systems.
