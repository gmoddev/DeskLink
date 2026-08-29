# DeskLink Windows 10 Development Alpha

This unsigned package is an experimental compatibility build, not a production
release. Windows SmartScreen may warn because the binaries and installer are
not Authenticode-signed.

The package preserves DeskLink's normal security model:

- Windows 11 and Windows Server 2022+ select the stock MsQuic Schannel runtime.
- Windows 10 selects the patched MsQuic/OpenSSL 3.5 runtime with the existing
  opaque, non-exportable CNG device key.
- Provider selection is based on the operating-system version before runtime
  loading. Credential, signing, certificate, authentication, or handshake
  failure never retries with another provider.
- Pairing, DER SHA-256 pinning, trust records, nonces, epochs, leases, and
  application admission remain provider-independent and fail closed.

Windows 10 remains experimental and unsupported until the physical sleep,
network-loss, held-input termination, accessibility, and remaining two-PC
qualification gates pass. Do not deploy this build as unattended production
infrastructure.

The installer is current-user only, does not elevate, and does not create or
modify Windows Firewall rules. Confirm any Windows network prompt manually and
allow only the intended Private network.
