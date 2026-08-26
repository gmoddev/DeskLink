# DeskLink Alpha wrapper

`desklink_alpha.exe` is a native Windows launcher for the existing
`desklink_pair.exe` security and transport runtime. It is an engineering-alpha
surface, not the final DeskLink UI or installer.

## Supported boundary

- Windows 11 or Windows Server 2022 and newer
- packaged, hash-pinned MsQuic 2.6.0 with Schannel
- one wrapper-owned DeskLink operation at a time
- manual pairing/focus plus explicitly enabled reciprocal edge roaming
- optional physical input capture, audio startup, and text-clipboard sync
- presentation-only monitor arrangement and explicit saved edge configuration
- single-instance notification-area lifecycle and optional sign-in startup

The wrapper always passes `--tls-provider schannel`. It does not expose the
experimental Windows 10 OpenSSL/CNG path, does not read or rewrite the trust
store, and does not implement a second connection or pairing stack.

## Safe workflow

1. Start `desklink_alpha.exe` on both PCs.
2. Enter the same UDP port on both PCs. The default is `43821`.
   Enter only a hostname or IP address in **Address**; the wrapper rejects an
   address that already contains a port because the UDP port has its own field.
3. On the receiving PC, select only the capabilities the other PC should have,
   then select **Open pairing window**.
4. On the controlling PC, enter the receiving PC's address, select the
   reciprocal capability grants, and select **Pair with address**.
5. Compare the six-digit code shown by both native confirmation dialogs before
   accepting.
6. After pairing finishes, select **Start receiver** on the receiving PC. To
   let that PC initiate a reverse crossing, enable both physical capture and
   experimental edge roaming before starting it.
7. Select the desired capture/audio options and **Start controller** on the
   controlling PC. Enable both physical capture and experimental edge roaming
   there for forward crossings.
   **Pointer gain** defaults to 100%. **Mouse DPI** defaults to 0, which keeps
   raw device counts. If the physical mouse DPI is known, entering 100..32000
   normalizes it to an 800-DPI reference before gain is applied.
   **Peer audio %** applies 0-100% attenuation to audio rendered from this peer;
   **Mute** toggles that receiver without changing the Windows system volume.
   **Sync text clipboard** is a separate session opt-in and remains ineffective
   unless both pairing directions have the complementary clipboard grants.
8. The controller connects in **Local** mode. Confirm that status reports an
   authenticated connected peer, then select **Focus remote**. When
   **Experimental edge roaming** is checked, this action arms saved Ready links
   while remaining Local; move outward through the configured edge to request
   focus. Physical capture must also be checked.
9. Select **RETURN LOCAL** before stopping a session. The physical emergency
   chord remains **Ctrl+Alt+Pause/Break**.
10. Select **Arrange monitors** to refresh current local/authenticated peer
    layouts, drag the physical-size cards, identify local displays, and confirm
    edge connections. Existing peers require the topology grant in both
    directions before their current cards can appear.

The audio permission names describe what the peer may do:

- **Allow peer audio into this PC** grants `AudioSend` to the peer.
- **Allow this PC audio to peer** grants `AudioReceive` to the peer.
- **Send this PC system audio** is a receiver-session startup option.
- **Render peer system audio** is a controller-session startup option.

**Exchange monitor layouts** grants `DisplayTopologyExchange` to that peer.
It is checked explicitly during pairing and defaults on in the alpha UI, but it
is still a visible user choice. Existing paired peers do not acquire it; re-pair
both directions to enable topology exchange. The grant shares no input or focus
authority and cannot modify the saved roaming graph.

The clipboard permission names also describe what the peer may do:

- **Peer may read text clipboard** grants `ClipboardRead` on this PC.
- **Peer may write text clipboard** grants `ClipboardWrite` on this PC.
- **Sync text clipboard** enables the current session module; it grants nothing.

All three controls default off. Bidirectional synchronization requires both
grants on both PCs plus session opt-in on both PCs. Existing paired peers are
not upgraded automatically. Only bounded Unicode text is supported; clipboard
content is never included in the diagnostic view.

## Process and failure behavior

The wrapper launches `desklink_pair.exe` by absolute path with
`CreateProcessW`; it does not invoke `cmd.exe`, PowerShell, or another shell.
Arguments are built from typed, bounded fields and quoted with Windows argument
rules. Production network operations are pinned to Schannel. Pointer
calibration is passed as bounded typed arguments, scales relative motion inside
DeskLink, and never changes either PC's global mouse settings.

Standard output and standard error are captured into a bounded in-memory
diagnostic view. The wrapper does not persist diagnostics and DeskLink does not
log keyboard, mouse, clipboard, or audio content.

The controlling session starts in `lock-pc1` so merely connecting cannot begin
capture. **Focus remote** and **RETURN LOCAL** use the existing current-user,
same-SID, local-only authenticated control pipe. Returning local changes the
desired mode to `LockPc1`, which disables capture, releases remote focus, and
releases DeskLink-owned input state. If the control endpoint is unavailable,
the wrapper closes the owned process input as a fail-local fallback. Normal
stop and wrapper shutdown use the CLI's graceful stdin shutdown path.

The wrapper is single-instance. By default the X button hides it to the
notification area without stopping an active or reconnecting operation. The
tray offers Open, Return Local, and Exit. Explicit Exit and Windows shutdown
perform ordered fail-local cleanup and remove the tray icon. **Keep DeskLink
running when I close the window** controls close-to-background behavior.
**Start DeskLink when I sign in to Windows** writes only the current user's Run
entry and starts the same executable with `--background`; first launch still
shows onboarding.

The configurator obtains remote cards only through the existing same-user
control pipe after authenticated topology admission. Refresh is asynchronous.
At most eight machine entries and 512 KiB are accepted, and non-Ready peers
cannot supply a snapshot. Saving validates the complete graph, confirms the
runtime is Local, then atomically replaces
`%LOCALAPPDATA%\DeskLink\roaming.settings`. Canvas geometry never enables input.
Identify overlays are click-through and expire after five seconds.

Edge roaming passes the saved settings file only through an absolute typed
argument. Local observation does not suppress input. Suppression remains owned
by the existing Host lifecycle and begins only after the current trusted
session, capability, nonce, topology generation, fresh focus response, landing
queue, initial snapshot, and direction token are all admitted. A 1.5-second
focus stall, settings/topology mutation, capture or send failure, manual
return, emergency chord, or session failure returns Local. One authenticated
peer session owns both directions. Independent local trust records gate each
direction, simultaneous opposite attempts return Local, and reconnect does not
restore focus. Listener-side capture is rejected unless the absolute
roaming-settings path is also supplied.

## Portable package and installer foundation

Configure a Windows MsQuic build and create the ZIP with CPack:

```powershell
cmake -S . -B build-alpha `
  -DDESKLINK_BUILD_MSQUIC=ON `
  -DDESKLINK_MSQUIC_ROOT=C:\path\to\Microsoft.Native.Quic.MsQuic.Schannel
cmake --build build-alpha --config Release --target desklink_alpha
cpack --config build-alpha\CPackConfig.cmake -C Release
```

The ZIP contains:

- `desklink_alpha.exe`
- `desklink_pair.exe`
- `runtime/schannel/msquic.dll`
- the MSVC runtime support DLLs required by these Release binaries
- this alpha guide
- the MIT license

Do not copy a different `msquic.dll` into the package. Runtime version and hash
verification remain fail closed.

The Windows CI job also stages the same allowlisted payload into a current-user
installer. Development artifacts are explicitly named `*-unsigned.exe` and are
not release candidates. A production installer build requires an explicit
current-user code-signing certificate and RFC 3161 timestamp, and refuses to
continue without them. Setup never elevates, installs a service, or changes
Firewall policy. Active Alpha/runtime mutexes block upgrade and uninstall, and
the uninstaller preserves `%LOCALAPPDATA%\DeskLink` identity, trust, and
preferences while removing an enabled current-user startup value. See
[`WINDOWS_INSTALLER.md`](WINDOWS_INSTALLER.md).

The package also includes `desklink_update.exe`. It accepts only explicit local
candidate/current-version rollback installers and their SHA-256 values. Before
Setup, it proves both packages have the installed release signer, returns input
Local through the typed control API, shuts down the runtime and UI, and rolls
back on install or health-check failure. It performs no release discovery or
download. See [`WINDOWS_UPDATES.md`](WINDOWS_UPDATES.md).

## Deliberately deferred

- physical reciprocal edge qualification
- Windows 10 production support
- two-Windows-11 physical failure-matrix signoff
- final visual polish and production-signed clean-system installer/update
  qualification
- persistent diagnostics or telemetry
