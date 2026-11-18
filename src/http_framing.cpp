// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/http_framing.hpp"

#include "xanatoo/util.hpp"

#include <charconv>
#include <cstdint>
#include <string>

namespace xanatoo {

namespace {

// ASCII lower case (header names and tokens); local so this file needs no
// util.cpp.
std::string lower(std::string_view s) {
  std::string out(s);
  for (char& c : out)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return out;
}

// Value of header `name` (lower case) in the header block `head` (status
// line first, no final blank line); empty when absent.
std::string_view header_in(std::string_view head, std::string_view name) {
  size_t at = head.find("\r\n");
  while (at != std::string_view::npos) {
    const size_t start = at + 2;
    const size_t end = head.find("\r\n", start);
    std::string_view line = head.substr(
        start, end == std::string_view::npos ? std::string_view::npos
                                             : end - start);
    const size_t colon = line.find(':');
    if (colon != std::string_view::npos) {
      std::string_view key = line.substr(0, colon);
      trim_ascii(key);
      if (lower(key) == name) {
        std::string_view value = line.substr(colon + 1);
        trim_ascii(value);
        return value;
      }
    }
    at = end;
  }
  return {};
}

}  // namespace

bool HttpResponseFraming::update(std::string_view data) {
  if (complete_ || broken_) return complete_;
  if (body_ == Body::Unknown) {
    const size_t end = data.find("\r\n\r\n");
    if (end == std::string_view::npos) return false;
    const std::string_view head = data.substr(0, end);
    body_start_ = end + 4;
    const size_t sp = head.find(' ');
    int status = 0;
    if (sp == std::string_view::npos ||
        std::from_chars(head.data() + sp + 1, head.data() + head.size(), status)
                .ec != std::errc{}) {
      broken_ = true;
      return false;
    }
    const std::string te = lower(header_in(head, "transfer-encoding"));
    const std::string_view cl = header_in(head, "content-length");
    if (status == 204 || status == 304) {
      body_ = Body::Length;
      length_ = 0;
    } else if (status < 200) {
      body_ = Body::UntilClose;  // interim answer: we never ask for one
    } else if (te.find("chunked") != std::string::npos) {
      body_ = Body::Chunked;
      chunk_at_ = body_start_;
    } else if (!cl.empty()) {
      const auto [p, ec] = std::from_chars(cl.data(), cl.data() + cl.size(),
                                           length_);
      if (ec != std::errc{} || p != cl.data() + cl.size()) {
        broken_ = true;
        return false;
      }
      body_ = Body::Length;
    } else {
      body_ = Body::UntilClose;
    }
    delimited_ = body_ != Body::UntilClose;
    reusable_ = delimited_ && starts_with(head, "HTTP/1.1 ") &&
                lower(header_in(head, "connection")).find("close") ==
                    std::string::npos;
  }
  if (body_ == Body::Length) {
    if (data.size() - body_start_ < length_) return false;
    size_ = body_start_ + length_;
    complete_ = true;
    return true;
  }
  if (body_ != Body::Chunked) return false;
  for (;;) {
    if (chunk_at_ >= data.size()) return false;  // inside a chunk's data
    const size_t eol = data.find("\r\n", chunk_at_);
    if (eol == std::string_view::npos) {
      if (data.size() - chunk_at_ > 1024) broken_ = true;
      return false;
    }
    std::string_view hex = data.substr(chunk_at_, eol - chunk_at_);
    hex = hex.substr(0, hex.find(';'));
    trim_ascii(hex);
    size_t n = 0;
    const auto [p, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), n,
                                         16);
    if (hex.empty() || ec != std::errc{} || p != hex.data() + hex.size() ||
        n > SIZE_MAX / 2) {
      broken_ = true;
      return false;
    }
    if (n > 0) {
      chunk_at_ = eol + 2 + n + 2;  // size line, data, CRLF
      continue;
    }
    // Last chunk; the trailer section ends with an empty line.
    const size_t t = eol + 2;
    if (data.size() < t + 2) return false;
    size_t done = 0;
    if (data.substr(t, 2) == "\r\n") {
      done = t + 2;
    } else {
      const size_t e = data.find("\r\n\r\n", t);
      if (e == std::string_view::npos) return false;
      done = e + 4;
    }
    size_ = done;
    complete_ = true;
    return true;
  }
}

}  // namespace xanatoo
