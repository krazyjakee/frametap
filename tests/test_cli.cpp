#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

// cli_args.h is a header-only parser in cli/
#include "cli_args.h"

using namespace cli;

// Helper to build argv from initializer list.
// parse_args expects char*[] so we use const_cast (safe for read-only parsing).
static Args parse(std::initializer_list<const char *> tokens) {
  std::vector<char *> argv;
  for (auto t : tokens)
    argv.push_back(const_cast<char *>(t));
  return parse_args(static_cast<int>(argv.size()), argv.data());
}

// --- No arguments ---

TEST_CASE("No arguments shows help", "[unit][cli]") {
  auto a = parse({"frametap"});
  CHECK(a.error.empty());
  CHECK(a.action == Action::help);
}

// --- Help ---

TEST_CASE("--help", "[unit][cli]") {
  auto a = parse({"frametap", "--help"});
  CHECK(a.error.empty());
  CHECK(a.action == Action::help);
}

TEST_CASE("-h", "[unit][cli]") {
  auto a = parse({"frametap", "-h"});
  CHECK(a.error.empty());
  CHECK(a.action == Action::help);
}

// --- Info actions ---

TEST_CASE("--list-monitors", "[unit][cli]") {
  auto a = parse({"frametap", "--list-monitors"});
  CHECK(a.error.empty());
  CHECK(a.action == Action::list_monitors);
}

TEST_CASE("--list-windows", "[unit][cli]") {
  auto a = parse({"frametap", "--list-windows"});
  CHECK(a.error.empty());
  CHECK(a.action == Action::list_windows);
}

TEST_CASE("--check-permissions", "[unit][cli]") {
  auto a = parse({"frametap", "--check-permissions"});
  CHECK(a.error.empty());
  CHECK(a.action == Action::check_permissions);
}

// --- Monitor capture ---

TEST_CASE("--monitor with ID", "[unit][cli]") {
  auto a = parse({"frametap", "--monitor", "2"});
  CHECK(a.error.empty());
  CHECK(a.action == Action::capture);
  CHECK(a.mode == CaptureMode::monitor);
  CHECK(a.monitor_id == 2);
}

TEST_CASE("--monitor missing ID", "[unit][cli]") {
  auto a = parse({"frametap", "--monitor"});
  CHECK_FALSE(a.error.empty());
}

TEST_CASE("--monitor invalid ID", "[unit][cli]") {
  auto a = parse({"frametap", "--monitor", "abc"});
  CHECK_FALSE(a.error.empty());
}

// --- Window capture ---

TEST_CASE("--window with ID", "[unit][cli]") {
  auto a = parse({"frametap", "--window", "12345"});
  CHECK(a.error.empty());
  CHECK(a.action == Action::capture);
  CHECK(a.mode == CaptureMode::window);
  CHECK(a.window_id == 12345);
}

TEST_CASE("--window missing ID", "[unit][cli]") {
  auto a = parse({"frametap", "--window"});
  CHECK_FALSE(a.error.empty());
}

TEST_CASE("--window invalid ID", "[unit][cli]") {
  auto a = parse({"frametap", "--window", "xyz"});
  CHECK_FALSE(a.error.empty());
}

// --- Region capture ---

TEST_CASE("--region valid", "[unit][cli]") {
  auto a = parse({"frametap", "--region", "10,20,1920,1080"});
  CHECK(a.error.empty());
  CHECK(a.action == Action::capture);
  CHECK(a.mode == CaptureMode::region);
  CHECK(a.region.x == Catch::Approx(10));
  CHECK(a.region.y == Catch::Approx(20));
  CHECK(a.region.w == Catch::Approx(1920));
  CHECK(a.region.h == Catch::Approx(1080));
}

TEST_CASE("--region missing value", "[unit][cli]") {
  auto a = parse({"frametap", "--region"});
  CHECK_FALSE(a.error.empty());
}

TEST_CASE("--region invalid format", "[unit][cli]") {
  auto a = parse({"frametap", "--region", "bad"});
  CHECK_FALSE(a.error.empty());
}

TEST_CASE("--region zero dimensions", "[unit][cli]") {
  auto a = parse({"frametap", "--region", "0,0,0,0"});
  CHECK_FALSE(a.error.empty());
}

// --- Interactive ---

TEST_CASE("--interactive", "[unit][cli]") {
  auto a = parse({"frametap", "--interactive"});
  CHECK(a.error.empty());
  CHECK(a.action == Action::capture);
  CHECK(a.mode == CaptureMode::interactive);
}

// --- Output ---

TEST_CASE("Default output", "[unit][cli]") {
  auto a = parse({"frametap", "--monitor", "1"});
  CHECK(a.output == "screenshot.bmp");
}

TEST_CASE("-o custom output", "[unit][cli]") {
  auto a = parse({"frametap", "--monitor", "1", "-o", "out.bmp"});
  CHECK(a.error.empty());
  CHECK(a.output == "out.bmp");
}

TEST_CASE("--output custom output", "[unit][cli]") {
  auto a = parse({"frametap", "--monitor", "1", "--output", "out.bmp"});
  CHECK(a.error.empty());
  CHECK(a.output == "out.bmp");
}

TEST_CASE("-o missing filename", "[unit][cli]") {
  auto a = parse({"frametap", "-o"});
  CHECK_FALSE(a.error.empty());
}

// --- Errors ---

TEST_CASE("Unknown option", "[unit][cli]") {
  auto a = parse({"frametap", "--bogus"});
  CHECK_FALSE(a.error.empty());
}

TEST_CASE("Output only, no capture mode", "[unit][cli]") {
  auto a = parse({"frametap", "-o", "out.bmp"});
  CHECK_FALSE(a.error.empty());
}

// --- parse_region ---

TEST_CASE("parse_region valid", "[unit][cli]") {
  Region r;
  CHECK(parse_region("100,200,800,600", r));
  CHECK(r.x == Catch::Approx(100));
  CHECK(r.y == Catch::Approx(200));
  CHECK(r.w == Catch::Approx(800));
  CHECK(r.h == Catch::Approx(600));
}

TEST_CASE("parse_region fractional", "[unit][cli]") {
  Region r;
  CHECK(parse_region("0.5,1.5,100.25,200.75", r));
  CHECK(r.x == Catch::Approx(0.5));
  CHECK(r.y == Catch::Approx(1.5));
  CHECK(r.w == Catch::Approx(100.25));
  CHECK(r.h == Catch::Approx(200.75));
}

TEST_CASE("parse_region too few values", "[unit][cli]") {
  Region r;
  CHECK_FALSE(parse_region("10,20,30", r));
}

TEST_CASE("parse_region zero width", "[unit][cli]") {
  Region r;
  CHECK_FALSE(parse_region("0,0,0,100", r));
}

TEST_CASE("parse_region negative height", "[unit][cli]") {
  Region r;
  CHECK_FALSE(parse_region("0,0,100,-50", r));
}
