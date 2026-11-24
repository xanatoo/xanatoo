// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke
//
// Network side of the forum clients: which API URLs a ref needs, then the
// pure document mappers of forum_<site>.cpp. One request per page except
// Stack Exchange questions (question, answers, comments), Bluesky threads
// by handle (resolveHandle first), Flarum discussions (head + posts) and
// LessWrong user / tag pages (the id of the slug first). LessWrong asks
// its GraphQL API by POST.

#include "forum_common.hpp"

#if defined(XANATOO_FORUMS) && XANATOO_FORUMS

#include "xanatoo/http_status.hpp"
#include "xanatoo/text_escape.hpp"
#include "xanatoo/https.hpp"
#include "xanatoo/util.hpp"
#include "xanatoo/version.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>

namespace xanatoo {

namespace forum {

namespace {

bool digits(std::string_view s) {
  if (s.empty()) return false;
  for (char c : s)
    if (c < '0' || c > '9') return false;
  return true;
}

bool name_ok(std::string_view s) {
  if (s.empty()) return false;
  for (char c : s)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ||
          c == '.' || c == '~' || c == '/' || c == ':'))
      return false;
  return s.find("..") == std::string_view::npos;
}

std::vector<std::string> segments(const std::string& path) {
  std::vector<std::string> out;
  size_t p = 0;
  while (p <= path.size()) {
    const size_t sl = path.find('/', p);
    out.push_back(path.substr(p, sl == std::string::npos ? std::string::npos : sl - p));
    if (sl == std::string::npos) break;
    p = sl + 1;
  }
  return out;
}

std::optional<Document> bad(const ForumRef& ref, std::string* err) {
  if (err) *err = std::string("not a ") + forum_site_name(ref.site) +
                  " page: " + forum_ref_string(ref);
  return std::nullopt;
}

}  // namespace

std::optional<Json> get_json(const std::string& url, std::string* err) {
  static const std::string agent = user_agent("read-only forum client");
  std::cerr << "Forums: GET " << url << "\n";
  std::string e;
  auto resp = https_get_follow(url, agent, &e);
  if (!resp) {
    if (err) *err = e;
    return std::nullopt;
  }
  if (resp->status != 200) {
    if (err) *err = http_status_error(resp->status, url);
    return std::nullopt;
  }
  auto json = json_parse(resp->body);
  if (!json && err) *err = url + " sent a reply that is not JSON";
  return json;
}

std::optional<Document> discourse_fetch(const ForumRef& ref, std::string* err) {
  const std::string base = "https://" + ref.host + "/";
  if (!ref.path.empty() && !name_ok(ref.path)) return bad(ref, err);
  if (starts_with(ref.path, "t/")) {
    // t/ID, t/SLUG/ID, t/ID/N or t/SLUG/ID/N (N = post number).
    const auto seg = segments(ref.path);
    std::string id;
    long long post = 0;
    if (seg.size() >= 3 && digits(seg[2]) && !digits(seg[1])) {
      id = seg[2];
      if (seg.size() >= 4 && digits(seg[3])) post = std::atoll(seg[3].c_str());
    } else if (seg.size() >= 2 && digits(seg[1])) {
      id = seg[1];
      if (seg.size() >= 3 && digits(seg[2])) post = std::atoll(seg[2].c_str());
    }
    if (id.empty()) return bad(ref, err);
    // /t/ID/N.json starts near post N; the document works out its page.
    const std::string url =
        post > 1 && ref.page == 1
            ? base + "t/" + id + "/" + std::to_string(post) + ".json"
            : base + "t/" + id + ".json?page=" + std::to_string(ref.page);
    auto reply = get_json(url, err);
    if (!reply) return std::nullopt;
    return discourse_topic_document(*reply, ref);
  }
  const std::string path = ref.path.empty() ? "latest" : ref.path;
  std::string url = base + path + ".json";
  // Topic lists count pages from 0.
  if (ref.page > 1 && path != "categories") url += "?page=" + std::to_string(ref.page - 1);
  auto reply = get_json(url, err);
  if (!reply) return std::nullopt;
  return discourse_list_document(*reply, ref);
}

std::optional<Document> lobsters_fetch(const ForumRef& ref, std::string* err) {
  if (!ref.path.empty() && !name_ok(ref.path)) return bad(ref, err);
  if (starts_with(ref.path, "s/")) {
    const auto seg = segments(ref.path);
    if (seg.size() < 2 || seg[1].empty()) return bad(ref, err);
    auto reply = get_json("https://lobste.rs/s/" + seg[1] + ".json", err);
    if (!reply) return std::nullopt;
    return lobsters_story_document(*reply, ref);
  }
  std::string url = "https://lobste.rs/";
  if (ref.path.empty()) {
    url += ref.page > 1 ? "page/" + std::to_string(ref.page) : "hottest";
  } else {
    url += ref.path;
    if (ref.page > 1) url += "/page/" + std::to_string(ref.page);
  }
  auto reply = get_json(url + ".json", err);
  if (!reply) return std::nullopt;
  return lobsters_list_document(*reply, ref);
}

std::optional<Document> fourchan_fetch(const ForumRef& ref, std::string* err) {
  if (ref.path.empty()) {
    auto reply = get_json("https://a.4cdn.org/boards.json", err);
    if (!reply) return std::nullopt;
    return fourchan_boards_document(*reply);
  }
  const auto seg = segments(ref.path);
  for (char c : seg[0])
    if (!std::isalnum(static_cast<unsigned char>(c))) return bad(ref, err);
  if (seg.size() == 1) {
    auto reply = get_json("https://a.4cdn.org/" + seg[0] + "/catalog.json", err);
    if (!reply) return std::nullopt;
    return fourchan_catalog_document(*reply, ref);
  }
  if (seg.size() >= 3 && seg[1] == "thread" && digits(seg[2])) {
    auto reply = get_json("https://a.4cdn.org/" + seg[0] + "/thread/" + seg[2] + ".json", err);
    if (!reply) {
      if (err && err->find("404") != std::string::npos)
        *err = "This thread is gone (4chan threads expire; the board's archive "
               "is at https://boards.4chan.org/" + seg[0] + "/archive).";
      return std::nullopt;
    }
    return fourchan_thread_document(*reply, ref);
  }
  return bad(ref, err);
}

std::optional<Document> se_fetch(const ForumRef& ref, std::string* err) {
  const std::string api = "https://api.stackexchange.com/2.3/";
  const std::string site = "site=" + url_encode(ref.host);
  const auto seg = segments(ref.path);
  if (seg.size() >= 2 && seg[0] == "questions" && digits(seg[1])) {
    auto q = get_json(api + "questions/" + seg[1] + "?" + site + "&filter=withbody", err);
    if (!q) return std::nullopt;
    auto a = get_json(api + "questions/" + seg[1] + "/answers?" + site +
                          "&filter=withbody&sort=votes&order=desc&pagesize=30&page=" +
                          std::to_string(ref.page),
                      err);
    if (!a) return std::nullopt;
    // One request for the comments of the question and the answers shown;
    // without them the page still works.
    std::string ids = seg[1];
    for (const Json& item : arr(*a, "items"))
      ids += ";" + std::to_string(num(item, "answer_id"));
    std::string ignored;
    auto c = get_json(api + "posts/" + ids + "/comments?" + site +
                          "&filter=withbody&sort=creation&order=asc&pagesize=100",
                      &ignored);
    return se_question_document(*q, *a, c ? &*c : nullptr, ref);
  }
  std::string url = api + "questions?" + site +
                    "&sort=activity&order=desc&pagesize=30&page=" + std::to_string(ref.page);
  if (seg.size() == 3 && seg[0] == "questions" && seg[1] == "tagged")
    url += "&tagged=" + url_encode(seg[2]);
  else if (!ref.path.empty() && ref.path != "questions")
    return bad(ref, err);
  auto reply = get_json(url, err);
  if (!reply) return std::nullopt;
  return se_list_document(*reply, ref);
}

std::optional<Document> bsky_fetch(const ForumRef& ref, std::string* err) {
  const std::string api = "https://public.api.bsky.app/xrpc/";
  const auto seg = segments(ref.path);
  if (!name_ok(seg[0])) return bad(ref, err);
  if (seg.size() == 1) {
    std::string url = api + "app.bsky.feed.getAuthorFeed?limit=30&actor=" + url_encode(seg[0]);
    if (!ref.cursor.empty()) url += "&cursor=" + ref.cursor;  // kept encoded
    auto reply = get_json(url, err);
    if (!reply) return std::nullopt;
    return bsky_feed_document(*reply, ref);
  }
  if (seg.size() != 3 || seg[1] != "post" || !name_ok(seg[2])) return bad(ref, err);
  std::string did = seg[0];
  if (!starts_with(did, "did:")) {
    auto who = get_json(api + "com.atproto.identity.resolveHandle?handle=" + url_encode(did), err);
    if (!who) return std::nullopt;
    did = str(*who, "did");
    if (did.empty()) {
      if (err) *err = "Bluesky does not know the handle " + seg[0];
      return std::nullopt;
    }
  }
  auto reply = get_json(api + "app.bsky.feed.getPostThread?depth=10&parentHeight=20&uri=" +
                            url_encode("at://" + did + "/app.bsky.feed.post/" + seg[2]),
                        err);
  if (!reply) return std::nullopt;
  return bsky_thread_document(*reply, ref);
}

std::optional<Document> flarum_fetch(const ForumRef& ref, std::string* err) {
  const std::string api = "https://" + ref.host + "/api/";
  constexpr int kPerPage = 20;
  const std::string offset = "&page%5Boffset%5D=" + std::to_string((ref.page - 1) * kPerPage) +
                             "&page%5Blimit%5D=" + std::to_string(kPerPage);
  if (ref.path.empty()) {
    auto reply = get_json(api + "discussions?include=user%2Ctags&sort=-lastPostedAt" + offset, err);
    if (!reply) return std::nullopt;
    return flarum_list_document(*reply, ref);
  }
  const auto seg = segments(ref.path);
  if (seg.size() < 2 || seg[0] != "d") return bad(ref, err);
  const std::string id = seg[1].substr(0, seg[1].find('-'));
  if (!digits(id)) return bad(ref, err);
  // The head alone: its default includes would bring the first posts again.
  auto head = get_json(api + "discussions/" + id + "?include=", err);
  if (!head) return std::nullopt;
  auto posts = get_json(api + "posts?filter%5Bdiscussion%5D=" + id +
                            "&include=user&sort=number" + offset,
                        err);
  if (!posts) return std::nullopt;
  return flarum_discussion_document(*head, *posts, ref);
}

namespace {

const char* const kLwPostFields =
    "_id slug title postedAt baseScore commentCount url question "
    "user { slug displayName } coauthors { slug displayName } tags { name slug }";
const char* const kLwCommentFields =
    "_id parentCommentId postedAt baseScore deleted descendentCount "
    "user { slug displayName } contents { html }";

// POST `body` (JSON) to LessWrong; the reply parsed.
std::optional<Json> lw_post(const std::string& url, const std::string& body,
                            std::string* err) {
  static const std::string agent = user_agent("read-only forum client");
  std::cerr << "Forums: POST " << url << "\n";
  std::string e;
  auto resp = https_request("POST", url, agent,
                            "Content-Type: application/json\r\n"
                            "Accept: application/json\r\n",
                            body, &e);
  if (!resp) {
    if (err) *err = e;
    return std::nullopt;
  }
  auto json = json_parse(resp->body);
  if (resp->status != 200 && !(json && json_get(*json, "errors"))) {
    if (err) *err = http_status_error(resp->status, url);
    return std::nullopt;
  }
  if (!json && err) *err = url + " sent a reply that is not JSON";
  return json;
}

// One GraphQL query; GraphQL errors without data fail.
std::optional<Json> lw_query(const std::string& query, std::string* err) {
  auto reply = lw_post("https://www.lesswrong.com/graphql",
                       "{\"query\":" + json_quote(query) + "}", err);
  if (!reply) return std::nullopt;
  const Json* data = json_get(*reply, "data");
  if (!data || data->type != Json::Type::Object) {
    std::string msg = json_string(json_path(*reply, "errors/0/message"));
    if (err) *err = "LessWrong: " + (msg.empty() ? std::string("no data in the reply") : msg);
    return std::nullopt;
  }
  return reply;
}

// posts(input: {terms: {view: …, limit, offset, <extra>}}).
std::string lw_posts_query(const std::string& view, int page, const std::string& extra) {
  return "{ posts(input: {terms: {view: " + json_quote(view) +
         ", limit: " + std::to_string(kLessWrongPageSize) +
         ", offset: " + std::to_string((page - 1) * static_cast<int>(kLessWrongPageSize)) +
         extra + "}}) { results { " + kLwPostFields + " } } }";
}

bool lw_name(std::string_view s) {
  if (s.empty() || s.size() > 200) return false;
  for (char c : s)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_'))
      return false;
  return true;
}

}  // namespace

std::optional<Document> lesswrong_fetch(const ForumRef& ref, std::string* err) {
  const auto seg = segments(ref.path);
  const int page = std::min(ref.page, 60);  // LessWrong caps offsets at 2000
  if (ref.path.empty() || ref.path == "new" || ref.path == "top" ||
      ref.path == "curated") {
    const std::string view = ref.path.empty() ? "magic" : ref.path;
    auto reply = lw_query(lw_posts_query(view, page, {}), err);
    if (!reply) return std::nullopt;
    return lesswrong_list_document(*reply, ref);
  }
  if (ref.path == "quicktakes") {
    auto reply = lw_query(
        "{ comments(input: {terms: {view: \"shortformFrontpage\", limit: " +
            std::to_string(kLessWrongPageSize) + ", offset: " +
            std::to_string((page - 1) * static_cast<int>(kLessWrongPageSize)) +
            "}}) { results { " + kLwCommentFields + " post { _id slug title } } } }",
        err);
    if (!reply) return std::nullopt;
    return lesswrong_quicktakes_document(*reply, ref);
  }
  if (seg[0] == "posts" && seg.size() >= 2 && lw_name(seg[1])) {
    const std::string id = json_quote(seg[1]);
    auto reply = lw_query(
        "{ post(input: {selector: {_id: " + id + "}}) { result { " + kLwPostFields +
            " contents { html } } } comments(input: {terms: {view: \"postCommentsTop\", "
            "postId: " + id + ", limit: 1000}}) { results { " + kLwCommentFields + " } } }",
        err);
    if (!reply) return std::nullopt;
    return lesswrong_post_document(*reply, ref);
  }
  if (seg[0] == "users" && seg.size() == 2 && lw_name(seg[1])) {
    auto user = lw_query("{ user(input: {selector: {slug: " + json_quote(seg[1]) +
                             "}}) { result { _id slug displayName karma postCount "
                             "commentCount createdAt biography { html } } } }",
                         err);
    if (!user) return std::nullopt;
    const std::string uid = json_string(json_path(*user, "data/user/result/_id"));
    Json posts;
    if (!uid.empty()) {
      auto p = lw_query(lw_posts_query("userPosts", page,
                                       ", userId: " + json_quote(uid) + ", sortedBy: \"new\""),
                        err);
      if (!p) return std::nullopt;
      posts = std::move(*p);
    }
    return lesswrong_user_document(*user, posts, ref);
  }
  if (seg[0] == "w" && seg.size() == 2 && lw_name(seg[1])) {
    auto tag = lw_query("{ tags(input: {terms: {view: \"tagBySlug\", slug: " +
                            json_quote(seg[1]) +
                            "}}) { results { _id name slug postCount description { html } } } }",
                        err);
    if (!tag) return std::nullopt;
    const std::string tid = json_string(json_path(*tag, "data/tags/results/0/_id"));
    Json posts;
    if (!tid.empty()) {
      auto p = lw_query(lw_posts_query("tagRelevance", page, ", tagId: " + json_quote(tid)),
                        err);
      if (!p) return std::nullopt;
      posts = std::move(*p);
    }
    return lesswrong_tag_document(*tag, posts, ref);
  }
  if (ref.path == "search") {
    if (ref.query.empty()) {
      if (err) *err = "LessWrong search needs a query: lesswrong:search?query=WORDS";
      return std::nullopt;
    }
    const std::string q = json_quote(ref.query);
    auto index = [&](const char* name, int hits, int pg) {
      return std::string("{\"indexName\":\"") + name + "\",\"params\":{\"query\":" + q +
             ",\"hitsPerPage\":" + std::to_string(hits) +
             ",\"page\":" + std::to_string(pg) + "}}";
    };
    const std::string body = "[" + index("posts", kLessWrongPageSize, page - 1) + "," +
                             index("tags", 10, 0) + "," + index("users", 10, 0) + "]";
    auto reply = lw_post("https://www.lesswrong.com/api/search", body, err);
    if (!reply) return std::nullopt;
    if (reply->type != Json::Type::Array) {
      if (err) *err = "LessWrong search sent an unexpected reply";
      return std::nullopt;
    }
    return lesswrong_search_document(*reply, ref);
  }
  return bad(ref, err);
}

// Digg's public MCP server (Streamable HTTP): anonymous, read-only tools.
std::optional<Json> digg_mcp_call(std::string_view tool, std::string_view arguments,
                                  std::string* err) {
  const std::string body = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                           "\"params\":{\"name\":" + json_quote(tool) +
                           ",\"arguments\":" + std::string(arguments) + "}}";
  static const std::string agent = user_agent("read-only forum client");
  std::cerr << "Forums: POST https://digg.com/mcp " << tool << "\n";
  HttpRequestSecurity security;
  security.require_https = true;
  security.public_only = true;
  security.max_response_bytes = 4 * 1024 * 1024;
  std::string e;
  auto result = https_request("POST", "https://digg.com/mcp", agent,
                              "Content-Type: application/json\r\n"
                              "Accept: application/json, text/event-stream\r\n"
                              "MCP-Protocol-Version: 2025-06-18\r\n",
                              body, &e, security);
  if (!result) {
    if (err) *err = "Digg: " + e;
    return std::nullopt;
  }
  if (result->status != 200) {
    if (err) *err = http_status_error(result->status, "https://digg.com/mcp");
    return std::nullopt;
  }
  return digg_mcp_decode(result->body, err);
}

// A digg.com page, for what the tools leave out ("" when it cannot be had).
std::string digg_page(const std::string& url) {
  static const std::string agent = user_agent("read-only forum client");
  std::cerr << "Forums: GET " << url << "\n";
  std::string e;
  auto resp = https_get_follow(url, agent, &e);
  return resp && resp->status == 200 ? std::move(resp->body) : std::string();
}

// The replies to the first answered comments, one call each.
DiggReplies digg_fetch_replies(const Json& comments, const ForumRef& ref) {
  constexpr size_t kMaxCalls = 8;
  DiggReplies out;
  for (const Json& c : arr(comments, "comments")) {
    if (num(c, "replyCount") <= 0) continue;
    if (out.size() >= kMaxCalls) break;
    const std::string id = str(c, "id");
    auto r = digg_mcp_call("digg_get_comment_replies",
                           digg_replies_arguments(id, ref), nullptr);
    if (!r) break;
    out.emplace(id, std::move(*r));
  }
  return out;
}

std::optional<Document> digg_fetch_story(const ForumRef& ref, const DiggPath& p,
                                         std::string* err) {
  DiggPost extra;
  std::string id(p.id);
  if (p.kind == DiggPath::Kind::Story) {
    extra.web = digg_web_story(digg_page(digg_web_url(ref)));
    id = extra.web.id;
    if (id.empty()) {
      if (err) *err = "no such Digg story: " + std::string(p.id);
      return std::nullopt;
    }
  }
  auto reply = digg_mcp_call("digg_get_post", digg_post_arguments(id, ref.page), err);
  if (!reply) return std::nullopt;
  if (ref.page > 1) return digg_post_document(*reply, ref);
  if (const Json* story = json_get(*reply, "story")) {
    id = str(*story, "id");
    const std::string url = str(*story, "url");
    if (extra.web.id.empty() && starts_with(url, "https://digg.com/"))
      extra.web = digg_web_story(digg_page(url));
  }
  if (extra.web.id != id) extra.web = {};
  extra.comments = digg_mcp_call("digg_get_post_comments",
                                 digg_comments_arguments(id, ref), nullptr);
  if (extra.comments) extra.replies = digg_fetch_replies(*extra.comments, ref);
  return digg_post_document(*reply, ref, extra);
}

std::optional<Document> digg_fetch_feed(const ForumRef& ref, const DiggPath& p,
                                        std::string* err) {
  auto reply = digg_mcp_call("digg_get_feed", digg_feed_arguments(ref), err);
  // Two topics are also published as plain JSON.
  if (!reply && ref.cursor.empty() && (p.topic == "ai" || p.topic == "tech") &&
      (p.view == "trending" || p.view == "top")) {
    reply = get_json(p.topic == "ai" ? "https://digg.com/ai-clusters-ranked.json"
                                     : "https://digg.com/tech/clusters.json",
                     nullptr);
    if (reply && err) err->clear();
  }
  if (!reply) return std::nullopt;
  DiggImages images;
  if (ref.cursor.empty()) images = digg_web_images(digg_page(digg_web_url(ref)));
  return digg_list_document(*reply, ref, images);
}

std::optional<Document> digg_fetch(const ForumRef& ref, std::string* err) {
  using Kind = DiggPath::Kind;
  const DiggPath p = digg_path(ref.path);
  std::optional<Json> reply;
  switch (p.kind) {
    case Kind::Invalid: return bad(ref, err);
    case Kind::Feed: return digg_fetch_feed(ref, p, err);
    case Kind::Post:
    case Kind::Story: return digg_fetch_story(ref, p, err);
    case Kind::Search:
      if (ref.query.size() < 3 || ref.query.size() > 120) {
        if (err) *err = "Digg searches for 3 to 120 characters";
        return std::nullopt;
      }
      reply = digg_mcp_call("digg_search_posts", digg_search_arguments(ref), err);
      if (!reply) return std::nullopt;
      return digg_list_document(*reply, ref);
    case Kind::Comments:
      reply = digg_mcp_call("digg_get_post_comments",
                            digg_comments_arguments(p.id, ref), err);
      if (!reply) return std::nullopt;
      return digg_comments_document(*reply, ref, digg_fetch_replies(*reply, ref));
    case Kind::Replies:
      reply = digg_mcp_call("digg_get_comment_replies",
                            digg_replies_arguments(p.comment, ref), err);
      if (!reply) return std::nullopt;
      return digg_comments_document(*reply, ref);
  }
  return bad(ref, err);
}

}  // namespace forum

std::optional<Document> forum_fetch(std::string_view ref, std::string* err) {
  auto r = parse_forum_ref(ref);
  if (!r) {
    if (err) *err = "not a forum address: " + std::string(ref);
    return std::nullopt;
  }
  switch (r->site) {
    case ForumSite::Discourse: return forum::discourse_fetch(*r, err);
    case ForumSite::Lobsters: return forum::lobsters_fetch(*r, err);
    case ForumSite::FourChan: return forum::fourchan_fetch(*r, err);
    case ForumSite::StackExchange: return forum::se_fetch(*r, err);
    case ForumSite::Bluesky: return forum::bsky_fetch(*r, err);
    case ForumSite::Flarum: return forum::flarum_fetch(*r, err);
    case ForumSite::LessWrong: return forum::lesswrong_fetch(*r, err);
    case ForumSite::Digg: return forum::digg_fetch(*r, err);
  }
  return std::nullopt;
}

}  // namespace xanatoo

#endif  // XANATOO_FORUMS
