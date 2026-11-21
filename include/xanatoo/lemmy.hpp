// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include "xanatoo/types.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace xanatoo {

// Lemmy community / post / API URLs, e.g.:
//   https://lemmy.world/c/asklemmy
//   https://programming.dev/post/12345
//   https://lemmy.world/api/v3/post/list?community_name=linux
// Any host is accepted when the path matches /c/, /post/, /api/v3/ or
// /api/alpha/ (PieFed's Lemmy-shaped API).
bool is_lemmy_url(std::string_view s);

// The PieFed spelling of a Lemmy API URL: the first /api/v3/ becomes
// /api/alpha/. Returns the input unchanged when it has no /api/v3/.
std::string lemmy_alpha_api_url(std::string_view url);

// The document for one API v3 post response ({"post_view": …}) and its
// comment list ({"comments": […]}), without network I/O.
Document lemmy_post_document(std::string_view post_json,
                             std::string_view comments_json,
                             std::string_view origin);

#if defined(XANATOO_LEMMY) && XANATOO_LEMMY
// Fetch a community listing or single post (+ comments) via Lemmy API v3.
// Builds a Document with Thread + Message nodes (read-only).
std::optional<Document> lemmy_fetch(std::string_view url,
                                    std::string* err = nullptr);
#endif

}  // namespace xanatoo
