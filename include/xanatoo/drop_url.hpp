// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include "xanatoo/file_url.hpp"

#include <string>
#include <string_view>

namespace xanatoo {

// A text drop can be a plain URL or a text/uri-list (comments and CRLF).
// Only navigation targets are accepted: ordinary prose must not open a local
// filename, and unsupported schemes such as javascript:/data: are ignored.
inline std::string dropped_navigation_target(std::string_view text) {
  constexpr std::string_view spaces = " \t\r\n";
  auto trim = [&](std::string_view& s) {
    const size_t begin = s.find_first_not_of(spaces);
    if (begin == std::string_view::npos) { s = {}; return; }
    s.remove_prefix(begin);
    s.remove_suffix(s.size() - s.find_last_not_of(spaces) - 1);
  };
  auto valid = [](std::string_view s) {
    for (const unsigned char c : s)
      if (c <= 0x20 || c == 0x7f) return false;
    return true;
  };
  auto prefix = [](std::string_view s, std::string_view p) {
    return s.starts_with(p);
  };
  while (!text.empty()) {
    const size_t end = text.find('\n');
    std::string_view line = text.substr(0, end);
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    trim(line);
    if (line.empty() || line.front() == '#' || !valid(line)) continue;
    if (prefix(line, "file://")) {
      if (std::string path = file_url_path(line); !path.empty()) return path;
      continue;
    }
    if (prefix(line, "https://") || prefix(line, "http://") ||
        prefix(line, "gemini://")) {
      const size_t start = line.find("://") + 3;
      if (start < line.size() && line[start] != '/' &&
          line[start] != '?' && line[start] != '#')
        return std::string(line);
      continue;
    }
    for (std::string_view scheme : {"hn:", "reddit:", "lemmy:", "matrix:",
                                    "nostr:", "wikipedia:", "feed:",
                                    "sitemap:", "search:", "db:",
                                    "db-original:", "draft:"})
      if (line.starts_with(scheme) && line.size() > scheme.size())
        return std::string(line);
  }
  return {};
}

} // namespace xanatoo
