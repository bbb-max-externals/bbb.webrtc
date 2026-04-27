# bbb.webrtc — Max/MSP WebRTC External

## Purpose

Max/MSP external objects for WebRTC-based audio and video send/receive.

## Externals

### bbb.webrtc.send — MSP Audio Sender

Captures audio from Max signal chain, Opus-encodes, and sends via WebRTC.

```
[adc~] → [bbb.webrtc.send @stun_server stun:stun.l.google.com:19302]
                       |
                       | offer <sdp>        (send to remote via udpsend etc.)
                       | candidate <c> <mid>
                       | state connected
```

- `@stun_server` — STUN server URL (default: `stun:stun.l.google.com:19302`)
- `@turn_server` — TURN server URL (empty = no TURN)
- `@turn_username` / `@turn_password` — TURN credentials
- `@bitrate` — Opus bitrate in bps (default: 64000, range: 6000–510000)
- Messages: `offer`, `answer <sdp>`, `candidate <c> <mid>`, `close`, `dump`
- Left inlet: MSP signal. Right outlet: SDP/ICE/status messages.

### bbb.webrtc.recv — MSP Audio Receiver

Receives WebRTC audio, Opus-decodes, and outputs to Max signal chain.

```
[bbb.webrtc.recv @stun_server stun:stun.l.google.com:19302]
    |                                    |
    | [signal out]                       | answer <sdp>
    |                                    | candidate <c> <mid>
```

- Same STUN/TURN attributes as send.
- Messages: `offer <sdp>`, `candidate <c> <mid>`, `close`, `dump`
- Left outlet: MSP signal. Right outlet: SDP/ICE/status messages.

### bbb.webrtc.cfg — ICE Configuration Helper

Outputs formatted ICE server config that can be routed to send/recv objects.

```
[bbb.webrtc.cfg @stun_server stun:... @turn_server turn:... @turn_username foo @turn_password bar]
```

- Messages: `bang` (output config), `defaults` (reset), `dump`

### bbb.webrtc.send.video — Jitter Video Sender

Jitter MOP object. Captures RGBA matrix frames, H.264-encodes (hardware), sends via WebRTC.

```
[jit.matrix] → [bbb.webrtc.send.video @width 640 @height 480 @bitrate 2000000 @fps 30]
                               |
                               | offer <sdp>
                               | candidate <c> <mid>
                               | state connected
```

- Same STUN/TURN attributes as audio send.
- `@width` — Video width (default: 640, range: 16–3840)
- `@height` — Video height (default: 480, range: 16–2160)
- `@bitrate` — H.264 bitrate in bps (default: 2000000, range: 100000–10000000)
- `@fps` — Frame rate (default: 30, range: 1–60)
- Messages: `offer`, `answer <sdp>`, `candidate <c> <mid>`, `close`, `dump`
- Uses `matrix_operator<>` (Jitter MOP). Captures frame at `calc_cell(0,0)`.

### bbb.webrtc.recv.video — Jitter Video Receiver

Jitter MOP object. Receives WebRTC video, H.264-decodes (hardware), outputs RGBA matrix.

```
[bbb.webrtc.recv.video @stun_server stun:stun.l.google.com:19302]
    |                                    |
    | [matrix out]                       | answer <sdp>
    |                                    | candidate <c> <mid>
```

- Same STUN/TURN attributes.
- Messages: `offer <sdp>`, `candidate <c> <mid>`, `close`, `dump`
- Generator-mode `matrix_operator<>`. Writes decoded RGBA in `calc_cell` (plane_count == 4).

## Architecture

```
bbb.webrtc/
├── CMakeLists.txt              # Root CMake (FetchContent for deps)
├── cmake/
│   ├── bbb_external.cmake      # bbb_add_external() macro
│   └── generate_version.cmake  # Auto-version from git commit count
├── deps/
│   └── min-api/                # git submodule (max-sdk-base inside)
├── source/
│   ├── webrtc/                 # Shared static libraries
│   │   ├── include/bbb/
│   │   │   ├── webrtc_session.hpp
│   │   │   ├── opus_codec.hpp
│   │   │   ├── audio_ring_buffer.hpp
│   │   │   ├── video_session.hpp
│   │   │   ├── video_encoder.hpp
│   │   │   ├── video_decoder.hpp
│   │   │   └── color_convert.hpp
│   │   └── src/
│   │       ├── webrtc_session.cpp    (bbb_webrtc)
│   │       ├── opus_codec.cpp        (bbb_webrtc)
│   │       ├── audio_ring_buffer.cpp (bbb_webrtc)
│   │       ├── video_session.cpp     (bbb_webrtc_video)
│   │       ├── color_convert.cpp     (bbb_webrtc_video)
│   │       ├── video_encoder_videotoolbox.mm  (macOS)
│   │       ├── video_decoder_videotoolbox.mm  (macOS)
│   │       ├── video_encoder_mf.cpp           (Windows)
│   │       └── video_decoder_mf.cpp           (Windows)
│   ├── projects/
│   │   ├── bbb.webrtc.send/     (links bbb_webrtc)
│   │   ├── bbb.webrtc.recv/     (links bbb_webrtc)
│   │   ├── bbb.webrtc.cfg/      (links bbb_webrtc)
│   │   ├── bbb.webrtc.send.video/ (links bbb_webrtc_video)
│   │   └── bbb.webrtc.recv.video/ (links bbb_webrtc_video)
│   └── bbb/version.h           # Auto-generated
├── externals/                  # Build output (.mxo)
├── help/
└── package-info.json
```

Two static libraries:
- `bbb_webrtc` — Audio core (session, opus, ring buffer). Linked by all externals.
- `bbb_webrtc_video` — Video extension (encoder, decoder, color convert, video session). Links `bbb_webrtc` + platform video frameworks. Linked only by video externals.

Naming convention: directory = `bbb.webrtc.send`, C++ class = `bbb_webrtc_send`, MIN_EXTERNAL arg = `bbb_webrtc_send`.

## Dependencies

| Library | Method | Purpose |
|---------|--------|---------|
| min-api | git submodule | Max/MSP C++ API |
| libdatachannel | FetchContent (v0.22.6) | WebRTC (PeerConnection, ICE, DTLS, RTP) |
| libopus | FetchContent (v1.5.2) | Opus audio codec |

## Build

```bash
git clone --recursive <repo>
mkdir -p build
cmake -B build -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build
```

`CMAKE_POLICY_VERSION_MINIMUM=3.5` is required for libdatachannel's plog dependency.

For arm64 only (Apple Silicon):
```bash
cmake -B build -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_OSX_ARCHITECTURES=arm64
```

Universal Binary (x86_64 + arm64) requires OpenSSL built for both architectures. CI builds OpenSSL from source and creates universal static libraries.

Adding a new external: create directory under `source/projects/`, add `project(bbb.webrtc.xxx)` then `bbb_add_external()` in its CMakeLists.txt, add `.mxo` entry to `package-info.json` filelist. Root CMake auto-discovers subdirectories.

## Signaling

No built-in signaling server. Objects output SDP offers/answers and ICE candidates as Max messages. Route these through any Max networking object (udpsend, tcpsend, netsend) or copy-paste manually.

Typical flow:
1. `[offer]` → send outputs SDP offer via outlet → route to remote peer
2. Remote peer's recv receives `[offer <sdp>]` → outputs SDP answer → route back
3. Both sides exchange ICE candidates via `[candidate <c> <mid>]`
4. State changes output via `[state connected/disconnected/failed]`

## Skills

- **max-external**: New external scaffolding, CMake setup, naming conventions
- **max-patgen**: Generate `.maxpat` / `.maxhelp` files as JSON
- **max-external-githubactions**: CI workflow for macOS/Windows builds

## Key Constraints

1. **Attribute values unavailable in constructor** — Use `timer<>::delay(0)` to defer init.
2. **Outlet output is main-thread only** — Worker threads must use `c74::min::queue<>`. `timer::delay()` from worker threads silently does nothing.
3. **`std::filesystem` unavailable** — min-api sets deployment target to 10.11.
4. **`NIL` macro collision** — Max SDK defines `#define NIL`. Wrap third-party includes.
5. **`cout`/`cerr` are members** — Not `std::cout`.
6. **`attribute<symbol>` to `std::string`** — Use `std::string(attr.get().c_str())`.
7. **`audio_bundle` uses `double*`** — `samples(channel)` returns `double*`, not `float*`.
8. **`vector_operator<>` takes no template args** — Not `vector_operator<MyClass>`.
9. **`rtc::binary` is `std::vector<std::byte>`** — Cast from/to `uint8_t` explicitly.
10. **`project()` required in subdirectory CMakeLists.txt** — Without it, `bbb_add_external()` uses the root project name for all targets.
11. **`calc_cell` template instantiates for multiple plane_counts** — Use `if constexpr (plane_count == 4)` for RGBA handling.
12. **MF `ProcessMessage` takes `ULONG_PTR`** — Use `0`, not `nullptr`, on Windows.
13. **MF has no `GetOutputType`** — Use `GetOutputAvailableType(stream, index, &type)`.
14. **`matrix_info::m_bip` is raw RGBA pointer** — Access directly for frame capture in send.video.

## CI

GitHub Actions workflow at `.github/workflows/build.yml`:
- **macOS** (`macos-latest`): Universal Binary (x86_64 + arm64)
  - Builds OpenSSL 3.3.2 from source for both architectures
  - Creates universal static libraries via `lipo`
  - Caches OpenSSL build for subsequent runs
  - Verifies `.mxo` is universal with `lipo -info`
- **Windows** (`windows-latest`): x64 build via Visual Studio 2022
  - Builds OpenSSL 3.3.2 from source with `/MT` (static CRT)
  - Caches OpenSSL build for subsequent runs
- Both use `CMAKE_POLICY_VERSION_MINIMUM=3.5`, `OPENSSL_USE_STATIC_LIBS=ON`, and `submodules: recursive`

## Help Files

Help patches are in the `help/` directory:
- `help/bbb.webrtc.send.maxhelp`
- `help/bbb.webrtc.recv.maxhelp`
- `help/bbb.webrtc.cfg.maxhelp`
- `help/bbb.webrtc.send.video.maxhelp`
- `help/bbb.webrtc.recv.video.maxhelp`

A loopback test patch is at `help/bbb.webrtc-test.maxpat`.

## Platform Support

- **macOS**: arm64 (tested). Universal Binary (x86_64 + arm64) via CI — builds OpenSSL from source for both archs, `lipo` creates universal static libs.
- **Windows**: `.mxe64` supported via `package-info.json`. Requires Visual Studio 2022 + CMake. OpenSSL built from source with `/MT` to match min-api CRT. Build command: `cmake -B build -G "Visual Studio 17 2022" -A x64 && cmake --build build --config Release`.
