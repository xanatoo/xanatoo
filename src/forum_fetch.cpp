// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke
//
// Network side of the forum clients: which API URLs a ref needs, then the
// pure document mappers of forum_<site>.cpp. One request per page except
// Stack Exchange questions (question, answers, comments), Bluesky threads
// by handle (resolveHandle first) and Flarum discussions (head + posts).

#include "forum_common.hpp"

#if defined(XANATOO_FORUMS) && XANATOO_FORUMS

#include "xanatoo/http_status.hpp"
#include "xanatoo/https.hpp"
#include "xanatoo/util.hpp"
#include "xanatoo/version.hpp"

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
  }
  return std::nullopt;
}

}  // namespace xanatoo

#endif  // XANATOO_FORUMS
