// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Read-only clients for discussion forums with public JSON APIs. Same shape
// as hn: / lemmy: — listings become Thread + Article entries, discussions
// Thread + Message nodes. No account, no posting. See docs/FORUMS.md.
//
//   discourse:HOST                     latest topics   (/latest, /top, /c/SLUG/ID)
//   discourse:HOST/t/SLUG/ID[?page=N]  topic (also /t/ID; /t/SLUG/ID/N → #id:N)
//   lobsters:                          hottest         (newest, active, t/TAG, ~USER)
//   lobsters:s/ID[/slug]               story + comments
//   fourchan:                          board list
//   fourchan:BOARD/                    board catalog
//   fourchan:BOARD/thread/NO           thread
//   stackexchange:HOST                 recent questions (?page=N, /questions/tagged/TAG)
//   stackexchange:HOST/questions/ID    question, answers and comments
//   bsky:HANDLE[?cursor=C]             author feed     (handle or did:plc:…)
//   bsky:HANDLE/post/RKEY              post thread
//   flarum:HOST                        recent discussions (?page=N)
//   flarum:HOST/d/ID[-slug][?page=N]   discussion
//   lesswrong:                         frontpage       (new, top, curated, quicktakes)
//   lesswrong:posts/ID[/slug]          post + comments
//   lesswrong:users/SLUG               profile + posts
//   lesswrong:w/SLUG                   tag / wiki page + posts
//   lesswrong:search?query=Q           posts, tags and users found
//   digg: / digg:TOPIC[/VIEW]          Digg feed (all topics or a topic)
//   digg:search?query=Q               recent published posts
//   digg:post/UUID                    post, summary and original sources
//
// Parsers are pure (JSON in, Document out) and tested with cut-down recorded
// replies; only forum_fetch touches the network.

#include "xanatoo/json_dom.hpp"
#include "xanatoo/types.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace xanatoo {

enum class ForumSite {
  Discourse, Lobsters, FourChan, StackExchange, Bluesky, Flarum, LessWrong, Digg
};

struct ForumRef {
  ForumSite site = ForumSite::Discourse;
  std::string host;  // Discourse / Stack Exchange / Flarum: site host
  // Path after the host (no leading '/', no query / fragment), e.g.
  // "t/slug/123", "s/abc", "g/thread/1", "questions/42", "alice/post/3k".
  // Bluesky: "HANDLE" or "HANDLE/post/RKEY".
  std::string path;
  int page = 1;          // ?page=N
  std::string cursor;    // ?cursor=C (Bluesky feeds page by cursor)
  std::string query;     // ?query=Q, decoded (LessWrong search)
  std::string fragment;  // after '#', kept for the address
};

std::optional<ForumRef> parse_forum_ref(std::string_view ref);
bool is_forum_ref(std::string_view ref);
std::string forum_ref_string(const ForumRef& r);
const char* forum_site_name(ForumSite s);
// Equivalent web page ("" when there is none).
std::string forum_web_url(const ForumRef& r);
// Web page of a site with a fixed host (lobste.rs, boards.4chan.org, Stack
// Exchange sites, bsky.app) → ref; "" otherwise. Discourse and Flarum run
// on any host, so their refs are only ever written explicitly.
std::string forum_ref_from_web_url(std::string_view url);
// One level up ("" at the top): topic → forum, thread → board, …
std::string forum_up_ref(const ForumRef& r);

// ---- documents (pure) ----
// Discourse: /latest.json-style topic list; /t/ID.json topic.
Document discourse_list_document(const Json& reply, const ForumRef& ref);
Document discourse_topic_document(const Json& reply, const ForumRef& ref);
// Lobsters: an array of stories; one story with "comments".
Document lobsters_list_document(const Json& reply, const ForumRef& ref);
Document lobsters_story_document(const Json& reply, const ForumRef& ref);
// fourchan: boards.json; BOARD/catalog.json; BOARD/thread/NO.json.
Document fourchan_boards_document(const Json& reply);
Document fourchan_catalog_document(const Json& reply, const ForumRef& ref);
Document fourchan_thread_document(const Json& reply, const ForumRef& ref);
// Stack Exchange API 2.3: /questions list; one question + its answers +
// the comments of all of them (`comments` may be null).
Document se_list_document(const Json& reply, const ForumRef& ref);
Document se_question_document(const Json& question, const Json& answers,
                              const Json* comments, const ForumRef& ref);
// Bluesky AppView: getAuthorFeed; getPostThread.
Document bsky_feed_document(const Json& reply, const ForumRef& ref);
Document bsky_thread_document(const Json& reply, const ForumRef& ref);
// Flarum JSON:API: /api/discussions; /api/discussions/ID (head) + /api/posts.
Document flarum_list_document(const Json& reply, const ForumRef& ref);
Document flarum_discussion_document(const Json& discussion, const Json& posts,
                                    const ForumRef& ref);
// Digg public MCP tools: structured results, no account or write operations.
// The MCP transport envelope (JSON or SSE) is decoded by digg_mcp_decode.
// Construct feed tool arguments separately so offline tests cover API limits.
std::string digg_feed_arguments(const ForumRef& ref);
std::string digg_search_arguments(const ForumRef& ref);
std::optional<Json> digg_mcp_decode(std::string_view body, std::string* err = nullptr);
Document digg_list_document(const Json& reply, const ForumRef& ref);
Document digg_post_document(const Json& reply, const ForumRef& ref);
// LessWrong: whole GraphQL replies ({"data": …}); /api/search multi-query
// reply (array: posts, tags, users). Listings ask for this many entries.
constexpr size_t kLessWrongPageSize = 30;
Document lesswrong_list_document(const Json& reply, const ForumRef& ref);
Document lesswrong_quicktakes_document(const Json& reply, const ForumRef& ref);
// data.post.result + data.comments.results in one reply.
Document lesswrong_post_document(const Json& reply, const ForumRef& ref);
Document lesswrong_user_document(const Json& user, const Json& posts,
                                 const ForumRef& ref);
Document lesswrong_tag_document(const Json& tag, const Json& posts,
                                const ForumRef& ref);
Document lesswrong_search_document(const Json& reply, const ForumRef& ref);

#if defined(XANATOO_FORUMS) && XANATOO_FORUMS
std::optional<Document> forum_fetch(std::string_view ref,
                                    std::string* err = nullptr);
#endif

}  // namespace xanatoo
