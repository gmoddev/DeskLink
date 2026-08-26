# DeskLink Architecture

## 1. Purpose

DeskLink provides multiple desk-level capabilities over one authenticated peer session. The initial capabilities are keyboard/mouse routing and network audio, but the architecture intentionally allows future modules without turning the platform into a generic remote shell.

The base topology is:

```text
PC1 / Host                                      PC2 / Agent
┌─────────────────────────┐                  ┌─────────────────────────┐
│ DeskLink.Host           │                  │ DeskLink.Agent          │
│                         │                  │                         │
│ Focus coordinator       │                  │ Input authorization     │
│ Profile engine          │                  │ Input injection         │
│ Monitor graph           │                  │ Audio capture           │
│ Audio receiver          │                  │ State provider          │
└────────────┬────────────┘                  └────────────┬────────────┘
             │                                            │
             └──────── DeskLink.Core / Protocol ──────────┘
                              │
                    Native MsQuic / Schannel
                   authenticated TLS 1.3 QUIC
```

DeskLink UI and Stream Deck integrations are clients of the Host coordinator. They are not granted direct access to input injection or raw transport primitives.

---

## 2. Component ownership

### DeskLink.Core

Owns mechanisms shared by all modules:

- peer identity
- trust and capability grants
- session state
- binary message protocol
- bounded decoding
- connection lifecycle contract
- transport lanes
- sequence/epoch metadata
- health/statistics interfaces

The portable core contains neither hooks nor WASAPI details. The optional
`desklink_windows` adapter contains low-level physical keyboard capture,
Raw Input mouse capture, fail-local suppression hooks, `SendInput`, and
event-driven shared-mode WASAPI loopback/render adapters.

### DeskLink.Host

Runs on the PC with the physical keyboard/mouse.

Responsibilities:

- authoritative desired/effective input mode
- focus transfer initiation
- focus lease renewal
- emergency fail-local
- monitor topology/edge mapping
- input capture routing
- profile application
- remote audio receive/playout control
- local UI/control API

### DeskLink.Agent

Runs in the interactive user session on a remote PC.

Responsibilities:

- capability enforcement
- focus lease enforcement
- epoch enforcement
- input injection
- DeskLink-owned input cleanup
- audio capture
- remote state reporting

The Agent should normally run at the same integrity level as the user. It should not be permanently elevated merely to bypass Windows UIPI.

---

## 3. Trust layers

DeskLink treats these as independent states:

```text
DISCOVERED
PAIRED
AUTHENTICATED
CAPABILITY_GRANTED
CAPABILITY_ACTIVE
```

Discovery data is never trusted. Pairing stores a persistent peer identity. Every later session must authenticate as that identity. An authenticated peer still cannot invoke an ungranted capability.

For input:

```text
Authenticated peer
    AND InputInject grant
    AND active focus lease
    AND current focus epoch
    AND valid protocol message
        => injection permitted
```

---

## 4. Transport architecture

The production transport baseline is Windows 11/Server 2022 or newer using the
stock MsQuic Schannel provider. The persistent device identity is the existing
non-exportable current-user CNG key and its pinned self-signed certificate.
Provider selection must not export or replace that identity, weaken peer
validation, or downgrade TLS. Windows 10 is outside the production architecture.
Its experimental compatibility architecture is an explicit MsQuic/OpenSSL
runtime with opaque CNG-backed signing. Its security and guarded physical gates
pass, but making it production-admissible requires a separately reviewed
release-integration decision.

A single logical peer connection carries several lanes:

```text
QUIC connection
├── control stream        reliable + ordered
├── state stream          reliable + ordered
├── input-state stream    reliable + ordered
├── input-motion          QUIC DATAGRAM
└── audio                 QUIC DATAGRAM
```

Reliable traffic includes:

- pairing/session control after the cryptographic bootstrap
- capability/state changes
- focus requests/renewals/releases
- keyboard down/up
- mouse button down/up
- bounded vertical/horizontal mouse-wheel deltas
- reconciliation snapshots

The Host records authoritative routed key/button state as events are accepted
for transmission. A snapshot travels on the ordered reliable lane every 500 ms
alongside lease renewal and distinguishes normal from extended Windows scan
codes. The Agent validates capability, session, focus epoch, and lease before
converging only its DeskLink-owned injected state. It releases stale holds
before applying missing presses; local physical state is outside that ownership
boundary. Snapshot-send failure uses the same fail-local path as lease-renewal
failure.

Wheel input is cumulative rather than latest-state, so it remains on the
ordered reliable lane. Each event carries one axis and a nonzero signed delta
bounded to `-1200..1200`. The Windows low-level mouse hook enqueues a physical
wheel event before suppressing it. Invalid deltas, queue contention, queue
overflow, and reliable-send failure disable remote routing; an event that
cannot be enqueued is passed to Windows locally.

Datagrams include:

- current pointer position
- PCM audio frames

The current implementation provides `ITransportEndpoint` plus a deterministic in-memory implementation. The production adapter should use native MsQuic.

The runtime-selection layer is retained as an integrity and diagnostics
boundary. Production packages ship only Schannel. Stage 2 research builds add
an explicitly selected, hash-pinned OpenSSL runtime and a built-in provider
that holds only public RSA parameters and an opaque reference to the same CNG
key. Both providers deliver the exact leaf DER into the same asynchronous pin
validator. The Windows 10 path is not an alternate trust model and remains
non-admissible until later gates pass; its constraints are recorded in
[`PLATFORM_SUPPORT.md`](PLATFORM_SUPPORT.md).

### Link-local discovery boundary

Windows uses native DNS-SD/mDNS for `_desklink._udp.local`. A listener publishes
an SRV host/port plus a small TXT record containing `txtvers`, `protovers`, the
machine ID, display name, capability hints, and pairing-window state. Browsing
is explicitly bounded; resolves are bounded separately. TXT records have fixed
field/count/byte limits, canonical lowercase hexadecimal identities, strict
UTF-8 text, and an exact supported protocol version.

Discovery lives before and outside the transport trust boundary. Its cache
groups multi-interface endpoints deterministically, expires observations, and
marks conflicting metadata for one machine ID as ambiguous. No candidate can
create a CNG identity, change the trust store, open a pairing window, select a
TLS provider, establish a connection, grant a capability, request focus, or
admit application traffic. Only the existing manual pairing and pinned TLS
paths can cross that boundary.

---

## 5. Input focus model

Input focus is a lease, not a permanent boolean.

The receiver tracks:

```text
focus: LOCAL | REMOTE
epoch: u64
lease_expiry: monotonic timestamp
mode: ROAM | LOCK_PC1 | LOCK_PC2 | GAME
```

A remote focus begins by advancing the epoch and assigning a short lease.

Example:

```text
Host                              Agent
  │                                 │
  ├── FocusRequest(id=R,750 ms) ───>│
  │                                 │ validate capability
  │                                 │ create epoch 42
  │<── FocusReady(id=R,epoch=42) ───┤
  │                                 │
  ├── KeyEvent(epoch=42) ──────────>│ accepted
  ├══ Pointer(epoch=42) ═══════════>│ accepted
  ├── FocusRenew(epoch=42) ────────>│ refresh lease
```

If the lease expires, epoch 42 is invalidated and the Agent releases DeskLink-owned key/button state.

A delayed packet carrying epoch 42 after a later focus transition is rejected even if it was valid when originally transmitted. Focus setup also carries a transaction `request_id`, so a delayed `FocusReady` from an older request cannot satisfy a newer focus attempt.

---

## 6. Fail-local rule

The system is designed around this invariant:

> Failure of DeskLink must never make the physical workstation permanently dependent on DeskLink.

Examples:

- Host hook process exits: Windows hooks disappear and PC1 input becomes local.
- Network disappears: PC1 stops forwarding and Agent's lease expires.
- Agent disconnects: Host returns local.
- Lease expires: Agent stops injection and releases owned state.
- Emergency chord: Host invalidates remote focus immediately.

`InputFocusStateMachine::emergency_fail_local()` makes this explicit in the core.

The foreground policy layer is deterministic and bounded independently of the
Windows event source. It accepts at most 32 exact executable basenames, may
require a fullscreen match, and applies this precedence:

```text
emergency fail-local
manual override
profile rule
system default
```

When rules are configured but the current foreground process cannot be
inspected, the decision is `LOCK_PC1`; missing metadata never silently matches
or weakens a rule. The Windows adapter observes `EVENT_SYSTEM_FOREGROUND` with
an out-of-context WinEvent hook on an owned message-loop thread. It requests
only `PROCESS_QUERY_LIMITED_INFORMATION`, extracts a bounded executable
basename, compares window bounds with the containing monitor for fullscreen
state, and never loads target-process code. The policy and observer are
implemented. The production Host CLI accepts a bounded fallback mode and at
most 32 exact `executable=mode` rules, including a separate fullscreen-only
form. The Host runtime serializes foreground WinEvent callbacks, current-user
control requests, transport `FocusReady`, renewal ticks, emergency release, and
capture/transport failures onto one lifecycle owner. Capture callbacks disable
routing synchronously before queueing any failure event. Its event queue is
bounded to 64 entries; overflow disables routing and terminates fail-local. The
lifecycle disables
routing before focus release, synchronously stops Win32 capture hooks before a
restricted mode is entered, and requests a new focus transaction when leaving
`GAME` or `LOCK_PC1`. Capture cannot be reinstalled or enabled until that
transaction produces a fresh `FocusReady` and the initial reliable input-state
snapshot succeeds. Renewal and authoritative snapshots run only while the
serialized lifecycle is `Remote`; restrictive profile decisions are not
misreported as renewal failures.

---

## 7. Pointer semantics

Protocol v2 separates ordinary physical motion from explicit display mapping:

```cpp
PointerMotionMessage { int32 DeltaX; int32 DeltaY; }
PointerPositionMessage { uint16 DisplayId; uint16 NormalizedX; uint16 NormalizedY; }
```

Raw Input movement is sent as bounded relative device counts and injected with
relative `SendInput`. It is therefore independent of the controlling PC's
virtual-desktop width. The Host may apply bounded 25-400% fixed-point gain and,
when the physical DPI is known, normalize 100-32000 DPI to an 800-DPI reference.
Fractional counts are retained between samples. DeskLink does not change global
Windows mouse settings.

Absolute `PointerPosition` remains available for monitor-edge transitions and
resynchronization. `0..65535` maps to the target display coordinate range.
Display ID zero is the legacy whole-virtual-desktop path; nonzero IDs map a
stable DisplayConfig target through its rectangle. The injector pins topology
generation for the focus lifetime and rejects unknown, ambiguous, colliding, or
stale mappings.

Both pointer datagram types share a monotonic sequence gate within the active
focus epoch. Older or duplicate datagrams are rejected, so delayed relative
motion cannot be applied after a newer motion or absolute transition.

---

## 8. Monitor topology

Production Host should maintain a graph rather than merely one giant coordinate rectangle.

Example:

```text
PC1.Display1.RIGHT <-> PC1.Display2.LEFT
PC1.Display2.RIGHT <-> PC2.Display1.LEFT
```

Each edge relationship should include:

- source machine/display
- source edge
- destination machine/display
- destination edge
- normalized crossing position
- optional dead-zone/hysteresis

Hysteresis is important to prevent rapid focus bouncing when the cursor sits exactly on a boundary.

The Phase 2 exchange is implemented independently of focus. After
`PeerValidated`, normal trust lookup, and fresh nonce negotiation, each session
role may publish its canonical local `DisplayTopologySnapshot` on the reliable
lane only when the peer has an explicit `DisplayTopologyExchange` grant. The
receiver binds it to the stored peer machine and both envelope and payload
nonces, validates exact canonical display descriptors within the 64 KiB
reliable limit, and retains it for five seconds. The Windows runtime refreshes
at two-second intervals and on generation changes.

Peer state and route state remain separate. A transport may be Connected while
a route is Synchronizing, missing a capability/display, directionally
unsupported, disabled, or invalid. Only a current validated topology can make
route resolution Ready. Lower generations do not refresh the lease;
same-generation mutations and malformed/wrong-peer traffic invalidate the
remote topology until reconnect. This layer cannot request focus or switch an
edge. Phase 3 displays and persists the graph, while crossing behavior remains
disabled until Phase 4.

The companion reads topology through a bounded current-user control request.
The named-pipe server retains its one-SID DACL and mutual process-SID checks;
responses contain one canonical local snapshot plus at most seven current peer
statuses/snapshots. A remote snapshot is exposed only while the authenticated
session tracker reports it current. This read-only surface has no trust,
capability, focus, or input mutation operation.

Configurator geometry is a separate presentation model. EDID millimeters and
DPI estimates choose card dimensions, and saved `CanvasLayout` coordinates
choose card positions. Only an explicitly confirmed `RoamingLink` containing
machine ID, stable display identity, side, and normalized segment can enter the
runtime graph. Atomic save validates the complete candidate and confirms Local
before replacement.

The experimental Phase 4 reciprocal runtime consumes only explicit resolved link
fields; canvas position and physical-size metadata never enter crossing or
landing calculations. While Local, the existing Win32 capture object observes
Raw Input motion with both low-level hooks passing through. A selected policy
may create a `FocusPending` request only for a Ready route. The serialized Host
then rechecks the trusted peer, `InputInject`, nonce, both topology generations,
link settings, and a direction token. Fresh `FocusReady` advances the portable
gate to `RemoteReady`; the absolute landing datagram and initial reliable input
snapshot must both be accepted before the lifecycle enables suppression and
marks the gate `Remote`. Focus stalls expire after 1.5 seconds.

`PeerSession` owns both `HostCoordinator` (outgoing) and `AgentCoordinator`
(incoming) for one validated transport. After admission, both endpoints report
the exact capability mask from their local persisted trust record. The masks
remain directional and immutable for that nonce; they do not alter trust.
Peer-reported grants may preflight an outbound input attempt, but local audio
and topology disclosure remain gated by this PC's persisted grant.
`PeerDirectionArbiter` binds each token to the authenticated peer machine,
session nonce, generation, and direction. It admits only one pending/active
direction, rejects a new outgoing request while incoming control is active,
and resolves simultaneous opposite requests by releasing/rejecting both to
Local. Duplicate authenticated sessions for the same peer and competing
capture-owner startups are converged before runtime publication. Reconnect
starts unbound/Local with a fresh nonce and never restores focus.

---

## 9. Audio architecture

PC2 captures render output using WASAPI loopback and packetizes small PCM blocks.

Recommended V1 wire format:

```text
48 kHz
stereo
signed PCM16
240 frames/channel
5 ms per packet
960 PCM bytes per packet
```

The wire `AudioFrameMessage` additionally carries stream/sample metadata and a capture timestamp.

PC1 performs:

```text
network
  ↓
sequence reorder
  ↓
bounded jitter buffer
  ↓
loss concealment
  ↓
clock drift compensation
  ↓
gain
  ↓
WASAPI shared render
```

The implementation provides exact 48 kHz/stereo/PCM16/5 ms block assembly,
bounded reorder/jitter with silence concealment, event-driven loopback capture,
and a shared-render adapter with a bounded 64-frame submission queue and
silence underrun. The operational path is explicitly PC2 to PC1: the sender
requires the peer's local `AudioReceive` grant and `--send-audio`; the receiver
requires the sender's `AudioSend` grant and `--receive-audio`. `PeerValidated`,
the current session nonce, one invariant nonzero stream ID, exact frame shape,
and bounded sequence admission all precede rendering. Both WASAPI workers watch
the selected render endpoint and the default-console selection. Endpoint loss,
removal, disablement, property/format change, and default switching stop the old
worker and schedule audio-only reopen with 250 ms to 5 s capped backoff. Capture
client rejection is not recoverable and never restarts. Recovery preserves the
admitted peer, session nonce, capability grants, TLS provider, CNG identity, and
input state; receiver recovery discards queued audio before playout resumes.
The receiver compares sender capture-time deltas with local steady arrival-time
deltas only after format, stream, sequence, nonce, capability, and peer
validation. A bounded estimator raises the playout target immediately on an
arrival spike or concealment, but lowers it by only one block after 200 stable
samples. An increase can deliberately rebuffer before the next block; the
target is clamped to 2-12 five-millisecond blocks (10-60 ms) and cannot affect
the admitted session, transport, identity, or input lifecycle. A separate
controller averages total jitter-plus-resampler source occupancy over 400 pump
samples, changes the source-consumption ratio by at most 50 ppm per window, and
clamps correction to ±1000 ppm. The streaming linear resampler retains no more
than four source blocks and still emits exact canonical blocks. Target/timing
discontinuity, reconnect, and endpoint recovery clear the correction; peer
timestamps never directly choose a ratio. Gain/mute remains separate work.

---

## 10. Local control API

The implemented local control surface is a current-user-only named pipe rather
than a LAN HTTP server. Its name is versioned and suffixed with the current
user SID. The transport uses an explicit one-SID DACL, rejects remote clients,
and verifies the opposite process token belongs to the same SID on both ends.

Frames carry a fixed magic/version, nonzero request ID, exact payload length,
and at most 128 payload bytes. Each connection carries one typed request and
one typed response, with bounded I/O waits and a response-consumption
acknowledgement before disconnect.

Example commands:

```text
GetState             implemented
SetDesiredMode       implemented
FocusMachine         typed, currently unsupported
SetAudioGain         implemented on an active Host peer receiver
ToggleAudioMute      implemented on an active Host peer receiver
```

The local API must not expose:

```text
SendRawTransportPacket
InjectArbitraryInput
ExecuteCommandLine
LoadArbitraryModule
```

This keeps a compromised Stream Deck plugin from automatically gaining the entire DeskLink trust boundary.

`SetDesiredMode` never bypasses the session state machine. On an Agent, a
local `Game` or `LockPc1` choice takes precedence over a remote peer's `Roam`
preference. Restrictive mode changes release DeskLink-owned input state and
disable active capture fail-locally.

Audio gain and mute are local render policy owned by the active Host's
`AudioReceiver`. They run after authenticated audio admission, jitter
buffering, concealment, and drift correction, immediately before WASAPI
submission. A transition ramps over one five-millisecond block. Audio endpoint
recovery preserves the configured per-peer policy; session teardown discards
it. These commands cannot alter transport, focus, or the Windows system mixer.

---

## 11. Future modules

Future capabilities should be explicit:

```text
core.state.read
input.send
input.inject
audio.send
audio.receive
clipboard.read
clipboard.write
system.sleep
system.wake
system.launch
file.send
file.receive
```

There should be no generic `remote.shell` capability in the normal architecture.

For app launching, prefer locally configured identifiers:

```text
LaunchApp("steam")
```

where `steam` resolves against an allowlist on the receiving PC.

---

## 12. Recommended process model

Initial implementation:

```text
User session
├── DeskLink.Host.exe        PC1 only
├── DeskLink.Agent.exe       remote-capable PCs
├── DeskLink.UI.exe
└── Stream Deck plugin
```

Do not introduce a SYSTEM service until a concrete requirement needs one.

If privileged functions later become necessary, add a small broker service with an allowlisted API. The broker should not own networking, hooks, UI, or arbitrary command execution.
