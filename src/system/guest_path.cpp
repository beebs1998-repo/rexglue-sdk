/**
 * @file        system/guest_path.cpp
 * @brief       Guest path normalization for Xbox 360 VFS paths
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/system/guest_path.h>

#include <algorithm>
#include <cctype>

#include <rex/string/utf8.h>

namespace rex::system {

std::string NormalizeGuestPath(std::string_view path) {
  // Manifest / config consumers expect POSIX-style guest paths: forward
  // slashes, no device prefix, lowercase. The runtime VFS tolerates either
  // separator, so converting backslashes here keeps writers (TOML) happy.
  std::string result = rex::string::utf8_fix_path_separators(path, U'/');

  auto colon = result.find(':');
  if (colon != std::string::npos && colon + 1 < result.size() && result[colon + 1] == '/') {
    result.erase(0, colon + 2);
  }

  result = rex::string::utf8_canonicalize_path(result, U'/');

  while (!result.empty() && result.front() == '/') {
    result.erase(result.begin());
  }

  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  // NT device paths (\Device\Harddisk0\Partition1\...) carry no drive colon, so
  // the strip above misses them. Drop the mount prefix through the first
  // "partitionN" segment so the result matches manifest-style guest paths.
  // Without this, the kernel's NT path for a loaded module never compares equal
  // to the manifest guest path, and unload cannot find the recompiled module.
  if (result.rfind("device/", 0) == 0) {
    constexpr std::string_view kPartitionPrefix = "partition";
    for (std::size_t pos = 0; pos < result.size();) {
      std::size_t next = result.find('/', pos);
      std::size_t end = (next == std::string::npos) ? result.size() : next;
      std::string_view segment(result.data() + pos, end - pos);
      if (segment.rfind(kPartitionPrefix, 0) == 0) {
        result.erase(0, (next == std::string::npos) ? result.size() : next + 1);
        break;
      }
      if (next == std::string::npos) {
        break;
      }
      pos = next + 1;
    }
  }

  return result;
}

}  // namespace rex::system
