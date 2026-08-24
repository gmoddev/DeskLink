# DeskLink Roadmap

## Completed foundation

- portable protocol, capability, lease, epoch, and session core
- persistent non-exportable CNG identity and DPAPI trust store
- mutually pinned MsQuic/Schannel pairing and operational sessions
- integrity-checked runtime-selection foundation from PR #9 / `10147fe`
- opt-in low-level keyboard/Raw Input mouse capture, fail-local suppression,
  and `SendInput` injection
- periodic reliable input-state reconciliation with owned-state convergence

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
5. **Blocked on item 1:** monitor edge graph, crossing hysteresis, and roaming
   policy.
6. **Complete:** bounded LAN DNS-SD/mDNS discovery. `listen` and `serve`
   advertise `_desklink._udp.local`; `discover [1..30 seconds]` reports strict,
   expiring, deterministic candidates without connecting, pairing, granting a
   capability, or writing trust. Conflicting duplicate identities are marked
   ambiguous.
7. **Complete foundation:** bounded foreground profile evaluation and native
   Windows foreground observation. Rules match exact executable basenames with
   an optional fullscreen requirement; precedence is emergency, manual,
   profile, then default. A configured but uninspectable foreground produces a
   fail-local decision. The WinEvent source owns its message-loop thread and
   unhooks on that same thread.
8. **Complete safety boundary:** the portable Host lifecycle disables routing,
   releases remote focus, synchronously tears down capture hooks, and then
   enters `GAME`/`LOCK_PC1`. Leaving a restricted mode requests a fresh focus
   transaction; capture cannot restart or enable until `FocusReady` and the
   initial state snapshot succeed. Win32 capture is restart-safe.
9. **Complete:** foreground/profile decisions and bounded profile configuration
   are wired into the production Host CLI through one serialized lifecycle
   owner. Exact `exe=mode` rules, fullscreen-only rules, current-user manual
   overrides, `FocusReady`, renewal, and capture failures share the same ordered
   event boundary. Renewal and reconciliation run only while Remote.
10. **Complete foundation:** event-driven shared-mode WASAPI loopback capture
    and render adapters normalize to exact 48 kHz/stereo/PCM16/5 ms blocks.
    Capture packet handling, renderer submission, and renderer buffering are
    bounded; silence, discontinuity, underrun, and module-local failure paths
    are explicit.
11. **Complete:** capability-gated audio session/datagram wiring and receiver
    jitter/render pumping. Sender and receiver require complementary local trust
    grants plus explicit runtime opt-in after `PeerValidated`; nonce, format,
    stream, sequence, and bounded queue checks precede playout.
12. **Complete:** audio endpoint-loss/recovery with audio-only restart
    semantics. Scoped endpoint notifications stop stale WASAPI workers;
    restart-safe adapters reopen the current default endpoint with capped
    250 ms to 5 s backoff while the admitted session and input remain intact.
    Client/send rejection never triggers reopen.
13. **Complete:** bounded adaptive jitter targeting. The receiver compares
    capture and steady arrival deltas, raises the 2-12 block target immediately
    on spikes/concealment, enters an explicit rebuffer state when latency must
    grow, and requires 200 stable samples for each one-block decrease.
14. **Next unblocked slice:** bounded clock-drift correction while edge roaming
    remains gated on the deferred Windows 11 physical matrix.

Edge roaming does not begin until snapshot reconciliation and the real two-PC
failure matrix pass.

## Later milestones

- monitor edge graph, crossing hysteresis, and roaming policy
- clock-drift correction and per-peer gain/mute
- UI, Stream Deck integration, installer, and update flow

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

Completion of the R&D gates does not by itself make the prototype a production
artifact. Windows 10 stays experimental/unsupported until a separately reviewed
production-admission and release-integration change is approved.

No later stage begins before the preceding stage passes. Full constraints are
in [`PLATFORM_SUPPORT.md`](PLATFORM_SUPPORT.md).
