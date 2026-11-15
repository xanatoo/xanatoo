// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
// Gopher protocol client (RFC 1436) for xanatoo.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xanatoo {

// True for gopher:// URLs (scheme case-insensitive).
bool is_gopher_url(std::string_view url);

// Split gopher://host[:port][/[type][selector]][?query]
// Classic form: gopher://host:70/1/path  or  gopher://host/0selector
// Type 7 (search): query becomes the TAB-separated search string on the wire.
struct GopherUrl {
  std::string host;
  int port = 70;
  char type = '1';          // item type (1 = menu by default)
  std::string selector;     // path after type, may be empty
  std::string query;        // type-7 search string (from ?query)
  std::string canonical;    // reconstructed URL (includes ?query when set)
};
std::optional<GopherUrl> parse_gopher_url(std::string_view url);

// Resolve a relative link against a base gopher:// URL.
std::string resolve_gopher_url(std::string_view base, std::string_view ref);

struct GopherMenuItem {
  char type = 'i';          // 'i' informational, '0' text, '1' menu, '7' search, 'h' HTML, etc.
  std::string label;
  std::string selector;
  std::string host;
  int port = 70;
};

struct GopherResponse {
  bool is_menu = false;
  bool needs_input = false;   // type 7 without a query yet
  bool is_binary = false;     // image / binary item types
  std::vector<GopherMenuItem> menu;
  std::string body;           // rendered Markdown / text, or binary bytes when is_binary
  std::string final_url;
  std::string mime_hint;      // "text/plain", "text/gopher-menu", "application/octet-stream", …
  std::string input_prompt;   // shown when needs_input
};

#if defined(XANATOO_GOPHER) && XANATOO_GOPHER
// Fetch over plain TCP. Type 7 with no query returns needs_input without
// contacting the server. Binary types fill body with raw bytes and set
// is_binary. On failure, nullopt and err set.
std::optional<GopherResponse> gopher_fetch(std::string_view url,
                                           std::string* err = nullptr);
#endif

// Pure helpers (always available for tests / offline conversion).
std::vector<GopherMenuItem> parse_gopher_menu(std::string_view data);
std::string gopher_menu_to_markdown(const std::vector<GopherMenuItem>& items,
                                    std::string_view base_url);
bool gopher_type_is_binary(char type);

}  // namespace xanatoo
