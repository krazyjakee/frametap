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

### Test infrastructure

Catch2 v3 with tags `[unit]` (headless-safe) and `[integration]` (display-required). `tests/helpers.h` provides `has_display()` to skip integration tests in CI.

## Code Style

LLVM style via `.clang-format`: 80-column limit, 2-space indent.
