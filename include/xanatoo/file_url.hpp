// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include <filesystem>
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

// An address typed into the URL bar, completed. An explicit local path —
// "/", "./", "../", "." / "..", a Windows drive — becomes a file:// URL
// (relative ones against `cwd`, which should be absolute; a '#' starts the
// selectors as in any address). Text with a scheme ("gemini://…", "db:…",
// "about:") stays, as does a leading '~' (left unexpanded), '#' or '?',
// other text with blanks
// and the empty string. Anything else is a web host: "example.com/x",
// "localhost:8080" → https://….
inline std::string complete_typed_address(std::string_view text,
                                          std::string_view cwd) {
  if (text.empty() || text.front() == '~' || text.front() == '#' ||
      text.front() == '?')
    return std::string(text);
  const bool drive = text.size() >= 3 && text[1] == ':' &&
      (text[2] == '/' || text[2] == '\\') &&
      ((text[0] >= 'a' && text[0] <= 'z') || (text[0] >= 'A' && text[0] <= 'Z'));
  const bool explicit_path = text.front() == '/' || drive || text == "." ||
      text == ".." || text.starts_with("./") || text.starts_with("../");
  if (explicit_path) {
    const size_t hash = text.find('#');
    std::filesystem::path p(std::string(text.substr(0, hash)));
    if (!drive && p.is_relative())
      p = std::filesystem::path(std::string(cwd)) / p;
    std::string abs = p.lexically_normal().generic_string();
    // A trailing slash only when typed ("./dir/", ".", "..").
    if (abs.size() > 1 && abs.back() == '/' &&
        !(text.substr(0, hash).ends_with("/") ||
          text.substr(0, hash).ends_with(".")))
      abs.pop_back();
    std::string out = file_url(abs);
    if (hash != std::string_view::npos) out += text.substr(hash);
    return out;
  }
  for (const char c : text)
    if (c == ' ' || c == '\t') return std::string(text);
  if (!looks_like_local_path(text)) {
    // "host:8080/x" reads as a scheme to looks_like_local_path; a port is
    // digits up to the path.
    const size_t colon = text.find(':');
    size_t i = colon + 1;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
    const bool port = i > colon + 1 &&
        (i == text.size() || text[i] == '/' || text[i] == '?' || text[i] == '#');
    if (!port) return std::string(text);
  }
  return "https://" + std::string(text);
}

}  // namespace xanatoo
