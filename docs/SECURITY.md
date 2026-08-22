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

Discovery/mDNS only locates candidates. It must never create trust.

The Windows implementation uses CNG for cryptographic randomness/SHA-256 and
current-user DPAPI for the bounded trust store. Device-certificate private-key
creation uses a named current-user Microsoft Software Key Storage Provider key;
the self-signed SHA-256 certificate is stored in the current-user `MY` store.

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

## 7. Protocol hardening

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

## 8. Transport security requirements

`ITransportEndpoint` exposes whether the peer is authenticated/encrypted. The in-memory transport is only a deterministic test adapter.

Production session establishment must require both values to be true before any capability message is accepted.

Native MsQuic is the intended transport because it supplies authenticated TLS 1.3 QUIC, reliable streams, and QUIC DATAGRAM support in one connection.

Recommended production defaults:

- no 0-RTT for privileged control/input messages initially
- no UPnP
- no automatic port forwarding
- Windows Firewall Private/Domain profiles only by default
- no cloud relay
- no Internet/NAT traversal mode in V1
- connection rate limits before expensive pairing work
- datagram receive explicitly enabled only after authenticated session setup

---

## 9. Windows privilege boundary

The Agent should run at normal user integrity.

Windows `SendInput` is subject to UIPI. That means normal DeskLink input should not silently gain authority over higher-integrity applications, secure desktop/UAC, or the Windows login screen.

Do not permanently elevate DeskLink merely to bypass this boundary.

If elevated injection is ever required, implement it as a separately installed, separately granted component with a tightly scoped API.

---

## 10. Logging policy

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

## 11. Local IPC security

The eventual Host control API should use a named pipe with a DACL restricted to the active DeskLink user/SID.

Do not rely on default named-pipe ACLs for a sensitive control surface.

The API should expose high-level operations only:

```text
SetMode
FocusMachine
SetAudioGain
ToggleMute
GetState
```

It must not expose a generic transport passthrough or arbitrary input-injection primitive.

---

## 12. Dependency policy

DeskLink's most security-sensitive dependencies are transport/TLS, Windows bindings, and any future codec libraries.

Production CI should include:

- pinned dependency versions
- SBOM generation
- CVE/advisory scanning
- reproducible or provenance-tracked release artifacts
- signed Windows executables/installers
- a documented rapid-update path for high/critical transport vulnerabilities

---

## 13. Security invariants to keep in tests

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
12. pointer/audio datagram semantics do not require retransmission
