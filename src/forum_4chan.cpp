// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
//
// 4chan's read-only JSON API (a.4cdn.org, github.com/4chan/4chan-API):
// boards.json → board list, BOARD/catalog.json → one Article per thread,
// BOARD/thread/NO.json → a flat Message list. Post HTML is small (br,
// quote spans, quotelinks); ">>N" quotelinks become #id:N links and the
// first one is the message's reply-to. Images show as thumbnails
// (i.4cdn.org/BOARD/TIMs.jpg) with a link to the full file.

#include "forum_common.hpp"
#include "xanatoo/html_min.hpp"
#include "xanatoo/util.hpp"

#include <cstdio>

namespace xanatoo {

using namespace forum;

namespace {

std::string chan_ref(std::string path, std::string frag = {}) {
  ForumRef r;
  r.site = ForumSite::FourChan;
  r.path = std::move(path);
  r.fragment = std::move(frag);
  return forum_ref_string(r);
}

std::string board_of(const ForumRef& ref) {
  return ref.path.substr(0, ref.path.find('/'));
}

std::string tim_str(const Json& p) {
  const Json* t = json_get(p, "tim");
  if (!t || t->type != Json::Type::Number) return {};
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.0f", t->n);
  return buf;
}

std::string size_text(long long bytes) {
  if (bytes >= 1024 * 1024) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buf;
  }
  return std::to_string((bytes + 1023) / 1024) + " KB";
}

// A thumbnail linked to its full file: a click (and the gallery) shows the
// original, not the small "s.jpg".
void add_thumb(Document& doc, NodeId parent, const std::string& base,
               const std::string& ext) {
  NodeId im = doc.create(NodeKind::Image, parent);
  doc.get(im).src = base + "s.jpg";
  doc.get(im).float_side = "left";
  if (ext.empty()) return;
  InlineSpan link;
  link.kind = InlineKind::Link;
  link.href = base + ext;
  doc.get(im).spans.push_back(std::move(link));
}

// Thumbnail + "file.jpg (500 KB, 800×600)" link to the full file.
void add_file(Document& doc, NodeId parent, const Json& p, const std::string& board) {
  const std::string tim = tim_str(p);
  if (tim.empty()) return;
  const std::string base = "https://i.4cdn.org/" + board + "/" + tim;
  const std::string ext = str(p, "ext");
  if (num(p, "filedeleted") == 0) add_thumb(doc, parent, base, ext);
  NodeId cap = doc.create(NodeKind::Caption, parent);
  add_link(doc, cap, html_decode_entities(str(p, "filename")) + ext, base + ext,
           InlineRole::Metadata);
  add_meta(doc, cap, " (" + size_text(num(p, "fsize")) + ", " +
                         std::to_string(num(p, "w")) + "×" +
                         std::to_string(num(p, "h")) + ")");
}

// Poster name with tripcode and poster id.
std::string poster(const Json& p) {
  std::string name = html_decode_entities(str(p, "name"));
  if (name.empty()) name = "Anonymous";
  if (const std::string trip = str(p, "trip"); !trip.empty()) name += " " + trip;
  if (const std::string id = str(p, "id"); !id.empty()) name += " (ID: " + id + ")";
  return name;
}

// 4chan breaks long words with <wbr>; drop it so URLs stay whole.
std::string clean_comment(std::string_view html) {
  std::string out;
  out.reserve(html.size());
  for (size_t i = 0; i < html.size();) {
    if (html.compare(i, 5, "<wbr>") == 0) {
      i += 5;
      continue;
    }
    out.push_back(html[i++]);
  }
  return out;
}

// Number of the first ">>N" quotelink to this thread ("" when none).
std::string first_quote(std::string_view html) {
  const size_t at = html.find("href=\"#p");
  if (at == std::string_view::npos) return {};
  size_t e = at + 8;
  while (e < html.size() && html[e] >= '0' && html[e] <= '9') ++e;
  return std::string(html.substr(at + 8, e - at - 8));
}

}  // namespace

Document fourchan_boards_document(const Json& reply) {
  Document doc;
  ForumRef top;
  top.site = ForumSite::FourChan;
  NodeId thread = start_document(doc, "4chan — Boards", "4chan", {}, "4chan");
  // Work-safe boards first; the rest under their own heading.
  for (int ws = 1; ws >= 0; --ws) {
    NodeId sect = doc.create(NodeKind::Article, thread);
    if (ws == 0)
      doc.append_text(doc.create(NodeKind::Header, sect), "Not work safe",
                      InlineKind::Strong);
    NodeId list = doc.create(NodeKind::List, sect);
    for (const Json& b : arr(reply, "boards")) {
      if ((num(b, "ws_board") != 0) != (ws == 1)) continue;
      const std::string board = str(b, "board");
      // A ListItem is a leaf text block: the spans go on the item itself.
      NodeId item = doc.create(NodeKind::ListItem, list);
      add_link(doc, item, "/" + board + "/ – " + html_decode_entities(str(b, "title")),
               chan_ref(board));
      const std::string desc = html_to_plain(str(b, "meta_description"));
      if (!desc.empty()) add_meta(doc, item, " — " + desc);
    }
  }
  add_web_link(doc, doc.root, top);
  return doc;
}

Document fourchan_catalog_document(const Json& reply, const ForumRef& ref) {
  const std::string board = board_of(ref);
  Document doc;
  NodeId thread = start_document(
      doc, "/" + board + "/ — Catalog", "4chan",
      {{"Boards", chan_ref("")}, {"/" + board + "/", "", true}}, "4chan");
  size_t n = 0;
  for (const Json& page : reply.type == Json::Type::Array ? reply.items : std::vector<Json>{}) {
    for (const Json& t : arr(page, "threads")) {
      ++n;
      const std::string no = std::to_string(num(t, "no"));
      NodeId art = doc.create(NodeKind::Article, thread);
      doc.get(art).label = no;
      const std::string tim = tim_str(t);
      if (!tim.empty() && num(t, "filedeleted") == 0)
        add_thumb(doc, art, "https://i.4cdn.org/" + board + "/" + tim, str(t, "ext"));
      std::string text = html_to_plain(clean_comment(str(t, "com")));
      std::string sub = html_decode_entities(str(t, "sub"));
      if (sub.empty()) {
        sub = text.substr(0, 80);
        if (text.size() > 80) sub += "…";
        text.clear();
      }
      if (sub.empty()) sub = "No. " + no;
      NodeId head = doc.create(NodeKind::Header, art);
      add_link(doc, head, sub, chan_ref(board + "/thread/" + no));
      if (num(t, "sticky")) add_meta(doc, head, " (sticky)");
      if (num(t, "closed")) add_meta(doc, head, " (closed)");
      if (!text.empty()) {
        if (text.size() > 300) text = text.substr(0, 300) + "…";
        append_plain(doc, art, text);
      }
      NodeId cap = doc.create(NodeKind::Caption, art);
      add_meta(doc, cap, count(num(t, "replies"), "reply", "replies"));
      add_sep(doc, cap);
      add_meta(doc, cap, count(num(t, "images"), "image", "images"));
      add_sep(doc, cap);
      InlineSpan date;
      date.role = InlineRole::Metadata;
      date.datetime = iso_from_unix(num(t, "time"));
      date.text = short_time(date.datetime);
      doc.get(cap).spans.push_back(std::move(date));
    }
  }
  if (n == 0) add_note(doc, doc.root, "No threads.");
  add_web_link(doc, doc.root, ref);
  return doc;
}

Document fourchan_thread_document(const Json& reply, const ForumRef& ref) {
  const std::string board = board_of(ref);
  const std::vector<Json>& posts = arr(reply, "posts");
  const std::string op = posts.empty() ? std::string() : std::to_string(num(posts[0], "no"));
  std::string title = posts.empty() ? std::string() : html_decode_entities(str(posts[0], "sub"));
  if (title.empty() && !posts.empty()) {
    title = html_to_plain(clean_comment(str(posts[0], "com"))).substr(0, 60);
  }
  if (title.empty()) title = "/" + board + "/ thread " + op;
  Document doc;
  NodeId thread = start_document(
      doc, title, "4chan",
      {{"Boards", chan_ref("")}, {"/" + board + "/", chan_ref(board)}}, "4chan");
  const std::string here = board + "/thread/" + op;
  for (const Json& p : posts) {
    MsgHead h;
    h.label = std::to_string(num(p, "no"));
    h.author = poster(p);
    h.iso = iso_from_unix(num(p, "time"));
    h.permalink = chan_ref(here, "id:" + h.label);
    const std::string com = clean_comment(str(p, "com"));
    h.reply_to = first_quote(com);
    NodeId msg = add_message(doc, thread, h);
    if (h.label == op) {
      const std::string sub = html_decode_entities(str(p, "sub"));
      if (!sub.empty()) doc.append_text(doc.create(NodeKind::Header, msg), sub, InlineKind::Strong);
    }
    add_file(doc, msg, p, board);
    if (!com.empty())
      append_html(doc, msg, com, "https://boards.4chan.org/" + here, false);
  }
  if (posts.empty()) add_note(doc, doc.root, "Thread not found.");
  // Quotelinks: this thread → #id:N, other threads and boards → fourchan: refs.
  const std::string self = chan_ref(here);
  map_links(doc, [&](std::string& href) {
    std::string r = forum_ref_from_web_url(href);
    if (r.empty()) return;
    if (starts_with(r, self + "#")) r = r.substr(self.size());
    href = r;
  });
  add_web_link(doc, doc.root, ref);
  return doc;
}

}  // namespace xanatoo
