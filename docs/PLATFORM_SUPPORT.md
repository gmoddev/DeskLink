# DeskLink Platform Support

## Production baseline

| Platform | Transport status | Notes |
|---|---|---|
| Windows 11 | Supported baseline | Stock MsQuic 2.6.0 Schannel runtime |
| Windows Server 2022 or newer | Supported baseline | Stock MsQuic 2.6.0 Schannel runtime |
| Windows 10, including 22H2 | Experimental/unsupported | Equal-security OpenSSL/CNG R&D; no production admission yet |
| Linux/macOS | Core validation only | No production Windows input/transport backend |

Production transport uses stock MsQuic/Schannel and the existing persistent
current-user CNG device identity. The private key remains non-exportable. The
certificate DER, SHA-256 pin, key name, and export policy are identity state and
must not be silently replaced or modified.

Windows 10 remains deliberately unsupported until the approved equal-security
R&D project passes every security and physical gate. DeskLink does not solve
its lack of Schannel QUIC/TLS 1.3 support by downgrading TLS,
exporting/replacing the device identity, disabling peer authentication,
weakening certificate validation, or falling back after a credential,
certificate, authentication, or general transport failure.

## Runtime-selection foundation

PR #9 / commit `10147fe` remains the completed runtime-selection foundation.
It loads MsQuic only from an application-owned provider directory, verifies the
build-pinned SHA-256 and exact provider/version metadata, and defaults to
`auto`. Production packages contain only the Schannel runtime.

Stage 1 pins stable MsQuic `v2.6.0` at verified upstream commit
`e7e7a114e20a55ec2d5f723cf6bdf3bfb7b0b24a`. The stable Microsoft Schannel and
OpenSSL NuGet artifacts are available under the same version. This release
provides the explicit external OpenSSL build path with a minimum of OpenSSL
3.5.0, while retaining the Schannel provider used by production systems. The
DeskLink Stage 1 package and tests still stage Schannel only; no CNG/OpenSSL
credential code is introduced by the foundation upgrade.

The `openssl` selector remains available only for the approved, separately
gated research. It is not a support promise. With no admitted OpenSSL runtime,
selection fails closed before networking starts.

## Active Windows 10 R&D design

The Windows 10 project is approved only through separately reviewed stages.
Stock MsQuic's ordinary OpenSSL credential path requires private-key material
in an OpenSSL-compatible form, which is incompatible with DeskLink's identity
policy.

The required design is:

```text
DeskLink explicit CNG credential
    -> narrowly maintained MsQuic 2.6.x integration
    -> built-in OpenSSL 3.5 LTS provider
    -> opaque CNG key handle
    -> NCryptSignHash for TLS 1.3 signing
```

This is a new security-sensitive project, not a continuation of PR #9. Each
stage requires a clean review before the next begins. A standalone provider
loaded through environment or generic module search is not acceptable, and
deprecated OpenSSL ENGINE/RSA method hooks are prohibited.

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

Until those criteria and the physical matrix pass, the production support
statement remains Windows 11/Server 2022 or newer with stock Schannel and
Windows 10 remains experimental/unsupported.

The upstream source and platform lifecycle material supporting this decision is
listed in [`REFERENCES.md`](REFERENCES.md).
