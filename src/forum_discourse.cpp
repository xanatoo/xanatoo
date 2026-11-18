// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
//
// Discourse (any instance): every page has a .json twin. Topic lists
// (/latest, /top, /c/SLUG/ID, /categories) become Thread + Article
// entries; a topic (/t/SLUG/ID) is a flat Thread + Message list (Discourse
// shows replies chronologically; the reply-to post is a header link).

#include "forum_common.hpp"
#include "xanatoo/util.hpp"

#include <map>

namespace xanatoo {

using namespace forum;

namespace {

std::string ref_for(const ForumRef& base, std::string path, int page = 1,
                    std::string frag = {}) {
  ForumRef r;
  r.site = ForumSite::Discourse;
  r.host = base.host;
  r.path = std::move(path);
  r.page = page;
  r.fragment = std::move(frag);
  return forum_ref_string(r);
}

std::vector<NavItem> nav_for(const ForumRef& ref) {
  return {{"Latest", ref_for(ref, ""), ref.path.empty() || ref.path == "latest"},
          {"Top", ref_for(ref, "top"), ref.path == "top"},
          {"Categories", ref_for(ref, "categories"), ref.path == "categories"}};
}

std::string topic_path(const Json& t) {
  const std::string slug = str(t, "slug");
  const std::string id = std::to_string(num(t, "id"));
  return slug.empty() ? "t/" + id : "t/" + slug + "/" + id;
}

Document categories_document(const Json& reply, const ForumRef& ref) {
  Document doc;
  NodeId thread = start_document(doc, ref.host + " — Categories", ref.host,
                                 nav_for(ref), "discourse");
  const Json* list = json_path(reply, "category_list/categories");
  for (const Json& c : list ? list->items : std::vector<Json>{}) {
    NodeId art = doc.create(NodeKind::Article, thread);
    doc.get(art).label = "c" + std::to_string(num(c, "id"));
    add_link(doc, doc.create(NodeKind::Header, art),
             html_decode_entities(str(c, "name")),
             ref_for(ref, "c/" + str(c, "slug") + "/" + std::to_string(num(c, "id"))));
    const std::string desc = str(c, "description_text");
    if (!desc.empty()) append_plain(doc, art, html_decode_entities(desc));
    NodeId cap = doc.create(NodeKind::Caption, art);
    add_meta(doc, cap, count(num(c, "topic_count"), "topic", "topics"));
  }
  if (doc.get(thread).children.empty()) add_note(doc, doc.root, "No categories.");
  return doc;
}

}  // namespace

Document discourse_list_document(const Json& reply, const ForumRef& ref) {
  if (ref.path == "categories") return categories_document(reply, ref);
  std::map<long long, std::string> users;
  for (const Json& u : arr(reply, "users")) users[num(u, "id")] = str(u, "username");

  std::string what = "Latest";
  if (ref.path == "top") what = "Top";
  else if (starts_with(ref.path, "c/")) what = "Category " + ref.path.substr(2);
  Document doc;
  NodeId thread = start_document(doc, ref.host + " — " + what, ref.host,
                                 nav_for(ref), "discourse");
  const Json* list = json_get(reply, "topic_list");
  const std::vector<Json>& topics = arr(list ? *list : reply, "topics");
  for (const Json& t : topics) {
    NodeId art = doc.create(NodeKind::Article, thread);
    doc.get(art).label = std::to_string(num(t, "id"));
    NodeId head = doc.create(NodeKind::Header, art);
    add_link(doc, head, html_decode_entities(str(t, "title")), ref_for(ref, topic_path(t)));
    if (json_get(t, "pinned") && json_get(t, "pinned")->b) add_meta(doc, head, " (pinned)");
    std::string excerpt = html_decode_entities(str(t, "excerpt"));
    for (char& c : excerpt)
      if (c == '\n') c = ' ';
    if (!excerpt.empty()) append_plain(doc, art, excerpt);
    NodeId cap = doc.create(NodeKind::Caption, art);
    add_meta(doc, cap, count(std::max(0LL, num(t, "posts_count") - 1), "reply", "replies"));
    add_sep(doc, cap);
    add_meta(doc, cap, count(num(t, "views"), "view", "views"));
    if (long long likes = num(t, "like_count"); likes > 0) {
      add_sep(doc, cap);
      add_meta(doc, cap, count(likes, "like", "likes"));
    }
    // The original poster is the first of "posters".
    const std::vector<Json>& posters = arr(t, "posters");
    if (!posters.empty()) {
      auto it = users.find(num(posters[0], "user_id"));
      if (it != users.end()) {
        add_sep(doc, cap);
        add_meta(doc, cap, "by ");
        add_link(doc, cap, it->second, "https://" + ref.host + "/u/" + it->second,
                 InlineRole::Metadata);
      }
    }
    const std::string last = str(t, "last_posted_at");
    if (!last.empty()) {
      add_sep(doc, cap);
      InlineSpan date;
      date.role = InlineRole::Metadata;
      date.text = "last post " + short_time(last);
      date.datetime = last;
      doc.get(cap).spans.push_back(std::move(date));
    }
    if (long long cat = num(t, "category_id"); cat > 0) {
      add_sep(doc, cap);
      add_link(doc, cap, "category", ref_for(ref, "c/" + std::to_string(cat)),
               InlineRole::Metadata);
    }
  }
  if (topics.empty()) add_note(doc, doc.root, "No topics.");
  const bool more = list && !str(*list, "more_topics_url").empty();
  add_pager(doc, ref, more);
  add_web_link(doc, doc.root, ref);
  return doc;
}

Document discourse_topic_document(const Json& reply, const ForumRef& ref) {
  const std::string title = html_decode_entities(str(reply, "title"));
  const long long chunk = num(reply, "chunk_size");
  const long long per_page = chunk > 0 ? chunk : 20;
  // Pages are slices of the stream (every post id in order); post numbers
  // have gaps where posts were deleted, so the page comes from the stream.
  std::map<long long, long long> index;
  if (const Json* stream = json_path(reply, "post_stream/stream"))
    for (size_t i = 0; i < stream->items.size(); ++i)
      index[static_cast<long long>(stream->items[i].n)] = static_cast<long long>(i);
  const long long total = index.empty() ? num(reply, "posts_count")
                                        : static_cast<long long>(index.size());
  ForumRef topic = ref;
  topic.path = topic_path(reply);
  topic.fragment.clear();
  auto page_of = [&](const Json& p) {
    auto it = index.find(num(p, "id"));
    return it == index.end() ? topic.page : static_cast<int>(it->second / per_page + 1);
  };
  const Json* posts = json_path(reply, "post_stream/posts");
  if (posts && !posts->items.empty()) topic.page = page_of(posts->items[0]);
  Document doc;
  NodeId thread = start_document(doc, title.empty() ? ref.host : title, ref.host,
                                 nav_for(ref), "discourse");
  for (const Json& p : posts ? posts->items : std::vector<Json>{}) {
    const std::string cooked = str(p, "cooked");
    const long long number = num(p, "post_number");
    if (cooked.empty()) continue;  // moderator actions ("pinned", "closed")
    MsgHead h;
    h.label = std::to_string(number);
    h.author = str(p, "username");
    if (!h.author.empty()) h.author_href = "https://" + ref.host + "/u/" + h.author;
    h.iso = str(p, "created_at");
    h.permalink = ref_for(ref, topic.path, page_of(p), "id:" + h.label);
    if (long long to = num(p, "reply_to_post_number"); to > 0)
      h.reply_to = std::to_string(to);
    long long likes = 0;
    for (const Json& a : arr(p, "actions_summary"))
      if (num(a, "id") == 2) likes = num(a, "count");
    if (likes > 0) h.score = count(likes, "like", "likes");
    NodeId msg = add_message(doc, thread, h);
    if (number == 1 && !title.empty())
      doc.append_text(doc.create(NodeKind::Header, msg), title, InlineKind::Strong);
    append_html(doc, msg, cooked, "https://" + ref.host + "/");
  }
  if (doc.get(thread).children.empty()) add_note(doc, doc.root, "No posts on this page.");
  // Topic links of this instance stay native.
  const std::string web = "https://" + ref.host + "/";
  map_links(doc, [&](std::string& href) {
    if (!starts_with(href, web)) return;
    std::string_view rest = std::string_view(href).substr(web.size());
    if (starts_with(rest, "t/") || starts_with(rest, "c/")) href = "discourse:" + ref.host + "/" + std::string(rest);
  });
  add_pager(doc, topic, topic.page * per_page < total, "Next page");
  add_web_link(doc, doc.root, ref);
  return doc;
}

}  // namespace xanatoo
