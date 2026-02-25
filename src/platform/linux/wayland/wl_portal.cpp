#include "wl_portal.h"
#include <frametap/frametap.h>

#include <systemd/sd-bus.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

namespace frametap::internal {

namespace {

constexpr const char *PORTAL_DEST = "org.freedesktop.portal.Desktop";
constexpr const char *PORTAL_PATH = "/org/freedesktop/portal/desktop";
constexpr const char *SCREENCAST_IFACE = "org.freedesktop.portal.ScreenCast";
constexpr const char *SCREENSHOT_IFACE = "org.freedesktop.portal.Screenshot";
constexpr const char *REQUEST_IFACE = "org.freedesktop.portal.Request";

// Build the expected request object path from the connection's unique name
// and a handle token. The portal uses this so we can subscribe to the
// Response signal before the method returns (avoiding a race).
std::string make_request_path(sd_bus *bus, const std::string &token) {
  const char *name = nullptr;
  sd_bus_get_unique_name(bus, &name);

  std::string sender(name);
  // Replace ':' and '.' with '_'
  for (char &c : sender) {
    if (c == ':' || c == '.')
      c = '_';
  }

  return "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;
}

// Context for waiting on a portal Response signal.
struct ResponseCtx {
  std::atomic<bool> done{false};
  uint32_t status = UINT32_MAX;
  // For CreateSession → session_handle
  std::string session_handle;
  // For Start → PipeWire node ID
  uint32_t pw_node = 0;
};

int on_response(sd_bus_message *msg, void *userdata, sd_bus_error * /*err*/) {
  auto *ctx = static_cast<ResponseCtx *>(userdata);

  // Read response status
  uint32_t status = 0;
  int r = sd_bus_message_read(msg, "u", &status);
  if (r < 0) {
    ctx->status = 99;
    ctx->done.store(true);
    return 0;
  }
  ctx->status = status;

  // Parse the results dict a{sv}
  r = sd_bus_message_enter_container(msg, 'a', "{sv}");
  if (r < 0) {
    ctx->done.store(true);
    return 0;
  }

  while (sd_bus_message_enter_container(msg, 'e', "sv") > 0) {
    const char *key = nullptr;
    sd_bus_message_read(msg, "s", &key);

    if (std::strcmp(key, "session_handle") == 0) {
      sd_bus_message_enter_container(msg, 'v', "s");
      const char *val = nullptr;
      sd_bus_message_read(msg, "s", &val);
      if (val)
        ctx->session_handle = val;
      sd_bus_message_exit_container(msg);
    } else if (std::strcmp(key, "streams") == 0) {
      // streams: v a(ua{sv})
      sd_bus_message_enter_container(msg, 'v', "a(ua{sv})");
      sd_bus_message_enter_container(msg, 'a', "(ua{sv})");
      if (sd_bus_message_enter_container(msg, 'r', "ua{sv}") > 0) {
        uint32_t node_id = 0;
        sd_bus_message_read(msg, "u", &node_id);
        ctx->pw_node = node_id;
        // Skip the properties dict
        sd_bus_message_skip(msg, "a{sv}");
        sd_bus_message_exit_container(msg); // r
      }
      sd_bus_message_exit_container(msg); // a
      sd_bus_message_exit_container(msg); // v
    } else {
      sd_bus_message_skip(msg, "v");
    }

    sd_bus_message_exit_container(msg); // e
  }
  sd_bus_message_exit_container(msg); // a

  ctx->done.store(true);
  return 0;
}

// Wait for a response signal with a timeout.
void wait_for_response(sd_bus *bus, ResponseCtx &ctx, int timeout_secs = 60) {
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::seconds(timeout_secs);

  while (!ctx.done.load()) {
    if (std::chrono::steady_clock::now() >= deadline)
      throw CaptureError("Portal response timed out");

    int r = sd_bus_process(bus, nullptr);
    if (r < 0)
      throw CaptureError("D-Bus process error: " +
                         std::string(strerror(-r)));
    if (r > 0)
      continue; // processed a message, check again

    // Nothing to process — wait for activity (up to 1 second)
    uint64_t usec = 1000000ULL;
    sd_bus_wait(bus, usec);
  }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// ScreenCast portal session
// ---------------------------------------------------------------------------

PortalSession open_screencast_session(bool capture_window) {
  sd_bus *bus = nullptr;
  int r = sd_bus_open_user(&bus);
  if (r < 0)
    throw CaptureError(
        "Failed to open D-Bus session bus: " + std::string(strerror(-r)) +
        ". Is D-Bus running?");

  // Ensure bus is cleaned up on error
  struct BusGuard {
    sd_bus *b;
    bool release = true;
    ~BusGuard() {
      if (release)
        sd_bus_unref(b);
    }
  } bus_guard{bus};

  sd_bus_error error = SD_BUS_ERROR_NULL;
  sd_bus_message *reply = nullptr;

  // ---- Step 1: CreateSession ----
  std::string token1 = "frametap_cs";
  std::string request_path1 = make_request_path(bus, token1);

  ResponseCtx ctx1;
  sd_bus_slot *slot1 = nullptr;
  r = sd_bus_match_signal(bus, &slot1, PORTAL_DEST, request_path1.c_str(),
                          REQUEST_IFACE, "Response", on_response, &ctx1);
  if (r < 0)
    throw CaptureError("Failed to subscribe to portal Response signal");

  r = sd_bus_call_method(bus, PORTAL_DEST, PORTAL_PATH, SCREENCAST_IFACE,
                         "CreateSession", &error, &reply, "a{sv}", 2,
                         "handle_token", "s", token1.c_str(),
                         "session_handle_token", "s", "frametap_session");
  if (r < 0) {
    std::string msg = error.message ? error.message : "Unknown D-Bus error";
    sd_bus_error_free(&error);
    throw CaptureError("CreateSession failed: " + msg +
                       ". Is xdg-desktop-portal installed?");
  }
  sd_bus_message_unref(reply);
  reply = nullptr;

  wait_for_response(bus, ctx1);
  sd_bus_slot_unref(slot1);

  if (ctx1.status != 0)
    throw CaptureError("CreateSession was denied (status=" +
                       std::to_string(ctx1.status) + ")");
  if (ctx1.session_handle.empty())
    throw CaptureError("CreateSession returned empty session handle");

  std::string session_handle = ctx1.session_handle;

  // ---- Step 2: SelectSources ----
  std::string token2 = "frametap_ss";
  std::string request_path2 = make_request_path(bus, token2);

  ResponseCtx ctx2;
  sd_bus_slot *slot2 = nullptr;
  sd_bus_match_signal(bus, &slot2, PORTAL_DEST, request_path2.c_str(),
                      REQUEST_IFACE, "Response", on_response, &ctx2);

  // source types: 1 = MONITOR, 2 = WINDOW, 3 = MONITOR | WINDOW
  uint32_t source_types = capture_window ? 2u : 1u;
  // cursor mode: 2 = EMBEDDED (cursor drawn into frame)
  uint32_t cursor_mode = 2u;

  r = sd_bus_call_method(
      bus, PORTAL_DEST, PORTAL_PATH, SCREENCAST_IFACE, "SelectSources", &error,
      &reply, "oa{sv}", session_handle.c_str(), 3, "handle_token", "s",
      token2.c_str(), "types", "u", source_types, "cursor_mode", "u",
      cursor_mode);
  if (r < 0) {
    std::string msg = error.message ? error.message : "Unknown D-Bus error";
    sd_bus_error_free(&error);
    throw CaptureError("SelectSources failed: " + msg);
  }
  sd_bus_message_unref(reply);
  reply = nullptr;

  wait_for_response(bus, ctx2);
  sd_bus_slot_unref(slot2);

  if (ctx2.status != 0)
    throw CaptureError("SelectSources was denied (status=" +
                       std::to_string(ctx2.status) + ")");

  // ---- Step 3: Start (shows interactive picker dialog) ----
  std::string token3 = "frametap_st";
  std::string request_path3 = make_request_path(bus, token3);

  ResponseCtx ctx3;
  sd_bus_slot *slot3 = nullptr;
  sd_bus_match_signal(bus, &slot3, PORTAL_DEST, request_path3.c_str(),
                      REQUEST_IFACE, "Response", on_response, &ctx3);

  r = sd_bus_call_method(bus, PORTAL_DEST, PORTAL_PATH, SCREENCAST_IFACE,
                         "Start", &error, &reply, "osa{sv}",
                         session_handle.c_str(), "", 1, "handle_token", "s",
                         token3.c_str());
  if (r < 0) {
    std::string msg = error.message ? error.message : "Unknown D-Bus error";
    sd_bus_error_free(&error);
    throw CaptureError("Start failed: " + msg);
  }
  sd_bus_message_unref(reply);
  reply = nullptr;

  // User interaction happens here — give generous timeout
  wait_for_response(bus, ctx3, 120);
  sd_bus_slot_unref(slot3);

  if (ctx3.status != 0)
    throw CaptureError("User cancelled screen capture (status=" +
                       std::to_string(ctx3.status) + ")");
  if (ctx3.pw_node == 0)
    throw CaptureError("Portal returned no PipeWire stream");

  // ---- Step 4: OpenPipeWireRemote ----
  r = sd_bus_call_method(bus, PORTAL_DEST, PORTAL_PATH, SCREENCAST_IFACE,
                         "OpenPipeWireRemote", &error, &reply, "oa{sv}",
                         session_handle.c_str(), 0);
  if (r < 0) {
    std::string msg = error.message ? error.message : "Unknown D-Bus error";
    sd_bus_error_free(&error);
    throw CaptureError("OpenPipeWireRemote failed: " + msg);
  }

  int pw_fd = -1;
  r = sd_bus_message_read(reply, "h", &pw_fd);
  if (r < 0 || pw_fd < 0) {
    sd_bus_message_unref(reply);
    throw CaptureError("Failed to receive PipeWire file descriptor");
  }

  // dup the FD because sd-bus owns the original
  int duped_fd = dup(pw_fd);
  sd_bus_message_unref(reply);

  if (duped_fd < 0)
    throw CaptureError("Failed to duplicate PipeWire FD");

  PortalSession session;
  session.pw_fd = duped_fd;
  session.pw_node = ctx3.pw_node;
  session.session_handle = session_handle;

  // M2: Keep the bus alive — the portal session is only valid as long as
  // the D-Bus connection stays open. Store it in the session for the caller
  // to manage via close_screencast_session().
  bus_guard.release = false;
  session.bus = bus;

  return session;
}

void close_screencast_session(PortalSession &session) {
  if (session.pw_fd >= 0) {
    close(session.pw_fd);
    session.pw_fd = -1;
  }
  session.pw_node = 0;
  session.session_handle.clear();
  // M2: Release the D-Bus connection (invalidates the portal session)
  if (session.bus) {
    sd_bus_unref(session.bus);
    session.bus = nullptr;
  }
}

// ---------------------------------------------------------------------------
// Screenshot portal
// ---------------------------------------------------------------------------

std::string portal_screenshot() {
  sd_bus *bus = nullptr;
  int r = sd_bus_open_user(&bus);
  if (r < 0)
    throw CaptureError("Failed to open D-Bus session bus");

  struct BusGuard {
    sd_bus *b;
    ~BusGuard() { sd_bus_unref(b); }
  } guard{bus};

  std::string token = "frametap_scr";
  std::string request_path = make_request_path(bus, token);

  ResponseCtx ctx;
  sd_bus_slot *slot = nullptr;
  sd_bus_match_signal(bus, &slot, PORTAL_DEST, request_path.c_str(),
                      REQUEST_IFACE, "Response", on_response, &ctx);

  // Extend ResponseCtx to capture the URI — we do it here by parsing
  // the raw message in on_response. But our on_response above doesn't
  // handle "uri". Let's use a dedicated callback:
  struct ScreenshotCtx {
    std::atomic<bool> done{false};
    uint32_t status = UINT32_MAX;
    std::string uri;
  };

  ScreenshotCtx sctx;

  // Unsubscribe the generic handler and use a screenshot-specific one
  sd_bus_slot_unref(slot);
  slot = nullptr;

  auto screenshot_cb = [](sd_bus_message *msg, void *userdata,
                          sd_bus_error *) -> int {
    auto *c = static_cast<ScreenshotCtx *>(userdata);

    uint32_t status = 0;
    sd_bus_message_read(msg, "u", &status);
    c->status = status;

    sd_bus_message_enter_container(msg, 'a', "{sv}");
    while (sd_bus_message_enter_container(msg, 'e', "sv") > 0) {
      const char *key = nullptr;
      sd_bus_message_read(msg, "s", &key);
      if (std::strcmp(key, "uri") == 0) {
        sd_bus_message_enter_container(msg, 'v', "s");
        const char *val = nullptr;
        sd_bus_message_read(msg, "s", &val);
        if (val)
          c->uri = val;
        sd_bus_message_exit_container(msg);
      } else {
        sd_bus_message_skip(msg, "v");
      }
      sd_bus_message_exit_container(msg);
    }
    sd_bus_message_exit_container(msg);

    c->done.store(true);
    return 0;
  };

  r = sd_bus_match_signal(bus, &slot, PORTAL_DEST, request_path.c_str(),
                          REQUEST_IFACE, "Response",
                          screenshot_cb, &sctx);
  if (r < 0)
    throw CaptureError("Failed to subscribe to Screenshot Response");

  sd_bus_error error = SD_BUS_ERROR_NULL;
  sd_bus_message *reply = nullptr;
  r = sd_bus_call_method(bus, PORTAL_DEST, PORTAL_PATH, SCREENSHOT_IFACE,
                         "Screenshot", &error, &reply, "sa{sv}", "", 2,
                         "handle_token", "s", token.c_str(), "interactive",
                         "b", 0);
  if (r < 0) {
    std::string msg = error.message ? error.message : "Unknown error";
    sd_bus_error_free(&error);
    throw CaptureError("Screenshot portal failed: " + msg);
  }
  sd_bus_message_unref(reply);

  // Wait for response
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (!sctx.done.load()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      sd_bus_slot_unref(slot);
      throw CaptureError("Screenshot portal timed out");
    }
    int pr = sd_bus_process(bus, nullptr);
    if (pr < 0)
      break;
    if (pr > 0)
      continue;
    sd_bus_wait(bus, 1000000ULL);
  }
  sd_bus_slot_unref(slot);

  if (sctx.status != 0)
    throw CaptureError("Screenshot was denied (status=" +
                       std::to_string(sctx.status) + ")");
  if (sctx.uri.empty())
    throw CaptureError("Screenshot portal returned no URI");

  // M5: Convert file:// URI to path with validation
  std::string path = sctx.uri;
  const std::string prefix = "file://";
  if (path.substr(0, prefix.size()) == prefix)
    path = path.substr(prefix.size());

  // Validate the path is absolute and doesn't contain path traversal sequences
  if (path.empty() || path[0] != '/')
    throw CaptureError("Screenshot portal returned invalid path: " + path);
  if (path.find("/../") != std::string::npos || path.find("/./") != std::string::npos)
    throw CaptureError("Screenshot portal returned suspicious path: " + path);

  return path;
}

} // namespace frametap::internal
