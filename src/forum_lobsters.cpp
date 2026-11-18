// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
//
// Lobsters (lobste.rs): listings are arrays of stories (/hottest.json,
// /newest.json, /t/TAG.json, /~USER/stories.json, …/page/N.json); a story
// (/s/ID.json) carries its comment tree in display order with depth and
// parent_comment, which become Message threading.

#include "forum_common.hpp"
#include "xanatoo/util.hpp"

#include <set>

namespace xanatoo {

using namespace forum;

namespace {

std::string lob_ref(std::string path, std::string frag = {}) {
  ForumRef r;
  r.site = ForumSite::Lobsters;
  r.path = std::move(path);
  r.fragment = std::move(frag);
  return forum_ref_string(r);
}

std::vector<NavItem> nav_for(const ForumRef& ref) {
  return {{"Hottest", lob_ref(""), ref.path.empty() || ref.path == "hottest"},
          {"Newest", lob_ref("newest"), ref.path == "newest"},
          {"Active", lob_ref("active"), ref.path == "active"}};
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

void add_user(Document& doc, NodeId cap, const std::string& user) {
  add_link(doc, cap, user, lob_ref("~" + user + "/stories"), InlineRole::Metadata);
}

void add_tags(Document& doc, NodeId cap, const Json& story) {
  for (const Json& t : arr(story, "tags")) {
    if (t.type != Json::Type::String) continue;
    add_sep(doc, cap);
    add_link(doc, cap, t.s, lob_ref("t/" + t.s), InlineRole::Metadata);
  }
}

// Title line: the linked article (or the story page) plus its host.
void add_title(Document& doc, NodeId parent, const Json& s, bool on_story) {
  NodeId head = doc.create(NodeKind::Header, parent);
  const std::string url = str(s, "url");
  const std::string title = str(s, "title");
  const std::string page = lob_ref("s/" + str(s, "short_id"));
  if (!url.empty()) add_link(doc, head, title, url);
  else if (on_story) doc.append_text(head, title, InlineKind::Strong);
  else add_link(doc, head, title, page);
  if (const std::string host = url_host(url); !host.empty())
    add_meta(doc, head, " (" + host + ")");
}

}  // namespace

Document lobsters_list_document(const Json& reply, const ForumRef& ref) {
  std::string what = "Hottest";
  if (ref.path == "newest") what = "Newest";
  else if (ref.path == "active") what = "Active";
  else if (starts_with(ref.path, "t/")) what = "Tag " + ref.path.substr(2);
  else if (starts_with(ref.path, "~")) what = ref.path.substr(1, ref.path.find('/') - 1);
  Document doc;
  NodeId thread = start_document(doc, "Lobsters — " + what, "Lobsters",
                                 nav_for(ref), "lobsters");
  const std::vector<Json>& stories =
      reply.type == Json::Type::Array ? reply.items : arr(reply, "stories");
  for (const Json& s : stories) {
    NodeId art = doc.create(NodeKind::Article, thread);
    doc.get(art).label = str(s, "short_id");
    add_title(doc, art, s, false);
    NodeId cap = doc.create(NodeKind::Caption, art);
    add_meta(doc, cap, count(num(s, "score"), "point", "points") + " by ");
    add_user(doc, cap, str(s, "submitter_user"));
    const std::string created = str(s, "created_at");
    if (!created.empty()) {
      add_sep(doc, cap);
      InlineSpan date;
      date.role = InlineRole::Metadata;
      date.text = short_time(created);
      date.datetime = created;
      doc.get(cap).spans.push_back(std::move(date));
    }
    add_tags(doc, cap, s);
    add_sep(doc, cap);
    const long long n = num(s, "comment_count");
    add_link(doc, cap, n > 0 ? count(n, "comment", "comments") : "discuss",
             lob_ref("s/" + str(s, "short_id")), InlineRole::Metadata);
  }
  if (stories.empty()) add_note(doc, doc.root, "No stories.");
  add_pager(doc, ref, stories.size() >= 25);
  add_web_link(doc, doc.root, ref);
  return doc;
}

Document lobsters_story_document(const Json& s, const ForumRef& ref) {
  const std::string id = str(s, "short_id");
  const std::string title = str(s, "title");
  Document doc;
  NodeId thread = start_document(doc, title.empty() ? "Lobsters" : title,
                                 "Lobsters", nav_for(ref), "lobsters");
  MsgHead head;
  head.label = id;
  head.author = str(s, "submitter_user");
  if (!head.author.empty()) head.author_href = lob_ref("~" + head.author + "/stories");
  head.iso = str(s, "created_at");
  head.permalink = lob_ref("s/" + id);
  head.score = count(num(s, "score"), "point", "points");
  NodeId story = add_message(doc, thread, head);
  add_title(doc, story, s, true);
  const std::string desc = str(s, "description");
  if (!desc.empty()) append_html(doc, story, desc, "https://lobste.rs/");
  NodeId cap = doc.create(NodeKind::Caption, story);
  add_meta(doc, cap, count(num(s, "comment_count"), "comment", "comments"));
  add_tags(doc, cap, s);

  for (const Json& c : arr(s, "comments")) {
    MsgHead h;
    h.label = str(c, "short_id");
    const bool gone = json_get(c, "is_deleted") && json_get(c, "is_deleted")->b;
    h.author = gone ? std::string() : str(c, "commenting_user");
    if (!h.author.empty()) h.author_href = lob_ref("~" + h.author + "/stories");
    h.iso = str(c, "created_at");
    h.permalink = lob_ref("s/" + id, "id:" + h.label);
    h.reply_to = str(c, "parent_comment");
    if (h.reply_to.empty()) h.reply_to = id;
    h.indent = true;
    h.score = count(num(c, "score"), "point", "points");
    NodeId msg = add_message(doc, thread, h);
    if (gone) add_note(doc, msg, "[deleted]");
    else append_html(doc, msg, str(c, "comment"), "https://lobste.rs/");
  }
  // Story and tag links on lobste.rs stay native; comment permalinks
  // (/c/ID or /s/ID/slug#c_ID) point into this page when they can.
  std::set<std::string> here;
  for (const Json& c : arr(s, "comments")) here.insert(str(c, "short_id"));
  map_links(doc, [&](std::string& href) {
    if (starts_with(href, "https://lobste.rs/c/") && here.count(href.substr(20))) {
      href = "#id:" + href.substr(20);
      return;
    }
    if (size_t c = href.find("#c_"); c != std::string::npos &&
        here.count(href.substr(c + 3))) {
      href = "#id:" + href.substr(c + 3);
      return;
    }
    if (std::string r = forum_ref_from_web_url(href);
        !r.empty() && starts_with(r, "lobsters:"))
      href = r;
  });
  add_web_link(doc, doc.root, ref);
  return doc;
}

}  // namespace xanatoo
