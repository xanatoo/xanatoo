// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Shared document helpers of the forum clients (forum_*.cpp). Private to src/.

#include "xanatoo/forums.hpp"
#include "xanatoo/json_dom.hpp"
#include "xanatoo/types.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace xanatoo::forum {

// Member `key` of `v` as text / integer ("" / 0 when absent or another type).
std::string str(const Json& v, std::string_view key);
long long num(const Json& v, std::string_view key);
// Array member (or an empty array).
const std::vector<Json>& arr(const Json& v, std::string_view key);

// Unix seconds → "2024-01-15T12:34:56Z"; "" for 0.
std::string iso_from_unix(int64_t t);
// Any ISO 8601 instant (offsets allowed) → "2024-01-15 12:34 UTC".
std::string short_time(std::string_view iso);
// "1 point" / "2 points".
std::string count(long long n, std::string_view one, std::string_view many);
// Percent-encode a query value (unreserved characters kept).
std::string url_encode(std::string_view s);
// JSON / GraphQL string literal with quotes.
std::string json_quote(std::string_view s);

void add_text(Document& doc, NodeId node, const std::string& text,
              InlineKind kind = InlineKind::Text);
void add_link(Document& doc, NodeId node, const std::string& text,
              const std::string& href, InlineRole role = InlineRole::Normal);
void add_meta(Document& doc, NodeId node, const std::string& text);
// " · " when `node` already has spans.
void add_sep(Document& doc, NodeId node);

// The fields of a Message header (see types.hpp, Message).
struct MsgHead {
  std::string label;     // message id (#id:LABEL)
  std::string author;    // shown name; "" → "[deleted]"
  std::string author_href;
  std::string iso;       // creation instant
  std::string permalink; // date link
  std::string reply_to;  // label of the message answered ("" = none)
  bool indent = false;   // thread under reply_to (alt), not only link it
  std::string score;     // "42 points" (extra metadata field)
};
NodeId add_message(Document& doc, NodeId parent, const MsgHead& h);

// Foreign HTML → blocks under `parent`, links resolved against `base`.
void append_html(Document& doc, NodeId parent, std::string_view html,
                 const std::string& base, bool images = true);
// Plain text (paragraphs at blank lines, line breaks kept).
void append_plain(Document& doc, NodeId parent, std::string_view text);

// Apply `fn` to every link href under the document (spans of all nodes).
void map_links(Document& doc, const std::function<void(std::string&)>& fn);

// New document: root, an optional nav line "Site: a · b" (`nav` pairs of
// text / ref, the current one bold), and a Thread. Returns the thread.
struct NavItem {
  std::string text, ref;
  bool current = false;
};
NodeId start_document(Document& doc, const std::string& title,
                      const std::string& site,
                      const std::vector<NavItem>& nav,
                      const std::string& thread_label);
// "← Newer · Page N · Older →" under the root.
void add_pager(Document& doc, const ForumRef& ref, bool has_next,
               const std::string& next_word = "More");
// The ref string of `ref` on another page.
std::string with_page(ForumRef ref, int page);

// "Open on Site (web)" link line (external, never rewritten to a ref).
void add_web_link(Document& doc, NodeId parent, const ForumRef& ref);

// An empty-listing note.
void add_note(Document& doc, NodeId parent, const std::string& text);

#if defined(XANATOO_FORUMS) && XANATOO_FORUMS
// forum_fetch.cpp: HTTPS GET + JSON parse; the site fetchers.
std::optional<Json> get_json(const std::string& url, std::string* err);
std::optional<Document> discourse_fetch(const ForumRef& ref, std::string* err);
std::optional<Document> lobsters_fetch(const ForumRef& ref, std::string* err);
std::optional<Document> fourchan_fetch(const ForumRef& ref, std::string* err);
std::optional<Document> se_fetch(const ForumRef& ref, std::string* err);
std::optional<Document> bsky_fetch(const ForumRef& ref, std::string* err);
std::optional<Document> flarum_fetch(const ForumRef& ref, std::string* err);
std::optional<Document> lesswrong_fetch(const ForumRef& ref, std::string* err);
#endif

}  // namespace xanatoo::forum
