#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace frametap {

struct Rect {
  double x = 0;
  double y = 0;
  double width = 0;
  double height = 0;
};

struct Monitor {
  int id = 0;
  std::string name;
  int x = 0, y = 0;
  int width = 0, height = 0;
  float scale = 1.0f;
};

struct Window {
  uint64_t id = 0;
  std::string name;
  int x = 0, y = 0;
  int width = 0, height = 0;
};

struct ImageData {
  std::vector<uint8_t> data;
  size_t width = 0;
  size_t height = 0;

  [[nodiscard]] std::span<const uint8_t> pixels() const {
    return {data.data(), data.size()};
  }
};

struct Frame {
  ImageData image;
  double duration_ms = 0;
};

// --- Permission diagnostics ---

enum class PermissionStatus {
  ok,      // Capture should work
  warning, // Might work, but something is suboptimal
  error,   // Capture will fail
};

struct PermissionCheck {
  PermissionStatus status = PermissionStatus::ok;
  std::string summary;              // One-line description
  std::vector<std::string> details; // Actionable advice per issue
};

} // namespace frametap
