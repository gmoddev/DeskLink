# DeskLink 0.1.0 Beta 1

DeskLink Beta 1 is the first public beta of the product-shell experience for
secure keyboard and mouse roaming across trusted Windows PCs.

## Highlights

- Guided pairing with independently approved per-PC permissions.
- Authenticated edge roaming, reciprocal focus, monitor mapping, and display
  identification.
- Text clipboard sharing and per-peer audio routing with gain and mute.
- Persistent background broker, tray controls, reconnect, suspend/resume, and
  fail-local emergency return.
- Stock MsQuic/Schannel on Windows 11 and Server 2022 or newer.
- Experimental equal-security Windows 10 22H2 compatibility through the
  reviewed OpenSSL 3.5 provider and the same non-exportable CNG identity.

## Audio qualification

The Windows 10-to-Windows 11 voice run accepted and submitted 4,412 five-ms
blocks with no concealment or rejection. Median emit-to-WASAPI-submit latency
was 7.152 ms; p95 was 14.914 ms and p99 was 16.018 ms. These figures end at
software submission and exclude device, DAC, speaker, room, and microphone
delay.

## Important beta limitations

- This build and installer are unsigned; Windows SmartScreen may warn.
- Windows 10 transport is experimental and unsupported.
- Physical two-PC Windows 11-to-Windows 11 qualification is deferred because a
  second Windows 11 PC is not currently available.
- Hands-on accessibility, high-DPI, sleep/network interruption, and wider audio
  endpoint coverage remain ongoing beta work.
- Extended-display/video transport is not part of this release.

Use Ctrl+Alt+Pause to return input locally if necessary. Report issues at
https://github.com/gmoddev/DeskLink/issues.
