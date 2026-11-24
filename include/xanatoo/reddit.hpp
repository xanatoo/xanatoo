// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include "xanatoo/types.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace xanatoo {

// True for reddit.com / old.reddit.com listing or post URLs (with or without
// .json). Also accepts bare "reddit.com/…" without a scheme.
bool is_reddit_url(std::string_view s);

// Normalize to an https://www.reddit.com/…/.json request URL.
// Returns empty on failure.
std::string reddit_json_url(std::string_view s);

// Equivalent feed URL for a Reddit listing or comment thread. Normalizes
// www/old/np hosts, strips fragments, and preserves listing query options.
// Empty for non-Reddit URLs and redd.it short links (no subreddit path).
std::string reddit_rss_url(std::string_view s);

// A post's own video (v.redd.it) for the Media panel, from its `data`
// object: the HLS playlist (with sound) where this build plays it, else the
// progressive MP4 (`fallback_url`, picture only). Empty url: none.
struct RedditVideo {
  std::string url;
  bool silent = false;  // the fallback: no sound track
};
RedditVideo reddit_video(std::string_view post_data_json);

// Reddit's Markdown links "u/name" and "/u/name" to the user's page; in
// the body blocks under `node` such text becomes a Link (rel mention, href
// https://www.reddit.com/user/NAME), and links that already go to a user
// page get rel mention. Text inside code spans or links stays as it is.
void reddit_mark_users(Document& doc, NodeId node);

#if defined(XANATOO_REDDIT) && XANATOO_REDDIT
// Fetch a subreddit listing or post+comments via the public .json API.
// Builds a Document with Thread + Message nodes (read-only).
// On failure returns nullopt and sets *err.
std::optional<Document> reddit_fetch(std::string_view url,
                                     std::string* err = nullptr);
#endif

}  // namespace xanatoo
