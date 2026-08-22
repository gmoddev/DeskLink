# DeskLink Foundation

DeskLink is a local distributed desk-control platform intended to provide secure keyboard/mouse roaming, network audio, state synchronization, and future capability modules between trusted Windows PCs.

This repository is a **reference foundation implementation**, not a finished production release. It deliberately implements and tests the protocol/state/security invariants before binding them to privileged or timing-sensitive Windows APIs.

## Implemented

- C++20 portable core
- Binary wire envelope with bounded decoding
- Reliable-vs-datagram lane validation
- Explicit per-peer capability set
- Host-side focus transaction state
- Agent-side input authorization gate
- Short-lived focus leases
- Epoch-based stale-input rejection
- Fail-local emergency state
- Owned input-state cleanup callback
- Absolute normalized pointer messages
- PCM audio frame format
- Bounded reorder/jitter buffer with silence concealment
- Abstract transport interface
- Authenticated/encrypted session gate and session-nonce binding
- Manual pairing transcript with short authentication code confirmation
- Stored peer certificate-pin enforcement for every operational session
- Windows CNG randomness/SHA-256 and user-scoped DPAPI trust store
- Optional MsQuic stream/datagram endpoint adapter pinned to MsQuic 2.5.8
- Current-user CNG device key and self-signed certificate lifecycle
- MsQuic listener/client bootstrap with mutually pinned certificate validation
- Dedicated bounded pairing ALPN and per-address attempt limits
- End-to-end HostSession/AgentSession focus handshake over the transport abstraction
- In-memory transport for deterministic testing
- Windows `SendInput` injector adapter
- Simulation CLI
- Regression/adversarial tests

## Not yet implemented

The following are intentionally kept behind interfaces and are the next production layers:

- Pairing confirmation UI
- Two-PC MsQuic failure-injection and reconnect validation
- LAN discovery/mDNS
- Windows Raw Input capture and minimal low-level suppression hooks
- Monitor graph and edge roaming
- WASAPI loopback capture and render backend
- Clock-drift resampling
- Named-pipe local control API
- Profile/foreground-window engine
- UI and Stream Deck plugin

See [`docs/IMPLEMENTATION_STATUS.md`](docs/IMPLEMENTATION_STATUS.md) for the exact boundary.

## Build

### Linux/macOS portable validation

```bash
cmake -S . -B build -DDESKLINK_BUILD_WINDOWS_BACKENDS=OFF
cmake --build build
ctest --test-dir build --output-on-failure
./build/desklink_sim
```

### Windows

Use Visual Studio 2022/2026 or another C++20-capable compiler:

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The official Schannel MsQuic runtime requires Windows 11 or Windows Server 2022
or newer. Windows 10 can cross-build the adapter, but cannot run Schannel's
QUIC/TLS 1.3 path; supporting it would require the separate OpenSSL package and
credential model.

When built on Windows, `desklink_windows` includes the current `Win32InputInjector` implementation using `SendInput`.

## Repository layout

```text
include/desklink/
    protocol.hpp         Wire types and bounded codec contract
    capabilities.hpp     Capability grant model
    focus.hpp            Focus/lease/epoch state machine
    host.hpp             PC1 focus transaction generator
    session.hpp          Secure session binding over ITransportEndpoint
    agent.hpp            PC2 authorization and injection gate
    input.hpp            Input backend interface
    audio.hpp            Audio jitter/reorder buffer
    transport.hpp        Authenticated transport abstraction
    win32_input.hpp      Windows SendInput adapter

src/
    protocol.cpp
    focus.cpp
    host.cpp
    session.cpp
    agent.cpp
    audio.cpp
    in_memory_transport.cpp
    win32_input.cpp

apps/
    desklink_sim.cpp

tests/
    core_tests.cpp

docs/
    IMPLEMENTATION.md
    ARCHITECTURE.md
    SECURITY.md
    PROTOCOL.md
    WINDOWS_BACKENDS.md
    IMPLEMENTATION_STATUS.md
    VALIDATION.md
    REFERENCES.md
```

## Design rule

A paired DeskLink peer is not automatically a remote-control endpoint.

Trust is layered:

```text
DISCOVERED
    ↓
PAIRED / AUTHENTICATED
    ↓
CAPABILITY GRANTED
    ↓
ACTIVE LEASE / CURRENT EPOCH
    ↓
ACTION ACCEPTED
```

For input injection in particular, all layers must succeed.

## Production transport requirement

The production transport must guarantee:

- TLS 1.3 authenticated encryption
- peer identity binding/pinning
- reliable ordered streams
- QUIC DATAGRAM support
- no implicit Internet exposure
- authenticated peer metadata exposed to DeskLink Core

Native MsQuic is the intended implementation.

## Security posture

DeskLink is intended to fail local. If Host, Agent, or the network disappears, control must return to the physical PC and any DeskLink-owned injected key/button state must be released.

See [`docs/SECURITY.md`](docs/SECURITY.md).
