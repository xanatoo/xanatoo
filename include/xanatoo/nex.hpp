// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
// Nex protocol client (port 1900) for xanatoo.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace xanatoo {

bool is_nex_url(std::string_view url);

struct NexUrl {
  std::string host;
  int port = 1900;
  std::string path;       // may be empty (root)
  std::string canonical;
};
std::optional<NexUrl> parse_nex_url(std::string_view url);

std::string resolve_nex_url(std::string_view base, std::string_view ref);

struct NexResponse {
  bool is_directory = false;
  std::string body;         // raw or rendered Markdown for directories
  std::string final_url;
};

#if defined(XANATOO_NEX) && XANATOO_NEX
std::optional<NexResponse> nex_fetch(std::string_view url,
                                     std::string* err = nullptr);
#endif

// Convert Nex directory listing (=> lines) to Markdown.
std::string nex_directory_to_markdown(std::string_view data,
                                      std::string_view base_url);

}  // namespace xanatoo
