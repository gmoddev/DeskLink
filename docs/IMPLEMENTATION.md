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

Host will not generate key/button/pointer packets unless a `FocusReady` has established a nonzero remote epoch. Focus requests carry a monotonically generated nonzero transaction ID, and stale responses cannot satisfy a newer pending request.

### `agent.hpp` / `agent.cpp`

Implements PC2-side authorization.

For input, the Agent requires:

```text
InputInject capability
AND current epoch
AND unexpired lease
```

Pointer datagrams additionally require a sequence newer than the last accepted pointer sequence for the current epoch.

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

The in-memory adapter proves these semantics. Native MsQuic should replace it without changing Host/Agent policy code.

### `input.hpp`

Defines `IInputInjector`.

The important cleanup contract is:

```cpp
release_owned_state()
```

A backend must release only state injected by DeskLink.

### `win32_input.hpp` / `win32_input.cpp`

Implements the current Windows injection backend using `SendInput`.

It tracks injected keyboard and mouse-button down state and emits matching releases during cleanup.

### `audio.hpp` / `audio.cpp`

Implements a bounded sequence-aware jitter buffer.

It:

- reorders future frames
- rejects late frames
- waits for a target amount of evidence before declaring a gap
- synthesizes silence for a confirmed missing frame
- limits buffered frame count

WASAPI capture/render and clock-drift correction are deliberately outside this class.

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

The Windows capture layer must wire the physical emergency chord directly to this behavior and stop suppressing local physical input immediately.

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

The highest-value next implementation is `MsQuicTransportEndpoint` plus a very small pairing/trust store.

Acceptance criteria:

1. Two Windows PCs pair only during an explicit pairing window.
2. Both display/confirm the same authentication string.
3. Peer identity survives restart.
4. An unpaired PC cannot create an accepted DeskLink session.
5. A paired PC with no `InputInject` grant cannot acquire focus.
6. An authenticated encrypted session can execute the existing HostSession -> AgentSession focus test over the LAN.
7. Ethernet removal while a key is held causes Agent cleanup after lease expiry.
8. Reconnect creates a fresh logical session nonce and old packets cannot regain authority.
