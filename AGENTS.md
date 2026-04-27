# bbb.webrtc — Max/MSP WebRTC External

## Purpose

Max/MSP external objects for WebRTC-based audio send and receive.

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
│   ├── webrtc/                 # Shared static library
│   │   ├── include/bbb/
│   │   │   ├── webrtc_session.hpp
│   │   │   ├── opus_codec.hpp
│   │   │   └── audio_ring_buffer.hpp
│   │   └── src/
│   ├── projects/
│   │   ├── bbb.webrtc.send/
│   │   ├── bbb.webrtc.recv/
│   │   └── bbb.webrtc.cfg/
│   └── bbb/version.h           # Auto-generated
├── externals/                  # Build output (.mxo)
├── help/
└── package-info.json
```

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

Each external has a `.maxhelp` patch in its project directory:
- `source/projects/bbb.webrtc.send/bbb.webrtc.send.maxhelp`
- `source/projects/bbb.webrtc.recv/bbb.webrtc.recv.maxhelp`
- `source/projects/bbb.webrtc.cfg/bbb.webrtc.cfg.maxhelp`

A loopback test patch is at `help/bbb.webrtc-test.maxpat`.

## Platform Support

- **macOS**: arm64 (tested). Universal Binary (x86_64 + arm64) via CI — builds OpenSSL from source for both archs, `lipo` creates universal static libs.
- **Windows**: `.mxe64` supported via `package-info.json`. Requires Visual Studio 2022 + CMake. OpenSSL built from source with `/MT` to match min-api CRT. Build command: `cmake -B build -G "Visual Studio 17 2022" -A x64 && cmake --build build --config Release`.
