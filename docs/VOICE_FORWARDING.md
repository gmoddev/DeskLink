# DeskLink voice forwarding

## Status and scope

DeskLink has an implementation-complete, security-gated microphone forwarding
slice on the protocol-v5 development branch. It is deliberately separate from
system-audio forwarding. Production qualification remains blocked on the
physical two-PC voice matrix described below.

The first slice is push-to-talk (PTT) only. It does not implement open-mic,
voice activation, acoustic echo cancellation, mixing, conferencing, recording,
or remote microphone activation. The WinUI hold button and typed local runtime
control are implemented. A global PTT binding is the one explicitly deferred
control surface because adding it safely requires a separate input-lifecycle
design.

## Security and privacy invariants

- Voice uses the existing mutually authenticated, pinned MsQuic session and
  fresh session nonce. It never creates a second trust or TLS path.
- `VoiceSend` and `VoiceReceive` are distinct from system-audio permissions.
  Existing trust records gain neither bit during schema migration.
- Sending requires this PC to have granted `VoiceReceive`, the peer to have
  reported `VoiceSend`, and the current local grant revision to have been
  acknowledged exactly. Receiving applies the complementary check again.
- `VoiceFrame` is accepted only as a QUIC datagram after `PeerValidated`, exact
  protocol-v5 decoding, current nonce validation, reciprocal grant admission,
  and strict format/size checks.
- Only a local current-user PTT command can open the capture endpoint. There is
  no network message that presses PTT, clears mute, or selects a microphone.
- Hard mute prevents PTT from starting capture and stops active capture before
  reporting success. PTT release, permission loss, disconnect, endpoint loss,
  process shutdown, or configuration change stops capture and requires a fresh
  local PTT activation.
- Audio samples and decoded voice are bounded in memory and are never written
  to disk or included in diagnostics. Logs contain state and counters only.
- A malformed or unauthorized voice datagram is rejected locally. Voice-device
  failure does not grant focus, restart TLS, weaken identity checks, or change
  system-audio admission.

## Data path and authorization

Transmit:

```text
authenticated PeerSession
  -> exact reciprocal acknowledged voice grants
  -> local voice-route intent
  -> local hard mute is clear
  -> local PTT press
  -> selected eCapture endpoint (default communications or exact saved ID)
  -> WASAPI shared/event-driven 48 kHz mono PCM16 normalization
  -> exact 960-sample / 20 ms frame
  -> Opus VOIP encoder
  -> bounded VoiceFrame
  -> dedicated voice datagram sequence
```

Receive:

```text
MsQuic datagram
  -> PeerValidated transport
  -> protocol-v5 datagram-lane decode
  -> current session nonce
  -> reciprocal acknowledged voice grants
  -> stream ID + independent sequence checks
  -> bounded adaptive jitter / Opus FEC / Opus PLC
  -> local incoming gain and echo guard
  -> eRender communications endpoint
```

Authority is re-evaluated after capability updates. Any loss stops the
transmitter synchronously and rejects subsequent frames. Reconnect creates a
fresh session nonce, resets voice stream/sequence state, and does not restore an
old PTT-down state.

## Wire format

Protocol version 5 adds `VoiceSend` (bit 13), `VoiceReceive` (bit 14), and the
datagram-only `VoiceFrame` (type 32):

```text
stream_id               u32, nonzero
sample_rate              u32, exactly 48000
samples_per_channel      u16, exactly 960
channels                  u8, exactly 1
codec                     u8, Opus = 1
capture_timestamp_us      u64, diagnostic timing only
encoded_size             u16, 1..512
encoded                   encoded_size bytes
```

Voice has its own envelope sequence counter. A new nonzero stream ID is chosen
for each PTT activation. Old-stream packets cannot re-enter a newer stream.
Protocol 4 peers are incompatible and are rejected during negotiation; there is
no voice downgrade.

## Codec and playout

The build fetches Opus 1.6.1 from the Xiph release archive with SHA-256
`6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1`.
The static encoder uses `OPUS_APPLICATION_VOIP`, 32 kbit/s, VBR, in-band FEC,
10% expected packet loss, and DTX disabled. Decoder failures are contained to
the voice module.

Playout starts at two packets (40 ms), increases immediately on observed
reordering/gaps to a hard six-packet (120 ms) target, and decreases only after
100 stable arrivals. Queues remain bounded to twelve encoded/decoded frames.
One missing frame uses in-band FEC from the following packet when possible and
otherwise Opus PLC. Invalid formats, duplicates, stale streams, excessive
future gaps, oversized payloads, and queue overflow are counted and rejected.

Incoming gain is local attenuation from 0 through 100% with a one-frame ramp.
It never changes the Windows system mixer.

## Windows devices and feedback protection

The microphone backend enumerates active `eCapture` endpoints and defaults to
the communications-role capture device. A saved endpoint is opened by exact ID;
if it disappears, DeskLink does not silently substitute another microphone.
Capture uses the communications audio category and `NOPERSIST` so DeskLink does
not alter persistent Windows mixer policy.

Voice renders to the communications-role `eRender` endpoint. This does not
alter the existing system-audio loopback source or ordinary system-audio render
path. A build-time source check rejects loopback capture APIs in the voice
backend.

Echo guard defaults on. While this PC transmits, incoming DeskLink voice is
locally ramped to mute; release restores it. This is half-duplex feedback-loop
protection, not acoustic echo cancellation. Full software AEC remains deferred.

## Product behavior

Preferences schema 5 stores a separate voice route, optional exact input
endpoint ID, incoming gain, and echo-guard setting. Migration from older schema
versions leaves voice off, selects the default communications microphone,
sets gain to 100%, and enables echo guard. Route and device changes are
negotiated through the existing managed runtime; they do not modify pairing.

Pairing and the Devices page expose two separate, default-off consequences:

- allow the peer to play microphone voice into this PC (`VoiceSend`);
- allow the peer to receive this PC's microphone voice (`VoiceReceive`).

The feature card shows off, permission missing, PTT ready, transmitting,
muted, and input-unavailable states. Capture is not started by enabling the
route. Press-and-hold pointer capture owns the PTT lifetime so release,
cancellation, or pointer-capture loss sends a local PTT-up command.

## Validation gates

Automated coverage must remain green for:

- protocol-v5 round trip, wrong lane, malformed metadata, empty/oversized Opus,
  stale nonce, stream changes, duplicates, reordering, gaps, and queue bounds;
- reciprocal acknowledged grants, live revocation, and independence from
  system-audio grants;
- deterministic Opus encode/decode, FEC/PLC, gain ramp, mute, and adaptive
  jitter bounds;
- preference migration, separate route planning, default echo guard, and
  current-user control framing;
- native Windows, MsQuic loopback/runtime, reliability soak, locked WinUI, and
  the voice-capture isolation source check.

Before production sign-off, two supported physical Windows PCs must pass:

1. PTT A to B and B to A, followed by immediate capture stop on release.
2. Hard mute, route off, permission revocation, disconnect during PTT, process
   termination, and reconnect requiring a fresh PTT action.
3. Default and explicit microphone selection, unplug/default changes, and no
   fallback from a missing explicit endpoint.
4. Controlled packet loss/reordering with bounded FEC/PLC and no unbounded
   latency growth.
5. Speaker feedback checks with echo guard on and off, clearly retaining the
   no-AEC limitation.
6. Confirmation that system-audio forwarding, input roaming, clipboard,
   suspend/resume, reconnect, and fail-local behavior are unchanged.

Latency goals are measured validation targets, not protocol constants: capture
to authenticated network emission below 30 ms under normal load, median
software arrival-to-WASAPI submission below 80 ms, p95 below 150 ms, with no
unbounded growth during loss or scheduler disturbance.
