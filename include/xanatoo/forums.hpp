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
//   bsky:HANDLE/feed/RKEY[?cursor=C]   custom feed     (feed generator)
//   bsky:hashtag/TAG[?cursor=C]        top posts with #TAG (TAG percent-encoded)
//   flarum:HOST                        recent discussions (?page=N)
//   flarum:HOST/d/ID[-slug][?page=N]   discussion
//   lesswrong:                         frontpage       (new, top, curated, quicktakes)
//   lesswrong:posts/ID[/slug]          post + comments
//   lesswrong:users/SLUG               profile + posts
//   lesswrong:w/SLUG                   tag / wiki page + posts
//   lesswrong:search?query=Q           posts, tags and users found
//   digg:[TOPIC[/VIEW]]                Digg feed       (also TOPIC/top/RANGE)
//   digg:search[/SORT]?query=Q         recent stories found
//   digg:post/UUID[?page=N]            story, comments, sources (20 a page)
//   digg:TOPIC/ID                      the same, by the id in its web address
//   digg:post/UUID/comments[/UUID]     more comments / replies to one
//
// Parsers are pure (JSON in, Document out) and tested with cut-down recorded
// replies; only forum_fetch touches the network.

#include "xanatoo/json_dom.hpp"
#include "xanatoo/types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace xanatoo {

enum class ForumSite {
  Discourse, Lobsters, FourChan, StackExchange, Bluesky, Flarum, LessWrong, Digg
};

struct ForumRef {
  ForumSite site = ForumSite::Discourse;
  std::string host;  // Discourse / Stack Exchange / Flarum: site host
  // Path after the host (no leading '/', no query / fragment), e.g.
  // "t/slug/123", "s/abc", "g/thread/1", "questions/42", "alice/post/3k".
  // Bluesky: "HANDLE", "HANDLE/post/RKEY", "HANDLE/feed/RKEY", "hashtag/TAG".
  std::string path;
  int page = 1;          // ?page=N
  std::string cursor;    // ?cursor=C (Bluesky and Digg lists)
  std::string query;     // ?query=Q, decoded (LessWrong, Digg search)
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
// Bluesky AppView: getAuthorFeed, getFeed or searchPosts (a hashtag) — a
// list of posts; `head` (may be null) is the getProfile reply or the
// generator view of getFeedGenerator, shown above them. getPostThread.
Document bsky_feed_document(const Json& reply, const ForumRef& ref,
                            const Json* head = nullptr);
Document bsky_thread_document(const Json& reply, const ForumRef& ref);
// Flarum JSON:API: /api/discussions; /api/discussions/ID (head) + /api/posts.
Document flarum_list_document(const Json& reply, const ForumRef& ref);
Document flarum_discussion_document(const Json& discussion, const Json& posts,
                                    const ForumRef& ref);
// Digg: the tools of its public read-only MCP server. digg_mcp_decode takes
// the reply out of its envelope (JSON or event stream); digg_*_arguments are
// the JSON arguments of the tool calls.
constexpr int kDiggSourcePageSize = 20;  // the tool's maximum
constexpr int kDiggMaxSourcePage = 25;   // sourceOffset ends at 499
std::optional<Json> digg_mcp_decode(std::string_view body, std::string* err = nullptr);
std::string digg_feed_arguments(const ForumRef& ref);
std::string digg_search_arguments(const ForumRef& ref);
std::string digg_post_arguments(std::string_view id, int page);
std::string digg_comments_arguments(std::string_view post, const ForumRef& ref);
std::string digg_replies_arguments(std::string_view comment, const ForumRef& ref);
// What the tools leave out, read from digg.com pages: story UUID → path of
// its picture on the CDN; the article of a story page.
using DiggImages = std::unordered_map<std::string, std::string>;
struct DiggWebStory {
  std::string id;  // UUID ("": not a story page)
  std::string image, image_credit;
  std::string article_html;
};
DiggImages digg_web_images(std::string_view html);
DiggWebStory digg_web_story(std::string_view html);
// Comment UUID → its digg_get_comment_replies reply.
using DiggReplies = std::unordered_map<std::string, Json>;
// The optional parts of a story page.
struct DiggPost {
  std::optional<Json> comments;
  DiggReplies replies;
  DiggWebStory web;
};
Document digg_list_document(const Json& reply, const ForumRef& ref,
                            const DiggImages& images = {});
Document digg_post_document(const Json& reply, const ForumRef& ref,
                            const DiggPost& extra = {});
Document digg_comments_document(const Json& reply, const ForumRef& ref,
                                const DiggReplies& replies = {});
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
