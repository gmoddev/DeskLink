# DeskLink Foundation Implementation

## 1. Implementation intent

This codebase implements the security-critical DeskLink foundation before committing the project to concrete Windows transport/capture/audio APIs.

The core assumption is:

> Transport authentication establishes who the peer is. Capability authorization establishes what the peer may do. A live lease/epoch establishes whether it may do that sensitive action now.

The first implemented sensitive action is remote input injection.

---

## 2. Source map

### `types.hpp`

Defines shared primitive types, peer identity metadata, payload ceilings, and the monotonic clock abstraction used to make lease behavior deterministic in tests.

### `capabilities.hpp`

Implements `CapabilitySet`, an explicit 64-bit capability grant mask.

The initial implementation has no wildcard/admin capability.

### `protocol.hpp` / `protocol.cpp`

Implements:

- strict 36-byte envelope
- big-endian integer encoding
- typed message variant
- bounded payload decoder
- reliable/datagram lane enforcement
- enum/range validation
- strict no-trailing-data parsing

### `focus.hpp` / `focus.cpp`

Implements the core input authority state machine:

```text
mode
focus location
epoch
lease expiry
```

It is independent of networking and Windows APIs.

### `host.hpp` / `host.cpp`

Implements PC1-side packet generation and remote focus tracking.

Host will not generate key/button/pointer/wheel packets unless a `FocusReady`
has established a nonzero remote epoch. Wheel generation additionally rejects
zero, unknown-axis, and out-of-range deltas before serialization. Focus
requests carry a monotonically generated nonzero transaction ID, and stale
responses cannot satisfy a newer pending request.

### `agent.hpp` / `agent.cpp`

Implements PC2-side authorization.

For input, the Agent requires:

```text
InputInject capability
AND current epoch
AND unexpired lease
```

Pointer datagrams additionally require a sequence newer than the last accepted pointer sequence for the current epoch.

Reliable mouse-wheel messages pass through the same capability, session,
epoch, and lease admission checks as keyboard and mouse-button input.

Failure invokes the input-backend cleanup contract where appropriate.

### `session.hpp` / `session.cpp`

Binds the portable core to an abstract transport.

The session layer refuses startup unless the transport reports:

```text
authenticated = true
encrypted = true
```

It also:

- binds incoming packets to the expected session nonce
- decodes each lane with the proper limits
- converts an accepted `FocusRequest` into `FocusReady`
- records decode/session/authorization rejection counters

The in-memory adapter proves these semantics. The optional MsQuic endpoint now
maps an established pinned connection into the same contract without changing
Host/Agent policy code.

### `input.hpp`

Defines `IInputInjector`.

The important cleanup contract is:

```cpp
release_owned_state()
```

A backend must release only state injected by DeskLink.

### `win32_input.hpp` / `win32_input.cpp`

Implements the current Windows injection backend using `SendInput`.

It tracks injected keyboard and mouse-button down state and emits matching
releases during cleanup. Bounded vertical/horizontal wheel events map to
`MOUSEEVENTF_WHEEL` and `MOUSEEVENTF_HWHEEL` and create no held state.

### `audio.hpp` / `audio.cpp`

Implements exact V1 audio block assembly, a bounded sequence-aware jitter
buffer, and the thread-safe receiver/render pump.

It:

- accepts only whole PCM16 stereo source frames in bounded pushes
- assembles exact 48 kHz, 240-frame, 5 ms wire blocks
- produces explicit silence blocks and resets partial state on discontinuity
- reorders future frames
- rejects late frames
- waits for a target amount of evidence before declaring a gap
- synthesizes silence for a confirmed missing frame
- estimates arrival variation from capture/arrival timestamp deltas
- raises a bounded playout target immediately and lowers it with hysteresis
- enters an explicit rebuffer state when a larger target must add latency
- tracks sustained total source occupancy around the adaptive target
- applies linear asynchronous resampling in 50 ppm steps, capped at ±1000 ppm
- retains at most four source blocks and emits only canonical 240-frame blocks
- clears correction state on timing, target, session, and endpoint discontinuity
- limits buffered frame count
- fixes one nonzero stream ID for each admitted receiver
- fails the audio receiver closed when the render callback rejects a block

### `win32_audio.hpp` / `win32_audio.cpp`

Implements an event-driven shared-mode WASAPI foundation. The capture adapter
opens the default render endpoint with the loopback and audio-engine conversion
flags, drains every complete capture packet, respects silent/timestamp-error/
discontinuity flags, and publishes exact V1 blocks. The render adapter requests
the same canonical format, pre-rolls silence, accepts at most 64 validated
blocks, and fills underruns with silence. Each adapter owns COM and WASAPI
objects on its worker thread, registers a scoped `IMMNotificationClient` for
the selected/default render endpoint, and exposes restart-safe
start/stop/typed-failure state.

`AgentSession` sends these blocks only when the stored peer grant includes
`AudioReceive`. `HostSession` admits them only when the stored sender grant
includes `AudioSend`, the current session nonce matches, and the receiver
accepts the exact format/stream/sequence. The production CLI exposes explicit
`--send-audio` and `--receive-audio` switches; neither capability silently
starts an endpoint. Recoverable endpoint failures schedule reopen without
reconnecting the session: retry begins after 250 ms and doubles to a five-second
cap while audio remains explicitly enabled. A capture callback/send rejection
is non-recoverable and stops capture rather than retrying an authorization or
transport failure. The receiver's adaptive target is 2-12 blocks (10-60 ms):
arrival spikes and concealment raise it immediately, while 200 stable samples
are required for each one-block decrease. Backward/reordered timestamp samples
do not update the estimator. A separate occupancy controller observes 400
five-millisecond pump samples before each 50 ppm correction step, clamps the
linear asynchronous resampler to ±1000 ppm, and resets on timing/target/session/
endpoint discontinuity. Peer timestamps do not directly set its ratio. The
active Host applies bounded per-peer attenuation/mute after drift correction,
with one-block ramps and settings preserved through audio-only recovery. Audio
failure and render policy have no input-mode authority.

### `clipboard.hpp` / `clipboard.cpp`

Implements the portable text-clipboard module handshake, complementary
`ClipboardRead`/`ClipboardWrite` admission, strict UTF-8 and 48 KiB bounds,
session-nonce and authenticated-origin binding, monotonic update IDs, and a
20-per-second send/receive gate. `PeerSession` starts it only by explicit option
after authenticated transport admission. It sends no text until both peers
exchange the canonical module hello, which keeps older protocol-v2 peers
fail-closed. Callback rejection or exception increments clipboard-only
statistics and cannot alter direction arbitration, focus, leases, or input.

### `win32_clipboard.hpp` / `win32_clipboard.cpp`

Implements a current-user, message-only Windows clipboard listener for
`CF_UNICODETEXT`. It converts with strict Windows UTF-8 flags, uses bounded
clipboard-open attempts, caps pending remote writes at eight, owns all content
only in process memory, and suppresses an applied remote update by exact
clipboard sequence plus exact text. Publication remains disabled until the
portable session reports complete mutual consent and negotiation. The CLI and
wrapper expose explicit pairing grants and `--sync-clipboard`; no clipboard
option is enabled by default, and failures remain confined to this adapter.

### `transport.hpp` / `in_memory_transport.cpp`

Defines the transport contract and deterministic in-memory adapter.

The contract exposes authenticated/encrypted peer metadata because the application authorization layer must never have to guess whether the underlying connection is trusted.

---

## 3. End-to-end focus path implemented today

The tested in-memory path is:

```text
HostSession.focus_remote()
        ↓
HostCoordinator creates FocusRequest
        ↓
ITransportEndpoint reliable send
        ↓
AgentSession bounded decode
        ↓
session nonce check
        ↓
AgentCoordinator capability check
        ↓
InputFocusStateMachine creates lease + epoch
        ↓
AgentSession sends FocusReady(epoch)
        ↓
HostSession receives response
        ↓
HostCoordinator stores remote epoch
        ↓
HostSession.send_key()/send_pointer()
        ↓
AgentSession bounded decode
        ↓
capability + epoch + lease checks
        ↓
IInputInjector
```

That path is already regression-tested.

---

## 4. Failure path implemented today

### Lease timeout

```text
no renewals
    ↓
AgentSession.tick()
    ↓
InputFocusStateMachine.poll_expiry()
    ↓
focus becomes LOCAL
    ↓
epoch advances
    ↓
release_owned_state()
```

A later packet from the old Host epoch is rejected.

### Capability revocation

Calling `AgentCoordinator::set_peer_capabilities()` without `InputInject`:

```text
release remote focus
release owned injected state
reject later input
```

### Disconnect

`AgentCoordinator::disconnect()` releases focus and owned state.

### Emergency Host action

`HostCoordinator::emergency_fail_local()`:

```text
desired mode = LOCK_PC1
remote epoch = 0
```

The Windows capture layer wires Ctrl+Alt+Pause directly to this behavior and
stops suppressing local physical input before notifying the control worker.

### Coordinated update

`UpdateCoordinator` requires successful package validation before the first
lifecycle operation. It then orders `Return Local -> confirm no focus/capture ->
request runtime shutdown -> wait runtime -> request UI shutdown -> wait UI ->
install -> validate`. Backend exceptions become failures rather than escaping
the coordinator. Once candidate installation has been attempted, every install
or health failure enters rollback; rollback install/validation failure prevents
restart. The Windows backend binds that state machine to the same-SID control
pipe, named lifecycle gates, timestamped same-signer installers, a bounded Job
object, registered version checks, and an installed Alpha self-test.

---

## 5. Why the implementation does not contain a generic message bus

DeskLink protocol types are explicit.

There is intentionally no message equivalent to:

```text
Invoke(methodName, arbitraryJson)
Execute(command)
LoadPlugin(bytes)
```

Every new privileged operation should receive:

- a typed message
- a capability requirement
- size/range validation
- ownership definition
- failure behavior
- regression tests

This is slower than a generic RPC surface but materially safer for a long-running local-control agent.

---

## 6. Native MsQuic integration contract

A production `MsQuicTransportEndpoint` must implement the existing `ITransportEndpoint` contract.

At minimum it must map:

```text
send_reliable()
    -> a dedicated reliable QUIC stream/lane

send_datagram()
    -> QUIC DATAGRAM

peer_info()
    -> authenticated pinned DeskLink peer identity
```

Transport callbacks must hand already-bounded receive chunks to the session layer. The session layer remains responsible for DeskLink wire validation.

Recommended MsQuic configuration:

- TLS 1.3
- stock Schannel on Windows 11/Server 2022 or newer
- existing non-exportable current-user CNG identity
- peer certificate/public-key pinning after pairing
- datagram receive enabled
- 0-RTT disabled for privileged actions initially
- one logical connection per peer pair
- independent logical reliable streams for control/state/input where useful
- application-level packet ceilings retained even though QUIC itself has flow control

---

## 7. Windows Host capture contract

The missing Host capture backend should produce typed internal events, not DeskLink packets directly.

Recommended internal interface:

```text
PhysicalKey
PhysicalButton
PhysicalPointer
ForegroundChanged
DisplayTopologyChanged
EmergencyChord
```

A routing worker then decides whether to:

```text
pass local
suppress + forward remote
trigger focus crossing
activate GAME mode
```

This keeps network state out of Windows hook callbacks.

---

## 8. WASAPI integration contract

### Sender

```text
WASAPI loopback capture
    ↓
normalize 48k stereo
    ↓
5 ms block
    ↓
PCM16 conversion
    ↓
AudioFrameMessage
    ↓
QUIC DATAGRAM
```

### Receiver

```text
AudioFrameMessage
    ↓
AudioJitterBuffer
    ↓
loss concealment
    ↓
clock drift resampling
    ↓
per-peer gain
    ↓
WASAPI shared render
```

Audio has no authority over input mode. Module failures are isolated.

The implemented sender and receiver are not keyed to the input focus epoch.
They are independently authorized capabilities within the same fresh,
`PeerValidated` session nonce. This permits audio to continue without granting
or holding input focus while still rejecting stale-session datagrams.

---

## 9. Configuration ownership

Suggested split:

```text
trust.db
    peer public identity
    capability grants
    protected by current-user ACL + protected secret storage

config.json
    machine display name
    monitor adjacency
    profiles
    audio gains
    UI preferences

runtime state
    not persisted as truth
    connection/focus/lease/current foreground/etc.
```

Do not persist an active focus lease across process restart.

---

## 10. Recommended next code change

The endpoint, device identity, pairing wire lane, MsQuic bootstrap, fresh
session-nonce negotiation, explicit Windows pairing control, reconciliation,
stable monitor mapping, bounded wheel transport, current-user control pipe, and
bounded native Windows DNS-SD/mDNS discovery are implemented. Discovery emits
only untrusted candidates and cannot connect, pair, grant capabilities, or
write trust. The bounded foreground profile evaluator, native WinEvent
observation, and production Host CLI/live event runtime are now
implemented. One serialized lifecycle owner handles profile/manual decisions,
`FocusReady`, renewal/reconciliation, emergency release, and capture failures.
It enforces fail-local GAME/LOCK_PC1 transitions, fresh-focus capture admission,
and restart-safe Win32 capture teardown/recreation. The bounded WASAPI
capture/render foundation, exact V1 block assembler, complementary audio grants,
session/datagram admission, receiver jitter/render pump, audio-only endpoint
recovery, bounded adaptive jitter targeting, and bounded asynchronous clock-
drift correction and per-peer gain/mute are now implemented. Roaming Phase 1
adds checked display presentation metadata, a bounded directional edge graph,
stable-identity resolution against current topology generations, and atomic
current-user preferences. Canvas placement and physical size remain
presentation-only and are absent from route resolution. Roaming Phase 2 adds an
explicit pairing grant and bounded reliable topology snapshots after trusted
session admission. Expected peer machine, current envelope/payload nonce,
canonical descriptors, monotonic generation, and a five-second freshness lease
all gate separate route readiness. Phase 3 now adds the presentation-only native
configurator, offline display/route state, explicit adjacency suggestions,
advanced directional edge editing, atomic Local-before-save replacement,
five-second Identify overlays, and the companion tray/startup lifecycle.
Phase 4 now adds an explicitly enabled reciprocal crossing slice: local Raw Input
observation without suppression, three bounded intent policies, proportional
corner-safe landing, cooldown, a focus timeout, active-route/session
invalidation, and fresh-focus/landing/snapshot/direction gates before the
existing lifecycle may suppress. One `PeerSession` owns inbound and outbound
coordinators on the validated connection, exchanges immutable per-direction
persisted grants without mutating trust, binds direction tokens to the peer and
nonce, and converges collision/duplicate/reconnect paths to Local. The full
Windows 11 physical qualification matrix remains outstanding.
The opt-in low-level keyboard, Raw Input mouse, and suppression backend now
supplies the physical input path. Periodic reliable input-state snapshots now
converge DeskLink-owned normal/extended scan-code and mouse-button holds under
the active lease and epoch. The remaining two-PC failure injection on real
Windows 11 systems is deferred until a second Windows 11 target is available.
Stable multi-monitor mapping is implemented using active DisplayConfig target
paths, deterministic nonzero display IDs, rectangle transforms, and
generation-based stale-topology rejection. Bounded reliable mouse-wheel
transport is implemented with enqueue-before-suppress fail-local capture.

The first installation slice is also implemented as a fixed-path current-user
package. It checks Alpha/runtime lifecycle mutexes before replacing files,
accepts only the exact staged Schannel payload, preserves the device identity,
trust, and preferences outside installer ownership, and removes the optional
Run value on uninstall. CI proves unsigned development install, in-place
upgrade, state preservation, and uninstall. Production output remains blocked
until an explicit current-user Authenticode certificate and timestamp service
can sign and verify the application executables, uninstaller, and Setup.

Acceptance criteria:

1. Two Windows PCs pair only during an explicit pairing window.
2. Both display/confirm the same authentication string.
3. Peer identity survives restart.
4. An unpaired PC cannot create an accepted DeskLink session.
5. A paired PC with no `InputInject` grant cannot acquire focus.
6. An authenticated encrypted peer session can execute focus in either direction while admitting at most one direction at a time.
7. Ethernet removal while a key is held causes Agent cleanup after lease expiry.
8. Reconnect creates a fresh logical session nonce and old packets cannot regain authority.
