#include "x11_backend.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xinerama.h>

#include <cstring>
#include <string>

namespace frametap::internal {

std::vector<frametap::Monitor> x11_enumerate_monitors() {
  std::vector<frametap::Monitor> result;

  Display *dpy = XOpenDisplay(nullptr);
  if (!dpy)
    return result;

  int event_base, error_base;
  if (XineramaQueryExtension(dpy, &event_base, &error_base) &&
      XineramaIsActive(dpy)) {
    int count = 0;
    XineramaScreenInfo *screens = XineramaQueryScreens(dpy, &count);
    if (screens) {
      for (int i = 0; i < count; i++) {
        Monitor m;
        m.id = screens[i].screen_number;
        m.name = "Screen " + std::to_string(i);
        m.x = screens[i].x_org;
        m.y = screens[i].y_org;
        m.width = screens[i].width;
        m.height = screens[i].height;
        m.scale = 1.0f;
        result.push_back(std::move(m));
      }
      XFree(screens);
    }
  } else {
    // Fallback: single screen
    Monitor m;
    m.id = 0;
    m.name = "Default";
    m.x = 0;
    m.y = 0;
    m.width = DisplayWidth(dpy, DefaultScreen(dpy));
    m.height = DisplayHeight(dpy, DefaultScreen(dpy));
    m.scale = 1.0f;
    result.push_back(std::move(m));
  }

  XCloseDisplay(dpy);
  return result;
}

std::vector<frametap::Window> x11_enumerate_windows() {
  std::vector<frametap::Window> result;

  Display *dpy = XOpenDisplay(nullptr);
  if (!dpy)
    return result;

  ::Window root = DefaultRootWindow(dpy);

  Atom net_client_list = XInternAtom(dpy, "_NET_CLIENT_LIST", True);
  if (net_client_list == None) {
    XCloseDisplay(dpy);
    return result;
  }

  Atom actual_type;
  int actual_format;
  unsigned long nitems, bytes_after;
  unsigned char *data = nullptr;

  if (XGetWindowProperty(dpy, root, net_client_list, 0, ~0L, False, XA_WINDOW,
                         &actual_type, &actual_format, &nitems, &bytes_after,
                         &data) == Success &&
      data) {
    auto *windows = reinterpret_cast<::Window *>(data);

    for (unsigned long i = 0; i < nitems; i++) {
      XWindowAttributes attrs;
      if (!XGetWindowAttributes(dpy, windows[i], &attrs))
        continue;
      if (attrs.map_state != IsViewable)
        continue;

      // Try _NET_WM_NAME (UTF-8) first
      std::string name;
      Atom net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", True);
      Atom utf8_string = XInternAtom(dpy, "UTF8_STRING", True);
      if (net_wm_name != None && utf8_string != None) {
        Atom type;
        int fmt;
        unsigned long n, after;
        unsigned char *prop = nullptr;
        if (XGetWindowProperty(dpy, windows[i], net_wm_name, 0, 256, False,
                               utf8_string, &type, &fmt, &n, &after,
                               &prop) == Success &&
            prop) {
          name = std::string(reinterpret_cast<char *>(prop), n);
          XFree(prop);
        }
      }

      // Fallback to XGetWMName
      if (name.empty()) {
        XTextProperty text_prop;
        if (XGetWMName(dpy, windows[i], &text_prop) && text_prop.value) {
          name = reinterpret_cast<char *>(text_prop.value);
          XFree(text_prop.value);
        }
      }

      if (name.empty())
        continue;

      frametap::Window w;
      w.id = static_cast<uint64_t>(windows[i]);
      w.name = std::move(name);
      w.x = attrs.x;
      w.y = attrs.y;
      w.width = attrs.width;
      w.height = attrs.height;
      result.push_back(std::move(w));
    }

    XFree(data);
  }

  XCloseDisplay(dpy);
  return result;
}

} // namespace frametap::internal
