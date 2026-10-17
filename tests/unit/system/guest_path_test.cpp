/**
 * @file        tests/unit/system/guest_path_test.cpp
 * @brief       Unit tests for guest path normalization
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>
#include <rex/system/guest_path.h>

using rex::system::NormalizeGuestPath;

TEST_CASE("Guest path normalization: strip device prefix", "[system][path]") {
  CHECK(NormalizeGuestPath("game:\\bin\\somelib.dll") == "bin/somelib.dll");
  CHECK(NormalizeGuestPath("GAME:\\BIN\\SomeLib.DLL") == "bin/somelib.dll");
  CHECK(NormalizeGuestPath("d:\\content\\modules\\test.dll") == "content/modules/test.dll");
}

TEST_CASE("Guest path normalization: strip NT device prefix", "[system][path]") {
  CHECK(NormalizeGuestPath("\\Device\\Harddisk0\\Partition1\\media\\Module\\palm_normal.xex") ==
        "media/module/palm_normal.xex");
  CHECK(NormalizeGuestPath("/Device/Harddisk0/Partition1/default.xex") == "default.xex");
  CHECK(NormalizeGuestPath("\\device\\harddisk0\\partition0\\cache\\foo.bin") == "cache/foo.bin");
  // No partition segment: leave the device path untouched rather than guessing.
  CHECK(NormalizeGuestPath("\\Device\\Harddisk0\\Cache0\\foo.bin") ==
        "device/harddisk0/cache0/foo.bin");
}

TEST_CASE("Guest path normalization: already normalized", "[system][path]") {
  CHECK(NormalizeGuestPath("bin/somelib.dll") == "bin/somelib.dll");
}

TEST_CASE("Guest path normalization: backslash to forward slash", "[system][path]") {
  CHECK(NormalizeGuestPath("data\\libs\\foo.dll") == "data/libs/foo.dll");
}

TEST_CASE("Guest path normalization: case folding", "[system][path]") {
  CHECK(NormalizeGuestPath("BIN/SomeLib.DLL") == "bin/somelib.dll");
}
