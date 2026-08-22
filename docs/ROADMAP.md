# DeskLink Roadmap

## Completed foundation

- portable protocol, capability, lease, epoch, and session core
- persistent non-exportable CNG identity and DPAPI trust store
- mutually pinned MsQuic/Schannel pairing and operational sessions
- integrity-checked runtime-selection foundation from PR #9 / `10147fe`
- opt-in Raw Input capture, fail-local suppression, and `SendInput` injection

## Current roadmap

Work proceeds in this order:

1. Implement periodic input-state snapshot reconciliation so lost or interrupted
   key/button transitions converge to the authoritative Host state.
2. Validate the complete input path on two real Windows 11 or Windows Server
   2022-or-newer PCs, including cable removal, client termination, lease expiry,
   reconnect nonce rotation, stale epoch rejection, and Ctrl+Alt+Pause fail-local.
3. Implement stable multi-monitor enumeration, identity, rectangle mapping, and
   topology-change invalidation before edge roaming.
4. Implement bounded mouse-wheel transport without suppressing wheel input when
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

## Closed for the current roadmap

Windows 10 transport compatibility is closed. Production support begins with
Windows 11/Windows Server 2022 and uses stock MsQuic/Schannel with the existing
non-exportable CNG identity. No MsQuic/OpenSSL fork or CNG provider will be
implemented in the current roadmap.

The investigation is preserved in [`PLATFORM_SUPPORT.md`](PLATFORM_SUPPORT.md).
Reconsidering it requires a new, separately approved security project based on
a maintained MsQuic/OpenSSL baseline and all documented fail-closed gates.
