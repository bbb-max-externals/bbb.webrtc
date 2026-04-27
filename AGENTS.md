# bbb.webrtc — Max/MSP WebRTC External

## Purpose

Max/MSP external objects for WebRTC-based audio/video send and receive.

## Project Status

Greenfield. Skills and build infrastructure are set up via git submodules; no source code yet.

## Architecture

This project follows the `bbb.xxx.yyy` external convention. Expected structure:

```
bbb.webrtc/
├── CMakeLists.txt              # Root CMake (templates/CMakeLists.root.txt)
├── cmake/
│   └── bbb_external.cmake      # bbb_add_external() macro (shared)
├── deps/
│   ├── min-api/                # git submodule (max-sdk-base inside)
│   └── libwebrtc/              # TBD: prebuilt or built from source
├── source/
│   ├── projects/
│   │   ├── bbb.webrtc.send/    # Sender external
│   │   │   ├── CMakeLists.txt
│   │   │   └── bbb.webrtc.send.cpp
│   │   └── bbb.webrtc.recv/    # Receiver external
│   │       ├── CMakeLists.txt
│   │       └── bbb.webrtc.recv.cpp
│   └── bbb/                    # Shared headers (WebRTC session, signaling, etc.)
├── externals/                  # Build output (*.mxo)
├── help/                       # .maxhelp files
└── package-info.json
```

Naming convention: directory = `bbb.webrtc.send`, C++ class = `bbb_webrtc_send`, MIN_EXTERNAL arg = `bbb_webrtc_send`.

## Build

```bash
mkdir -p build && cd build
cmake ..
cmake --build .
```

Output: `externals/bbb.webrtc.send.mxo`, `externals/bbb.webrtc.recv.mxo` (Universal Binary: x86_64 + arm64).

Adding a new external: create directory under `source/projects/`, add `CMakeLists.txt` with `bbb_add_external()`, add `.mxo` entry to `package-info.json` filelist. Root CMake auto-discovers subdirectories.

## Skills

- **max-external**: New external scaffolding, CMake setup, naming conventions, `bbb_add_external()` reference
- **max-patgen**: Generate `.maxpat` / `.maxhelp` files as JSON
- **max-external-githubactions**: CI workflow for macOS/Windows builds

Load relevant skill before implementing: `skill(name="max-external")`.

## Key Constraints (from max-external skill pitfalls)

These are the non-obvious traps that cause silent failures or cryptic errors:

1. **Attribute values unavailable in constructor** — Use `timer<>::delay(0)` to defer init until attributes are set.
2. **Outlet output is main-thread only** — Worker threads must use `c74::min::queue<>` to deliver results to main thread. `timer::delay()` from worker threads silently does nothing.
3. **`std::filesystem` unavailable** — min-api sets `CMAKE_OSX_DEPLOYMENT_TARGET` to 10.11. Use `c74::min::path` or string operations.
4. **`NIL` macro collision** — Max SDK defines `#define NIL`. Wrap third-party includes with `#pragma push_macro("NIL")` / `#undef NIL`.
5. **`cout`/`cerr` are members** — Not `std::cout`. Use `cout << "msg" << c74::min::endl;`.
6. **`attribute<symbol>` to `std::string`** — Use `std::string(attr.get().c_str())`, not `std::string(attr)`.
7. **`enum_map` for int attributes** — `range{"a","b"}` + `style::enum_index` causes "bad number". Use `enum_map{...}`.
8. **`m_maxobj` is private** — Use public `maxobj()` method for Max API calls.

## WebRTC-Specific Considerations

- **Threading**: WebRTC runs its own threads. All outlet output must go through `c74::min::queue<>` to reach the Max main thread.
- **Library**: libwebrtc is large (~100MB+). Decide early: static link, dynamic link with RPATH, or wrapper over a separate process.
- **Signal path**: Audio from WebRTC → Max signal chain requires `c74::min::vector_operator<>` or buffer-based transfer, not simple message outlets.
- **Platform**: WebRTC APIs differ between macOS/Windows. Use `bbb_add_external(MACOS_ONLY)` or guard with `#ifdef _WIN32` if needed.
