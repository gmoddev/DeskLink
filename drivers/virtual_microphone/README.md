# DeskLink virtual microphone

This optional component turns DeskLink's already authenticated and decoded
48 kHz mono PCM16 voice receive stream into a genuine Windows capture endpoint.
It is not a second network or trust boundary. The existing `VoiceFrame` session,
permission, nonce, epoch, jitter, FEC, PLC, and Opus decode path remains
authoritative.

## Endpoint and bridge design

The driver is a narrow downstream overlay on Microsoft's Simple Audio Sample,
which is the smaller WaveRT/SYSVAD-derived sample. The upstream source is pinned
to commit `197ba2156a60e2b76fcd4820bae594223e91a1e9`; every patched upstream file is
also checked by SHA-256 before a build.

The package exposes exactly two endpoints:

- `DeskLink Microphone Feed`, a render endpoint used only by the DeskLink
  runtime;
- `DeskLink Remote Microphone`, a capture endpoint applications such as Discord
  can select.

Windows has no supported WaveRT mechanism that makes an ordinary render
endpoint available exclusively to one desktop process while retaining the
normal audio-engine data path. The feed endpoint therefore remains visible and
is named unmistakably. DeskLink never makes it a default device.

Both endpoints carry the stable property
`{D21F0A7C-80DA-4E7E-A906-81DF3E2EA4B9}, 2`: value `1` is the feed and value `2`
is the remote microphone. Runtime lookup is by this property only. Friendly
names, default-device selection, and arbitrary endpoint fallback are forbidden.
The capture endpoint is rejected as a DeskLink outgoing microphone source by
the same stable identity, preventing a receive-to-send loop even if a user
makes it the Windows communications default.

The kernel bridge is preallocated nonpaged storage with no IOCTL surface, file
I/O, network access, codec, trust logic, or allocation in its data callbacks.
The inherited sample tone generator and render-to-disk implementation are not
compiled or referenced. Protected render content flushes the bridge and the
capture endpoint emits silence until the protection signal clears.
It accepts only 48 kHz mono PCM16. Its hard capacity is 60 ms, it trims normal
buffering to at most 40 ms by dropping the oldest samples, and it writes silence
on underflow. Entering or leaving RUN on either pin securely clears the ring.
The capture pin also starts from an empty generation, so opening an application
can never reveal audio buffered before that application opened the endpoint.

The user-mode runtime owns another bounded 60 ms oldest-drop queue before the
WASAPI feed. Network/session replacement, permission loss, receive mute, route
disable, endpoint loss, process shutdown, or runtime crash stops the feed pin;
the driver transition clears stale PCM and the capture endpoint remains valid
but silent.

## Build isolation

Normal DeskLink CMake builds never require the WDK. The driver target exists
only when all of these are supplied explicitly:

```powershell
cmake -S . -B build-driver `
  -DDESKLINK_BUILD_VIRTUAL_MICROPHONE_DRIVER=ON `
  -DDESKLINK_WINDOWS_DRIVER_SAMPLES_ROOT=C:\pinned\windows-driver-samples `
  -DDESKLINK_VIRTUAL_MICROPHONE_WDK_ROOT=C:\packages\Microsoft.Windows.WDK.x64.10.0.26100.6584
cmake --build build-driver --target desklink_virtual_microphone_driver --config Release
```

The build emits an unsigned development package and a hash manifest. It never
creates or trusts a test certificate and never changes boot policy.

## Mandatory signing and installation boundary

Production distribution requires a Microsoft-signed x64 catalog obtained
through the Windows Hardware Dev Center/HLK or another currently supported
Microsoft production signing path. The production verifier requires kernel
policy verification of the catalog and its SYS membership before the package
can enter an installer or release.

DeskLink must never disable Secure Boot, signature enforcement, BitLocker, or
enable test-signing. A developer may test a separately marked test-signed build
only on a machine already dedicated and configured for driver development; no
DeskLink script performs that configuration. The normal application, input,
clipboard, audio, and communications-playback voice route continue to work when
the optional component is absent.

Installation is always a local, explicit UAC action. The helper accepts only
the fixed bundled package and does not accept a caller-supplied INF path. It
does not change Windows default input/output or per-application microphone
selections. Uninstall stops the DeskLink feed first, removes only the fixed
DeskLink root device/package, and requires a reboot only when Windows reports
that one is necessary. The application installer stops its runtime before it
invokes that fixed uninstall operation.

## Validation gates

Before a production package can ship:

1. the router, migration, source-filter, sink-failure, receive-mute, and route
   transition tests must pass;
2. the driver must build from the pinned upstream and WDK inputs;
3. the bridge validator must prove signal forwarding, silence before first
   data, underflow silence, oldest-drop overflow, stop-to-silence, and clean
   restart;
4. Driver Verifier and the relevant HLK audio tests must pass;
5. a physical Windows 11/Server 2022+ machine with zero physical microphones
   must show `DeskLink Remote Microphone`, remain silent while idle, and receive
   DeskLink voice in Discord without feedback or stale audio after disconnect,
   mute, permission revoke, process kill, sleep/resume, or endpoint restart.

Unsigned output is not installable validation evidence. Until the Microsoft
production-signed package exists and these gates pass, the virtual microphone
is an optional development component rather than a release feature.
