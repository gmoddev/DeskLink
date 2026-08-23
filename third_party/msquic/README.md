# DeskLink Windows 10 MsQuic research patch

This directory contains the reviewable downstream patch for DeskLink's
experimental Windows 10 transport. It is not part of the supported production
transport and it is not used by the default build.

## Pinned foundation

- MsQuic `v2.6.0`, commit
  `e7e7a114e20a55ec2d5f723cf6bdf3bfb7b0b24a`
- OpenSSL `3.5.7` LTS, source commit
  `8cf17aaeb4599f8af87fefd810b5b5fee90fe69e`
- patch: `patches/0001-Add-explicit-opaque-CNG-OpenSSL-credential-path.patch`

The patch must apply cleanly to that exact MsQuic commit. Generated binaries
are never committed.

## Security boundary

The patch adds one explicit `QUIC_CREDENTIAL_TYPE_DESKLINK_CNG` path. Ordinary
OpenSSL credentials retain their upstream behavior and cannot silently select
the DeskLink provider.

The built-in provider implements only OpenSSL 3 KEYMGMT and RSA-PSS SIGNATURE
operations required by TLS 1.3. It stores public RSA parameters plus the CNG
provider/key names needed to reopen an opaque key handle. It rejects any key
whose `NCRYPT_EXPORT_POLICY_PROPERTY` is nonzero, verifies signing usage and
RSA algorithm, and proves the certificate public key matches the CNG key by a
challenge sign/verify before installing the credential. TLS signing delegates
to `NCryptSignHash`; private parameters are neither requested nor exported.

The private `OSSL_LIB_CTX`, built-in default provider, and built-in DeskLink CNG
provider are initialized exactly once and retained for the process lifetime.
MsQuic worker threads retain OpenSSL per-thread DRBG state beyond individual
TLS configurations, so freeing a per-configuration library context is unsafe.
The DeskLink loader correspondingly retains the already hash-verified OpenSSL
MsQuic and OpenSSL modules until process teardown. No provider is discovered
through `PATH`, `OPENSSL_MODULES`, configuration files, or module search paths.

The patch does not use OpenSSL ENGINE, `RSA_METHOD`, private-key export APIs, a
PFX, or an OpenSSL private-key representation. It does not add fallback.

## Reproduction

Build OpenSSL 3.5.7 for `VC-WIN64A`, install it to a private directory, and
check out MsQuic with its pinned submodules. From a Visual Studio x64 developer
PowerShell run:

```powershell
.\scripts\Build-Windows10Compat.ps1 `
    -MsQuicSource C:\src\msquic `
    -OpenSslRoot C:\deps\openssl-3.5.7 `
    -SchannelRoot C:\deps\Microsoft.Native.Quic.MsQuic.Schannel
```

The script verifies the pins, applies or verifies the patch, rejects prohibited
private-key APIs/mechanisms, builds both runtimes, hashes every packaged DLL,
and runs the Schannel and OpenSSL/CNG DeskLink suites. The OpenSSL suite includes
positive pairing/reconnect/session-nonce coverage plus exportable-key,
certificate/key mismatch, and runtime-component tamper rejection.

Windows 10 remains experimental/unsupported. Fail-closed peer-validation,
identity-invariance, and physical two-PC gates remain separately reviewed
stages. Stage 3 now supplies the fail-closed validation/admission state machine
and negative matrix; identity invariance and physical validation still remain.
