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

`DESKLINK_MSQUIC_ROOT` must contain either `include/msquic.h` or
`src/inc/msquic.h`. If a prebuilt `msquic` library is present under `lib` or
`bin`, CMake links it; otherwise the static adapter target still compiles for an
application that supplies the MsQuic API table separately.

## Remaining connection bootstrap

This slice deliberately does not yet provide the executable listener/client
bootstrap. The next transport change must add:

- device-certificate creation and private-key storage through Windows CNG
- MsQuic registration/configuration ownership
- server listener and client connection factories
- the bounded wire exchange for `PairingOffer`
- deferred certificate-validation completion in the connection callback
- connection-attempt and pairing-attempt rate limits
- a two-PC LAN integration test including cable removal and reconnect

Until those are implemented, the adapter consumes an already-established
MsQuic connection and optional reliable stream; it does not independently open
or listen for connections.
