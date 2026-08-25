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
| Windows physical input capture | Done | Low-level non-injected keyboard scan codes plus Raw Input mouse, bounded 1024-event queue, relative-motion coalescing |
| Low-level suppression gate | Done | Atomic route flag, injected-event pass-through, Ctrl+Alt+Pause/Break fail-local |
| Input-state reconciliation | Done | Reliable 500 ms snapshots; normal/extended keys and five buttons; owned-state convergence |
| Pointer format | Done | Protocol v2 relative-motion datagrams for ordinary movement; absolute display-aware datagrams retained for monitor transitions/resync; shared monotonic sequence gate |
| Pointer calibration | Done | 25-400% fixed-point gain with fractional-count retention; optional 100-32000 source-DPI normalization; no global Windows setting changes |
| Stable multi-monitor mapping | Done | Active DisplayConfig target identities; deterministic nonzero IDs; negative-origin rectangle transform; topology-generation invalidation |
| Display presentation metadata | Done | Checked active resolution/refresh/orientation; EDID physical size with raw-DPI estimate fallback; metadata does not change routing generations |
| Roaming graph foundation | Done | Strict bounded directional links and normalized edge segments; duplicate/overlapping source routes rejected; stable identities resolve only against current machine topologies |
| Roaming preference persistence | Done | Versioned 512 KiB-bounded exact codec and atomic current-user replacement; canvas positions remain presentation-only and trust/identity stay in security stores |
| Authenticated topology exchange | Done | Explicit trust grant; reliable snapshot bound to PeerValidated session, expected machine, and fresh nonce; canonical 64-display/64 KiB bounds; two-second refresh and five-second fail-closed freshness |
| Roaming connection/route status | Done | Peer and route state are separate; missing capability/display, unsupported direction, invalid snapshot, and topology synchronization cannot report Ready |
| Monitor configurator | Done | Native per-PC physical-size canvas; estimated/offline states; explicit bidirectional suggestions; manual one-way/partial segments; complete-graph validation and atomic Local-before-save replacement |
| Identify overlays | Done | One five-second topmost/no-activate/click-through overlay per active local display; resolution, refresh, PC, and estimate status; no capture or suppression |
| Companion lifecycle | Done | Single instance; first-run guidance; close-to-tray default; tray Open/Return Local/Exit; ordered shutdown; atomic preferences and optional current-user sign-in startup |
| Mouse-wheel transport | Done | Reliable ordered axis + signed delta; `-1200..1200` bound; enqueue-before-suppress fail-local hook path |
| Current-user control IPC | Done | SID-derived named pipe; explicit one-user DACL; remote rejection; mutual process-SID checks; bounded typed state/mode/audio plus read-only canonical topology snapshots for at most eight machines/512 KiB |
| LAN DNS-SD/mDNS discovery | Done | Native Windows link-local advertise/browse/resolve; strict TXT bounds; deterministic conflict reporting; no automatic trust, pairing, or connection |
| Foreground profile foundation | Done | At most 32 exact executable-name rules; optional fullscreen match; emergency/manual/profile/default precedence; uninspectable configured foreground fails local |
| Windows foreground monitor | Done | Out-of-context `EVENT_SYSTEM_FOREGROUND` hook on an owned message-loop thread; bounded image-name lookup; same-thread unhook; no polling or code loading |
| Host input lifecycle safety boundary | Done | Disable capture, release focus, synchronously stop hooks, then enter GAME/LOCK_PC1; exit requires fresh FocusReady + initial snapshot before capture restart/enable |
| Production Host profile runtime | Done | Bounded exact CLI rules and fallback mode; 64-event serialized WinEvent/control/FocusReady/renewal/failure queue; renewal only while Remote |
| Native Windows alpha wrapper | Done | Schannel-only typed launcher; manual pairing/session controls; Local-first controller; bounded gain/DPI controls; host-with-port rejection; same-user status/mode IPC; bounded in-memory diagnostics; portable ZIP |
| PCM audio frame | Done | Bounded wire representation |
| Exact audio block assembly | Done | 48 kHz/stereo/PCM16; exact 240-frame/5 ms blocks; bounded source packet acceptance; silence and discontinuity reset |
| Audio jitter buffer | Done | Reorder + bounded silence concealment; adaptive 2-12 block target; immediate bounded increases, 200-sample downward hysteresis, explicit rebuffer accounting |
| WASAPI capture/render foundation | Done | Event-driven shared-mode loopback capture and shared render; 64-frame render queue; silence underrun; explicit start/stop and failure isolation |
| Audio session/datagram wiring | Done | Explicit runtime opt-in; complementary AudioReceive/AudioSend grants; PeerValidated transport, nonce, format, stream, sequence, and bounded receiver/render admission |
| Audio endpoint recovery | Done | Scoped endpoint notifications; restart-safe adapters; audio-only 250 ms to 5 s capped retry; buffered audio reset; client/send rejection is not retried |
| Audio clock-drift correction | Done | 400-sample occupancy windows; 50 ppm slew steps; ±1000 ppm cap; four-block source bound; exact-block linear resampling; discontinuity reset |
| Per-peer audio gain/mute | Done | Host-local 0-10000 permyriad attenuation; five-millisecond ramp; endpoint-recovery persistence; no system mixer changes |
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
| Windows pairing control | Done | Five-minute window, native two-PC code prompt, explicit input/audio/topology grants; existing trust records are not migrated |
| Manual focus control | Done | Trusted serve/focus commands, lease renewal, explicit release |
| Windows 10 physical compatibility | R&D Stage 5 complete | Pairing, reconnect, nonce rotation, protocol-v2 relative pointer feel at 100%/raw DPI, physical forwarding, `SendInput`, snapshot recovery, emergency release, process termination, scoped network interruption, and live stale epoch/session rejection passed |
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
39. bounded local-control request/response framing, malformed/oversized rejection, and typed state validation
40. live same-user named-pipe state/unsupported-command round trip and bounded shutdown
41. local desired-mode precedence over a remote attempt to weaken `Game`/`LockPc1`
42. strict discovery TXT round trip, required/canonical fields, malformed and oversized rejection
43. deterministic multi-interface discovery grouping, conflict marking, removal, and expiry
44. bounded foreground rules, case normalization, fullscreen matching, duplicate rejection, and precedence
45. native Windows foreground-hook startup, initial bounded snapshot delivery, and same-thread teardown
46. ordered GAME/LOCK_PC1 teardown, stale FocusReady rejection, fresh-focus capture recreation, and fail-local snapshot/start failure handling
47. exact profile-spec parsing, all four mode names, invalid-path/mode rejection, and case-normalized duplicate rejection
48. protocol-v2 signed relative-motion codec bounds, wrong-lane rejection, shared pointer sequence gating, and end-to-end admission
49. fixed-point pointer gain/DPI scaling, including fractional-count retention and invalid calibration rejection
50. alpha-launcher typed argument bounds, Schannel pinning, gain/DPI forwarding, and duplicated endpoint-port rejection
51. trusted reconnect with fresh nonce plus successfully decoded application traffic before graceful close
52. strict EDID header/checksum/detailed-timing parsing, physical-size fallback, and all four orientations
53. presentation-only metadata updates preserve routing generation while rectangle changes invalidate it
54. roaming graph bounds, reciprocal/one-way links, duplicate and overlapping source rejection, and exact codec framing
55. current-topology stable-identity resolution including offline, missing, and ambiguous machines
56. atomic current-user roaming preference save/load, invalid-candidate rollback, and malformed-file rejection
57. topology snapshot reliable-lane round trip, canonical descriptor validation, truncation, wrong-lane, embedded-NUL, duplicate, ID, and 64 KiB aggregate bounds
58. topology admission gating for explicit capability, expected peer machine, envelope/payload nonce, generation monotonicity, same-generation invariance, and five-second timeout
59. pre-admission refusal and end-to-end bidirectional HostSession/AgentSession topology exchange, local-machine pinning, stale generation handling, malformed traffic invalidation, nonce rejection, and fresh reconnect recovery
60. route status remains unready for authenticating peers, synchronization, missing grants/displays, unsupported directions, and rejected topology
61. alpha launcher forwards the explicit topology grant while retaining production Schannel pinning
62. bounded control-topology codec and same-user named-pipe round trip, including local-machine uniqueness, Ready/snapshot correspondence, and mutually exclusive response payloads
63. physical-size canvas construction, estimate/offline presentation, explicit bidirectional adjacency suggestion, and proof that canvas movement cannot affect stable route resolution
64. strict atomic application-preference persistence with malformed/trailing-data rejection
65. first-save DPAPI trust persistence creates only the exact configured parent directory before atomic replacement

Build/test result in the creation environment:

```text
100% tests passed, 0 tests failed
```

---

## Production blockers

### P0 — required before using DeskLink across real PCs

- two-PC Windows 11 LAN pairing, cable-removal, and reconnect validation
  (deferred until a second Windows 11 target is available)

### P1 — required for useful KM roaming

- physical emergency-chord and high-poll-rate timing validation
- edge hysteresis

### P2 — final product surface

- final onboarding and tray visual polish beyond the completed companion lifecycle
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

The complete Windows 11 physical matrix remains a production-release gate. It
does not block the approved staged implementation and controlled experimental
validation of roaming.

Snapshot reconciliation and stable multi-monitor mapping are complete. The real
Windows 11 two-PC failure matrix is deferred until a second Windows 11 target is
available. Bounded mouse-wheel transport, current-user IPC, and link-local
discovery are complete. The bounded foreground policy engine, native WinEvent
monitor, and production CLI/live event runtime are complete. One serialized
Host owner applies WinEvent and manual decisions through the fail-local
lifecycle, admits capture only after fresh focus plus snapshot, and
renews/reconciles only while Remote. The bounded event-driven WASAPI
capture/render foundation, exact V1 audio block assembler, complementary
capability/session datagram gates, and receiver jitter/render pump are complete.
Scoped WASAPI endpoint notification and audio-only bounded reopen are complete.
Bounded adaptive jitter targeting, rebuffer accounting, and asynchronous clock-
drift correction and per-peer gain/mute are complete. Roaming Phase 1 adds
bounded display presentation metadata, the strict persisted directional graph,
current-topology stable-identity resolution, and atomic preferences without
allowing canvas geometry to influence routing. Phase 2 adds the explicit
topology capability, bounded reliable snapshots, peer/machine/nonce admission,
freshness timeout, and separate peer/route readiness without edge switching.
Phase 3 adds the native presentation-only configurator, explicit edge editing,
offline/route status, Local-before-save atomic replacement, Identify overlays,
and the single-instance tray/startup lifecycle. Phase 4 controlled crossing is
next. The native Windows alpha wrapper provides the manual pairing, session,
status, and configuration surface used for physical validation without changing
any trust or transport boundary.
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
