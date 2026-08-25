# DeskLink Wire Protocol V2

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
| version | 2 | protocol version, currently `2` |
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
```

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
20-byte header contains `DLCT` magic, version `1`, request/response frame type,
a nonzero 64-bit request ID, and an exact 32-bit payload length. Payloads are
limited to 128 bytes and trailing data is rejected.

Request command numbers are:

| Value | Command | Payload | Current behavior |
|---:|---|---|---|
| 1 | `GetState` | empty | returns bounded typed runtime state |
| 2 | `SetDesiredMode` | one validated `DeskMode` byte | applies through the existing focus/session state machines |
| 3 | `FocusMachine` | one nonzero 16-byte machine ID | `Unsupported` until persistent host orchestration exists |
| 4 | `SetAudioGain` | unsigned permyriad, `0..10000` | applies bounded gain to the active Host peer receiver; otherwise `NotReady` |
| 5 | `ToggleAudioMute` | empty | toggles mute on the active Host peer receiver; otherwise `NotReady` |

Responses contain a bounded status and an optional typed state record. A state
record includes local/focused machine IDs, role, desired mode, peer count,
current per-peer audio gain/mute, focus, and capture state. Cross-field validation
rejects impossible combinations such as active capture without Host remote
focus. Each connection exchanges exactly one request/response and completes
with a fixed acknowledgement byte so the server does not disconnect before the
client consumes the response.
