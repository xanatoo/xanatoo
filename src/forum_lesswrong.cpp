// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
//
// LessWrong (ForumMagnum): the public GraphQL API at /graphql answers the
// listings (posts by view, quick takes), a post with all its comments, user
// and tag (wiki) pages; /api/search (Algolia-style multi-query, served by
// the site itself) finds posts, tags and users. Comments arrive flat with
// parentCommentId and are put in tree order here, best first.

#include "forum_common.hpp"
#include "xanatoo/util.hpp"

#include <map>
#include <set>

namespace xanatoo {

using namespace forum;

namespace {

constexpr const char* kBase = "https://www.lesswrong.com/";

std::string lw_ref(std::string path, std::string frag = {}) {
  ForumRef r;
  r.site = ForumSite::LessWrong;
  r.path = std::move(path);
  r.fragment = std::move(frag);
  return forum_ref_string(r);
}

std::string post_ref(const Json& p, std::string frag = {}) {
  std::string path = "posts/" + str(p, "_id");
  if (const std::string slug = str(p, "slug"); !slug.empty()) path += "/" + slug;
  return lw_ref(path, std::move(frag));
}

std::string user_ref(std::string_view slug) {
  return slug.empty() ? std::string() : lw_ref("users/" + std::string(slug));
}

std::vector<NavItem> nav_for(const ForumRef& ref) {
  return {{"Frontpage", lw_ref(""), ref.path.empty()},
          {"New", lw_ref("new"), ref.path == "new"},
          {"Top", lw_ref("top"), ref.path == "top"},
          {"Curated", lw_ref("curated"), ref.path == "curated"},
          {"Quick takes", lw_ref("quicktakes"), ref.path == "quicktakes"}};
}

// GraphQL replies: data/<field>/results (or /result); a list of errors
// instead of data is reported by the fetcher.
const std::vector<Json>& results(const Json& reply, std::string_view field) {
  static const std::vector<Json> kEmpty;
  const Json* r = json_path(reply, "data/" + std::string(field) + "/results");
  return r && r->type == Json::Type::Array ? r->items : kEmpty;
}

const Json* result(const Json& reply, std::string_view field) {
  const Json* r = json_path(reply, "data/" + std::string(field) + "/result");
  return r && r->type == Json::Type::Object ? r : nullptr;
}

std::string url_host(const std::string& url) {
  size_t s = url.find("://");
  if (s == std::string::npos) return {};
  s += 3;
  const size_t e = url.find_first_of("/?#:", s);
  std::string h = url.substr(s, e == std::string::npos ? std::string::npos : e - s);
  if (starts_with(h, "www.")) h.erase(0, 4);
  return h;
}

std::string karma(long long n) { return std::to_string(n) + " karma"; }

void add_date(Document& doc, NodeId cap, const std::string& iso) {
  if (iso.empty()) return;
  add_sep(doc, cap);
  InlineSpan date;
  date.role = InlineRole::Metadata;
  date.text = short_time(iso);
  date.datetime = iso;
  doc.get(cap).spans.push_back(std::move(date));
}

void add_tags(Document& doc, NodeId cap, const Json& post) {
  for (const Json& t : arr(post, "tags")) {
    const std::string slug = str(t, "slug");
    if (slug.empty()) continue;
    add_sep(doc, cap);
    add_link(doc, cap, str(t, "name"), lw_ref("w/" + slug), InlineRole::Metadata);
  }
}

// Author names of a post (user + coauthors) as links.
void add_authors(Document& doc, NodeId cap, const Json& post) {
  const Json* u = json_get(post, "user");
  if (u && u->type == Json::Type::Object)
    add_link(doc, cap, str(*u, "displayName"), user_ref(str(*u, "slug")),
             InlineRole::Metadata);
  for (const Json& c : arr(post, "coauthors")) {
    add_meta(doc, cap, ", ");
    add_link(doc, cap, str(c, "displayName"), user_ref(str(c, "slug")),
             InlineRole::Metadata);
  }
}

// One post of a listing: title line + caption.
void add_post_entry(Document& doc, NodeId thread, const Json& p) {
  NodeId art = doc.create(NodeKind::Article, thread);
  doc.get(art).label = str(p, "_id");
  NodeId head = doc.create(NodeKind::Header, art);
  std::string title = str(p, "title");
  if (json_get(p, "question") && json_get(p, "question")->b) title = "[Question] " + title;
  add_link(doc, head, title, post_ref(p));
  if (const std::string host = url_host(str(p, "url")); !host.empty())
    add_meta(doc, head, " (" + host + ")");
  NodeId cap = doc.create(NodeKind::Caption, art);
  add_meta(doc, cap, karma(num(p, "baseScore")) + " · ");
  add_authors(doc, cap, p);
  add_date(doc, cap, str(p, "postedAt"));
  add_tags(doc, cap, p);
  add_sep(doc, cap);
  const long long n = num(p, "commentCount");
  add_link(doc, cap, n > 0 ? count(n, "comment", "comments") : "discuss",
           post_ref(p), InlineRole::Metadata);
}

MsgHead comment_head(const Json& c, const std::string& permalink) {
  MsgHead h;
  h.label = str(c, "_id");
  const bool gone = json_get(c, "deleted") && json_get(c, "deleted")->b;
  const Json* u = json_get(c, "user");
  if (!gone && u && u->type == Json::Type::Object) {
    h.author = str(*u, "displayName");
    h.author_href = user_ref(str(*u, "slug"));
  }
  h.iso = str(c, "postedAt");
  h.permalink = permalink;
  h.score = karma(num(c, "baseScore"));
  return h;
}

void add_comment_body(Document& doc, NodeId msg, const Json& c) {
  if (json_get(c, "deleted") && json_get(c, "deleted")->b) {
    add_note(doc, msg, "[deleted]");
    return;
  }
  const std::string html = json_string(json_path(c, "contents/html"));
  if (!html.empty()) append_html(doc, msg, html, kBase);
}

// Links to lesswrong.com stay native; comments of `post_id` shown on this
// page become in-page anchors.
void native_links(Document& doc, const std::string& post_id,
                  const std::set<std::string>& here) {
  map_links(doc, [&](std::string& href) {
    std::string r = forum_ref_from_web_url(href);
    if (r.empty() || !starts_with(r, "lesswrong:")) return;
    if (!post_id.empty() && starts_with(r, "lesswrong:posts/" + post_id)) {
      const size_t h = r.find("#id:");
      if (h != std::string::npos && here.count(r.substr(h + 4))) {
        href = r.substr(h);
        return;
      }
    }
    href = r;
  });
}

}  // namespace

Document lesswrong_list_document(const Json& reply, const ForumRef& ref) {
  std::string what = "Frontpage";
  if (ref.path == "new") what = "New";
  else if (ref.path == "top") what = "Top";
  else if (ref.path == "curated") what = "Curated";
  Document doc;
  NodeId thread = start_document(doc, "LessWrong — " + what, "LessWrong",
                                 nav_for(ref), "lesswrong");
  const std::vector<Json>& posts = results(reply, "posts");
  for (const Json& p : posts) add_post_entry(doc, thread, p);
  if (posts.empty()) add_note(doc, doc.root, "No posts.");
  add_pager(doc, ref, posts.size() >= kLessWrongPageSize);
  native_links(doc, {}, {});
  add_web_link(doc, doc.root, ref);
  return doc;
}

Document lesswrong_quicktakes_document(const Json& reply, const ForumRef& ref) {
  Document doc;
  NodeId thread = start_document(doc, "LessWrong — Quick takes", "LessWrong",
                                 nav_for(ref), "lesswrong");
  const std::vector<Json>& takes = results(reply, "comments");
  for (const Json& c : takes) {
    const Json* post = json_get(c, "post");
    const bool has_post = post && post->type == Json::Type::Object;
    const std::string link =
        has_post ? post_ref(*post, "id:" + str(c, "_id")) : std::string();
    NodeId msg = add_message(doc, thread, comment_head(c, link));
    add_comment_body(doc, msg, c);
    NodeId cap = doc.create(NodeKind::Caption, msg);
    const long long n = num(c, "descendentCount");
    add_link(doc, cap, n > 0 ? count(n, "reply", "replies") : "discuss", link,
             InlineRole::Metadata);
    if (has_post) {
      add_sep(doc, cap);
      add_link(doc, cap, str(*post, "title"), post_ref(*post), InlineRole::Metadata);
    }
  }
  if (takes.empty()) add_note(doc, doc.root, "No quick takes.");
  add_pager(doc, ref, takes.size() >= kLessWrongPageSize);
  native_links(doc, {}, {});
  add_web_link(doc, doc.root, ref);
  return doc;
}

Document lesswrong_post_document(const Json& reply, const ForumRef& ref) {
  const Json* p = result(reply, "post");
  Document doc;
  const std::string title = p ? str(*p, "title") : std::string();
  NodeId thread = start_document(doc, title.empty() ? "LessWrong" : title,
                                 "LessWrong", nav_for(ref), "lesswrong");
  if (!p) {
    add_note(doc, thread, "No such post.");
    add_web_link(doc, doc.root, ref);
    return doc;
  }
  const std::string id = str(*p, "_id");
  MsgHead head;
  head.label = id;
  if (const Json* u = json_get(*p, "user"); u && u->type == Json::Type::Object) {
    head.author = str(*u, "displayName");
    head.author_href = user_ref(str(*u, "slug"));
  }
  head.iso = str(*p, "postedAt");
  head.permalink = post_ref(*p);
  head.score = karma(num(*p, "baseScore"));
  NodeId post = add_message(doc, thread, head);
  NodeId hd = doc.create(NodeKind::Header, post);
  doc.append_text(hd, title, InlineKind::Strong);
  NodeId cap = doc.create(NodeKind::Caption, post);
  add_authors(doc, cap, *p);
  if (const std::string url = str(*p, "url"); !url.empty()) {
    add_sep(doc, cap);
    add_link(doc, cap, "Linkpost: " + url_host(url), url, InlineRole::Metadata);
  }
  add_tags(doc, cap, *p);
  const std::string html = json_string(json_path(*p, "contents/html"));
  if (!html.empty()) append_html(doc, post, html, kBase);

  // Tree order: children in reply order (best first), depth first.
  const std::vector<Json>& comments = results(reply, "comments");
  std::set<std::string> here;
  for (const Json& c : comments) here.insert(str(c, "_id"));
  std::map<std::string, std::vector<const Json*>> kids;
  for (const Json& c : comments) {
    std::string parent = str(c, "parentCommentId");
    if (!here.count(parent)) parent.clear();  // orphan → top level
    kids[parent].push_back(&c);
  }
  std::vector<const Json*> stack;
  auto push_kids = [&](const std::string& parent) {
    auto it = kids.find(parent);
    if (it == kids.end()) return;
    for (auto c = it->second.rbegin(); c != it->second.rend(); ++c) stack.push_back(*c);
  };
  push_kids("");
  std::set<std::string> done;
  while (!stack.empty()) {
    const Json& c = *stack.back();
    stack.pop_back();
    const std::string cid = str(c, "_id");
    if (!done.insert(cid).second) continue;
    MsgHead h = comment_head(c, post_ref(*p, "id:" + cid));
    h.reply_to = str(c, "parentCommentId");
    if (!here.count(h.reply_to)) h.reply_to = id;
    h.indent = true;
    NodeId msg = add_message(doc, thread, h);
    add_comment_body(doc, msg, c);
    push_kids(cid);
  }
  if (comments.empty() && num(*p, "commentCount") > 0)
    add_note(doc, doc.root, "The comments did not load.");
  native_links(doc, id, here);
  add_web_link(doc, doc.root, ref);
  return doc;
}

Document lesswrong_user_document(const Json& user, const Json& posts,
                                 const ForumRef& ref) {
  const Json* u = result(user, "user");
  Document doc;
  const std::string name = u ? str(*u, "displayName") : std::string();
  NodeId thread = start_document(doc, name.empty() ? "LessWrong" : name + " — LessWrong",
                                 "LessWrong", nav_for(ref), "lesswrong");
  if (!u) {
    add_note(doc, thread, "No such user.");
    add_web_link(doc, doc.root, ref);
    return doc;
  }
  if (ref.page <= 1) {
    NodeId hd = doc.create(NodeKind::Header, thread);
    doc.append_text(hd, name, InlineKind::Strong);
    NodeId cap = doc.create(NodeKind::Caption, thread);
    add_meta(doc, cap, karma(num(*u, "karma")) + " · " +
                           count(num(*u, "postCount"), "post", "posts") + " · " +
                           count(num(*u, "commentCount"), "comment", "comments"));
    if (const std::string since = str(*u, "createdAt"); !since.empty())
      add_meta(doc, cap, " · joined " + short_time(since).substr(0, 10));
    const std::string bio = json_string(json_path(*u, "biography/html"));
    if (!bio.empty()) append_html(doc, thread, bio, kBase);
  }
  const std::vector<Json>& list = results(posts, "posts");
  for (const Json& p : list) add_post_entry(doc, thread, p);
  if (list.empty()) add_note(doc, doc.root, "No posts.");
  add_pager(doc, ref, list.size() >= kLessWrongPageSize);
  native_links(doc, {}, {});
  add_web_link(doc, doc.root, ref);
  return doc;
}

Document lesswrong_tag_document(const Json& tag, const Json& posts,
                                const ForumRef& ref) {
  const std::vector<Json>& tags = results(tag, "tags");
  const Json* t = tags.empty() ? nullptr : &tags.front();
  Document doc;
  const std::string name = t ? str(*t, "name") : std::string();
  NodeId thread = start_document(doc, name.empty() ? "LessWrong" : name + " — LessWrong",
                                 "LessWrong", nav_for(ref), "lesswrong");
  if (!t) {
    add_note(doc, thread, "No such tag or wiki page.");
    add_web_link(doc, doc.root, ref);
    return doc;
  }
  if (ref.page <= 1) {
    NodeId hd = doc.create(NodeKind::Header, thread);
    doc.append_text(hd, name, InlineKind::Strong);
    NodeId cap = doc.create(NodeKind::Caption, thread);
    add_meta(doc, cap, count(num(*t, "postCount"), "post", "posts"));
    const std::string html = json_string(json_path(*t, "description/html"));
    if (!html.empty()) append_html(doc, thread, html, kBase);
  }
  const std::vector<Json>& list = results(posts, "posts");
  for (const Json& p : list) add_post_entry(doc, thread, p);
  if (list.empty() && num(*t, "postCount") > 0) add_note(doc, doc.root, "No more posts.");
  add_pager(doc, ref, list.size() >= kLessWrongPageSize);
  native_links(doc, {}, {});
  add_web_link(doc, doc.root, ref);
  return doc;
}

Document lesswrong_search_document(const Json& reply, const ForumRef& ref) {
  Document doc;
  NodeId thread = start_document(doc, "LessWrong — Search: " + ref.query,
                                 "LessWrong", nav_for(ref), "lesswrong");
  // One result object per index, in request order: posts, tags, users.
  const Json* posts = json_at(reply, 0);
  const Json* tags = json_at(reply, 1);
  const Json* users = json_at(reply, 2);
  if (posts) {
    NodeId cap = doc.create(NodeKind::Caption, thread);
    add_meta(doc, cap, count(num(*posts, "nbHits"), "post", "posts") + " found");
  }
  auto list_line = [&](const Json* res, const char* label, auto&& add) {
    if (!res || arr(*res, "hits").empty()) return;
    NodeId line = doc.create(NodeKind::Paragraph, thread);
    doc.append_text(line, std::string(label) + ": ", InlineKind::Strong);
    bool first = true;
    for (const Json& h : arr(*res, "hits")) {
      if (!first) doc.append_text(line, " · ");
      first = false;
      add(line, h);
    }
  };
  if (ref.page <= 1) {
    list_line(tags, "Tags", [&](NodeId line, const Json& h) {
      add_link(doc, line, str(h, "name"), lw_ref("w/" + str(h, "slug")));
    });
    list_line(users, "Users", [&](NodeId line, const Json& h) {
      add_link(doc, line, str(h, "displayName"), user_ref(str(h, "slug")));
    });
  }
  size_t n = 0;
  if (posts)
    for (const Json& h : arr(*posts, "hits")) {
      ++n;
      NodeId art = doc.create(NodeKind::Article, thread);
      const std::string id = str(h, "objectID");
      doc.get(art).label = id;
      std::string path = "posts/" + id;
      if (const std::string slug = str(h, "slug"); !slug.empty()) path += "/" + slug;
      NodeId head = doc.create(NodeKind::Header, art);
      add_link(doc, head, str(h, "title"), lw_ref(path));
      if (const std::string host = url_host(str(h, "url")); !host.empty())
        add_meta(doc, head, " (" + host + ")");
      NodeId cap = doc.create(NodeKind::Caption, art);
      add_meta(doc, cap, karma(num(h, "baseScore")) + " · ");
      add_link(doc, cap, str(h, "authorDisplayName"), user_ref(str(h, "authorSlug")),
               InlineRole::Metadata);
      add_date(doc, cap, str(h, "postedAt"));
      add_tags(doc, cap, h);
      add_sep(doc, cap);
      add_link(doc, cap, count(num(h, "commentCount"), "comment", "comments"),
               lw_ref(path), InlineRole::Metadata);
    }
  if (n == 0) add_note(doc, doc.root, "No posts found.");
  const long long pages = posts ? num(*posts, "nbPages") : 0;
  add_pager(doc, ref, ref.page < pages);
  add_web_link(doc, doc.root, ref);
  return doc;
}

}  // namespace xanatoo
