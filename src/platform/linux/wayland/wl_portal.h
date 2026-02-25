#pragma once

#include <cstdint>
#include <string>

// Forward declaration — avoid requiring sd-bus header in dependents
struct sd_bus;

namespace frametap::internal {

// Result of opening a portal ScreenCast session.
// Contains the PipeWire FD and node ID needed to connect a stream.
struct PortalSession {
  int pw_fd = -1;
  uint32_t pw_node = 0;
  std::string session_handle;
  // M2: Keep the D-Bus connection alive for the session lifetime.
  // The portal session is invalidated when the bus connection closes.
  sd_bus *bus = nullptr;
};

// Opens a ScreenCast portal session via xdg-desktop-portal.
//
// Flow: CreateSession -> SelectSources -> Start -> OpenPipeWireRemote
//
// The Start call shows an interactive picker dialog to the user.
// If capture_window is true, SelectSources requests window capture;
// otherwise it requests monitor capture.
//
// Throws CaptureError on failure (no portal, user cancelled, D-Bus error).
PortalSession open_screencast_session(bool capture_window = false);

// Closes the portal session, releases its file descriptor and D-Bus connection.
void close_screencast_session(PortalSession &session);

// Takes a one-shot screenshot via the org.freedesktop.portal.Screenshot portal.
// Returns the path to a temporary file containing the screenshot image (PNG).
// Throws CaptureError on failure.
std::string portal_screenshot();

} // namespace frametap::internal
