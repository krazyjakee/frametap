#pragma once

#include <cstdint>
#include <sstream>
#include <string>

namespace cli {

enum class Action {
  none,
  help,
  version,
  list_monitors,
  list_windows,
  check_permissions,
  capture,
};

enum class CaptureMode {
  none,
  monitor,
  window,
  region,
  interactive,
};

struct Region {
  double x = 0, y = 0, w = 0, h = 0;
};

struct Args {
  Action action = Action::none;
  CaptureMode mode = CaptureMode::none;
  std::string output = "screenshot.bmp";
  int monitor_id = -1;
  uint64_t window_id = 0;
  Region region{};
  std::string error;
};

inline bool parse_region(const char *arg, Region &r) {
  std::istringstream ss(arg);
  char c1, c2, c3;
  if (!(ss >> r.x >> c1 >> r.y >> c2 >> r.w >> c3 >> r.h))
    return false;
  if (c1 != ',' || c2 != ',' || c3 != ',')
    return false;
  return r.w > 0 && r.h > 0;
}

inline Args parse_args(int argc, char *argv[]) {
  Args args;

  if (argc <= 1) {
    args.action = Action::help;
    return args;
  }

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if (arg == "-h" || arg == "--help") {
      args.action = Action::help;
      return args;
    }

    if (arg == "-v" || arg == "--version") {
      args.action = Action::version;
      return args;
    }

    if (arg == "--list-monitors") {
      args.action = Action::list_monitors;
      return args;
    }

    if (arg == "--list-windows") {
      args.action = Action::list_windows;
      return args;
    }

    if (arg == "--check-permissions") {
      args.action = Action::check_permissions;
      return args;
    }

    if (arg == "--interactive") {
      args.action = Action::capture;
      args.mode = CaptureMode::interactive;
      continue;
    }

    if (arg == "-o" || arg == "--output") {
      if (++i >= argc) {
        args.error = std::string(arg) + " requires an argument.";
        return args;
      }
      args.output = argv[i];
      continue;
    }

    if (arg == "--monitor") {
      if (++i >= argc) {
        args.error = "--monitor requires an ID.";
        return args;
      }
      try {
        args.monitor_id = std::stoi(argv[i]);
      } catch (...) {
        args.error =
            std::string("Invalid monitor ID '") + argv[i] + "'.";
        return args;
      }
      args.action = Action::capture;
      args.mode = CaptureMode::monitor;
      continue;
    }

    if (arg == "--window") {
      if (++i >= argc) {
        args.error = "--window requires an ID.";
        return args;
      }
      try {
        args.window_id = std::stoull(argv[i]);
      } catch (...) {
        args.error =
            std::string("Invalid window ID '") + argv[i] + "'.";
        return args;
      }
      args.action = Action::capture;
      args.mode = CaptureMode::window;
      continue;
    }

    if (arg == "--region") {
      if (++i >= argc) {
        args.error = "--region requires x,y,w,h.";
        return args;
      }
      if (!parse_region(argv[i], args.region)) {
        args.error =
            std::string("Invalid region '") + argv[i] + "'. Expected: x,y,w,h";
        return args;
      }
      args.action = Action::capture;
      args.mode = CaptureMode::region;
      continue;
    }

    args.error = std::string("Unknown option '") + arg + "'.";
    return args;
  }

  // Had flags like -o but no capture mode
  if (args.action == Action::none) {
    args.error = "No capture mode specified.";
  }

  return args;
}

} // namespace cli
