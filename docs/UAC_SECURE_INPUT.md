# DeskLink secure-desktop input R&D

## Decision

DeskLink implements UAC secure-desktop control in the separately trusted
Development Secure channel with a minimal Windows service and per-session
SYSTEM helper. No kernel input driver is used. This remains an experimental,
default-off security boundary, not a privilege increase for the ordinary
current-user package and not a publicly trusted production feature.

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

## Current product slice

`DESKLINK_BUILD_SECURE_INPUT_PRODUCT=ON` adds three fixed binaries only to a
Development Secure stage:

- `desklink_secure_input_service.exe` runs as LocalSystem, owns no network
  stack, and accepts one fixed-size local named-pipe protocol. It rejects remote
  pipe clients and authenticates the caller's PID, active console session,
  exact protected `desklink_pair.exe` path, and exact Authenticode signer.
- `desklink_secure_input_helper.exe` is launched only as the same signed,
  non-reparse Program Files sibling on the exact active input desktop. Requests
  cannot supply a path, process, window, title, command, desktop, or executable.
- `desklink_secure_input_configurator.exe` is the only grant writer. It requires
  administrator elevation and writes a protected, revisioned exact peer machine
  ID and certificate DER hash. It writes `Enabled=0` first and `Enabled=1` last,
  so partial failure remains disabled.

The WinUI Advanced page deliberately labels enablement **I know what I'm doing
— enable elevated control**, explains the authority increase, and makes **Keep
fail-local protection** the default action. The setting does not change TLS,
pinning, pairing, `PeerValidated`, capabilities, identity storage, UAC policy,
or any transport fallback. Each focus still needs the existing capability and
epoch plus a service-side exact peer/pin/session nonce/revision/sequence and
100-2000 ms lease. Focus release, expiry, disconnect, capability removal, and
input failure release owned state and revoke the broker grant.

On `Default`, the helper accepts only the bounded input wire operations needed
for already elevated applications. On `Winlogon`, it additionally requires
foreground `consent.exe` and rejects every key and state-reconciliation request;
only pointer motion, mouse buttons, wheel, and release remain. DeskLink can
therefore support a manual visible consent/cancel click but cannot type
administrator secrets. It never acts on lock/sign-in desktops and supplies no
secure-desktop video.

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
is rejected. Expiration and revocation clear authority. This model is wired
only in Development Secure. The service independently authenticates the fixed
signed runtime from its administrator-protected install and independently reads
the administrator-protected secure-input grant.

The existing LocalAppData runtime and current-user pipe cannot be trusted as a
LocalSystem authorization source: that would turn same-user process control or
a writable binary replacement into elevation. A production slice therefore
requires a separately approved machine-wide, signed installation boundary.

The runtime distinguishes a broker rejection from the narrow helper handoff
that can occur while Windows changes between `Winlogon` and `Default`.
`DesktopUnavailable` and a deliberately blocked Winlogon operation preserve
the already authenticated broker authorization; after `Default` returns, only
an exact temporary result receives a 750 ms recovery window. Packets are
rejected rather than queued or admitted during that window. Recovery continues
the same bounded focus lease. Expiry of the window, an authorization mismatch,
an IPC failure, or an injection failure releases owned state and fails Local.

## Stage gates

1. **Integration baseline — passed:** ordinary secure-desktop transitions fail
   Local without replacing trust or misclassifying UAC as a transport failure.
2. **Foundation and lab probes — passed:** default-off R&D components, fixed
   Default release/minimize, and visible Winlogon cancel probes passed under the
   recorded constraints.
3. **Protected signing/install boundary — passed as a private prerequisite:**
   the non-exportable Development Secure signer, explicit public trust, and
   protected Program Files install were validated on the approved PCs.
4. **Authorization integration — implemented:** fixed-size authenticated IPC,
   signed caller/helper/path validation, protected exact-peer grant, and the
   pin/nonce/epoch/revision/sequence/operation/lease envelope are wired only in
   Development Secure. No session is admitted from a current-user assertion.
5. **Physical product qualification — open:** enable one exact peer, exercise
   elevated Task Manager and manual pointer-only UAC accept/cancel, and validate
   held-state cleanup, wrong signer/path/peer/pin/nonce/epoch/revision/sequence,
   replay, expiry, grant revocation, process/helper/service failure, session
   switch, lock/unlock, sleep, disconnect, upgrade, and uninstall.

No stage may automatically approve UAC, type on Winlogon, handle authentication
UI, run on the lock or sign-in desktops, stream secure-desktop video, or broaden
the helper into a general remote-control service. If user-mode SYSTEM injection
cannot satisfy these gates, stop and reassess; a virtual HID/kernel driver is
not an automatic fallback.

## Remaining qualification work

Development Secure remains controlled R&D; a public production release still
requires a publicly trusted Authenticode identity. The device CNG identity is
not a release-signing key and remains non-exportable and unchanged. Ordinary
packages contain no privileged broker and continue to fail Local at UIPI and
secure-desktop boundaries.

The signed physical matrix must still verify:

1. install, repair, upgrade, rollback, and uninstall preserve identity/trust,
   use the exact service image/account/DACL, and remove the protected grant;
2. wrong signer/path/pipe client/peer/pin/nonce/epoch/revision/sequence,
   replay, and lease expiry admit no input;
3. enable targets only one exact already paired peer, defaults to no change,
   takes effect without re-pairing, and disable revokes before success;
4. ordinary and elevated Task Manager input, held key/button cleanup, and
   runtime/helper/service termination recover without stranded suppression;
5. visible benign UAC prompts allow manual pointer-only accept/cancel while
   key, reconciliation, no-prompt, authentication, lock, and sign-in cases are
   rejected; and
6. lock/session switch, sleep, disconnect, reconnect, foreground races, and
   service recovery remain fail-closed.

A separate bounded **Minimize blocking app and retry** operation and specific
broker diagnostics are still desirable UX work. They may act only on the
helper-derived visible elevated foreground on the active unlocked `Default`
desktop and must return a verified result; no caller-supplied window, process,
path, title, or command may cross IPC.
