// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
// Finger protocol client (RFC 1288, port 79) for xanatoo.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace xanatoo {

bool is_finger_url(std::string_view url);

// finger://host[/user]  or  finger://user@host
struct FingerUrl {
  std::string host;
  int port = 79;
  std::string user;         // may be empty (host-wide info)
  std::string canonical;
};
std::optional<FingerUrl> parse_finger_url(std::string_view url);

struct FingerResponse {
  std::string body;
  std::string final_url;
};

#if defined(XANATOO_FINGER) && XANATOO_FINGER
std::optional<FingerResponse> finger_fetch(std::string_view url,
                                           std::string* err = nullptr);
#endif

}  // namespace xanatoo
