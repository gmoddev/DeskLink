# DeskLink Security Model

## 1. Security objective

DeskLink intentionally grants sensitive capabilities. `input.inject` allows a trusted peer to operate applications in the logged-in user's security context, so it must be treated as a high-trust permission.

The objective is not to make that permission harmless. The objective is to ensure:

- only explicitly paired peers can authenticate
- authentication alone grants no capability
- capabilities are narrow and revocable
- active input requires a short-lived focus lease
- stale traffic cannot regain authority
- failure returns input to the physical/local machine
- DeskLink does not create an arbitrary remote-code-execution primitive

---

## 2. Threat model

### In scope

- unknown machine on the LAN attempts to connect
- spoofed discovery advertisement
- man-in-the-middle during first pairing
- man-in-the-middle after pairing
- replayed or delayed packets
- malformed protocol input
- oversized allocation attempts
- stale input after a focus switch
- lost key-up/button-up events
- network loss while input is remote
- Agent/Host crash
- compromised low-privilege local integration such as a Stream Deck plugin
- capability misuse by an authenticated peer
- dependency vulnerabilities in the transport stack

### Not fully mitigated by DeskLink

If a peer with `input.inject` is itself compromised, an attacker controlling that peer can exercise the granted input capability until it is revoked. Capability separation limits blast radius but cannot make a compromised trusted endpoint trustworthy.

---

## 3. Pairing and identity

The pairing core now requires a persistent certificate identity and an interactive verification step.

Recommended flow:

1. User explicitly opens a pairing window of no more than five minutes on both PCs.
2. Peers exchange machine IDs, display names, certificate SHA-256 pins, and fresh 32-byte nonces.
3. Both derive a six-digit authentication string from the canonical SHA-256 transcript.
4. Both display the same code/words.
5. User confirms the match.
6. Each stores the peer's public identity/fingerprint.
7. Future QUIC sessions must authenticate as the pinned identity.

Discovery/mDNS only locates candidates. It must never create trust. The native
Windows browser accepts at most 64 service names per bounded run and enforces
strict TXT key/value/count/aggregate-size limits, canonical machine IDs and
capability fields, printable UTF-8 display names, `.local` hostnames, nonzero
interface/port values, and the exact supported protocol version. Unknown valid
TXT keys are ignored for forward compatibility; duplicate keys are rejected.
Conflicting advertisements for one machine ID are marked ambiguous and are
never acted on automatically. Untrusted display names are escaped for terminal
output. Discovery cannot open pairing, persist a peer, connect, select/fallback
a TLS provider, grant a capability, request focus, or admit a session.

The Windows implementation uses CNG for cryptographic randomness/SHA-256 and
current-user DPAPI for the bounded trust store. Device-certificate private-key
creation uses a named current-user Microsoft Software Key Storage Provider key;
the self-signed SHA-256 certificate is stored in the current-user `MY` store.
The private key is non-exportable. Its export policy, key name, certificate, and
pin must not be changed to accommodate a transport provider or older platform.

First-time exchange is isolated on `desklink/pair/1`. It accepts only one
strictly bounded offer while the manual window is open, binds the offer pin to
the TLS leaf, and never exposes an operational transport endpoint. Confirmed
peers reconnect on `desklink/session/1`, where both certificates must match the
stored pins.

Operational `HostSession` and `AgentSession` startup independently checks the
stored machine ID and certificate pin after the transport reports TLS
authentication and encryption. A TLS-authenticated but unpaired peer is refused.

---

## 4. Capability authorization

The foundation implements a bitset capability model.

Initial capability names:

```text
CoreStateRead
InputSend
InputInject
AudioSend
AudioReceive
ClipboardRead
ClipboardWrite
SystemSleep
SystemWake
SystemLaunch
FileSend
FileReceive
```

For remote input, `AgentCoordinator` rejects input unless `InputInject` is currently granted.

Revoking `InputInject` immediately:

- ends remote focus
- invokes the owned-input cleanup callback
- blocks later input traffic

Foreground profile metadata is a local policy input, not an authorization
input. Rules are bounded, exact executable-basename matches with no wildcards,
regular expressions, command lines, module loading, or target-process code
injection. Windows image lookup requests only limited query access. If profiles
exist and the foreground window or process cannot be inspected, the policy
returns `LockPc1` instead of assuming the system default. Manual override is
still subordinate to the emergency fail-local state.

The production profile surface is limited to one validated fallback mode and
32 exact `executable=mode` rules. Fullscreen matching is an explicit CLI option;
there is no configuration-file discovery, wildcard/regex matching, path lookup,
shell expansion, or target-process command-line inspection. Control-pipe mode
changes are explicit process-lifetime manual overrides.

The Host input lifecycle treats `GAME` and `LOCK_PC1` as restricted modes. It
first disables local suppression/forwarding, releases the current focus epoch,
and synchronously removes the capture hooks before applying the restricted
mode. A later permissive decision cannot reuse the old epoch or capture
installation: a fresh focus grant and a successful reliable input-state
snapshot are required before capture is restarted and routing is enabled. Any
focus-request, snapshot, or capture-start failure returns to `LOCK_PC1` with
capture disabled.

Foreground callbacks, control requests, `FocusReady`, lease/snapshot ticks,
emergency release, and capture failures execute through one serialized Host
lifecycle owner. A capture callback first clears the routing gate synchronously;
it never tears down its own worker thread. The serialized queue is capped at 64
events; admission failure clears routing and signals terminal fail-local rather
than dropping a foreground restriction. The serialized owner then releases
focus and joins capture threads. Renewal is attempted only in the `Remote`
state, so an intentional `GAME`/`LOCK_PC1` transition cannot be mistaken for a
transport failure or reopen capture.

---

## 5. Focus lease and epoch

Capability is necessary but insufficient for live input.

The Agent also requires:

```text
active lease
AND matching epoch
```

Lease durations are clamped in the current foundation to 100-2000 ms. The normal requested value is 750 ms.

When focus starts, the Agent advances an epoch. Every input message must carry that epoch.

Epoch invalidation occurs on:

- normal focus release
- lease expiry
- emergency fail-local
- a subsequent focus acquisition

This turns delayed network traffic into harmless stale data. Focus acquisition additionally uses a nonzero transaction `request_id`; Host accepts `FocusReady` only for its currently pending request.

The Windows capture adapter uses Ctrl+Alt+Pause as its physical fail-local
chord, accepting both the Pause and Windows Ctrl+Break virtual-key forms. The
low-level hook clears the atomic routing flag before notifying the worker.
Injected events always pass through. An invalid keyboard scan code, a busy/full
keyboard capture queue, Raw Input queue overflow, or an invalid/unqueueable
physical wheel event disables routing before the session is released. Wheel
input is suppressed only after its bounded reliable event has been accepted by
the nonblocking capture queue; otherwise that physical event passes locally.

---

## 6. Input cleanup

The Windows injector tracks only keys/buttons injected by DeskLink.

On cleanup it generates UP transitions for those owned inputs.

It must not blindly release every key reported down by Windows because that would corrupt legitimate local physical input state.

Cleanup is invoked on:

- focus release
- lease expiry
- capability revocation
- Agent disconnect

A later hardened Windows implementation can move the cleanup watchdog into a minimal current-user helper process so an Agent crash can still release DeskLink-owned state.

---

### Audio backend isolation

The WASAPI path is a current-user, shared-mode capability module. It has
no authority over focus, input capture, injection, pairing, trust, or session
admission. Capture packets are capped at 8192 source frames, emitted network
blocks are fixed at 960 PCM bytes, and the renderer accepts at most 64 validated
blocks. Queue overflow or endpoint failure stops/rejects audio work without
weakening input or transport security.

The backend never logs audio samples. Loopback capture requires an explicit
`--send-audio` runtime opt-in after `PeerValidated` and a local trust record that
grants the peer `AudioReceive`. Rendering requires `--receive-audio` after
`PeerValidated` and a local trust record that grants the sender `AudioSend`.
Both grants are independently confirmed during manual pairing and default off.

Every audio datagram is decoded on the datagram lane, bound to the fresh
session nonce, fixed to the canonical frame shape, restricted to one nonzero
stream ID per receiver, and admitted to a bounded sequence-aware jitter buffer
before the render callback runs. A missing grant, stale nonce, malformed frame,
changed stream, duplicate/late sequence, full queue, capture send failure, or
render rejection cannot become application-admissible audio. Endpoint recovery
must remain local to the audio module and cannot trigger provider, certificate,
session, or transport fallback.

Capture timestamps from an authenticated peer remain untrusted timing hints.
Only forward, already-admitted frames update the adaptive jitter estimator;
backward/reordered timestamps and intervals over five seconds reset the timing
baseline. Per-sample variation is capped, and the target is clamped to 2-12
five-millisecond blocks. A peer can therefore cause at most bounded audio-only
latency/rebuffering, not unbounded allocation, identity/provider changes,
session admission, focus, capture authority, or input state changes.

Clock-drift correction is driven only by already-admitted bounded source
occupancy, never directly by a peer timestamp. It observes 400 pump samples,
uses a quarter-block deadband, changes by only 50 ppm per window, clamps the
linear resampler to ±1000 ppm, and retains at most four source blocks. Timing or
adaptive-target discontinuity, reconnect/session reset, renderer rejection, and
endpoint recovery clear correction state. Invalid or adversarial timing can
therefore cause only bounded audio-module resampling/buffering and cannot alter
transport, identity, admission, focus, capture, or input authority.

The selected/default render endpoint is watched by a scoped
`IMMNotificationClient`. Endpoint change, removal, disablement, property/format
change, or operational WASAPI failure stops the old worker, drops queued audio,
and schedules reopen with 250 ms to 5 s capped backoff. Reopen reuses only the
already admitted session, nonce, and complementary grants; it does not reconnect,
re-pair, reload TLS, replace identity, or alter input authority. Capture callback
or datagram-send rejection is explicitly non-recoverable, so credential,
authorization, session, and transport failures cannot be disguised as endpoint
recovery.

---

## 7. Authenticated topology exchange

Display topology is untrusted peer metadata even after TLS. It is accepted only
on the reliable lane after `PeerValidated`, encrypted/authenticated transport,
normal trust lookup, fresh session-nonce negotiation, and an explicit
`DisplayTopologyExchange` grant in the local trust record. The payload repeats
both the sender machine ID and session nonce; they must exactly match the stored
peer and envelope nonce. Existing peers are never granted this capability by a
trust-store migration.

The decoder validates exact framing, one to 64 canonical descriptors, stable-ID
derivation/collision freedom, one primary display, virtual bounds, strings,
dimensions, physical-size source, orientation, and the aggregate 64 KiB
reliable ceiling before state replacement. A lower generation is ignored
without extending freshness. A different snapshot at the same generation,
malformed topology framing, wrong machine, or wrong nonce invalidates topology
readiness for that connection. Accepted state expires after five seconds;
timeout never falls back to stale topology.

Topology messages cannot grant capabilities, write trust, modify the saved
roaming graph, acquire focus, open streams, inject input, or enable capture.
Route Ready is derived separately from connection state and requires current
validated topologies plus explicit direction/capability checks. Failure leaves
the connection usable for already-authorized modules but the affected roaming
route remains unavailable. Recovery from rejection requires a fresh
authenticated connection and nonce.

---

## 8. Protocol hardening

The current codec performs bounded parsing before constructing messages.

Limits:

```text
reliable payload: 64 KiB
QUIC datagram payload: 1200 bytes
focus lease request: 100..5000 ms on wire
Agent effective lease: 100..2000 ms
audio sample rate: 8 kHz..192 kHz
audio channels: 1..8
audio frame count: 1..2048
sample width: 2 or 4 bytes
```

It also rejects:

- incorrect wire magic
- unsupported protocol version
- unknown message type
- truncated packet
- trailing data
- invalid enum values
- wrong transport lane
- inconsistent audio payload length

Production transport parsing must preserve these limits before allocating large buffers.

---

## 9. Transport security requirements

`ITransportEndpoint` exposes whether the peer is authenticated/encrypted. The in-memory transport is only a deterministic test adapter.

Production session establishment must require both values to be true before any capability message is accepted.

Production transport is stock MsQuic/Schannel on Windows 11 or Windows Server
2022 or newer because it supplies authenticated TLS 1.3 QUIC, reliable streams,
and QUIC DATAGRAM support while using the existing non-exportable CNG identity.
Windows 10 remains unsupported while the equal-security compatibility project
is incomplete. Its explicit OpenSSL runtime may be admitted only after opaque
CNG signing, fail-closed peer validation, identity invariance, and physical
validation pass. A TLS downgrade, identity export/replacement, reduced-security
validation mode, or automatic provider fallback remains prohibited.

Recommended production defaults:

- no 0-RTT for privileged control/input messages initially
- no UPnP
- no automatic port forwarding
- Windows Firewall Private/Domain profiles only by default
- no cloud relay
- no Internet/NAT traversal mode in V1
- connection rate limits before expensive pairing work
- datagram receive explicitly enabled only after authenticated session setup
- no session admission before peer certificate-pin validation succeeds
- no provider fallback after credential, certificate, authentication, or transport failure

---

## 10. Windows privilege boundary

The Agent should run at normal user integrity.

Windows `SendInput` is subject to UIPI. That means normal DeskLink input should not silently gain authority over higher-integrity applications, secure desktop/UAC, or the Windows login screen.

Do not permanently elevate DeskLink merely to bypass this boundary.

If elevated injection is ever required, implement it as a separately installed, separately granted component with a tightly scoped API.

---

## 11. Logging policy

DeskLink logs operational/security metadata, not user content.

Allowed examples:

```text
peer connected/disconnected
pair/revoke event
mode transition
focus transition
epoch change
lease expiry
RTT/loss/jitter statistics
protocol rejection counts
audio underrun count
```

Prohibited by default:

```text
raw keystrokes
text typed by the user
full cursor trails
clipboard contents
audio samples
window title contents
```

Debug builds should follow the same rule unless a user explicitly opts into a narrowly scoped diagnostic capture.

---

## 12. Local IPC security

The Host/Agent control API uses a versioned named pipe suffixed with the active
DeskLink user SID. It does not rely on the default named-pipe ACL: creation
uses a protected DACL containing exactly one allow ACE for that SID. The pipe
sets `PIPE_REJECT_REMOTE_CLIENTS`; the server validates the connecting client
process token SID, and the client validates the server process token SID before
sending a request. A first-instance flag and the client-side identity check
turn cross-user pipe squatting into a startup/availability failure rather than
command disclosure or execution.

The binary protocol limits payloads to 128 bytes, requires exact
magic/version/type/request-ID/length framing, permits one request per
connection, bounds I/O waits, and requires a response-consumption
acknowledgement. Malformed, oversized, stalled, identity-mismatched, or
unexpected responses fail closed.

The API defines high-level operations only:

```text
GetState             implemented
SetDesiredMode       implemented
FocusMachine         typed, currently unsupported
SetAudioGain         implemented as local bounded render policy
ToggleAudioMute      implemented as local bounded render policy
```

It must not expose a generic transport passthrough or arbitrary input-injection primitive.

Local restrictive `Game`/`LockPc1` policy takes precedence over a remote
`Roam` request. Restrictive changes release DeskLink-owned state and disable
capture locally. A control client never receives input contents, certificate
private-key material, trust-store secrets, or raw transport frames.
Gain and mute are applied only after peer/session/audio admission and cannot
grant a capability, admit traffic, select a TLS provider, or change system
audio settings. The bounded `0..10000` gain cannot amplify PCM beyond its
admitted level.

---

## 13. Dependency policy

DeskLink's most security-sensitive dependencies are transport/TLS, Windows bindings, and any future codec libraries.

Production CI should include:

- pinned dependency versions
- SBOM generation
- CVE/advisory scanning
- reproducible or provenance-tracked release artifacts
- signed Windows executables/installers
- a documented rapid-update path for high/critical transport vulnerabilities

The Windows 10 path is approved only as a staged security-sensitive R&D
project: MsQuic 2.6.x plus OpenSSL 3.5 LTS and an opaque CNG signing provider.
Stage 2's prototype explicitly selects the provider, rejects nonzero CNG export
policy and certificate/key mismatch, exposes only public RSA parameters, and
delegates RSA-PSS signing to `NCryptSignHash`. It retains its private OpenSSL
library context for the MsQuic runtime's process lifetime so worker-thread DRBG
cleanup never observes a freed context. Each stage requires its own clean
review and gates. Stage 3 implements an explicit `PeerValidated` admission state
and a bounded validation watchdog. CONNECTED notification alone grants no
application access; streams, datagrams, pairing/session delivery, focus, and
endpoint traffic all require completed peer validation. Rejection, timeout,
exception, missing-certificate, malformed-DER, invalid-time, signing-failure,
unknown-peer, wrong-pin, and changed-identity cases are regression-tested as
fail-closed with no admitted application session. The provider must never
export private key material. Stage 4 performs exact before/after comparison of
the CNG key name, provider, algorithm, zero export policy, certificate public
key, certificate DER hash, and DeskLink identity pin. It also makes prohibited
private-key export API references a build and test failure. The guarded Stage 5
physical gates also pass. Windows 10 remains experimental/unsupported pending a
separate production-admission and release-integration review. See
[`PLATFORM_SUPPORT.md`](PLATFORM_SUPPORT.md).

---

## 14. Security invariants to keep in tests

The following must remain regression-tested:

1. no capability => no injection
2. expired lease => no injection
3. stale epoch => no injection
4. capability revocation => owned input cleanup
5. disconnect => owned input cleanup
6. GAME/LOCK_PC1 => no remote focus
7. malformed packet => no side effect
8. oversized packet => rejected before large allocation
9. reliable input cannot be accepted from datagram lane
10. older/duplicate pointer datagrams are rejected within an epoch
11. stale FocusReady transaction IDs cannot acquire authority
12. wheel messages reject the datagram lane, unknown axes, zero deltas, and
    deltas outside `-1200..1200`
13. a wheel event is never suppressed unless its bounded hook enqueue succeeds
14. no CONNECTED effect, stream, datagram, pairing, focus, or session admission
    before `PeerValidated`
15. validator failure, exception, or timeout rejects the connection
16. missing, malformed, expired, not-yet-valid, unknown, wrong-pin, signing, and
    changed-identity cases admit no application session
17. 0-RTT application streams and datagrams are rejected
18. pointer/audio datagram semantics do not require retransmission
19. certificate rejection cannot reach `CONNECTED` or session admission
20. the device key remains non-exportable and identity pin remains unchanged
21. OpenSSL runtime components fail integrity before loading when modified
22. exportable or certificate-mismatched CNG credentials fail before networking
23. WASAPI frames and queues remain bounded and audio failure cannot alter
    input authority
24. audio capture/render require explicit runtime opt-in plus complementary
    `AudioReceive`/`AudioSend` grants after `PeerValidated`
25. stale nonce, malformed format, changed stream, duplicate/late sequence,
    and render rejection cannot reach audio playout
26. endpoint recovery drops queued audio, preserves identity/session/input
    authority, caps retry rate, and never retries client/send rejection
27. topology exchange requires the explicit trust grant and cannot occur before
    trusted session admission
28. topology machine and both nonce bindings must match the authenticated peer
    and current connection
29. malformed, oversized, stale, conflicting, rejected, or timed-out topology
    can never make a route Ready
30. topology exchange cannot grant focus/input authority or mutate the saved
    roaming graph
