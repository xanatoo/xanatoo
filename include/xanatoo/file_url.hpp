// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include <string>
#include <string_view>

namespace xanatoo {

// `file:` URLs ↔ local paths. Local pages are addressed as file:///abs/path
// wherever an address is shown or stored; only the path reaches the file
// system. Percent-encoded are '%', '#', '?', space and control bytes, so
// the locator never mistakes a filename for a selector. UTF-8 stays as is.

inline bool is_file_url(std::string_view s) { return s.starts_with("file:"); }

// file:///p, file://localhost/p and file:/p → "/p" (decoded). Empty when
// the URL names another host or does not decode.
inline std::string file_url_path(std::string_view url) {
  if (!is_file_url(url)) return {};
  url.remove_prefix(5);
  if (url.starts_with("//")) {
    url.remove_prefix(2);
    if (url.starts_with("localhost/")) url.remove_prefix(9);
  }
  if (url.empty() || url.front() != '/') return {};
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::string path;
  path.reserve(url.size());
  for (size_t i = 0; i < url.size(); ++i) {
    char c = url[i];
    if (c == '%') {
      if (i + 2 >= url.size()) return {};
      const int hi = hex(url[i + 1]), lo = hex(url[i + 2]);
      if (hi < 0 || lo < 0) return {};
      c = static_cast<char>(hi * 16 + lo);
      i += 2;
    }
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f || c == '\0')
      return {};
    path.push_back(c);
  }
#if defined(_WIN32)
  if (path.size() >= 3 && path[0] == '/' && path[2] == ':')
    path.erase(path.begin());
#endif
  return path;
}

// An absolute path → file:///path. Windows drive paths ("C:\a") become
// file:///C:/a.
inline std::string file_url(std::string_view abs_path) {
  std::string out = "file://";
  if (abs_path.empty() || (abs_path.front() != '/' && abs_path.front() != '\\'))
    out += '/';
  static constexpr char kHex[] = "0123456789ABCDEF";
  for (const char ch : abs_path) {
    const unsigned char c = static_cast<unsigned char>(ch);
#if defined(_WIN32)
    if (c == '\\') { out += '/'; continue; }
#endif
    if (c <= 0x20 || c == 0x7f || c == '%' || c == '#' || c == '?') {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 15];
    } else {
      out += ch;
    }
  }
  return out;
}

// True for what looks like a local path rather than an address with a
// scheme ("about:", "db:…", "https://…"). A one-letter scheme is a Windows
// drive.
inline bool looks_like_local_path(std::string_view s) {
  if (s.empty()) return false;
  if (s.front() == '/' || s.front() == '.') return true;
  size_t i = 0;
  auto alpha = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
  };
  if (!alpha(s[0])) return true;
  while (i < s.size() && (alpha(s[i]) || (s[i] >= '0' && s[i] <= '9') ||
                          s[i] == '+' || s[i] == '-' || s[i] == '.'))
    ++i;
  return !(i < s.size() && s[i] == ':' && i >= 2);
}

}  // namespace xanatoo
