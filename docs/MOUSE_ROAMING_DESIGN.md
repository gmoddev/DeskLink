# Mouse roaming, companion lifecycle, and monitor configurator design

Status: revised implementation plan. Phases 1-3 and the companion foundation
may proceed. Phase 4 may be implemented and exercised experimentally after its
automated safety gates pass. The complete two-PC Windows 11 matrix is a
production-release gate, not an implementation blocker.

## 1. Decisions

- Connection state and input-control state remain separate. Reconnection can
  restore a `Ready` route but never restores remote focus.
- Physical monitor dimensions affect only the configurator visualization and
  route suggestions. Runtime pointer routing uses explicit normalized edge
  segments and never reads canvas coordinates, EDID dimensions, or DPI.
- Dragging compatible monitor edges together offers a one-click route
  suggestion. Manual edge and partial-overlap editing remains available.
- The UI offers a reciprocal link by default, but each direction is a separate
  authorization decision. A direction is `Ready` only when its destination has
  granted input injection and the runtime supports that direction.
- Startup, reconnect, settings mutation, update, exit, and every failure begin
  or return `Local`.
- Windows 10 compatibility hardware may be used for incremental development
  testing where its provider path is supported. Production sign-off still
  requires the complete Windows 11 physical matrix.

## 2. Mandatory safety invariants

No edge crossing may suppress local input or admit remote application traffic
unless all of these hold:

1. the transport peer is `PeerValidated`, pinned, and trusted;
2. the destination independently grants `InputInject`;
3. the current session nonce matches;
4. a fresh focus epoch and bounded lease are active;
5. the source and target stable identities resolve in current topology
   generations;
6. the absolute landing event and initial state snapshot enter their bounded
   sender paths; and
7. the existing Host input lifecycle authorizes suppression.

Transport, topology, capture, process, authorization, queue, or focus failure
immediately fails local. Ctrl+Alt+Pause/Break remains an independent physical
emergency return. Discovery advertisements and last-known addresses are
untrusted connection candidates only; neither can establish identity, trust,
capability, topology, a route, or focus.

## 3. Lightweight native companion

DeskLink remains a user-scoped native companion, not a SYSTEM service. The
product application should own a reusable peer runtime directly; automatic
reconnection must not be implemented by repeatedly spawning the CLI. The CLI
and companion may share extracted runtime components.

### Single instance

Exactly one visible WinUI companion instance is allowed per Windows user. A
second launch sends `Open DeskLink` to its message-only activation window and
exits. The persistent native broker independently owns exactly one tray icon,
listener, connection manager, and input owner.

### Startup and responsiveness budgets

Initial performance budgets for release builds are:

- warm main-window visibility: preferably under 500 ms at P95;
- cold main-window or native background tray availability: under 1 second at P95,
  excluding an OS-controlled first-run signature scan;
- no discovery, connection, authentication, or topology wait on the UI thread;
- no periodic canvas rendering while the window is hidden;
- hidden and idle CPU: under 0.5% of one logical processor averaged over one
  minute with no network events; and
- steady background private working set target: under 16 MiB for the broker and
  transport child on the Schannel build.

These are profiling budgets, not reasons to weaken validation. The native
broker, tray, and input authority initialize first. Discovery, reconnection,
topology synchronization, and recoverable operations run on bounded background
workers and publish state changes asynchronously. The WinUI process is not a
background worker and must not remain resident after its window closes.

### Close, tray, and exit contract

The native broker owns background lifetime:

- the main-window X fully exits WinUI while preserving the broker and sessions;
- launching DeskLink again creates the on-demand settings window;
- Windows logoff/shutdown performs ordered fail-local cleanup without an
  unnecessary prompt; and
- **Tray > Exit DeskLink** always returns local, releases DeskLink-owned remote
  state, stops capture, closes sessions, removes the tray icon, and exits.

The tray menu provides at least:

```text
DeskLink — Connected / Reconnecting / Offline
Open DeskLink
Return Local                 shown/enabled when applicable
Disconnect <peer>            returns local first when applicable
Reconnect                    shown for a selected unavailable peer
Settings
Exit DeskLink
```

For multiple peers, the summary shows counts and the menu exposes per-peer
status rather than implying only one connection.

Manual **Disconnect** returns local, closes that peer, and suppresses automatic
retry for the rest of the process lifetime until **Reconnect** is selected.
It does not revoke trust or silently change the persisted `AutoConnect` choice.

### Start with Windows and first run

**Start DeskLink when I sign in to Windows** is user-scoped and requires no
elevation. It starts the signed `desklink.exe` at its fixed path with
`--background`; that process starts the fixed sibling native broker and exits
before constructing WinUI. DeskLink validates the registration against its own
installed path; no preference value becomes an arbitrary command line.

First launch does not disappear into the tray. It opens onboarding with:

```text
This PC
Nearby PCs
Pair a PC
Arrange monitors
```

After onboarding, sign-in startup normally opens only the tray. Errors that
require user action may surface a bounded notification or window. Startup
always initializes input authority as `Local`; focus, leases, epochs, held
input, and an old `Remote` state are never persisted or restored.

## 4. Settings and trust separation

The versioned, length-bounded, strictly parsed current-user settings file is
replaced atomically and contains preferences only:

```text
ApplicationSettings
  RunAtLogin
  CloseToTray
  AutoReconnect
  StartMinimized
  ShowNotifications
  OnboardingComplete

PeerSettings[]
  MachineId
  AutoConnect
  LastKnownEndpointCandidates[]   bounded and untrusted

RoamingSettings
  Links[]
  CrossingDefaults
  CanvasLayout[]                  visualization only
```

Certificates, pins, capability grants, CNG key references, trust records, and
device identity remain in their existing security stores and never become
ordinary application preferences. `AutoConnect` and `AutoReconnect` authorize
connection attempts only; they do not authorize focus or input.

## 5. Connection manager and automatic reconnection

Each configured peer has an event-driven state machine independent of input
authority:

```text
Disabled
  -> Discovering
  -> Connecting
  -> Authenticating
  -> SynchronizingTopology
  -> Connected
  -> Backoff
  -> Connecting

Any state -> UserActionRequired
```

The UI renders the reason, including `Authentication failed`, `Certificate
changed`, `Trust revoked`, or `Protocol incompatible`, rather than presenting
all terminal conditions as an ordinary offline peer.

Unexpected network loss uses bounded retry delays:

```text
immediate -> 1 s -> 2 s -> 5 s -> 10 s -> 30 s maximum
```

Add bounded jitter to prevent synchronized peers. A meaningful Windows network
change notification may schedule one rate-limited immediate retry. Successful
authenticated connection resets the backoff. The manager does not poll in a
tight loop and admits at most one active operational connection per machine ID;
simultaneous inbound/outbound duplicates converge deterministically after peer
validation. Each admitted connection carries an authenticated initiator machine
ID and connection tie-break nonce; both ends retain the same lexicographically
lowest tuple and cancel retry for the losing connection. If input is Remote,
the established connection wins regardless and the duplicate closes. A losing
connection is not treated as a retryable outage while the retained peer is
healthy.

Retryable conditions include network unavailable, address unavailable, peer
offline, and timeout before authentication. These stop retry and require user
action or a material local state change:

- certificate or identity pin changed;
- trust revoked or peer removed;
- authentication or certificate validation rejected;
- protocol/provider incompatibility;
- missing mandatory capability for an explicitly requested operation; or
- malformed authenticated control/topology traffic.

At startup, only trusted peers with `AutoConnect` enabled are discovered or
contacted. Every attempt performs normal pin, trust, provider, nonce, and
session validation. The startup sequence is:

```text
Windows sign-in
  -> local settings + tray + Local authority
  -> asynchronous discovery/reconnect
  -> peer validation
  -> fresh session nonce
  -> topology exchange
  -> saved endpoint resolution
  -> routes Ready or Unavailable
```

On disconnect while Remote:

```text
Remote -> fail local -> release state -> Disconnected -> Reconnecting
       -> Connected -> topology synchronized -> Local / routes Ready
```

Reconnect never changes `Local` to `Remote` automatically.

## 6. Display metadata

Extend the existing stable `DisplayDescriptor` with presentation metadata:

```text
PixelWidth                  u32
PixelHeight                 u32
RefreshMilliHertz           u32
PhysicalWidthMillimeters    u16
PhysicalHeightMillimeters   u16
PhysicalSizeSource          Edid | RawDpiEstimate | Unknown
Orientation                 Landscape | Portrait | LandscapeFlipped |
                            PortraitFlipped
```

Routing identity remains the DisplayConfig target device path and its derived
nonzero `DisplayId`; presentation metadata never participates in stable ID.
Refresh comes from the checked active DisplayConfig rational. Physical size
comes from the matching monitor EDID, then a bounded raw-DPI estimate, then an
explicit unknown state.

At most 64 unambiguous active displays are accepted. Strings, dimensions,
rationals, orientations, and identity collisions validate before a snapshot
can replace the current topology. Clone/mirror paths remain unavailable because
they do not identify one injection target.

## 7. Authenticated topology exchange

Add an explicit `DisplayTopologyExchange` capability and bounded reliable
snapshot after normal session admission. Acceptance requires `PeerValidated`,
trust, the current nonce, negotiated capability, exact framing, and validation
of every descriptor.

The snapshot contains the sender machine ID, nonce, topology generation, and
descriptors. It is informational and cannot focus, inject input, grant a
capability, or mutate the saved graph. Wrong-peer, malformed, oversized,
stale-nonce, pre-admission, and timed-out exchanges fail closed.

Peer connection state and route state are reported separately:

```text
Peer:   Connected | Reconnecting | Offline | User action required
Route:  Ready | Synchronizing topology | Display missing | Capability missing |
        Direction unsupported | Disabled | Invalid
```

When topology changes, active resolution is discarded immediately. Saved
endpoints remain keyed by stable identity. If the same monitor returns, a fresh
validated snapshot resolves it automatically and the route becomes `Ready`
without reconfiguration. A connected PC can therefore coexist with an
unavailable route.

## 8. Persisted link graph and directional authorization

Display IDs are scoped by machine. Persistence uses full stable identities:

```text
RoamingEndpoint
  MachineId
  StableDisplayIdentity
  Side                         Left | Top | Right | Bottom
  SegmentStartPermyriad        0..10000
  SegmentEndPermyriad          1..10000, greater than start

RoamingLink
  EndpointA
  EndpointB
  DirectionMode                Bidirectional | AToB | BToA
  CrossingPolicyAtoB
  CrossingPolicyBtoA
  PushDistancePixelsAtoB       1..64
  PushDistancePixelsBtoA       1..64
  DwellMillisecondsAtoB        0..1000
  DwellMillisecondsBtoA        0..1000
  DoublePushWindowMsAtoB       1..1000
  DoublePushWindowMsBtoA       1..1000
  LandingInsetPixels           1..64
  CornerClearancePixels        1..256
  ReentryDistancePixels        1..128
  Enabled
```

The UI defaults a new link to `Bidirectional`, but the runtime expands it into
two directed routes and validates them independently. A to B requires B's
local trust record to grant A `InputInject`; B to A requires A's local trust
record to grant B `InputInject`. A missing reverse grant never inherits the
forward grant and never silently downgrades authentication. The UI shows the
unavailable direction and may offer an explicit one-way configuration.

The runtime now uses one reciprocal `PeerSession` per authenticated peer. It
owns inbound and outbound coordinators and exchanges the exact persisted local
capability masks after `PeerValidated`. This exchange never mutates trust; the
first value is immutable for the nonce and a conflicting/unknown replay fails
both directions Local. Only one control direction may be pending or active per
peer at a time:

- an incoming focus request is rejected while outgoing focus is pending/active;
- an outgoing request is rejected while incoming control is active;
- simultaneous opposite requests resolve by rejecting/releasing both to Local,
  never by granting both; and
- epochs and leases remain issued and checked by the destination independently
  for each direction.

Every direction token is bound to the authenticated peer machine, session
nonce, generation, and direction. Reconnect invalidates all old tokens and
starts Local. Duplicate authenticated sessions and competing capture owners
are rejected during startup convergence. Reciprocal roaming is never simulated
with two uncontrolled concurrent sessions.

Graph validation rejects self-links, duplicate links, overlapping active source
segments, zero-length segments, unknown sides/directions, excessive counts,
ambiguous endpoints, and endpoints not owned by the stated machine. The first
implementation permits at most 128 links.

## 9. Configurator and Identify behavior

The canvas groups displays by PC. Each tile shows in its top-left:

```text
2560×1440 · 177 Hz
1 · LG UltraGear 27 · Primary
```

EDID physical millimeters scale tile size relative to other displays. DPI
estimates use a dotted border and `size estimated`; unknown size is explicit.
`CanvasLayout` positions and physical dimensions are presentation data only.
The edge-crossing engine is forbidden from reading them.

Dragging one PC's edge adjacent to a compatible edge on another PC creates a
visible suggestion such as **Connect both directions**. The user confirms it
before a `RoamingLink` is saved. Manual edge selection, one-way links, and
partial normalized segments remain under advanced editing. No drag operation
silently enables input.

**Identify displays** creates a borderless, topmost, no-activate, click-through
window per local display. It shows number, PC, resolution, and refresh for five
seconds, then closes without installing capture or suppression.

Saving validates a complete candidate graph before atomic replacement. If a
mutation affects the active route, DeskLink first returns local and confirms
input cleanup, then activates the new graph. Failure to return local aborts the
mutation and initiates fail-local shutdown; the active route is never modified
in place.

## 10. Crossing intent, landing, and state machine

```text
Local
  -> EdgeCandidate       current explicit route is Ready; movement is outward
  -> FocusPending        configured intent threshold met
  -> RemoteReady         fresh epoch/lease and target generation confirmed
  -> Remote              landing + initial snapshot admitted, then suppression
  -> ReturnPending       reciprocal edge, manual return, emergency, or failure
  -> LocalCooldown       remote state released; immediate bounce blocked
  -> Local
```

Crossing policies remain selectable:

- `Push`: bounded continued outward Raw Input movement;
- `DwellAndPush`: bounded dwell plus outward push; and
- `DoublePush`: two bounded outward attempts within a time window.

No universal production default is selected before physical usability tests.
Development may begin with `Push` and conservative bounds. Any velocity-aware
adjustment must stay within fixed minimum/maximum thresholds and must not turn a
single high-velocity flick into unbounded or unauditable behavior.

The source along-edge coordinate is normalized within its explicit segment and
mapped to the target segment. Landing is inset perpendicular to the edge and
clamped along the edge by `CornerClearancePixels`, away from adjacent configured
edges. A target segment too short to preserve corner clearance is invalid. A
re-entry latch clears only after bounded inward movement or a return to Local,
preventing immediate bounce or an accidental third-PC transition.

The existing Host input lifecycle remains the only suppression owner. Local
edge observation cannot directly enable routing. Suppression begins only after
fresh focus plus initial snapshot and bounded landing admission.

## 11. Fail-local and continuous-operation rules

Every transition remains or returns Local on:

- unvalidated/untrusted peer or missing directional capability;
- nonce, epoch, lease, machine, display, or topology-generation mismatch;
- focus, snapshot, landing, or queue timeout/rejection;
- topology change or target disappearance during `FocusPending`/`Remote`;
- transport loss, process termination, capture failure, restrictive mode,
  settings mutation affecting the active route, manual return, or emergency;
- invalid/disabled graph; or
- application exit, logoff, restart, or update.

An updater is later product work, but its contract is fixed now:

```text
Return Local -> release injected state -> stop capture -> close sessions
             -> update/restart
```

No automatic updater may restart while Remote. Suggested logs use
`[App:Lifecycle]`, `[Connection:Peer]`, `[Roaming:Topology]`,
`[Roaming:Edge]`, `[Roaming:Focus]`, and `[Roaming:FailLocal]`.

## 12. Implementation and validation sequence

### Companion foundation workstream

This can proceed in parallel with Phases 1-2 and must integrate before the
configurator becomes the primary UI:

1. versioned preference store, single-instance activation, first-run state,
   native broker-owned tray/hotkeys, on-demand WinUI lifetime, ordered exit,
   user-scoped sign-in bootstrap, and performance instrumentation;
2. reusable event-driven peer connection manager, per-peer auto-connect,
   bounded reconnect/backoff, duplicate convergence, asynchronous status, and
   startup-always-Local tests.

### Phase 1: metadata and graph

- Add refresh/physical-size metadata and bounded EDID/DPI fallback parsing.
- Add graph types, strict validation, stable-identity resolution, visualization
  layout separation, and atomic preference persistence.
- Test rotations, estimates, collisions, overlapping segments, reciprocal and
  one-way links, offline displays, and generation invalidation.
- No network exchange or edge switching.

### Phase 2: authenticated topology exchange — complete

- Add the explicit capability and bounded reliable snapshot.
- Gate it on `PeerValidated`, trust, nonce, and normal session admission.
- Integrate connection and route statuses plus automatic stable-identity
  recovery after hot-plug/reconnect.
- Test wrong peer, stale nonce, malformed/oversized snapshot, pre-admission
  traffic, reconnect, missing display, and same-display return.
- No edge switching.

The implementation uses an explicit `DisplayTopologyExchange` trust grant;
existing trust records are never upgraded silently. Both session roles publish
canonical snapshots on the reliable lane after trusted-session admission and
refresh them every two seconds. Remote snapshots have a five-second freshness
lease. Lower generations are ignored without extending freshness, while a
same-generation mutation, wrong machine/nonce, malformed framing, or invalid
descriptor invalidates route readiness until a new authenticated connection.
Peer connection and route status remain separate, and no Phase 2 code requests
focus or switches an edge.

### Phase 3: configurator, tray integration, and Identify — complete

- Add scaled PC/display canvas, adjacency suggestions, directional availability,
  advanced edge editing, offline/invalid states, atomic save, first-run flow,
  tray lifecycle, and five-second Identify overlays.
- Exercise with the available Windows 11 and Windows 10 compatibility machines.
- No physical input routing is required for this phase.

The implementation adds a bounded same-SID `GetDisplayTopologies` control
request. The runtime returns one current local topology and at most seven
authenticated peer entries; only a `Ready` entry may carry a canonical current
snapshot. The aggregate response is capped at 512 KiB and cannot mutate trust,
capabilities, focus, or input state.

The native configurator refreshes this view asynchronously. It scales cards by
EDID physical dimensions, marks DPI/unknown fallbacks with a dotted estimated
border, preserves saved missing displays as offline, and reports connection and
route availability separately. Drag adjacency creates only a suggestion;
confirmation saves a bidirectional full-edge link by default. Advanced controls
permit one-way and partial normalized segments. Canvas coordinates never enter
route resolution. A complete candidate graph is validated, control is confirmed
Local, and the file is atomically replaced; failure leaves the previous graph.

Identify creates one topmost, no-activate, click-through local overlay per
active display for five seconds and never starts capture or suppression. The
native broker owns tray Open/Return Local/Exit and product hotkeys. Its
single-instance WinUI settings client shows first-run guidance and exits when
closed; opt-in sign-in startup uses a short-lived bootstrap. Automatic peer
reconnection remains companion follow-up work and never restores remote focus.

### Phase 4: roaming implementation and experimental testing — automated slice complete

1. add symmetric peer-session direction arbitration and independent directional
   capability tests;
2. add Local observation, crossing policies, corner-safe landing, re-entry
   hysteresis, topology invalidation, active-route mutation handling, and Host
   lifecycle integration;
3. pass automated/simulated failures for held state, disconnect, stale nonce,
   stale epoch/lease/topology, collision, queue failure, and simultaneous
   opposite focus; and
4. enable a development/manual-testing gate and begin physical testing on the
   available supported machines. Do not wait for a second Windows 11 PC to
   exercise safe development builds.

The controlled reciprocal Phase 4 slice is implemented. `RoamingRuntime` owns
the portable `Local -> EdgeCandidate -> FocusPending -> RemoteReady -> Remote`
gate, a 1.5-second focus timeout, return cooldown, and active route/session
invalidation. `PeerDirectionArbiter` rejects stale tokens, busy directions, and
opposite-direction collisions. `PeerSession` owns both coordinators on one
authenticated connection, exchanges independent directional grants without
mutating trust, binds direction tokens to peer/nonce/generation, and starts
Local after every reconnect. The Windows input lifecycle's capture object runs
with suppression off while observing local Raw Input and is reused only after
fresh focus; landing enters the pointer datagram queue and the initial state
snapshot enters the reliable queue before the lifecycle enables suppression.

The Alpha checkbox and `--edge-roaming <absolute-settings-path>` are explicit
experimental opt-ins on either side; listener-side capture is rejected unless
that path is also supplied. The normal Local-first/manual-focus path is
unchanged. Automated tests cover both directions, independent grants,
simultaneous opposite requests, stale peer/nonce tokens, and invalid capability
replay. Physical two-PC Windows 11 qualification is still deferred.

### Phase 5: production qualification

Run the complete two-PC Windows 11 matrix before production sign-off:

- every crossing direction and rapid repeated crossings;
- mismatched resolution, refresh, physical dimensions, DPI, and scaling;
- corners, partial-edge mappings, and short/invalid target segments;
- held keyboard keys and mouse buttons;
- emergency/manual return and active-route edits;
- abrupt process exit, logoff/restart, and network interruption/recovery;
- monitor hot-plug, topology change, same-monitor return, and missing display;
- reconnect that restores `Local`/route readiness but not focus; and
- stale epoch, lease, session, identity, capability, and topology rejection.

Failure of or inability to run this matrix blocks production readiness only.
It does not block Phase 4 implementation, automated validation, or controlled
experimental physical testing.
