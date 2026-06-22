# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Frametap is a cross-platform C++20 screen capture library with a CLI tool and GUI application. It provides single-frame screenshots and real-time streaming on macOS, Linux (X11/Wayland), Windows, and Android.

## Build Commands

Build system is **SCons** (Python-based). All targets output to the project root.

```bash
scons                          # Build libframetap.a (default)
scons cli                      # Build CLI tool
scons gui                      # Build GUI application
scons example                  # Build capture example
scons test                     # Build and run full test suite
scons tests/test_runner        # Build test binary without running
```

### Running specific tests

```bash
./tests/test_runner "[unit]"          # Unit tests only (no display needed)
./tests/test_runner "[integration]"   # Integration tests (needs display)
./tests/test_runner "test name"       # Run single test by name
DISPLAY=:99 ./tests/test_runner       # Linux headless with Xvfb
```

### Build options

```bash
scons target=android                        # Android cross-compile
scons sanitize=address|thread|undefined     # Sanitizers
scons macos_arch=arm64|x86_64|universal     # macOS arch
scons crt=static|dynamic                    # Windows CRT linkage
```

### Submodules / recording deps

The GUI and recording deps are git submodules — run `git submodule update
--init --recursive` first. GPU recording also needs the NVENC headers
(`git clone https://github.com/FFmpeg/nv-codec-headers vendor/nv-codec-headers`).
On the first `scons record|cli|gui`, the vendored libsrt submodule is built once
via `cmake` (encryption off, no OpenSSL); UDP/RTMP work without it.

**Windows** recording/streaming/receiving (NVENC encode + NVDEC decode, video
only — no audio backend yet) builds into `scons cli`/`scons gui`:
- Build the GUI with `crt=static` (it links vcpkg's `glfw3:x64-windows-static`,
  which uses the static CRT). The vendored libsrt is auto-built as `/MT` to
  match; build the CLI with `crt=static` too when you need SRT.
- The NVENC/NVDEC API version must not exceed the installed driver's. If
  `OpenEncodeSessionEx`/decode fails with `INVALID_VERSION`, check out a
  matching `nv-codec-headers` tag (e.g. `n13.0.19.0` for driver branch 5xx).
- SRT latency is tunable per stream via `?latency=<ms>` on the URL (default 120
  ms suits the lossy internet; drop to ~30 ms or less on a LAN).

## Architecture

### Backend abstraction

`src/backend.h` defines the `Backend` interface. Platform backends implement it:

- **macOS** (`src/platform/macos/macos_backend.mm`) — ScreenCaptureKit (12.3+), CoreGraphics fallback
- **Linux** (`src/platform/linux/`) — Runtime dispatch between X11 and Wayland
  - X11: XShm shared memory capture (`src/platform/linux/x11/`)
  - Wayland: XDG Desktop Portal + PipeWire (`src/platform/linux/wayland/`)
- **Windows** (`src/platform/windows/`) — DXGI (Win10+), GDI fallback
- **Android** (`src/platform/android/`) — JNI bridge + MediaProjection

`make_backend()` factory functions create the correct platform implementation.

### Core patterns

- **Pimpl** — `FrameTap::Impl` hides platform details from the public API
- **Move-only** — `FrameTap` supports move semantics, no copy
- **Exceptions** — `CaptureError` thrown on capture failures with platform-specific messages
- **Thread-safe queue** — `ThreadSafeQueue<T>` in `include/frametap/queue.h` for frame delivery

### Public API

Headers are in `include/frametap/`. The main class is `frametap::FrameTap` with constructors for monitor, window, region, or primary-monitor capture. Key free functions: `get_monitors()`, `get_windows()`, `check_permissions()`.

### Recording & streaming (Linux/NVENC · macOS/VideoToolbox · Windows/NVENC)

`src/encode/` holds the GPU recording pipeline, compiled into the `record`
example and (when NVENC headers are present) the CLI/GUI. It has **no
ffmpeg/libav dependency**:

- **NVENC** (`nvenc_encoder.cpp`) encodes RGBA → Annex-B H.264/HEVC; CUDA/NVENC are loaded at runtime via the cross-platform shim in `encode/dynlib.h` (`dlopen` on POSIX, `LoadLibrary` of `nvcuda.dll`/`nvEncodeAPI64.dll` on Windows). NVDEC (`decode/nvdec_decoder.cpp`) decodes the same way (`nvcuvid.dll`). The video encoder/decoder and audio capture are the only platform-specific pieces; muxers, AAC, and the SRT/UDP/RTMP transports are shared (sockets via `net_compat.h`). On Windows, audio capture and AAC are stubbed (`audio/null_capture.h`, `encode/aac_encoder_null.cpp`) so recordings/streams are video-only.
- **Muxers** are hand-rolled: `mp4_muxer.cpp` (file), `ts_muxer.cpp` (MPEG-TS), and FLV inside `rtmp_sink.cpp`. `nal_util.cpp` converts Annex-B → length-prefixed and builds avcC/hvcC.
- **AAC** (`aac_encoder.cpp`) wraps vendored vo-aacenc (Apache-2.0).
- **Streaming** goes through the `StreamSink` interface (`stream_sink.h`): `TsSink` over UDP/SRT (`ts_sink.cpp`, `net_transport.cpp`) and `RtmpSink` over TCP. `NetworkStreamer` (`net_stream.cpp`) owns the worker thread + bounded queue. Sockets are cross-platform via `net_compat.h` (POSIX + Winsock).
- `VideoRecorder` (`recording.h` / `recorder.cpp`) ties capture frames to the encoder, muxer, and optional stream.

### Test infrastructure

Catch2 v3 with tags `[unit]` (headless-safe) and `[integration]` (display-required). `tests/helpers.h` provides `has_display()` to skip integration tests in CI. Muxer logic is covered by `[unit][muxer]` tests in `tests/test_muxer.cpp` (no GPU needed).

## Code Style

LLVM style via `.clang-format`: 80-column limit, 2-space indent.
