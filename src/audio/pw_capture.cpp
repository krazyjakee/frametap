#include "audio/pw_capture.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/string.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace frametap::audio {

// Per-app registry/link bookkeeping. The application's PID lives only in a
// node's full info props, so we bind each Stream/Output/Audio node to read it,
// track every output port, and link the matching app's ports to ours.
struct BoundNode {
  PwCapture *self = nullptr;
  uint32_t node_id = 0;
  spa_hook hook{};
  pw_proxy *proxy = nullptr;
};

struct PwCapture::AppState {
  pw_registry *registry = nullptr;
  spa_hook registry_listener{};
  uint32_t our_node_id = SPA_ID_INVALID;

  // node-local port index -> global port id
  std::map<uint32_t, uint32_t> our_in_ports;
  // node id -> (port index -> global port id), for every output node seen
  std::unordered_map<uint32_t, std::map<uint32_t, uint32_t>> out_ports;
  std::unordered_map<uint32_t, bool> matched; // app node -> matched our PID
  std::unordered_map<uint32_t, uint32_t> linked; // app node -> #channels linked
  std::vector<BoundNode *> bound;
  std::vector<pw_proxy *> links;
};

namespace {

void process_trampoline(void *data) {
  static_cast<PwCapture *>(data)->on_process();
}
void param_changed_trampoline(void *data, uint32_t id,
                              const struct spa_pod *param) {
  static_cast<PwCapture *>(data)->on_param_changed(id, param);
}
const struct pw_stream_events kStreamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .param_changed = param_changed_trampoline,
    .process = process_trampoline,
};

void registry_global_trampoline(void *data, uint32_t id, uint32_t /*perm*/,
                                const char *type, uint32_t /*version*/,
                                const struct spa_dict *props) {
  static_cast<PwCapture *>(data)->on_registry_global(id, type, props);
}
const struct pw_registry_events kRegistryEvents = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = registry_global_trampoline,
};

void node_info_trampoline(void *data, const struct pw_node_info *info) {
  auto *bn = static_cast<BoundNode *>(data);
  bn->self->on_node_info(bn->node_id, info ? info->props : nullptr);
}
const struct pw_node_events kNodeEvents = {
    .version = PW_VERSION_NODE_EVENTS,
    .info = node_info_trampoline,
};

// Parent PID from /proc/<pid>/stat (field 4, after the ")"). 0 if unknown.
uint64_t ppid_of(uint64_t pid) {
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/%llu/stat",
                static_cast<unsigned long long>(pid));
  FILE *f = std::fopen(path, "r");
  if (!f)
    return 0;
  std::string s;
  int c;
  while ((c = std::fgetc(f)) != EOF)
    s.push_back(static_cast<char>(c));
  std::fclose(f);
  auto rp = s.rfind(')');
  if (rp == std::string::npos)
    return 0;
  // After ") ": state, then ppid.
  unsigned long long ppid = 0;
  char state = 0;
  if (std::sscanf(s.c_str() + rp + 1, " %c %llu", &state, &ppid) == 2)
    return ppid;
  return 0;
}

// True if `pid` is `target`, or one is an ancestor of the other.
bool pid_related(uint64_t pid, uint64_t target) {
  if (pid == 0 || target == 0)
    return false;
  if (pid == target)
    return true;
  for (uint64_t p = ppid_of(pid), i = 0; p > 1 && i < 24; p = ppid_of(p), ++i)
    if (p == target)
      return true;
  for (uint64_t p = ppid_of(target), i = 0; p > 1 && i < 24;
       p = ppid_of(p), ++i)
    if (p == pid)
      return true;
  return false;
}

} // namespace

PwCapture::~PwCapture() { stop(); }

void PwCapture::start(PcmSink sink, uint64_t target_pid) {
  sink_ = std::move(sink);
  target_pid_ = target_pid;

  pw_init(nullptr, nullptr);
  loop_ = pw_thread_loop_new("frametap-audio", nullptr);
  if (!loop_)
    throw std::runtime_error("audio: pw_thread_loop_new failed");

  pw_thread_loop_lock(loop_);

  context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
  if (!context_) {
    pw_thread_loop_unlock(loop_);
    throw std::runtime_error("audio: pw_context_new failed");
  }
  core_ = pw_context_connect(context_, nullptr, 0);
  if (!core_) {
    pw_thread_loop_unlock(loop_);
    throw std::runtime_error("audio: pw_context_connect failed "
                             "(is PipeWire running?)");
  }

  const bool per_app = target_pid_ != 0;

  pw_properties *props;
  if (per_app)
    props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY,
                              "Capture", PW_KEY_MEDIA_ROLE, "Music", nullptr);
  else
    props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY,
                              "Capture", PW_KEY_MEDIA_ROLE, "Music",
                              PW_KEY_STREAM_CAPTURE_SINK, "true", nullptr);

  stream_ = pw_stream_new(core_, "frametap-audio", props);
  if (!stream_) {
    pw_thread_loop_unlock(loop_);
    throw std::runtime_error("audio: pw_stream_new failed");
  }
  static const struct pw_stream_events events = kStreamEvents;
  auto *hook = new spa_hook();
  listener_ = hook;
  pw_stream_add_listener(stream_, hook, &events, this);

  uint8_t buffer[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  struct spa_audio_info_raw info = {};
  info.format = SPA_AUDIO_FORMAT_F32;
  info.rate = static_cast<uint32_t>(rate_);
  info.channels = static_cast<uint32_t>(channels_);
  const struct spa_pod *params[1];
  params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

  // Per-app capture links manually, so don't autoconnect to the default.
  int flags = PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS;
  if (!per_app)
    flags |= PW_STREAM_FLAG_AUTOCONNECT;

  int res = pw_stream_connect(stream_, PW_DIRECTION_INPUT, PW_ID_ANY,
                              static_cast<pw_stream_flags>(flags), params, 1);
  pw_thread_loop_unlock(loop_);
  if (res < 0)
    throw std::runtime_error("audio: pw_stream_connect failed");

  if (pw_thread_loop_start(loop_) < 0)
    throw std::runtime_error("audio: pw_thread_loop_start failed");

  if (per_app) {
    app_ = new AppState();
    // The loop is running now, so the node gets an id; then watch the
    // registry for the target app's ports and link them to ours.
    for (int i = 0; i < 250 && app_->our_node_id == SPA_ID_INVALID; ++i) {
      pw_thread_loop_lock(loop_);
      uint32_t nid = pw_stream_get_node_id(stream_);
      pw_thread_loop_unlock(loop_);
      if (nid != SPA_ID_INVALID) {
        app_->our_node_id = nid;
        break;
      }
      struct timespec ts = {0, 2 * 1000 * 1000}; // 2 ms
      nanosleep(&ts, nullptr);
    }
    pw_thread_loop_lock(loop_);
    app_->registry = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
    if (app_->registry)
      pw_registry_add_listener(app_->registry, &app_->registry_listener,
                               &kRegistryEvents, this);
    pw_thread_loop_unlock(loop_);
  }
}

void PwCapture::on_registry_global(uint32_t id, const char *type,
                                   const void *props_dict) {
  if (!app_ || !type)
    return;
  const auto *props = static_cast<const struct spa_dict *>(props_dict);

  auto get = [&](const char *k) -> const char * {
    return props ? spa_dict_lookup(props, k) : nullptr;
  };

  if (spa_streq(type, PW_TYPE_INTERFACE_Node)) {
    const char *cls = get(PW_KEY_MEDIA_CLASS);
    if (!cls || std::string(cls) != "Stream/Output/Audio")
      return;
    // The PID isn't in the registry global props (pulse clients only expose
    // the daemon's pid), so bind the node to read its full info props.
    auto *bn = new BoundNode{this, id, {}, nullptr};
    bn->proxy = static_cast<pw_proxy *>(pw_registry_bind(
        app_->registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
    if (!bn->proxy) {
      delete bn;
      return;
    }
    app_->bound.push_back(bn);
    pw_node_add_listener(reinterpret_cast<pw_node *>(bn->proxy), &bn->hook,
                         &kNodeEvents, bn);
    return;
  }

  if (spa_streq(type, PW_TYPE_INTERFACE_Port)) {
    const char *node = get(PW_KEY_NODE_ID);
    const char *dir = get(PW_KEY_PORT_DIRECTION);
    const char *pidx = get(PW_KEY_PORT_ID);
    if (!node || !dir)
      return;
    uint32_t node_id = static_cast<uint32_t>(std::strtoul(node, nullptr, 10));
    uint32_t port_index =
        pidx ? static_cast<uint32_t>(std::strtoul(pidx, nullptr, 10)) : 0;

    if (std::string(dir) == "in" && node_id == app_->our_node_id)
      app_->our_in_ports[port_index] = id;
    else if (std::string(dir) == "out")
      app_->out_ports[node_id][port_index] = id;

    try_link();
  }
}

void PwCapture::on_node_info(uint32_t node_id, const void *props_dict) {
  if (!app_ || !props_dict)
    return;
  const auto *props = static_cast<const struct spa_dict *>(props_dict);
  const char *pid = spa_dict_lookup(props, PW_KEY_APP_PROCESS_ID);
  if (!pid)
    return;
  if (pid_related(std::strtoull(pid, nullptr, 10), target_pid_)) {
    app_->matched[node_id] = true;
    try_link();
  }
}

// Link every matched app node's output ports to our input ports, by channel
// order, creating each link only once.
void PwCapture::try_link() {
  if (app_->our_in_ports.empty())
    return;
  for (auto &[node_id, matched] : app_->matched) {
    if (!matched)
      continue;
    auto oit_map = app_->out_ports.find(node_id);
    if (oit_map == app_->out_ports.end() || oit_map->second.empty())
      continue;
    uint32_t &done = app_->linked[node_id];
    auto oit = oit_map->second.begin();
    auto iit = app_->our_in_ports.begin();
    for (uint32_t k = 0; k < done && oit != oit_map->second.end(); ++k)
      ++oit, ++iit;
    for (; oit != oit_map->second.end() && iit != app_->our_in_ports.end();
         ++oit, ++iit, ++done) {
      pw_properties *lp = pw_properties_new(nullptr, nullptr);
      pw_properties_setf(lp, PW_KEY_LINK_OUTPUT_PORT, "%u", oit->second);
      pw_properties_setf(lp, PW_KEY_LINK_INPUT_PORT, "%u", iit->second);
      auto *link = static_cast<pw_proxy *>(pw_core_create_object(
          core_, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK,
          &lp->dict, 0));
      pw_properties_free(lp);
      if (link)
        app_->links.push_back(link);
    }
  }
}

void PwCapture::stop() {
  if (loop_)
    pw_thread_loop_stop(loop_);
  if (app_) {
    for (auto *link : app_->links)
      pw_proxy_destroy(link);
    for (auto *bn : app_->bound) {
      pw_proxy_destroy(bn->proxy);
      delete bn;
    }
    if (app_->registry)
      pw_proxy_destroy(reinterpret_cast<pw_proxy *>(app_->registry));
  }
  if (stream_) {
    pw_stream_destroy(stream_);
    stream_ = nullptr;
  }
  if (listener_) {
    delete static_cast<spa_hook *>(listener_);
    listener_ = nullptr;
  }
  if (core_) {
    pw_core_disconnect(core_);
    core_ = nullptr;
  }
  if (context_) {
    pw_context_destroy(context_);
    context_ = nullptr;
  }
  if (loop_) {
    pw_thread_loop_destroy(loop_);
    loop_ = nullptr;
  }
  if (app_) {
    delete app_;
    app_ = nullptr;
  }
}

void PwCapture::on_param_changed(uint32_t id, const void *param) {
  if (id != SPA_PARAM_Format || !param)
    return;
  struct spa_audio_info_raw info = {};
  if (spa_format_audio_raw_parse(static_cast<const struct spa_pod *>(param),
                                 &info) < 0)
    return;
  if (info.rate)
    rate_ = static_cast<int>(info.rate);
  if (info.channels)
    channels_ = static_cast<int>(info.channels);
}

void PwCapture::on_process() {
  struct pw_buffer *b = pw_stream_dequeue_buffer(stream_);
  if (!b)
    return;
  struct spa_buffer *buf = b->buffer;
  if (buf->datas[0].data && sink_) {
    const auto *pcm = static_cast<const float *>(buf->datas[0].data);
    const uint32_t bytes = buf->datas[0].chunk->size;
    const uint32_t stride = static_cast<uint32_t>(channels_) * sizeof(float);
    if (stride > 0)
      sink_(pcm, bytes / stride);
  }
  pw_stream_queue_buffer(stream_, b);
}

} // namespace frametap::audio
