# DeskLink Foundation Validation

## Validation environment

The portable core was built in the artifact-generation environment using:

```text
GCC 14.2.0
CMake 3.31.6
C++20
```

The Windows-only `desklink_windows` target was disabled because the validation environment is Linux. Its source is included for Windows builds.

---

## Normal build

Commands:

```bash
cmake -S . -B build -DDESKLINK_BUILD_WINDOWS_BACKENDS=OFF
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

Result:

```text
100% tests passed, 0 tests failed
```

---

## Sanitizer build

The portable core/tests were also rebuilt with:

```text
-fsanitize=address,undefined
-fno-omit-frame-pointer
```

Result:

```text
100% tests passed, 0 tests failed
```

No AddressSanitizer or UndefinedBehaviorSanitizer failure was reported by the test run.

---

## Independent remote revalidation

The public repository was cloned and rebuilt on 2026-08-21 using Docker Desktop
on a separate Windows PC. The source was mounted read-only into disposable Linux
containers.

Release validation passed with both toolchains:

| Toolchain | CMake | Result |
|---|---:|---|
| GCC 15.2.0 | 4.2.3 | Build, tests, and simulation passed |
| Clang 22.1.3 | 4.2.3 | Build and tests passed |

The GCC build was repeated with AddressSanitizer and UndefinedBehaviorSanitizer.
All tests passed with no sanitizer failure reported.

The remote Windows host did not have CMake or the Visual Studio C++ workload
installed. GitHub Actions subsequently built and tested the project with MSVC
on `windows-latest`, including compilation of the native `desklink_windows`
target.

---

## Product UX PR 4 automated validation

The persistent role-driven broker slice was validated locally on 2026-08-26:

- the full Release Windows/MsQuic configuration passed 8/8 CTest cases;
- the independent core/non-MsQuic configuration passed 3/3 CTest cases;
- the native MsQuic loopback test passed 25 consecutive runs;
- the private-key-export source gate passed; and
- CPack produced the portable Windows x64 ZIP.

The security diff review covered every changed source file and found two
low-severity close-path defects before publication. The resulting regression
tests now prove that a known transport close performs immediate session
fail-local/owned-input cleanup, an intentional peer close remains ordinary
unavailability, and a nonzero peer QUIC application error becomes a protocol
failure rather than the broker's retryable exit.

Physical two-PC Windows 11 reconnect qualification remains deferred until a
second supported Windows 11/Server 2022+ system is available.

---

## Lightweight background lifetime validation

The broker-owned tray lifecycle was measured on the Windows 11 development PC
after a real trusted Windows 10 compatibility connection. Before this change,
the hidden WinUI process retained about 65.5 MiB of private working set. With
the visible settings window closed, no `desklink.exe` process remained; the
Schannel broker used 2.47 MiB and its connected transport child used 5.35 MiB,
for 7.82 MiB total private working set. The same Windows 10 OpenSSL/CNG path
used 2.06 MiB for the broker and 6.50 MiB for its transport child. These are
development measurements rather than release guarantees.

The test launched `desklink.exe --background`, required that bootstrap to exit,
opened and closed the normal WinUI window, confirmed the broker and connected
session survived, returned input Local, and reran the identity snapshot. Key
name, provider, algorithm, zero export policy, public key, certificate DER
hash, and DeskLink pin remained unchanged on both PCs.

---

## Product UX PR 5-8 automated validation

The WinUI deployment foundation is built with locked NuGet resolution and an
unpackaged self-contained x64 payload. Local validation proves:

- locked restore/build and self-contained `--smoke-test` launch;
- a 243-file allowlisted staging payload with required core graph files,
  pinned SDK license/notices, no reparse points, and valid Microsoft signatures
  on every redistributed DLL/EXE;
- distinct Alpha and product-shell lifecycle mutexes, secondary activation,
  a short-lived `--background` WinUI bootstrap, broker-owned tray lifetime,
  complete visible-shell exit, and bounded explicit exit;
- the full Windows core regression suite after the new fail-local presentation
  mapping tests;
- interactive UI Automation controls have accessible names; and
- real Inno Setup 7.1.0 compilation of the unsigned development installer.

PR 5 intentionally drove the shell from simulated broker states. PR 6 replaces
those simulations with the current-user control protocol. The shell
now reads broker-owned state, preferences, trust records, discovery results,
and pairing candidates without opening transport, identity, capture, or input
authority itself. Automated PR 6 validation additionally proves:

- all 25 control requests and their bounded typed responses round-trip, while
  invalid discovery durations, manual hosts, candidate bindings, tokens,
  fingerprints, phases, and cross-field combinations fail closed;
- broker-launched pairing children require the matching nonzero 128-bit token
  and operation ID, while direct command-line pairing retains its existing
  explicit local-confirmation path;
- a managed pairing candidate is bound to its operation, peer identity,
  fingerprint, transcript, requested grants, and 90-second lease;
- Nearby results remain untrusted and bounded to 64 entries, and only a unique,
  compatible, pairing-open cached record can enter pairing;
- a pre-canceled native browse performs no network work, while an active browse
  checks cooperative cancellation at 50-millisecond intervals so stop, update,
  and broker teardown do not inherit the requested 30-second scan lifetime;
- permission reductions and forgetting first force Local cleanup, while grant
  additions require a bounded reject-default local candidate tied to the exact
  existing identity pin and grant snapshot; stale/replayed/expired candidates,
  identity changes, and cleanup failures add no authority; and
- successful external pairing reloads the protected trust store before the
  role-driven runtime can resume.

The pairing child and broker also reject a managed operation when the product
shell is absent, exits, rejects, times out, supplies a stale operation ID, or
cannot complete the local pipe exchange. No such path admits a session or
mutates trust.

PR 7 adds release and Debug builds of the production monitor page plus focused
core coverage for its save coordinator. The tests prove that an active runtime
returns Local before the atomic-write callback, every save confirms Local,
cleanup failure never reaches persistence, and live preference negotiation is
attempted only after a successful write. Store and preference-apply failures
remain explicit and fail Local without replacing the authenticated session.
Existing monitor-model tests continue to prove bounded physical sizing,
stable/offline identities, deterministic full-edge suggestions, presentation-
only canvas movement, strict graph validation, and route resolution against
current topology rather than canvas coordinates.

Clean-system keyboard, Narrator, DPI, high-contrast, and visual qualification
of the PR 7 surface remains a release gate. The real two-supported-Windows-11-PC
roaming/fault qualification also remains deferred until that hardware exists.

PR 8 extends the portable and Windows suites with product-preference schema-4
round trips, exact version-1/version-2/version-3 migration, malformed/reserved/trailing
data rejection, bounded explicit-endpoint rejection, allowlisted and conflicting hotkeys, duplicate profile
rejection, global fullscreen fail-local precedence, all three product crossing
presets, and exact launcher propagation into the managed Host child. The
current local control codec round-trips the bounded preferences, exact trusted
connection state, and permission-authorization candidate payloads.

The standard Release tree, MsQuic-enabled Release tree, and locked WinUI
Release payload must all compile. CTest must pass the portable/core, reliability
soak, native MsQuic runtime, and native Schannel loopback gates. Physical
focus-hotkey feel, fullscreen transitions, clipboard/audio intention UX, and
two-supported-Windows-11-PC failure qualification remain PR 9 release gates;
the user has explicitly deferred that hardware matrix.

Disposable-account CI additionally exercises installed shell activation,
product-shell-only broker startup, broker survival after shell exit, update
shutdown, shell/broker health probes, exact startup rollback/migration, and
uninstall. It now repeats the complete artifact-identical flow on a dependent
Windows Server 2022 worker and includes same-version repair. It seeds a valid
DPAPI trust record, schema-4 preferences, and saved
roaming layout, then compares their hashes plus the complete non-exportable CNG
identity snapshot after forced rollback and successful upgrade.

Static product-contract validation rejects lost PerMonitorV2/as-invoker
metadata, inaccessible interactive controls, missing navigation access keys or
live status announcements, hard-coded theme colors, malformed UTF-8/mojibake,
an unsupported OS floor, and installer elevation/Firewall behavior. Broker
controller tests prove suspend/resume preserves user pause and security
`ActionRequired`, while ordinary wake schedules only a fresh Local session;
the production broker uses the corresponding native Windows notification.
Production signing plus clean Windows 11 hands-on DPI, keyboard, Narrator,
light/dark, contrast, physical sleep, and network-loss qualification remain
release gates.

---

## Simulation output

The simulation demonstrates:

- focus acquisition
- epoch creation
- keyboard event acceptance
- absolute pointer acceptance
- bounded vertical/horizontal mouse-wheel acceptance
- key release
- lease expiry cleanup
- stale packet rejection
- Host emergency fail-local

Representative output:

```text
DeskLink foundation simulation
requesting remote focus...
agent decision=0
focus epoch=2
agent: key scan=30 down
agent decision=0
agent: pointer display=0 x=42000 y=25000
agent decision=0
agent: mouse wheel axis=1 delta=120
agent decision=0
agent: key scan=30 up
agent decision=0
simulating network silence past lease...
agent: release DeskLink-owned key/button state
stale decision=3
host mode after emergency=1
```

---

## Multi-monitor mapping validation

The portable topology tests construct a two-monitor virtual desktop with a
negative X origin and verify deterministic IDs regardless of enumeration order,
friendly-name changes without generation churn, per-display corner mapping,
material rectangle-change invalidation, stale-generation rejection, unknown-ID
rejection, duplicate-identity rejection, and fail-closed 16-bit ID collision
handling.

On 2026-08-23, the Windows build used the production DisplayConfig enumerator on
the local Windows 11 PC and found three active displays. Every descriptor had a
nonzero deterministic ID, nonempty normalized target-device identity, valid
rectangle, and a single primary display. The Windows injector retains display
zero as the legacy whole-virtual-desktop path; a nonzero mapping is pinned to the
current topology generation and is rejected after a material topology change
until focus is released.

### Authenticated topology-exchange validation

Portable tests encode and decode canonical topology snapshots on the reliable
lane, enforce the aggregate 64 KiB bound, and reject truncation, wrong-lane use,
embedded NULs, duplicate descriptors, noncanonical IDs, malformed bounds, and
oversized dense 64-display snapshots. Admission tests require the explicit
capability, expected trusted machine, and current envelope and payload nonce.
They cover lower generations, conflicting same-generation metadata, five-second
freshness expiry, missing displays, unsupported directions, and invalid route
state.

The in-memory HostSession/AgentSession test exchanges topology in both
directions, verifies two independent Ready states, invalidates them on wrong
nonce or malformed authenticated traffic, and proves recovery only through a
fresh connection and session nonce after rejection. Existing tests continue to
prove that pre-validation MsQuic stream traffic is unavailable. Phase 2 does
not request focus, enable capture, or switch an edge.

### Phase 3 configurator validation

Portable tests build EDID-sized and DPI-estimated display cards, retain saved
missing displays as offline, honor exact saved canvas coordinates, reject
invalid/duplicate machines, and produce a full-edge bidirectional suggestion
only for compatible adjacent displays on different PCs. Moving cards removes
or changes only the suggestion; the same stable-identity link continues to
resolve against current topology, proving canvas geometry is not routing input.

Control-codec tests round-trip canonical topology state, require one unique
local machine, enforce `Ready`/snapshot correspondence, reject simultaneous
state/topology response payloads, and exercise the larger response through the
same-user Windows named pipe. Windows tests also verify strict atomic companion
preference persistence and malformed/trailing-data rejection. The configurator
and Identify surface compiles under `/W4 /permissive-`; physical edge movement
is intentionally absent until Phase 4.

---

## LAN DNS-SD/mDNS discovery

The portable tests round-trip the six required TXT properties, accept unknown
bounded keys, and reject missing/duplicate/noncanonical/oversized fields,
invalid UTF-8 text, wrong protocol versions, invalid service/host names, and
zero ports/interfaces. Cache tests cover deterministic multi-interface
selection, conflicting metadata, exact removal, and TTL expiry.

On 2026-08-23, the Windows 11 build successfully registered a bounded
`serve` advertisement. The Windows 10 Docker PC, running the same staged
`discover 8` executable from `C:\Sandbox\Codex\Workspaces\desklink-discovery`,
resolved it as protocol 1 on the exact UDP port with the expected machine ID,
display name, capability hint, closed-pairing state, and no ambiguity. No trust,
pairing, connection, focus, firewall, network-profile, or Docker-workload state
was changed.

The reverse probe used Windows 10's existing non-exportable CNG identity and
the hash-pinned OpenSSL/CNG runtime. Registration reported success, but the
Windows 11 PC did not observe the advertisement in either an 8-second or
20-second browse. No firewall or network changes were authorized or attempted,
so symmetric physical discovery remains an environment/interoperability
follow-up. The Windows 10 identity snapshot was byte-for-byte unchanged after
the probe (`export_policy=0`, certificate/DeskLink pin
`95dfabcf017751cbb6f863c147c638e7ae678d7c88462eaa757fee494e1f8108`), and all
pre-existing Docker containers remained running.

---

## Physical pairing-control check

On two Windows 11 or Windows Server 2022-or-newer PCs on the same trusted LAN,
build `desklink_pair` against the pinned Schannel package. Start the listener on
the PC that should accept remote input:

```powershell
desklink_pair.exe listen 43821 --grant-input --grant-topology
```

Then connect from the other PC:

```powershell
desklink_pair.exe pair <listener-ip> 43821 --grant-topology
```

Verify that both prompts show the same six-digit code and the intended
capability consequence before selecting Yes. Repeat after restarting both tools
to verify that `%LOCALAPPDATA%\DeskLink\trust.db` remains readable. A firewall
exception, if needed, must be created manually and limited to the appropriate
Private or Domain network profile.

For SSH-driven compatibility validation where Windows session isolation hides
message boxes, add `--console-confirm` to both pairing commands. Each side prints
the same six-digit code and waits for the exact input `yes`; no confirmation is
automatic. A local confirmation is exchanged on the peer-validated pairing
stream, and neither side reports success or persists new trust until both
confirmations arrive. Trusted connections also log their shared session nonce so
reconnect rotation can be recorded without exposing identity or trust secrets.

Windows 10 remains unsupported. The MsQuic 2.6.x foundation, opaque CNG/OpenSSL
provider, fail-closed admission matrix, identity-invariance comparison, and
private-key-export source gates now pass. The guarded physical compatibility
matrix also passes. Windows 10 remains experimental/unsupported pending a
separate production-admission and release-integration review. It never receives
an OpenSSL reduced-security fallback.

The guarded physical Windows 11 Schannel to Windows 10 OpenSSL run has passed
manual same-code pairing, bilateral trust persistence, trusted reconnect, two
matching cross-provider session nonces, nonce rotation on the second reconnect,
and focus without capture. The before/after identity snapshots were identical
on both PCs and retained export policy zero. Physical keyboard, button, and
pointer forwarding plus interactive `SendInput` observation also passed. The
completed R&D matrix is not by itself a Windows 10 production-support claim.

On 2026-08-23, the same guarded pair passed input-state reconciliation over a
fresh Windows 11 Schannel to Windows 10 OpenSSL connection. The opt-in
`desklink_pair_validation.exe` receiver deliberately acknowledged but omitted
one key release and one X2-button release after their corresponding downs.
The next reliable 500 ms authoritative snapshot repaired both states. Receiver
diagnostics recorded both `validation fault injected` events followed by both
matching `validation fault recovered` events. The sender did not enable
physical capture for this deterministic probe. The normal `desklink_pair.exe`
does not accept these validation options, and the validation control is built
only when `DESKLINK_BUILD_PHYSICAL_VALIDATION=ON` is explicitly selected.

The guarded pair also passed abrupt held-input process termination. The
validation sender transmitted one held key and one held auxiliary mouse button,
then terminated with no focus release and without running C++ destructors. The
Windows 10 agent stopped receiving 750 ms lease renewals, observed both
DeskLink-owned states held at expiry, and released both successfully. A
subsequent cleanup observed neither state held.

A subsequent reconnect rotated the shared session nonce again. The sender
delivered one valid validation key down/up pair, then wrote one authenticated
packet carrying the prior connection's nonce and one carrying an old focus
epoch directly to the reliable lane. The Windows 10 receiver recorded
`accepted_probe_deliveries=2`, `stale_epoch_deliveries=0`, and
`stale_session_deliveries=0`.

The guarded pair then passed the approved network-interruption case without
changing adapters, routers, Docker workloads, or persistent firewall policy.
After a trusted session reached focus-active with nonce
`11952655969917357047`, the Windows 10 target applied inbound and outbound block
rules for four seconds, scoped to the validation executable, Windows 11 address,
UDP, and local port 43821. The sender failed local when focus-lease renewal
failed. Independent cleanup and a watchdog removed both exact rules, and a
readback found zero remaining rules. A fresh receiver accepted a reconnect with
new nonce `6118348576609549568`; it recorded
`accepted_probe_deliveries=2`, `stale_epoch_deliveries=0`, and
`stale_session_deliveries=0` when probed with the interrupted nonce and an old
focus epoch. This completed the Stage 5 physical matrix.

Final post-test identity snapshots remained byte-for-byte equal to the earlier
snapshots on both PCs: key name `DeskLink-Device-Identity-v1`, Microsoft
Software Key Storage Provider, RSA, export policy zero, identical public-key
DER, identical certificate DER hashes, and identical DeskLink pins. The local
pin remained `52f3f6ebc9e35b96a6b668890b7a0e24f77bac5d9be602a00e02cb971282f50b`;
the Windows 10 pin remained
`95dfabcf017751cbb6f863c147c638e7ae678d7c88462eaa757fee494e1f8108`.

The native MsQuic loopback additionally verifies that both trusted endpoints
receive the same nonzero session nonce and that reconnecting rotates it.

Stage 2 CI builds the pinned OpenSSL 3.5.7 source, applies the reviewable patch
to the exact MsQuic 2.6.0 commit, and runs both provider loopbacks. Its OpenSSL
gate verifies pairing, trusted reconnect, fresh nonce, reliable traffic,
exportable-key rejection, certificate/key mismatch rejection, and pinned hash
rejection for MsQuic, libcrypto, and libssl. It also rejects the patch if it
mentions private-key export APIs, ENGINE, or `RSA_METHOD`. These are prototype
gates. Stage 3 adds missing/malformed/time-invalid DER classification, wrong
pin, unknown peer, validator failure/exception/timeout, signing failure, and
changed-identity rejection. Every network case asserts that no pairing or
trusted application session was delivered.

Stage 4 snapshots both loopback identities before and after Schannel and
OpenSSL/CNG handshakes. It compares the actual CNG key name, provider,
algorithm, export policy, certificate public-key DER, certificate DER hash, and
DeskLink identity pin byte-for-byte. A CMake build target and test reject the
three prohibited private-key export APIs across the DeskLink credential/runtime
boundary and downstream patch.

For a physical-test record, run `desklink_pair.exe identity` on each PC before
and after the matrix and preserve the output. The command emits the full public
SubjectPublicKeyInfo DER plus every other invariant field; its two records must
match exactly, and it exits unsuccessfully if the export policy is nonzero.

After pairing with `--grant-input` on the receiving PC, run
`desklink_pair.exe serve 43821` there and
`desklink_pair.exe focus <receiver-ip> 43821 --capture` on the other PC. Verify
that keyboard, buttons, and pointer movement reach the receiver while the sender
stays locally suppressed. Press Ctrl+Alt+Pause and verify local input returns
immediately, the remote lease is released, and held remote state is cleaned up.
Repeat using Enter as the normal release path. While capture is active, verify
both vertical and horizontal wheel input reaches the receiver without also
scrolling locally. Saturate or fault the bounded sender path in an instrumented
build and verify routing disables before an unqueued wheel event is suppressed;
that physical event must continue locally.

For current-user IPC validation, leave one `serve` or `focus` process running
and execute from a second process under the same Windows user:

```powershell
desklink_pair.exe control state
desklink_pair.exe control mode game
desklink_pair.exe control state
desklink_pair.exe control mode roam
desklink_pair.exe control gain 5000
desklink_pair.exe control state
desklink_pair.exe control mute
```

The state response must report the expected role, connected-peer count, focus,
capture, and desired mode without exposing input contents or trust secrets.
On an active Host receiver, gain must report `5000`, rendered samples must ramp
to 50% within one five-millisecond block, and mute must ramp to silence without
changing the Windows endpoint volume. An Agent or inactive Host must return
`NotReady`. Audio-only endpoint recovery must preserve both values.
`game` and `lock` must release remote focus/capture fail-locally. On an Agent,
send a remote `Roam` preference after selecting a restrictive local mode and
verify the effective mode remains restrictive. A second server for the same
user endpoint must fail rather than silently sharing or replacing the pipe.
Malformed, oversized, stalled, wrong-request-ID, and unsupported commands must
not invoke a handler or produce an application action.

Foreground-profile foundation validation covers the 32-rule bound, executable
name and UTF-8 validation, ASCII case normalization, duplicate rejection,
fullscreen-only matching, and emergency/manual/profile/default precedence. A
configured but missing or uninspectable foreground must select `LockPc1`. The
Windows test installs the out-of-context foreground WinEvent hook, receives an
initial bounded snapshot even when no inspectable foreground exists, and stops
the monitor through same-thread unregistration. Runtime GAME capture teardown
and recreation have a portable ordering test: routing is disabled before focus
release, hook teardown completes before the restricted mode is applied, stale
`FocusReady` cannot reinstall capture, and a new capture starts only after a
fresh focus grant and initial state snapshot. Snapshot and capture-start
failures return to `LockPc1`. The opt-in Windows capture smoke test also performs
a complete start/stop/restart/stop cycle with remote routing disabled.

Production profile-runtime validation additionally requires:

1. invalid paths, modes, malformed specifications, duplicate normalized rules,
   and a 33rd rule are rejected before identity or network work;
2. a live matching foreground transition to `game`/`lock-pc1` disables routing,
   releases focus, and completes hook teardown in lifecycle order;
3. a later permissive transition requests a fresh focus transaction and cannot
   route before `FocusReady` plus the initial snapshot;
4. control-pipe mode changes act as manual overrides above profile/default;
5. intentional restricted modes do not run renewal or report a lease failure;
6. WinEvent, control, `FocusReady`, renewal, and capture-failure concurrency
   remains serialized, and every forwarding/start/snapshot failure fails local.

Controlled-roaming automation now covers Push, DwellAndPush, and DoublePush;
edge/segment rejection; proportional horizontal, vertical, and reverse
landing; first-sample authenticated remote-edge return; target corner
clearance; re-entry cooldown; the 1.5-second focus timeout; presentation-only
canvas mutation; and active link/topology/nonce invalidation. Negative cases
revoke peer validation, `InputInject`, topology
freshness, or link enablement and prove that none can reach Remote. Reciprocal
session tests exercise focus and input in both directions, independent local
grants, simultaneous opposite intent, busy ownership, peer/nonce/generation-
bound stale tokens, invalid capability replay, and refusal to treat a remote
grant report as local audio/topology disclosure consent. Positive cases retain
explicit bidirectional audio and topology exchange. The Alpha argument tests
require Focus or Serve, physical capture, and an absolute settings path before
the experimental path can start; Serve capture without edge roaming is denied.

Physical-distance landing tests additionally prove that EDID-backed landscape
segments preserve distance along their shared physical edge only when both
segments are at least 25 mm and their spans agree within the larger of 5 mm or
5%. Raw-DPI estimates, rotated displays, contradictory spans, and short segments
all use the existing proportional fallback. Recorded traces prove **Cross
immediately** admits the first positive outward count at an exact configured
edge, including shallow diagonal and high-poll-rate input, while lateral-only
edge skims, corners, and partial-edge misses remain rejected. Pause-and-push
and double-push retain the bounded 60% outward-intent threshold. The traces
exercise all three policies and verify cooldown in both roaming directions.

The standalone reliability harness is registered in CTest with 2,000 default
iterations. A bounded extended run is:

```powershell
desklink_reliability_soak_tests.exe --iterations 1000000 --seed 123456789
```

It deterministically alternates reciprocal directions and cycles graceful
return, cable-loss state, nonce rotation, peer topology change, local monitor
hot-plug generation change, capability revocation, focus timeout, sleep/wake,
process termination, and RDP detach. Every iteration also checks complete
release of a held Ctrl/Alt/extended-Ctrl plus left/right-button chord, reliable
vertical or horizontal wheel round trip, canonical bounded audio load, active
request cleanup, fail-local state, outward cooldown rejection, and inward
cooldown release. On 2026-08-26, the Windows Release suite passed all 8 tests,
the full Windows AddressSanitizer configuration passed all 9, the extended
1,000,000-iteration run passed, and isolated Linux passes on the trusted Docker
worker completed under GCC 13 (3/3) and Clang 17 with Address/Undefined
sanitizers (3/3). Existing Docker workloads were unchanged.

These tests do not replace physical qualification. Still required on two
supported Windows 11/Server 2022+ systems are high-poll-rate outward motion,
visible landing on differently scaled displays, held-state transitions,
cooldown/re-entry feel, emergency/manual return, process/network loss, monitor
hot-plug, and reconnect without automatic refocus. Reciprocal roaming is not a
production-qualified path until this symmetric physical matrix passes; the
peer-session owner itself is implemented and ready for that deferred test.

---

## WASAPI foundation validation

The portable suite splits and joins PCM input across packet boundaries, checks
exact 48 kHz/stereo/PCM16/240-frame output, verifies five-millisecond timestamp
steps, emits a full silence block, and rejects a non-frame-aligned byte count.
The standard Windows build compiles the concrete Core Audio adapter under
`/W4`.

The session suite also proves that outbound audio requires the peer's
`AudioReceive` grant, inbound audio requires `AudioSend`, and neither direction
is available before an authenticated/encrypted pinned session starts. It checks
fresh nonce binding, canonical format rejection, invariant stream ID, late and
duplicate sequence rejection, bounded jitter concealment, renderer rejection,
and audio-only fail-stop behavior. The end-to-end in-memory session transfers
canonical datagrams through both capability gates and pumps them into the
receiver callback without acquiring input focus.

Deterministic adaptive-jitter tests prove immediate bounded target growth for
arrival variation, one-block decreases only after 200 stable samples, a hard
12-block ceiling, reordered-sample rejection, concealment-driven growth,
explicit rebuffer accounting, maximum-frame clamping, and reset behavior.

Deterministic clock-drift tests prove a quarter-block occupancy deadband,
50 ppm bidirectional slew steps, hard ±1000 ppm saturation, discontinuity
reset, exact 240-frame resampled output, and bounded long-run source storage for
both compression and expansion. Receiver/session and endpoint recovery tests
continue to clear the complete audio pipeline, including drift state.

The Windows suite classifies endpoint change/unavailability as recoverable and
capture client/send rejection as non-recoverable. Runtime recovery preserves
the current admitted session and uses a capped 250 ms to 5 s retry interval;
receiver recovery clears jitter and renderer queues before playout resumes.

Set `DESKLINK_WASAPI_SMOKE=1` for the opt-in native device test. It opens the
current default render endpoint for event-driven loopback, starts and stops the
capture worker, opens a shared render stream, submits one canonical silence
block, and verifies that neither worker reports a failure. The handler never
logs or persists captured samples. The smoke then performs a second complete
capture start/stop and renderer start/submit/stop cycle on the same objects,
proving notification registration, worker teardown, event handles, and queues
are reopen-safe.

This device smoke does not physically switch or disable the default endpoint;
that matrix, sustained physical two-PC timing, drift correction, and gain/mute
remain separate validation.

## Text clipboard validation

The portable suite proves reliable-only canonical hello/text framing, strict
UTF-8 including overlong/surrogate/NUL rejection, the 48 KiB bound, every
truncated frame, nonzero authenticated origins/update IDs, explicit default-off
startup, complementary read/write consent, and refusal to infer support from
protocol-v2 capability bits without the module handshake. It also covers wrong
peer and nonce, replay/stale update, 20-per-second gates, one-sided consent,
fresh reconnect/update-ID reset, and rejection of the prior session nonce.

Session fault tests make the apply callback reject and throw, then successfully
complete focus and input traffic on the same authenticated connection. This is
the required proof that clipboard failure has no direction, focus, lease,
capture, or input-cleanup authority. Typed launcher tests cover both grant flags
and `--sync-clipboard`, reject them outside their allowed pairing/session scope,
and preserve production Schannel pinning. The standard Windows build compiles
the message-only `CF_UNICODETEXT` adapter with bounded open attempts, an
eight-write queue, strict conversions, current-session memory-only content, and
exact sequence/text loop suppression.

No default automated test mutates the interactive user's real clipboard.
Production qualification therefore still requires two supported physical PCs
to verify consent UX and privacy in both directions, rapid-copy/contention
behavior, clipboard-owner process exit/delayed rendering interactions, loop
suppression, interruption, and reconnect. Until that matrix passes, text
clipboard remains experimental; image clipboard and file transfer remain out
of scope.

## Windows installer validation

The Windows MsQuic job installs a hash-pinned, Authenticode-verified Inno Setup
7.1.0 compiler and stages CMake's `Alpha` component plus the exact
self-contained product-shell payload. The packaging script
rejects unexpected files, reparse points, the wrong Schannel runtime digest,
and every production invocation that lacks both an explicit current-user code
signing certificate and timestamp URL.

On the disposable CI account, the installer test first holds the Alpha and
product-shell lifecycle mutexes and proves Setup cannot proceed. It proves coordinator-mode
Setup cannot run without the update mutex and that ordinary Setup/runtime
startup cannot overlap that mutex. After installing 0.1.0, it seeds valid
product preferences, a saved monitor layout, one current-user DPAPI trust
record, and an exact legacy Alpha Run value. Starting
`desklink.exe --background` must start the broker and exit without retaining a
WinUI process. A normal product launch must support secondary activation, and
closing that visible shell must leave the lightweight broker responsive. The
explicitly labeled Alpha diagnostics shell is then started to
retain dual-UI update-shutdown coverage. The production updater must reject
unsigned packages without stopping either UI or changing the version.

A validation-only updater injects candidate health failure and must restore
0.1.0 plus the exact prior startup command. A clean transaction then advances
to 0.1.1 and migrates that exact command to `desklink.exe --background`.
Broker/state health probes and the product-shell validation mode must pass
during both paths. The
test compares the full CNG key/provider/algorithm/export-policy/public-key/
certificate-hash/identity-pin snapshot and exact hashes of trust, preferences,
and roaming files after each transaction. Finally uninstall removes the Run
value and installer-owned files while the state files and sentinel remain
byte-for-byte unchanged. The test requires an explicit mutation switch and is
not run on developer accounts.

Portable coordinator tests independently cover exact lifecycle ordering,
pre-shutdown validation failure, local-confirmation refusal, backend exception
containment, candidate install/health rollback, rollback failure with no
restart, and restart failure.

This is an unsigned development-artifact gate. Production qualification still
requires the actual release-signing identity plus clean Windows 11 and Server
2022 signed install/repair/update/rollback/uninstall validation and destructive
fault injection at each transaction phase.

---

## Authenticated audio latency diagnostics

The non-installed `desklink_pair_validation` target accepts
`--validation-audio-latency` only for `serve --send-audio` and
`focus --receive-audio`. Both peers must opt in. The focus peer sends 32
authenticated reliable clock probes and retains the lowest-round-trip NTP-style
sample. In this mode only, the sender replaces each captured block timestamp at
the immediate `SendAudioFrame` boundary. The receiver records non-silent blocks
at authenticated datagram arrival and immediately before WASAPI submission.

At shutdown the receiver reports sample count, median, p95, p99, and maximum
for emit-to-arrival and emit-to-submit, plus the selected clock offset, best
round trip, and half-round-trip uncertainty. This measures DeskLink's software
transport and buffering path. It does not measure WASAPI device buffering,
DAC/speaker delay, room propagation, or microphone capture; an acoustic
loopback rig is still required for true sound-to-sound latency.

The clock exchange and timestamp observations are disabled in the installed
binary. They run only after authenticated session admission, accept no
unauthorized audio, never log PCM, and have no authority over validation,
capabilities, focus, playout, leases, or reconnect decisions.

## Voice-forwarding validation

Protocol-v5 tests cover exact voice framing, datagram-only lane policy,
malformed metadata and bounds, Opus encode/decode, FEC/PLC, stream/sequence
rejection, adaptive 40-120 ms jitter, gain/mute, reciprocal acknowledged
grants, live revocation, and preference/control migration. Native validation
also builds the complete MsQuic runtime and locked WinUI shell and runs the
source gate proving that the microphone backend does not use loopback capture.

Physical two-PC privacy, endpoint, loss/reorder, feedback, reconnect, and
bidirectional PTT qualification is still required before voice is described as
production-qualified. The exact matrix and latency goals are in
[`VOICE_FORWARDING.md`](VOICE_FORWARDING.md#validation-gates).

## Limitations of this validation

This validation does not yet prove:

- sustained high-poll-rate keyboard-hook/Raw Input timing across physical Windows 11 PCs
- sustained WASAPI timing and clock-drift behavior, or physical endpoint
  switch/disable/sleep recovery
- real packet-loss/jitter characteristics beyond the deterministic adaptive
  target and bounded rebuffer tests
- physical two-PC audio privacy, interruption, reconnect, and endpoint-change behavior
- physical two-PC text-clipboard privacy, contention, owner-exit, loop, and reconnect behavior
- physical two-PC microphone/PTT privacy, endpoint selection, packet-loss,
  feedback, and reconnect behavior

The Windows CI job additionally runs a native MsQuic 2.6.0 Schannel loopback:
two current-user CNG identities exchange bounded offers, confirm the same code,
reconnect with mutual pins, and transfer a reliable DeskLink packet. The test
deletes both temporary identities on success or failure.

Those are explicitly production integration stages rather than silently assumed complete features.
