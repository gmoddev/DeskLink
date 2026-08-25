# Mouse roaming and monitor configurator design

Status: implementation-ready design. Physical edge switching remains disabled
until the required Windows 11 two-PC safety matrix can run.

## Goals

- Show every active display grouped by PC, with resolution and refresh rate in
  the top-left of each display tile.
- Scale tiles approximately to their physical dimensions relative to other
  displays, with a visible estimate marker when EDID dimensions are unavailable.
- Provide a five-second, click-through **Identify displays** overlay on the
  physical monitors.
- Let the user arrange monitors and explicitly connect a source edge segment to
  a target edge segment on a trusted PC.
- Convert an intentional outward edge movement into the existing authenticated
  focus transaction without weakening fail-local input behavior.

## Non-goals

- Network discovery never creates a roaming route or grants input permission.
- A monitor layout never changes trust, pairing, TLS, identity, capabilities,
  session admission, or the Windows display arrangement.
- DeskLink does not modify Windows pointer speed, acceleration, DPI, resolution,
  refresh rate, or display scaling.
- Clone/mirror display paths remain unsupported because they do not provide an
  unambiguous injection target.

## Display metadata

Extend the existing stable `DisplayDescriptor` model with presentation metadata:

```text
PixelWidth              u32
PixelHeight             u32
RefreshMilliHertz       u32
PhysicalWidthMillimeters  u16
PhysicalHeightMillimeters u16
PhysicalSizeSource      Edid | RawDpiEstimate | Unknown
Orientation             Landscape | Portrait | LandscapeFlipped | PortraitFlipped
```

Routing identity remains the existing DisplayConfig target device path and its
derived nonzero `DisplayId`. Presentation metadata must not participate in the
stable ID.

Refresh comes from the active DisplayConfig path rational and is displayed with
only meaningful precision, for example `177 Hz` or `59.94 Hz`. Physical size is
read from the monitor EDID associated with the same DisplayConfig target. If the
EDID dimensions are absent or invalid, raw monitor DPI and active pixels provide
an estimate. If neither is trustworthy, the UI uses pixel-aspect scaling and
labels the size unknown.

All arithmetic and strings retain the existing topology bounds: at most 64
displays, bounded names/identities, nonzero dimensions, checked rationals, and
one unambiguous active target per source.

## Authenticated topology exchange

Add an explicit `DisplayTopologyExchange` capability and a bounded reliable
topology snapshot message. The message is accepted only after:

1. `PeerValidated` is true.
2. the peer is in the local trust store;
3. the current session nonce matches;
4. the negotiated capability is present; and
5. the descriptor count and every descriptor field validate.

The snapshot carries the sender machine ID, session nonce, topology generation,
and descriptors. It is informational and cannot cause focus or input injection.
Unknown, malformed, oversized, stale-nonce, and pre-validation snapshots are
rejected without changing the current graph.

## Persisted roaming graph

Display IDs are scoped by machine because the 16-bit hashes can collide across
PCs. Persisted endpoints use the full stable display identity:

```text
RoamingEndpoint
  MachineId
  StableDisplayIdentity
  Side                    Left | Top | Right | Bottom
  SegmentStartPermyriad   0..10000
  SegmentEndPermyriad     1..10000, greater than start

RoamingRoute
  Source                  RoamingEndpoint
  Target                  RoamingEndpoint
  CrossingPolicy          Push | DwellAndPush | DoublePush
  PushDistancePixels      1..64
  DwellMilliseconds       0..1000
  LandingInsetPixels      1..64
  ReentryDistancePixels   1..128
  Enabled
```

The current-user settings file is versioned, length-bounded, strictly parsed,
and replaced atomically. It is separate from trust records and device identity.
At runtime, each stable identity resolves to the current `(DisplayId,
TopologyGeneration)` pair. An offline, missing, cloned, or ambiguous endpoint is
shown in the UI but never becomes an active route.

Validation rejects duplicate routes, self-edges, overlapping source segments,
zero-length segments, unknown sides, excessive route counts, and endpoints not
owned by the stated machine. The first implementation permits at most 128
routes and one active route for any point on a source edge.

## Configurator behavior

The canvas groups displays by PC. A display tile shows, in its top-left:

```text
2560×1440 · 177 Hz
1 · LG UltraGear 27 · Primary
```

Tile width and height are proportional to physical millimeters. EDID-backed
sizes use a solid border; DPI estimates use a dotted border and `size estimated`
badge. Dragging changes only DeskLink's desk layout. Tiles snap to nearby tile
edges while preserving deliberate gaps and may not overlap within one PC group.

**Identify displays** creates one borderless, topmost, no-activate, click-through
window per local display. Each overlay shows its number, PC name, resolution,
and refresh rate, closes after five seconds, and never installs capture or
suppression.

Selecting a display exposes its metadata and configured edges. Selecting a
source edge and then a target edge creates a colored route. The default maps the
full along-edge position proportionally; advanced editing can shorten either
segment for physically partial overlaps. Offline PCs and stale displays are
rendered muted and their routes show `Unavailable`.

Saving first validates the entire graph, then atomically replaces the previous
settings. Invalid edits never partially apply. A preview mode can exercise tile
layout, identify overlays, topology exchange, and graph validation before edge
switching is enabled.

## Crossing state machine

```text
Local
  -> EdgeCandidate       pointer is on an enabled edge and moving outward
  -> FocusPending        intent threshold met; send normal focus request
  -> RemoteReady         fresh focus epoch/lease and target topology confirmed
  -> Remote              send landing position + initial state snapshot,
                         then enable suppression
  -> ReturnPending       reciprocal edge, manual return, emergency, or failure
  -> LocalCooldown       release remote state and suppress immediate bounce
  -> Local
```

An edge candidate requires both position and outward motion. The default policy
uses a 120 ms dwell plus 8-pixel-equivalent outward push. Configurable corner
dead zones avoid accidental switches while selecting controls such as Start or
Show Desktop.

The source along-edge coordinate is normalized within the configured source
segment and mapped proportionally into the target segment. The first remote
absolute pointer event lands inside the target by `LandingInsetPixels`; ordinary
movement remains relative after the transition. A re-entry latch clears only
after the pointer moves inward by `ReentryDistancePixels` or returns local.

The existing Host input lifecycle remains the only suppression owner. Edge
detection may observe unsuppressed Raw Input in Local mode, but it cannot set
remote routing directly. Suppression begins only after the same fresh focus and
initial snapshot requirements used by manual focus.

## Fail-local rules

Every transition remains Local or returns Local when any of these occurs:

- peer not `PeerValidated`, untrusted, or missing `InputInject`;
- session nonce, epoch, lease, machine ID, display ID, or topology generation
  mismatch;
- target display disappears or topology changes during `FocusPending`;
- focus request, reliable snapshot, absolute landing event, or queue admission
  fails or times out;
- transport disconnect, process termination, capture failure, restrictive
  foreground mode, manual return, or Ctrl+Alt+Pause/Break;
- graph settings become invalid or the route is disabled.

No input event is suppressed unless it has been admitted to the bounded sender
path under current remote authority. On fail-local, DeskLink disables routing,
releases DeskLink-owned remote key/button state, releases focus, stops capture
when required by the lifecycle, and enters a local cooldown. Suggested logs use
`[Roaming:Topology]`, `[Roaming:Edge]`, `[Roaming:Focus]`, and
`[Roaming:FailLocal]`.

## Implementation sequence

### PR 1: metadata and graph foundation

- Add refresh/physical-size metadata and bounded EDID/DPI fallback parsing.
- Add portable graph types, strict validation, stable-identity resolution, and
  atomic current-user persistence.
- Add unit tests for dimensions, rotations, estimates, collisions, overlapping
  segments, offline displays, and topology-generation invalidation.
- No network exchange, input capture, or edge switching.

### PR 2: authenticated topology exchange

- Add the explicit capability and bounded reliable snapshot message.
- Gate it on `PeerValidated`, trust, nonce, and session admission.
- Add wrong-peer, stale-nonce, malformed, oversized, reconnect, and pre-admission
  rejection tests.
- No edge switching.

### PR 3: configurator and identify overlay

- Add the scaled PC/display canvas, selection inspector, edge editor, offline
  states, graph validation, atomic save, and five-second local identify overlay.
- Ship in preview mode with physical roaming unavailable.
- This PR can be implemented and manually inspected with the current PC plus
  the Windows 10 compatibility PC because it does not route physical input.

### PR 4: edge detector and focus transaction

- Add Local-mode observation, crossing intent, absolute landing, re-entry
  hysteresis, topology invalidation, and integration with the existing Host
  lifecycle.
- Keep the feature behind a build/runtime gate that defaults off.
- Unit and simulated failure tests must pass before any physical run.

### PR 5: physical Windows 11 validation and enablement

- Run the full two-PC Windows 11 matrix: crossings in every direction, unequal
  resolution/refresh/physical sizes, corners, held inputs, emergency return,
  process termination, network interruption, hot-plug, changed topology,
  reconnect, stale epoch/session/topology rejection, and rapid bounce attempts.
- Enable production roaming only after that matrix passes. Until a second
  Windows 11 PC is available, PR 4 remains gated and PR 5 remains deferred.
