// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Location-bar icon for addresses without a favicon of their own: local
// files and folders, built-in pages, the revision store, Nostr, feeds. Pure;
// app_frame.cpp shows it when no real favicon / cover / thumbnail is there.

#include <string_view>

namespace xanatoo {

// Name of an icon under icons/ (without .png); "" for no icon. `is_dir` says
// a local path names a directory (the caller stats it). search: addresses
// return "search": the caller draws the engine's tile instead.
inline std::string_view pseudo_favicon(std::string_view address,
                                       bool is_dir = false) {
  auto has = [&](std::string_view p) {
    if (address.size() < p.size()) return false;
    for (size_t i = 0; i < p.size(); ++i) {
      char c = address[i];
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      if (c != p[i]) return false;
    }
    return true;
  };
  if (address.empty() || address == "untitled") return "site-file";
  if (has("about:")) return "site-about";
  if (has("search:")) return "search";
  if (has("db:") || has("db-original:") || has("draft:") || has("drafts:"))
    return "site-store";
  if (has("sitemap:")) return "sitemap";
  if (has("feed:") || has("atom:")) return "site-feed";
  if (has("gemini://")) return "gemini";
  // Small-net siblings share the globe until dedicated icons exist.
  if (has("gopher://") || has("spartan://") || has("nex://") ||
      has("finger://"))
    return "site-web";
  if (has("nostr:") || has("npub1") || has("nprofile1") || has("note1") ||
      has("nevent1") || has("naddr1"))
    return "site-nostr";
  if (has("file:")) return is_dir ? "site-folder" : "site-file";
  // Any other scheme (http, hn:, wikipedia:, wayback:, gopher://, …) is a
  // network service whose favicon is missing or still loading.
  const size_t colon = address.find(':');
  const size_t slash = address.find_first_of("/\\");
  if (colon != std::string_view::npos && colon > 1 &&
      (slash == std::string_view::npos || colon < slash))
    return "site-web";
  return is_dir ? "site-folder" : "site-file";
}

}  // namespace xanatoo
