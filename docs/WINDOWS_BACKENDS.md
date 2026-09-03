# DeskLink Windows Backend Plan

## Platform baseline

Production Windows backends target Windows 11 or Windows Server 2022 or newer.
The production transport is stock MsQuic/Schannel using DeskLink's existing
non-exportable current-user CNG identity. Windows 10 remains unsupported while
an approved, separately gated equal-security MsQuic/OpenSSL R&D path is built.
It receives no identity-export or reduced-security fallback. The complete
support decision and R&D boundary are recorded in
[`PLATFORM_SUPPORT.md`](PLATFORM_SUPPORT.md).

## Native link-local discovery

`Win32MdnsAdvertiser` and `Win32MdnsBrowser` use Windows 10-or-newer
`DnsServiceRegister`, `DnsServiceBrowse`, and `DnsServiceResolve` with interface
index zero (all local interfaces), `_desklink._udp.local`, and mDNS rather than
unicast DNS. `listen` advertises an open manual-pairing window; `serve`
advertises a closed window. Registration failure is non-fatal because manual
host/IP operation remains available.

`discover` is a one-shot bounded observer. It resolves at most 64 names, rejects
malformed metadata through the portable discovery codec, groups duplicate
multi-interface results deterministically, and prints candidates without any
transport or trust-store action. No Windows service, firewall rule, network
profile, router setting, or persistent background browser is created.

The persistent runtime broker reuses the same native browser for the product
shell's explicit one-to-30-second Nearby scan. Results are cached in one
generation, exposed as visibly untrusted typed records, and invalidated on stop
or replacement. The native browse observes cooperative cancellation in bounded
increments, so stop and broker shutdown do not wait out a long scan. A pairing
start accepts only a unique, nonambiguous,
protocol-compatible cached machine whose advertisement says the pairing window
is open. Manual host/IP entry remains an explicit fallback through the same
managed pairing boundary.

## 1. Current Windows implementation

The repository includes `Win32InputInjector` and the opt-in
`Win32InputCapture`/`Win32SuppressionGate` path.

It uses `SendInput` with:

- `KEYEVENTF_SCANCODE` for keyboard injection
- `KEYEVENTF_EXTENDEDKEY` when required
- `KEYEVENTF_KEYUP` for release
- mouse down/up flags for buttons
- `MOUSEEVENTF_WHEEL` / `MOUSEEVENTF_HWHEEL` for bounded signed wheel deltas
- `MOUSEEVENTF_MOVE | MOUSEEVENTF_MOVE_NOCOALESCE` for ordinary relative motion
- `MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK` for absolute pointer movement

The injector tracks DeskLink-owned pressed keys/buttons and releases only those states during cleanup.

Display ID zero retains the whole-virtual-desktop mapping for protocol
compatibility. Nonzero IDs use active DisplayConfig target device paths as
stable identities, deterministic order-independent 16-bit IDs, and a
display-rectangle-to-virtual-desktop transform. The injector pins a topology
generation for the focus lifetime and rejects unknown IDs, ambiguous clone
paths, hash collisions, refresh failures, and mappings invalidated by a
material topology change. Focus release clears the generation pin.

---

## 2. Host input capture

The initial implementation uses two mechanisms with separate responsibilities.

### Raw Input mouse capture

Use Raw Input as the authoritative high-rate physical mouse event source.

Responsibilities:

- physical mouse movement
- physical mouse buttons
- device identity if required later
- high polling-rate devices

Raw input should feed a bounded in-process queue.

The current adapter registers the mouse `RIDEV_INPUTSINK` device against a
message-only window. It feeds a 1024-event queue, coalesces adjacent bounded
relative-motion samples, and disables routing on overflow. Relative counts are
not normalized through the source virtual-desktop dimensions. Optional
fixed-point gain and source-DPI normalization retain fractional counts and do
not modify global Windows settings. Absolute input devices establish a first
sample baseline and are converted to relative screen-pixel deltas rather than
jumping the receiving cursor.

The capture owner also polls the active Windows input desktop every 50 ms.
Only the case-insensitive `Default` desktop is eligible for routing. An
inaccessible desktop or any other desktop (including the UAC consent desktop)
atomically clears suppression before queued input can be forwarded, returns
focus Local, and preserves the authenticated transport. The capture watcher is
kept alive while input is unavailable so returning to `Default` re-arms the
saved roaming policy without a reconnect. It never injects into, attaches to,
or attempts to bypass the secure desktop.

Physical wheel messages are the deliberate exception to Raw Input authority:
the low-level mouse hook must enqueue each cumulative wheel delta before it can
safely suppress the matching local event. It accepts vertical and horizontal
signed deltas in `-1200..1200`, excludes zero, and forwards them on the reliable
ordered lane. Injected wheel events always pass. If validation or the hook's
nonblocking bounded enqueue fails, routing is cleared and that event passes to
Windows locally. A later reliable forwarding failure also clears routing and
releases focus through the normal fail-local control path.

### Low-level keyboard capture and suppression

`WH_KEYBOARD_LL` is the authoritative keyboard scan-code source and the
keyboard suppression gate while routing input remotely. This avoids losing
keyboard packets on systems where suppressing the event prevents the hidden
window from receiving the corresponding `WM_INPUT`. `WH_MOUSE_LL` remains only
the mouse suppression gate; Raw Input supplies mouse movement and buttons.

The hook callback must be minimal:

```text
read current routing flag
reject injected events
enqueue the hardware scan code without waiting
if remote -> suppress
else -> CallNextHookEx
```

Do not perform:

- network I/O
- disk logging
- heap-heavy serialization
- profile evaluation
- blocking locks or waits

inside the hook callback.

A separate worker consumes the bounded capture queue and sends DeskLink
protocol messages. If the keyboard hook sees an invalid scan code or cannot
enqueue immediately, it disables routing and fails local.

The current hook gate passes injected events and uses Ctrl+Alt+Pause as the
physical emergency chord. It accepts both `VK_PAUSE` and Windows' Ctrl+Break
`VK_CANCEL` representation. The chord clears the route flag in the hook before
notifying the control worker. Capture is never enabled unless the operator
supplies `--capture`.

---

## 3. Injection boundary

`SendInput` should remain the default injection mechanism.

The injector applies the same `Default`-desktop gate. Focus acquisition is
temporarily rejected while Windows owns another input desktop, and input that
arrives during an already-held lease is rejected as unavailable rather than as
malformed protocol traffic. This distinction prevents an expected UAC
transition from terminating TLS while retaining UIPI as the authority boundary.
Held DeskLink-owned state remains subject to the normal bounded snapshot and
release reconciliation when `Default` returns.

Do not install a virtual HID driver in the initial product merely to make synthetic input look more physical.

A virtual HID backend should be a later optional component only if a concrete use case cannot be met by `SendInput`.

---

## 4. Foreground/profile detection

The Host profile engine should use WinEvent hooks for foreground-window changes rather than aggressive polling.

Profile evaluation inputs may include:

- executable path/name
- fullscreen state
- current foreground HWND
- current user override

Precedence:

```text
emergency fail-local
manual override
profile rule
system default
```

`GAME` should release remote focus and remove/disable interception components as aggressively as practical.

The implemented foundation uses an out-of-context
`EVENT_SYSTEM_FOREGROUND` WinEvent hook rather than polling. The registration
thread owns a message loop and also performs `UnhookWinEvent`, matching the
Win32 thread-affinity requirement. Each observation contains only a bounded
UTF-8 executable basename, an opaque window identifier, an inspectability bit,
and monitor-relative fullscreen state. Process access is limited to
`PROCESS_QUERY_LIMITED_INFORMATION`; DeskLink does not inject or load code into
the foreground process.

`ForegroundProfileEngine` accepts at most 32 exact basename rules. ASCII case
is normalized, path separators/control characters and duplicate matches are
rejected, and rules may require fullscreen. If configured rules cannot inspect
the current foreground, the engine selects fail-local `LockPc1`. The production
Host CLI exposes `--default-mode`, repeated `--profile exe=mode`, and repeated
`--profile-fullscreen exe=mode`; parsing uses the same bounded validator as the
policy engine. WinEvent, control-pipe, `FocusReady`, renewal, emergency, and
capture-failure events are serialized by one Host runtime owner. The lifecycle
boundary disables routing, releases the active focus epoch, stops the Win32
capture threads and hooks, then
applies `GAME`/`LOCK_PC1`. On exit it requests a new focus transaction and does
not restart or enable capture until `FocusReady` and the initial state snapshot
succeed. `Win32InputCapture` resets its queues, worker stop state, hook handles,
window, pointer-calibration residuals, and absolute-device baseline before a restart.

---

## 5. Monitor mapping

The current implementation combines active `QueryDisplayConfig` source/target
records with `EnumDisplayMonitors` and `GetMonitorInfo` rectangles. The stable
identity is the normalized DisplayConfig target device path; friendly names and
transient GDI/HMONITOR ordering never determine the ID.

Do not persist transient HMONITOR handles.

The topology map derives a nonzero 16-bit ID from the stable identity and rejects
duplicates or collisions. Rectangle, primary-display, addition, and removal
changes advance a process-local generation. An active nonzero-display pointer
mapping remains pinned to its original generation and fails closed after such a
change until focus is released and reacquired.

Each active descriptor also carries checked pixel dimensions, refresh in
millihertz, orientation, and physical millimeters for presentation. Physical
size prefers the matching monitor-interface EDID base block after header and
checksum validation, falls back to a bounded `MDT_RAW_DPI` estimate, and
otherwise remains explicitly unknown. Friendly-name, refresh, physical-size,
orientation, and canvas changes update presentation state without advancing the
routing generation.

The Phase 1 roaming model persists at most 128 explicit cross-machine links.
Endpoints use machine ID, full stable display identity, side, and a normalized
edge segment. The complete graph is validated before a versioned, 512 KiB-
bounded preferences file is atomically replaced. Duplicate links, overlapping
active source segments, invalid direction/policy bounds, ambiguous machines,
and missing displays fail closed. The preference store contains no certificate,
pin, capability, trust record, or CNG identity material. Canvas coordinates are
not accepted by any route-resolution API.

The Phase 2 runtime uses `Win32DisplayTopology` as the local snapshot source on
both trusted session roles. It publishes after session admission, on material
generation changes, and every two seconds while the peer's trust record carries
the explicit `DisplayTopologyExchange` capability. Existing trust records are
not rewritten; the grant requires re-pairing. Incoming snapshots remain behind
the transport's `PeerValidated` gate and the session's trust, expected-machine,
and fresh-nonce checks. Invalid or five-second-stale topology leaves routes
unready without affecting focus, input cleanup, or the persisted graph.

Phase 3 adds a native configurator backed by the same `Win32DisplayTopology`
descriptors and atomic roaming store. The canvas groups displays by machine,
scales exact EDID sizes relative to one another, marks DPI/unknown sizes as
estimated, retains unresolved saved identities as offline cards, and shows
route resolution independently of peer status. Dragging may propose an
opposing-edge full link, but only explicit confirmation adds it; the advanced
editor supports normalized partial segments and one-way modes.

`Identify displays` creates a borderless topmost/no-activate/click-through
overlay near the top-left of each active display on the connected desk. It
shows display number, PC, resolution, refresh, and estimate state, destroys
itself after five seconds, and does not interact with capture. The local shell
creates its overlays directly; the peer runtime uses a stoppable message-pump
worker after an authenticated, mutually topology-authorized request. Duplicate
requests are ignored while that worker is active. The companion refreshes
authenticated peer topology asynchronously through bounded same-SID IPC so the
UI thread does not wait for network synchronization.

Phase 4 adds an explicit experimental outbound mode. `Win32InputCapture` may
remain installed while Local to report only screen position plus raw motion;
the keyboard and mouse hooks still pass physical events locally and button,
wheel, key, and remote-motion handlers remain inactive. The serialized Host
coalesces observations, evaluates the portable crossing state, and reuses the
same capture instance after fresh focus. It cannot set `RemoteRouting` until
the route/session is revalidated, landing and initial snapshot sends succeed,
and the direction arbiter and portable state both admit Remote. Any failure
clears routing first and returns to the existing lifecycle's Local state.

The Host UI should store edge adjacency, for example:

```text
PC1.Display2.RIGHT -> PC2.Display1.LEFT
```

Crossing should include a small hysteresis/dead-zone to prevent focus oscillation.

---

## 6. WASAPI sender on PC2

The implemented foundation uses:

```text
IMMDeviceEnumerator
  ↓ default render endpoint
IAudioClient shared mode
  ↓ LOOPBACK flag
IAudioCaptureClient
  ↓
format normalization
  ↓
5 ms PCM blocks
  ↓
AudioFrame datagrams
```

V1 should normalize to:

```text
48 kHz / stereo / PCM16 / 5 ms
```

`Win32WasapiLoopbackCapture` owns COM and all endpoint interfaces on one MMCSS
worker. It requests the canonical format with the shared-mode audio-engine
conversion flags, drains each complete capture packet before release, converts
WASAPI silence into zero samples, resets a partial block on discontinuity, and
uses a steady timestamp if WASAPI marks its QPC timestamp invalid. Source
packets above 8192 frames fail the audio module rather than causing unbounded
work. Published blocks are always exactly 960 PCM bytes. The production sender
starts only with `--send-audio` after `PeerValidated` and the peer's
`AudioReceive` grant; datagram rejection stops capture without stopping input.
MsQuic datagram readiness observed during certificate/session bootstrap is
carried into the admitted transport endpoint, and the endpoint rechecks the
current send-enabled parameter after callback handoff. This prevents a valid
audio session from permanently losing datagrams merely because MsQuic emitted
`DATAGRAM_STATE_CHANGED` before application admission; authentication and
session-admission gates remain unchanged.
The receiver drains at most the existing 20-block queue per pump wake so
Windows timer coalescing and bursty QUIC callback delivery cannot make valid
blocks late merely because a nominal five-millisecond sleep woke later. The
renderer queue remains bounded independently.

---

## 7. WASAPI receiver on PC1

The implemented foundation uses:

```text
network datagrams
  ↓
AudioJitterBuffer
  ↓
loss concealment
  ↓
small asynchronous resampler
  ↓
gain
  ↓
IAudioRenderClient shared mode
```

Windows then mixes the DeskLink stream with local applications. DeskLink does not need to capture/re-mix all PC1 audio itself.

`Win32WasapiRenderer` validates the canonical frame shape before accepting a
block, caps its queue at 64 blocks (320 ms), pre-rolls silence, and fills an
empty shared endpoint buffer with silence while counting underruns. The
production receiver starts only with `--receive-audio` after `PeerValidated`
and the sender's `AudioSend` grant. `HostSession` rejects stale nonces and the
portable receiver enforces format, stream, sequence, and jitter bounds before a
five-millisecond pump submits to WASAPI. Capture/arrival delta variation and
concealment raise an adaptive 2-12 block target immediately; each downward
step requires 200 stable samples, and target growth enters a bounded rebuffer
state. Bounded clock-drift resampling follows concealment. Per-peer gain/mute
then applies at the portable receiver boundary with a one-block transition
before WASAPI submission; endpoint recovery preserves the setting and no
Windows mixer or endpoint volume is modified.

---

## 8. Clock drift controller

Separate machines do not have perfectly identical audio clocks. The receiver
tracks total admitted source samples in the jitter buffer plus asynchronous
resampler around the adaptive target. It averages 400 five-millisecond pump
observations before changing the ratio by one 50 ppm step:

```text
adaptive target occupancy: 10-60 ms
observation window: 2 s
ratio slew: 50 ppm per window
hard correction bound: ±1000 ppm (±0.1%)
resampler source bound: four 5 ms blocks
```

The streaming linear interpolator may consume slightly more or less than one
source block per output block, but emits only the canonical 240-frame PCM16
stereo shape. Occupancy—not an authenticated peer's still-untrusted capture
timestamp—selects the direction. A quarter-block deadband filters scheduling
jitter. Adaptive-target changes, capture-time regression/large jumps, session
reset/reconnect, render rejection, and endpoint recovery clear correction
state. These controls prevent hours-long sessions from gradually underrunning
or overflowing without allowing unbounded buffering or rate selection.

---

## 9. Device changes

The production adapters recover audio locally for:

- default endpoint changes
- device removal
- device disable/re-enable
- format change
- sleep/resume

Each worker registers a scoped `IMMNotificationClient` for the selected render
endpoint and default-console selection. A matching state/property/removal event,
default change, or operational WASAPI failure stops and unregisters the old
worker. The production audio owner clears assembler/receiver/render state and
reopens the current default endpoint after 250 ms, doubling to a five-second
retry cap while the same explicitly enabled session remains active. A new
endpoint never changes the audio stream ID, session nonce, peer grant, TLS
provider, CNG identity, or input state.

Capture callback/datagram-send rejection is classified as `ClientRejected` and
is not retried. This keeps session, authorization, and transport failure outside
the endpoint-recovery path. Physical default-device switching, disable/re-enable,
and sleep/resume remain release validation work rather than unimplemented
runtime behavior.

Capabilities should degrade independently.

## Text clipboard adapter

`Win32ClipboardSynchronizer` owns one message-only window and registers
`AddClipboardFormatListener` on its worker thread. It handles only
`CF_UNICODETEXT`; image, rich-text, HTML, file-drop, and delayed-render formats
are not read or transmitted. Unicode conversion uses strict invalid-character
flags and the portable module applies the 48 KiB UTF-8 bound before send or
write.

The listener may start for an explicitly requested session, but local reads
remain disabled until `PeerSession::CanSendClipboard()` confirms authenticated
mutual capability and module negotiation. Remote writes enter an eight-item
queue. `OpenClipboard` uses five five-millisecond attempts, and failure rejects
only that update. An applied update records the resulting Windows clipboard
sequence plus exact in-memory text; matching notifications are suppressed until
the sequence changes, preventing feedback without hiding a later local copy of
the same text. Stop joins the window thread and clears queued and suppression
content. Diagnostics expose counts and failure classes only, never text.

---

## 10. Local named pipe

The persistent broker and active session process expose a current-user-only
pipe named from the endpoint generation and current user SID:

```text
\\.\pipe\DeskLink.Control.v1.<current-user-SID>
```

Creation uses a protected DACL with exactly one current-user allow ACE,
`PIPE_REJECT_REMOTE_CLIENTS`, overlapped I/O, a two-second server I/O bound, and
first-instance protection. Both endpoints read the opposite process ID and
compare its token user SID before exchanging data. The client permits at most a
five-second requested timeout.

The local control protocol is independently versioned (currently version 7) and uses
exact bounded typed commands; it never exposes arbitrary transport packets,
input injection, OS commands, or module loading. In addition to runtime state,
mode, focus, audio, topology, update, and preference operations, it exposes
bounded trusted-device, discovery, pairing-window, pairing-candidate,
permission-reduction, forget, pause, resume, and return-local operations.
Nearby data remains untrusted. Grant additions require cryptographic re-pairing,
and reductions/forget force cleanup before persisted trust changes.

Managed pairing uses a separate internal command pair on this same-user pipe.
The broker creates a fresh 128-bit token and operation ID, gives them only to
its fixed sibling `desklink_pair.exe`, and accepts a candidate only when its
machine, certificate fingerprint, transcript, grants, operation, and 90-second
lease all match. The product shell receives only the bounded display candidate
and resolves its current operation. Shell absence/exit, expiry, token mismatch,
pipe failure, signing/authentication failure, or child failure rejects rather
than falling back. A successful child exit causes an in-process trust-store
reload before the role-driven runtime resumes.
