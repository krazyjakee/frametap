#include "linux_backend.h"
#include "wayland/wl_backend.h"
#include "x11/x11_backend.h"

#include <X11/Xlib.h>

#include <cstdlib>
#include <string>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace frametap::internal {

namespace {

bool has_wayland() { return std::getenv("WAYLAND_DISPLAY") != nullptr; }
bool has_x11() { return std::getenv("DISPLAY") != nullptr; }

} // anonymous namespace

// ---------------------------------------------------------------------------
// Factory functions (top-level, called by frametap.cpp via backend.h)
// ---------------------------------------------------------------------------

std::unique_ptr<Backend> make_backend() { return make_linux_backend(); }

std::unique_ptr<Backend> make_backend(Rect region) {
  return make_linux_backend(region);
}

std::unique_ptr<Backend> make_backend(Monitor monitor) {
  return make_linux_backend(monitor);
}

std::unique_ptr<Backend> make_backend(Window window) {
  return make_linux_backend(window);
}

// ---------------------------------------------------------------------------
// Linux dispatch — prefer Wayland when both are available
// ---------------------------------------------------------------------------

static constexpr const char *NO_DISPLAY_MSG =
    "No display server found. Set $WAYLAND_DISPLAY or $DISPLAY. "
    "Run from within a graphical session (GNOME, KDE, Sway, X11, etc.).";

std::unique_ptr<Backend> make_linux_backend() {
  if (has_wayland())
    return std::make_unique<WaylandBackend>();
  if (has_x11())
    return std::make_unique<X11Backend>();
  throw CaptureError(NO_DISPLAY_MSG);
}

std::unique_ptr<Backend> make_linux_backend(Rect region) {
  if (has_wayland())
    return std::make_unique<WaylandBackend>(region);
  if (has_x11())
    return std::make_unique<X11Backend>(region);
  throw CaptureError(NO_DISPLAY_MSG);
}

std::unique_ptr<Backend> make_linux_backend(Monitor monitor) {
  if (has_wayland())
    return std::make_unique<WaylandBackend>(monitor);
  if (has_x11())
    return std::make_unique<X11Backend>(monitor);
  throw CaptureError(NO_DISPLAY_MSG);
}

std::unique_ptr<Backend> make_linux_backend(Window window) {
  if (has_wayland())
    return std::make_unique<WaylandBackend>(window);
  if (has_x11())
    return std::make_unique<X11Backend>(window);
  throw CaptureError(NO_DISPLAY_MSG);
}

// ---------------------------------------------------------------------------
// Enumeration — dispatch to active backend
// ---------------------------------------------------------------------------

std::vector<Monitor> enumerate_monitors() {
  if (has_wayland())
    return wl_enumerate_monitors();
  if (has_x11())
    return x11_enumerate_monitors();
  return {};
}

std::vector<Window> enumerate_windows() {
  if (has_wayland())
    return wl_enumerate_windows();
  if (has_x11())
    return x11_enumerate_windows();
  return {};
}

// ---------------------------------------------------------------------------
// Permission diagnostics
// ---------------------------------------------------------------------------

namespace {

bool can_connect_x11() {
  Display *dpy = XOpenDisplay(nullptr);
  if (dpy) {
    XCloseDisplay(dpy);
    return true;
  }
  return false;
}

// Execute a command with execvp (no shell involved), return true if exit 0.
bool exec_check(const char *file, const char *const argv[]) {
  pid_t pid = fork();
  if (pid < 0)
    return false;
  if (pid == 0) {
    // Child: redirect stdout/stderr to /dev/null
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    execvp(file, const_cast<char *const *>(argv));
    _exit(127); // exec failed
  }
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool check_dbus_portal() {
  const char *const argv[] = {
      "busctl",       "--user",  "--no-pager",
      "introspect",   "org.freedesktop.portal.Desktop",
      "/org/freedesktop/portal/desktop",
      "org.freedesktop.portal.ScreenCast", nullptr};
  return exec_check("busctl", argv);
}

bool check_pipewire_running() {
  const char *const argv[] = {"pidof", "pipewire", nullptr};
  return exec_check("pidof", argv);
}

} // anonymous namespace

PermissionCheck check_platform_permissions() {
  PermissionCheck result;
  result.status = PermissionStatus::ok;

  bool wayland = has_wayland();
  bool x11 = has_x11();

  if (!wayland && !x11) {
    result.status = PermissionStatus::error;
    result.summary = "No display server detected";
    result.details.push_back(
        "Neither $WAYLAND_DISPLAY nor $DISPLAY is set.");
    result.details.push_back(
        "Run from within a graphical session (GNOME, KDE, Sway, X11, etc.).");
    return result;
  }

  if (wayland) {
    result.summary = "Wayland session detected";

    if (!check_pipewire_running()) {
      result.status = PermissionStatus::error;
      result.details.push_back(
          "PipeWire is not running. Screen capture requires PipeWire.");
      result.details.push_back(
          "Install and start PipeWire: sudo apt install pipewire && "
          "systemctl --user start pipewire");
    }

    if (!check_dbus_portal()) {
      result.status = PermissionStatus::error;
      result.details.push_back(
          "xdg-desktop-portal ScreenCast interface not available.");
      result.details.push_back(
          "Install xdg-desktop-portal and your compositor's portal backend:");
      result.details.push_back(
          "  GNOME: sudo apt install xdg-desktop-portal-gnome");
      result.details.push_back(
          "  KDE:   sudo apt install xdg-desktop-portal-kde");
      result.details.push_back(
          "  Sway/wlroots: sudo apt install xdg-desktop-portal-wlr");
      result.details.push_back(
          "  Hyprland: install xdg-desktop-portal-hyprland");
    }

    if (result.details.empty())
      result.details.push_back("Wayland + PipeWire + portal ready.");
  } else {
    result.summary = "X11 session detected";

    if (!can_connect_x11()) {
      result.status = PermissionStatus::error;
      result.details.push_back(
          "Cannot connect to X11 display. Check $DISPLAY and X11 auth.");
    } else {
      result.details.push_back("X11 connection OK.");
    }
  }

  return result;
}

} // namespace frametap::internal
