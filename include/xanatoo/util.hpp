// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include "xanatoo/types.hpp"
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xanatoo {

// C++17-friendly prefix/suffix checks (std::*::starts_with is C++20).
inline bool starts_with(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() &&
         s.compare(0, prefix.size(), prefix) == 0;
}
inline bool ends_with(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// ASCII-only lowercasing (anchors, extensions, comparisons).
std::string ascii_lower(std::string s);

inline std::string ascii_lower(std::string_view s) {
  return ascii_lower(std::string(s));
}

// Case-insensitive ASCII prefix.
inline bool starts_with_ci(std::string_view s, std::string_view prefix) {
  if (s.size() < prefix.size()) return false;
  for (size_t i = 0; i < prefix.size(); ++i) {
    char a = s[i], b = prefix[i];
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

// Trim ASCII space/tab from both ends (in place).
inline void trim_ascii(std::string_view& s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '	'))
    s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '	'))
    s.remove_suffix(1);
}

bool is_text_node_kind(NodeKind k);

// Depth-first list of text-bearing nodes under id (typically doc.root).
void collect_text_nodes(const Document& doc, NodeId id,
                        std::vector<NodeId>& out);

// Resolve #anchor to a text node: exact plain-text match, then heading
// substring. "#fn:key" names a footnote (its first text node). Returns
// INVALID_NODE if not found.
NodeId find_anchor_node(const Document& doc, std::string_view href);
// What an include of `id` shows: the node itself when it holds text, else
// its first text block below the header (a labelled section → its first
// paragraph); INVALID_NODE when there is none.
NodeId transclude_text_node(const Document& doc, NodeId id);

// The Footnote node whose label is `key` (ASCII case-insensitive).
NodeId find_footnote(const Document& doc, std::string_view key);

// "#fn:key" — in-document href of a footnote reference.
inline std::string footnote_href(std::string_view key) {
  return "#fn:" + std::string(key);
}

// Concatenate a node's inline spans.
std::string plain_text(const Document& doc, NodeId id);

// Raster formats supported by stb_image in this project.
bool is_raster_image_path(std::string_view path);

// URL scheme the remote image fetcher downloads: http(s), gemini, spartan,
// gopher, nex.
bool has_image_fetch_scheme(std::string_view url);

// Link target that is an image file by its extension (query / fragment
// ignored; a CDN's "…@jpeg" format suffix counts too): URLs with has_image_fetch_scheme and local paths; other schemes
// ("mailto:a.png") are not.
bool is_image_url(std::string_view url);

// Case-insensitive suffix check (e.g. ".sexp").
bool path_ends_with_ci(std::string_view path, std::string_view suffix);


// Remove the last UTF-8 code point (no-op on an empty string).
void utf8_pop_back(std::string& s);

// Decode HTML character references to UTF-8: numeric &#NN; / &#xHH;, the
// XML five, Latin-1, lower-case Greek and common punctuation (&rsquo;
// &mdash; &hellip; …). Unknown names stay as written.
std::string html_decode_entities(std::string_view s);

// SHA-256 of `data` as 64 lower-case hex digits (content ids, revstore.hpp).
std::string sha256_hex(std::string_view data);

// Charset of a fetched body: the Content-Type "charset=" parameter, else
// <meta charset> / http-equiv / <?xml encoding?> near the top. Lower-case,
// empty when undeclared.
std::string detect_charset(std::string_view content_type,
                           std::string_view body);

// `body` as UTF-8. Valid UTF-8 is kept whatever the label says; otherwise
// Latin-1 / Windows-1252 / undeclared bytes are converted. Other code
// pages are returned unchanged.
std::string to_utf8(std::string body, std::string_view charset);
// Well-formed UTF-8 (structure only: lead bytes and continuation bytes).
bool is_valid_utf8(std::string_view s);

// Whether `head` (the start of a file) is binary data rather than text: it
// holds a NUL byte, or more than one in ten bytes are control characters
// other than tab, line breaks, form feed and escape. Legacy 8-bit text
// counts as text.
bool looks_like_binary(std::string_view head);

// Write a config / state file atomically (gamepkgstd DIR-14): <path>.tmp
// in the same directory (created if missing), flushed, renamed over
// <path>. False, with an error naming the path on stderr, on failure.
bool write_file_atomic(const std::string& path, std::string_view content);

// One bookmark: an address (docs/PASSAGES.md), which may name a passage, and
// what it showed when it was made.
struct Bookmark {
  std::string address;
  std::string title;    // section or page it was made in
  std::string quote;    // the passage text then (display, re-anchoring)
  std::string created;  // YYYY-MM-DD
  std::string folder;   // "/"-separated folder path, "" = top level
};

// File line: the address, then optional tab-separated key=value fields
// (title, quote, at, folder), so plain one-URL-per-line files stay valid.
// Bookmark folders: paths like "Video/YouTube". Every folder the user made
// is listed explicitly (empty ones too); a bookmark's folder and its
// ancestors are always in that list (bookmark_folder_list).
//
// Trims each part, drops empty parts and control characters: " a // b/" →
// "a/b".
std::string normalize_bookmark_folder(std::string_view path);
// "a/b/c" → "a/b"; "a" → "".
std::string bookmark_folder_parent(std::string_view path);
// "a/b/c" → "c".
std::string_view bookmark_folder_leaf(std::string_view path);
// `path` is `folder` or lies inside it ("" contains everything).
bool bookmark_folder_within(std::string_view path, std::string_view folder);
// Orders folder paths part by part, case-insensitively: "a", "a/b", "B".
bool bookmark_folder_less(std::string_view a, std::string_view b);
// `folders` plus the folders of `marks`, with all ancestors, normalized,
// without "", sorted (bookmark_folder_less) and unique.
std::vector<std::string> bookmark_folder_list(
    const std::vector<Bookmark>& marks, const std::vector<std::string>& folders);
// Moves folder `from` with everything in it to `to` (rename and re-parent
// alike). False (nothing changed) when `from` is empty or `to` lies inside
// `from`. Changed bookmarks' indices go to `changed`.
bool bookmark_folder_move(std::vector<Bookmark>& marks,
                          std::vector<std::string>& folders,
                          std::string_view from, std::string_view to,
                          std::vector<size_t>* changed = nullptr);
// Removes folder `path`; its bookmarks and subfolders move up one level.
void bookmark_folder_remove(std::vector<Bookmark>& marks,
                            std::vector<std::string>& folders,
                            std::string_view path,
                            std::vector<size_t>* changed = nullptr);

// Empty for blank lines and # comments.
std::optional<Bookmark> parse_bookmark_line(std::string_view line);
std::string format_bookmark_line(const Bookmark& b);
// "Section — “quote”" for a passage, else the title or the address.
std::string bookmark_label(const Bookmark& b);

// Stored in statedb.hpp (stored_bookmarks, store_bookmark_add / _remove).

}  // namespace xanatoo
