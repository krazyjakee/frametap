#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <frametap/types.h>

#include <type_traits>
#include <utility>

using namespace frametap;

// --- Section 2.1: Data types ---

TEST_CASE("Rect default values", "[unit][types]") {
  Rect r;
  CHECK(r.x == 0);
  CHECK(r.y == 0);
  CHECK(r.width == 0);
  CHECK(r.height == 0);
}

TEST_CASE("Rect stores values", "[unit][types]") {
  Rect r{10, 20, 1920, 1080};
  CHECK(r.x == 10);
  CHECK(r.y == 20);
  CHECK(r.width == 1920);
  CHECK(r.height == 1080);
}

TEST_CASE("Monitor fields", "[unit][types]") {
  Monitor m;
  m.id = 1;
  m.name = "Primary";
  m.x = 0;
  m.y = 0;
  m.width = 2560;
  m.height = 1440;
  m.scale = 2.0f;

  CHECK(m.id == 1);
  CHECK(m.name == "Primary");
  CHECK(m.x == 0);
  CHECK(m.y == 0);
  CHECK(m.width == 2560);
  CHECK(m.height == 1440);
  CHECK(m.scale == 2.0f);
}

TEST_CASE("Monitor default values", "[unit][types]") {
  Monitor m;
  CHECK(m.id == 0);
  CHECK(m.name.empty());
  CHECK(m.x == 0);
  CHECK(m.y == 0);
  CHECK(m.width == 0);
  CHECK(m.height == 0);
  CHECK(m.scale == 1.0f);
}

TEST_CASE("Window fields", "[unit][types]") {
  Window w;
  w.id = 0xDEADBEEF;
  w.name = "Test Window";
  w.x = 100;
  w.y = 200;
  w.width = 800;
  w.height = 600;

  CHECK(w.id == 0xDEADBEEF);
  CHECK(w.name == "Test Window");
  CHECK(w.x == 100);
  CHECK(w.y == 200);
  CHECK(w.width == 800);
  CHECK(w.height == 600);
}

TEST_CASE("Window default values", "[unit][types]") {
  Window w;
  CHECK(w.id == 0);
  CHECK(w.name.empty());
  CHECK(w.x == 0);
  CHECK(w.y == 0);
  CHECK(w.width == 0);
  CHECK(w.height == 0);
}

TEST_CASE("ImageData empty", "[unit][types]") {
  ImageData img;
  CHECK(img.data.empty());
  CHECK(img.width == 0);
  CHECK(img.height == 0);
  CHECK(img.pixels().empty());
}

TEST_CASE("ImageData stores pixel data", "[unit][types]") {
  ImageData img;
  img.width = 2;
  img.height = 2;
  img.data = {
      255, 0, 0,   255, // pixel (0,0) red
      0,   255, 0, 255, // pixel (1,0) green
      0,   0, 255, 255, // pixel (0,1) blue
      255, 255, 0, 255, // pixel (1,1) yellow
  };

  CHECK(img.width == 2);
  CHECK(img.height == 2);
  CHECK(img.data.size() == 16);
  CHECK(img.pixels().size() == 16);
}

TEST_CASE("ImageData move", "[unit][types]") {
  ImageData src;
  src.width = 100;
  src.height = 100;
  src.data.resize(100 * 100 * 4, 42);

  ImageData dst = std::move(src);

  CHECK(dst.width == 100);
  CHECK(dst.height == 100);
  CHECK(dst.data.size() == 100 * 100 * 4);
  CHECK(dst.data[0] == 42);

  // Source should be empty after move
  CHECK(src.data.empty());
}

TEST_CASE("Frame duration", "[unit][types]") {
  Frame f;
  CHECK(f.duration_ms == 0);

  f.duration_ms = 16.67;
  CHECK(f.duration_ms == Catch::Approx(16.67));
}

TEST_CASE("Frame holds ImageData", "[unit][types]") {
  Frame f;
  f.image.width = 1920;
  f.image.height = 1080;
  f.image.data.resize(1920 * 1080 * 4);
  f.duration_ms = 33.33;

  CHECK(f.image.width == 1920);
  CHECK(f.image.height == 1080);
  CHECK(f.image.data.size() == 1920 * 1080 * 4);
  CHECK(f.duration_ms == Catch::Approx(33.33));
}

TEST_CASE("PermissionStatus values", "[unit][types]") {
  CHECK(PermissionStatus::ok != PermissionStatus::warning);
  CHECK(PermissionStatus::ok != PermissionStatus::error);
  CHECK(PermissionStatus::warning != PermissionStatus::error);

  // Verify they are usable in switch/comparison
  PermissionStatus s = PermissionStatus::ok;
  CHECK(s == PermissionStatus::ok);
}

TEST_CASE("PermissionCheck defaults", "[unit][types]") {
  PermissionCheck pc;
  CHECK(pc.status == PermissionStatus::ok);
  CHECK(pc.summary.empty());
  CHECK(pc.details.empty());
}

TEST_CASE("PermissionCheck stores data", "[unit][types]") {
  PermissionCheck pc;
  pc.status = PermissionStatus::error;
  pc.summary = "No display server found";
  pc.details = {"Install X11", "Or install Wayland"};

  CHECK(pc.status == PermissionStatus::error);
  CHECK(pc.summary == "No display server found");
  CHECK(pc.details.size() == 2);
  CHECK(pc.details[0] == "Install X11");
}
