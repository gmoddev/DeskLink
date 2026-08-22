# MsQuic and Pairing Integration

## Implemented boundary

DeskLink now has three separate security layers for production transport work:

1. `PairingCoordinator` opens a bounded manual pairing window, generates a fresh
   nonce, and derives a six-digit verification code from both machine IDs,
   display names, certificate SHA-256 pins, and nonces.
2. `ITrustStore` persists the confirmed peer identity and capability grant. On
   Windows, `DpapiTrustStore` protects the complete bounded trust database with
   current-user DPAPI and replaces it atomically.
3. `HostSession` and `AgentSession` require the authenticated transport identity
   to match the stored machine ID and certificate pin before accepting traffic.

Authentication, encryption, and pairing are intentionally separate checks. A
valid TLS connection from an unknown certificate is not a DeskLink session.

## Pairing flow

```text
PC1                                              PC2
 |                                                |
 | BeginPairing(60 s)                             | BeginPairing(60 s)
 | CreateOffer(machine, cert pin, nonce)          | CreateOffer(...)
 |---------------- unauthenticated exchange ----->|
 |<--------------- unauthenticated exchange ------|
 |                                                |
 | display six-digit transcript code              | display same code
 | user confirms physical match                   | user confirms physical match
 | ConfirmOffer() -> persist PC2 pin               | ConfirmOffer() -> persist PC1 pin
```

The exchange can be observed or modified by the LAN. Modification produces a
different verification code on each PC. The UI must require the user to compare
and confirm both codes; it must not confirm automatically.

Pairing confirmation closes the local window and clears its nonce. A window may
be open for at most five minutes.

## Certificate pin definition

The pin is lowercase hexadecimal SHA-256 over the exact DER-encoded leaf
certificate bytes presented by MsQuic. It is not a hash of a display name or a
user-entered identifier.

`VerifyMsQuicPeerCertificate()` must run while the certificate supplied in
`QUIC_CONNECTION_EVENT_PEER_CERTIFICATE_RECEIVED` remains valid. It returns the
stored identity only when the DER hash matches a trusted record. The returned
`TransportPeerInfo` can then be passed to `MsQuicTransportEndpoint::Adopt()`.

For self-signed device certificates, the MsQuic configuration must request the
certificate-received callback and defer built-in validation so DeskLink can
complete validation from its pin. Servers must require client authentication.
Privileged messages must not use 0-RTT.

`MsQuicBootstrap` copies the bounded DER leaf during the callback, returns
`QUIC_STATUS_PENDING`, validates on a separate worker, and completes the
handshake with `ConnectionCertificateValidationComplete()`. Trusted sessions
are promoted to `MsQuicTransportEndpoint` only after the stored pin matches.

## Bootstrap and pairing lanes

The bootstrap owns the MsQuic API table, registration, four client/server
configurations, listener, and outgoing connections. It uses separate ALPNs:

```text
desklink/pair/1     one provisional, bounded PairingOffer exchange
desklink/session/1  mutually pinned operational DeskLink session
```

The pairing lane is available only while the local manual window is open. It
accepts one bidirectional stream, one frame no larger than 153 bytes, valid
UTF-8 display names, and no 0-RTT. The offer's certificate pin must match the
exact TLS leaf presented on that connection before the UI callback receives
the six-digit candidate. Dropping or rejecting the callback closes the
connection. Confirmation persists trust and also closes the provisional
connection; the peer must reconnect on the operational ALPN.

Inbound connection attempts are limited per source address. Pairing has a
separate stricter limiter, and both limiter key tables are bounded.

## Adapter behavior

The optional `desklink_msquic` target maps:

```text
reliable DeskLink packets -> one bidirectional QUIC stream
pointer/audio packets     -> QUIC DATAGRAM
```

The adapter:

- retains send buffers until MsQuic reports completion
- reconstructs DeskLink packets across arbitrary stream chunk boundaries
- enforces reliable and datagram size ceilings before dispatch
- rejects 0-RTT stream and datagram data
- allows only one DeskLink reliable stream per connection
- keeps callback state alive until `QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE`
- exposes only the stored identity returned by certificate-pin verification

## Build

MsQuic is an optional Windows target. The repository pins compatibility testing
to MsQuic `v2.5.8`.

```powershell
cmake -S . -B build-msquic `
  -DDESKLINK_BUILD_MSQUIC=ON `
  -DDESKLINK_MSQUIC_ROOT=C:\deps\msquic-2.5.8
cmake --build build-msquic --config Release --target desklink_msquic
```

`DESKLINK_MSQUIC_ROOT` may point to a source checkout or the official
`Microsoft.Native.Quic.MsQuic.Schannel` NuGet package. If a prebuilt library is
present, CMake also builds the native pairing/session loopback test and copies
`msquic.dll` beside it.

The Schannel package requires Windows 11 or Windows Server 2022 or newer for
QUIC/TLS 1.3. Windows 10 remains useful as a cross-build worker but cannot run
the Schannel loopback.

## Remaining physical integration

- two-PC LAN test on supported Windows versions
- firewall-scoped listener deployment for Private/Domain profiles
- cable removal while input is held, lease cleanup, and reconnect
- automatic lease renewal and fresh session-nonce verification after reconnect

The automated native loopback proves device-certificate reload, provisional
pairing, user-code confirmation, mutual pin validation, reconnect, and a real
reliable DeskLink packet over MsQuic. It does not substitute for physical-link
failure injection.

## Trusted session preface

After mutual certificate-pin validation under `desklink/session/1`, the
initiator opens the single reliable stream and sends a bounded 16-byte `DLSN`
version-1 preface containing a cryptographically random, nonzero 64-bit session
nonce. The acceptor validates the preface before the transport endpoint is
delivered. Both endpoint callbacks receive the same nonce and whether they
initiated the connection, so `HostSession` and `AgentSession` can reject packets
from previous connections. The preface and reliable stream reject 0-RTT data;
early post-preface packets remain bounded and buffered until a receive handler
is installed. Every reconnect negotiates a different nonce.

## Manual trusted focus

Once both PCs have persisted trust and the receiving PC granted `InputInject`,
run `desklink_pair.exe serve 43821` on that PC and
`desklink_pair.exe focus <receiver-ip> 43821` on the other. The focus command
requests a 750 ms lease, waits for `FocusReady`, renews every 500 ms, and sends
an explicit release when Enter is pressed. The serving side ticks lease expiry
every 50 ms. This control-plane proof does not capture or suppress local input.

## Windows pairing control

The optional `desklink_pair` executable is the first current-user control
surface. It loads or creates the persistent CNG device identity, opens the
DPAPI-protected trust store in `%LOCALAPPDATA%\DeskLink`, and runs one explicit
five-minute pairing operation over `desklink/pair/1`.

```powershell
# PC that will accept the connection and grant remote input injection
desklink_pair.exe listen 43821 --grant-input

# Other PC; no capability is granted locally unless the flag is also supplied
desklink_pair.exe pair 192.168.1.25 43821
```

Each side independently displays the remote display name, the transcript-derived
six-digit code, and the exact input capability consequence. The default button
is No. Confirmation never occurs automatically, and dismissing either prompt
rejects the provisional connection. The tool does not create a firewall rule or
listen beyond the bounded pairing operation.
