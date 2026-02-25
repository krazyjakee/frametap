#include <catch2/catch_test_macros.hpp>
#include <frametap/frametap.h>

#include "helpers.h"

using namespace frametap;

// --- Section 3.4: Permissions ---

TEST_CASE("check_permissions returns", "[integration][permissions]") {
  PermissionCheck pc;
  CHECK_NOTHROW(pc = check_permissions());
}

TEST_CASE("Status is valid enum", "[integration][permissions]") {
  auto pc = check_permissions();
  bool valid = (pc.status == PermissionStatus::ok ||
                pc.status == PermissionStatus::warning ||
                pc.status == PermissionStatus::error);
  CHECK(valid);
}

TEST_CASE("Summary is non-empty", "[integration][permissions]") {
  auto pc = check_permissions();
  CHECK_FALSE(pc.summary.empty());
}

TEST_CASE("Granted on CI", "[integration][permissions]") {
  if (!test_helpers::has_display()) {
    SKIP("No display server available");
  }

  auto pc = check_permissions();
  // On CI with display access (Xvfb), status should be ok
  CHECK(pc.status == PermissionStatus::ok);
}
