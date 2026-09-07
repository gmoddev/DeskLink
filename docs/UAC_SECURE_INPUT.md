# DeskLink secure-desktop input R&D

## Decision

DeskLink will investigate UAC secure-desktop control with a minimal Windows
service and a per-session SYSTEM helper before considering a kernel input
driver. This is an experimental security boundary, not an extension of the
ordinary current-user runtime and not a production feature.

The normal product remains unchanged:

- `desklink.exe`, `desklink_runtime.exe`, networking, pairing, trust, capture,
  roaming, clipboard, and audio remain current-user processes;
- ordinary input injection remains `SendInput` on `winsta0\\Default` and
  continues to obey UIPI;
- UAC continues to use the Windows secure desktop; DeskLink does not modify
  `EnableLUA`, `PromptOnSecureDesktop`, consent policy, or desktop switching;
- the existing non-exportable CNG identity, DER SHA-256 pin, `PeerValidated`
  gate, pairing records, nonce, epoch, lease, and capability rules do not
  change; and
- no video or secure-desktop capture is claimed. The person at the controlled
  PC must still be able to see and independently assess the UAC prompt.

## Why a service alone is insufficient

Windows services run in Session 0. The interactive UAC prompt is on the active
user session's `WinSta0\\Winlogon` desktop. Therefore the service must not
inject from Session 0 or become the product runtime. Its only eventual role is
to enforce a machine-level authorization boundary and launch a narrow SYSTEM
helper into the exact active session and desktop.

```text
current-user DeskLink runtime (network, pairing, focus)
        |
        | future authenticated, bounded authorization channel
        v
LocalSystem service (no network, no capture, protected policy)
        |
        | fixed signed sibling, SYSTEM token bound to active session
        v
per-session helper on WinSta0\\Default or WinSta0\\Winlogon
        |
        | validated input envelope only
        v
SendInput on the exact active desktop
```

UIAccess is not the foundation for this feature. It is intended for signed
assistive-technology applications installed in protected locations and does
not give a normal process authority over the system-integrity UAC UI. DeskLink
also does not disable the secure desktop to make UIAccess sufficient.

## Implemented validation foundation

`DESKLINK_BUILD_SECURE_INPUT_RND=ON` builds two explicitly experimental
executables. The option defaults off and neither file is installed or packaged
by the product build.

`desklink_secure_service.exe`:

- runs only as the SCM service `DeskLinkSecureInputRnd` under LocalSystem;
- refuses privileged work outside Program Files;
- owns no socket, MsQuic handle, device identity, trust record, clipboard,
  audio, capture, or product control pipe;
- duplicates only its own SYSTEM token, binds it to the active console session,
  and launches one fixed non-reparse sibling path with `CreateProcessAsUser`;
- accepts only two lab controls: a Default-desktop release probe and a
  Winlogon-desktop cancel probe; and
- has manual startup and an admin/SYSTEM-only start, stop, reconfigure, and
  user-defined-control DACL in the separate lab script.

`desklink_secure_input_helper.exe`:

- refuses non-SYSTEM execution and any session other than the active console
  session;
- refuses locked/unknown sessions and verifies both its thread desktop and the
  current input desktop are exactly `Default` or `Winlogon` for the requested
  fixed probe;
- accepts no path, process, command line, scan code, pointer coordinate, text,
  credential, or network input;
- the Default probe releases modifier and mouse-button state only; and
- the secure probe performs the same release and sends Escape to cancel a UAC
  dialog. It cannot approve a prompt or select a consent credential.

The lab installer is intentionally separate from the DeskLink installer. It
requires elevation plus `-ConfirmExperimental -DenyProductIntegration`, stages
only the two fixed files beneath Program Files, creates a manual-start service,
and applies a restrictive service DACL. It must not be used on a daily-use PC
until the build and static security gates pass.

## Authorization model for product integration

The portable `SecureInputAuthorizationGate` defines the minimum eventual input
envelope. Every admitted event must match all of:

- explicit machine-level `AllowSecureDesktopInput` grant;
- exact peer machine ID and certificate DER SHA-256 hash;
- current authenticated session nonce;
- current nonzero focus epoch;
- monotonically increasing grant revision and event sequence; and
- a 100-2000 ms service-side lease.

Wrong identity, grant, nonce, epoch, sequence, operation, expiration, or replay
is rejected. Expiration and revocation clear authority. This model is not yet
wired to the helper. Wiring is prohibited until the service can independently
authenticate a fixed production-signed runtime installed in an administrator-
protected location and read an administrator-protected secure-input grant.

The existing LocalAppData runtime and current-user pipe cannot be trusted as a
LocalSystem authorization source: that would turn same-user process control or
a writable binary replacement into elevation. A production slice therefore
requires a separately approved machine-wide, signed installation boundary.

## Stage gates

1. **Integration baseline:** secure-desktop transitions fail Local without
   replacing trust or misclassifying UAC as TLS/authentication failure.
2. **Foundation:** default-off service/helper targets compile and self-test;
   source contracts prove the absence of networking, arbitrary execution,
   private-key export, policy weakening, UIAccess, and product packaging.
3. **Lab access probe:** on a disposable/approved Windows 11 target, prove the
   fixed release probe on `Default` and fixed Escape probe on `Winlogon`.
   Record session ID, desktop, process identity, outcome, and cleanup without
   logging input content.
4. **Authorization integration:** only after production code signing and a
   protected machine-wide install design exist, implement authenticated local
   IPC and the exact authorization envelope. No session is admitted from a
   current-user assertion alone.
5. **Physical product validation:** separately granted secure-input permission,
   UAC cancel and approve workflows, held-state cleanup, helper/service crash,
   session switch, lock/unlock, sleep, disconnect, stale/replayed envelopes,
   grant revocation, and uninstall all fail closed. The ordinary fail-local
   shortcut remains available on the controlling PC.

No stage may automatically approve UAC, handle credential UI, run on the lock
or sign-in desktops, stream secure-desktop video, or broaden the helper into a
general remote-control service. If user-mode SYSTEM injection cannot satisfy
these gates, stop and reassess; a virtual HID/kernel driver is not an automatic
fallback.
