# DeskLink Foundation

DeskLink is a local distributed desk-control platform intended to provide secure keyboard/mouse roaming, network audio, state synchronization, and future capability modules between trusted Windows PCs.

This repository is a **reference foundation implementation**, not a finished production release. It deliberately implements and tests protocol/state/security invariants before and alongside privileged or timing-sensitive Windows adapters.

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
- Relative pointer-motion datagrams for ordinary Raw Input plus absolute,
  display-aware pointer messages for monitor transitions/resynchronization
- Bounded reliable vertical/horizontal mouse-wheel messages
- PCM audio frame format
- Exact 48 kHz/stereo/PCM16/5 ms audio block assembler
- Bounded reorder/jitter buffer with silence concealment
- Adaptive 10-60 ms audio jitter targeting with bounded rebuffer hysteresis
- Bounded asynchronous audio clock-drift correction with ±0.1% resampling
- Event-driven Windows WASAPI loopback-capture and shared-render foundation
- Two-sided capability-gated audio datagrams and bounded receiver/render pump
- Explicit, text-only clipboard synchronization with complementary per-peer
  read/write grants, a session-scoped module handshake, and loop suppression
- Default-endpoint notification and bounded audio-only WASAPI recovery
- Abstract transport interface
- Authenticated/encrypted session gate and session-nonce binding
- Pairing-v2 role-bound commit/reveal transcript with short authentication code confirmation
- Stored peer certificate-pin enforcement for every operational session
- Windows CNG randomness/SHA-256 and generation-checked, SID-scoped cross-session user DPAPI trust store
- Optional MsQuic stream/datagram endpoint adapter pinned to MsQuic 2.6.0
- Integrity-checked MsQuic runtime selection with Schannel as the production provider
- Experimental, explicitly selected OpenSSL 3.5/CNG credential prototype for Windows 10 R&D
- Current-user CNG device key and self-signed certificate lifecycle
- MsQuic listener/client bootstrap with mutually pinned certificate validation
- Explicit `PeerValidated` gate for CONNECTED, streams, datagrams, pairing, and sessions
- Bounded fail-closed certificate-validation worker with rejection/timeout handling
- Fresh per-connection session nonce negotiation inside pinned TLS
- Dedicated bounded pairing ALPN and per-address attempt limits
- Native Windows pairing control with explicit two-PC code confirmation
- End-to-end HostSession/AgentSession focus handshake over the transport abstraction
- In-memory transport for deterministic testing
- Windows `SendInput` injector adapter
- Opt-in Windows low-level keyboard and Raw Input mouse capture with bounded sender queue
- Bounded pointer gain (25-400%) and optional source-DPI normalization without
  changing either PC's Windows mouse settings
- Fail-local keyboard/mouse suppression gate with Ctrl+Alt+Pause escape
- Periodic reliable input-state reconciliation for normal/extended keys and mouse buttons
- Guarded two-PC reconciliation fault validation with a non-production-only control
- Stable Windows DisplayConfig identities, deterministic display IDs, rectangle mapping,
  and topology-generation invalidation
- Bounded monitor presentation metadata from DisplayConfig plus checked EDID/raw-DPI
  size fallback
- Strict 128-link roaming graph with stable-identity resolution and atomic
  current-user preference persistence; canvas geometry is presentation-only
- Explicit capability-gated display-topology exchange over the authenticated
  reliable session, with nonce/machine binding, five-second freshness, and
  fail-closed route readiness
- Bounded same-user topology inspection for the companion UI; canonical remote
  snapshots remain authenticated-session data and expire with normal topology
  readiness
- Native monitor configurator with per-PC physical-size canvas, offline states,
  explicit bidirectional adjacency suggestions, one-way/partial-edge advanced
  editing, atomic save, and five-second click-through local Identify overlays
- Single-instance Windows companion lifecycle with first-run guidance,
  close-to-tray, tray Open/Return Local/Exit, and optional current-user sign-in
  startup
- Explicitly enabled experimental edge roaming with local Raw Input observation,
  Push/DwellAndPush/DoublePush intent, proportional corner-safe landing,
  re-entry cooldown, a 1.5-second focus timeout, and fail-closed route/session
  revalidation before the existing Host lifecycle may suppress input
- One authenticated reciprocal peer-session owner with independent directional
  grants, nonce-bound direction tokens, deterministic collision-to-Local, and
  explicit listener-side roaming opt-in
- Low-level-hook wheel capture with enqueue-before-suppress fail-local behavior
- Current-user-only named-pipe control API with bounded typed state/mode commands
- Bounded native Windows DNS-SD/mDNS discovery with late-callback-safe untrusted candidate output
- Bounded foreground-profile policy engine and native Windows WinEvent monitor
- Fail-local Host input lifecycle planner with restart-safe Win32 capture
- Production Host profile CLI and serialized live mode-event runtime
- Native Windows alpha launcher with bounded pairing/session controls,
  authenticated status/mode IPC, graceful child lifecycle, a Schannel-only
  ZIP, and a current-user installer foundation
- Explicit current-user update coordinator with prevalidated same-signer
  candidate/rollback installers, ordered fail-local shutdown, bounded Setup,
  post-install health checks, and automatic rollback
- Simulation CLI
- Regression/adversarial tests

## Not yet implemented

The following are intentionally kept behind interfaces and are the next production layers:

- Two-PC Windows 11 MsQuic failure-injection and reconnect validation
- Windows 10 OpenSSL/CNG production admission and release integration
- Physical two-PC reciprocal edge-roaming signoff
- Sustained physical two-PC audio timing and failure validation
- Physical default-device switch, disable/re-enable, and sleep/resume validation
- Physical two-PC text-clipboard privacy, contention, reconnect, and owner-exit validation
- Production signing/clean-system installer and update qualification, final
  product polish, and Stream Deck plugin

See [`docs/IMPLEMENTATION_STATUS.md`](docs/IMPLEMENTATION_STATUS.md) for the exact boundary.
Current release-specific defects and workarounds are tracked in
[`docs/KNOWN_ISSUES.md`](docs/KNOWN_ISSUES.md).

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

DeskLink's production baseline is Windows 11 or Windows Server 2022 or newer.
Production transport uses the stock MsQuic Schannel runtime and the existing
non-exportable current-user CNG device identity. Windows 10 is explicitly
unsupported while the separately staged equal-security compatibility project
is incomplete. That project may not replace or export the identity, downgrade
TLS, weaken validation, or add automatic provider fallback. See
[`docs/PLATFORM_SUPPORT.md`](docs/PLATFORM_SUPPORT.md).

Stage 4 records and compares the CNG key name, provider, algorithm, export
policy, certificate public-key DER, certificate DER hash, and DeskLink identity
pin before and after both Schannel and OpenSSL/CNG compatibility handshakes. A
normal build-and-test gate also rejects private-key export API references in the
DeskLink credential/runtime boundary and downstream provider patch.

Compatibility builds may explicitly set
`DESKLINK_BUILD_PHYSICAL_VALIDATION=ON` to produce
`desklink_pair_validation.exe`. That separately named control is not built by
default and is not a production artifact. It exposes only bounded Stage 5 fault
probes; the normal `desklink_pair.exe` rejects those options.

### Native alpha wrapper

Windows MsQuic builds produce `desklink_alpha.exe` and the long-lived
current-user `desklink_runtime.exe` broker. The Alpha remains a migration
launcher around the existing `desklink_pair.exe` transport runtime, but normal
UI status, topology, product-preference, and trust-management traffic goes
through the broker's bounded same-user endpoint. The broker owns UI-facing
identity, trust, and preference access; it never exposes raw certificates, input events,
clipboard text, audio frames, arbitrary execution, or module loading.

Generic broker clients may enumerate bounded trust metadata, reduce grants, or
forget a device only after fail-local session cleanup. They cannot persist a
permission increase: additions require a new broker-owned local
reauthorization flow. The current Alpha's transport child is assigned to a
kill-on-close job, so an actual UI process death also terminates pairing and
fails active input Local during migration.

Controller sessions always start in `lock-pc1` and require an explicit
**Focus remote** action. **RETURN LOCAL** applies `LockPc1` through the
authenticated control pipe, while Ctrl+Alt+Pause/Break remains the independent
physical emergency path. The alpha package is Schannel-only and supports the
Windows 11 / Server 2022+ production baseline. See
[`docs/ALPHA_WRAPPER.md`](docs/ALPHA_WRAPPER.md) for the workflow and packaging
command.

Windows CI also creates an explicitly unsigned current-user development
installer and validates install, fail-closed update rejection, forced rollback,
coordinated upgrade, startup cleanup, state preservation, and uninstall.
Unsigned installers are never production release artifacts. Production
packaging fails closed unless the DeskLink executables, uninstaller, and Setup
can be Authenticode-signed and timestamped with an explicit current-user
certificate. See [`docs/WINDOWS_INSTALLER.md`](docs/WINDOWS_INSTALLER.md) and
[`docs/WINDOWS_UPDATES.md`](docs/WINDOWS_UPDATES.md).

When built on Windows, `desklink_windows` includes the current `Win32InputInjector` implementation using `SendInput`.

While `serve` or `focus` is running, the same Windows user can inspect the
bounded runtime state or select a desired mode without opening a LAN control
port:

```powershell
desklink_pair.exe control state
desklink_pair.exe control mode roam
desklink_pair.exe control mode lock
desklink_pair.exe control mode game
desklink_pair.exe control gain 7500
desklink_pair.exe control mute
desklink_pair.exe control preferences
desklink_pair.exe control devices
desklink_pair.exe control return-local
```

The named pipe uses an explicit one-SID DACL, rejects remote clients, verifies
both endpoint process-token SIDs, and does not expose raw packets, arbitrary
input injection, command execution, or module loading.

### Discover PCs on the local link

`listen` and `serve` advertise `_desklink._udp.local` through Windows' native
DNS-SD API. Discovery is bounded to three seconds by default and may be set to
1–30 seconds:

```powershell
desklink_pair.exe discover
desklink_pair.exe discover 10
```

Advertisements contain only a protocol version, machine ID, display name,
capability hints, and whether the manual pairing window is open; host and port
come from DNS-SD SRV resolution. They are untrusted address hints. Discovery
never writes the trust store, opens pairing, connects, grants a capability, or
admits a session. Duplicate machine IDs with conflicting metadata are reported
as ambiguous, and advertisement failure leaves manual IP pairing/connection
available. DeskLink does not alter Windows Firewall or network profiles.

### Pair two Windows PCs

Build with the pinned MsQuic package, then open the same Private/Domain-profile
UDP port on both PCs as appropriate for the local network. On the receiving PC:

```powershell
desklink_pair.exe listen 43821 --grant-input
```

On the initiating PC:

```powershell
desklink_pair.exe pair 192.168.1.25 43821
```

Both PCs show a native confirmation prompt. Accept only when the same six-digit
code appears on both machines. `--grant-input` grants the newly paired remote PC
permission to inject input on the PC where that flag is supplied; it is omitted
by default. `--grant-audio-send` permits that remote PC to send audio into this
PC. `--grant-audio-receive` permits it to receive loopback audio captured on
this PC. `--grant-clipboard-read` permits it to read this PC's text clipboard,
and `--grant-clipboard-write` permits it to replace this PC's text clipboard.
These independent grants are shown in the confirmation prompt and are omitted
by default. Trust is stored under the current user's local application-data
directory with DPAPI protection. DeskLink does not modify Windows Firewall.

`--tls-provider auto|schannel|openssl` controls the packaged MsQuic TLS runtime.
`auto` is the default. The loader resolves `runtime/<provider>/msquic.dll`
relative to the executable, verifies its build-pinned SHA-256 and MsQuic 2.6.0
provider/version metadata, locks and rejects reparse points across the runtime
path while loading, and never uses the current directory or `PATH`.
Production packages include only the Schannel runtime. Research builds may add
the pinned, patched OpenSSL runtime explicitly; they verify `msquic.dll`,
`libcrypto`, and `libssl` before loading. No runtime or credential failure can
fall back to a different provider. See
[`third_party/msquic/README.md`](third_party/msquic/README.md).

After pairing, run `desklink_pair.exe serve 43821` on the input-receiving PC.
On the other PC, `desklink_pair.exe focus 192.168.1.25 43821 --capture` acquires
and renews a remote-focus lease, forwards physical keyboard events from the
low-level hook and mouse events from Raw Input, and suppresses corresponding
local input until Enter is pressed. Ctrl+Alt+Pause (including Windows'
Ctrl+Break representation) immediately disables suppression and fails local.
Omitting `--capture` retains the manual control-plane-only check.
Ordinary motion is transported as relative Raw Input counts, avoiding any
dependency on the controlling PC's total virtual-desktop width. Optional
`--pointer-gain 25..400` and `--pointer-dpi 100..32000` calibrate those counts;
omitting DPI preserves raw counts. DeskLink never changes global Windows mouse
speed, acceleration, or device settings.

Experimental controlled roaming is opt-in. Save an explicit link with
**Arrange monitors**, enable **Capture and route physical input** plus
**Experimental edge roaming** on each PC that may initiate a crossing, then
start the receiver and controller. Each side observes only its own Local edge
until a crossing is admitted. The equivalent CLI forms are:

```powershell
# accepting PC, with reciprocal roaming enabled
desklink_pair.exe serve 43821 --capture `
  --edge-roaming "$env:LOCALAPPDATA\DeskLink\roaming.settings"

# connecting PC
desklink_pair.exe focus 192.168.1.25 43821 --capture `
  --edge-roaming "$env:LOCALAPPDATA\DeskLink\roaming.settings"
```

One authenticated connection owns both directions, but only one direction may
be pending or active at a time. Each crossing requires the destination's
independent `InputInject` grant, the trusted current session, fresh nonce and
topology, an explicit Ready route, fresh `FocusReady`, accepted landing and
snapshot, and direction arbitration. Simultaneous opposite attempts return
both sides Local. Any failure keeps or returns input Local; there is no
automatic fallback to manual capture or a different route.

When both edge segments have trustworthy EDID-backed landscape dimensions,
DeskLink preserves the pointer's physical distance along the shared edge. It
uses this only as a landing hint: estimated, rotated, contradictory, or short
geometry falls back to deterministic proportional mapping and never changes
route, peer, or capability admission. Crossing also requires bounded outward
intent so motion along an edge does not roam accidentally. These behaviors and
the deterministic reciprocal fault/soak harness are automated, but controlled
roaming remains experimental until the deferred two-supported-PC physical
matrix verifies cursor visibility, device timing, feel, and real fault recovery.

Audio is separately opt-in and requires complementary grants on both PCs. To
send PC2's system mix to PC1, pair PC1 with `--grant-audio-send` and pair PC2
with `--grant-audio-receive`, then run:

```powershell
# PC2
desklink_pair.exe serve 43821 --send-audio

# PC1
desklink_pair.exe focus 192.168.1.25 43821 --receive-audio
```

`--send-audio` starts loopback only after the trusted session is admitted and
the peer has `AudioReceive`. `--receive-audio` starts rendering only after
admission and an `AudioSend` grant. Audio failure stops only the audio module;
it does not change input focus, identity, TLS, or session admission.
Default render-device changes and recoverable WASAPI failures reopen audio with
capped 250 ms to 5 s backoff while that admitted session remains active.
Capture send/client rejection is not retried.
On the receiving Host, `control gain` applies a per-peer `0..10000` render
gain and `control mute` toggles mute. Changes ramp across one five-millisecond
block, persist through audio-only endpoint recovery, and never change the
Windows endpoint or system mixer volume.

Text clipboard synchronization is separately opt-in and requires complementary
grants. To synchronize both directions, pair each PC with both clipboard grants,
then add `--sync-clipboard` to `serve` and `focus`:

```powershell
# accepting PC
desklink_pair.exe serve 43821 --sync-clipboard

# connecting PC
desklink_pair.exe focus 192.168.1.25 43821 --sync-clipboard
```

No clipboard message is admitted until `PeerValidated`, fresh nonce negotiation,
immutable capability exchange, and the clipboard-module handshake all complete.
DeskLink synchronizes only `CF_UNICODETEXT`, enforces strict UTF-8 and a 48 KiB
limit, sends no more than 20 updates per second, caps pending remote writes at
eight, and rejects stale update IDs, wrong peers/nonces, malformed data, and
one-sided consent. Existing trust records are never upgraded. Clipboard content
is memory-only and never logged. Clipboard contention or adapter failure stops
only that update; it cannot change focus, input routing, audio, identity, or TLS.
Image clipboard and file transfer remain out of scope.

The Host CLI accepts an optional fallback mode plus at most 32 exact executable
basename rules:

```powershell
desklink_pair.exe focus 192.168.1.25 43821 --capture `
  --default-mode roam `
  --profile-fullscreen game.exe=game `
  --profile editor.exe=lock-pc1
```

Modes are `roam`, `lock-pc1`, `lock-pc2`, and `game`. Rules contain no paths,
wildcards, regular expressions, or command lines. `control mode` remains an
explicit process-lifetime manual override and therefore outranks profile and
default decisions; Ctrl+Alt+Pause remains the highest-precedence emergency
fail-local action. When rules exist and the foreground process is not
inspectable, the effective mode is `lock-pc1`.

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
    clipboard.hpp        Text clipboard consent/nonce/replay gates
    audio.hpp            Exact audio framing and jitter/reorder buffer
    transport.hpp        Authenticated transport abstraction
    win32_input.hpp      Windows SendInput adapter
    win32_capture.hpp    Physical input and fail-local suppression adapter
    win32_clipboard.hpp  Bounded CF_UNICODETEXT synchronization adapter
    win32_audio.hpp      WASAPI loopback capture and shared render adapters

src/
    protocol.cpp
    focus.cpp
    host.cpp
    session.cpp
    agent.cpp
    audio.cpp
    in_memory_transport.cpp
    win32_input.cpp
    win32_audio.cpp

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
    PLATFORM_SUPPORT.md
    ROADMAP.md
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
