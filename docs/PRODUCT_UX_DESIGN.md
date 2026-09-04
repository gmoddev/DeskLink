# DeskLink product UX redesign

Status: **Approved — implementation authorized in gated PRs 1-9**

Review basis: `main` at `a45e963`, plus the current uncommitted topology/UTF-8
fixes

Date: 2026-08-26

## 1. Decision summary

DeskLink should move from an engineering launcher to a configure-once desktop
product without moving security or latency-sensitive work into the UI.

The proposed product model is:

- **Main PC** — use this PC's keyboard and mouse across the desk.
- **Companion PC** — control this PC from a paired Main PC.
- **Flexible** — retain reciprocal and advanced/manual behavior.

These are persisted product preferences. They are not new protocol roles,
authentication roles, or security authorities. Runtime Host/Agent and
Controller/Receiver state remains dynamic, and changing a product role never
creates, broadens, or replaces a trust grant.

The proposed process model is:

```text
On-demand WinUI 3 settings shell
        │
        │ bounded same-user typed control protocol
        ▼
Per-user native DeskLink runtime broker + tray/hotkeys
        │
        ├── existing identity / trust / pairing
        ├── existing MsQuic transport and PeerValidated gate
        ├── existing focus / capture / roaming state machines
        ├── existing audio / clipboard modules
        └── existing topology and profile engines

desklink_pair.exe remains a diagnostic and compatibility CLI
```

The broker is an ordinary current-user process, not a Windows service and not
permanently elevated. It becomes the single owner of the current-user control
endpoint and runtime lifecycle. The visible shell may open and fully exit
without stopping an active, locally safe runtime or removing its lightweight
native tray and product hotkeys.

The target production shell is WinUI 3 with C++/WinRT, gated by an early
deployment prototype. The existing fixed-coordinate Alpha wrapper remains a
diagnostic/migration surface until the replacement passes packaging,
accessibility, lifecycle, and update qualification.

## 2. Validation of the supplied UX review

| Review claim | Verdict | Repository evidence or correction |
| --- | --- | --- |
| Generic pairing preselects input injection | Valid, high-priority product/security default | `desklink_alpha.cpp` checks `GrantInput` during control creation. The later pairing dialog still requires explicit confirmation, so this is not an authentication bypass, but it presents a persistent high-authority grant as an ordinary default. |
| Users manually operate receiver/controller/focus state | Valid | The Alpha surface exposes `Start receiver`, `Start controller`, and `Focus remote`, and persists none of those desired runtime choices. |
| Discovery is reduced to diagnostic child output | Valid | The native discovery backend has structured bounded results, but Alpha launches `desklink_pair discover` and displays captured text rather than a device model. |
| Address and UDP port dominate normal setup | Valid | Both are permanent controls in the main Alpha window and are required by its launcher request. |
| Permissions and runtime module choices are cognitively mixed | Valid | Pairing grants and session flags are separate controls even when they jointly represent one user intention. That separation must remain internally. |
| There is no usable trust-management UI | Valid, with an important correction | `ITrustStore` can get, save, and remove a known peer, but it cannot enumerate peers and the control protocol exposes no trust operation. A Devices page therefore requires a new bounded enumeration and mutation surface; the UI must not parse `trust.db`. |
| Pointer calibration and engineering status are overexposed | Valid | Gain, DPI, raw role/mode, machine IDs, diagnostics, and explicit process controls are all on Home. |
| The monitor canvas is sound but advanced routing dominates | Valid | Stable display identities, atomic save, Local-before-save, and adjacency suggestions already exist. Partial edge percentages and one-way direction occupy the primary right panel. |
| Foreground profiles are useful but CLI-oriented | Valid | The runtime supports bounded executable/fullscreen rules and fail-local precedence; Alpha does not expose them. |
| Persistent application preferences are too small | Valid | The settings schema stores only close-to-tray, run-at-login, and first-run completion. |
| Fixed Win32 layout is not a production shell | Valid | Alpha and the configurator use fixed pixel sizes/positions, do not handle `WM_DPICHANGED`, and the canvas uses hard-coded RGB colors. |
| Nearby cards can show the peer OS before pairing | Not valid with today's discovery schema | The advertisement carries protocol, machine ID, display name, capability hints, pairing state, host, and port. It does not carry an authenticated OS version. The UI must show **Unverified nearby PC**, not “Windows 11,” until authenticated metadata exists. |
| One feature switch can directly configure both peers' grants | Valid only as an orchestration goal | A simple switch may start a two-PC approval flow, but each PC remains authoritative over its own grants. A Main PC cannot silently enable a Companion's input, clipboard, audio, or future system/file authority. |
| WinUI 3 is the appropriate production direction | Valid with a deployment gate | Microsoft recommends WinUI 3 for new native Windows desktop applications and supports C++ and C#. DeskLink must first prove Windows App SDK deployment, signed installer/update integration, tray/single-instance behavior, and clean-system startup. |

The competitor references support simplifying setup and monitor arrangement,
but they do not define DeskLink's security model. Mouse Without Borders exposes
connection credentials followed by a draggable machine layout. Input Leap's
documented server/client, screen-name, and address flow is closer to the model
DeskLink should leave behind.

### 2.1 Competitive feature intake

Synergy is the appropriate benchmark for DeskLink's current product category:
its primary experience is moving the pointer across a screen edge to control a
different computer, with keyboard and clipboard following. Multiplicity is a
useful broader feature reference, but its remote-video and Seamless Display
features are separate product classes rather than evidence that DeskLink's
roaming implementation is nearly an extended-display implementation.

| Idea from the overview | Validation | Roadmap disposition |
| --- | --- | --- |
| Intentional crossing controls and direct switching hotkeys | Good fit, mostly existing foundation | Expose the existing Push, Dwell-and-Push, and Double-Push policies with plain-language presets. Add bounded local hotkey authoring in PR 8. A hotkey may request normal focus but cannot bypass route, peer, capability, nonce, epoch, lease, topology, or `PeerValidated` admission. |
| Image and formatted clipboard | Useful breadth after text is qualified | Later, separately capability/privacy-reviewed clipboard-format extension. Keep strict type/size/rate bounds, local consent, delayed-render handling, and module-only failure containment. |
| File transfer and Explorer drag/drop | High-value daily-use feature, but not “just clipboard” | Later independent transfer design after the production UX and text-clipboard qualification. Require explicit directional authority, user-selected source objects, bounded size/rate/queueing, safe destination naming, cancellation, integrity checks, and no generic filesystem access. |
| Better audio routing and recovery presentation | Directly aligned | Keep in the current audio/productization roadmap. Present named routes and actionable endpoint/recovery states without weakening the existing independent audio capability gates. |
| Multiple Companion PCs | Architecturally plausible, not part of the first slice | Keep the trust/device model bounded for multiple peers, but require a separately approved scheduler and conflict design before simultaneous automatic control. |
| Remote-screen KVM video | Technically possible, substantially new scope | Later R&D only, after the complete local-desk product is production-qualified. It requires a screen-capture, codec, congestion, presentation, cursor, protected-content, and session-security design; existing QUIC and input code are only supporting components. |
| Use another PC as a true Windows extended display | Technically possible, but the most distant item | Far-future R&D only. Windows can support network/virtual displays through an Indirect Display Driver, but DeskLink would need a signed driver plus a complete GPU-frame, encode, transport, decode, and presentation pipeline. Current monitor topology is useful metadata, not an implementation shortcut. |
| Cross-platform parity | Does not match the current product direction | Continue to prioritize a coherent, reliable Windows desk. Do not abstract away Windows security, display, input, audio, profile, installer, and update advantages while the Windows product remains unfinished. |

The resulting product order is intentionally asymmetric:

1. Finish and physically qualify roaming, text clipboard, audio, installer,
   updater, broker, and production UX.
2. Add intentional-crossing/hotkey presentation as part of that UX.
3. Design and qualify richer clipboard formats, then explicit file transfer.
4. Consider remote-screen KVM video only as a separate later project.
5. Consider extended-display mode last, after remote-video experience and only
   under a new security, driver, deployment, and physical-validation approval.

Extended-display mode is not part of PRs 1-9, is not implied by approval of this
document, and must not introduce an Indirect Display Driver, screen capture, or
video protocol dependency into the current product architecture.

## 3. Goals

1. A normal two-PC desk can be configured once and operate after sign-in without
   manually starting listener/controller processes.
2. Home communicates user state rather than implementation state.
3. Discovery, pairing, trust review, feature consent, and monitor arrangement
   form one coherent workflow.
4. Sensitive authority remains explicit, directional, locally approved,
   inspectable, and revocable.
5. The UI never owns QUIC, private-key operations, capture hooks, input
   injection, audio timing, or raw clipboard/input content.
6. Failure, ambiguity, stale state, UI exit, or broker/UI disagreement leaves
   keyboard and mouse Local.
7. Flexible mode preserves the existing reciprocal, partial-edge, one-way,
   profile, and diagnostic functionality without putting it on Home.

## 4. Non-goals

- No cloud account, relay, telemetry requirement, or Internet discovery.
- No generic remote shell, arbitrary command execution, or plugin loading.
- No SYSTEM service, permanent elevation, virtual HID, or firewall mutation.
- No TLS/provider fallback, identity replacement, identity export, or 0-RTT.
- No automatic capability grant based on product role, discovery, proximity,
  previous preference, or a remote request.
- No cross-PC preference synchronization in the first production UX slice.
  A future “Follow Main PC settings” protocol requires its own capability,
  threat review, versioning, and approval.
- No production admission of Windows 10. WinUI availability does not alter the
  current Windows 11 / Server 2022+ transport baseline or the experimental
  status of the Windows 10 OpenSSL/CNG path.
- No remote-screen KVM, screen capture/streaming, virtual monitor, Indirect
  Display Driver, or extended-display implementation in PRs 1-9.

## 5. Mandatory security and safety invariants

The redesign must preserve all existing transport and input gates:

- The existing non-exportable CNG device identity and DER SHA-256 pin remain
  unchanged.
- Production transport remains stock MsQuic + Schannel on supported Windows.
- Discovery results remain untrusted address hints. Selecting a card grants
  nothing and admits no session.
- Pairing retains the role-bound transcript and matching six-digit code on both
  PCs. The default action remains reject/cancel.
- `PeerValidated` must succeed before CONNECTED effects, streams, datagrams,
  pairing/session delivery, or application traffic are exposed.
- Authentication alone grants no capability.
- Every local capability addition requires a local, foreground approval that
  names the peer and consequence.
- A product-role or preference change cannot mutate `trust.db`.
- Capability reductions apply immediately. Revoking input must first disable
  capture/routing, return Local, release DeskLink-owned input, and terminate the
  affected session.
- Forgetting a device terminates its sessions before removing trust.
- Auto-connect may retry ordinary availability/network failures with bounded
  backoff. It must stop and show **Action required** after certificate, pin,
  identity, credential, signing, authentication, capability, or protocol
  failure.
- Connecting state must name its current phase. Retry state shows the attempt
  and live bounded countdown; terminal state shows the typed failure category,
  a recommended action, and an explicit **Retry now** control. It must never
  remain as an unexplained indefinite "Connecting" label.
- Reconnect always uses a fresh nonce and begins Local. It never restores focus
  or suppression automatically.
- Capture remains disabled until a validated session, current topology, Ready
  route, fresh focus transaction, accepted landing, and initial state snapshot
  all succeed.
- UI/broker IPC retains the current one-user DACL and mutual process-token SID
  verification, bounded frames, strict decoding, and explicit operations. It
  exposes neither arbitrary execution nor raw input/clipboard content.

## 6. Product roles

### 6.1 Main PC

User-facing description:

> Use this PC's keyboard and mouse across your desk.

Default desired behavior after the user finishes setup:

- owns the local desk graph and foreground behavior policy;
- starts the per-user runtime at sign-in;
- attempts a trusted connection to the preferred Companion;
- starts and reconnects in Local mode;
- enables edge observation only when capture and a Ready saved route are
  explicitly enabled;
- evaluates application/fullscreen profiles locally;
- presents display arrangement and feature routing controls.

### 6.2 Companion PC

User-facing description:

> Control this PC from your Main PC.

Default desired behavior after the user finishes setup:

- starts the per-user runtime at sign-in;
- advertises the trusted-session endpoint on the local link;
- listens for authenticated sessions from paired peers;
- injects input only for a peer with a local `InputInject` grant and current
  focus lease;
- shows minimal connection, permission, and emergency state;
- never treats “Main PC” as permission to broaden local security policy.

### 6.3 Flexible

Flexible exposes reciprocal control, multiple input origins, partial/shared
edges, one-way routes, manual endpoint selection, explicit runtime operations,
and advanced profiles. It uses the same broker and trust model; it is not a
legacy insecure mode.

### 6.4 Terminology rule

Use **Main PC**, **Companion PC**, and **Flexible** in product UI. Preserve
Host/Agent, Controller/Receiver, and focus-mode names only in source,
diagnostics, protocol documentation, and advanced support views. Do not use
“Slave.”

## 7. Process and control architecture

### 7.1 Per-user runtime broker

Introduce `desklink_runtime.exe` as the long-lived per-user owner of:

- identity and trust-store access;
- discovery browsing/advertising;
- pairing windows and candidate lifetime;
- one listener and the bounded set of peer sessions;
- auto-connect/backoff planning;
- capture, focus, reconciliation, topology, audio, clipboard, and profiles;
- fail-local update coordination;
- structured status and bounded in-memory diagnostics.

This removes the current wrapper pattern where every action spawns a different
`desklink_pair.exe` operation and prevents two UI/process owners from racing for
the same current-user endpoint.

`desklink_pair.exe` remains available for tests, recovery, compatibility R&D,
and expert diagnostics. During migration it may either invoke existing direct
operations when the broker is absent or use explicit diagnostic broker calls;
it must never silently take ownership from a running broker.

Implementation checkpoint: PR 3 introduces `desklink_runtime.exe` as the
single UI-facing endpoint and owner of product preferences and broker-mediated
trust mutations. Existing transport operations remain in the separately
single-owned diagnostic runtime during migration and are proxied through the
broker. Alpha-owned child operations use a kill-on-close job, so an actual UI
process death rejects an in-progress pairing prompt and fails active input
Local. Role-driven listener/connect ownership moves into the broker in PR 4;
the production pairing shell consumes the broker pairing lease in PR 6.

### 7.2 WinUI 3 shell

Introduce a separate UI executable, provisionally `desklink.exe`, using WinUI
3 and C++/WinRT. It is an ordinary current-user client of the broker.

Before feature work, a deployment spike must prove:

- CMake/Visual Studio integration and reproducible dependency pinning;
- Windows App SDK version/support policy;
- framework-dependent versus self-contained package size and servicing choice;
- signed Inno Setup installation and rollback integration;
- single-instance activation, broker-owned tray behavior, short-lived startup
  bootstrap, and update shutdown;
- a clean Windows 11 and Windows Server 2022 Desktop Experience install;
- no runtime dependency or UI failure can stop the broker from failing Local.

If that spike fails, stop for a UI-technology decision. Do not fall back to
Electron, in-process web content, or Dear ImGui without separate approval.

### 7.3 Typed broker API

Extend the existing control protocol with bounded request/response models:

- `GetDashboardState`
- `GetProductPreferences` / `SetProductPreferences`
- `StartDiscovery` / `GetNearbyPeers` / `StopDiscovery`
- `OpenPairingWindow` / `PairNearbyPeer` / `PairManualAddress`
- `GetPairingCandidate` / `ConfirmPairingCandidate` / `RejectPairingCandidate`
- `ListTrustedDevices`
- `RequestLocalPermissionChange`
- `ForgetTrustedDevice`
- `GetDisplayTopologies` / `GetRoamingConfiguration` /
  `SetRoamingConfiguration`
- `PauseDeskLink` / `ResumeDeskLink` / `ReturnLocal`
- `SetDesiredFeatureState`
- `GetProfilePolicy` / `SetProfilePolicy`
- `GetBoundedDiagnostics`

The names are conceptual; final wire names may differ. Every operation needs a
fixed payload bound, explicit version, validation, failure status, and tests.
Pairing candidates are short-lived and nonce/request-ID bound. If the shell is
absent, disconnects, stalls, or throws, the broker rejects the candidate.

The generic same-user control pipe may request a permission change but must not
directly approve or persist broader authority. Capability additions remain
inside the pairing/reauthorization state machine and require a broker-owned,
foreground, reject-default confirmation. Low-authority integrations such as a
future Stream Deck plugin receive only a restricted operation set and cannot
invoke trust mutation. PR 3 must establish and test this separation before a
Devices page is allowed to add authority.

The shell receives display names, status, permissions, monitor metadata, and
bounded diagnostics. It never receives private-key handles, trust-store
ciphertext, raw certificates beyond an explicitly requested diagnostic view,
input events, clipboard text, or audio frames.

This broker does not claim to solve the repository's documented hostile
same-user isolation limitation. A compromised process running as the same
Windows user can already access that user's DPAPI and CNG resources. Stronger
isolation would require a separately secured broker/service identity and a
trusted approval UI, with its own threat review and approval. The UX redesign
must not make that limitation worse or describe same-SID checks as a complete
security boundary.

## 8. Persistence and authority boundaries

### 8.1 Product preferences

Replace the three-flag application file with an atomically written, versioned
product-preference document conceptually containing:

```text
DeskRole
PreferredPeerMachine
RunAtLogin
CloseToTray
AutoStartRuntime
AutoConnect
InputRoamingDesired
ClipboardDesired
AudioRoute
AudioGain
VoiceRoute
VoiceInputEndpointId
VoiceGain
VoiceEchoGuard
GamingBehavior
AdvancedModeEnabled
FirstRunComplete
```

Use typed fields with strict bounds. Unknown versions fail safely and do not
start capture or remote focus. Migration from the current version preserves the
existing close-to-tray, run-at-login, and first-run choices but defaults the new
role and feature desires to unconfigured/off.

The persisted preference describes desire, not authority. A runtime planner
converts it into a typed plan after intersecting it with current trust grants,
authenticated peer state, topology readiness, and safety state.

### 8.2 Trust and device metadata

`trust.db` remains the DPAPI-protected source of local peer identity pins and
capability grants. The shell never opens it directly.

Add a bounded `ListPeers()`-equivalent operation inside the trust/runtime layer
that returns immutable copies of at most `kMaxTrustedPeers`. Do not expose
`SavePeer()` as a generic UI primitive. Use typed operations for permission
reduction, locally confirmed permission addition, and forget.

Friendly aliases, last-seen time, cached endpoints, and connection health are
non-authoritative product metadata. They must not replace the stored machine ID
or pin and must not be treated as authenticated when sourced only from
discovery.

### 8.3 Configuration authority

For the first slice, “Main PC owns the desk configuration” means the Main PC
stores the graph, preferred peer, session desires, and profile policy it needs
to operate. It does not push arbitrary settings into the Companion.

A future Companion option to follow Main settings requires a dedicated bounded
protocol message and explicit local opt-in. It may synchronize presentation and
behavior preferences only. It can never add trust or capabilities, forget a
peer, launch applications, or enable future system/file authority.

## 9. Guided first-run and pairing flow

### 9.1 Choose a role

The first screen presents the three product roles with plain-language
descriptions. Choosing a role persists no peer grant and starts no capture.

### 9.2 Add a PC

The normal path shows structured discovery results as cards:

```text
HOSTPC
Nearby · Unverified
[Connect]
```

Rules:

- ambiguous discovery records are disabled and explain the conflict;
- discovery names and endpoints are visibly unverified;
- selecting a result only supplies the address to the existing cryptographic
  pairing lane;
- the port, machine ID, TLS provider, and certificate pin remain hidden in the
  normal flow;
- **Can't find your PC?** reveals manual host/IP and an Advanced port field;
- manual entry and discovery use the same pairing and validation path.

The receiving PC still explicitly opens a five-minute pairing window. During
initial Companion setup, the wizard may make that action prominent and keep the
window status visible; it must not keep pairing permanently open.

### 9.3 Choose intentions, not protocol flags

The wizard presents concrete intentions. It maps each intention to local grants
and runtime desires, but both PCs must approve their own side.

| User intention | Main PC local effect | Companion PC local effect |
| --- | --- | --- |
| Control the Companion | topology desire; controller plan | explicit `InputInject` grant to Main; topology desire |
| Share display layouts | explicit topology grant to Companion | explicit topology grant to Main |
| Share clipboard both ways | explicit clipboard read/write grant to Companion plus local module desire | explicit clipboard read/write grant to Main plus local module desire |
| Play Companion audio on Main | explicit grant allowing Companion audio into Main plus render desire | explicit grant allowing Main to receive Companion loopback plus capture desire |

Generic/Flexible manual pairing defaults every sensitive grant off. A
Main/Companion wizard may recommend **Control the Companion** because the user
already selected that relationship, but it must appear as a separate reviewed
choice. The Companion confirmation dialog names the Main PC and the exact input
consequence, uses a reject-default button, and cannot be bypassed by the Main.

### 9.4 Verify and allow

Both PCs show:

- the same six-digit code;
- the peer display name;
- whether the address came from untrusted discovery or manual entry;
- the exact local permissions this PC is about to grant;
- clipboard/audio direction using concrete PC names;
- **Allow** and reject-default **Cancel** actions.

Trust is not shown as successful until both durable trust writes and completion
acknowledgements succeed, matching the current pairing protocol.

### 9.5 Arrange displays and finish

After pairing and authenticated topology exchange, the Main PC opens the simple
monitor canvas. Saving the desk does not focus or enable capture. Completion
enables the desired auto-start/auto-connect plan, which still begins Local.

## 10. Home and tray information architecture

### 10.1 Home

Home contains:

```text
DeskLink                                             Connected

Your desk
[ THIS-PC · Main PC ] ━━━ [ HOSTPC · Companion ]

Keyboard & mouse
Ready to move between both PCs

Clipboard sharing                         On
Audio from HOSTPC                          On · 72%
Start with Windows                         On

[Arrange displays]        [Devices & permissions]
```

When remote focus is active, replace the keyboard/mouse status with:

```text
Currently controlling HOSTPC
[Return to this PC]
```

Normal Home does not show ports, TLS provider, machine IDs, raw role/mode,
pointer DPI, process controls, or the diagnostic log.

### 10.2 Tray

The persistent native broker owns this menu. The WinUI process is started only
for **Open DeskLink** and exits completely when its window closes.

The tray menu provides:

- current connection and focus state;
- **This PC** and authenticated peer focus choices when admissible;
- clipboard desired state;
- peer-audio gain/mute;
- **Return to this PC** whenever remote focus is pending or active;
- **Pause DeskLink** / **Resume DeskLink**;
- **Open DeskLink** and **Exit**.

`Ctrl+Alt+Pause/Break` remains the independent physical emergency path. UI and
tray shortcuts are conveniences, not replacements.

### 10.3 Advanced and diagnostics

Move these out of Home:

- identity pin and transport/provider diagnostics;
- manual IP/port;
- explicit listener/controller operations;
- pointer gain and source DPI;
- raw runtime role/mode, nonce, machine IDs, peer count, and logs;
- partial/one-way edge mapping;
- exact executable profile editing.

Diagnostics remain memory-bounded and never contain input, clipboard, or audio
content. The shipped connection check reads only the broker's typed state and
reports runtime phase, failure category, retry timing, and a safe next action.
It neither probes arbitrary endpoints nor relaxes authentication.

## 11. Devices and permissions

The Devices page lists authenticated trust records separately from untrusted
Nearby results:

```text
HOSTPC
Paired · Connected now

Can control this PC              Yes
Clipboard access                 Read and write
Display layout                   Shared
Audio                            HOSTPC → THIS-PC

[Change permissions]  [Forget this PC]
```

Rules:

- only authenticated/stored identity supplies the paired name and pin binding;
- permission additions require a local consequence dialog;
- permission reductions require confirmation when they interrupt a feature,
  then apply fail-closed immediately;
- revoking `InputInject` first returns Local and cleans up owned input;
- **Forget** closes all sessions, returns Local, removes trust atomically, and
  leaves discovery metadata untrusted if the same machine remains nearby;
- a remote peer cannot approve, suppress, or undo local revocation;
- future system/file capabilities are hidden until implemented and separately
  approved.

## 12. Monitor configurator

The default configurator retains the current strengths:

- per-PC grouping;
- stable display identity;
- current/offline state;
- physical-size approximation;
- resolution and refresh rate;
- local Identify overlays;
- atomic graph save after confirming Local.

The default authoring model becomes:

1. Drag display cards into their physical positions.
2. When edges on different PCs snap within a bounded threshold, show a proposed
   connector.
3. Saving an accepted connector creates a full-edge (`0..100%`), bidirectional
   `RoamingLink` using the exact stable display identities and inferred opposing
   sides.
4. Canvas coordinates remain presentation-only and never become runtime route
   authority.

Dragging alone must not activate input. A new/changed connector remains a
visible unsaved proposal until **Save desk layout** succeeds.

An **Advanced edge mapping** disclosure retains:

- source and destination displays;
- side selection;
- partial start/end percentages;
- one-way direction;
- link enable/disable and removal;
- route-resolution diagnostics.

All current validation remains: different machines, non-overlapping active
source segments, valid percentages, bounded link count, current topology for a
Ready route, and Local-before-atomic-save.

## 13. Persistent runtime behavior

### Companion

- Start the broker at sign-in when enabled.
- Listen and advertise on the configured port (default hidden).
- Admit only paired, pinned, capability-authorized peers.
- Remain available without a visible shell.

### Main

- Start the broker at sign-in when enabled.
- Resolve the preferred peer from authenticated cached identity plus untrusted
  endpoint hints.
- Auto-connect with bounded exponential backoff and jitter after ordinary
  network/unavailable failures.
- Stop automatic retries on security, identity, protocol, or permission failure
  and show **Action required**.
- Begin every connection Local with a fresh nonce.
- Arm roaming only after the saved route is Ready and the user has enabled
  physical capture/roaming.

The first production slice supports one preferred Main-to-Companion automatic
connection while keeping trust/device models bounded for multiple peers.
Multi-Companion simultaneous controller planning is a later design extension,
not an implicit part of this approval.

## 14. Profiles and feature presentation

### Gaming behavior

Expose a simple default:

> When a game is fullscreen, keep keyboard and mouse on this PC.

Advanced editing may add bounded exact executable rules and fullscreen-only
conditions. The existing precedence remains emergency, manual override,
profile, then system default. Uninspectable foreground state stays Local.

### Intentional switching

Present the existing crossing policies as understandable choices such as
**Cross immediately**, **Pause and push**, and **Push twice**. Preserve their
bounded thresholds, replayable tuning, cooldown, and fail-local behavior.

Allow an optional local hotkey to request focus for a named paired PC or return
Local. Hotkeys are user preferences and never grants. A remote-focus request
still requires the complete authenticated route and session-admission chain;
failure or ambiguity leaves focus Local.

### Clipboard

Home shows one desired-state switch per peer. Turning it on starts the local
approval/orchestration flow when grants are missing; it does not manufacture
remote consent. Runtime activation still requires complementary stored grants,
the module handshake, current nonce, and explicit local desire on both sides.

### Audio

Use concrete routes such as **Play HOSTPC audio on THIS-PC**, with gain/mute near
that route. Missing local or peer authority opens a permission explanation; it
does not reverse or infer `AudioSend`/`AudioReceive` silently.

### Voice and microphone

Voice is presented separately from system audio. Pairing and permission review
name the two local consequences independently: allowing peer microphone
playback and allowing this microphone to be received. Both default off and
neither may be inferred from an audio switch.

The feature surface uses a press-and-hold PTT button, an explicit hard mute,
communications microphone selection, incoming gain, and a default-on echo
guard. It visibly distinguishes off, missing permission, PTT ready,
transmitting, muted, and unavailable-input states. Route enablement never opens
capture; release/cancel/pointer-capture loss stops it. Echo guard is labeled as
half-duplex feedback protection and never as acoustic echo cancellation. A
global PTT binding is deferred until it has a separate input-lifecycle design.

## 15. Accessibility and visual requirements

- Responsive XAML layout with no fixed 940×890-style dependency.
- Per-monitor DPI and text scaling at 100%, 125%, 150%, 200%, and 300%.
- Full keyboard navigation and visible focus indicators.
- Narrator names, roles, values, and state-change announcements.
- Light, dark, and all supported Windows contrast themes.
- Theme resources/system colors; no status communicated by color alone.
- Reduced-motion behavior and no required animation.
- Minimum target/control sizes and non-truncated translated strings.
- UTF-8 source/build enforcement and automated checks for the Unicode
  punctuation regression already observed in the Alpha artifact.
- Monitor cards expose resolution, refresh, PC, online state, and connection
  state through accessible text, not canvas drawing alone.

Microsoft's Win32 guidance expects per-monitor-aware windows to handle
`WM_DPICHANGED` and resize/reposition from the suggested rectangle. Its contrast
guidance warns against hard-coded colors. The replacement shell should use
WinUI theme/layout facilities rather than recreate those responsibilities in
manual HWND code.

## 16. Staged implementation plan

Each stage is a separate reviewable PR and must pass its own gates before the
next stage becomes mergeable.

### PR 1 — Immediate Alpha safety defaults

- Default every generic/manual pairing capability to off.
- Keep topology and input recommendations in explanatory role text only until
  the user makes an explicit choice.
- Add tests for launcher default grant masks.
- Do not change trust, pairing protocol, or existing stored grants.

Gate: full Windows/Linux/sanitizer tests; confirmation text reviewed; no
existing trust migration.

### PR 2 — Product model, preferences, and planner

Implementation status: **automated foundation complete; integration remains in
later PRs**.

- Add `DeskRole`, versioned product preferences, migration, and strict bounds.
- Add typed `DesiredDeskConfiguration` and deterministic runtime planner.
- Unit-test every role, missing-grant state, malformed preference, and
  fail-local plan.
- No auto-connect or new UI yet.

Gate: preference corruption is fail-safe; role changes produce no trust-store
write; current Alpha remains operational.

### PR 3 — Single per-user runtime broker and control contract

Implementation status: **complete and merged**.

- Introduce the broker and single-owner lifecycle.
- Move structured discovery, pairing orchestration, runtime status, and
  topology ownership behind bounded IPC.
- Add bounded trust enumeration, permission reduction/addition, and forget
  operations with fail-local ordering.
- Retain CLI diagnostics and existing transport/security code.

Gate: same-user IPC security tests; malformed/oversized request tests; UI-client
death rejects pairing; revoke/forget clean active input; ordinary same-user
control clients cannot silently broaden trust; no arbitrary execution or raw
content exposure. Stop at this boundary if broader authority cannot remain
inside explicit local reauthorization.

### PR 4 — Persistent role-driven startup and reconnect

Implementation status: **merged and validated**.

- Companion auto-listen/advertise.
- Main preferred-peer auto-connect with bounded retry policy.
- Pause/resume/return-local operations.
- Network-change and process-restart reconciliation.

The broker treats discovery as an endpoint hint only and passes the exact
persisted preferred MachineId into certificate validation. Managed sessions
start `LockPc1`; after an admitted peer is observed, the broker may arm an
already enabled roaming graph but never restores focus automatically. Only a
positively classified ordinary availability failure retries. Authentication,
certificate, identity, capability, protocol, credential, signing, and unknown
failures stop in `ActionRequired`. Transport loss performs intrinsic
`PeerSession` fail-local cleanup before the supervisor observes it, and a
nonzero peer QUIC application error is a protocol failure rather than a retry
signal.

Gate: fresh nonce each reconnect; no automatic focus restoration; security
failures do not retry/fallback; broker/UI/process/network failure remains Local.

### PR 5 — WinUI deployment spike and shell foundation

- Pin Windows App SDK and build a minimal C++/WinRT shell.
- Prove installer/update/single-instance/tray/activation behavior.
- Implement Home, Advanced, Diagnostics, and accessibility foundations against
  simulated broker states.

Gate: clean-system install/update/rollback; UI exit does not stop broker;
Windows 11 and Server 2022 Desktop Experience smoke tests; DPI/contrast/Narrator
baseline. Stop for approval if deployment cannot meet current updater and
fail-local guarantees.

Implementation checkpoint: the shell foundation uses the locked minimal
component graph `Microsoft.WindowsAppSDK.Runtime` 2.4.0,
`Microsoft.WindowsAppSDK.WinUI` 2.3.6, and `Microsoft.Windows.CppWinRT`
3.0.260818.1. It is an unpackaged self-contained x64 application; the staging
allowlist contains 243 files (about 58 MiB), including the pinned SDK license
and notices, and rejects unexpected files,
reparse points, or invalid Microsoft runtime signatures. Home, Advanced, and
Diagnostics render six bounded simulated states without opening network,
trust, capture, or input authority. The shell has its own lifecycle mutex,
single-instance activation, explicit bounded exit, and a coordinated-update
window. The completed lightweight lifecycle moves tray and hotkey ownership to
the native broker and exits WinUI on close. Installer/update orchestration
stops both visible
clients when present while rollback remains compatible with pre-shell packages.
Alpha intentionally remains the default installed entry point through PR 8.

### PR 6 — Guided onboarding, Nearby PCs, and Devices

- Role selection and first-run state machine.
- Structured Nearby cards and manual fallback.
- Pairing candidate/code/permission UX.
- Devices & permissions with local review, revoke, and forget.

Gate: ambiguous/spoofed discovery grants nothing; codes and local consequences
appear on both PCs; timeout/UI exit rejects; permission additions require local
approval; revocation is immediate and fail-local.

Implementation checkpoint: the version-11 local control protocol now carries
bounded discovery, pairing-start, candidate-decision, and Nearby result types.
The broker owns the five-minute discovery/pairing lifecycle and launches only
its fixed sibling pairing executable after stopping the normal runtime Local.
Each child receives a fresh random 128-bit token and nonzero operation ID;
candidate presentation and decision polling must match both. The broker exposes
only machine/name/code/grants/source to the shell, keeps fingerprint/transcript
material inside the child-to-broker boundary, and leases the candidate for 90
seconds. Missing shell mutex, UI exit, timeout, mismatch, Cancel, process exit,
or update shutdown rejects. A zero-grant candidate remains valid, so the UI
does not manufacture authority merely to complete pairing.

Nearby records are capped at 64 and cached by the broker after native mDNS
browse/resolve. Ambiguous, closed, zero-endpoint, or protocol-incompatible
records cannot start pairing; selecting a valid card supplies only its cached
address to the same cryptographic pairing lane as manual entry. Devices reads
only stored authenticated trust. Permission reductions and Forget preserve the
existing Local/owned-input cleanup ordering. Additions now use the designed
broker-owned local reauthorization candidate, bound to the stored machine ID,
certificate pin, and exact grant snapshot; rejection, expiry, stale state, or
cleanup failure adds nothing and the PCs are not paired again. Nearby results
with a stored machine ID are labeled **Already paired** and route to trusted
connection/device actions instead of the new-pairing lane. Pairing copy remains
in a waiting state until both sides have an actual comparison-code candidate.

### PR 7 — Simplified monitor authoring

- Port the canvas to the production shell.
- Add snap/proposed full-edge bidirectional links.
- Move percentages/one-way routes under Advanced.
- Preserve stable identities, offline displays, Identify, route validation, and
  Local-before-save.

Gate: canvas placement alone cannot alter routing; accepted proposals compile
deterministically; topology loss invalidates Ready routes; accessibility has a
non-canvas equivalent.

Implementation checkpoint: the production WinUI shell now renders the existing
bounded monitor model as physically scaled per-PC cards, including stable
identities, current/offline state, checked resolution/refresh metadata, and the
local five-second Identify overlays. Dragging or keyboard nudging changes only
pending canvas placement. Cross-PC adjacency produces a highlighted proposal
that must be accepted explicitly; the non-canvas editor can create the same
full-edge bidirectional link, and Advanced retains exact percentages and one-
way direction. The shared save coordinator is unit-tested to return active
capture Local, confirm that state before atomic persistence, and apply the
roaming preference only after the write. It keeps the authenticated transport
alive and automatically arms accepted routes. Topology loss continues to
resolve saved links as not Ready rather than falling back to canvas geometry.

### PR 8 — Feature intent and profiles

- Clipboard and audio intention flows with concrete peer names.
- Gain/mute and gaming behavior on normal settings surfaces.
- Plain-language crossing-policy presets and bounded per-PC focus/return-local
  hotkeys, backed by the existing focus admission path.
- Advanced exact executable/fullscreen profile editing.

Gate: two-sided consent remains necessary; feature failure is module-scoped;
uninspectable profile state and permission mismatch remain Local/off.

Implementation checkpoint: Home now names the preferred authenticated PC in
clipboard and peer-audio controls. Enabling either intent first checks the
local stored grants; missing authority explains the exact local permission and
never manufactures the peer's complementary consent. Persisted desire is still
planned through the existing `PeerValidated`, capability, nonce, and module
admission gates. Audio gain remains bounded to 0-100%, mute remains a live
admitted-session action, and neither touches the system mixer.

The product shell applies **Cross immediately**, **Pause and push**, and
**Push twice** to the validated defaults and both directions of every existing
route, then uses the same Local-before-atomic-save coordinator as Arrange
displays. Focus and Return shortcuts are chosen from a fixed modifier/F11/F12
allowlist; `Ctrl+Alt+Pause/Break` is reserved. A named focus request reaches the
existing authenticated input lifecycle only when that exact peer and the Host
input owner are active, so a shortcut cannot bypass normal admission.

Application preferences schema 6 persists at most 32 exact executable rules,
optional fullscreen matching, the two bounded shortcuts, the simple global
fullscreen keep-local choice, and a local-only received-voice destination that
migrates older installs to communications playback. The broker passes these only to the explicit
input-roaming child. A Roam fallback arms edge observation without installing a
manual override, preserving emergency, manual, exact-rule, global-fullscreen,
then default precedence. Any required foreground observation that cannot be
inspected remains Local. Schema 4 also permits one explicit endpoint for the
preferred already-trusted machine. The Devices page presents it only as a
recovery for blocked discovery; saving it starts a trusted connection but never
grants trust or permissions. Discovery remains preferred and ambiguous or
incompatible records remain fail-closed. Alpha remains the default installed entry point until
PR 9.

### PR 9 — Cutover and product qualification

- Make the broker plus production shell the normal installed experience.
- Retain Alpha/CLI only as explicit diagnostic tooling or remove Alpha after a
  migration release.
- Complete signed installer/update, DPI, contrast, keyboard, Narrator,
  localization, sleep/resume, network-loss, and two-PC failure qualification.

Gate: identity pin, CNG key properties, trust records, saved graph, and product
preferences survive upgrade; rollback restores the previous working product;
all security and physical validation gates pass.

Implementation checkpoint: **PR 9A cutover foundation is complete.** The
installed Start menu entry, post-install action, sign-in registration, and
updater restart now target `desklink.exe`. The shell starts only the fixed
sibling broker with a bounded readiness probe and never crosses an active
install/update gate. Alpha remains an explicitly labeled diagnostics fallback
for one migration release. Setup migrates only the exact legacy Alpha Run
command; the updater snapshots and restores the exact prior command on
rollback. Product-shell deployment and broker/state health probes replace the
Alpha health baseline. Disposable-account validation compares the full
non-exportable CNG identity snapshot and byte-identical DPAPI trust, schema-4
preferences, and saved roaming graph across rollback and upgrade.

**PR 9B automated qualification foundation is complete.** CI now treats the
product UI and installer as contracts: PerMonitorV2/as-invoker metadata,
keyboard navigation and a keyboard-equivalent monitor editor, accessible names
and live status announcements, theme/high-contrast resources, strict UTF-8 and
fallback-language settings, supported OS floor, and the absence of automatic
Firewall/elevation behavior all fail closed. Release-signing policy has pure
tests for missing/private-key/EKU/validity and RFC 3161 endpoint failures.
Disposable Windows Server 2022 now repeats install, same-version repair,
rollback, upgrade, identity/state preservation, and uninstall qualification
using the exact artifact already tested by the MsQuic job.

The broker consumes native suspend/resume notifications, stops the transport
Local, and starts a fresh session after wake without clearing user-pause or
security/action-required state. Existing deterministic tests cover network
change, availability-only reconnect, nonce rotation, and the power-state
policy. Real production-signed packages, hands-on Narrator/scaling/contrast,
physical sleep/network interruption, a clean Windows 11 target, and the
explicitly deferred two-supported-Windows-11-PC failure matrix remain release
gates. DeskLink therefore remains experimental.

## 17. Product acceptance criteria

The redesign is complete only when all of these hold:

1. A generic/manual pairing screen has no sensitive grant enabled by default.
2. A user can configure one Main and one Companion without seeing a port or IP
   when discovery works.
3. Discovery ambiguity or spoofing cannot create trust, select a TLS provider,
   or start a session.
4. Both PCs show the same pairing code and their own exact local grant impact.
5. Main/Companion role changes never add or modify trust capabilities.
6. A configured Companion becomes available after sign-in without pressing
   **Start receiver**.
7. A configured Main reconnects after ordinary network recovery, remains Local,
   and never retries authentication/certificate failures.
8. Only one per-user runtime owns the endpoint; opening a second UI cannot cause
   the current `could not create the current-user endpoint` conflict.
9. Home uses peer names and user state; raw roles, modes, ports, IDs, and logs
   live under Advanced/Diagnostics.
10. Devices lists every bounded trust record and can reduce permissions or
    forget a peer with fail-local cleanup.
11. Clipboard/audio convenience switches cannot bypass complementary local
    grants or peer consent.
12. Dragging displays can propose an ordinary bidirectional full-edge link;
    routing changes only after validated atomic save.
13. Partial and one-way routes remain available under Advanced.
14. Closing or crashing the UI does not capture input, weaken validation, or
    stop fail-local broker behavior.
15. The shell passes DPI, keyboard, Narrator, light/dark/contrast, and Unicode
    rendering checks.
16. Upgrade and rollback preserve the non-exportable identity, pin, trust,
    permissions, and saved desk graph.
17. Windows 10 remains experimental/unsupported until its separate transport
    admission is approved, regardless of whether the shell can launch there.

## 18. Approval requested

Approval of this document means:

- adopt Main PC / Companion PC / Flexible as product roles;
- implement a single current-user native runtime broker;
- target a WinUI 3 C++/WinRT production shell, subject to the PR 5 deployment
  stop/go gate;
- keep security grants locally authoritative and separate from preferences;
- implement the work in PRs 1–9, merging only after each listed gate passes;
- defer cross-PC preference synchronization and simultaneous multi-Companion
  controller planning to separately approved designs;
- keep image/formatted clipboard and explicit file transfer as later,
  independently approved capability/privacy projects; and
- place remote-screen KVM in later R&D and extended-display mode after it as a
  far-future, separately approved driver/video project outside PRs 1-9.

Approval does not authorize weakening any existing identity, transport,
validation, capability, session-admission, input, or fail-local guarantee.

## 19. Primary references

- [WinUI 3 overview](https://learn.microsoft.com/en-us/windows/apps/winui/winui3/)
- [Windows App SDK deployment for unpackaged apps](https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/deploy-unpackaged-apps)
- [`WM_DPICHANGED` requirements](https://learn.microsoft.com/en-us/windows/win32/hidpi/wm-dpichanged)
- [Supporting Windows high-contrast themes](https://learn.microsoft.com/en-us/windows/win32/controls/supporting-high-contrast-themes)
- [PowerToys Mouse Without Borders setup and layout](https://learn.microsoft.com/en-us/windows/powertoys/mouse-without-borders)
- [Input Leap documented setup flow](https://github.com/input-leap/input-leap/blob/master/README.md)
- [Synergy features](https://symless.com/synergy/features)
- [Synergy security and architecture overview](https://support.symless.com/hc/en-us/articles/48326695360529-Security-and-Architecture-Overview)
- [Multiplicity 4 product features](https://www.stardock.com/products/multiplicity/)
- [Microsoft Indirect Display Driver model](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/indirect-display-driver-model-overview)
