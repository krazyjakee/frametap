# Frametap

A lightweight C++20 screen capture library for macOS, Linux, and Windows.

Frametap provides:
- Single-frame screenshots (RGBA)
- Live streaming frame capture with callbacks
- Monitor and window enumeration
- Region, monitor, and window capture
- Pause/resume streaming
- Platform permission diagnostics

## Platform Support

| Platform | Backend | Notes |
|---|---|---|
| macOS 12.3+ | ScreenCaptureKit | Preferred path |
| macOS 10.15+ | CoreGraphics | Fallback for screenshots |
| Linux (Wayland) | xdg-desktop-portal + PipeWire | GNOME, KDE, Sway, Hyprland |
| Linux (X11) | X11 + XShm | Any X11 session |
| Windows 10+ | DXGI Desktop Duplication | Preferred path |
| Windows 8+ | GDI | Fallback (RDP, older GPUs) |

Linux automatically detects whether to use Wayland or X11 at runtime.

## Building

Requires C++20 and [SCons](https://scons.org/).

```bash
# Build the static library
scons

# Build the example
scons example
```

This produces `libframetap.a` (or `frametap.lib` on Windows) and optionally `examples/capture_example`.

### Platform Dependencies

**macOS** — No extra dependencies. Xcode command line tools provide everything.

**Linux** — Install development libraries:

```bash
# Debian / Ubuntu
sudo apt install \
  libx11-dev libxext-dev libxfixes-dev libxinerama-dev \
  libpipewire-0.3-dev libsystemd-dev libwayland-dev \
  pkg-config

# Fedora
sudo dnf install \
  libX11-devel libXext-devel libXfixes-devel libXinerama-devel \
  pipewire-devel systemd-devel wayland-devel \
  pkg-config

# Arch
sudo pacman -S \
  libx11 libxext libxfixes libxinerama \
  pipewire libsystemd wayland \
  pkg-config
```

For Wayland screen capture, you also need a portal backend for your compositor:

```bash
# GNOME
sudo apt install xdg-desktop-portal-gnome
# KDE
sudo apt install xdg-desktop-portal-kde
# Sway / wlroots
sudo apt install xdg-desktop-portal-wlr
# Hyprland
# install xdg-desktop-portal-hyprland from your distro or AUR
```

**Windows** — MSVC with C++20 support. No extra libraries needed (DXGI, D3D11, GDI are part of the Windows SDK).

## Usage

```cpp
#include <frametap/frametap.h>
#include <frametap/queue.h>

// Check platform readiness
auto perms = frametap::check_permissions();
if (perms.status == frametap::PermissionStatus::error) {
    // perms.details has actionable advice
}

// Take a screenshot
frametap::FrameTap tap;
auto image = tap.screenshot();
// image.data = std::vector<uint8_t> in RGBA format
// image.width, image.height = dimensions

// Stream frames
frametap::ThreadSafeQueue<frametap::Frame> queue;
tap.on_frame([&](const frametap::Frame& frame) {
    queue.push(frame);
});
tap.start_async();
// ... process frames from queue ...
tap.pause();   // pause streaming
tap.resume();  // resume streaming
tap.stop();

// Enumerate monitors and windows
auto monitors = frametap::get_monitors();
auto windows  = frametap::get_windows();
```

## Integration

Frametap builds as a static library. To integrate into your project:

```python
# In your SConstruct
env.Append(CPPPATH=['path/to/frametap/include'])
env.Append(LIBPATH=['path/to/frametap'])
env.Append(LIBS=['frametap'])
# + platform frameworks/libraries as needed
```

## API

### Free Functions

| Function | Description |
|---|---|
| `get_monitors()` | List connected monitors with geometry and DPI scale |
| `get_windows()` | List visible windows (empty on Wayland — use portal picker) |
| `check_permissions()` | Diagnose platform permission/dependency issues |

### `frametap::FrameTap`

| Method | Description |
|---|---|
| `FrameTap()` | Capture the full primary monitor |
| `FrameTap(Rect region)` | Capture a specific region |
| `FrameTap(Monitor monitor)` | Capture a specific monitor |
| `FrameTap(Window window)` | Capture a specific window |
| `set_region(Rect)` | Change the capture region |
| `on_frame(callback)` | Set frame callback (call before `start`) |
| `start()` | Start capture, blocks until `stop()` |
| `start_async()` | Start capture, returns immediately |
| `stop()` | Stop capture |
| `pause()` | Pause streaming (no frames delivered) |
| `resume()` | Resume streaming |
| `is_paused()` | Check if capture is paused |
| `screenshot()` | Grab a single frame |
| `screenshot(Rect)` | Grab a single frame of a region |

### Types

| Type | Fields |
|---|---|
| `Rect` | `double x, y, width, height` |
| `Monitor` | `int id, x, y, width, height; std::string name; float scale` |
| `Window` | `uint64_t id; std::string name; int x, y, width, height` |
| `ImageData` | `std::vector<uint8_t> data; size_t width, height` |
| `Frame` | `ImageData image; double duration_ms` |
| `PermissionCheck` | `PermissionStatus status; std::string summary; std::vector<std::string> details` |
| `ThreadSafeQueue<T>` | Thread-safe queue for passing frames between threads |

### Exceptions

All errors throw `frametap::CaptureError` (inherits `std::runtime_error`) with platform-specific messages and actionable advice.

## Platform Notes

**Wayland**: Screen capture requires user interaction on first use — the portal shows a picker dialog where the user selects the monitor or window to share. Window enumeration (`get_windows()`) returns an empty list because Wayland restricts window access; the user selects windows interactively through the portal.

**macOS**: Screen recording permission must be granted in System Settings > Privacy & Security > Screen Recording. Use `check_permissions()` to detect this before attempting capture.

**Windows**: DXGI Desktop Duplication may be unavailable in Remote Desktop sessions or on older GPUs. Frametap automatically falls back to GDI in these cases.

## License

See [LICENSE](LICENSE).
