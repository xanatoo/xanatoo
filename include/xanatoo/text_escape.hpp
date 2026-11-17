// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
// Header-only escaping for URLs and generated Markdown (no dependencies, so
// the small-net clients and their standalone tests can use it).
#pragma once

#include <string>
#include <string_view>

namespace xanatoo {

inline int hex_digit_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// %XX → byte; everything else (including '+' and broken escapes) stays.
inline std::string percent_decode(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      const int hi = hex_digit_value(s[i + 1]), lo = hex_digit_value(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>(hi * 16 + lo));
        i += 2;
        continue;
      }
    }
    out.push_back(s[i]);
  }
  return out;
}

// Encode the bytes that cannot stand in a URL path / query or a Markdown
// link destination: controls, space, non-ASCII, and % ? # ( ) < > " \ `.
inline std::string percent_encode_path(std::string_view s) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());
  for (char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    const bool unsafe = c <= 0x20 || c >= 0x7f || c == '%' || c == '?' ||
                        c == '#' || c == '(' || c == ')' || c == '<' ||
                        c == '>' || c == '"' || c == '\\' || c == '`';
    if (unsafe) {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 15]);
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

// Backslash-escape inline Markdown syntax (link labels, single-line text).
inline std::string markdown_escape_inline(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '\\': case '`': case '*': case '_': case '[': case ']':
      case '<': case '>':
        out.push_back('\\');
        out.push_back(c);
        break;
      default:
        out.push_back(c);
    }
  }
  return out;
}

// A backtick fence longer than any backtick run in `body` (at least 3), so
// the body cannot close the code block early.
inline std::string markdown_code_fence(std::string_view body) {
  size_t longest = 0, run = 0;
  for (char c : body) {
    run = c == '`' ? run + 1 : 0;
    if (run > longest) longest = run;
  }
  return std::string(longest >= 3 ? longest + 1 : 3, '`');
}

}  // namespace xanatoo
