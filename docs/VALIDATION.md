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

---

## Limitations of this validation

This validation does not yet prove:

- sustained high-poll-rate keyboard-hook/Raw Input timing across physical Windows 11 PCs
- sustained WASAPI timing and clock-drift behavior, or physical endpoint
  switch/disable/sleep recovery
- real packet-loss/jitter characteristics beyond the deterministic adaptive
  target and bounded rebuffer tests
- physical two-PC audio privacy, interruption, reconnect, and endpoint-change behavior

The Windows CI job additionally runs a native MsQuic 2.6.0 Schannel loopback:
two current-user CNG identities exchange bounded offers, confirm the same code,
reconnect with mutual pins, and transfer a reliable DeskLink packet. The test
deletes both temporary identities on success or failure.

Those are explicitly production integration stages rather than silently assumed complete features.
