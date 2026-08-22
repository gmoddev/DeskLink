# DeskLink Implementation Status

## Status summary

This repository is a **tested foundation / vertical slice of the security-critical core**, not yet an end-user KM/audio application.

The current build proves the core invariants independently of Windows networking and device APIs.

---

## Implemented and validated

| Component | Status | Notes |
|---|---|---|
| C++20 core | Done | Portable |
| Protocol envelope | Done | 36-byte bounded envelope |
| Typed binary codec | Done | Big-endian, strict validation |
| Reliable/datagram separation | Done | Wrong-lane messages rejected |
| Capabilities | Done | Explicit bitset grants |
| Host focus coordinator | Done | Request/ready/renew/release packet generation |
| Agent authorization | Done | Capability + epoch + lease gating |
| Focus epochs | Done | Stale authority rejected |
| Focus leases | Done | Expiry returns local |
| Emergency fail-local | Done | Explicit state transition |
| Input cleanup contract | Done | Backend callback on failure/release |
| Windows SendInput injector | Done | Built only on Windows |
| Pointer format | Done | Absolute normalized datagram |
| PCM audio frame | Done | Bounded wire representation |
| Audio jitter buffer | Done | Reorder + bounded silence concealment |
| Transport abstraction | Done | Authentication/encryption metadata contract |
| Secure session binding | Done | Refuses insecure transport; binds session nonce |
| Manual pairing core | Done | Bounded window + canonical transcript + user-confirmed six-digit code |
| Peer certificate pinning | Done | Stored machine ID and SHA-256 certificate pin required by sessions |
| Windows trust provider | Done | CNG randomness/hash + current-user DPAPI atomic trust store |
| MsQuic endpoint adapter | Done | Optional v2.5.8 stream framing/datagram adapter for established connections |
| Windows device identity | Done | Current-user CNG key + self-signed SHA-256 certificate lifecycle |
| MsQuic connection bootstrap | Done | Registration/configuration, listener, client, deferred mutual pin validation |
| Pairing wire lane | Done | Separate ALPN, 153-byte ceiling, TLS-leaf binding, no 0-RTT |
| Connection rate limits | Done | Bounded per-address connection and pairing windows |
| Native MsQuic loopback | Done | Pair, confirm, reconnect, mutual pins, reliable packet |
| Trusted session nonce | Done | Fresh initiator nonce, pinned-TLS preface, reconnect rotation, no 0-RTT |
| Windows pairing control | Done | Five-minute window, native two-PC code prompt, explicit input grant |
| Manual focus control | Done | Trusted serve/focus commands, lease renewal, explicit release |
| End-to-end focus session | Done | FocusRequest -> FocusReady -> input over transport abstraction |
| In-memory transport | Done | Deterministic test adapter |
| Simulation CLI | Done | Host -> Agent focus/injection flow |
| Core regression tests | Done | Passing in supplied build environment |

---

## Current automated tests

The current test suite verifies:

1. pointer protocol round trip
2. wrong-lane packet rejection
3. oversized datagram rejection
4. missing capability blocks focus/injection
5. valid capability + lease permits input
6. lease expiry invokes cleanup
7. packets from expired epoch are rejected
8. refocus creates a new epoch
9. stale old-epoch key event is rejected
10. Host/Agent focus handshake semantics
11. emergency Host fail-local
12. audio reordering and missing-frame concealment
13. transport peer security metadata propagation
14. insecure transport refusal
15. end-to-end secure HostSession/AgentSession focus and stale-input rejection
16. out-of-order/duplicate pointer datagram rejection
17. stale FocusReady transaction rejection
18. fragmented and malformed pairing-offer frames
19. bounded/expiring connection-attempt limits
20. CNG device-certificate creation, reload, pin stability, and cleanup
21. native MsQuic pairing-to-trusted-session loopback on supported Windows CI

Build/test result in the creation environment:

```text
100% tests passed, 0 tests failed
```

---

## Production blockers

### P0 — required before using DeskLink across real PCs

- two-PC LAN pairing, cable-removal, and reconnect validation
- Windows Raw Input capture
- low-level suppression gate
- focus-ready response plumbing through the real transport
- current-user IPC

### P1 — required for useful KM roaming

- monitor enumeration/identity
- edge graph
- coordinate transform
- edge hysteresis
- automatic lease renew timer
- input state reconciliation snapshot implementation
- physical emergency chord
- foreground profile engine
- GAME capture teardown/reinstall lifecycle

### P1 — required for useful audio

- WASAPI loopback capture
- format conversion
- WASAPI render
- adaptive jitter target
- clock drift resampler
- endpoint-loss/recovery handling
- gain/mute state

### P2 — product surface

- UI
- Stream Deck plugin
- installer/update flow
- diagnostics/telemetry that never logs input content

---

## What should not be added prematurely

Do not add these to solve early implementation friction:

- generic remote shell
- arbitrary command execution API
- SYSTEM service owning the whole application
- permanent elevation
- virtual HID driver
- virtual audio driver
- cloud relay
- UPnP port forwarding
- dynamic in-process plugin loading from untrusted directories

Each one significantly expands the attack surface and is unnecessary for the first complete desk-to-desk implementation.

---

## Recommended next implementation slice

The pairing and focus controls now perform the trusted focus handshake. The next
slice should add raw input capture and make two real Windows PCs perform this
exact sequence:

```text
1. manually pair
2. authenticated MsQuic connection
3. PC1 presses a local focus hotkey
4. Agent grants a 750 ms focus lease
5. PC1 sends keyboard/mouse events
6. PC2 injects them with SendInput
7. Host renews lease periodically
8. hotkey returns focus to PC1
9. unplug Ethernet while Ctrl is held
10. both machines recover safely without stuck state
```

Do **not** add edge roaming until this manual focus switch survives failure injection reliably.
