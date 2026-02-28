# Frametap

Cross-platform screen capture for **macOS**, **Linux**, and **Windows**.

Frametap lets you take screenshots of your entire screen, a specific monitor, a window, or a custom region. It works as a standalone command-line tool or as a C++ library you can embed in your own apps.

## Install

### macOS (Homebrew)

```bash
brew tap krazyjakee/tap
brew install frametap
```

### Ubuntu / Debian (.deb)

Download the `.deb` from the [Releases page](https://github.com/krazyjakee/frametap/releases) and install:

```bash
sudo apt install ./frametap_*.deb
```

### Linux (quick install)

```bash
curl -fsSL https://raw.githubusercontent.com/krazyjakee/frametap/main/install.sh | sh
```

### Arch Linux (AUR)

```bash
yay -S frametap-bin
```

### Windows (Scoop)

```powershell
scoop bucket add frametap https://github.com/krazyjakee/scoop-frametap
scoop install frametap
```

### Windows (winget)

```powershell
winget install krazyjakee.frametap
```

### Download binaries

Grab the latest release for your platform from the [Releases page](https://github.com/krazyjakee/frametap/releases).

## Quick Start

### Take a Screenshot

Run the `frametap` command from a terminal:

```bash
# List available monitors
frametap --list-monitors

# Capture a monitor by ID
frametap --monitor 1

# Capture a window by ID
frametap --window 12345

# Capture a region (x,y,width,height)
frametap --region 0,0,1920,1080

# Save to a custom file
frametap --monitor 1 -o desktop.bmp

# Interactive mode (menu-driven)
frametap --interactive
```

Run `frametap --help` to see all options.

#### CLI Reference

```
Capture modes (mutually exclusive):
  --monitor <id>                   Capture a monitor by ID
  --window <id>                    Capture a window by ID
  --region <x>,<y>,<w>,<h>        Capture a screen region
  --interactive                    Interactive mode (menu-driven)

Options:
  -o, --output <file>              Output file (default: screenshot.bmp)
  --list-monitors                  List available monitors and exit
  --list-windows                   List available windows and exit
  --check-permissions              Check capture permissions and exit
  -v, --version                    Show version and exit
  -h, --help                       Show this help
```

### Platform Permissions

**macOS** — The first time you run Frametap, macOS will ask you to grant Screen Recording permission. Go to **System Settings > Privacy & Security > Screen Recording** and enable it for your terminal app.

**Linux (Wayland)** — A system dialog will pop up asking you to pick which screen or window to share. This is normal — Wayland requires it for security.

**Linux (X11)** — No extra setup needed.

**Windows** — No extra setup needed. If you're on Remote Desktop, Frametap will automatically use a compatible capture method.

## Platform Support

| Platform | Status |
|---|---|
| macOS 12.3+ | Fully supported |
| macOS 10.15+ | Screenshots only (older fallback) |
| Linux (Wayland) | Fully supported — GNOME, KDE, Sway, Hyprland |
| Linux (X11) | Fully supported |
| Windows 10+ | Fully supported |
| Windows 8+ | Screenshots only (GDI fallback) |

Linux automatically detects Wayland vs X11 at runtime.

---

## Building from Source

If you'd rather build Frametap yourself instead of downloading a release, you'll need C++20 and [SCons](https://scons.org/).

```bash
# Build the library
scons

# Build the CLI tool
scons cli
```

This produces `libframetap.a` (or `frametap.lib` on Windows) and `cli/frametap`.

### Dependencies

**macOS** — Just Xcode command-line tools (`xcode-select --install`).

**Linux** — Install the development libraries for your distro:

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

For Wayland, you also need a portal backend for your compositor:

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

**Windows** — MSVC with C++20 support. No extra libraries needed.

---

## Using the C++ Library

Frametap also works as a static C++ library for embedding screen capture in your own applications.

### Quick Example

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
// image.data = RGBA pixel buffer
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

### Integration

Link the static library into your project:

```python
# In your SConstruct
env.Append(CPPPATH=['path/to/frametap/include'])
env.Append(LIBPATH=['path/to/frametap/lib'])
env.Append(LIBS=['frametap'])
# + platform frameworks/libraries as needed
```

### API Reference

#### Free Functions

| Function | Description |
|---|---|
| `get_monitors()` | List connected monitors with geometry and DPI scale |
| `get_windows()` | List visible windows (empty on Wayland — use portal picker) |
| `check_permissions()` | Diagnose platform permission/dependency issues |

#### `frametap::FrameTap`

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

#### Types

| Type | Fields |
|---|---|
| `Rect` | `double x, y, width, height` |
| `Monitor` | `int id, x, y, width, height; std::string name; float scale` |
| `Window` | `uint64_t id; std::string name; int x, y, width, height` |
| `ImageData` | `std::vector<uint8_t> data; size_t width, height` |
| `Frame` | `ImageData image; double duration_ms` |
| `PermissionCheck` | `PermissionStatus status; std::string summary; std::vector<std::string> details` |
| `ThreadSafeQueue<T>` | Thread-safe queue for passing frames between threads |

#### Exceptions

All errors throw `frametap::CaptureError` (inherits `std::runtime_error`) with platform-specific messages and actionable advice.

## License

MIT — see [LICENSE](LICENSE).
