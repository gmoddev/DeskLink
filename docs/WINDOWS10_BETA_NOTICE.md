# DeskLink 0.1.0 Beta 1

This package is an unsigned beta build. Windows SmartScreen may warn because
the DeskLink binaries and installer are not Authenticode-signed.

Windows 11 and Windows Server 2022 or newer are the supported beta baseline and
use stock MsQuic with Schannel. Windows 10 22H2 remains experimental and
unsupported; it uses the reviewed MsQuic/OpenSSL 3.5 compatibility runtime with
the existing opaque, non-exportable CNG device key.

The package preserves DeskLink's normal security model:

- Provider selection occurs once from the operating-system version.
- Credential, signing, certificate, authentication, handshake, or transport
  failure never retries with another provider.
- Pairing, DER SHA-256 pinning, trust records, nonces, epochs, leases, and
  application admission remain provider-independent and fail closed.
- The CNG private key remains non-exportable and is never serialized or placed
  in a PFX.

The Windows 10 compatibility path passed its guarded mixed-provider physical
matrix, identity-invariance checks, and authenticated audio timing run. It is
still excluded from the supported baseline until production admission is
separately approved. A second physical Windows 11 PC, hands-on accessibility,
and broader sleep/network fault coverage also remain beta qualification work.

The installer is current-user only, does not elevate, and does not create or
modify Windows Firewall rules. Confirm any Windows network prompt manually and
allow only the intended Private network.
