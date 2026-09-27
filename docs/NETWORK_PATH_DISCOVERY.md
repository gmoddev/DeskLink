# Network path discovery and selection

A device has one authoritative paired identity: stored machine ID and
certificate pin. Its addresses, local adapter GUID/index/name, link speed,
route availability, and previous connection health are routing metadata.
Neither a fast adapter nor a matching discovery ID grants trust or capability.
Ethernet metadata does not identify fiber or prove a direct physical cable.

The bounded Windows mDNS browse remains `_desklink._udp.local`. Each discovered
service is resolved independently on each active non-loopback interface (up to
eight interfaces with usable IP addresses and 64 service names). The native
`DnsStartMulticastQuery` API queries service SRV/TXT records, then scoped A and
AAAA records under separate five-second deadlines. Queries stop before their
storage is released; expired callbacks cannot enter a later query phase.
Records accumulate across responses under fixed record/callback limits and
retain their encoding when converted to Unicode. IPv4 and IPv6 are retained;
IPv6 link-local addresses carry the local interface scope. Local adapter and
route information comes from Windows, separately from the untrusted DNS data.
A scoped route check establishes eligibility, not remote reachability or trust.
The authenticated QUIC attempt is the actual connection check.
Advertisements use the PC's actual DNS hostname with `.local` and explicitly
publish the adapter's addresses. A synthetic hostname without corresponding
address records can advertise a service that clients cannot connect to.

The cache retains all observed address/interface candidates and presents one
device per machine ID. Conflicting identity/service metadata makes the whole
device ambiguous. Observations expire and interface removal removes its paths.

Selection rejects ambiguous, invalid, inactive, addressless, and cooling-down
candidates. It ranks eligible candidates by an optional explicit adapter GUID,
previously authenticated success, usable local link speed, and deterministic
endpoint order. An ordinary unavailable path cools down for 30 seconds. If no
candidate is eligible, the broker waits for its bounded availability retry;
it never escapes to an unbound hostname connection for a discovered device.
The existing manually saved address hint remains available only when discovery
does not return that identity; certificate pin and machine ID still apply.
The broker currently uses automatic preference; the selector's adapter-GUID
override is available for a future product settings surface.

The supervisor retains the selected path internally, passing its literal
remote address and local interface index through the structured launcher to
MsQuic. Before ConnectionStart, MsQuic sets LOCAL_INTERFACE and REMOTE_ADDRESS.
Native CLI callers can use `--local-interface <index>` with `focus` or `pair`
and a literal address. The shared Nearby control schema stays unchanged; it
provides a device summary while the broker owns the candidates and selection.
Nearby pairing uses the selected path but still requires the existing bounded
manual ceremony, role-bound transcript, matching code, and local confirmation.

QUIC migration is explicitly disabled for DeskLink session and pairing
configurations. Selected-path loss closes the transport and performs existing
fail-local cleanup. Only ordinary network/availability failures permit another
candidate attempt. Identity, certificate, credential, capability, protocol,
and unknown failures remain Action Required. Every reconnect authenticates
the pinned peer again, negotiates a fresh nonce, and begins ConnectedLocal;
focus and suppression are not restored by path selection.

Diagnostics use `[Broker:Network]` and include selected local adapter, interface,
address, and link speed. No dialog, focus change, firewall change, or adapter
configuration is introduced. Product connection-details UI is a separate
presentation extension; this implementation does not modify the monitor UI.

Validation: `desklink_network_path_tests` checks multi-path retention, both
address families, expiry/removal, ranking, explicit preference, cooldown,
all-unavailable behavior, ambiguity, terminal identity failure, and launcher
identity/interface propagation. Its optional `--advertise` and `--browse`
modes exercise native mDNS without creating credentials or granting access.
`--verify-paths` additionally requires one unambiguous peer with two available
interfaces, verifies fast-path selection, and cools down the selected adapter's
addresses to verify alternate-interface selection.
The MsQuic loopback suite binds pairing and the first session to IPv4 loopback,
then reconnects on IPv6 loopback and checks that the session nonce changes.

## Verified build and PC discovery

The Release build uses the remote worker `dockerbox`, workspace
`C:\Sandbox\Codex\Workspaces\desklink-network-paths`, and incremental build
directory `C:\Sandbox\Codex\Builds\desklink-network-paths`. MSVC uses two
project jobs with `/MP2` (up to four compiler jobs), retaining the Opus 1.6.1
dependency build and existing pinned MsQuic runtime distributions. Executables
are in the build directory's `Release` folder; CTest records its log under
`Testing\Temporary\LastTest.log`. The product wrapper, broker, core tests,
path tests, and both transport test targets build successfully.

All 17 selected CTest checks passed, including Schannel and OpenSSL/CNG
interface-bound pairing, IPv4 session establishment, and IPv6 reconnect with
a fresh nonce. The unrelated reliability soak target was not included.

The native cross-PC probe discovered one remote probe identity with IPv4 and
scoped IPv6 candidates on local Ethernet 3 (10 Gbps) and Ethernet (2.5 Gbps).
It selected `10.253.3.2`, then selected `192.168.0.108` after the preferred
adapter's candidates were put on cooldown. This verifies discovery and
selection on the real NICs. Physical cable removal during an active game was
not performed; fail-local cleanup and authenticated reconnect are covered by
the existing lifecycle tests and the bound transport reconnect tests.

Probe advertisements expire after 30 seconds and leave no persistent process,
credential, pairing grant, firewall rule, or adapter setting behind. Product
binaries have been built for review; installed DeskLink processes are unchanged.
