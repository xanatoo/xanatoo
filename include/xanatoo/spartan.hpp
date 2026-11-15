// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
// Spartan protocol client (port 300) for xanatoo.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace xanatoo {

bool is_spartan_url(std::string_view url);

struct SpartanUrl {
  std::string host;
  int port = 300;
  std::string path;       // absolute, begins with /
  std::string query;      // optional, becomes upload data per common convention
  std::string canonical;
};
std::optional<SpartanUrl> parse_spartan_url(std::string_view url);

std::string resolve_spartan_url(std::string_view base, std::string_view ref);

struct SpartanResponse {
  int status = 0;         // 2 success, 3 redirect, 4 client err, 5 server err
  std::string meta;
  std::string body;
  std::string final_url;
};

#if defined(XANATOO_SPARTAN) && XANATOO_SPARTAN
std::optional<SpartanResponse> spartan_fetch(std::string_view url,
                                             std::string* err = nullptr);
#endif

}  // namespace xanatoo
