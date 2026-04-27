# bbb.webrtc

Max/MSP externals for sending and receiving audio via WebRTC.

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

## Signaling

No built-in signaling server. Objects output SDP offers/answers and ICE candidates as Max messages. Route these through any Max networking object (`udpsend`, `tcpsend`, `netsend`) or copy-paste manually.

Typical flow:
1. Send: `[offer]` → outputs SDP offer → route to remote peer
2. Recv: `[offer <sdp>]` → outputs SDP answer → route back
3. Both: exchange ICE candidates via `[candidate <c> <mid>]`
4. State changes: `[state connected/disconnected/failed]`

## Build

```bash
git clone --recursive https://github.com/2bbb/bbb.webrtc.git
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
