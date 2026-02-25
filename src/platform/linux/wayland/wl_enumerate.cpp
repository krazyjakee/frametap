#include "wl_backend.h"

#include <wayland-client.h>

#include <cstring>
#include <string>
#include <vector>

namespace frametap::internal {

namespace {

struct OutputInfo {
  int32_t x = 0, y = 0;
  int32_t width = 0, height = 0;
  int32_t scale = 1;
  std::string make;
  std::string model;
  bool has_mode = false;
};

struct EnumState {
  std::vector<OutputInfo> outputs;
  int next_id = 0;
};

void output_geometry(void *data, struct wl_output * /*output*/, int32_t x,
                     int32_t y, int32_t /*physical_width*/,
                     int32_t /*physical_height*/, int32_t /*subpixel*/,
                     const char *make, const char *model,
                     int32_t /*transform*/) {
  auto *info = static_cast<OutputInfo *>(data);
  info->x = x;
  info->y = y;
  if (make)
    info->make = make;
  if (model)
    info->model = model;
}

void output_mode(void *data, struct wl_output * /*output*/, uint32_t flags,
                 int32_t width, int32_t height, int32_t /*refresh*/) {
  if (flags & WL_OUTPUT_MODE_CURRENT) {
    auto *info = static_cast<OutputInfo *>(data);
    info->width = width;
    info->height = height;
    info->has_mode = true;
  }
}

void output_scale(void *data, struct wl_output * /*output*/, int32_t scale) {
  auto *info = static_cast<OutputInfo *>(data);
  info->scale = scale;
}

void output_done(void * /*data*/, struct wl_output * /*output*/) {
  // All events for this output have been sent
}

void output_name(void * /*data*/, struct wl_output * /*output*/,
                 const char * /*name*/) {}

void output_description(void * /*data*/, struct wl_output * /*output*/,
                        const char * /*description*/) {}

const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                     const char *interface, uint32_t version) {
  auto *state = static_cast<EnumState *>(data);

  if (std::strcmp(interface, wl_output_interface.name) == 0) {
    auto *output = static_cast<struct wl_output *>(
        wl_registry_bind(registry, name, &wl_output_interface,
                         version < 4 ? version : 4));

    state->outputs.emplace_back();
    wl_output_add_listener(output, &output_listener, &state->outputs.back());
  }
}

void registry_global_remove(void * /*data*/, struct wl_registry * /*registry*/,
                            uint32_t /*name*/) {}

const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

} // anonymous namespace

std::vector<frametap::Monitor> wl_enumerate_monitors() {
  std::vector<frametap::Monitor> result;

  struct wl_display *display = wl_display_connect(nullptr);
  if (!display)
    return result;

  EnumState state;
  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, &state);

  // First roundtrip: discover globals (wl_output bindings happen here)
  wl_display_roundtrip(display);
  // Second roundtrip: receive events for bound objects (geometry, mode, scale)
  wl_display_roundtrip(display);

  for (size_t i = 0; i < state.outputs.size(); i++) {
    const auto &info = state.outputs[i];
    if (!info.has_mode)
      continue;

    Monitor m;
    m.id = static_cast<int>(i);
    m.name = info.make.empty() && info.model.empty()
                 ? "Display " + std::to_string(i)
                 : info.make + " " + info.model;
    m.x = info.x;
    m.y = info.y;
    m.width = info.width;
    m.height = info.height;
    m.scale = static_cast<float>(info.scale);
    result.push_back(std::move(m));
  }

  wl_registry_destroy(registry);
  wl_display_disconnect(display);
  return result;
}

std::vector<frametap::Window> wl_enumerate_windows() {
  // Wayland does not expose a window list to unprivileged clients.
  // Window capture is handled interactively via the portal picker dialog.
  return {};
}

} // namespace frametap::internal
