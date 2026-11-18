// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
//
// Bluesky through the public AppView (public.api.bsky.app, no account):
// app.bsky.feed.getAuthorFeed → an author's posts, getPostThread → the
// parents, the post and its reply tree. Post text is plain with "facets"
// (links, mentions, tags) on UTF-8 byte ranges; embeds (images, link cards,
// quoted posts, video) follow the text.

#include "forum_common.hpp"
#include "xanatoo/util.hpp"

#include <algorithm>

namespace xanatoo {

using namespace forum;

namespace {

std::string bsky_ref(std::string path, std::string frag = {}) {
  ForumRef r;
  r.site = ForumSite::Bluesky;
  r.path = std::move(path);
  r.fragment = std::move(frag);
  return forum_ref_string(r);
}

std::string rkey_of(std::string_view at_uri) {
  const size_t sl = at_uri.rfind('/');
  return std::string(sl == std::string_view::npos ? at_uri : at_uri.substr(sl + 1));
}

std::string handle_of(const Json& author) {
  std::string h = str(author, "handle");
  if (h.empty() || h == "handle.invalid") h = str(author, "did");
  return h;
}

std::string display_of(const Json& author) {
  std::string name = str(author, "displayName");
  const std::string handle = str(author, "handle");
  if (name.empty()) return handle.empty() ? str(author, "did") : "@" + handle;
  if (!handle.empty()) name += " (@" + handle + ")";
  return name;
}

// Text with facets as link spans, line breaks kept, under `parent`: one
// `kind` block per paragraph.
void add_rich_text(Document& doc, NodeId parent, const std::string& text,
                   const std::vector<Json>& facets, NodeKind kind = NodeKind::Paragraph) {
  struct Range {
    size_t start, end;
    std::string href;
  };
  std::vector<Range> ranges;
  for (const Json& f : facets) {
    const Json* idx = json_get(f, "index");
    if (!idx) continue;
    const long long a = num(*idx, "byteStart"), b = num(*idx, "byteEnd");
    if (a < 0 || b <= a || static_cast<size_t>(b) > text.size()) continue;
    std::string href;
    for (const Json& feat : arr(f, "features")) {
      const std::string type = str(feat, "$type");
      if (type == "app.bsky.richtext.facet#link") href = str(feat, "uri");
      else if (type == "app.bsky.richtext.facet#mention") href = bsky_ref(str(feat, "did"));
      else if (type == "app.bsky.richtext.facet#tag")
        href = "https://bsky.app/hashtag/" + url_encode(str(feat, "tag"));
    }
    if (!href.empty()) ranges.push_back({static_cast<size_t>(a), static_cast<size_t>(b), href});
  }
  std::sort(ranges.begin(), ranges.end(),
            [](const Range& x, const Range& y) { return x.start < y.start; });
  // Paragraphs at blank lines; facets never cross them in practice, but a
  // range is clipped to its paragraph to be safe.
  size_t pos = 0;
  size_t ri = 0;
  while (pos <= text.size()) {
    size_t end = text.find("\n\n", pos);
    if (end == std::string::npos) end = text.size();
    NodeId para = INVALID_NODE;
    auto out = [&]() {
      if (para == INVALID_NODE) para = doc.create(kind, parent);
      return para;
    };
    size_t at = pos;
    while (ri < ranges.size() && ranges[ri].start < at) ++ri;
    while (at < end) {
      if (ri < ranges.size() && ranges[ri].start < end) {
        const Range& r = ranges[ri++];
        if (r.start < at) continue;  // overlaps the previous facet
        if (r.start > at) doc.append_text(out(), text.substr(at, r.start - at));
        const size_t stop = std::min(r.end, end);
        add_link(doc, out(), text.substr(r.start, stop - r.start), r.href);
        at = stop;
      } else {
        doc.append_text(out(), text.substr(at, end - at));
        at = end;
      }
    }
    if (end >= text.size()) break;
    pos = end + 2;
    while (pos < text.size() && text[pos] == '\n') ++pos;
  }
}

// `full`: the original a click (and the gallery) shows; may be empty.
void add_image(Document& doc, NodeId parent, const std::string& src,
               const std::string& alt, const std::string& full = {}) {
  if (src.empty()) return;
  NodeId im = doc.create(NodeKind::Image, parent);
  doc.get(im).src = src;
  doc.get(im).alt = alt;
  if (full.empty() || full == src) return;
  InlineSpan link;
  link.kind = InlineKind::Link;
  link.href = full;
  doc.get(im).spans.push_back(std::move(link));
}

void add_embed(Document& doc, NodeId parent, const Json& embed);

// A quoted post (app.bsky.embed.record#viewRecord) as block quotes. A
// BlockQuote is a leaf block (spans only), so the author line and each
// paragraph are quotes of their own; its media follow below them.
void add_quoted(Document& doc, NodeId parent, const Json& rec) {
  const std::string type = str(rec, "$type");
  if (type != "app.bsky.embed.record#viewRecord") {
    doc.append_text(doc.create(NodeKind::BlockQuote, parent), "[quoted post unavailable]",
                    InlineKind::Emphasis);
    return;
  }
  NodeId by = doc.create(NodeKind::BlockQuote, parent);
  if (const Json* author = json_get(rec, "author"))
    add_link(doc, by, display_of(*author),
             bsky_ref(handle_of(*author) + "/post/" + rkey_of(str(rec, "uri"))),
             InlineRole::Metadata);
  if (const Json* value = json_get(rec, "value"))
    add_rich_text(doc, parent, str(*value, "text"), arr(*value, "facets"), NodeKind::BlockQuote);
  for (const Json& e : arr(rec, "embeds")) add_embed(doc, parent, e);
}

void add_embed(Document& doc, NodeId parent, const Json& embed) {
  const std::string type = str(embed, "$type");
  if (type == "app.bsky.embed.images#view") {
    for (const Json& im : arr(embed, "images"))
      add_image(doc, parent, str(im, "thumb"), str(im, "alt"), str(im, "fullsize"));
  } else if (type == "app.bsky.embed.external#view") {
    const Json* ext = json_get(embed, "external");
    if (!ext) return;
    NodeId card = doc.create(NodeKind::Paragraph, parent);
    const std::string title = str(*ext, "title");
    const std::string uri = str(*ext, "uri");
    add_link(doc, card, title.empty() ? uri : title, uri);
    if (const std::string d = str(*ext, "description"); !d.empty())
      add_meta(doc, card, " — " + d);
  } else if (type == "app.bsky.embed.video#view") {
    add_image(doc, parent, str(embed, "thumbnail"), str(embed, "alt"));
    if (const std::string pl = str(embed, "playlist"); !pl.empty())
      add_link(doc, doc.create(NodeKind::Caption, parent), "Video", pl, InlineRole::Metadata);
  } else if (type == "app.bsky.embed.record#view") {
    if (const Json* rec = json_get(embed, "record")) add_quoted(doc, parent, *rec);
  } else if (type == "app.bsky.embed.recordWithMedia#view") {
    if (const Json* media = json_get(embed, "media")) add_embed(doc, parent, *media);
    if (const Json* outer = json_get(embed, "record"))
      if (const Json* rec = json_get(*outer, "record")) add_quoted(doc, parent, *rec);
  }
}

// One post as a Message; `reply_to` is the label of the post it answers
// when that one is shown in the same document.
NodeId add_post(Document& doc, NodeId thread, const Json& post,
                const std::string& reply_to, bool indent) {
  const Json* author = json_get(post, "author");
  const std::string handle = author ? handle_of(*author) : std::string();
  const std::string rkey = rkey_of(str(post, "uri"));
  MsgHead h;
  h.label = rkey;
  h.author = author ? display_of(*author) : std::string();
  if (!handle.empty()) h.author_href = bsky_ref(handle);
  const Json* record = json_get(post, "record");
  h.iso = record ? str(*record, "createdAt") : std::string();
  if (h.iso.empty()) h.iso = str(post, "indexedAt");
  if (!handle.empty()) h.permalink = bsky_ref(handle + "/post/" + rkey);
  h.reply_to = reply_to;
  h.indent = indent;
  std::string score = count(num(post, "likeCount"), "like", "likes");
  if (long long r = num(post, "replyCount"); r > 0) score += " · " + count(r, "reply", "replies");
  if (long long r = num(post, "repostCount"); r > 0) score += " · " + count(r, "repost", "reposts");
  h.score = score;
  NodeId msg = add_message(doc, thread, h);
  if (record) add_rich_text(doc, msg, str(*record, "text"), arr(*record, "facets"));
  if (const Json* embed = json_get(post, "embed")) add_embed(doc, msg, *embed);
  return msg;
}

bool is_post_view(const Json& node) {
  return str(node, "$type") == "app.bsky.feed.defs#threadViewPost" ||
         (str(node, "$type").empty() && json_get(node, "post"));
}

void add_replies(Document& doc, NodeId thread, const Json& node,
                 const std::string& parent_label) {
  for (const Json& r : arr(node, "replies")) {
    if (!is_post_view(r)) continue;
    const Json* post = json_get(r, "post");
    if (!post) continue;
    add_post(doc, thread, *post, parent_label, true);
    add_replies(doc, thread, r, rkey_of(str(*post, "uri")));
  }
}

std::string feed_title(const Json& reply, const ForumRef& ref) {
  for (const Json& item : arr(reply, "feed")) {
    const Json* post = json_get(item, "post");
    const Json* author = post ? json_get(*post, "author") : nullptr;
    if (author && (handle_of(*author) == ref.path || str(*author, "did") == ref.path))
      return display_of(*author);
  }
  return "@" + ref.path;
}

// bsky.app links in posts stay native.
void native_links(Document& doc) {
  map_links(doc, [](std::string& href) {
    if (std::string r = forum_ref_from_web_url(href); starts_with(r, "bsky:")) href = r;
  });
}

}  // namespace

Document bsky_feed_document(const Json& reply, const ForumRef& ref) {
  Document doc;
  const std::string title = feed_title(reply, ref);
  NodeId thread = start_document(doc, "Bluesky — " + title, "Bluesky", {}, "bluesky");
  for (const Json& item : arr(reply, "feed")) {
    const Json* post = json_get(item, "post");
    if (!post) continue;
    NodeId msg = add_post(doc, thread, *post, {}, false);
    const Json* reason = json_get(item, "reason");
    if (reason && str(*reason, "$type") == "app.bsky.feed.defs#reasonRepost") {
      NodeId cap = doc.create(NodeKind::Caption, msg);
      add_meta(doc, cap, "Reposted by ");
      if (const Json* by = json_get(*reason, "by"))
        add_link(doc, cap, display_of(*by), bsky_ref(handle_of(*by)), InlineRole::Metadata);
    }
    const Json* parent = json_path(item, "reply/parent");
    const Json* pa = parent ? json_get(*parent, "author") : nullptr;
    if (pa) {
      NodeId cap = doc.create(NodeKind::Caption, msg);
      add_meta(doc, cap, "Reply to ");
      add_link(doc, cap, display_of(*pa),
               bsky_ref(handle_of(*pa) + "/post/" + rkey_of(str(*parent, "uri"))),
               InlineRole::Metadata);
    }
  }
  if (doc.get(thread).children.empty()) add_note(doc, doc.root, "No posts.");
  if (const std::string cursor = str(reply, "cursor"); !cursor.empty()) {
    ForumRef next = ref;
    next.page = 1;
    next.cursor = url_encode(cursor);
    next.fragment.clear();
    add_link(doc, doc.create(NodeKind::Paragraph, doc.root), "Older posts →",
             forum_ref_string(next));
  }
  native_links(doc);
  add_web_link(doc, doc.root, ref);
  return doc;
}

Document bsky_thread_document(const Json& reply, const ForumRef& ref) {
  Document doc;
  const Json* root = json_get(reply, "thread");
  if (!root || !is_post_view(*root)) {
    start_document(doc, "Bluesky", "Bluesky", {}, "bluesky");
    add_note(doc, doc.root, "Post not found (deleted, or hidden from logged-out viewers).");
    add_web_link(doc, doc.root, ref);
    return doc;
  }
  const Json* post = json_get(*root, "post");
  const Json* author = post ? json_get(*post, "author") : nullptr;
  NodeId thread = start_document(
      doc, "Bluesky — " + (author ? display_of(*author) : std::string("post")),
      "Bluesky", {}, "bluesky");
  // Parents, outermost first.
  std::vector<const Json*> parents;
  for (const Json* p = json_get(*root, "parent"); p && is_post_view(*p);
       p = json_get(*p, "parent"))
    parents.push_back(p);
  std::string above;
  for (auto it = parents.rbegin(); it != parents.rend(); ++it) {
    const Json* pp = json_get(**it, "post");
    if (!pp) continue;
    add_post(doc, thread, *pp, above, false);
    above = rkey_of(str(*pp, "uri"));
  }
  if (post) {
    add_post(doc, thread, *post, above, false);
    add_replies(doc, thread, *root, rkey_of(str(*post, "uri")));
  }
  native_links(doc);
  add_web_link(doc, doc.root, ref);
  return doc;
}

}  // namespace xanatoo
