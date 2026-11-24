// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
//
// Digg: feeds, search, stories with their sources and comments, from the
// replies of the public read-only MCP server (https://digg.com/mcp). The
// MCP records carry no pictures and no article text; digg_web_story /
// digg_web_images read those from the story's / feed's web page. Pure
// mappers; the requests are made in forum_fetch.cpp.

#include "forum_common.hpp"
#include "xanatoo/html_min.hpp"
#include "xanatoo/text_escape.hpp"
#include "xanatoo/util.hpp"

#include <cctype>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace xanatoo {

using namespace forum;

namespace {

struct Choice {
  std::string_view slug, name;
};

constexpr Choice kTopics[] = {
    {"all", "All"},
    {"ai", "AI"},
    {"tech", "Technology"},
    {"gaming", "Gaming"},
    {"entertainment", "Entertainment"},
    {"world-business", "World & Business"},
    {"science", "Science"},
    {"sports", "Sports"},
};

// The first view is what a feed shows when none is named.
constexpr Choice kViews[] = {
    {"trending", "Trending"}, {"top", "Top"}, {"best", "Best"},
    {"hot", "Hot"},           {"newest", "Newest"},
};

// Time windows of the Top view, the default first.
constexpr Choice kRanges[] = {
    {"today", "Today"}, {"24h", "24 hours"}, {"7d", "7 days"},
    {"30d", "30 days"}, {"1y", "Year"},      {"all", "All time"},
};

constexpr Choice kSorts[] = {
    {"relevance", "Relevance"},
    {"score", "Score"},
    {"newest", "Newest"},
};

template <size_t N>
const Choice* find_choice(const Choice (&list)[N], std::string_view slug) {
  for (const Choice& c : list)
    if (c.slug == slug) return &c;
  return nullptr;
}

std::string topic_name(std::string_view slug) {
  if (const Choice* c = find_choice(kTopics, slug)) return std::string(c->name);
  return std::string(slug);
}

bool is_uuid(std::string_view id) {
  if (id.size() != 36) return false;
  for (size_t i = 0; i < id.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (id[i] != '-') return false;
    } else if (!std::isxdigit(static_cast<unsigned char>(id[i]))) {
      return false;
    }
  }
  return true;
}

// The id in a story's web address ("7xbz1s0w").
bool is_short_id(std::string_view id) {
  if (id.size() < 6 || id.size() > 16) return false;
  for (char c : id)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
  return true;
}

std::vector<std::string_view> split_path(std::string_view path) {
  std::vector<std::string_view> seg;
  while (!path.empty()) {
    const size_t sl = path.find('/');
    seg.push_back(path.substr(0, sl));
    path = sl == std::string_view::npos ? std::string_view{} : path.substr(sl + 1);
  }
  return seg;
}

std::string digg_ref(std::string path, std::string frag = {}) {
  ForumRef r;
  r.site = ForumSite::Digg;
  r.path = std::move(path);
  r.fragment = std::move(frag);
  return forum_ref_string(r);
}

std::string feed_ref(std::string_view topic, std::string_view view = {},
                     std::string_view range = {}) {
  if (topic == "all" && view.empty()) return digg_ref("");
  std::string path(topic);
  if (!view.empty()) path += "/" + std::string(view);
  if (!range.empty()) path += "/" + std::string(range);
  return digg_ref(path);
}

}  // namespace

// ---- refs -------------------------------------------------------------------

namespace forum {

DiggPath digg_path(std::string_view path) {
  using Kind = DiggPath::Kind;
  DiggPath p;
  const std::vector<std::string_view> seg = split_path(path);
  auto at = [&](size_t i) { return i < seg.size() ? seg[i] : std::string_view{}; };
  if (at(0) == "search") {
    if (seg.size() > 2 || (seg.size() == 2 && !find_choice(kSorts, at(1)))) return p;
    p.kind = Kind::Search;
    p.sort = seg.size() == 2 ? at(1) : kSorts[0].slug;
    return p;
  }
  if (at(0) == "post") {
    if (!is_uuid(at(1))) return p;
    p.id = at(1);
    if (seg.size() == 2) {
      p.kind = Kind::Post;
    } else if (seg.size() == 3 && at(2) == "comments") {
      p.kind = Kind::Comments;
    } else if (seg.size() == 4 && at(2) == "comments" && is_uuid(at(3))) {
      p.kind = Kind::Replies;
      p.comment = at(3);
    }
    return p;
  }
  p.topic = seg.empty() ? kTopics[0].slug : at(0);
  if (seg.size() > 3 || !find_choice(kTopics, p.topic)) return p;
  if (seg.size() == 2 && !find_choice(kViews, at(1))) {
    // digg.com/TOPIC/ID; view names win over ids ("trending" looks like one).
    if (p.topic == "all" || !is_short_id(at(1))) return p;
    p.kind = Kind::Story;
    p.id = at(1);
    return p;
  }
  p.view = seg.size() > 1 ? at(1) : kViews[0].slug;
  if (!find_choice(kViews, p.view)) return p;
  if (p.view == "top") p.range = seg.size() > 2 ? at(2) : kRanges[0].slug;
  else if (seg.size() > 2) return p;
  if (!p.range.empty() && !find_choice(kRanges, p.range)) return p;
  p.kind = Kind::Feed;
  return p;
}

bool digg_ref_ok(const ForumRef& r) {
  using Kind = DiggPath::Kind;
  const DiggPath p = digg_path(r.path);
  if (p.kind == Kind::Invalid) return false;
  if ((p.kind == Kind::Search) == r.query.empty()) return false;
  // Sources page by number, everything else by cursor.
  if (p.kind == Kind::Post || p.kind == Kind::Story)
    return r.cursor.empty() && r.page <= kDiggMaxSourcePage;
  return r.page == 1;
}

std::string digg_web_url(const ForumRef& r) {
  const DiggPath p = digg_path(r.path);
  if (p.kind == DiggPath::Kind::Story)
    return "https://digg.com/" + std::string(p.topic) + "/" + std::string(p.id);
  if (p.kind != DiggPath::Kind::Feed) return {};
  std::string url = "https://digg.com/";
  if (p.topic != "all") url += p.topic;
  if (p.view != kViews[0].slug) url += "?filter=" + std::string(p.view);
  return url;
}

std::string digg_ref_from_web(std::string_view path, std::string_view filter) {
  const DiggPath p = digg_path(path);
  if (p.kind == DiggPath::Kind::Story) return digg_ref(std::string(path));
  if (p.kind != DiggPath::Kind::Feed || path.find('/') != std::string_view::npos)
    return {};
  return feed_ref(p.topic, find_choice(kViews, filter) ? filter : std::string_view{});
}

std::string digg_up_path(std::string_view path) {
  using Kind = DiggPath::Kind;
  const DiggPath p = digg_path(path);
  if (p.kind == Kind::Comments || p.kind == Kind::Replies)
    return "post/" + std::string(p.id);
  if (p.kind == Kind::Story || p.kind == Kind::Feed)
    if (const size_t sl = path.rfind('/'); sl != std::string_view::npos)
      return std::string(path.substr(0, sl));
  return {};
}

}  // namespace forum

// ---- MCP envelope and arguments ---------------------------------------------

namespace {

std::optional<Json> parse_json_value(std::string_view text) {
  trim_ascii(text);
  if (text.empty() || (text.front() != '{' && text.front() != '['))
    return std::nullopt;
  return json_parse(text);
}

std::optional<Json> unwrap(const Json& reply, std::string* err) {
  if (const Json* error = json_get(reply, "error")) {
    if (err) *err = "Digg: " + str(*error, "message");
    return std::nullopt;
  }
  const Json* result = json_get(reply, "result");
  if (!result) {
    if (err) *err = "Digg sent no result";
    return std::nullopt;
  }
  const std::vector<Json>& parts = arr(*result, "content");
  if (const Json* failed = json_get(*result, "isError"); failed && failed->b) {
    if (err)
      *err = "Digg: " + (parts.empty() ? std::string("the request failed")
                                       : str(parts.front(), "text"));
    return std::nullopt;
  }
  if (const Json* data = json_get(*result, "structuredContent");
      data && data->type == Json::Type::Object)
    return *data;
  // The same record as JSON text.
  for (const Json& part : parts)
    if (auto parsed = parse_json_value(str(part, "text"))) return parsed;
  if (err) *err = "Digg sent no structured result";
  return std::nullopt;
}

// Cursors are kept escaped in refs; the tools want them as they were sent.
void add_cursor(std::string& args, const ForumRef& ref) {
  if (!ref.cursor.empty())
    args += ",\"cursor\":" + json_quote(percent_decode(ref.cursor));
}

}  // namespace

std::optional<Json> digg_mcp_decode(std::string_view body, std::string* err) {
  if (auto j = parse_json_value(body)) return unwrap(*j, err);
  // text/event-stream: the last complete event is the reply.
  std::string payload;
  std::optional<Json> last;
  auto flush = [&] {
    if (auto j = parse_json_value(payload)) last = std::move(j);
    payload.clear();
  };
  while (!body.empty()) {
    const size_t end = body.find('\n');
    std::string_view line = body.substr(0, end);
    body.remove_prefix(end == std::string_view::npos ? body.size() : end + 1);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (starts_with(line, "data:")) {
      line.remove_prefix(5);
      if (!line.empty() && line.front() == ' ') line.remove_prefix(1);
      if (!payload.empty()) payload += '\n';
      payload += line;
    } else if (line.empty()) {
      flush();
    }
  }
  flush();
  if (last) return unwrap(*last, err);
  if (err) *err = "Digg sent neither JSON nor an event stream";
  return std::nullopt;
}

std::string digg_feed_arguments(const ForumRef& ref) {
  const DiggPath p = digg_path(ref.path);
  std::string args = "{\"topic\":" + json_quote(p.topic) +
                     ",\"view\":" + json_quote(p.view);
  if (!p.range.empty()) args += ",\"range\":" + json_quote(p.range);
  args += ",\"limit\":20";  // the tool's maximum
  add_cursor(args, ref);
  return args + "}";
}

std::string digg_search_arguments(const ForumRef& ref) {
  std::string args = "{\"query\":" + json_quote(ref.query) +
                     ",\"sort\":" + json_quote(digg_path(ref.path).sort) +
                     ",\"limit\":30";
  add_cursor(args, ref);
  return args + "}";
}

std::string digg_post_arguments(std::string_view id, int page) {
  return "{\"id\":" + json_quote(id) + ",\"sourceOffset\":" +
         std::to_string((page - 1) * kDiggSourcePageSize) +
         ",\"sourceLimit\":" + std::to_string(kDiggSourcePageSize) + "}";
}

// Only the comment pages carry a cursor of theirs.
std::string digg_comments_arguments(std::string_view post, const ForumRef& ref) {
  std::string args =
      "{\"postId\":" + json_quote(post) + ",\"sort\":\"top\",\"limit\":50";
  if (digg_path(ref.path).kind == DiggPath::Kind::Comments) add_cursor(args, ref);
  return args + "}";
}

std::string digg_replies_arguments(std::string_view comment, const ForumRef& ref) {
  std::string args = "{\"commentId\":" + json_quote(comment) + ",\"limit\":50";
  if (digg_path(ref.path).kind == DiggPath::Kind::Replies) add_cursor(args, ref);
  return args + "}";
}

// ---- web pages --------------------------------------------------------------

namespace {

constexpr std::string_view kImageDir = "editorial-story-images/";

// The CDN scales (and crops) an editorial picture as asked.
std::string cdn_image(std::string_view path, std::string_view size) {
  return "https://cdn.digg.com/cdn-cgi/image/" + std::string(size) +
         ",quality=85,format=jpeg/" + std::string(kImageDir) + std::string(path);
}

// Value of ` name="…"` in a start tag ("" when absent).
std::string_view attribute(std::string_view tag, std::string_view name) {
  const std::string key = " " + std::string(name) + "=\"";
  size_t at = tag.find(key);
  if (at == std::string_view::npos) return {};
  at += key.size();
  const size_t end = tag.find('"', at);
  return end == std::string_view::npos ? std::string_view{}
                                       : tag.substr(at, end - at);
}

// "Image: CREDIT" under the picture of a story page.
std::string image_credit(std::string_view html) {
  constexpr std::string_view kMark = ">Image: ";
  const size_t at = html.find(kMark);
  if (at == std::string_view::npos) return {};
  const std::string_view rest = html.substr(at + kMark.size(), 200);
  const size_t end = rest.find('<');
  return end == std::string_view::npos ? std::string()
                                       : html_decode_entities(rest.substr(0, end));
}

// The contents of the article <section>, without the sections inside it
// (embedded "Featured Source" players).
std::string article_body(std::string_view html) {
  size_t at = html.find("data-cluster-article-body=");
  if (at != std::string_view::npos) at = html.find('>', at);
  if (at == std::string_view::npos) return {};
  std::string out;
  int depth = 1;
  for (size_t i = at + 1; i < html.size();) {
    const size_t lt = html.find('<', i);
    if (lt == std::string_view::npos) break;
    if (depth == 1) out += html.substr(i, lt - i);
    const size_t gt = html.find('>', lt);
    if (gt == std::string_view::npos) break;
    const std::string_view tag = html.substr(lt, gt + 1 - lt);
    if (starts_with(tag, "</section")) {
      if (--depth == 0) return out;
    } else if (starts_with(tag, "<section")) {
      ++depth;
    } else if (depth == 1) {
      out += tag;
    }
    i = gt + 1;
  }
  return {};  // cut off: no half article
}

}  // namespace

DiggImages digg_web_images(std::string_view html) {
  DiggImages out;
  for (size_t at = html.find(kImageDir); at != std::string_view::npos;
       at = html.find(kImageDir, at)) {
    at += kImageDir.size();
    const std::string_view rest = html.substr(at, 160);
    size_t n = 0;
    while (n < rest.size() &&
           (std::isalnum(static_cast<unsigned char>(rest[n])) || rest[n] == '-' ||
            rest[n] == '.' || rest[n] == '_' || rest[n] == '/'))
      ++n;
    const std::string_view path = rest.substr(0, n);
    const std::vector<std::string_view> seg = split_path(path);
    if (seg.size() == 2 && is_uuid(seg[0]) && !seg[1].empty() && seg[1] != "..")
      out.emplace(seg[0], path);
  }
  return out;
}

DiggWebStory digg_web_story(std::string_view html) {
  DiggWebStory s;
  // Related stories have a data-cluster-id too; only the page's own element
  // names the id of the address.
  const size_t at = html.find("data-cluster-detail-url-cluster-id=");
  if (at == std::string_view::npos) return s;
  const size_t lt = html.rfind('<', at);
  const size_t gt = html.find('>', at);
  if (lt == std::string_view::npos || gt == std::string_view::npos) return s;
  const std::string_view id = attribute(html.substr(lt, gt - lt), "data-cluster-id");
  if (!is_uuid(id)) return s;
  s.id = id;
  const DiggImages images = digg_web_images(html);
  if (auto it = images.find(s.id); it != images.end()) {
    s.image = it->second;
    s.image_credit = image_credit(html);
  }
  s.article_html = article_body(html);
  return s;
}

// ---- documents --------------------------------------------------------------

namespace {

// One story, from an MCP post record or an entry of the JSON cluster feeds.
struct Story {
  std::string id, ref, web, topic, title, description, tldr, published, updated;
  long long score = 0, sources = 0, authors = 0;
};

Story read_story(const Json& j) {
  Story s;
  s.id = str(j, "id");
  if (is_uuid(s.id)) s.ref = digg_ref("post/" + s.id);
  else s.id.clear();
  s.title = str(j, "title");
  s.description = str(j, "description");
  s.tldr = str(j, "tldr");
  s.topic = str(j, "topic");
  s.web = str(j, "url");
  if (starts_with(s.web, "/")) {  // cluster feeds: "/ai/7xbz1s0w", no topic
    if (s.topic.empty()) s.topic = s.web.substr(1, s.web.find('/', 1) - 1);
    s.web.insert(0, "https://digg.com");
  }
  if (!starts_with(s.web, "https://digg.com/")) s.web.clear();
  if (!find_choice(kTopics, s.topic)) s.topic.clear();
  s.published = str(j, "publishedAt");
  if (s.published.empty()) s.published = str(j, "first_post_at");
  s.updated = str(j, "updatedAt");
  s.score = num(j, "diggScore");
  s.sources = num(j, json_get(j, "story_count") ? "story_count" : "sourceCount");
  s.authors = num(j, "uniqueAuthors");
  return s;
}

// Plain one-line excerpt, cut at a word.
std::string preview(std::string_view text) {
  constexpr size_t kMax = 230;
  std::string out;
  for (unsigned char c : text) {
    if (std::isspace(c)) {
      if (!out.empty() && out.back() != ' ') out += ' ';
    } else {
      out += static_cast<char>(c);
    }
  }
  if (!out.empty() && out.back() == ' ') out.pop_back();
  if (out.size() <= kMax) return out;
  size_t stop = out.rfind(' ', kMax);
  if (stop == std::string::npos || stop < kMax / 2) {
    stop = kMax;
    while (stop && (static_cast<unsigned char>(out[stop]) & 0xc0) == 0x80) --stop;
  }
  out.resize(stop);
  return out + "…";
}

// Bold body-sized link: a feed is a reading list, not a stack of headings.
void add_title(Document& doc, NodeId para, const std::string& text,
               const std::string& href) {
  if (href.empty()) {
    add_text(doc, para, text, InlineKind::Strong);
    return;
  }
  add_link(doc, para, text, href);
  add_mark(doc.get(para).spans.back(), InlineKind::Strong);
}

void add_date(Document& doc, NodeId cap, const std::string& iso,
              std::string_view prefix = {}) {
  if (iso.empty()) return;
  add_sep(doc, cap);
  InlineSpan span;
  span.role = InlineRole::Metadata;
  span.text = std::string(prefix) + short_time(iso);
  span.datetime = iso;
  doc.get(cap).spans.push_back(std::move(span));
}

NodeId add_image(Document& doc, NodeId parent, std::string src,
                 const std::string& alt) {
  const NodeId id = doc.create(NodeKind::Image, parent);
  doc.get(id).src = std::move(src);
  doc.get(id).alt = alt;
  return id;
}

void add_caption(Document& doc, NodeId art, const Story& s,
                 std::string_view current_topic, bool detail) {
  const NodeId cap = doc.create(NodeKind::Caption, art);
  if (!s.topic.empty() && s.topic != current_topic)
    add_link(doc, cap, topic_name(s.topic), feed_ref(s.topic), InlineRole::Metadata);
  auto field = [&](const std::string& text) {
    add_sep(doc, cap);
    add_meta(doc, cap, text);
  };
  if (s.score > 0) field("score " + std::to_string(s.score));
  if (s.sources > 0) field(count(s.sources, "source", "sources"));
  if (s.authors > 0) field(count(s.authors, "author", "authors"));
  add_date(doc, cap, s.published);
  if (detail && short_time(s.updated) != short_time(s.published))
    add_date(doc, cap, s.updated, "updated ");
  if (!s.web.empty()) {
    add_sep(doc, cap);
    InlineSpan web;
    web.kind = InlineKind::Link;
    web.role = InlineRole::Metadata;
    web.rel = "external";
    web.text = "View on Digg";
    web.href = s.web;
    doc.get(cap).spans.push_back(std::move(web));
  }
  if (doc.get(cap).spans.empty()) add_meta(doc, cap, "Digg");
}

void add_feed_entry(Document& doc, NodeId thread, const Story& s,
                    std::string_view current_topic, const DiggImages& images) {
  if (s.title.empty()) return;
  const NodeId art = doc.create(NodeKind::Article, thread);
  doc.get(art).label = s.id;
  if (auto it = images.find(s.id); it != images.end()) {
    const NodeId img = add_image(doc, art, cdn_image(it->second, "width=240,height=160,fit=cover"), s.title);
    doc.get(img).float_side = "right";
  }
  add_title(doc, doc.create(NodeKind::Paragraph, art), s.title,
            s.ref.empty() ? s.web : s.ref);
  const std::string text = preview(s.description.empty() ? s.tldr : s.description);
  if (!text.empty()) doc.append_text(doc.create(NodeKind::Paragraph, art), text);
  add_caption(doc, art, s, current_topic, false);
}

// Another "Label: a · b" line in the document's navigation.
void add_nav_row(Document& doc, const std::string& label,
                 const std::vector<NavItem>& items) {
  for (NodeId id : doc.get(doc.root).children) {
    if (doc.get(id).kind != NodeKind::Chrome) continue;
    const NodeId line = doc.create(NodeKind::Paragraph, id);
    doc.append_text(line, label + ": ", InlineKind::Strong);
    bool first = true;
    for (const NavItem& item : items) {
      if (!first) doc.append_text(line, " · ");
      first = false;
      if (item.current) doc.append_text(line, item.text, InlineKind::Strong);
      else add_link(doc, line, item.text, item.ref);
    }
    return;
  }
}

void add_feed_nav(Document& doc, const ForumRef& ref, const DiggPath& p) {
  std::vector<NavItem> items;
  if (p.kind == DiggPath::Kind::Search) {
    for (const Choice& c : kSorts) {
      ForumRef r = ref;
      r.cursor.clear();
      r.fragment.clear();
      r.path = "search/" + std::string(c.slug);
      items.push_back({std::string(c.name), forum_ref_string(r), c.slug == p.sort});
    }
    add_nav_row(doc, "Sort", items);
    return;
  }
  for (const Choice& c : kViews)
    items.push_back({std::string(c.name), feed_ref(p.topic, c.slug), c.slug == p.view});
  add_nav_row(doc, "Sort", items);
  if (p.range.empty()) return;
  items.clear();
  for (const Choice& c : kRanges)
    items.push_back({std::string(c.name), feed_ref(p.topic, p.view, c.slug),
                     c.slug == p.range});
  add_nav_row(doc, "From", items);
}

// "More … →" by the reply's opaque cursor.
void add_more(Document& doc, NodeId parent, const Json& reply, ForumRef ref,
              const std::string& text) {
  const std::string cursor = str(reply, "nextCursor");
  if (cursor.empty()) return;
  ref.cursor = url_encode(cursor);
  ref.page = 1;
  ref.fragment.clear();
  add_link(doc, doc.create(NodeKind::Paragraph, parent), text + " →",
           forum_ref_string(ref));
}

bool is_web(std::string_view url) {
  return starts_with_ci(url, "https://") || starts_with_ci(url, "http://");
}

std::string x_handle(std::string_view url) {
  for (std::string_view base : {"https://x.com/", "https://twitter.com/"}) {
    if (!starts_with(url, base)) continue;
    const std::string_view name = url.substr(base.size());
    return "@" + std::string(name.substr(0, name.find_first_of("/?#")));
  }
  return {};
}

std::string web_host(std::string_view url) {
  const size_t s = url.find("://");
  if (s == std::string_view::npos) return {};
  std::string_view h = url.substr(s + 3);
  h = h.substr(0, h.find_first_of("/?#:"));
  if (starts_with(h, "www.")) h.remove_prefix(4);
  return std::string(h);
}

// A post on X (author and text) or a web page (title, author, excerpt).
void add_source(Document& doc, NodeId section, const Json& s) {
  const bool x = str(s, "kind") == "x";
  std::string url = str(s, "url");
  if (!is_web(url)) url.clear();
  const std::string author = str(s, "author");
  const std::string host = web_host(url);
  const std::string handle = x ? x_handle(url) : std::string();
  std::string title = x ? author : str(s, "title");
  for (const std::string& other : {handle, author, host})
    if (title.empty()) title = other;
  if (title.empty()) title = "Source";
  const NodeId art = doc.create(NodeKind::Article, section);
  doc.get(art).label = str(s, "id");
  add_title(doc, doc.create(NodeKind::Paragraph, art), title, url);
  const NodeId cap = doc.create(NodeKind::Caption, art);
  auto field = [&](const std::string& text) {
    if (text.empty() || text == title) return;
    add_sep(doc, cap);
    add_meta(doc, cap, text);
  };
  if (x) {
    field(handle);
    field("X");
  } else {
    field(author);
    field(host);
  }
  add_date(doc, cap, str(s, "publishedAt"));
  if (doc.get(cap).spans.empty()) add_meta(doc, cap, "Web page");
  append_plain(doc, art, str(s, "text"));
}

struct CommentList {
  std::string post;  // UUID, for the permalinks
  std::unordered_set<std::string> shown;
};

void add_comment(Document& doc, NodeId parent, const Json& c, CommentList& list) {
  const std::string id = str(c, "id");
  if (!list.shown.insert(id).second) return;
  MsgHead h;
  h.label = id;
  if (const Json* a = json_get(c, "author")) {
    h.author = str(*a, "name");
    if (h.author.empty()) h.author = str(*a, "username");
    h.author_href = str(*a, "url");
    if (!is_web(h.author_href)) h.author_href.clear();
  }
  h.iso = str(c, "createdAt");
  h.permalink = digg_ref("post/" + list.post, "id:" + id);
  for (const char* key : {"repliedToCommentId", "parentId"})
    if (h.reply_to.empty() && list.shown.count(str(c, key))) h.reply_to = str(c, key);
  h.indent = !h.reply_to.empty();
  if (const long long n = num(c, "diggCount"); n > 0) h.score = count(n, "digg", "diggs");
  const NodeId msg = add_message(doc, parent, h);
  append_plain(doc, msg, str(c, "body"));
  const Json* gif = json_get(c, "gif");
  if (str(c, "state") == "unavailable") add_note(doc, msg, "Comment not available.");
  else if (gif && gif->type != Json::Type::Null) add_note(doc, msg, "GIF on Digg's page.");
  if (const std::string edited = str(c, "editedAt"); !edited.empty())
    add_date(doc, doc.create(NodeKind::Caption, msg), edited, "edited ");
}

// The comments of `reply`, each with the replies that came along, or a link
// to the rest of them.
void add_comments(Document& doc, NodeId parent, const Json& reply,
                  const DiggReplies& replies, CommentList& list) {
  for (const Json& c : arr(reply, "comments")) {
    const std::string id = str(c, "id");
    add_comment(doc, parent, c, list);
    const long long total = num(c, "replyCount");
    long long shown = 0;
    if (auto it = replies.find(id); it != replies.end()) {
      for (const Json& r : arr(it->second, "replies")) {
        add_comment(doc, parent, r, list);
        ++shown;
      }
    }
    if (shown >= total) continue;
    add_link(doc, doc.create(NodeKind::Paragraph, parent),
             (shown ? "All " : "") + count(total, "reply", "replies") + " →",
             digg_ref("post/" + list.post + "/comments/" + id));
  }
}

void add_related(Document& doc, NodeId parent, const Json& reply,
                 std::string_view key, const std::string& label) {
  const Json* j = json_get(reply, key);
  if (!j || j->type != Json::Type::Object) return;
  const Story s = read_story(*j);
  if (s.title.empty()) return;
  const NodeId para = doc.create(NodeKind::Paragraph, parent);
  add_text(doc, para, label + ": ", InlineKind::Strong);
  add_link(doc, para, s.title, s.ref.empty() ? s.web : s.ref);
}

NodeId add_section(Document& doc, NodeId parent, const std::string& label,
                   const std::string& title) {
  const NodeId section = doc.create(NodeKind::Section, parent);
  doc.get(section).label = label;
  add_text(doc, doc.create(NodeKind::Header, section), title);
  return section;
}

// Picture, summary and article of a story.
void add_story_body(Document& doc, NodeId art, const Story& s,
                    const DiggWebStory& web) {
  if (!web.image.empty()) {
    const NodeId img = add_image(doc, art, cdn_image(web.image, "width=1080"), s.title);
    InlineSpan full;
    full.kind = InlineKind::Link;
    full.href = "https://cdn.digg.com/" + std::string(kImageDir) + web.image;
    doc.get(img).spans.push_back(std::move(full));
    if (!web.image_credit.empty())
      add_meta(doc, doc.create(NodeKind::Caption, art), "Image: " + web.image_credit);
  }
  append_plain(doc, art, s.description);
  if (!s.tldr.empty() && s.tldr != s.description) {
    const NodeId para = doc.create(NodeKind::Paragraph, art);
    add_text(doc, para, "In brief: ", InlineKind::Strong);
    add_text(doc, para, s.tldr);
  }
  if (!web.article_html.empty()) {
    // Subheadings open sections, so they stay where they are in the text.
    HtmlOptions opt;
    opt.base_url = "https://digg.com/";
    opt.sections = true;
    html_append_blocks(doc, art, web.article_html, opt);
  }
}

void add_comment_section(Document& doc, NodeId thread, const Story& s,
                         const DiggPost& extra) {
  const Json& c = *extra.comments;
  const long long n = num(c, "commentCount");
  const NodeId section = add_section(
      doc, thread, "comments",
      n > 0 ? "Comments (" + std::to_string(n) + ")" : "Comments");
  CommentList list{s.id, {}};
  add_comments(doc, section, c, extra.replies, list);
  if (list.shown.empty()) add_note(doc, section, "No comments.");
  if (const Json* locked = json_get(c, "locked"); locked && locked->b)
    add_note(doc, section, "The discussion is closed.");
  ForumRef more;
  more.site = ForumSite::Digg;
  more.path = "post/" + s.id + "/comments";
  add_more(doc, section, c, more, "More comments");
}

}  // namespace

Document digg_list_document(const Json& reply, const ForumRef& ref,
                            const DiggImages& images) {
  const DiggPath p = digg_path(ref.path);
  const bool search = p.kind == DiggPath::Kind::Search;
  const std::string_view topic = search ? std::string_view{} : p.topic;
  std::string title = "Digg — " + topic_name(p.topic);
  if (search) title = "Search Digg: " + ref.query;
  else if (p.view != kViews[0].slug)
    title += " · " + std::string(find_choice(kViews, p.view)->name);
  if (p.range.size() && p.range != kRanges[0].slug)
    title += " · " + std::string(find_choice(kRanges, p.range)->name);
  std::vector<NavItem> nav;
  for (const Choice& t : kTopics)
    nav.push_back({std::string(t.name), feed_ref(t.slug), t.slug == topic});
  Document doc;
  const NodeId thread = start_document(doc, title, "Digg", nav, "digg");
  add_feed_nav(doc, ref, p);
  if (search && json_get(reply, "totalMatches"))
    add_meta(doc, doc.create(NodeKind::Caption, thread),
             count(num(reply, "totalMatches"), "story", "stories") + " found");
  // The JSON feeds name their stories "clusters".
  const std::vector<Json>& posts =
      arr(reply, json_get(reply, "clusters") ? "clusters" : "posts");
  for (const Json& post : posts)
    add_feed_entry(doc, thread, read_story(post), topic, images);
  if (posts.empty()) add_note(doc, thread, search ? "No stories found." : "No stories.");
  add_more(doc, doc.root, reply, ref, "More stories");
  add_web_link(doc, doc.root, ref);
  return doc;
}

Document digg_post_document(const Json& reply, const ForumRef& ref,
                            const DiggPost& extra) {
  const Json* record = json_get(reply, "story");
  const Story s = record ? read_story(*record) : Story{};
  std::vector<NavItem> nav = {{"All stories", "digg:", false}};
  if (!s.topic.empty()) nav.push_back({topic_name(s.topic), feed_ref(s.topic), false});
  Document doc;
  const NodeId thread = start_document(
      doc, s.title.empty() ? "Digg story" : s.title, "Digg", nav, "digg");
  const NodeId art = doc.create(NodeKind::Article, thread);
  doc.get(art).label = s.id;
  add_caption(doc, art, s, {}, true);
  add_related(doc, art, reply, "newer", "Newer story");
  if (ref.page == 1) {
    add_story_body(doc, art, s, extra.web);
    add_related(doc, art, reply, "follows", "Earlier story");
    if (extra.comments) add_comment_section(doc, thread, s, extra);
  }
  const NodeId section = add_section(
      doc, thread, "sources",
      ref.page == 1 ? "Sources" : "Sources, page " + std::to_string(ref.page));
  const std::vector<Json>& sources = arr(reply, "sources");
  for (const Json& src : sources) add_source(doc, section, src);
  if (sources.empty()) add_note(doc, section, "No sources.");
  ForumRef pager = ref;
  pager.fragment.clear();
  if (!s.id.empty()) pager.path = "post/" + s.id;  // no web page to look up
  add_pager(doc, pager,
            num(reply, "nextSourceOffset") > 0 && ref.page < kDiggMaxSourcePage,
            "More sources");
  return doc;
}

Document digg_comments_document(const Json& reply, const ForumRef& ref,
                                const DiggReplies& replies) {
  const DiggPath p = digg_path(ref.path);
  const bool one = p.kind == DiggPath::Kind::Replies;
  CommentList list{std::string(p.id), {}};
  Document doc;
  const NodeId thread = start_document(
      doc, one ? "Digg — Replies" : "Digg — Comments", "Digg",
      {{"All stories", "digg:", false}, {"Story", digg_ref("post/" + list.post), false}},
      "digg");
  if (one)
    for (const Json& r : arr(reply, "replies")) add_comment(doc, thread, r, list);
  else
    add_comments(doc, thread, reply, replies, list);
  if (list.shown.empty()) add_note(doc, thread, one ? "No replies." : "No comments.");
  add_more(doc, doc.root, reply, ref, one ? "More replies" : "More comments");
  return doc;
}

}  // namespace xanatoo
