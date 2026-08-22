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
to MsQuic `v2.6.0` at verified upstream commit
`e7e7a114e20a55ec2d5f723cf6bdf3bfb7b0b24a`.

```powershell
cmake -S . -B build-msquic `
  -DDESKLINK_BUILD_MSQUIC=ON `
  -DDESKLINK_MSQUIC_ROOT=C:\deps\msquic-2.6.0
cmake --build build-msquic --config Release --target desklink_msquic
```

`DESKLINK_MSQUIC_ROOT` may point to a source checkout or the official
`Microsoft.Native.Quic.MsQuic.Schannel` NuGet package. CMake builds the runtime
integrity and native pairing/session loopback tests, and stages the pinned DLL
under each target's `runtime/schannel` directory.

The Schannel package requires Windows 11 or Windows Server 2022 or newer for
QUIC/TLS 1.3. These versions are DeskLink's production baseline. Windows 10 may
be used only as the guarded target of the approved equal-security compatibility
R&D project; it is not yet a supported DeskLink transport target.

DeskLink loads MsQuic from the application-owned
`runtime/<provider>/msquic.dll` directory instead of relying on normal DLL
search order. The binary is checked against its build-pinned SHA-256 before
loading, then queried for MsQuic version and TLS provider. `auto` selects
Schannel on Windows 11/Server 2022-or-newer. The loader's OpenSSL selector is
retained for the explicitly staged compatibility R&D. Stage 2 research builds
also pin and verify `libcrypto-3-x64.dll` and `libssl-3-x64.dll`, then load the
explicit opaque CNG credential type. Production packages omit those binaries;
explicit OpenSSL requests therefore fail closed as unavailable.

On OpenSSL, MsQuic's portable-certificate flag supplies the bounded leaf DER to
the same DeskLink time and SHA-256 pin validator used by Schannel. The prototype
does not treat an OpenSSL `X509*` as a Windows certificate context and does not
admit a stream or session before deferred validation completes.

## Production platform decision

Stock Schannel plus the existing non-exportable CNG device identity is the only
production transport/security architecture on Windows 11/Server 2022+. The
approved Windows 10 R&D direction uses MsQuic 2.6.x, OpenSSL 3.5 LTS, and an
explicit opaque CNG provider that delegates signing without exporting the key.
It must prove fail-closed peer validation before `CONNECTED`, stream acceptance,
and session admission. Windows 10 remains unsupported until all gates pass. See
[`PLATFORM_SUPPORT.md`](PLATFORM_SUPPORT.md).

## Remaining physical integration

- two-PC LAN test on Windows 11/Server 2022-or-newer systems
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
