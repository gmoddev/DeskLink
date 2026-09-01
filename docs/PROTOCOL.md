# DeskLink Wire Protocol V3

## 1. Scope

This document describes the portable binary protocol implemented in `protocol.cpp`.

The transport is responsible for encryption/authentication. The DeskLink protocol is responsible for message identity, session binding, sequence metadata, epoch binding, validation, and bounded payload semantics.

All integer fields use network byte order (big-endian).

---

## 2. Envelope

Every message has a 36-byte envelope:

| Field | Size | Description |
|---|---:|---|
| magic | 4 | `DLNK` / `0x444C4E4B` |
| version | 2 | protocol version, currently `3` |
| message_type | 2 | `MessageType` |
| payload_size | 4 | bytes following envelope |
| session_nonce | 8 | local logical session identifier |
| epoch | 8 | focus/authority generation where applicable |
| sequence | 8 | lane-local sequence number |

The decoder rejects trailing bytes and truncated payloads.

---

## 3. Lane policy

### Reliable ordered stream

```text
Hello
CapabilityGrant
CapabilityGrantAck
SetMode
FocusRequest
FocusReady
FocusRenew
FocusRelease
KeyEvent
MouseButton
InputStateSnapshot
MouseWheel
SetAudioGain
Heartbeat
DisplayTopologySnapshot
ClipboardHello
ClipboardText
```

### QUIC DATAGRAM

```text
PointerPosition
PointerMotion
AudioFrame
```

`Heartbeat` is permitted as a special case on either lane in the current codec.

Using a message on the wrong lane is a protocol error.

---

## 4. Message types

### Hello — type 1

```text
machine_id             16 bytes
min_version             u16
max_version             u16
offered_capabilities    u64
```

### CapabilityGrant — type 2

```text
capabilities            u64
revision                u64
```

For a reciprocal `PeerSession`, this message is sent only after
`PeerValidated` and fresh session-nonce negotiation. It reports the exact
capabilities in the sender's persisted local trust record for that peer; it is
not an offer and cannot modify either trust store. Revision 1 establishes the
initial grant. An exact replay is harmless; each later update must use exactly
the next revision and immediately fails both input directions Local before the
new grant is used. Unknown bits, skipped/conflicting revisions, or a changed
replay invalidate grant state and fail closed. The sender treats an update as
complete only after receiving the exact matching acknowledgement. A
peer-reported value can gate this machine's outbound input attempt, but it
never authorizes disclosure of this machine's audio, topology, or clipboard;
those remain gated by this machine's persisted local grant.

### CapabilityGrantAck — type 3

```text
capabilities            u64
revision                u64
```

The acknowledgement must exactly match the sender's current grant and
revision. A future, conflicting, malformed, or otherwise invalid
acknowledgement fails both directions Local. A stale acknowledgement is
ignored and cannot complete an update. If the bounded acknowledgement wait
expires, the broker stops that peer session fail-locally rather than assuming
the permission change took effect.

### SetMode — type 10

```text
mode                    u8
```

Values:

```text
0 ROAM
1 LOCK_PC1
2 LOCK_PC2
3 GAME
```

### FocusRequest — type 11

```text
requested_lease_ms      u32
request_id              u64
```

`request_id` must be nonzero. Wire validation permits 100..5000 ms. Receiver policy may clamp lower.

### FocusReady — type 12

```text
granted_lease_ms        u32
request_id              u64
```

`request_id` must match the outstanding `FocusRequest`. The authoritative new focus epoch is carried in the envelope `epoch` field.

### FocusRenew — type 13

```text
requested_lease_ms      u32
```

Envelope epoch must match the currently active focus epoch.

### FocusRelease — type 14

No payload. Envelope epoch must match active focus.

### KeyEvent — type 20

```text
scan_code               u16
extended                u8 bool
is_down                 u8 bool
```

The Windows backend uses scan-code injection rather than translated text.

### MouseButton — type 21

```text
button                  u8
is_down                 u8 bool
```

Buttons:

```text
1 left
2 right
3 middle
4 X1
5 X2
```

### PointerPosition — type 22

```text
display_id              u16
normalized_x            u16
normalized_y            u16
```

Pointer position is absolute and represents the latest desired position. This
is intentionally tolerant of datagram loss. Display ID zero preserves the
legacy whole-virtual-desktop interpretation. A nonzero ID selects a stable
target-display descriptor; receivers reject an unknown ID or a mapping pinned
to a stale local topology generation.

### InputStateSnapshot — type 23

```text
key_bitmap_normal       32 bytes
key_bitmap_extended     32 bytes
mouse_button_bitmap      1 byte
```

Each key bitmap represents scan codes 0..255; scan code zero is reserved and
must remain clear. The two bitmaps distinguish ordinary and Windows extended
scan-code events. Mouse-button bits 0..4 represent Left, Right, Middle, X1, and
X2; bits 5..7 are reserved and must remain zero.

The Host sends this message periodically on the reliable lane under the current
focus epoch. It is the authoritative state for DeskLink-routed input. The Agent
first releases DeskLink-owned holds absent from the snapshot, then presses
missing holds. Capability, session, active-lease, and epoch validation must
succeed before reconciliation. A snapshot never adopts or releases input that
DeskLink does not own.

### MouseWheel — type 24

```text
axis                     u8
delta                    i16
```

Axes are `1` for vertical and `2` for horizontal. Delta uses network byte
order, must be nonzero, and is bounded to `-1200..1200`. Positive and negative
values retain Windows' native vertical/horizontal wheel direction semantics.
Wheel events use the reliable ordered lane because deltas are cumulative and
must not be dropped or reordered.

### PointerMotion — type 25

```text
delta_x                  i32
delta_y                  i32
```

Ordinary Raw Input movement uses signed relative device counts rather than a
coordinate normalized to the controlling PC's virtual desktop. At least one
axis must be nonzero and each axis is bounded to `-1000000..1000000`.
`PointerPosition` remains the absolute display-aware message for explicit
monitor transitions and resynchronization. Both pointer message types share
one monotonic datagram sequence gate for the current focus epoch.

### SetAudioGain — type 30

```text
gain_permyriad          u16
```

Range 0..10000.

### AudioFrame — type 31

```text
stream_id               u32
sample_rate             u32
frames_per_channel      u16
channels                 u8
bytes_per_sample         u8
capture_timestamp_us    u64
pcm                      remaining payload
```

V2 recommendation:

```text
sample_rate = 48000
frames_per_channel = 240
channels = 2
bytes_per_sample = 2
PCM payload = 960 bytes
```

### Heartbeat — type 40

No payload.

### DisplayTopologySnapshot — type 50

```text
machine_id                      16 bytes
payload_session_nonce            u64
topology_generation              u64
virtual_left                     i32
virtual_top                      i32
virtual_right                    i32
virtual_bottom                   i32
display_count                    u16

repeated display_count times:
  display_id                     u16
  stable_identity_length         u16
  stable_identity                bytes
  friendly_name_length           u16
  friendly_name                  bytes
  left/top/right/bottom          4 × i32
  primary                        u8 bool
  pixel_width                    u32
  pixel_height                   u32
  refresh_millihertz             u32
  physical_width_mm              u16
  physical_height_mm             u16
  physical_size_source           u8
  orientation                    u8
```

This message is reliable-only and informational. It cannot grant a capability,
request focus, inject input, or mutate the persisted roaming graph. The decoder
requires one to 64 canonical, collision-free descriptors, one primary display,
exact virtual bounds, bounded strings/dimensions/enums, exact framing, and an
aggregate payload no larger than the 64 KiB reliable limit.

Session admission additionally requires an authenticated/encrypted trusted
peer, the explicit `DisplayTopologyExchange` trust grant, an envelope nonce and
payload nonce equal to the current session nonce, and a sender machine ID equal
to the stored peer identity. A lower generation is stale and does not extend
freshness. A changed snapshot at the same generation is rejected. Accepted
snapshots expire after five seconds unless an identical or newer canonical
snapshot refreshes them; the Windows runtime publishes at most every two
seconds and immediately after a material topology generation change. Timeout,
malformation, wrong identity/nonce, or rejection can never produce a
route-ready state. Rejected state requires a fresh authenticated connection.

### ClipboardHello — type 60

```text
clipboard_version                u16 (exactly 1)
maximum_text_bytes               u32 (exactly 49152)
```

This canonical reliable-only handshake prevents protocol-v2 peers that do not
implement this module from receiving clipboard text merely because the older
capability bit exists. It is sent only after `PeerValidated`, nonce negotiation,
immutable capability exchange, explicit session opt-in, and at least one
complementary clipboard direction. Text remains blocked until both peers have
sent and admitted this exact handshake.

### ClipboardText — type 61

```text
origin_machine_id                16 bytes
update_id                        u64
text_byte_length                 u32
text_utf8                        text_byte_length bytes
```

The message is reliable-only. `update_id` is nonzero and strictly increasing
for one sender and session nonce; it resets only for a fresh authenticated
session. Text is strict UTF-8 without embedded NUL and is bounded to 48 KiB.
Admission requires the current envelope nonce, the authenticated peer's exact
machine ID, the module handshake, explicit opt-in, and complementary grants:
the sender's local `ClipboardRead` plus the receiver-reported
`ClipboardWrite`. The receiver independently requires its local
`ClipboardWrite` plus the sender-reported `ClipboardRead`. Stale, replayed,
wrong-peer, wrong-session, malformed, and faster-than-20-per-second updates are
rejected without changing focus or input state.

---

## 5. Focus transaction

```text
Host                              Agent
  │                                 │
  │ FocusRequest(epoch=0, id=R)     │
  ├────────────────────────────────>│
  │                                 │ authorize InputInject
  │                                 │ allocate new epoch E
  │                                 │ set lease expiry
  │ FocusReady(epoch=E, id=R)       │
  │<────────────────────────────────┤
  │                                 │
  │ KeyEvent(epoch=E)               │
  ├────────────────────────────────>│
  │ PointerMotion(epoch=E)          │
  ╞════════════════════════════════>│
  │ FocusRenew(epoch=E)             │
  ├────────────────────────────────>│
```

On lease expiry the Agent invalidates E.

---

## 6. Session nonce

The V2 envelope includes a 64-bit logical session nonce. It is generated from
a cryptographically strong random source when a DeskLink logical session is
established and checked consistently by the Host/Agent session binding layer.

It does not replace QUIC/TLS anti-replay or authentication.

---

## 7. Versioning policy

V2 is strict. Unknown message types and unsupported envelope versions are rejected.

Future compatibility should be introduced deliberately, preferably using:

- negotiated protocol min/max in `Hello`
- new message type numbers
- feature/capability negotiation
- explicit optional extension blocks rather than silently accepting unknown trailing data

---

## 8. Allocation policy

The codec's hard bounds are part of the security contract.

Production adapters must not read an attacker-provided length and allocate arbitrary memory before applying these bounds.

---

## 9. Realtime sequence semantics

Pointer datagram sequence numbers are monotonic within a focus epoch. Absolute
position and relative motion messages share the same newest-accepted sequence.
The Agent rejects an older or duplicate pointer packet, preventing reordered
motion from being applied after a newer position or motion event.

Audio sequence numbers are consumed by the jitter buffer, which intentionally supports limited reordering rather than strict newest-only semantics.

---

## 10. Local control protocol

The Windows named-pipe control surface uses a separate local-only binary
protocol implemented in `control.cpp`; it is never carried over QUIC. Its
20-byte header contains `DLCT` magic, version `7`, request/response frame type,
a nonzero 64-bit request ID, and an exact 32-bit payload length. Payloads are
limited to 512 KiB for bounded topology/device responses and trailing data is
rejected.

Request command numbers are:

| Value | Command | Payload | Current behavior |
|---:|---|---|---|
| 1 | `GetState` | empty | returns bounded typed runtime state |
| 2 | `SetDesiredMode` | one validated `DeskMode` byte | applies through the existing focus/session state machines |
| 3 | `FocusMachine` | one nonzero 16-byte machine ID | immediately requests the exact active authenticated peer through normal focus admission |
| 4 | `SetAudioGain` | unsigned permyriad, `0..10000` | applies bounded gain to the active Host peer receiver; otherwise `NotReady` |
| 5 | `ToggleAudioMute` | empty | toggles mute on the active Host peer receiver; otherwise `NotReady` |
| 6 | `GetDisplayTopologies` | empty | returns the bounded read-only current topology view |
| 7 | `PrepareForUpdate` | empty | repeats fail-local mode application and schedules orderly local runtime shutdown |
| 8 | `GetProductPreferences` | empty | returns strictly decoded current-user product policy |
| 9 | `SetProductPreferences` | bounded typed preferences | saves validated policy and reconciles the supervised runtime after fail-local cleanup |
| 10 | `ListTrustedDevices` | empty | returns at most 64 peer machine/name/grant records, never pins or secrets |
| 11 | `RequestLocalPermissionChange` | exact machine and capability mask | permits reduction after cleanup; additions return `ReauthorizationRequired` |
| 12 | `ForgetTrustedDevice` | exact machine | stops the affected owner fail-locally before removing trust |
| 13 | `ReturnLocal` | empty | confirms no remote focus/capture |
| 14 | `GetPairingCandidate` | empty | returns only the current bounded, expiring local candidate lease |
| 15 | `PauseDeskLink` | empty | stops the managed child through fail-local cleanup and suppresses reconnect |
| 16 | `ResumeDeskLink` | empty | clears pause/action state and restarts from Local policy |
| 17 | `StartDiscovery` | duration `1..30` seconds | starts one bounded broker-owned native browse |
| 18 | `GetNearbyPeers` | empty | returns phase plus at most 64 untrusted structured results |
| 19 | `StopDiscovery` | empty | invalidates the current result generation |
| 20 | `OpenPairingWindow` | port and local grant mask | stops the ordinary runtime Local and opens one five-minute managed listener |
| 21 | `PairNearbyPeer` | exact cached machine and local grant mask | requires a unique, open, compatible cached record |
| 22 | `PairManualAddress` | bounded host, port, and local grant mask | enters the same managed cryptographic pairing lane |
| 23 | `ResolvePairingCandidate` | operation ID and approve/reject bit | resolves only the current expiring local candidate |
| 24 | `PresentManagedPairingCandidate` | internal token, operation, identity binding, code, transcript, and grants | accepted only from the matching broker-launched operation; never returned to ordinary UI clients |
| 25 | `GetManagedPairingDecision` | internal token and operation ID | returns pending/approved/rejected and fails closed on shell loss or expiry |
| 26 | `GetPermissionCandidate` | empty | returns only the current bounded, expiring local permission candidate |
| 27 | `ResolvePermissionCandidate` | operation ID and approve/reject bit | resolves only the current permission candidate and persists additions after protected review |
| 28 | `GetPairingOperation` | empty | returns waiting, verification, peer-wait, success, cancel, timeout, or failure for the latest operation |
| 29 | `RefreshTrustedPeerCapabilities` | exact machine ID | internal broker-to-child grant refresh with exact peer acknowledgement |
| 30 | `ApplyManagedPreferences` | bounded typed preferences | internal broker-to-child live module/profile update; ordinary broker clients receive `Unsupported` |

Responses contain a bounded status and an optional typed state record. A state
record includes local/focused machine IDs, role, desired mode, peer count,
current per-peer audio gain/mute, focus/capture state, retry attempt, typed
runtime phase, and typed failure class. Cross-field validation
rejects impossible combinations such as active capture without Host remote
focus. Each connection exchanges exactly one request/response and completes
with a fixed acknowledgement byte so the server does not disconnect before the
client consumes the response.

Commands 24-25 and 29-30 are internal child bridges on the same current-user
transport, not general trust primitives. The pairing bridge's fresh 128-bit token is generated by the
broker and passed only to the fixed sibling process. This does not claim hostile
same-user isolation, but it prevents an ordinary client from presenting or
polling a pairing operation accidentally. Fingerprint and transcript data are
used only to revalidate the candidate's machine binding inside the broker; the
shell receives neither.
