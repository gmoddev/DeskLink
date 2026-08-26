# DeskLink Windows updates

DeskLink's update coordinator is an explicit, current-user, local-only tool. It
does not discover, download, schedule, or silently approve releases. The user or
a future release UI supplies both the candidate installer and the installer for
the currently installed version, plus the exact SHA-256 of each.

The automated foundation is complete. Production use remains gated on the real
release-signing identity and clean Windows 11 / Windows Server 2022 validation.

## Fail-local sequence

`desklink_update.exe` runs this ordered transaction:

1. copy itself and both installers to a unique transaction directory under
   `%LOCALAPPDATA%\DeskLink\UpdateTransactions`;
2. verify both copied SHA-256 values, require the candidate version to be newer,
   and require the rollback installer to exactly match the installed version;
3. capture the exact current-user DeskLink Run value, then require valid,
   timestamped Authenticode on the running worker, installed product shell,
   candidate, and rollback installer, all from the same leaf signing
   certificate;
4. request `LockPc1`, then read the same-user control state and require both
   remote focus and capture to be false;
5. send the typed `PrepareForUpdate` request, which repeats the fail-local
   transition and schedules orderly runtime shutdown;
6. wait for `Local\DeskLink.Runtime.v1` and
   `Local\DeskLink.RuntimeBroker.v1`, then the Alpha and product-shell UI
   mutexes, to disappear;
7. run Setup in a bounded kill-on-close job and validate the registered version,
   exact installed executables, their signer, WinUI/XAML deployment through the
   product-shell health mode, and existing non-exportable identity plus
   trust/preferences/roaming loading through the broker health mode;
8. if install or validation fails, run and validate the already-approved
   rollback installer and restore the exact captured Run value; and
9. only after candidate or rollback validation may the product shell receive
   the optional background restart, which is successful only after both shell
   and broker are ready.

An authentication, hash, version, local-state, shutdown, installer, health, or
rollback error has no alternate package/provider path. A rollback failure leaves
DeskLink stopped and Local.

Setup and Uninstall reject an active update unless Setup receives the private
coordinator switch while `Local\DeskLink.Update.v1` actually exists. Conversely,
coordinator-mode Setup is rejected without that gate. This closes the gap between
runtime shutdown and Setup acquiring `Local\DeskLink.Install.v1`.

## Applying a reviewed release

Keep the signed installer for the currently installed version. The coordinator
refuses to begin without a viable rollback package.

```powershell
$Candidate = 'C:\Downloads\DeskLink-0.2.0-windows-x64-setup.exe'
$Rollback = 'C:\Downloads\DeskLink-0.1.0-windows-x64-setup.exe'

desklink_update.exe apply `
  --candidate $Candidate `
  --candidate-sha256 (Get-FileHash -Algorithm SHA256 $Candidate).Hash `
  --rollback $Rollback `
  --rollback-sha256 (Get-FileHash -Algorithm SHA256 $Rollback).Hash `
  --restart
```

The starter prints a 32-hex transaction identifier and exits after a staged
worker has inherited the update gate. Query the durable result with:

```powershell
desklink_update.exe status --transaction <32-hex>
```

The result reports `completed`, `rolled-back`, or `failed` plus the exact failure
stage. Installer logs contain paths and installer diagnostics, never input,
clipboard content, trust secrets, or private keys. A later update removes prior
completed transaction directories only when their names, direct-child file set,
and no-reparse-point shape match the coordinator's strict allowlist.

## Automated and deferred validation

Portable tests prove operation ordering, exception containment, rollback after
install/health failure, no restart after rollback failure, and the absence of
package mutation before local confirmation. Windows disposable-account CI proves:

- ordinary Setup and application startup cannot overlap the update gate;
- coordinator-mode Setup cannot be impersonated without that gate;
- the production updater rejects unsigned development packages before disturbing
  running diagnostic Alpha and product-shell UI processes;
- an injected post-install health failure restores the prior version and exact
  startup command;
- product-shell and broker/state health modes run only inside the update gate;
- the complete non-exportable identity snapshot plus DPAPI trust, schema-3
  preferences, and roaming graph remain unchanged; and
- a valid development transaction advances the version, migrates an exact
  legacy Alpha startup command, and then uninstalls normally.

The unsigned acceptance and injected-health controls exist only in the separate
`desklink_update_validation.exe` test target. The installer stages
`desklink_update.exe` and rejects a staged binary containing the unsigned-test
switch.

PR 9A closes the preview transition: post-install and rollback health now
require `desklink.exe`, the broker, and the retained Alpha diagnostics binary.
Compatibility with the immediately preceding migration package is preserved by
restoring its exact startup command after rollback. Alpha-only packages older
than that migration baseline are not accepted as rollback packages by the new
updater.

Production qualification still requires the actual timestamped release signer,
revocation behavior, signed candidate/rollback transactions, power loss and
process termination at each installer phase, disk-full behavior, clean-system
repair/rollback, and physical confirmation that no update can retain or reacquire
remote input.
