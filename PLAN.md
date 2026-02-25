# Frametap — Cross-Platform Plan

## Goal

Add full Windows, Linux, and macOS support while preserving frametap's lightweight,
modern C++20 API. Use `screen_capture_lite` (/home/krazy/Documents/GitHub/screen_capture_lite) as architectural reference for platform
backends, but keep frametap's simpler design: no builder chains, no chunked diff
engine, no per-monitor threading by default.

---

## Current State

- macOS-only (ScreenCaptureKit + CoreGraphics)
- Clean public API: `FrameTap`, `screenshot()`, `start()`/`start_async()`/`stop()`, `on_frame(callback)`
- PImpl pattern with layered architecture (C++ → C bridge → Objective-C)
- RGBA output, move-only semantics, `CaptureError` exceptions
- SCons build, static library output

## Reference: screen_capture_lite

| Platform | Monitor Capture | Window Capture | Mouse Capture |
|----------|----------------|----------------|---------------|
| Windows  | DXGI Desktop Duplication (DX11), GDI fallback | GDI BitBlt | GetCursorInfo + DrawIcon |
| Linux    | X11 + XShm | X11 + XShm on window | XFixes cursor image |
| macOS    | AVCaptureScreenInput | CGWindowListCreateImage | CGEvent + NSCursor |

Key things to borrow:
- DXGI desktop duplication strategy for Windows
- XShm shared-memory capture for Linux
- Monitor/window enumeration per platform
- Mouse capture approach per platform

Key things to **not** borrow:
- Builder-pattern config API (keep frametap's direct class)
- Per-monitor thread spawning (single capture thread is fine for v1)
- 256x256 chunk diff engine (overkill for our use case)
- BGRA output format (keep RGBA conversion at the backend boundary)
- C++17 atomics threading model (use C++20 `std::jthread`, `std::stop_token`)

## Reference: libscreencapture-wayland (DafabHoid)

Architecture for Wayland capture using the compositor-agnostic portal+PipeWire path:

```
xdg-desktop-portal (D-Bus)  →  PipeWire stream  →  Raw frames (SHM or DMA-BUF)
      (permission)               (transport)            (pixel data)
```

| Layer | Protocol/API | Purpose |
|-------|-------------|---------|
| Permission | `org.freedesktop.portal.ScreenCast` (D-Bus) | User authorization dialog |
| Transport | PipeWire (`libpipewire-0.3`) | Receive video frames from compositor |
| Buffers | SPA MemPtr / MemFd | Memory-mapped pixel data (fallback) |
| Buffers | DRM PRIME / DMA-BUF | Zero-copy GPU buffer sharing |

Portal flow (3 D-Bus calls + FD handoff):
1. `CreateSession` — creates a portal session with random token
2. `SelectSources` — configures source type (monitor/window) and cursor mode
3. `Start` — shows user a picker dialog; returns PipeWire stream info
4. `OpenPipeWireRemote` — returns a file descriptor to the PipeWire instance

PipeWire stream setup:
1. Connect via `pw_context_connect_fd()` using the portal's FD
2. Negotiate format with **two** parameter sets: DMA-BUF first (with DRM modifier
   list for Intel/AMD tiling), SHM fallback second
3. PipeWire allocates a buffer pool (16 buffers); frames must be returned via
   `pw_stream_queue_buffer()` when the consumer is done
4. Main loop runs on a dedicated thread; events delivered through a
   mutex-protected queue signaled by `eventfd` (pollable)

Key things to borrow:
- Portal + PipeWire approach (works on GNOME, KDE, wlroots, Hyprland — any
  compositor with xdg-desktop-portal support)
- Dual-path format negotiation: DMA-BUF with modifier list, SHM fallback
- `eventfd`-based pollable event delivery — integrates cleanly into our
  capture thread's event loop
- RAII frame lifetime: `onFrameDone` callback returns buffers to PipeWire's
  limited pool (critical for backpressure)
- Explicit DRM modifier negotiation (linear, Intel tiling, AMD tiling formats)

Key things to **not** borrow:
- DMA-BUF passthrough to encoder (we always read pixels to CPU RGBA at the
  backend boundary; zero-copy GPU output is a non-goal)
- C API wrapper (frametap is C++ only)
- sdbus-c++ library dependency (use `sd-bus` from libsystemd directly, or
  raw `libdbus-1`, to stay lean)
- Builder-pattern output classes

---

## Target API

The public API stays almost identical. New additions in **bold**.

```cpp
namespace frametap {

// --- Query functions (new) ---
std::vector<Monitor> get_monitors();
std::vector<Window>  get_windows();

struct Monitor {
    int         id;
    std::string name;
    int         x, y;           // offset in virtual screen
    int         width, height;  // pixel dimensions
    float       scale;          // DPI scale factor
};

struct Window {
    uint64_t    id;             // platform window handle
    std::string name;
    int         x, y;
    int         width, height;
};

// --- Existing types (unchanged) ---
struct Rect   { double x, y, width, height; };
struct ImageData { std::vector<uint8_t> data; size_t width, height; ... };
struct Frame  { ImageData image; double duration_ms; };
using FrameCallback = std::function<void(const Frame&)>;

// --- Capture (mostly unchanged) ---
class FrameTap {
public:
    FrameTap();                     // full primary monitor
    FrameTap(Rect region);          // region of primary monitor
    FrameTap(Monitor monitor);      // specific monitor
    FrameTap(Window window);        // specific window

    void set_region(Rect);
    void on_frame(FrameCallback);
    void start();                   // blocking
    void start_async();             // non-blocking
    void stop();

    ImageData screenshot();
    ImageData screenshot(Rect region);

    // new
    void pause();
    void resume();
    bool is_paused() const;
};

} // namespace frametap
```

Changes from current API:
- `Monitor` and `Window` structs + enumeration functions
- Constructor overloads to capture a specific monitor or window
- `pause()` / `resume()` / `is_paused()`
- Everything else stays the same

---

## Target Directory Layout

```
frametap/
├── SConstruct                          # cross-platform build (matches godot-livekit)
├── include/frametap/
│   ├── frametap.h                      # public API (unchanged location)
│   ├── types.h                         # Rect, ImageData, Frame, Monitor, Window
│   └── queue.h                         # ThreadSafeQueue (unchanged)
├── src/
│   ├── frametap.cpp                    # PImpl dispatch (unchanged pattern)
│   ├── backend.h                       # internal: abstract backend interface
│   ├── platform/
│   │   ├── macos/
│   │   │   ├── macos_backend.h
│   │   │   ├── macos_backend.mm        # ScreenCaptureKit streaming
│   │   │   ├── macos_screenshot.mm     # CoreGraphics single-frame
│   │   │   └── macos_enumerate.mm      # monitor/window enumeration
│   │   ├── linux/
│   │   │   ├── linux_backend.h         # dispatches to X11 or Wayland at runtime
│   │   │   ├── linux_backend.cpp       # runtime display server detection + dispatch
│   │   │   ├── x11/
│   │   │   │   ├── x11_backend.h
│   │   │   │   ├── x11_backend.cpp     # X11 + XShm streaming
│   │   │   │   ├── x11_screenshot.cpp  # X11 single-frame
│   │   │   │   └── x11_enumerate.cpp   # Xinerama + _NET_CLIENT_LIST
│   │   │   └── wayland/
│   │   │       ├── wl_backend.h
│   │   │       ├── wl_backend.cpp      # PipeWire stream → RGBA frames
│   │   │       ├── wl_portal.cpp       # xdg-desktop-portal D-Bus session
│   │   │       ├── wl_screenshot.cpp   # portal single-frame (Screenshot portal)
│   │   │       └── wl_enumerate.cpp    # portal-based monitor/window enumeration
│   │   └── windows/
│   │       ├── windows_backend.h
│   │       ├── windows_backend.cpp     # DXGI desktop duplication streaming
│   │       ├── windows_screenshot.cpp  # DXGI/GDI single-frame
│   │       └── windows_enumerate.cpp   # DXGI + EnumWindows
│   └── util/
│       └── color.h                     # BGRA→RGBA conversion (shared)
├── examples/
│   └── capture_example.cpp
├── stb/                                # vendored (unchanged)
└── native/                             # legacy macOS C bridge (remove after migration)
```

---

## Backend Interface

Each platform implements this internal interface. It is not public.

```cpp
// src/backend.h
#pragma once
#include <frametap/types.h>
#include <memory>
#include <functional>

namespace frametap::internal {

class Backend {
public:
    virtual ~Backend() = default;

    // One-shot capture
    virtual ImageData screenshot(Rect region) = 0;

    // Streaming
    virtual void start(FrameCallback cb) = 0;
    virtual void stop() = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;

    // Configuration
    virtual void set_region(Rect region) = 0;
};

// Factory — returns platform-appropriate backend
std::unique_ptr<Backend> make_backend();                  // primary monitor
std::unique_ptr<Backend> make_backend(Rect region);
std::unique_ptr<Backend> make_backend(Monitor monitor);
std::unique_ptr<Backend> make_backend(Window window);

// Enumeration — platform-specific
std::vector<Monitor> enumerate_monitors();
std::vector<Window>  enumerate_windows();

} // namespace frametap::internal
```

`frametap.cpp` becomes a thin wrapper:

```cpp
FrameTap::FrameTap() : impl_(internal::make_backend()) {}
void FrameTap::start_async() { impl_->start(callback_); }
ImageData FrameTap::screenshot() { return impl_->screenshot({}); }
// etc.
```

---

## Phases

### Phase 0 — Build System + Abstraction Layer

1. Extend `SConstruct` with platform detection and conditional source selection.
   - SCons only (matches godot-livekit's build system for seamless integration).
   - C++20 required. MSVC, Clang, GCC.
   - Static library target (`libframetap.a` / `frametap.lib`).
   - Platform conditionals via `sys.platform` / SCons `Platform()`.
   - Platform libs: see table below.
2. Create `src/backend.h` (abstract interface above).
3. Add `Monitor`, `Window` structs and enumeration declarations to `types.h`.
4. Add `src/util/color.h` with inline BGRA→RGBA conversion.

| Platform | Link Libraries |
|----------|---------------|
| macOS    | Foundation, AppKit, ScreenCaptureKit, CoreGraphics, CoreMedia, CoreVideo |
| Linux (X11) | X11, Xext (XShm), Xfixes, Xinerama |
| Linux (Wayland) | pipewire-0.3, dbus-1 (or libsystemd for sd-bus), drm |
| Linux (both) | All of the above; runtime detection picks the active path |
| Windows  | dxgi.lib, d3d11.lib, dwmapi.lib |

**godot-livekit integration**: The consuming project adds frametap via:
```python
env.Append(CPPPATH=['path/to/frametap/include'])
env.Append(LIBPATH=['path/to/frametap'])
env.Append(LIBS=['frametap'])
# + platform frameworks (already listed in godot-livekit for its own deps)
```

### Phase 1 — Migrate macOS Backend

1. Move current `native/screencap.{h,m}` + `src/macos_capture.{h,cpp}` into
   `src/platform/macos/`.
2. Wrap into a `MacOSBackend : Backend` class.
   - Streaming (macOS 12.3+): ScreenCaptureKit path (current).
   - Streaming (macOS 10.15+): AVCaptureScreenInput fallback
     (same approach as screen_capture_lite's NSFrameProcessor).
   - Screenshot: CGDisplayCreateImageForRect (monitors),
     CGWindowListCreateImage (windows).
   - Window capture: CGWindowListCreateImage with
     kCGWindowListOptionIncludingWindow per window.
   - Enumeration: CGGetActiveDisplayList (monitors),
     CGWindowListCopyWindowInfo (windows).
3. Merge the C bridge and the C++ wrapper into a single Objective-C++ file
   (`macos_backend.mm`) to eliminate the indirection layer.
4. Verify everything still works via `capture_example`.
5. Remove `native/` directory.

### Phase 2 — Linux Backend (X11 + Wayland)

Runtime detection: `linux_backend.cpp` checks `$WAYLAND_DISPLAY` (set by all
Wayland compositors) vs `$DISPLAY` (set by X11 and XWayland) and instantiates
the appropriate sub-backend. If both are set, prefer Wayland.

#### Phase 2a — X11 Sub-backend

1. `x11/x11_enumerate.cpp`:
   - Monitors via Xinerama (`XineramaQueryScreens`).
   - Windows via `_NET_CLIENT_LIST` + `XGetWindowAttributes` + `XGetWMName`.
2. `x11/x11_screenshot.cpp`:
   - Monitor: `XShmGetImage` on root window, crop to region.
   - Window: `XShmGetImage` on target window handle.
   - BGRA→RGBA conversion.
3. `x11/x11_backend.cpp`:
   - Capture thread (`std::jthread`) polls at configurable interval.
   - Uses XShm for zero-copy reads from X server.
   - Invokes `FrameCallback` with converted RGBA data.
   - `stop()` via `std::stop_token`.
4. Test on X11.

#### Phase 2b — Wayland Sub-backend (portal + PipeWire)

Inspired by [libscreencapture-wayland](https://github.com/DafabHoid/libscreencapture-wayland).
Uses the compositor-agnostic `xdg-desktop-portal` + PipeWire path, which works
on GNOME, KDE, Sway, Hyprland, and any compositor shipping a portal backend.

1. `wayland/wl_portal.cpp` — D-Bus portal session:
   - Connect to session bus via `sd-bus` (from libsystemd) or `libdbus-1`.
   - Implement the portal flow:
     `CreateSession` → `SelectSources(type=MONITOR|WINDOW)` → `Start` →
     `OpenPipeWireRemote`.
   - Each portal call returns a response signal on a temporary object path;
     block with `std::promise`/`std::future` until the response arrives.
   - Result: a PipeWire FD + node ID for the selected source.
   - On failure (no portal, user cancelled), throw `CaptureError` with
     actionable message ("Install xdg-desktop-portal-gnome/kde/wlr").

2. `wayland/wl_backend.cpp` — PipeWire stream → RGBA frames:
   - Connect to PipeWire using `pw_context_connect_fd()` with the portal's FD.
   - Create a `pw_stream` targeting the portal's node ID.
   - Negotiate format with **two** parameter sets (priority order):
     a. DMA-BUF: BGRA/RGBA + DRM modifiers (LINEAR, Intel tiling, AMD tiling).
     b. SHM: BGRA/RGBA via MemPtr/MemFd (fallback).
   - PipeWire selects one; detect which via presence of modifier property in
     `streamParamChanged` callback.
   - Request `SPA_META_Header` (timestamps) and `SPA_META_Cursor` (optional).
   - Configure buffer pool: request 16 buffers, enable `MAP_BUFFERS` flag.
   - Run `pw_main_loop` on a dedicated `std::jthread`.
   - In `processFrame` callback:
     - **SHM path** (`SPA_DATA_MemPtr`/`SPA_DATA_MemFd`): read pixel pointer
       + stride directly from `spa_data`. Convert BGRA→RGBA in-place or via
       `color.h` helper.
     - **DMA-BUF path** (`SPA_DATA_DmaBuf`): `mmap()` the DMA-BUF fd (after
       DMA_BUF_IOCTL_SYNC), read pixels, convert BGRA→RGBA, then `munmap()`.
       Alternatively, use `gbm_bo_import` + `gbm_bo_map` for GPU-tiled formats.
     - Wrap converted RGBA data in a `Frame` and invoke `FrameCallback`.
     - Return the PipeWire buffer via `pw_stream_queue_buffer()` in an RAII
       guard (critical — PipeWire's pool is limited).
   - `stop()`: signal `pw_main_loop_quit()` from the jthread's stop callback.
   - `pause()` / `resume()`: `pw_stream_set_active(false/true)`.

3. `wayland/wl_screenshot.cpp` — single-frame capture:
   - Use the `org.freedesktop.portal.Screenshot` portal for one-shot capture.
   - Or: open a PipeWire stream, grab one frame, close. The portal Screenshot
     API is simpler but returns a file URI (need to read the temp file).
   - Convert to RGBA `ImageData` and return.

4. `wayland/wl_enumerate.cpp` — monitor/window enumeration:
   - Portal limitation: the portal API does **not** expose monitor/window lists
     before the user picks one in the dialog. The `SelectSources` + `Start`
     flow is interactive.
   - Workaround for monitor enumeration: use `wl_output` via a minimal Wayland
     client connection (connect to `$WAYLAND_DISPLAY`, bind `wl_output` globals,
     read geometry/mode events). This doesn't require any special permissions.
   - Window enumeration on Wayland is intentionally restricted. Return empty
     `std::vector<Window>` and document that window capture requires the portal
     picker dialog (user selects the window interactively).
   - Alternative: if `org.freedesktop.portal.RemoteDesktop` is available,
     it may expose source info — but this is compositor-dependent and not
     guaranteed.

5. `linux_backend.cpp` — runtime dispatch:
   ```cpp
   std::unique_ptr<Backend> make_backend() {
       if (getenv("WAYLAND_DISPLAY"))
           return std::make_unique<WaylandBackend>();
       if (getenv("DISPLAY"))
           return std::make_unique<X11Backend>();
       throw CaptureError("No display server found");
   }
   ```

6. Test on:
   - GNOME (Mutter) — most common, uses `xdg-desktop-portal-gnome`
   - KDE (KWin) — `xdg-desktop-portal-kde`
   - Sway — `xdg-desktop-portal-wlr`
   - X11 fallback — verify XWayland path still works

### Phase 3 — Windows Backend

1. `windows_enumerate.cpp`:
   - Monitors via DXGI factory (`EnumAdapters` → `EnumOutputs`), plus
     `GetDeviceCaps` for DPI/scaling.
   - Windows via `EnumWindows` + `IsWindowVisible` filtering, DWM extended
     frame bounds for accurate sizing.
2. `windows_screenshot.cpp`:
   - Monitor: DXGI AcquireNextFrame + staging texture copy.
   - Window: GDI `BitBlt` on window DC + `GetDIBits`.
   - Fallback (monitor): GDI if DXGI unavailable.
   - BGRA→RGBA conversion.
3. `windows_backend.cpp`:
   - Monitor streaming: DXGI desktop duplication, frame timeout via
     `AcquireNextFrame(timeout_ms)`.
   - Window streaming: GDI polling loop (DXGI only captures desktops,
     not individual windows).
   - GDI fallback if DXGI init fails (e.g. RDP sessions, older GPUs,
     Windows 8).
   - `stop()` via `std::stop_token`.
4. Handle desktop switching for RDP scenarios (SetThreadDesktop).

### Phase 4 — Polish

1. Unified example that works on all three platforms.
2. Permission helpers:
   - macOS: detect screen recording permission, guide user.
   - Windows: detect admin/desktop access.
   - Linux/X11: check X11 connectivity.
   - Linux/Wayland: check portal availability (`busctl --user list | grep portal`),
     check PipeWire is running, provide actionable error messages
     ("Install xdg-desktop-portal and your compositor's portal backend").
3. Error messages: platform-specific `CaptureError` details.
4. CI: GitHub Actions matrix build (macOS, Ubuntu with X11+Wayland, Windows).
5. Update README with cross-platform instructions (including Wayland dep install).

---

## Non-Goals (v1)

Items marked **low-hanging fruit** are cheap to add post-v1.

- **Mouse cursor capture** — **Low-hanging fruit.** Already done on macOS
  (`setShowsCursor:YES`), Wayland (`cursor_mode = EMBEDDED`), and Windows
  (DXGI includes cursor). Only X11 is missing: use `XFixesGetCursorImage()`
  to composite cursor onto the captured frame. Single-platform fix.
- **Frame differencing / dirty-region callbacks** — **Low-hanging fruit
  (Windows only).** DXGI Desktop Duplication already exposes
  `GetFrameDirtyRects()` and `GetFrameMoveRects()` — just not surfaced in the
  API. Could be exposed as an optional callback with minimal work. Other
  platforms would need manual pixel comparison (not low-hanging).
- Audio capture — Substantial. No audio infrastructure exists. Needs new types,
  new callbacks, and per-platform capture APIs (ScreenCaptureKit audio streams,
  PipeWire audio nodes, WASAPI). Whole new subsystem.
- Per-monitor threading — Substantial. Architectural change: currently 1
  instance = 1 thread = 1 source. Multi-monitor would need a coordinator,
  synchronized delivery, and a different public API.
- GPU texture output — Substantial. Current API returns
  `std::vector<uint8_t>`. Exposing GPU textures means Metal/D3D11/EGL handle
  types in the public API, plus the consumer needs a matching GPU context.
- DMA-BUF zero-copy passthrough — Moderate. PipeWire already sends DMA-BUFs,
  but we `memcpy` to CPU in `on_process()`. Passing the fd through requires
  new `Frame` variants and the consumer must handle DMA-BUF import. API-level
  change.
- Wayland window enumeration (portal API is interactive; users pick via dialog)
- Compositor-specific protocols (wlr-screencopy, wlr-export-dmabuf — portal is sufficient)

---

## Decisions

1. **Build system**: SCons only. Matches godot-livekit's build system for
   direct integration as a static library. No CMake.
2. **Minimum OS versions**: Wider compatibility.
   - macOS 10.15+ (AVCaptureScreenInput fallback), prefer 12.3+
     (ScreenCaptureKit) at runtime.
   - Windows 8+ (DXGI 1.1 + GDI fallback), prefer 10+ (DXGI 1.2).
   - Linux/X11: any X11.
   - Linux/Wayland: PipeWire 0.3+, xdg-desktop-portal with a compositor
     backend (gnome/kde/wlr/hyprland).
3. **Window capture**: Ships in v1 on all platforms. Monitor and window
   capture both land together.
4. **Linux display server strategy**: Support both X11 and Wayland natively.
   Runtime detection via `$WAYLAND_DISPLAY` / `$DISPLAY` environment variables.
   Wayland preferred when both are available (avoids XWayland overhead and
   captures at native resolution with correct scaling).
5. **Wayland capture method**: xdg-desktop-portal + PipeWire (the standardized
   path). No compositor-specific protocols (wlr-screencopy, etc.). This means:
   - Works on all major compositors (GNOME, KDE, Sway, Hyprland)
   - Requires user interaction on first capture (portal picker dialog)
   - No programmatic window enumeration (portal is interactive)
   - Session can be persisted to avoid repeated dialogs (portal `Restore` token)
6. **D-Bus library**: Use `sd-bus` from libsystemd (already present on most
   Linux systems) rather than pulling in sdbus-c++ as a submodule. Keeps the
   dependency footprint small. Fallback: `libdbus-1` if libsystemd is unavailable.
7. **PipeWire buffer strategy**: Always read pixels to CPU memory (even DMA-BUF
   frames). Frametap's contract is RGBA CPU buffers. For DMA-BUF frames, use
   `mmap` or `gbm_bo_map` to read, convert BGRA→RGBA, and return the buffer to
   PipeWire immediately. This keeps the PipeWire buffer pool from stalling.
