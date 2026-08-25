# DeskLink Alpha wrapper

`desklink_alpha.exe` is a native Windows launcher for the existing
`desklink_pair.exe` security and transport runtime. It is an engineering-alpha
surface, not the final DeskLink UI or installer.

## Supported boundary

- Windows 11 or Windows Server 2022 and newer
- packaged, hash-pinned MsQuic 2.6.0 with Schannel
- one wrapper-owned DeskLink operation at a time
- manual pairing and manual focus switching
- optional physical input capture and optional audio startup

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
6. After pairing finishes, select **Start receiver** on the receiving PC.
7. Select the desired capture/audio options and **Start controller** on the
   controlling PC.
   **Pointer gain** defaults to 100%. **Mouse DPI** defaults to 0, which keeps
   raw device counts. If the physical mouse DPI is known, entering 100..32000
   normalizes it to an 800-DPI reference before gain is applied.
   **Peer audio %** applies 0-100% attenuation to audio rendered from this peer;
   **Mute** toggles that receiver without changing the Windows system volume.
8. The controller connects in **Local** mode. Confirm that status reports an
   authenticated connected peer, then select **Focus remote**.
9. Select **RETURN LOCAL** before stopping a session. The physical emergency
   chord remains **Ctrl+Alt+Pause/Break**.

The audio permission names describe what the peer may do:

- **Allow peer audio into this PC** grants `AudioSend` to the peer.
- **Allow this PC audio to peer** grants `AudioReceive` to the peer.
- **Send this PC system audio** is a receiver-session startup option.
- **Render peer system audio** is a controller-session startup option.

## Process and failure behavior

The wrapper launches `desklink_pair.exe` by absolute path with
`CreateProcessW`; it does not invoke `cmd.exe`, PowerShell, or another shell.
Arguments are built from typed, bounded fields and quoted with Windows argument
rules. Production network operations are pinned to Schannel. Pointer
calibration is passed as bounded typed arguments, scales relative motion inside
DeskLink, and never changes either PC's global mouse settings.

Standard output and standard error are captured into a bounded in-memory
diagnostic view. The wrapper does not persist diagnostics and DeskLink does not
log keyboard, mouse, or audio content.

The controlling session starts in `lock-pc1` so merely connecting cannot begin
capture. **Focus remote** and **RETURN LOCAL** use the existing current-user,
same-SID, local-only authenticated control pipe. Returning local changes the
desired mode to `LockPc1`, which disables capture, releases remote focus, and
releases DeskLink-owned input state. If the control endpoint is unavailable,
the wrapper closes the owned process input as a fail-local fallback. Normal
stop and wrapper shutdown use the CLI's graceful stdin shutdown path.

## Portable package

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

## Deliberately deferred

- edge-triggered roaming
- Windows 10 production support
- two-Windows-11 physical failure-matrix signoff
- persisted profiles or automatic startup
- final onboarding, tray UX, installer, signing, and updates
- persistent diagnostics or telemetry
