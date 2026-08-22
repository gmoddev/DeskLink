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
`desklink_windows` adapter contains Raw Input, minimal low-level suppression
hooks, and `SendInput`; WASAPI remains unimplemented.

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
- reconciliation snapshots

Datagrams include:

- current pointer position
- PCM audio frames

The current implementation provides `ITransportEndpoint` plus a deterministic in-memory implementation. The production adapter should use native MsQuic.

The runtime-selection layer is retained as an integrity and diagnostics
boundary. Production packages ship only Schannel. The previously investigated
Windows 10 OpenSSL path is future R&D, not an alternate production trust model;
its constraints are recorded in [`PLATFORM_SUPPORT.md`](PLATFORM_SUPPORT.md).

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

---

## 7. Pointer semantics

Pointer datagrams carry the latest absolute normalized position rather than raw deltas:

```cpp
PointerPositionMessage {
    uint16 display_id;
    uint16 normalized_x;
    uint16 normalized_y;
}
```

`0..65535` maps to the target display coordinate range.

This prevents permanent cursor divergence after datagram loss. If packet 101 is lost, packet 102 still expresses the newest desired location. The Agent also rejects pointer datagrams whose sequence is older than or equal to the newest accepted pointer packet within the current focus epoch, preventing reordering from jumping the cursor backward.

The current Windows injector maps the normalized values to the Windows virtual desktop using `MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK`. Production monitor routing should first transform `display_id` into the target monitor's virtual-desktop rectangle.

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

The foundation implements the bounded reorder/jitter and silence-concealment layer. WASAPI and drift compensation remain backend work.

---

## 10. Local control API

The intended local control surface is a current-user-only named pipe rather than a LAN HTTP server.

Example commands:

```text
GetState
SetDesiredMode
FocusMachine
SetAudioGain
ToggleAudioMute
```

The local API must not expose:

```text
SendRawTransportPacket
InjectArbitraryInput
ExecuteCommandLine
LoadArbitraryModule
```

This keeps a compromised Stream Deck plugin from automatically gaining the entire DeskLink trust boundary.

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
