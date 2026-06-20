#include <frametap/frametap.h>
#include <frametap/queue.h>
#ifdef FRAMETAP_GUI_RECORDING
#include <frametap/receiving.h>
#include <frametap/recording.h>
#endif

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "lodepng.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------------

struct AppState {
  std::vector<frametap::Monitor> monitors;
  std::vector<frametap::Window> windows;

  enum class SourceKind { None, Monitor, Window };
  SourceKind selected_kind = SourceKind::None;
  int selected_index = -1;

  std::unique_ptr<frametap::FrameTap> tap;
  frametap::ThreadSafeQueue<frametap::Frame> frame_queue;
  GLuint texture = 0;
  size_t tex_w = 0, tex_h = 0;

  frametap::ImageData last_frame;
  std::string status;

#ifdef FRAMETAP_GUI_RECORDING
  // Recording lives entirely on the main thread: submit() is driven from the
  // frame-queue drain, and start/stop from the UI buttons, so no locking is
  // needed around the recorder.
  std::unique_ptr<frametap::VideoRecorder> recorder;
  bool recording = false;
  std::string record_path;
  frametap::Codec record_codec = frametap::Codec::h264;

  // Network streaming controls.
  bool stream_enabled = false;
  int stream_protocol = 0; // 0=srt, 1=udp, 2=rtmp
  char stream_url[256] = "srt://0.0.0.0:9000?mode=listener";
  bool stream_save_file = true;

  // Network receiving (live preview of an incoming SRT stream). The receiver
  // worker pushes decoded frames into frame_queue, the same path capture uses.
  // Default is caller mode so it connects to a streaming instance using the
  // default listener URL (set the host for a remote sender).
  std::unique_ptr<frametap::StreamReceiver> receiver;
  bool receiving = false;
  char receive_url[256] = "srt://127.0.0.1:9000";
#endif
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void refresh_sources(AppState &s) {
  try {
    s.monitors = frametap::get_monitors();
  } catch (const std::exception &e) {
    s.status = std::string("Monitor enum failed: ") + e.what();
    s.monitors.clear();
  }
  try {
    s.windows = frametap::get_windows();
  } catch (const std::exception &e) {
    s.status = std::string("Window enum failed: ") + e.what();
    s.windows.clear();
  }
}

static void stop_capture(AppState &s); // defined below

#ifdef FRAMETAP_GUI_RECORDING
static frametap::StreamProtocol protocol_from_index(int i) {
  switch (i) {
  case 1:
    return frametap::StreamProtocol::udp_ts;
  case 2:
    return frametap::StreamProtocol::rtmp;
  default:
    return frametap::StreamProtocol::srt;
  }
}

static void start_recording(AppState &s) {
  if (!s.tap) {
    s.status = "Select a source before recording";
    return;
  }
  try {
    frametap::EncoderConfig cfg;
    cfg.codec = s.record_codec;
    // For a window, capture only that app's audio; for a monitor, capture all
    // system audio (pid 0).
    if (s.selected_kind == AppState::SourceKind::Window &&
        s.selected_index >= 0 &&
        s.selected_index < static_cast<int>(s.windows.size()))
      cfg.audio_source_pid = s.windows[s.selected_index].pid;
    if (s.stream_enabled) {
      cfg.stream.enabled = true;
      cfg.stream.protocol = protocol_from_index(s.stream_protocol);
      cfg.stream.url = s.stream_url;
      cfg.stream.also_save_file = s.stream_save_file;
    }
    s.record_path =
        cfg.stream.enabled && !cfg.stream.also_save_file
            ? std::string()
            : frametap::default_recording_path(s.record_codec);
    s.recorder =
        std::make_unique<frametap::VideoRecorder>(s.record_path, cfg);
    s.recording = true;
    if (cfg.stream.enabled)
      s.status = "Streaming to " + cfg.stream.url +
                 (s.record_path.empty() ? "" : " + " + s.record_path) + "...";
    else
      s.status = "Recording to " + s.record_path + "...";
  } catch (const std::exception &e) {
    s.status = std::string("Record start failed: ") + e.what();
    s.recorder.reset();
    s.recording = false;
  }
}

static void stop_recording(AppState &s) {
  if (!s.recorder)
    return;
  try {
    // finish() rethrows any error raised while encoding (e.g. the encoder
    // rejecting the frame size), so a mid-recording failure surfaces here.
    s.recorder->finish();
    auto st = s.recorder->stats();
    const std::string serr = s.recorder->stream_error();
    if (!serr.empty()) {
      s.status = "Stream error: " + serr;
    } else {
      const std::string where =
          s.record_path.empty() ? "stream" : s.record_path;
      s.status = "Saved " + where + " (" +
                 std::to_string(st.frames_encoded) + " frames, " +
                 std::to_string(st.bytes_written / (1024 * 1024)) + " MB)";
    }
  } catch (const std::exception &e) {
    s.status = std::string("Recording failed: ") + e.what();
  }
  s.recorder.reset();
  s.recording = false;
}

static void stop_receiving(AppState &s) {
  if (!s.receiver)
    return;
  s.receiver->stop();
  const std::string err = s.receiver->error();
  const auto st = s.receiver->stats();
  s.receiver.reset();
  s.receiving = false;
  if (!err.empty())
    s.status = "Receive error: " + err;
  else
    s.status = "Receive stopped (" + std::to_string(st.frames_decoded) +
               " frames)";
}

static void start_receiving(AppState &s) {
  // A received stream is its own source; stop any capture/recording first.
  stop_capture(s);
  try {
    frametap::ReceiverConfig cfg;
    cfg.url = s.receive_url;
    cfg.decode = true;
    s.receiver = std::make_unique<frametap::StreamReceiver>(cfg);
    s.receiver->on_frame([&s](const frametap::ImageData &img) {
      frametap::Frame f;
      f.image = img;
      s.frame_queue.push(std::move(f));
    });
    s.receiver->start();
    s.receiving = true;
    s.selected_kind = AppState::SourceKind::None;
    s.selected_index = -1;
    s.status = std::string("Receiving from ") + s.receive_url +
               " (waiting for sender)...";
  } catch (const std::exception &e) {
    s.status = std::string("Receive start failed: ") + e.what();
    s.receiver.reset();
    s.receiving = false;
  }
}
#endif

static void stop_capture(AppState &s) {
#ifdef FRAMETAP_GUI_RECORDING
  // Recording is bound to the active capture; tearing down the source ends it.
  stop_recording(s);
  stop_receiving(s);
#endif
  if (s.tap) {
    s.tap->stop();
    s.tap.reset();
  }
  // Drain any remaining frames
  while (s.frame_queue.try_pop().has_value()) {
  }
}

static void start_capture_monitor(AppState &s, int index) {
  stop_capture(s);
  s.selected_kind = AppState::SourceKind::Monitor;
  s.selected_index = index;
  try {
    s.tap = std::make_unique<frametap::FrameTap>(s.monitors[index]);
    s.tap->on_frame(
        [&s](const frametap::Frame &frame) { s.frame_queue.push(frame); });
    s.tap->start_async();

    s.status = "Capturing: " + s.monitors[index].name;
  } catch (const std::exception &e) {
    s.status = std::string("Capture failed: ") + e.what();
    s.tap.reset();
  }
}

static void start_capture_window(AppState &s, int index) {
  stop_capture(s);
  s.selected_kind = AppState::SourceKind::Window;
  s.selected_index = index;
  try {
    s.tap = std::make_unique<frametap::FrameTap>(s.windows[index]);
    s.tap->on_frame(
        [&s](const frametap::Frame &frame) { s.frame_queue.push(frame); });
    s.tap->start_async();

    s.status = "Capturing: " + s.windows[index].name;
  } catch (const std::exception &e) {
    s.status = std::string("Capture failed: ") + e.what();
    s.tap.reset();
  }
}

static void upload_frame(AppState &s, const frametap::ImageData &img) {
  if (img.data.empty())
    return;
  if (img.width != s.tex_w || img.height != s.tex_h) {
    glBindTexture(GL_TEXTURE_2D, s.texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(img.width),
                 static_cast<GLsizei>(img.height), 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, img.data.data());
    s.tex_w = img.width;
    s.tex_h = img.height;
  } else {
    glBindTexture(GL_TEXTURE_2D, s.texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(img.width),
                    static_cast<GLsizei>(img.height), GL_RGBA,
                    GL_UNSIGNED_BYTE, img.data.data());
  }
}

static void save_png(AppState &s) {
  if (s.last_frame.data.empty()) {
    s.status = "No frame to save";
    return;
  }
  unsigned error = lodepng::encode(
      "screenshot.png", s.last_frame.data,
      static_cast<unsigned>(s.last_frame.width),
      static_cast<unsigned>(s.last_frame.height));
  if (error) {
    s.status =
        std::string("PNG save failed: ") + lodepng_error_text(error);
  } else {
    s.status = "Saved screenshot.png (" +
               std::to_string(s.last_frame.width) + "x" +
               std::to_string(s.last_frame.height) + ")";
  }
}

// ---------------------------------------------------------------------------
// UI drawing
// ---------------------------------------------------------------------------

static void draw_sidebar(AppState &s) {
  ImGui::BeginChild("Sidebar", ImVec2(250, 0), ImGuiChildFlags_Borders);

  // Monitors
  if (!s.monitors.empty()) {
    ImGui::SeparatorText("Monitors");
    for (int i = 0; i < static_cast<int>(s.monitors.size()); ++i) {
      const auto &m = s.monitors[i];
      char label[256];
      std::snprintf(label, sizeof(label), "%s (%dx%d)##mon%d", m.name.c_str(),
                    m.width, m.height, i);
      bool selected = s.selected_kind == AppState::SourceKind::Monitor &&
                      s.selected_index == i;
      if (ImGui::Selectable(label, selected)) {
        start_capture_monitor(s, i);
      }
    }
  }

  // Windows
  if (!s.windows.empty()) {
    ImGui::SeparatorText("Windows");
    for (int i = 0; i < static_cast<int>(s.windows.size()); ++i) {
      const auto &w = s.windows[i];
      char label[256];
      std::snprintf(label, sizeof(label), "%s (%dx%d)##win%d", w.name.c_str(),
                    w.width, w.height, i);
      bool selected = s.selected_kind == AppState::SourceKind::Window &&
                      s.selected_index == i;
      if (ImGui::Selectable(label, selected)) {
        start_capture_window(s, i);
      }
    }
  }

  ImGui::Spacing();
  if (ImGui::Button("Refresh", ImVec2(-1, 0))) {
    refresh_sources(s);
  }

#ifdef FRAMETAP_GUI_RECORDING
  ImGui::Spacing();
  ImGui::SeparatorText("Receive");
  ImGui::SetNextItemWidth(-1);
  ImGui::InputText("##rxurl", s.receive_url, sizeof(s.receive_url));
  if (s.receiving) {
    if (ImGui::Button("Disconnect", ImVec2(-1, 0)))
      stop_receiving(s);
    const bool live = s.receiver && s.receiver->connected();
    ImGui::TextDisabled("%s", live ? "Receiving (connected)"
                                   : "Waiting for sender...");
  } else {
    if (ImGui::Button("Receive Stream", ImVec2(-1, 0)))
      start_receiving(s);
  }
#endif

  ImGui::EndChild();
}

static void draw_preview(AppState &s) {
  ImGui::BeginChild("Preview");

  if (s.tex_w > 0 && s.tex_h > 0) {
    // Compute aspect-ratio-preserving size that fits the available region
    ImVec2 avail = ImGui::GetContentRegionAvail();
    // Reserve space for the button row below the image
    float button_row_height = ImGui::GetFrameHeightWithSpacing() + 4;
    avail.y -= button_row_height;
    if (avail.y < 1)
      avail.y = 1;

    float src_aspect = static_cast<float>(s.tex_w) / static_cast<float>(s.tex_h);
    float dst_aspect = avail.x / avail.y;

    float draw_w, draw_h;
    if (src_aspect > dst_aspect) {
      draw_w = avail.x;
      draw_h = avail.x / src_aspect;
    } else {
      draw_h = avail.y;
      draw_w = avail.y * src_aspect;
    }

    // Center horizontally
    float pad_x = (avail.x - draw_w) * 0.5f;
    if (pad_x > 0)
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad_x);

    ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(s.texture)),
                 ImVec2(draw_w, draw_h));
  } else {
    ImGui::TextDisabled("Select a source to start preview");
  }

  // Bottom row: save button + status
  if (ImGui::Button("Save PNG")) {
    save_png(s);
  }

#ifdef FRAMETAP_GUI_RECORDING
  ImGui::SameLine();
  if (s.recording) {
    if (ImGui::Button("Stop Recording"))
      stop_recording(s);
  } else {
    ImGui::BeginDisabled(!s.tap);
    if (ImGui::Button("Record"))
      start_recording(s);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    const char *codecs[] = {"H.264", "HEVC"};
    int ci = s.record_codec == frametap::Codec::hevc ? 1 : 0;
    if (ImGui::Combo("##codec", &ci, codecs, 2))
      s.record_codec =
          ci == 1 ? frametap::Codec::hevc : frametap::Codec::h264;

    ImGui::SameLine();
    ImGui::Checkbox("Stream", &s.stream_enabled);
    if (s.stream_enabled) {
      ImGui::SameLine();
      ImGui::SetNextItemWidth(90);
      const char *protos[] = {"SRT", "UDP-TS", "RTMP"};
      ImGui::Combo("##proto", &s.stream_protocol, protos, 3);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(240);
      ImGui::InputText("##url", s.stream_url, sizeof(s.stream_url));
      ImGui::SameLine();
      ImGui::Checkbox("Save file too", &s.stream_save_file);
    }
  }
#endif

  ImGui::SameLine();
  ImGui::TextWrapped("%s", s.status.c_str());

  ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

static void glfw_error_callback(int error, const char *description) {
  std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

int main() {
  glfwSetErrorCallback(glfw_error_callback);
  if (!glfwInit())
    return 1;

  // OpenGL 3.2 core profile
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

  GLFWwindow *window = glfwCreateWindow(1280, 720, "Frametap", nullptr, nullptr);
  if (!window) {
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1); // vsync

  // ImGui setup
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  ImGui::StyleColorsDark();

  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init("#version 150");

  // App state
  AppState state;
  glGenTextures(1, &state.texture);
  glBindTexture(GL_TEXTURE_2D, state.texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  refresh_sources(state);

  // Main loop
  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    // Drain frame queue — keep only the latest frame
    {
      std::optional<frametap::Frame> frame;
      while (auto f = state.frame_queue.try_pop()) {
#ifdef FRAMETAP_GUI_RECORDING
        // Encode every frame (don't drop), unlike the preview which keeps only
        // the latest.
        if (state.recorder)
          state.recorder->submit(*f);
#endif
        frame = std::move(f);
      }
      if (frame) {
        state.last_frame = std::move(frame->image);
        upload_frame(state, state.last_frame);
      }
    }

    // Start ImGui frame
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // Full-window ImGui panel
    {
      const ImGuiViewport *vp = ImGui::GetMainViewport();
      ImGui::SetNextWindowPos(vp->WorkPos);
      ImGui::SetNextWindowSize(vp->WorkSize);
      ImGui::Begin("##Main", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                       ImGuiWindowFlags_NoBringToFrontOnFocus);

      draw_sidebar(state);
      ImGui::SameLine();
      draw_preview(state);

      ImGui::End();
    }

    // Render
    ImGui::Render();
    int display_w, display_h;
    glfwGetFramebufferSize(window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window);
  }

  // Cleanup
  stop_capture(state);
  if (state.texture)
    glDeleteTextures(1, &state.texture);

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();

  return 0;
}
