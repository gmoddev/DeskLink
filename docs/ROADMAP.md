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
9. **Next:** wire foreground/profile decisions and bounded profile
   configuration into the production Host CLI through that lifecycle.

Edge roaming does not begin until snapshot reconciliation and the real two-PC
failure matrix pass.

## Later milestones

- monitor edge graph, crossing hysteresis, and roaming policy
- production profile configuration and live runtime event wiring
- WASAPI capture/render, adaptive jitter, and clock-drift correction
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
