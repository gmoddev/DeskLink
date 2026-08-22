# DeskLink Implementation References

These references informed the production-backend recommendations. They are not dependencies of the portable foundation.

- Microsoft MsQuic repository and build documentation: https://github.com/microsoft/msquic
- MsQuic QUIC settings / DatagramReceiveEnabled: https://github.com/microsoft/msquic/blob/main/docs/api/QUIC_SETTINGS.md
- Microsoft Learn — Schannel TLS protocol support by Windows version: https://learn.microsoft.com/windows/win32/secauthn/protocols-in-tls-ssl--schannel-ssp-
- MsQuic v2.5.8 OpenSSL credential loading: https://github.com/microsoft/msquic/blob/bf10e4a60dd03c471343623eccd35b4ea671937f/src/platform/tls_quictls.c
- MsQuic v2.5.8 CNG private-key export requirement: https://github.com/microsoft/msquic/blob/bf10e4a60dd03c471343623eccd35b4ea671937f/src/platform/certificates_capi.c
- MsQuic v2.5.8 certificate-validation gate: https://github.com/microsoft/msquic/blob/bf10e4a60dd03c471343623eccd35b4ea671937f/src/core/crypto.c
- MsQuic credential flags and provider limitations: https://microsoft.github.io/msquic/msquicdocs/docs/api/QUIC_CREDENTIAL_CONFIG.html
- MsQuic v2.6.0 external OpenSSL 3.5 build support: https://github.com/microsoft/msquic/blob/v2.6.0/CMakeLists.txt
- OpenSSL provider key management: https://docs.openssl.org/3.1/man7/provider-keymgmt/
- OpenSSL provider signature operations: https://docs.openssl.org/3.1/man7/provider-signature/
- OpenSSL release lifecycle: https://openssl-library.org/roadmap/
- Microsoft Learn — SendInput: https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-sendinput
- Microsoft Learn — Raw Input: https://learn.microsoft.com/windows/win32/inputdev/raw-input
- Microsoft Learn — WM_INPUT: https://learn.microsoft.com/windows/win32/inputdev/wm-input
- Microsoft Learn — LowLevelKeyboardProc: https://learn.microsoft.com/windows/win32/winmsg/lowlevelkeyboardproc
- Microsoft Learn — LowLevelMouseProc: https://learn.microsoft.com/windows/win32/winmsg/lowlevelmouseproc
- Microsoft Learn — WASAPI loopback recording: https://learn.microsoft.com/windows/win32/coreaudio/loopback-recording
- Microsoft Learn — Audio sessions: https://learn.microsoft.com/windows/win32/coreaudio/audio-sessions
- Microsoft Learn — Named pipe security and access rights: https://learn.microsoft.com/windows/win32/ipc/named-pipe-security-and-access-rights
- Microsoft Learn — CryptProtectData / DPAPI: https://learn.microsoft.com/windows/win32/api/dpapi/nf-dpapi-cryptprotectdata
