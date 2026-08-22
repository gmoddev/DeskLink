# DeskLink Windows Backend Plan

## 1. Current Windows implementation

The repository currently includes `Win32InputInjector`.

It uses `SendInput` with:

- `KEYEVENTF_SCANCODE` for keyboard injection
- `KEYEVENTF_EXTENDEDKEY` when required
- `KEYEVENTF_KEYUP` for release
- mouse down/up flags for buttons
- `MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK` for absolute pointer movement

The injector tracks DeskLink-owned pressed keys/buttons and releases only those states during cleanup.

The current pointer adapter treats normalized coordinates as virtual-desktop coordinates. Production monitor routing needs a display-ID-to-monitor-rectangle transform first.

---

## 2. Host input capture

Recommended production implementation uses two mechanisms with separate responsibilities.

### Raw Input

Use Raw Input as the authoritative high-rate physical event source.

Responsibilities:

- physical mouse movement
- keyboard scan-code data
- device identity if required later
- high polling-rate devices

Raw input should feed a bounded in-process queue.

### Low-level hooks

Use `WH_KEYBOARD_LL` and `WH_MOUSE_LL` only as a suppression gate while routing input remote.

The hook callback must be minimal:

```text
read current routing flag
if remote -> suppress
else -> CallNextHookEx
```

Do not perform:

- network I/O
- disk logging
- heap-heavy serialization
- profile evaluation
- blocking locks

inside the hook callback.

A separate worker consumes Raw Input and sends DeskLink protocol messages.

---

## 3. Injection boundary

`SendInput` should remain the default injection mechanism.

Do not install a virtual HID driver in the initial product merely to make synthetic input look more physical.

A virtual HID backend should be a later optional component only if a concrete use case cannot be met by `SendInput`.

---

## 4. Foreground/profile detection

The Host profile engine should use WinEvent hooks for foreground-window changes rather than aggressive polling.

Profile evaluation inputs may include:

- executable path/name
- fullscreen state
- current foreground HWND
- current user override

Precedence:

```text
emergency fail-local
manual override
profile rule
system default
```

`GAME` should release remote focus and remove/disable interception components as aggressively as practical.

---

## 5. Monitor mapping

Use `EnumDisplayMonitors`, `GetMonitorInfo`, and current DPI/display APIs to produce stable runtime monitor descriptors.

Do not persist transient HMONITOR handles.

Persist a descriptor/fingerprint based on stable display identity information and require remapping if the topology changes materially.

The Host UI should store edge adjacency, for example:

```text
PC1.Display2.RIGHT -> PC2.Display1.LEFT
```

Crossing should include a small hysteresis/dead-zone to prevent focus oscillation.

---

## 6. WASAPI sender on PC2

Recommended capture path:

```text
IMMDeviceEnumerator
  ↓ default render endpoint
IAudioClient shared mode
  ↓ LOOPBACK flag
IAudioCaptureClient
  ↓
format normalization
  ↓
5 ms PCM blocks
  ↓
AudioFrame datagrams
```

V1 should normalize to:

```text
48 kHz / stereo / PCM16 / 5 ms
```

If the endpoint's mix format differs, convert in the sender before packetization.

---

## 7. WASAPI receiver on PC1

Recommended playback path:

```text
network datagrams
  ↓
AudioJitterBuffer
  ↓
loss concealment
  ↓
small asynchronous resampler
  ↓
gain
  ↓
IAudioRenderClient shared mode
```

Windows then mixes the DeskLink stream with local applications. DeskLink does not need to capture/re-mix all PC1 audio itself.

---

## 8. Clock drift controller

Separate machines do not have perfectly identical audio clocks.

Track jitter-buffer occupancy around a target. Apply very small resampling-ratio corrections when the buffer trends high/low.

Example policy:

```text
target occupancy: 20 ms
minimum: 10 ms
maximum: 50 ms
normal correction bound: ±0.1%
```

This prevents hours-long sessions from gradually underrunning or overflowing.

---

## 9. Device changes

The audio backend must handle:

- default endpoint changes
- device removal
- device disable/re-enable
- format change
- sleep/resume

Audio failure must not affect input capability availability.

Capabilities should degrade independently.

---

## 10. Local named pipe

Host should expose a current-user-only named pipe such as:

```text
\\.\pipe\DeskLink.Control
```

Create an explicit DACL for the current user SID.

The pipe protocol should use bounded typed commands and never expose arbitrary transport packets or OS commands.
