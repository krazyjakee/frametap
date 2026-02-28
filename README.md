# Frametap

Cross-platform screen capture for **macOS**, **Linux**, and **Windows**.

Frametap lets you capture your entire screen, a specific monitor, a window, or a custom region. It ships as a **GUI app**, a **command-line tool**, and a **C++ library** you can embed in your own apps.

## Install

### macOS (Homebrew)

```bash
brew tap krazyjakee/tap
brew install frametap
```

### Ubuntu / Debian (apt)

```bash
curl -fsSL https://krazyjakee.github.io/frametap/apt/frametap.gpg.key | sudo gpg --dearmor -o /usr/share/keyrings/frametap.gpg
echo "deb [signed-by=/usr/share/keyrings/frametap.gpg] https://krazyjakee.github.io/frametap/apt stable main" | sudo tee /etc/apt/sources.list.d/frametap.list
sudo apt update
sudo apt install frametap
```

Or download the `.deb` directly from the [Releases page](https://github.com/krazyjakee/frametap/releases) and install with `sudo apt install ./frametap_*.deb`.

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

Each release includes separate downloads for:
- **GUI** — `frametap_gui` (or `frametap_gui.exe`) — graphical app with live preview
- **CLI** — `frametap` (or `frametap.exe`) — interactive command-line tool
- **Library** — `lib/` and `include/` — static C++ library for developers

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

## GUI

The graphical app gives you a live preview of any monitor or window on your system.

### Usage

Launch the app:

```
./frametap_gui
```

The window has two panels:

- **Sidebar** — Lists all connected monitors and visible windows. Click any source to start capturing it. Hit **Refresh** to re-scan if you plug in a display or open a new window.
- **Preview** — Shows a live, aspect-ratio-preserving view of the selected source. Click **Save PNG** to save the current frame as `screenshot.png` in the working directory.

### Requirements

The GUI requires OpenGL 3.2+ and GLFW. See [Dependencies](#dependencies) for install instructions.

---

## CLI

The command-line tool runs an interactive menu in your terminal.

### Usage

```
./frametap
```

It will:
1. Check that your system has the right permissions for screen capture
2. Present a capture mode menu
3. Save the result as `screenshot.bmp` in the current directory

### Capture Modes

```
Capture mode:
  1) Screen (pick a monitor)
  2) Window (pick a window)
  3) Region (enter coordinates)
```

- **Screen** — Lists all connected monitors with resolution and DPI scale. Enter a number to capture that monitor.
- **Window** — Lists all visible windows with their dimensions. Enter a number to capture that window.
- **Region** — Prompts for `x`, `y`, `width`, and `height` to capture an arbitrary rectangle.

### Output

The CLI saves a 24-bit BMP file named `screenshot.bmp` in the current working directory. The file is overwritten on each run.

---

## Building from Source

If you'd rather build Frametap yourself instead of downloading a release, you'll need C++20 and [SCons](https://scons.org/).

```bash
# Build the library
scons

# Build the CLI tool
scons cli

# Build the GUI app
scons gui
```

This produces:
- `libframetap.a` (or `frametap.lib` on Windows) — static library
- `cli/frametap` — command-line tool
- `gui/frametap_gui` — graphical app

### Dependencies

**macOS** — Xcode command-line tools (`xcode-select --install`). For the GUI, also install GLFW: `brew install glfw`.

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

For the GUI, also install GLFW:

```bash
# Debian / Ubuntu
sudo apt install libglfw3-dev
# Fedora
sudo dnf install glfw-devel
# Arch
sudo pacman -S glfw
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

**Windows** — MSVC with C++20 support. For the GUI, install GLFW via [vcpkg](https://vcpkg.io/): `vcpkg install glfw3:x64-windows-static`.

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

## Support

Hi! I'm krazyjakee, creator and maintainer of Frametap. If you find this project useful, consider sponsoring to help sustain and grow it: more dev time, better docs, more features, and deeper community support.

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/krazyjakee)

Every contribution helps maintain and improve this project and encourages me to make more projects like this!

*This is optional support. The tool remains free and open-source regardless.*

## License

MIT — see [LICENSE](LICENSE).
