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
| Windows physical input capture | Done | Low-level non-injected keyboard scan codes plus Raw Input mouse, bounded 1024-event queue, pointer coalescing |
| Low-level suppression gate | Done | Atomic route flag, injected-event pass-through, Ctrl+Alt+Pause/Break fail-local |
| Input-state reconciliation | Done | Reliable 500 ms snapshots; normal/extended keys and five buttons; owned-state convergence |
| Pointer format | Done | Absolute normalized datagram |
| Stable multi-monitor mapping | Done | Active DisplayConfig target identities; deterministic nonzero IDs; negative-origin rectangle transform; topology-generation invalidation |
| Mouse-wheel transport | Done | Reliable ordered axis + signed delta; `-1200..1200` bound; enqueue-before-suppress fail-local hook path |
| PCM audio frame | Done | Bounded wire representation |
| Audio jitter buffer | Done | Reorder + bounded silence concealment |
| Transport abstraction | Done | Authentication/encryption metadata contract |
| Secure session binding | Done | Refuses insecure transport; binds session nonce |
| Manual pairing core | Done | Bounded window + canonical transcript + user-confirmed six-digit code |
| Peer certificate pinning | Done | Stored machine ID and SHA-256 certificate pin required by sessions |
| Windows trust provider | Done | CNG randomness/hash + current-user DPAPI atomic trust store |
| MsQuic endpoint adapter | Done | Optional v2.6.0 stream framing/datagram adapter for established connections |
| Production platform baseline | Done | Windows 11/Server 2022+ with stock Schannel; Windows 10 unsupported |
| MsQuic runtime selection | Done | Application-owned path, pinned hash/version/provider, fail-closed unavailable provider |
| Windows 10 opaque CNG prototype | R&D Stage 2 | Explicit built-in OpenSSL 3.5 provider; exportable and mismatched credentials rejected; not production-admitted |
| Fail-closed peer admission | R&D Stage 3 | `PeerValidated` gates CONNECTED effects, streams, datagrams, pairing/session delivery, and endpoint traffic; four-second watchdog |
| CNG identity invariance | R&D Stage 4 | Exact before/after snapshots cover key/provider/algorithm/export policy/public key/certificate hash/pin; build rejects private-key export APIs |
| Windows device identity | Done | Current-user CNG key + self-signed SHA-256 certificate lifecycle |
| MsQuic connection bootstrap | Done | Registration/configuration, listener, client, deferred mutual pin validation |
| Pairing wire lane | Done | Separate ALPN, bounded offer/confirmation frames, TLS-leaf binding, mutual confirmation before persistence, no 0-RTT |
| Connection rate limits | Done | Bounded per-address connection and pairing windows |
| Native MsQuic loopback | Done | Pair, confirm, reconnect, mutual pins, reliable packet |
| Trusted session nonce | Done | Fresh initiator nonce, pinned-TLS preface, reconnect rotation, no 0-RTT |
| Windows pairing control | Done | Five-minute window, native two-PC code prompt, explicit input grant |
| Manual focus control | Done | Trusted serve/focus commands, lease renewal, explicit release |
| Windows 10 physical compatibility | R&D Stage 5 complete | Pairing, reconnect, nonce rotation, physical forwarding, `SendInput`, snapshot recovery, emergency release, process termination, scoped network interruption, and live stale epoch/session rejection passed |
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
22. suppression gate pass/suppress/injected-event/emergency behavior
23. opt-in Raw Input window and hook install/remove smoke test on the Windows worker
24. input snapshot codec validation, including extended keys and reserved bits
25. release-before-press reconciliation ordering
26. end-to-end snapshot authorization and expired-lease rejection
27. OpenSSL/CNG pairing, trusted reconnect, nonce rotation, and reliable traffic
28. exportable CNG credential and certificate/key mismatch rejection
29. pinned OpenSSL MsQuic, libcrypto, and libssl tamper rejection
30. missing, malformed, expired, and not-yet-valid certificate-DER rejection
31. wrong pin, unknown peer, validator failure/exception/timeout, and changed-identity rejection with zero application sessions
32. deterministic CNG RSA-PSS signing failure and OpenSSL credential rejection
33. stable display IDs independent of enumeration order and friendly-name changes
34. per-display normalized mapping across negative-origin virtual desktops
35. topology rectangle changes invalidate stale generations
36. duplicate identities and 16-bit display-ID collisions fail closed
37. live Windows DisplayConfig enumeration when an active desktop is available
38. mouse-wheel codec round trip, wrong-lane rejection, axis/delta bounds, and end-to-end focus admission

Build/test result in the creation environment:

```text
100% tests passed, 0 tests failed
```

---

## Production blockers

### P0 — required before using DeskLink across real PCs

- two-PC Windows 11 LAN pairing, cable-removal, and reconnect validation
  (deferred until a second Windows 11 target is available)
- current-user IPC

### P1 — required for useful KM roaming

- physical emergency-chord and high-poll-rate timing validation
- edge graph
- edge hysteresis
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

The pairing, focus, opt-in capture controls, and periodic state reconciliation
now implement the first complete input path. The next slice is validation on
two real Windows 11 PCs using this exact sequence:

```text
1. manually pair
2. authenticated MsQuic connection
3. PC1 presses a local focus hotkey
4. Agent grants a 750 ms focus lease
5. PC1 sends keyboard/mouse events
6. PC2 injects them with SendInput
7. Host renews lease periodically
   and sends the authoritative key/button snapshot
8. hotkey returns focus to PC1
9. unplug Ethernet while Ctrl is held
10. both machines recover safely without stuck state
```

Do **not** add edge roaming until this manual focus switch survives failure injection reliably.

Snapshot reconciliation and stable multi-monitor mapping are complete. The real
Windows 11 two-PC failure matrix is deferred until a second Windows 11 target is
available. Bounded mouse-wheel transport is the next implementation milestone.
See [`ROADMAP.md`](ROADMAP.md).

## Experimental compatibility work

Windows 10 remains unsupported. An approved equal-security R&D project now
tracks MsQuic 2.6.x, OpenSSL 3.5 LTS, and an opaque CNG provider integration as
separately reviewable stages. The production architecture remains stock
MsQuic/Schannel with the existing non-exportable CNG identity on Windows
11/Server 2022 or newer until every security and physical acceptance criterion
in [`PLATFORM_SUPPORT.md`](PLATFORM_SUPPORT.md) passes. Stages 1 through 4 are
implemented. Stage 5 has passed mixed-provider manual pairing, trusted reconnect,
fresh nonce rotation, focus without capture, physical keyboard/Raw Input mouse
forwarding, interactive `SendInput` observation, emergency release, and
deterministic key/button snapshot reconciliation on the guarded Windows
11/Windows 10 pair. Abrupt sender termination while a key and mouse button were
held released both after lease expiry. A four-second, executable/IP/UDP-port
scoped network interruption caused focus-lease renewal to fail locally; the
temporary rules were removed automatically. A fresh reconnect rotated the
nonce, delivered both valid probes, and rejected live prior-session-nonce and
stale-epoch packets. Final identity snapshots were unchanged. Stage 5 is
complete, but Windows 10 remains experimental/unsupported pending a separate
production-admission and release-integration decision.
