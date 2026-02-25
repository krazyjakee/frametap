# Frametap Testing Plan

## Overview

This document defines the testing strategy for frametap, a cross-platform C++20 screen capture library targeting macOS (ScreenCaptureKit / CoreGraphics), Linux (X11 / Wayland+PipeWire), and Windows (DXGI / GDI).

Testing a screen capture library is inherently platform-dependent and requires real display servers for integration tests. The strategy below separates what can be tested in headless CI from what requires manual or hardware-specific validation.

---

## 1. Test Framework & Tooling

| Tool | Purpose |
|------|---------|
| [Catch2](https://github.com/catchorg/Catch2) | Unit & integration test runner (header-only, C++20 compatible) |
| SCons | Build test targets alongside the library |
| GitHub Actions | CI matrix (macOS, Ubuntu, Windows) |
| Xvfb | Virtual X11 server for headless Linux X11 tests |
| `stb_image` (vendored) | Pixel-level image verification |

### Build integration

Add a `scons test` target that compiles and runs the test suite:

```
tests/
  test_main.cpp          # Catch2 main entry
  test_types.cpp         # Data type unit tests
  test_queue.cpp         # ThreadSafeQueue tests
  test_color.cpp         # BGRA/RGBA conversion tests
  test_enumerate.cpp     # Monitor/window enumeration (integration)
  test_screenshot.cpp    # Single-frame capture (integration)
  test_streaming.cpp     # Streaming capture (integration)
  test_permissions.cpp   # Permission diagnostics (integration)
  test_lifecycle.cpp     # Object lifecycle & edge cases
```

---

## 2. Unit Tests (headless, all platforms)

These tests have no display server dependency and must pass in CI.

### 2.1 Data types (`test_types.cpp`)

| Test | Description |
|------|-------------|
| `Rect default values` | Default-constructed `Rect` has zeroed fields |
| `Rect stores values` | Constructed `Rect{10, 20, 1920, 1080}` round-trips correctly |
| `Monitor fields` | All `Monitor` fields (id, x, y, width, height, name, scale) are readable |
| `Window fields` | All `Window` fields (id, name, x, y, width, height) are readable |
| `ImageData empty` | Default `ImageData` has empty data, 0x0 dimensions |
| `ImageData move` | Move constructor transfers ownership, source is empty |
| `Frame duration` | `Frame.duration_ms` stores and retrieves correctly |
| `PermissionStatus values` | Enum values `ok`, `warning`, `error` are distinct |

### 2.2 ThreadSafeQueue (`test_queue.cpp`)

| Test | Description |
|------|-------------|
| `push and pop` | Push one item, pop returns it |
| `FIFO order` | Items come out in insertion order |
| `pop blocks until push` | Pop on empty queue blocks; push from another thread unblocks it |
| `try_pop empty` | Returns `std::nullopt` immediately on empty queue |
| `try_pop non-empty` | Returns the front element and removes it |
| `multi-producer` | N threads push concurrently; all items are received exactly once |
| `multi-consumer` | N threads pop concurrently; no item is delivered twice |
| `stress test` | 10k push/pop cycles across 4 threads with no data races (run under TSan) |

### 2.3 Color conversion (`test_color.cpp`)

| Test | Description |
|------|-------------|
| `BGRA to RGBA single pixel` | `{B, G, R, A}` becomes `{R, G, B, A}` |
| `RGBA to BGRA roundtrip` | Converting twice returns original bytes |
| `Full buffer conversion` | 1920x1080x4 buffer converts all pixels correctly |
| `In-place conversion` | Buffer is modified in-place without allocation |
| `Edge values` | `{0, 0, 0, 0}` and `{255, 255, 255, 255}` convert correctly |
| `Odd buffer sizes` | Non-aligned buffer lengths handled (1x1, 3x3) |

### 2.4 FrameTap lifecycle (`test_lifecycle.cpp`)

| Test | Description |
|------|-------------|
| `Default constructor` | `FrameTap()` constructs without throwing (may fail on headless; guard with `SKIP`) |
| `Move semantics` | `FrameTap` is move-constructible and move-assignable |
| `No copy` | `FrameTap` is not copy-constructible or copy-assignable (static_assert) |
| `Double stop` | Calling `stop()` twice does not crash or throw |
| `Stop without start` | Calling `stop()` before `start()` is a no-op |
| `Destructor stops` | Destroying a streaming `FrameTap` calls `stop()` implicitly |
| `Pause without start` | `pause()` before `start()` is a no-op or throws predictable error |

---

## 3. Integration Tests (require display server)

These tests need a real or virtual display. On Linux CI, use Xvfb for X11 tests. Wayland, macOS, and Windows integration tests require real sessions or are manual-only.

### 3.1 Monitor & window enumeration (`test_enumerate.cpp`)

| Test | Description |
|------|-------------|
| `get_monitors non-empty` | Returns at least one monitor when display is available |
| `Monitor fields valid` | Each monitor has `width > 0`, `height > 0`, `scale >= 1.0` |
| `Monitor names non-empty` | Each monitor has a non-empty `name` string |
| `get_windows returns list` | Returns zero or more windows (content varies) |
| `Window fields valid` | Each window has non-zero `id` and `width > 0`, `height > 0` |

### 3.2 Screenshots (`test_screenshot.cpp`)

| Test | Description |
|------|-------------|
| `Full screen screenshot` | `FrameTap().screenshot()` returns non-empty `ImageData` |
| `Screenshot dimensions` | Returned `width` and `height` match or exceed the requested region |
| `Screenshot pixel format` | Data size == `width * height * 4` (RGBA) |
| `Region screenshot` | `screenshot(Rect{0, 0, 100, 100})` captures a 100x100 region |
| `Non-zero pixels` | At least some pixels are non-zero (not a blank image) |
| `Monitor screenshot` | Capture from a specific `Monitor` returned by `get_monitors()` |
| `Window screenshot` | Capture from a specific `Window` returned by `get_windows()` |
| `Invalid region` | Zero-width or zero-height region throws `CaptureError` |

### 3.3 Streaming capture (`test_streaming.cpp`)

| Test | Description |
|------|-------------|
| `Start and receive frames` | `start()` with callback receives at least 1 frame within 2 seconds |
| `Frame dimensions valid` | Each frame has `width > 0`, `height > 0`, correct data size |
| `Frame duration positive` | `duration_ms > 0` for each frame |
| `Stop halts delivery` | No callbacks fire after `stop()` returns |
| `Pause suspends` | After `pause()`, no new frames arrive for 500ms |
| `Resume restarts` | After `resume()`, frames start arriving again within 1 second |
| `is_paused state` | `is_paused()` returns correct value after `pause()`/`resume()` |
| `start_async non-blocking` | `start_async()` returns immediately; frames arrive on background thread |
| `Callback thread safety` | Callback fires on a non-main thread; verify with `std::this_thread::get_id()` |
| `set_region mid-stream` | Calling `set_region()` during streaming changes capture area |

### 3.4 Permissions (`test_permissions.cpp`)

| Test | Description |
|------|-------------|
| `check_permissions returns` | Function returns a `PermissionCheck` without throwing |
| `Status is valid enum` | Returned `status` is one of `ok`, `warning`, `error` |
| `Granted on CI` | On CI with display access, status should be `ok` (platform-specific) |

---

## 4. Platform-Specific Tests

### 4.1 macOS

| Test | Backend | Description |
|------|---------|-------------|
| `SCK streaming` | ScreenCaptureKit | Receives frames via `SCStream` delegate |
| `CGDisplay screenshot` | CoreGraphics | `CGDisplayCreateImageForRect` returns valid image |
| `CGWindow screenshot` | CoreGraphics | `CGWindowListCreateImage` captures a specific window |
| `Permission prompt` | ScreenCaptureKit | Verify `check_permissions()` detects screen recording permission |
| `Retina scaling` | Both | Screenshot dimensions account for `scale` factor |

### 4.2 Linux X11

| Test | Backend | Description |
|------|---------|-------------|
| `XShm available` | X11 | `XShmQueryExtension` returns true on Xvfb |
| `XShm screenshot` | X11 | Captures frame via shared memory |
| `Xinerama monitors` | X11 | `get_monitors()` returns valid output on multi-head Xvfb |
| `_NET_CLIENT_LIST` | X11 | `get_windows()` parses EWMH window list |
| `No DISPLAY` | X11 | Throws `CaptureError` when `$DISPLAY` is unset |

### 4.3 Linux Wayland

| Test | Backend | Description |
|------|---------|-------------|
| `Portal session` | Portal+PipeWire | D-Bus `CreateSession` / `SelectSources` / `Start` sequence completes |
| `PipeWire stream` | PipeWire | Stream connects and receives at least 1 frame |
| `DMA-BUF preferred` | PipeWire | Format negotiation tries DMA-BUF before SHM |
| `SHM fallback` | PipeWire | Falls back to SHM when DMA-BUF unavailable |
| `No WAYLAND_DISPLAY` | Wayland | Throws `CaptureError` when `$WAYLAND_DISPLAY` is unset |

### 4.4 Windows

| Test | Backend | Description |
|------|---------|-------------|
| `DXGI streaming` | DXGI | Desktop Duplication receives frames |
| `GDI fallback` | GDI | When DXGI fails (e.g. RDP), GDI capture works |
| `GDI window capture` | GDI | `BitBlt` on a specific window DC returns valid image |
| `EnumWindows` | Win32 | `get_windows()` enumerates visible windows |
| `DXGI monitors` | DXGI | `get_monitors()` lists all adapters/outputs |

---

## 5. Error Handling & Edge Cases

| Test | Description |
|------|-------------|
| `CaptureError is catchable` | `catch(const CaptureError& e)` works, `e.what()` is non-empty |
| `CaptureError inherits runtime_error` | `catch(const std::runtime_error&)` also catches it |
| `No display server` | Library throws `CaptureError` (not segfault) when no display is available |
| `Invalid monitor ID` | Constructing with a fabricated `Monitor` throws or returns empty capture |
| `Invalid window ID` | Constructing with a fabricated `Window` throws or returns empty capture |
| `Rapid start/stop cycles` | 100 start/stop cycles without leaks or crashes |
| `Concurrent instances` | Two `FrameTap` instances streaming simultaneously do not interfere |
| `Large region` | Region larger than screen is clamped or throws, not UB |
| `Negative coordinates` | `Rect{-100, -100, 200, 200}` is handled gracefully |

---

## 6. Performance Tests

These are not pass/fail; they establish baselines and detect regressions.

| Test | Metric | Target |
|------|--------|--------|
| `Screenshot latency` | Time from `screenshot()` call to return | < 100ms (1080p) |
| `Streaming throughput` | Frames per second for 1080p monitor | >= 30 fps |
| `Streaming throughput 4K` | Frames per second for 4K monitor | >= 15 fps |
| `Memory steady state` | RSS during 10-second stream | No unbounded growth |
| `BGRA→RGBA throughput` | Conversion speed for 1080p buffer | < 5ms per frame |
| `Start-to-first-frame` | Time from `start()` to first callback | < 500ms |
| `Queue throughput` | Push/pop operations per second | > 100k ops/sec |

### Benchmarking approach

Use `std::chrono::steady_clock` for timing. Run each benchmark 10 times and report median. Track results in CI artifacts for trend analysis.

---

## 7. Sanitizer & Static Analysis

| Tool | Purpose | CI Integration |
|------|---------|----------------|
| **AddressSanitizer (ASan)** | Buffer overflows, use-after-free | `scons sanitize=address` |
| **ThreadSanitizer (TSan)** | Data races in streaming/queue | `scons sanitize=thread` |
| **UndefinedBehaviorSanitizer (UBSan)** | Undefined behavior | `scons sanitize=undefined` |
| **Valgrind** | Memory leak detection (Linux) | `valgrind --leak-check=full ./test_runner` |
| **clang-tidy** | Static analysis, modernization | `clang-tidy src/**/*.cpp` |
| **cppcheck** | Additional static analysis | `cppcheck --enable=all src/` |

### Sanitizer build variants

Add a `sanitize` option to SConstruct:

```python
# scons sanitize=address
# scons sanitize=thread
# scons sanitize=undefined
```

---

## 8. CI Matrix

```yaml
matrix:
  os: [macos-latest, ubuntu-latest, windows-latest]
  build_type: [release, debug]
  sanitizer: [none, address, thread]
  exclude:
    - os: windows-latest
      sanitizer: thread    # TSan not supported on MSVC
```

### CI steps

1. **Install dependencies** (platform-specific)
2. **Build library** (`scons`)
3. **Build tests** (`scons test`)
4. **Start Xvfb** (Linux only: `Xvfb :99 -screen 0 1920x1080x24 &`)
5. **Run unit tests** (all platforms, headless-safe)
6. **Run integration tests** (with display; skip Wayland on CI)
7. **Run sanitizer builds** (debug mode)
8. **Upload test results** as CI artifacts

---

## 9. Manual Test Checklist

These tests require a real desktop session and human verification.

### Visual verification

- [ ] Screenshot saved to PNG matches visible screen content
- [ ] Streaming preview shows real-time screen updates
- [ ] Region capture crops to exact specified coordinates
- [ ] Window capture tracks window movement
- [ ] Multi-monitor capture selects correct monitor
- [ ] Retina/HiDPI screenshots have correct pixel density

### Wayland-specific (cannot automate in CI)

- [ ] Portal picker dialog appears on `start()`
- [ ] User can select monitor or window in picker
- [ ] Stream continues after picker closes
- [ ] Restarting capture shows picker again

### Platform transitions

- [ ] X11 session: library selects X11 backend
- [ ] Wayland session: library selects Wayland backend
- [ ] XWayland apps: captured correctly under Wayland
- [ ] RDP session (Windows): GDI fallback activates
- [ ] macOS screen recording denied: meaningful error message

---

## 10. Test Priority & Phasing

### Phase 1 — Foundation (implement first)
- Unit tests: types, queue, color conversion
- Lifecycle tests
- Build `scons test` target
- ASan/TSan in CI

### Phase 2 — Integration
- Screenshot tests (Xvfb on Linux CI)
- Enumeration tests
- Error handling tests
- Permission tests

### Phase 3 — Streaming
- Streaming start/stop tests
- Pause/resume tests
- Concurrent instance tests
- Rapid lifecycle stress tests

### Phase 4 — Performance & Polish
- Benchmark suite
- Platform-specific edge cases
- Memory leak validation (Valgrind)
- Full manual test pass on each platform
