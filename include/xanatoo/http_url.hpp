// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace xanatoo {

struct HttpUrl {
  bool secure = false;
  std::string host;       // DNS / getaddrinfo name (IPv6 without brackets)
  std::string authority;  // Host header, including an explicit port
  int port = 80;
  std::string target;     // request path and query, no fragment
};

// Only http:// and https:// URLs are accepted. Reject malformed authorities
// and control characters before interpolating URL parts into HTTP headers.
std::optional<HttpUrl> parse_http_url(std::string_view url);

// Resolve a Location header against the URL that produced it. The target
// retains its own scheme; HTTPS is not silently substituted for HTTP.
std::optional<std::string> resolve_http_redirect(std::string_view base,
                                                 std::string_view location);

// Browsers may start at http://, but a redirect must never silently weaken
// an HTTPS connection into plaintext. Both URLs must already be resolved.
bool http_redirect_keeps_security(std::string_view from,
                                  std::string_view to);

// The URL to actually fetch for a resolved redirect from `from` to `to`.
// An HTTPS → http:// redirect is upgraded to the same address over https://
// (an explicit :80 is dropped), like browser HTTPS-Upgrades; no plaintext
// request is made. Everything else is returned unchanged. Sets *upgraded when
// the scheme was rewritten; nullopt when either URL is not HTTP(S).
std::optional<std::string> secure_redirect_target(std::string_view from,
                                                  std::string_view to,
                                                  bool* upgraded = nullptr);

// "bytes 100-199/1000" (Content-Range) → {100, 199, 1000}, total -1 for
// "/*"; nullopt when it is not a byte range.
struct ContentRange {
  int64_t first = 0, last = 0, total = -1;
};
std::optional<ContentRange> parse_content_range(std::string_view value);

}  // namespace xanatoo
