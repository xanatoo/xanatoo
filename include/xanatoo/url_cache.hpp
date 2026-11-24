// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Persistent cache of downloaded URLs: what each answer was (metadata) and
// its bytes, kept across runs under cache_dir():
//
//   urls/<hash>   the body as received (also the remote image file, see
//                 remote_image_path), written atomically
//   urls.sqlite   one row per requested URL: final URL, HTTP status,
//                 Content-Type, ETag / Last-Modified, time, size and
//                 the body's object hash (objects/<sha256>, a hard link
//                 to the same bytes; object_store.hpp)
//
// The metadata answers what an address *is* before it is fetched again:
// an address named like a picture ("/wiki/File:X.jpg") may serve a web
// page, and only its Content-Type tells. Safe from any thread (one
// connection behind a mutex). Without XANATOO_SQLITE only the bodies are
// kept and every address is UrlKind::Unknown.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace xanatoo {

struct UrlCacheEntry {
  std::string url;           // as requested (the key)
  std::string final_url;     // after redirects (== url when none)
  int status = 0;            // HTTP status; 0 for schemes without one
  std::string content_type;  // as sent ("text/html; charset=utf-8")
  std::string etag;
  std::string last_modified;
  int64_t fetched = 0;       // ms since the epoch, UTC
  int64_t size = -1;         // body bytes, -1 unknown / not kept
  std::string body{};        // object hash of the body (object_store.hpp),
                             // '' unknown / not kept; set by url_cache_put
};

// cache_dir()/urls/<FNV-1a of url>; empty without a cache directory.
std::string url_cache_body_path(std::string_view url);

// Record an answer: its metadata and, with `body`, its bytes. Also keyed
// by `final_url` when it differs, so the redirect target is known too.
// The bytes also become an object (hard link) and fill the snapshots that
// wait for this picture (snapshot_note_body).
// False when nothing could be written (no cache directory, I/O error).
bool url_cache_put(UrlCacheEntry entry, const std::string_view* body = nullptr);

std::optional<UrlCacheEntry> url_cache_find(std::string_view url);

// What the last answer for `url` was, by its Content-Type.
enum class UrlKind { Unknown, Image, Other };
UrlKind url_cache_kind(std::string_view url);

// "image/png; x=1" → true for the picture formats the image viewer decodes
// (PNG, JPEG, GIF, WebP, BMP, TIFF, AVIF, JPEG XL, SVG).
bool content_type_is_image(std::string_view content_type);

// Drop rows not refreshed for `max_age_ms` (bodies are pruned by size with
// prune_cache_dir). For a background thread at start-up.
void url_cache_prune_rows(int64_t max_age_ms);

// Close the metadata store (the next call reopens it): before its
// directory is removed (tests).
void url_cache_close();

}  // namespace xanatoo
