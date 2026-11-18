// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/http_framing.hpp"

#include <cstdio>
#include <string>
#include <string_view>

namespace {
int failures = 0;
void check(bool ok, std::string_view what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL %.*s\n", static_cast<int>(what.size()),
                 what.data());
    ++failures;
  }
}

// Feeds `all` one byte at a time; returns the framing after the last byte
// and the length at which it first completed (0: never).
xanatoo::HttpResponseFraming feed_bytes(const std::string& all,
                                        size_t* first_done) {
  xanatoo::HttpResponseFraming f;
  *first_done = 0;
  for (size_t i = 1; i <= all.size(); ++i)
    if (f.update(std::string_view(all).substr(0, i)) && !*first_done)
      *first_done = i;
  return f;
}
}  // namespace

int main() {
  size_t done = 0;
  {
    const std::string r =
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Type: image/jpeg"
        "\r\n\r\nhello";
    auto f = feed_bytes(r + "HTTP/1.1 200 OK", &done);
    check(f.complete() && f.size() == r.size() && done == r.size(),
          "Content-Length ends the response");
    check(f.delimited() && f.reusable(), "HTTP/1.1 length body is reusable");
  }
  {
    const std::string r =
        "HTTP/1.1 200 OK\r\ntransfer-encoding: Chunked\r\n\r\n"
        "4;ext=1\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
    auto f = feed_bytes(r, &done);
    check(f.complete() && f.size() == r.size() && done == r.size(),
          "chunked body ends after the last chunk");
    check(f.reusable(), "chunked body is reusable");
  }
  {
    const std::string r =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "3\r\nabc\r\n0\r\nX-Trailer: 1\r\n\r\n";
    auto f = feed_bytes(r, &done);
    check(f.complete() && done == r.size(), "chunked trailers");
  }
  {
    // A chunk whose data contains "\r\n" and a size-like line.
    const std::string r =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "7\r\n0\r\n\r\nab\r\n0\r\n\r\n";
    auto f = feed_bytes(r, &done);
    check(f.complete() && done == r.size(), "chunk data is skipped by size");
  }
  {
    const std::string r =
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
    auto f = feed_bytes(r, &done);
    check(f.complete() && !f.reusable(), "Connection: close is not reused");
  }
  {
    const std::string r = "HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok";
    auto f = feed_bytes(r, &done);
    check(f.complete() && !f.reusable(), "HTTP/1.0 is not reused");
  }
  {
    const std::string r = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nabc";
    auto f = feed_bytes(r, &done);
    check(!f.complete() && !f.delimited() && !f.reusable() && !f.broken(),
          "no length: read until close");
  }
  {
    const std::string r = "HTTP/1.1 304 Not Modified\r\nETag: x\r\n\r\n";
    auto f = feed_bytes(r, &done);
    check(f.complete() && f.size() == r.size() && f.reusable(),
          "304 has no body");
  }
  {
    const std::string r =
        "HTTP/1.1 301 Moved\r\nLocation: /x\r\nContent-Length: 0\r\n\r\n";
    auto f = feed_bytes(r, &done);
    check(f.complete() && f.size() == r.size(), "empty redirect body");
  }
  {
    xanatoo::HttpResponseFraming f;
    f.update("HTTP/1.1 200 OK\r\nContent-Length: 12x\r\n\r\n");
    check(f.broken() && !f.complete() && !f.reusable(),
          "bad Content-Length is broken");
    xanatoo::HttpResponseFraming g;
    g.update("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n");
    check(g.broken() && !g.complete(), "bad chunk size is broken");
  }
  if (failures) std::fprintf(stderr, "%d http_framing test(s) failed\n", failures);
  return failures ? 1 : 0;
}
