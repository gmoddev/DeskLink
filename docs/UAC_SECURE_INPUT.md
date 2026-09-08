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
- waits at most five seconds for that one-operation child and fails the probe
  unless the helper exits successfully;
- stops after every probe with an explicit success or service-specific failure
  status so the lab harness cannot confuse service-control delivery with
  successful input execution;
- accepts only three lab controls: a Default-desktop release probe, a
  Default-desktop elevated-foreground minimize probe, and a Winlogon-desktop
  cancel probe; and
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
- the Default minimize probe derives the target solely from the exact local
  foreground, requires an unlocked `Default` desktop plus a visible top-level
  window owned by an elevated process, rechecks the same foreground before
  posting only `SW_MINIMIZE`, and succeeds only after `IsIconic` confirms that
  exact window was minimized; and
- the secure probe verifies that `consent.exe` owns the foreground before
  releasing owned state, rechecks it afterward, then sends Escape. It uses an
  Escape scan code and reports success only after the active input desktop
  returns to `Default`. It cannot approve a prompt or select a consent
  credential.

The lab installer is intentionally separate from the DeskLink installer. It
requires elevation plus `-ConfirmExperimental -DenyProductIntegration`, stages
only the two fixed files beneath Program Files, creates a manual-start service,
and applies a restrictive service DACL. It must not be used on a daily-use PC
until the build and static security gates pass.

## Windows 11 physical access-probe result

The Stage 3 fixed-probe gate first passed on an approved Windows 11 Pro build
26200 target on 2026-09-07 using source revision `162368a`. After tightening
the acknowledgement contract and moving consent verification ahead of every
release/injection action, the complete visible probe passed again using the
reviewed source revision `a786563`. The exact hardened artifacts were:

- service SHA-256
  `38E8F836F4A94C3CB1906D0E4BE983429BDD19CEB97498355D67381FA854FE75`;
- helper SHA-256
  `59BF63A61E1957958E4CCE8752BD2DDB16324FF5ED6B9935F6FA06F9ED03FD9A`;
- LocalSystem, demand-start service under
  `C:\Program Files\DeskLink Secure Input R&D`; and
- service DACL
  `D:(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;SY)`
  `(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)(A;;CCLCSWLORC;;;AU)`.

The hardened current-build Default release probe completed. For the secure
probe, the helper verified LocalSystem, the active console session, an unlocked
session, its `Winlogon` thread desktop, the active `Winlogon` input desktop,
and foreground `consent.exe` before releasing any fixed input state. It
revalidated the secure context, sent scan-code Escape, and observed the input
desktop return to `Default`; the person viewing the target independently
confirmed that the benign UAC prompt disappeared. The one-shot service then
stopped with Win32 and service exit codes both zero, and no helper remained.

An earlier return-count-only attempt was explicitly rejected after visual
confirmation showed that the prompt remained. Acceptance was strengthened to
require the observable `Winlogon`-to-`Default` transition. A later attempt made
without an active secure desktop returned stage code 17, surfaced by the
service as `1017`, and injected nothing. This fail-closed negative result is
part of the evidence, not a successful prompt test.

The same hardened build passed all 10 configured native tests. A complete
security diff review from main revision `902931e` through `a786563` found no
reportable vulnerabilities. It intentionally did not promote the lab's
inherited Program Files protection into a production guarantee: a shipping
installer/service must verify every path component's owner and ACL, reject
reparse points throughout the path, and verify the helper's trusted signature.
After evidence capture, the R&D service, its Program Files staging directory,
and its helper process were removed and their absence was verified. The
incremental source and build trees remain under `C:\Sandbox\Codex` for
reproducibility.

This proves only that the narrow SYSTEM helper can cancel a visible consent
prompt under the lab constraints. It does not approve prompts, prove arbitrary
remote input, authorize a current-user runtime, provide secure-desktop video,
or satisfy the Stage 5 product matrix.

## Elevated-window recovery probe

On 2026-09-08, the Windows 11 target proved that the current-user process
cannot implement a reliable elevated-window recovery action. A limited
interactive process observed the exact elevated Task Manager foreground, but
`ShowWindowAsync(SW_MINIMIZE)` returned false and the window remained visible.
This is the expected UIPI boundary.

The separately staged LocalSystem R&D service then launched the fixed helper
on the active unlocked `Default` desktop. With Task Manager as the exact
visible foreground, the helper verified that its owner token was elevated,
rechecked the unchanged HWND, posted only `SW_MINIMIZE`, and observed
`IsIconic`. The one-shot service reported success. The harness then removed the
service and helper and verified that no helper remained.

This validates the recovery primitive but does not make it a product feature.
No window handle, process ID, process name, title, show command, or arbitrary
operation may cross a future IPC or network boundary. The helper must always
derive and revalidate the one local foreground target itself.

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
4. **Authorization integration:** only after an approved code-signing policy and
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

## Exact product work still required

The Development Secure channel now proves a dedicated non-exportable signing
key, explicit public-certificate trust, timestamped binaries, and a protected
machine-wide application directory on two approved PCs. This closes only the
signing/install prerequisite for private R&D. It is not publicly trusted and
does not package, start, or authorize the lab service/helper.

Until that helper is integrated, an elevated foreground on the ordinary
`Default` desktop is not controllable by the normal injector. While remote focus
is active the receiver rechecks this boundary every 50 ms; a higher-integrity
or uninspectable foreground releases owned input and fails both peers Local.
That safety fallback is not elevated-window control and does not minimize or
interact with the elevated application.

The following product work is still required before elevated-window recovery or
UAC control can be enabled in DeskLink:

1. Retain Development Secure only for controlled R&D; a public production
   release still requires a publicly trusted Authenticode identity. The existing
   device CNG key is not a release signing key and remains non-exportable and
   unchanged.
2. Extend the approved per-machine slice to install the signed service and fixed
   signed helper beneath the protected Program Files directory. Verify every
   path component's owner, ACL, and reparse status and verify exact signer
   equality before every helper launch.
3. Give the service no network stack. Add an ACL-restricted named pipe whose
   protocol is fixed-size, versioned, replay-resistant, bounded, and accepts no
   paths, commands, window handles, process IDs, titles, or input content.
4. Make the service independently authenticate the fixed signed DeskLink
   runtime from the protected installation. A LocalAppData binary or mere
   current-user pipe possession is not an authorization source.
5. Persist a separate, default-off machine-level permission for elevated input
   and recovery. Permission addition requires local foreground approval;
   revocation returns Local and releases owned input before persistence.
6. Bind each request to the exact `PeerValidated` machine ID and certificate
   DER hash, current session nonce, nonzero focus epoch, grant revision,
   monotonic sequence, requested operation, and a 100-2000 ms service lease.
7. Expose the bounded minimize operation separately from secure-desktop input.
   It may act only on the helper-derived visible elevated foreground on the
   active unlocked `Default` desktop and must return a verified result before
   DeskLink retries focus.
8. For UAC interaction, launch the fixed helper on `WinSta0\\Winlogon` only
   while `consent.exe` is the exact foreground. Forward only individually
   authorized scan-code, button, pointer, wheel, and release envelopes; keep
   arbitrary command execution, text injection, credentials, lock/sign-in
   desktops, and automatic approval out of scope.
9. Add controller UI for `Blocked by an elevated app`, `Minimize blocking app
   and retry`, secure-input opt-in, explicit operation result, timeout, and
   fail-local recovery. Never leave the product at an indefinite `Connecting`
   or `Connected` state when input admission is unavailable.
10. Complete security review and physical Windows 11 validation for wrong
    signer, writable/reparse path, pipe spoofing, wrong peer/pin/nonce/epoch,
    replay, expiry, grant revocation, foreground race, ordinary window,
    Task Manager, installers, UAC approve/cancel, held input, helper/service
    crash, lock/unlock, session switch, sleep, disconnect, upgrade, and
    uninstall. Every failure returns Local and admits no privileged input.
