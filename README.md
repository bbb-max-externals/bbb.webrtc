# bbb.webrtc

> [!WARNING]
> This repository is published as AI-assisted, insufficiently tested work in progress ("AI slop"). Treat it as experimental. Correctness, stability, compatibility, and fitness for production use are not guaranteed.

Max/MSP externals for sending and receiving audio and video via WebRTC.

## Objects

### bbb.webrtc.send

Captures audio from the Max signal chain, Opus-encodes it, and sends it via WebRTC.

```
[adc~] → [bbb.webrtc.send @stun_server stun:stun.l.google.com:19302]
                       |
                       | offer <sdp>
                       | candidate <c> <mid>
                       | state connected
```

**Attributes:**
- `@stun_server` — STUN server URL (default: `stun:stun.l.google.com:19302`)
- `@turn_server` — TURN server URL
- `@turn_username` / `@turn_password` — TURN credentials
- `@bitrate` — Opus bitrate in bps (default: 64000, range: 6000–510000)

**Messages:** `offer`, `answer <sdp>`, `candidate <c> <mid>`, `close`, `dump`

### bbb.webrtc.recv

Receives WebRTC audio, Opus-decodes it, and outputs to the Max signal chain.

```
[bbb.webrtc.recv @stun_server stun:stun.l.google.com:19302]
    |                                    |
    | [signal out]                       | answer <sdp>
    |                                    | candidate <c> <mid>
```

Same STUN/TURN attributes as send.

**Messages:** `offer <sdp>`, `candidate <c> <mid>`, `close`, `dump`

### bbb.webrtc.cfg

ICE configuration helper. Outputs STUN/TURN settings that can be routed to send/recv objects.

**Messages:** `bang` (output config), `defaults` (reset), `dump`

### bbb.webrtc.send.video

Jitter MOP object. Captures RGBA matrix frames, H.264-encodes (hardware), and sends via WebRTC.

```
[jit.noise 640 480 4] → [bbb.webrtc.send.video @width 640 @height 480 @bitrate 2000000]
                                   |
                                   | offer <sdp>
                                   | candidate <c> <mid>
                                   | state connected
```

**Attributes:**
- `@stun_server` — STUN server URL (default: `stun:stun.l.google.com:19302`)
- `@turn_server` — TURN server URL
- `@turn_username` / `@turn_password` — TURN credentials
- `@bitrate` — H.264 bitrate in bps (default: 2000000, range: 100000–10000000)
- `@width` — Video width (default: 640, range: 16–3840)
- `@height` — Video height (default: 480, range: 16–2160)
- `@fps` — Frame rate (default: 30, range: 1–60)

**Messages:** `offer`, `answer <sdp>`, `candidate <c> <mid>`, `close`, `dump`

### bbb.webrtc.recv.video

Jitter MOP object. Receives WebRTC video, H.264-decodes (hardware), and outputs RGBA matrix.

```
[bbb.webrtc.recv.video @stun_server stun:stun.l.google.com:19302]
    |                                    |
    | [matrix out → jit.window]          | answer <sdp>
    |                                    | candidate <c> <mid>
```

Same STUN/TURN attributes as send.video.

**Messages:** `offer <sdp>`, `candidate <c> <mid>`, `close`, `dump`

## Signaling

No built-in signaling server. Objects output SDP offers/answers and ICE candidates as Max messages. Route these through any Max networking object (`udpsend`, `tcpsend`, `netsend`) or copy-paste manually.

Typical flow:
1. Send: `[offer]` → outputs SDP offer → route to remote peer
2. Recv: `[offer <sdp>]` → outputs SDP answer → route back
3. Both: exchange ICE candidates via `[candidate <c> <mid>]`
4. State changes: `[state connected/disconnected/failed]`

## Build

```bash
git clone --recursive https://github.com/bbb-max-externals/bbb.webrtc.git
cd bbb.webrtc
cmake -B build -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build
```

### macOS (arm64 only)

```bash
cmake -B build -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build
```

### Windows

Requires Visual Studio 2022 + CMake.

```bash
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build --config Release
```

## CI

GitHub Actions builds both platforms on every push to `main`:

- **macOS**: Universal Binary (x86_64 + arm64). Builds OpenSSL from source for both architectures.
- **Windows**: x64 build via Visual Studio 2022.

## Dependencies

| Library | Method | Purpose |
|---------|--------|---------|
| min-api | git submodule | Max/MSP C++ API |
| libdatachannel | FetchContent (v0.22.6) | WebRTC (PeerConnection, ICE, DTLS, RTP) |
| libopus | FetchContent (v1.5.2) | Opus audio codec |

## License

MIT

### Third-Party Licenses

| Library | License | Copyright |
|---------|---------|-----------|
| [min-api](https://github.com/Cycling74/min-api) (max-sdk-base) | MIT | © Cycling '74 |
| [libdatachannel](https://github.com/paullouisageneau/libdatachannel) v0.22.6 | MPL 2.0 | © Paul-Louis Ageneau |
| [libopus](https://opus-codec.org/) v1.5.2 | BSD 3-Clause | © Xiph.Org, Skype Limited, Octasic, Jean-Marc Valin, Timothy B. Terriberry, CSIRO, Gregory Maxwell, Mark Borgerding, Erik de Castro Lopo, Mozilla, Amazon |
