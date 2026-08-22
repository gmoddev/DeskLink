# DeskLink Platform Support

## Production baseline

| Platform | Transport status | Notes |
|---|---|---|
| Windows 11 | Supported baseline | Stock MsQuic 2.5.8 Schannel runtime |
| Windows Server 2022 or newer | Supported baseline | Stock MsQuic 2.5.8 Schannel runtime |
| Windows 10, including 22H2 | Unsupported | No production QUIC/TLS 1.3 compatibility path |
| Linux/macOS | Core validation only | No production Windows input/transport backend |

Production transport uses stock MsQuic/Schannel and the existing persistent
current-user CNG device identity. The private key remains non-exportable. The
certificate DER, SHA-256 pin, key name, and export policy are identity state and
must not be silently replaced or modified.

Windows 10 is deliberately unsupported. DeskLink does not solve its lack of
Schannel QUIC/TLS 1.3 support by downgrading TLS, exporting/replacing the device
identity, disabling peer authentication, weakening certificate validation, or
falling back after a credential, certificate, authentication, or general
transport failure.

## Runtime-selection foundation

PR #9 / commit `10147fe` remains the completed runtime-selection foundation.
It loads MsQuic only from an application-owned provider directory, verifies the
build-pinned SHA-256 and exact provider/version metadata, and defaults to
`auto`. Production packages contain only the Schannel runtime.

The `openssl` selector remains available for diagnostics and future explicitly
approved research. It is not a support promise. With no approved OpenSSL
runtime, selection fails closed before networking starts.

## Preserved Windows 10 R&D design

The Windows 10 investigation is preserved but removed from the current roadmap.
Stock MsQuic's OpenSSL credential path requires exportable private-key material,
which is incompatible with DeskLink's identity policy.

If Windows 10 support is reconsidered, the starting design is:

```text
DeskLink explicit CNG credential
    -> narrowly maintained MsQuic 2.6.x integration
    -> built-in OpenSSL 3.5 LTS provider
    -> opaque CNG key handle
    -> NCryptSignHash for TLS 1.3 signing
```

This is a new security-sensitive project, not a continuation of PR #9. It
requires separate approval before implementation. A standalone provider loaded
through environment or generic module search is not acceptable, and deprecated
OpenSSL ENGINE/RSA method hooks are not the intended design.

## Mandatory future acceptance criteria

Any future OpenSSL implementation must prove all of the following:

1. `NCRYPT_EXPORT_POLICY_PROPERTY` remains non-exportable before, during, and
   after every credential load and handshake.
2. No private parameter, PFX, PEM, or private-key blob is produced or written.
3. The existing key name, certificate DER, and SHA-256 pin remain unchanged.
4. OpenSSL receives only an opaque provider key reference and public parameters;
   every private-key export request fails.
5. TLS remains TLS 1.3 with no cipher/security-level downgrade.
6. Wrong pin, missing certificate, malformed DER, expired certificate,
   validator exception, timeout, and signing failure terminate the connection.
7. None of those failures may emit an accepted `CONNECTED` transition, accept a
   peer stream, or admit a DeskLink session.
8. 0-RTT and privileged resumption remain disabled unless separately designed
   and approved.
9. Provider or credential failures never trigger automatic fallback.
10. Mixed-provider interoperability and the complete physical failure matrix
    pass on supported test systems before production consideration.

Until those criteria are met under separate approval, the production support
statement remains Windows 11/Server 2022 or newer with stock Schannel.

The upstream source and platform lifecycle material supporting this decision is
listed in [`REFERENCES.md`](REFERENCES.md).
