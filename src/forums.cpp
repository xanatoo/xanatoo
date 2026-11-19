// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
//
// Forum refs (discourse: / lobsters: / fourchan: / stackexchange: / bsky: /
// flarum: / lesswrong:), their web forms and the shared document helpers. The site
// mappers live in forum_<site>.cpp, the network side in forum_fetch.cpp.

#include "xanatoo/forums.hpp"

#include "forum_common.hpp"
#include "xanatoo/html_min.hpp"
#include "xanatoo/text_escape.hpp"
#include "xanatoo/time.hpp"
#include "xanatoo/util.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>

namespace xanatoo {

namespace {

struct SiteInfo {
  ForumSite site;
  std::string_view prefix;
  bool has_host;
  const char* name;
};

constexpr SiteInfo kSites[] = {
    {ForumSite::Discourse, "discourse:", true, "Discourse"},
    {ForumSite::Lobsters, "lobsters:", false, "Lobsters"},
    {ForumSite::FourChan, "fourchan:", false, "4chan"},
    {ForumSite::StackExchange, "stackexchange:", true, "Stack Exchange"},
    {ForumSite::Bluesky, "bsky:", false, "Bluesky"},
    {ForumSite::Flarum, "flarum:", true, "Flarum"},
    {ForumSite::LessWrong, "lesswrong:", false, "LessWrong"},
};

const SiteInfo& info(ForumSite s) {
  for (const SiteInfo& i : kSites)
    if (i.site == s) return i;
  return kSites[0];
}

bool host_ok(std::string_view h) {
  if (h.empty() || h.front() == '.' || h.front() == '-') return false;
  for (char c : h)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' ||
          c == ':'))
      return false;
  return true;
}

std::string_view strip_slashes(std::string_view s) {
  while (!s.empty() && s.front() == '/') s.remove_prefix(1);
  while (!s.empty() && s.back() == '/') s.remove_suffix(1);
  return s;
}

// "https://Host/path?q#f" → host (lower case, no port) and the rest
// (path?q#f, no leading '/'). False without an http(s) scheme.
bool split_web(std::string_view url, std::string& host, std::string& rest) {
  trim_ascii(url);
  if (starts_with_ci(url, "https://")) url.remove_prefix(8);
  else if (starts_with_ci(url, "http://")) url.remove_prefix(7);
  else return false;
  const size_t end = url.find_first_of("/?#");
  host = ascii_lower(url.substr(0, end));
  if (size_t colon = host.rfind(':'); colon != std::string::npos)
    host.erase(colon);
  rest = end == std::string_view::npos ? std::string() : std::string(url.substr(end));
  while (starts_with(rest, "/")) rest.erase(0, 1);
  return !host.empty();
}

// Splits "path?query#frag" into its parts.
void split_rest(std::string_view rest, std::string_view& path,
                std::string_view& query, std::string_view& frag) {
  frag = {};
  query = {};
  if (size_t h = rest.find('#'); h != std::string_view::npos) {
    frag = rest.substr(h + 1);
    rest = rest.substr(0, h);
  }
  if (size_t q = rest.find('?'); q != std::string_view::npos) {
    query = rest.substr(q + 1);
    rest = rest.substr(0, q);
  }
  path = rest;
}

// Value of `key=` in a query string ("" when absent).
std::string query_value(std::string_view query, std::string_view key) {
  size_t p = 0;
  while (p <= query.size()) {
    const size_t amp = query.find('&', p);
    std::string_view kv = query.substr(p, amp == std::string_view::npos
                                              ? std::string_view::npos
                                              : amp - p);
    if (kv.size() > key.size() && starts_with(kv, key) && kv[key.size()] == '=')
      return std::string(kv.substr(key.size() + 1));
    if (amp == std::string_view::npos) break;
    p = amp + 1;
  }
  return {};
}

int page_of(std::string_view query) {
  return std::max(1, std::atoi(query_value(query, "page").c_str()));
}

bool cursor_ok(std::string_view c) {
  for (char ch : c)
    if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' ||
          ch == '_' || ch == '.' || ch == ':' || ch == '%'))
      return false;
  return true;
}

// A query value as typed ('+' is a blank).
std::string decode_query(std::string_view v) {
  std::string plus(v);
  for (char& c : plus)
    if (c == '+') c = ' ';
  return percent_decode(plus);
}

bool all_digits(std::string_view s) {
  if (s.empty()) return false;
  for (char c : s)
    if (c < '0' || c > '9') return false;
  return true;
}

bool is_se_host(std::string_view h) {
  if (h == "stackoverflow.com" || h == "superuser.com" ||
      h == "serverfault.com" || h == "askubuntu.com" ||
      h == "mathoverflow.net" || h == "stackapps.com")
    return true;
  return ends_with(h, ".stackexchange.com") && h != "api.stackexchange.com" &&
         h != "chat.stackexchange.com" && h != "data.stackexchange.com";
}

}  // namespace

std::optional<ForumRef> parse_forum_ref(std::string_view s) {
  trim_ascii(s);
  for (const SiteInfo& si : kSites) {
    if (!starts_with_ci(s, si.prefix)) continue;
    std::string_view rest = s.substr(si.prefix.size());
    ForumRef r;
    r.site = si.site;
    if (si.has_host) {
      const size_t end = rest.find_first_of("/?#");
      r.host = ascii_lower(rest.substr(0, end));
      if (!host_ok(r.host)) return std::nullopt;
      rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end);
    }
    std::string_view path, query, frag;
    split_rest(rest, path, query, frag);
    r.path = std::string(strip_slashes(path));
    r.page = page_of(query);
    r.cursor = query_value(query, "cursor");
    if (!cursor_ok(r.cursor)) r.cursor.clear();
    r.query = decode_query(query_value(query, "query"));
    r.fragment = std::string(frag);
    if (si.site == ForumSite::Bluesky && r.path.empty()) return std::nullopt;
    return r;
  }
  return std::nullopt;
}

bool is_forum_ref(std::string_view ref) { return parse_forum_ref(ref).has_value(); }

const char* forum_site_name(ForumSite s) { return info(s).name; }

std::string forum_ref_string(const ForumRef& r) {
  const SiteInfo& si = info(r.site);
  std::string out(si.prefix);
  if (si.has_host) {
    out += r.host;
    if (!r.path.empty()) out += "/" + r.path;
  } else {
    out += r.path;
    // A board ("fourchan:g/") keeps the slash that tells it from a thread id.
    if (r.site == ForumSite::FourChan && !r.path.empty() &&
        r.path.find('/') == std::string::npos)
      out += "/";
  }
  char sep = '?';
  auto param = [&](const char* key, const std::string& value) {
    out += sep;
    out += key;
    out += "=" + value;
    sep = '&';
  };
  if (!r.query.empty()) param("query", forum::url_encode(r.query));
  if (r.page > 1) param("page", std::to_string(r.page));
  if (!r.cursor.empty()) param("cursor", r.cursor);
  if (!r.fragment.empty()) out += "#" + r.fragment;
  return out;
}

std::string forum_web_url(const ForumRef& r) {
  std::string url;
  switch (r.site) {
    case ForumSite::Discourse:
    case ForumSite::StackExchange:
    case ForumSite::Flarum:
      url = "https://" + r.host + "/" + r.path;
      if (r.page > 1) url += "?page=" + std::to_string(r.page);
      break;
    case ForumSite::Lobsters:
      url = "https://lobste.rs/" + r.path;
      if (r.page > 1)
        url += (r.path.empty() ? "" : "/") + std::string("page/") +
               std::to_string(r.page);
      break;
    case ForumSite::FourChan:
      if (r.path.empty()) return "https://www.4chan.org/";
      url = "https://boards.4chan.org/" + r.path;
      if (r.path.find('/') == std::string::npos) url += "/";
      break;
    case ForumSite::Bluesky:
      url = "https://bsky.app/profile/" + r.path;
      break;
    case ForumSite::LessWrong:
      url = "https://www.lesswrong.com/";
      if (r.path == "new") url += "allPosts?sortedBy=new";
      else if (r.path == "top") url += "allPosts?sortedBy=top&timeframe=allTime";
      else if (r.path == "curated") url += "allPosts?filter=curated&sortedBy=new";
      else if (r.path == "search") url += "search?query=" + forum::url_encode(r.query);
      else url += r.path;
      // A comment's own page on the site.
      if (starts_with(r.path, "posts/") && starts_with(r.fragment, "id:"))
        url += "?commentId=" + r.fragment.substr(3);
      break;
  }
  return url;
}

std::string forum_ref_from_web_url(std::string_view url) {
  std::string host, rest;
  if (!split_web(url, host, rest)) return {};
  std::string_view path, query, frag;
  split_rest(rest, path, query, frag);
  path = strip_slashes(path);
  ForumRef r;
  r.fragment = std::string(frag);
  r.page = page_of(query);
  std::vector<std::string_view> seg;
  for (std::string_view p = path; !p.empty();) {
    const size_t sl = p.find('/');
    seg.push_back(p.substr(0, sl));
    if (sl == std::string_view::npos) break;
    p.remove_prefix(sl + 1);
  }
  if (host == "lobste.rs" || host == "www.lobste.rs") {
    r.site = ForumSite::Lobsters;
    std::string p(path);
    if (ends_with(p, ".json")) p.resize(p.size() - 5);
    // ".../page/N" → page N.
    if (size_t at = p.rfind("page/");
        at != std::string::npos && (at == 0 || p[at - 1] == '/') &&
        all_digits(std::string_view(p).substr(at + 5))) {
      r.page = std::max(1, std::atoi(p.c_str() + at + 5));
      p.erase(at == 0 ? 0 : at - 1);
    }
    const std::string_view first = seg.empty() ? std::string_view{} : seg[0];
    const bool ok = p.empty() || p == "hottest" || p == "newest" ||
                    p == "active" || p == "recent" || first == "s" ||
                    first == "t" || (starts_with(first, "~") && first.size() > 1);
    if (!ok) return {};
    if (first == "s" && seg.size() >= 2) p = "s/" + std::string(seg[1]);
    if (starts_with(first, "~") && seg.size() == 1) p += "/stories";
    if (p == "hottest") p.clear();
    r.path = p;
    return forum_ref_string(r);
  }
  if (host == "boards.4chan.org" || host == "boards.4channel.org") {
    r.site = ForumSite::FourChan;
    if (seg.empty() || seg[0].empty()) return {};
    if (seg.size() >= 3 && seg[1] == "thread" && all_digits(seg[2])) {
      r.path = std::string(seg[0]) + "/thread/" + std::string(seg[2]);
      if (starts_with(r.fragment, "p") &&
          all_digits(std::string_view(r.fragment).substr(1)))
        r.fragment = "id:" + r.fragment.substr(1);
    } else if (seg.size() == 1 || seg[1] == "catalog" || all_digits(seg[1])) {
      r.path = std::string(seg[0]);
      r.fragment.clear();
    } else {
      return {};
    }
    r.page = 1;
    return forum_ref_string(r);
  }
  if (is_se_host(host)) {
    r.site = ForumSite::StackExchange;
    r.host = host;
    if (seg.empty() || (seg.size() == 1 && seg[0] == "questions")) {
      r.path.clear();
    } else if (seg[0] == "questions" && seg.size() >= 2 && all_digits(seg[1])) {
      r.path = "questions/" + std::string(seg[1]);
      r.page = 1;
      // /questions/Q/slug/A → the answer's anchor.
      if (seg.size() >= 4 && all_digits(seg[3]))
        r.fragment = "id:" + std::string(seg[3]);
      else if (all_digits(r.fragment))
        r.fragment = "id:" + r.fragment;
    } else if (seg[0] == "q" && seg.size() >= 2 && all_digits(seg[1])) {
      r.path = "questions/" + std::string(seg[1]);
      r.page = 1;
    } else if (seg[0] == "questions" && seg.size() == 3 && seg[1] == "tagged") {
      r.path = "questions/tagged/" + std::string(seg[2]);
    } else {
      return {};
    }
    return forum_ref_string(r);
  }
  if (host == "lesswrong.com" || host == "www.lesswrong.com") {
    r.site = ForumSite::LessWrong;
    r.page = 1;
    r.fragment.clear();
    auto name = [](std::string_view s) {
      if (s.empty()) return false;
      for (char c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_'))
          return false;
      return true;
    };
    if (seg.empty() || (seg.size() == 1 && seg[0].empty())) {
      r.path.clear();
    } else if (seg[0] == "posts" && seg.size() >= 2 && name(seg[1])) {
      r.path = "posts/" + std::string(seg[1]);
      if (seg.size() >= 3 && name(seg[2])) r.path += "/" + std::string(seg[2]);
      if (std::string c = query_value(query, "commentId"); name(c)) r.fragment = "id:" + c;
    } else if (seg[0] == "users" && seg.size() == 2 && name(seg[1])) {
      r.path = "users/" + std::string(seg[1]);
    } else if ((seg[0] == "w" || seg[0] == "tag") && seg.size() == 2 && name(seg[1])) {
      r.path = "w/" + std::string(seg[1]);
    } else if (seg.size() == 1 && seg[0] == "quicktakes") {
      r.path = "quicktakes";
    } else if (seg.size() == 1 && seg[0] == "search") {
      r.path = "search";
      r.query = decode_query(query_value(query, "query"));
    } else {
      return {};
    }
    return forum_ref_string(r);
  }
  if (host == "bsky.app" || host == "www.bsky.app") {
    r.site = ForumSite::Bluesky;
    r.page = 1;
    if (seg.size() == 2 && seg[0] == "profile")
      r.path = std::string(seg[1]);
    else if (seg.size() == 4 && seg[0] == "profile" && seg[2] == "post")
      r.path = std::string(seg[1]) + "/post/" + std::string(seg[3]);
    else
      return {};
    return forum_ref_string(r);
  }
  return {};
}

std::string forum_up_ref(const ForumRef& r) {
  ForumRef up = r;
  up.page = 1;
  up.cursor.clear();
  up.query.clear();
  up.fragment.clear();
  const std::string& p = r.path;
  if (r.site == ForumSite::Bluesky) {
    const size_t sl = p.find('/');
    if (sl == std::string::npos) return {};
    up.path = p.substr(0, sl);
    return forum_ref_string(up);
  }
  if (p.empty()) return {};
  if (r.site == ForumSite::FourChan && p.find('/') != std::string::npos)
    up.path = p.substr(0, p.find('/'));
  else
    up.path.clear();
  return forum_ref_string(up);
}

// ---- shared helpers ---------------------------------------------------------

namespace forum {

std::string str(const Json& v, std::string_view key) {
  return json_string(json_get(v, key));
}

long long num(const Json& v, std::string_view key) {
  return json_number(json_get(v, key));
}

const std::vector<Json>& arr(const Json& v, std::string_view key) {
  static const std::vector<Json> kEmpty;
  const Json* a = json_get(v, key);
  return a && a->type == Json::Type::Array ? a->items : kEmpty;
}

std::string iso_from_unix(int64_t t) {
  if (t <= 0) return {};
  std::time_t raw = static_cast<std::time_t>(t);
  std::tm utc{};
#if defined(_WIN32)
  if (gmtime_s(&utc, &raw) != 0) return {};
#else
  if (!gmtime_r(&raw, &utc)) return {};
#endif
  char buf[32];
  if (!std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc)) return {};
  return buf;
}

std::string short_time(std::string_view iso) {
  const auto t = parse_timestamp(iso);
  const std::string z = t ? iso_from_unix(*t) : std::string(iso);
  if (z.size() < 16) return z;
  return z.substr(0, 10) + " " + z.substr(11, 5) + " UTC";
}

std::string count(long long n, std::string_view one, std::string_view many) {
  return std::to_string(n) + " " + std::string(n == 1 || n == -1 ? one : many);
}

std::string json_quote(std::string_view s) {
  static const char* kHex = "0123456789abcdef";
  std::string o = "\"";
  for (char c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      o += '\\';
      o += c;
    } else if (u < 0x20) {
      o += "\\u00";
      o += kHex[u >> 4];
      o += kHex[u & 15];
    } else {
      o += c;
    }
  }
  return o + "\"";
}

std::string url_encode(std::string_view s) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 15]);
    }
  }
  return out;
}

void add_text(Document& doc, NodeId node, const std::string& text,
              InlineKind kind) {
  if (!text.empty()) doc.append_text(node, text, kind);
}

void add_link(Document& doc, NodeId node, const std::string& text,
              const std::string& href, InlineRole role) {
  InlineSpan s;
  s.kind = InlineKind::Link;
  s.role = role;
  s.text = text;
  s.href = href;
  doc.get(node).spans.push_back(std::move(s));
}

void add_meta(Document& doc, NodeId node, const std::string& text) {
  if (text.empty()) return;
  InlineSpan s;
  s.role = InlineRole::Metadata;
  s.text = text;
  doc.get(node).spans.push_back(std::move(s));
}

void add_sep(Document& doc, NodeId node) {
  if (!doc.get(node).spans.empty()) add_meta(doc, node, " · ");
}

NodeId add_message(Document& doc, NodeId parent, const MsgHead& h) {
  NodeId msg = doc.create(NodeKind::Message, parent);
  Node& n = doc.get(msg);
  n.label = h.label;
  n.src = h.author.empty() ? "[deleted]" : h.author;
  n.summary = short_time(h.iso);
  if (h.indent) n.alt = h.reply_to;
  // Slots are positional; a Strong placeholder keeps slot 1 in place when
  // there is no profile link.
  InlineSpan author;
  author.kind = h.author_href.empty() ? InlineKind::Strong : InlineKind::Link;
  author.text = n.src;
  author.href = h.author_href;
  n.spans.push_back(std::move(author));
  InlineSpan date;
  date.text = n.summary;
  date.datetime = h.iso;
  date.kind = h.permalink.empty() ? InlineKind::Strong : InlineKind::Link;
  date.href = h.permalink;
  n.spans.push_back(std::move(date));
  if (!h.reply_to.empty()) {
    InlineSpan reply;
    reply.kind = InlineKind::Link;
    reply.href = "#id:" + h.reply_to;
    n.spans.push_back(std::move(reply));
  }
  if (!h.score.empty()) {
    InlineSpan score;
    score.role = InlineRole::Metadata;
    score.rel = "score";
    score.text = h.score;
    n.spans.push_back(std::move(score));
  }
  return msg;
}

void append_html(Document& doc, NodeId parent, std::string_view html,
                 const std::string& base, bool images) {
  HtmlOptions opt;
  opt.base_url = base;
  opt.images = images;
  html_append_blocks(doc, parent, html, opt);
}

void append_plain(Document& doc, NodeId parent, std::string_view text) {
  NodeId para = INVALID_NODE;
  size_t i = 0;
  while (i <= text.size()) {
    const size_t nl = text.find('\n', i);
    std::string_view l = text.substr(
        i, nl == std::string_view::npos ? std::string_view::npos : nl - i);
    if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
    if (l.find_first_not_of(" \t") == std::string_view::npos) {
      para = INVALID_NODE;  // blank line: next text starts a paragraph
    } else {
      if (para == INVALID_NODE) para = doc.create(NodeKind::Paragraph, parent);
      else doc.append_text(para, "\n");
      doc.append_text(para, std::string(l));
    }
    if (nl == std::string_view::npos) break;
    i = nl + 1;
  }
}

void map_links(Document& doc, const std::function<void(std::string&)>& fn) {
  for (Node& n : doc.nodes)
    for (InlineSpan& s : n.spans)
      if (s.kind == InlineKind::Link && !s.href.empty() && s.rel != "external")
        fn(s.href);
}

NodeId start_document(Document& doc, const std::string& title,
                      const std::string& site, const std::vector<NavItem>& nav,
                      const std::string& thread_label) {
  NodeId root = doc.create(NodeKind::Document);
  doc.meta.title = title;
  if (!nav.empty()) {
    NodeId line = doc.create(NodeKind::Paragraph, root);
    doc.append_text(line, site + ": ", InlineKind::Strong);
    bool first = true;
    for (const NavItem& item : nav) {
      if (!first) doc.append_text(line, " · ");
      first = false;
      if (item.current || item.ref.empty())
        doc.append_text(line, item.text, InlineKind::Strong);
      else
        add_link(doc, line, item.text, item.ref);
    }
  }
  NodeId thread = doc.create(NodeKind::Thread, root);
  doc.get(thread).summary = title;
  doc.get(thread).label = thread_label;
  return thread;
}

std::string with_page(ForumRef ref, int page) {
  ref.page = page;
  ref.cursor.clear();
  ref.fragment.clear();
  return forum_ref_string(ref);
}

void add_pager(Document& doc, const ForumRef& ref, bool has_next,
               const std::string& next_word) {
  if (ref.page <= 1 && !has_next) return;
  NodeId nav = doc.create(NodeKind::Paragraph, doc.root);
  if (ref.page > 1) {
    add_link(doc, nav, "← Previous", with_page(ref, ref.page - 1));
    doc.append_text(nav, " · Page " + std::to_string(ref.page));
  }
  if (has_next) {
    if (ref.page > 1) doc.append_text(nav, " · ");
    add_link(doc, nav, next_word + " →", with_page(ref, ref.page + 1));
  }
}

void add_web_link(Document& doc, NodeId parent, const ForumRef& ref) {
  const std::string web = forum_web_url(ref);
  if (web.empty()) return;
  NodeId para = doc.create(NodeKind::Paragraph, parent);
  InlineSpan s;
  s.kind = InlineKind::Link;
  s.role = InlineRole::Metadata;
  s.rel = "external";
  s.text = std::string("Open on ") + forum_site_name(ref.site) + " (web)";
  s.href = web;
  doc.get(para).spans.push_back(std::move(s));
}

void add_note(Document& doc, NodeId parent, const std::string& text) {
  doc.append_text(doc.create(NodeKind::Paragraph, parent), text,
                  InlineKind::Emphasis);
}

}  // namespace forum

}  // namespace xanatoo
