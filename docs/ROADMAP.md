# DeskLink Roadmap

## Product direction

DeskLink targets the best coherent-desk experience for two or more trusted
Windows PCs. The roadmap does not pursue Synergy-style cross-platform feature
parity while Windows roaming, failure recovery, installation, and daily-use UX
remain unfinished. Windows-specific display, input, audio, profile, and
security integration is a product advantage rather than an abstraction to
remove.

The public reliability invariant is:

> DeskLink never depends on the remote computer to give your input back.

Every release milestone must preserve fail-local ownership: transport loss,
process failure, lease expiry, topology invalidation, update/restart, or a
module-local failure releases DeskLink-owned key/button state, disables remote
suppression, and leaves control local. Audio, clipboard, MCP, and later modules
must fail independently and cannot take down or retain input.

## Completed foundation

- portable protocol, capability, lease, epoch, and session core
- persistent non-exportable CNG identity and DPAPI trust store
- mutually pinned MsQuic/Schannel pairing and operational sessions
- integrity-checked runtime-selection foundation from PR #9 / `10147fe`
- opt-in low-level keyboard/Raw Input mouse capture, fail-local suppression,
  and `SendInput` injection
- periodic reliable input-state reconciliation with owned-state convergence
- protocol-v2 relative pointer motion with bounded gain/DPI calibration, while
  retaining absolute display packets for future edge transitions
- mixed-provider physical pointer feel validated at 100% gain with raw DPI;
  source virtual-desktop width no longer affects remote motion speed

## Current roadmap

Work proceeds in this order:

1. **Deferred by owner:** validate the complete input path on two real Windows 11 or Windows Server
   2022-or-newer PCs, including cable removal, client termination, lease expiry,
   reconnect nonce rotation, stale epoch rejection, and Ctrl+Alt+Pause fail-local.
   A second Windows 11 target is not currently available.
2. **Complete:** stable multi-monitor enumeration, identity, rectangle mapping,
   and topology-change invalidation before edge roaming. Active Windows
   DisplayConfig target paths produce deterministic IDs; ambiguous/colliding
   identities and stale generations fail closed.
3. **Complete:** bounded reliable mouse-wheel transport. The low-level hook
   enqueues physical vertical/horizontal wheel input before suppression and
   fails local on invalid deltas, queue contention/overflow, or forwarding
   failure.
4. **Complete:** current-user named-pipe control API. The versioned SID-derived
   endpoint uses an explicit one-user DACL, rejects remote clients, verifies
   both process-token SIDs, bounds frames/timeouts, and exposes only typed state
   and desired-mode operations. Restrictive local modes cannot be weakened by
   a remote peer.
5. **Phases 1-2 complete:** bounded monitor presentation metadata, strict
   directional edge graph, stable-identity/current-generation resolution, and
   atomic current-user preference persistence. Canvas layout and physical size
   are presentation-only and cannot influence runtime routing. Explicit
   capability-gated topology snapshots now use the authenticated reliable
   session, bind to the current peer/machine/nonce, expire after five seconds,
   and keep routes unready on missing, stale, malformed, oversized, or rejected
   state.
6. **Phase 3 complete:** the native companion now exposes a presentation-only,
   physical-size-scaled per-PC canvas, saved/offline displays, explicit
   bidirectional adjacency suggestions, advanced one-way/partial-edge editing,
   route-resolution status, atomic Local-before-save replacement, and
   five-second local Identify overlays. Bounded read-only topology IPC remains
   same-SID and does not expose focus or trust mutation. The native broker owns
   the tray and hotkeys; the single-instance WinUI settings process exits when
   closed, while ordered fail-local exit and a short-lived user-scoped sign-in
   bootstrap are implemented.
7. **Complete:** bounded LAN DNS-SD/mDNS discovery. `listen` and `serve`
   advertise `_desklink._udp.local`; `discover [1..30 seconds]` reports strict,
   expiring, deterministic candidates without connecting, pairing, granting a
   capability, or writing trust. Conflicting duplicate identities are marked
   ambiguous.
8. **Complete foundation:** bounded foreground profile evaluation and native
   Windows foreground observation. Rules match exact executable basenames with
   an optional fullscreen requirement; precedence is emergency, manual,
   profile, then default. A configured but uninspectable foreground produces a
   fail-local decision. The WinEvent source owns its message-loop thread and
   unhooks on that same thread.
9. **Complete safety boundary:** the portable Host lifecycle disables routing,
   releases remote focus, synchronously tears down capture hooks, and then
   enters `GAME`/`LOCK_PC1`. Leaving a restricted mode requests a fresh focus
   transaction; capture cannot restart or enable until `FocusReady` and the
   initial state snapshot succeed. Win32 capture is restart-safe.
10. **Complete:** foreground/profile decisions and bounded profile configuration
   are wired into the production Host CLI through one serialized lifecycle
   owner. Exact `exe=mode` rules, fullscreen-only rules, current-user manual
   overrides, `FocusReady`, renewal, and capture failures share the same ordered
   event boundary. Renewal and reconciliation run only while Remote.
11. **Complete foundation:** event-driven shared-mode WASAPI loopback capture
    and render adapters normalize to exact 48 kHz/stereo/PCM16/5 ms blocks.
    Capture packet handling, renderer submission, and renderer buffering are
    bounded; silence, discontinuity, underrun, and module-local failure paths
    are explicit.
12. **Complete:** capability-gated audio session/datagram wiring and receiver
    jitter/render pumping. Sender and receiver require complementary local trust
    grants plus explicit runtime opt-in after `PeerValidated`; nonce, format,
    stream, sequence, and bounded queue checks precede playout.
13. **Complete:** audio endpoint-loss/recovery with audio-only restart
    semantics. Scoped endpoint notifications stop stale WASAPI workers;
    restart-safe adapters reopen the current default endpoint with capped
    250 ms to 5 s backoff while the admitted session and input remain intact.
    Client/send rejection never triggers reopen.
14. **Complete:** bounded adaptive jitter targeting. The receiver compares
    capture and steady arrival deltas, raises the 2-12 block target immediately
    on spikes/concealment, enters an explicit rebuffer state when latency must
    grow, and requires 200 stable samples for each one-block decrease.
15. **Complete:** native Windows engineering-alpha wrapper and portable ZIP.
    Typed fields launch the existing pairing/session executable without a shell;
    controller sessions start Local, runtime status and mode use the same-user
    control pipe, diagnostics remain bounded/in-memory, and the package contains
    only the Schannel production runtime.
16. **Complete:** bounded clock-drift correction. A 400-sample occupancy window
    slews an asynchronous linear resampler in 50 ppm steps within a hard
    ±1000 ppm bound; source buffering remains capped at four blocks and
    discontinuities clear correction state.
17. **Complete:** per-peer audio gain/mute. The active Host receiver applies
    bounded attenuation after drift correction, ramps changes over one block,
    preserves policy across audio-only recovery, and exposes typed CLI/wrapper
    controls without changing the system mixer.
18. **Phase 4 controlled reciprocal slice complete:** the portable state machine
    implements all three crossing policies, proportional corner-safe landing,
    re-entry cooldown, bounded focus timeout, active-route invalidation, and
    stale direction-token rejection. The Windows Host observes local Raw Input
    without suppression and can enter Remote only after current trusted
    session/capability/nonce/topology checks, fresh `FocusReady`, landing,
    initial reconciliation, and direction admission. The Alpha switch and CLI
    path are explicit experimental opt-ins. A single `PeerSession` now owns
    incoming and outgoing coordination, independent directional grants,
    nonce-bound tokens, collision-to-Local, and duplicate-startup convergence.
    Physical two-PC qualification remains open.

The real two-PC Windows 11 failure matrix remains required for production
qualification, but the approved automated and controlled experimental roaming
work does not wait for unavailable hardware.

## Post-roaming milestones

Work proceeds in this order. Automated and single-machine fault work may begin
without unavailable hardware, but a milestone that requires two supported
Windows 11/Server 2022+ systems cannot receive production sign-off until that
physical matrix runs.

### 1. Symmetric roaming ownership — complete

- integrate `PeerDirectionArbiter` with a reusable reciprocal peer-session
  owner so either trusted machine can initiate an edge crossing;
- converge simultaneous opposite focus attempts deterministically, bind every
  direction token to the authenticated peer/session nonce, and never restore
  remote focus automatically after reconnect; and
- keep startup, duplicate-session convergence, reconnect, and all error paths
  Local until a new focus transaction, landing, and reconciliation succeed.

Implemented with one validated reciprocal `PeerSession`. Portable tests cover
both directions, independent capability grants, simultaneous opposite focus,
peer/nonce/generation token binding, reconnect invalidation, and malformed or
changing capability replay. The Alpha/CLI listener can opt into capture only
with an explicit absolute roaming-settings path. Physical two-PC signoff stays
in milestone 3; it does not reopen this ownership design.

### 2. Physical-distance-aware crossing polish — automated implementation complete

- preserve the source pointer's distance along a shared physical edge, in
  millimeters, when both displays have bounded trustworthy physical metadata;
- promote physical dimensions only into a separately validated optional
  landing hint. They must never select a route, peer, or capability, and saved
  canvas coordinates remain presentation-only;
- retain the current proportional segment mapping as the deterministic fallback
  for missing, estimated, contradictory, rotated, or too-short geometry;
- add bounded outward-versus-lateral intent scoring so motion along an edge does
  not cross accidentally, while high-velocity/high-poll-rate input cannot skip
  the configured threshold; and
- tune corner clearance, cooldown, partial edges, and all three crossing
  policies with recorded, replayable traces before choosing a production
  default.

The runtime now treats physical distance as an optional landing hint only when
both displays report EDID-backed, landscape geometry. The shared edge must be
at least 25 mm and the two physical spans must agree within the larger of 5 mm
or 5%; estimated, rotated, contradictory, or short geometry deterministically
uses the existing proportional mapping. Route selection, peer/capability
admission, and saved canvas positions remain independent of physical metadata.
The default **Cross immediately** policy fires on the first positive outward
Raw Input count at an exact configured edge. It deliberately ignores lateral
distance so diagonal approaches do not depend on mouse polling rate or require
repeated pushes. Lateral-only motion still cannot cross, and the exact route,
session, topology, capability, and re-entry cooldown gates remain mandatory.
The advanced pause-and-push and double-push policies retain bounded 60%
outward-intent scoring and their configured thresholds. Recorded traces cover
edge skimming, first-contact high-poll-rate input, high velocity, diagonal
intent, corners, partial edges, all three policies, cooldown, and both crossing
directions.

### 3. Reliability and failure qualification — automated harness complete

Build an automated fault/soak harness first, then run the same scenarios on two
supported physical systems. Include 8 kHz mouse input; held key/button chords;
wheel and audio load; cable removal and Wi-Fi reconnect; destination
sleep/wake; process kill/restart; monitor disable/re-enable and hot-plug;
dock/GPU/DPI changes; RDP attach/detach; and repeated roaming over long runs.

The hard acceptance criteria are no stuck key, no stuck button, no permanently
captured or hidden cursor, no stale focus/session/epoch/lease/topology
admission, no unauthenticated reconnect, and no audio or later module failure
affecting input cleanup. Every failure must produce bounded diagnostics and a
deterministic Local state. The owner-deferred two-Windows-11 matrix remains the
final production gate.

The portable deterministic harness is now a default CTest gate. It alternates
reciprocal directions and threshold/high-velocity crossings, exercises held
key/button cleanup plus wheel/audio load, and cycles graceful return, cable
loss, nonce rotation, peer/local topology change, capability revocation, focus
timeout, sleep/wake, process termination, and RDP detach. Every cycle must clear
active focus, settle Local/LocalCooldown, reject outward motion during cooldown,
and release cooldown only on inward motion. CI runs 2,000 iterations; bounded
manual runs support up to 1,000,000 iterations with an explicit deterministic
seed. Real Wi-Fi, endpoint, dock/GPU/DPI, cursor visibility, process, and monitor
faults still require the deferred physical matrix and are not claimed by the
model-based harness.

### 4. Capability-scoped text clipboard

**Automated foundation complete; experimental pending physical qualification.**
The portable/session and Windows runtime now implement:

- the text-only implementation uses the existing independent `ClipboardRead` and
  `ClipboardWrite` grants; pairing alone grants neither, existing trust records
  are never upgraded silently, and per-peer synchronization defaults off;
- the authenticated reliable lane uses `PeerValidated`, current session
  nonce, strict framing, bounded text size/rate/queue depth, update identifiers,
  and loop suppression before touching the Windows clipboard;
- malformed, oversized, stale, replayed, unsupported-format, or
  wrong-direction updates are rejected without retrying through another
  capability or input injection; and
- image clipboard and file transfer remain out of this slice until text clipboard
  passes privacy, reconnect, contention, clipboard-owner exit, and fuzz/fault
  validation. Clipboard failure must never change focus or input routing.

Strict framing/UTF-8 bounds, complementary and default-off consent, module
negotiation, nonce/origin/update replay gates, reconnect reset, callback-failure
isolation, typed launcher arguments, and the Windows text-only bounded adapter
are automated. Physical two-PC privacy, rapid-contention/coalescing behavior,
clipboard-owner exit, and real reconnect tests remain required before this item
is production-qualified. Image clipboard and file transfer remain excluded.

### 5. PTT voice forwarding — automated implementation complete

- protocol 5 adds independent default-off `VoiceSend`/`VoiceReceive` grants and
  a strictly bounded datagram-only Opus voice frame;
- the product and runtime expose separate voice route, communications
  microphone selection, incoming gain, hard mute, PTT state, and default-on
  half-duplex echo guard without modifying pairing or system-audio policy;
- capture opens only for a local PTT press after reciprocal acknowledged
  grants and closes on release, mute, revocation, disconnect, endpoint loss,
  configuration change, or shutdown;
- pinned Opus 1.6.1, FEC/PLC, and a bounded adaptive 40-120 ms jitter target
  are covered by portable/native tests; and
- production sign-off remains open until the two-PC privacy, device,
  loss/reorder, feedback, reconnect, and system-audio regression matrix passes.
  Full criteria are in [`VOICE_FORWARDING.md`](VOICE_FORWARDING.md).

Global PTT and full acoustic echo cancellation are explicitly deferred. Voice
must not be described as production-qualified before physical validation.

### 5.1 Virtual microphone application routing — implementation complete; certification open

- preferences schema 6 adds the local-only received-voice destination and
  migrates every existing install to communications playback;
- one decoded 48 kHz mono PCM16 playout block fans out through
  `VoiceOutputRouter` to communications monitoring, the virtual feed, or both,
  with independent sink failure and monitor-only gain/echo policy;
- the optional x64 WaveRT driver is a narrow pinned derivative of Microsoft's
  Simple Audio Sample, exposing `DeskLink Microphone Feed` and the genuine
  `DeskLink Remote Microphone` capture endpoint through stable properties;
- its fixed 40 ms target/60 ms maximum nonpaged ring drops oldest audio,
  returns silence on underrun, and flushes on feed/capture lifecycle boundaries;
- a stable-property filter prevents the virtual capture endpoint from becoming
  an outgoing DeskLink microphone, and no new network message or grant exists;
- the fixed UAC helper and application installer admit only an externally
  supplied Microsoft production-signed package; normal builds and all other
  DeskLink features remain driver-independent; and
- automated core, native, package, Inf2Cat, safety, and generic Core Audio
  validation are implemented. Physical zero-microphone, two-PC, Discord, and
  lifecycle qualification remain blocked on Microsoft production driver
  signing and must not be bypassed with test mode or reduced security.

### 6. Installation and daily-use productization

- **Product UX PRs 1-3 complete:** safer default-off pairing grants, the
  versioned role/preferences/planner model, and the single current-user broker
  and trust-authority contract are merged;
- **Product UX PR 4 complete:** the broker now owns persistent role-driven
  Companion listen/advertise and exact preferred-peer Main auto-connect,
  pause/resume, bounded availability-only backoff, and network/process/config
  reconciliation. Reconnect remains Local, rotates the session nonce through
  the normal trusted bootstrap, performs transport-loss input cleanup before
  notifying the supervisor, and treats nonzero peer protocol shutdown as
  action-required;
- **Product UX PR 5 implemented:** the locked, self-contained C++/WinRT shell
  foundation provides Home, Advanced, and Diagnostics over simulated broker
  states plus single-instance activation, on-demand WinUI lifetime, native
  broker-owned tray/hotkeys, explicit exit, and coordinated-update behavior.
  The installer carries its exact 243-file
  allowlisted runtime. PR 9A later promotes it to the installed default;
  Production signing and clean supported-system accessibility/update
  qualification remain release gates;
- **Product UX PR 6 implemented:** the shell consumes real broker state and
  provides first-run role selection, bounded untrusted Nearby results, manual
  fallback, broker-owned pairing confirmation, and authenticated Devices &
  permissions. Pairing is an exclusive managed child with a random operation
  token and expiring candidate lease; shell loss, timeout, ambiguity, or token
  mismatch rejects. Generic IPC still cannot increase authority, while revoke
  and Forget retain fail-local cleanup;
- **Product UX PR 7 implemented:** Arrange displays is now a production-shell
  page with physically scaled per-PC cards, stable identities, online/offline
  state, local Identify overlays, deterministic snap proposals, and explicit
  acceptance. A keyboard-accessible editor supplies card movement and full-edge
  two-way authoring without the canvas; one-way and partial segments remain in
  Advanced. Saving strictly validates the complete graph, confirms Local when
  capture is active, atomically replaces the settings file, automatically arms
  any enabled route, and live-reloads the authenticated session. Canvas
  movement alone never changes routing;
- **Product UX PR 8 implemented:** Home exposes concrete preferred-PC
  clipboard/audio intent, bounded peer-audio volume/mute, fullscreen keep-local
  behavior, three crossing presets, and fixed-list local focus/return hotkeys.
  Advanced persists at most 32 exact executable/fullscreen rules. Missing
  permission never creates consent, focus shortcuts use the named authenticated
  admission path, the capture hook owns Return-to-this-PC while remote,
  crossing changes reuse Local-before-atomic-save, and required
  but uninspectable foreground state remains Local. Preferences schema 6
  retains those migrations and adds a local-only received-voice destination
  that defaults older installs to communications playback;
- **Product UX PR 9A implemented:** `desklink.exe` is now the normal Start menu
  and post-install entry point. Its `--background` mode is a short-lived
  sign-in bootstrap, while updates restart the native broker directly. Every
  launch starts only the fixed sibling broker with a bounded no-shell launch.
  Alpha remains an
  explicitly labeled diagnostics fallback for one migration release. Setup
  migrates only an exact legacy Alpha Run command. The updater anchors signer
  trust on the product shell, runs separate shell and broker/state health
  probes, restores the exact pre-update Run value after rollback, and never
  owns `%LOCALAPPDATA%\DeskLink` or the CNG identity. Disposable-account tests
  compare the complete identity snapshot plus byte hashes for a real DPAPI
  trust record, schema-4 preferences, and saved roaming graph across rollback
  and upgrade;
- **Product UX PR 9B automated qualification implemented:** invalid release
  certificate/timestamp policy, product DPI/accessibility/theme/UTF-8 metadata,
  elevation/Firewall drift, and same-version repair are CI failures. The exact
  MsQuic-job artifacts repeat install/repair/rollback/upgrade/uninstall on a
  disposable Server 2022 worker. Native suspend/resume handling stops the
  transport Local and permits only a fresh session after wake while preserving
  user pause and security `ActionRequired`. Real production signing, manual
  supported-system accessibility/power/network testing, clean Windows 11, and
  the deferred two-Windows-11 physical failure matrix remain open;
- **Installer foundation complete:** current-user fixed-path Setup/Uninstall,
  active-runtime mutex gates, exact payload and pinned Schannel runtime checks,
  same-version repair, in-place upgrade, startup-value cleanup, state
  preservation, a fail-closed production signing gate, and artifact-identical
  Windows-latest/Server-2022 disposable-account CI validation. Production
  signing and clean Windows 11 qualification remain open;
- **Automated update foundation complete:** explicit same-signer candidate and
  current-version rollback packages are hash/version validated before an ordered
  `Return Local -> confirm local -> stop runtime/UI -> install/validate` flow.
  Candidate install/health failure rolls back before any optional restart.
  Production-signed and destructive-fault qualification remain open; and
- validate current-user startup plus Private/Domain firewall onboarding,
  production-signed repair/upgrades, physical sleep/resume and endpoint
  changes, and uninstall on clean supported systems.

### 7. MCP observation plane

Expose DeskLink as a machine-readable trusted desk, initially through a
default-off local current-user MCP server (stdio or the existing same-SID local
boundary), not a new unauthenticated LAN listener. Provide bounded read-only
resources such as `desklink://desk`, `desklink://peers/{id}`,
`desklink://topology`, connection/route state, explicit capability grants, and
current DeskLink mode. Foreground-process and workspace metadata require
separate opt-in and minimization. Never expose private keys, trust-store
secrets, clipboard content, raw input, or unrestricted filesystem/process data.

Define and threat-model an MCP-client identity/authorization boundary distinct
from peer pairing before shipping this phase. Resource content and peer/job
output are untrusted data, cannot grant capabilities, and cannot override local
policy or user approval.

### 8. Bounded MCP actions

- add narrow typed tools such as `return_local`, `set_mode`, `focus_peer`,
  `set_audio_route`, `identify_display`, and `request_capability` rather than a
  generic shell primitive;
- introduce explicit default-off MCP permissions independently of existing
  human peer grants, with no wildcard/admin permission and no silent migration;
- require current client authorization plus the normal trusted peer, capability,
  nonce, epoch, lease, and operation admission checks where applicable; and
- provide bounded audit records, cancellation/timeouts, idempotency tokens, and
  explicit user confirmation for security-sensitive or disruptive actions.
  MCP loss or client cancellation must return affected control operations to a
  safe local state.

### 9. Cross-PC workspace and job orchestration

After the observation and bounded-action surfaces pass security review, add
separate permissions for workspace read/write, build, test, process launch, and
artifact transfer. Scope every workspace to a canonical locally approved root.
Prefer named, locally configured build/test/launch profiles and typed job
handles over arbitrary command strings; constrain environment, duration,
output, concurrency, artifact size, and destination. Hash artifacts in transit,
bind jobs/results to the initiating client and peer session, preserve result
provenance, and require fresh approval for privilege or scope expansion.

The intended workflow is source inspection on one PC, an approved build on an
appropriate peer, deployment of a verified artifact, bounded physical or
automated validation, and collection of results. There remains no normal
`remote.shell` capability.

### 10. Later extensions

- image clipboard and explicit file transfer after their independent
  capability/privacy/security design and text-clipboard qualification;
- Explorer drag/drop only as a presentation over the explicit transfer model,
  never as implicit clipboard authority or generic remote filesystem access;
- richer automatic audio routing and per-application desk profiles;
- Stream Deck integration implemented as a narrowly permissioned local client
  of the same typed control model; and
- broader UX polish informed by real soak/telemetry-free diagnostic traces.

## Experimental Windows 10 compatibility project

Windows 11/Windows Server 2022 production support remains stock
MsQuic/Schannel with the existing non-exportable CNG identity. Windows 10 stays
unsupported while an approved equal-security R&D path proceeds through
independent gates:

1. **Complete:** upgrade and validate the shared MsQuic foundation on stable 2.6.x
2. **Complete:** implement the explicit opaque
   CNG/OpenSSL provider boundary
3. **Complete:** prove fail-closed validation and application admission with an
   explicit `PeerValidated` state, a four-second validation watchdog, and the
   required negative OpenSSL/CNG matrix
4. **Complete:** prove device-identity invariance and absence of private-key export paths
5. **Complete:** guarded Windows 11 Schannel to Windows 10 OpenSSL physical
   validation. Manual mutual-confirmation pairing, trusted reconnect, fresh
   nonce rotation, focus without capture, physical keyboard/button/pointer
   forwarding, interactive `SendInput` observation, emergency release, and
   deterministic two-PC snapshot reconciliation pass. Abrupt process termination
   with held key/button state also passes lease cleanup. A scoped four-second
   network interruption fails local on focus-lease renewal; a fresh reconnect
   rotates the nonce and rejects live prior-session and stale-epoch packets.
6. **Beta 1 qualification implemented:** the current product shell and broker are
   packaged for Windows 10 22H2 with both reviewed runtime graphs. Broker-owned
   operations use the fail-closed OS `auto` policy; Alpha/direct requests stay
   Schannel-pinned. The unsigned prerelease is a distribution/qualification
   vehicle, not production admission.

Completion of the R&D gates does not by itself make the prototype a production
artifact. Windows 10 stays experimental/unsupported until a separately reviewed
production-admission and release-integration change is approved.

No later stage begins before the preceding stage passes. Full constraints are
in [`PLATFORM_SUPPORT.md`](PLATFORM_SUPPORT.md).

## Far-future R&D backlog — no implementation authorization

Remote-video and network-display work comes after every current roaming,
clipboard, audio, product UX, installer/update, signing, and supported two-PC
physical gate. It is not part of the active roadmap or the product UX PR series.

If future product evidence justifies the investment, the order is:

1. Design remote-screen KVM video as an independent capability and protocol,
   including capture, hardware/software codec negotiation, congestion and
   latency control, presentation, cursor composition, protected-content and
   secure-desktop boundaries, reconnect behavior, and module-isolated failure.
2. Build and qualify that project without changing the existing device identity,
   trust, pinning, `PeerValidated`, nonce, epoch, lease, or fail-local model.
3. Only then investigate extended-display mode as a separate Windows driver and
   video project using an explicitly installed, production-signed Indirect
   Display Driver and a separately threat-modeled frame pipeline.

Extended-display mode is intentionally last. DeskLink's current topology and
input work can provide bounded display metadata and an authenticated return
path, but it does not provide a virtual monitor, GPU-frame transport, codec,
decoder, or renderer. No current milestone may add a display driver, capture
surface, video lane, automatic driver installation, or new elevated service in
anticipation of this work.

Any future approval must define protected-content, lock/UAC/secure-desktop,
driver update/rollback, bandwidth exhaustion, malformed bitstream, decoder/GPU
failure, display detach, sleep/resume, and network-loss behavior. Validation and
application admission remain fail-closed, 0-RTT remains disabled, and video or
driver failure must not retain input or weaken the ordinary roaming path.
