// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
//
// Flarum (any instance) through its JSON:API: /api/discussions lists,
// /api/discussions/ID for the head and /api/posts?filter[discussion]=ID
// for the posts (contentHtml), users from `included`. Post mentions
// (https://HOST/d/ID/N) inside the same discussion become #id:N.

#include "forum_common.hpp"
#include "xanatoo/util.hpp"

#include <map>

namespace xanatoo {

using namespace forum;

namespace {

std::string fl_ref(const ForumRef& base, std::string path, int page = 1,
                   std::string frag = {}) {
  ForumRef r;
  r.site = ForumSite::Flarum;
  r.host = base.host;
  r.path = std::move(path);
  r.page = page;
  r.fragment = std::move(frag);
  return forum_ref_string(r);
}

struct User {
  std::string name, username;
};

std::map<std::string, User> users_of(const Json& reply) {
  std::map<std::string, User> out;
  for (const Json& inc : arr(reply, "included")) {
    if (str(inc, "type") != "users") continue;
    const Json* a = json_get(inc, "attributes");
    if (!a) continue;
    User u;
    u.username = str(*a, "username");
    u.name = str(*a, "displayName");
    if (u.name.empty()) u.name = u.username;
    out[str(inc, "id")] = u;
  }
  return out;
}

std::map<std::string, std::string> tags_of(const Json& reply) {
  std::map<std::string, std::string> out;
  for (const Json& inc : arr(reply, "included"))
    if (str(inc, "type") == "tags")
      if (const Json* a = json_get(inc, "attributes")) out[str(inc, "id")] = str(*a, "name");
  return out;
}

// id of relationship `name` (relationships/NAME/data/id).
std::string rel_id(const Json& item, std::string_view name) {
  const Json* rel = json_get(item, "relationships");
  const Json* r = rel ? json_get(*rel, name) : nullptr;
  const Json* d = r ? json_get(*r, "data") : nullptr;
  return d ? json_string(json_get(*d, "id")) : std::string();
}

}  // namespace

Document flarum_list_document(const Json& reply, const ForumRef& ref) {
  const auto users = users_of(reply);
  const auto tags = tags_of(reply);
  Document doc;
  NodeId thread = start_document(doc, ref.host + " — Discussions", ref.host,
                                 {{"Discussions", fl_ref(ref, ""), true}}, "flarum");
  const std::vector<Json>& items = arr(reply, "data");
  for (const Json& d : items) {
    const Json* a = json_get(d, "attributes");
    if (!a) continue;
    const std::string id = str(d, "id");
    NodeId art = doc.create(NodeKind::Article, thread);
    doc.get(art).label = id;
    NodeId head = doc.create(NodeKind::Header, art);
    add_link(doc, head, str(*a, "title"), fl_ref(ref, "d/" + id));
    if (json_get(*a, "isSticky") && json_get(*a, "isSticky")->b) add_meta(doc, head, " (sticky)");
    NodeId cap = doc.create(NodeKind::Caption, art);
    add_meta(doc, cap, count(std::max(0LL, num(*a, "commentCount") - 1), "reply", "replies"));
    if (auto u = users.find(rel_id(d, "user")); u != users.end()) {
      add_sep(doc, cap);
      add_meta(doc, cap, "by ");
      add_link(doc, cap, u->second.name, "https://" + ref.host + "/u/" + u->second.username,
               InlineRole::Metadata);
    }
    const std::string last = str(*a, "lastPostedAt");
    if (!last.empty()) {
      add_sep(doc, cap);
      InlineSpan date;
      date.role = InlineRole::Metadata;
      date.text = "last post " + short_time(last);
      date.datetime = last;
      doc.get(cap).spans.push_back(std::move(date));
    }
    if (const Json* rel = json_get(d, "relationships"))
      if (const Json* t = json_get(*rel, "tags"))
        for (const Json& tag : arr(*t, "data"))
          if (auto it = tags.find(str(tag, "id")); it != tags.end()) {
            add_sep(doc, cap);
            add_meta(doc, cap, it->second);
          }
  }
  if (items.empty()) add_note(doc, doc.root, "No discussions.");
  const Json* links = json_get(reply, "links");
  add_pager(doc, ref, links && !str(*links, "next").empty());
  add_web_link(doc, doc.root, ref);
  return doc;
}

Document flarum_discussion_document(const Json& discussion, const Json& posts,
                                    const ForumRef& ref) {
  const Json* data = json_get(discussion, "data");
  const Json* attrs = data ? json_get(*data, "attributes") : nullptr;
  const std::string id = data ? str(*data, "id") : std::string();
  const std::string title = attrs ? str(*attrs, "title") : std::string();
  const auto users = users_of(posts);
  Document doc;
  NodeId thread = start_document(doc, title.empty() ? ref.host : title, ref.host,
                                 {{"Discussions", fl_ref(ref, "")}}, "flarum");
  const std::string path = "d/" + id;
  for (const Json& p : arr(posts, "data")) {
    const Json* a = json_get(p, "attributes");
    if (!a || str(*a, "contentType") != "comment") continue;  // renames, tags, …
    const long long number = num(*a, "number");
    MsgHead h;
    h.label = std::to_string(number);
    if (auto u = users.find(rel_id(p, "user")); u != users.end()) {
      h.author = u->second.name;
      h.author_href = "https://" + ref.host + "/u/" + u->second.username;
    }
    h.iso = str(*a, "createdAt");
    h.permalink = fl_ref(ref, path, ref.page, "id:" + h.label);
    if (long long likes = num(*a, "likesCount"); likes > 0) h.score = count(likes, "like", "likes");
    NodeId msg = add_message(doc, thread, h);
    if (number == 1 && !title.empty())
      doc.append_text(doc.create(NodeKind::Header, msg), title, InlineKind::Strong);
    append_html(doc, msg, str(*a, "contentHtml"), "https://" + ref.host + "/");
  }
  if (doc.get(thread).children.empty()) add_note(doc, doc.root, "No posts on this page.");
  // Mentions of posts here jump; other discussions of the forum stay native.
  const std::string web = "https://" + ref.host + "/d/";
  map_links(doc, [&](std::string& href) {
    if (!starts_with(href, web)) return;
    std::string rest = href.substr(web.size());
    std::string did = rest.substr(0, rest.find_first_of("-/?#"));
    const size_t sl = rest.find('/');
    if (did == id && sl != std::string::npos) {
      href = "#id:" + rest.substr(sl + 1);
      return;
    }
    href = fl_ref(ref, "d/" + did);
  });
  const Json* links = json_get(posts, "links");
  add_pager(doc, ref, links && !str(*links, "next").empty(), "Next page");
  add_web_link(doc, doc.root, ref);
  return doc;
}

}  // namespace xanatoo
