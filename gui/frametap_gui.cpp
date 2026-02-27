#include <frametap/frametap.h>
#include <frametap/queue.h>

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
  bool streaming = false;

  GLuint texture = 0;
  size_t tex_w = 0, tex_h = 0;

  frametap::ImageData last_frame;
  std::string status;
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

static void stop_capture(AppState &s) {
  if (s.tap) {
    s.tap->stop();
    s.tap.reset();
  }
  // Drain any remaining frames
  while (s.frame_queue.try_pop().has_value()) {
  }
  s.streaming = false;
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
    s.streaming = true;
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
    s.streaming = true;
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
  ImGui::SameLine();
  if (s.streaming && ImGui::Button("Stop")) {
    stop_capture(s);
    s.status = "Stopped";
  }
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
