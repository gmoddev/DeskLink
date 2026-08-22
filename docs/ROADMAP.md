# DeskLink Roadmap

## Completed foundation

- portable protocol, capability, lease, epoch, and session core
- persistent non-exportable CNG identity and DPAPI trust store
- mutually pinned MsQuic/Schannel pairing and operational sessions
- integrity-checked runtime-selection foundation from PR #9 / `10147fe`
- opt-in Raw Input capture, fail-local suppression, and `SendInput` injection
- periodic reliable input-state reconciliation with owned-state convergence

## Current roadmap

Work proceeds in this order:

1. Validate the complete input path on two real Windows 11 or Windows Server
   2022-or-newer PCs, including cable removal, client termination, lease expiry,
   reconnect nonce rotation, stale epoch rejection, and Ctrl+Alt+Pause fail-local.
2. Implement stable multi-monitor enumeration, identity, rectangle mapping, and
   topology-change invalidation before edge roaming.
3. Implement bounded mouse-wheel transport without suppressing wheel input when
   it cannot be forwarded safely.

Edge roaming does not begin until snapshot reconciliation and the real two-PC
failure matrix pass.

## Later milestones

- monitor edge graph, crossing hysteresis, and roaming policy
- current-user named-pipe control API
- LAN discovery/mDNS
- foreground profile engine and GAME lifecycle
- WASAPI capture/render, adaptive jitter, and clock-drift correction
- UI, Stream Deck integration, installer, and update flow

## Experimental Windows 10 compatibility project

Windows 11/Windows Server 2022 production support remains stock
MsQuic/Schannel with the existing non-exportable CNG identity. Windows 10 stays
unsupported while an approved equal-security R&D path proceeds through
independent gates:

1. upgrade and validate the shared MsQuic foundation on stable 2.6.x
2. prototype the explicit opaque CNG/OpenSSL provider boundary
3. prove fail-closed validation and application admission
4. prove device-identity invariance and absence of private-key export paths
5. run guarded Windows 11 Schannel to Windows 10 OpenSSL physical validation

No later stage begins before the preceding stage passes. Full constraints are
in [`PLATFORM_SUPPORT.md`](PLATFORM_SUPPORT.md).
