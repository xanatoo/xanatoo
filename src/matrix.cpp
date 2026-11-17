// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
//
// Read-only Matrix client (Client-Server API). Room directories become
// lists of Article entries, rooms a header plus a Thread of Messages,
// users a short profile. Never joins rooms, sends events or registers.

#include "xanatoo/matrix.hpp"
#include "xanatoo/html_min.hpp"
#include "xanatoo/json_min.hpp"
#include "xanatoo/util.hpp"

#if defined(XANATOO_MATRIX) && XANATOO_MATRIX
#include "xanatoo/catalog_credentials.hpp"
#include "xanatoo/http_status.hpp"
#include "xanatoo/https.hpp"
#include "xanatoo/matrix_directory.hpp"
#include "xanatoo/version.hpp"

#include <iostream>
#include <mutex>
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace xanatoo {

namespace {

constexpr std::string_view kPrefix = "matrix:";

bool unreserved(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.' ||
         c == '_' || c == '~';
}

std::string percent_decode(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() &&
        hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
      out.push_back(static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2])));
      i += 2;
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

// Id parts in a matrix: URI keep ':' (the server separator) literal.
std::string escape_id_part(std::string_view s) {
  std::string out;
  for (char c : s) {
    if (unreserved(c) || c == ':') {
      out.push_back(c);
    } else {
      char buf[4];
      std::snprintf(buf, sizeof buf, "%%%02X", static_cast<unsigned char>(c));
      out += buf;
    }
  }
  return out;
}

// No control characters, no whitespace, bounded: ids end up in URLs,
// labels and the persisted catalogue.
bool sane_id(std::string_view id) {
  if (id.size() < 2 || id.size() > 255) return false;
  for (char c : id)
    if (static_cast<unsigned char>(c) <= 32 || c == 127) return false;
  return true;
}

char sigil_for(std::string_view kind) {
  if (kind == "r" || kind == "room") return '#';
  if (kind == "roomid") return '!';
  if (kind == "u" || kind == "user") return '@';
  return 0;
}

std::string ref_kind_word(MatrixRefKind k) {
  switch (k) {
    case MatrixRefKind::Alias: return "r";
    case MatrixRefKind::RoomId: return "roomid";
    case MatrixRefKind::User: return "u";
    case MatrixRefKind::Directory: return "directory";
  }
  return "r";
}

void parse_query(std::string_view q, MatrixRef& ref) {
  while (!q.empty()) {
    const size_t amp = q.find('&');
    std::string_view kv = q.substr(0, amp);
    q = amp == std::string_view::npos ? std::string_view() : q.substr(amp + 1);
    const size_t eq = kv.find('=');
    if (eq == std::string_view::npos) continue;
    const std::string_view key = kv.substr(0, eq);
    std::string value = percent_decode(kv.substr(eq + 1));
    if (value.empty() || value.size() > 512) continue;
    if (key == "via") {
      const std::string server = matrix_server_name(value);
      if (!server.empty() && ref.via.size() < 8) ref.via.push_back(server);
    } else if (key == "from") {
      ref.from = std::move(value);
    } else if (key == "since") {
      ref.since = std::move(value);
    } else if (key == "q") {
      std::replace(value.begin(), value.end(), '+', ' ');
      ref.query = std::move(value);
    }
  }
}

std::string first_line(std::string_view s, size_t max) {
  std::string out(s.substr(0, s.find('\n')));
  if (out.size() > max) {
    out.resize(max);
    while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80)
      out.pop_back();
    if (!out.empty()) out.pop_back();
    out += "…";
  }
  return out;
}

}  // namespace

std::string matrix_escape(std::string_view s) {
  std::string out;
  for (char c : s) {
    if (unreserved(c)) {
      out.push_back(c);
    } else {
      char buf[4];
      std::snprintf(buf, sizeof buf, "%%%02X", static_cast<unsigned char>(c));
      out += buf;
    }
  }
  return out;
}

std::string matrix_server_name(std::string_view input) {
  std::string s = ascii_lower(input);
  if (s.empty() || s.size() > 255) return {};
  std::string_view host = s;
  std::string_view port;
  if (host.front() == '[') {
    const size_t close = host.find(']');
    if (close == std::string_view::npos || close < 3) return {};
    for (char c : host.substr(1, close - 1))
      if (!std::isxdigit(static_cast<unsigned char>(c)) && c != ':' && c != '.')
        return {};
    if (close + 1 < host.size()) {
      if (host[close + 1] != ':') return {};
      port = host.substr(close + 2);
      if (port.empty()) return {};
    }
  } else {
    const size_t colon = host.find(':');
    if (colon != std::string_view::npos) {
      port = host.substr(colon + 1);
      host = host.substr(0, colon);
      if (port.empty()) return {};
    }
    if (host.empty() || host.front() == '.' || host.front() == '-' ||
        host.back() == '.' || host.back() == '-' ||
        host.find("..") != std::string_view::npos)
      return {};
    for (char c : host)
      if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '.')
        return {};
  }
  if (port.size() > 5) return {};
  for (char c : port)
    if (!std::isdigit(static_cast<unsigned char>(c))) return {};
  return s;
}

std::string matrix_id_server(std::string_view id) {
  const size_t colon = id.find(':');
  if (colon == std::string_view::npos) return {};
  return matrix_server_name(id.substr(colon + 1));
}

std::optional<MatrixRef> parse_matrix_ref(std::string_view s) {
  trim_ascii(s);
  if (!starts_with_ci(s, kPrefix)) return std::nullopt;
  s.remove_prefix(kPrefix.size());
  if (starts_with(s, "//")) return std::nullopt;  // no authority form
  std::string_view query;
  if (const size_t q = s.find('?'); q != std::string_view::npos) {
    query = s.substr(q + 1);
    s = s.substr(0, q);
  }
  if (const size_t h = s.find('#'); h != std::string_view::npos) s = s.substr(0, h);

  MatrixRef ref;
  const size_t slash = s.find('/');
  const std::string kind = ascii_lower(s.substr(0, slash));
  std::string_view rest =
      slash == std::string_view::npos ? std::string_view() : s.substr(slash + 1);
  parse_query(query, ref);

  if (kind == "directory") {
    ref.kind = MatrixRefKind::Directory;
    while (!rest.empty() && rest.back() == '/') rest.remove_suffix(1);
    if (!rest.empty()) {
      ref.id = matrix_server_name(percent_decode(rest));
      if (ref.id.empty()) return std::nullopt;
    }
    return ref;
  }
  const char sigil = sigil_for(kind);
  if (!sigil) return std::nullopt;
  std::string_view id_part = rest.substr(0, rest.find('/'));
  std::string_view tail = id_part.size() < rest.size()
                              ? rest.substr(id_part.size() + 1)
                              : std::string_view();
  ref.kind = sigil == '#' ? MatrixRefKind::Alias :
             sigil == '!' ? MatrixRefKind::RoomId : MatrixRefKind::User;
  ref.id = std::string(1, sigil) + percent_decode(id_part);
  if (!sane_id(ref.id)) return std::nullopt;
  // Aliases and users always name their server; room ids may not (v12).
  if (ref.kind != MatrixRefKind::RoomId && matrix_id_server(ref.id).empty())
    return std::nullopt;
  if (!tail.empty()) {
    if (ref.kind == MatrixRefKind::User) return std::nullopt;
    const size_t e = tail.find('/');
    if (e == std::string_view::npos ||
        (tail.substr(0, e) != "e" && tail.substr(0, e) != "event"))
      return std::nullopt;
    std::string_view ev = tail.substr(e + 1);
    while (!ev.empty() && ev.back() == '/') ev.remove_suffix(1);
    ref.event = "$" + percent_decode(ev);
    if (!sane_id(ref.event) || ref.event.find('/') != std::string::npos)
      return std::nullopt;
  }
  return ref;
}

bool is_matrix_ref(std::string_view s) { return parse_matrix_ref(s).has_value(); }

std::string format_matrix_ref(const MatrixRef& ref) {
  std::string out(kPrefix);
  out += ref_kind_word(ref.kind);
  out += "/";
  if (ref.kind == MatrixRefKind::Directory) {
    out += ref.id;
  } else {
    out += escape_id_part(std::string_view(ref.id).substr(ref.id.empty() ? 0 : 1));
    if (!ref.event.empty())
      out += "/e/" + escape_id_part(std::string_view(ref.event).substr(1));
  }
  char sep = '?';
  auto param = [&](std::string_view key, std::string_view value) {
    if (value.empty()) return;
    out.push_back(sep);
    sep = '&';
    out += key;
    out += "=";
    out += matrix_escape(value);
  };
  for (const auto& v : ref.via) param("via", v);
  param("from", ref.from);
  param("since", ref.since);
  param("q", ref.query);
  return out;
}

std::string matrix_ref_for_id(std::string_view id) {
  if (!sane_id(id)) return {};
  MatrixRef ref;
  ref.id = std::string(id);
  switch (id.front()) {
    case '#': ref.kind = MatrixRefKind::Alias; break;
    case '!': ref.kind = MatrixRefKind::RoomId; break;
    case '@': ref.kind = MatrixRefKind::User; break;
    default: return {};
  }
  if (ref.kind != MatrixRefKind::RoomId && matrix_id_server(id).empty()) return {};
  return format_matrix_ref(ref);
}

std::string matrix_ref_from_url(std::string_view url) {
  trim_ascii(url);
  constexpr std::string_view kTo = "https://matrix.to/#/";
  if (!starts_with_ci(url, kTo)) return {};
  url.remove_prefix(kTo.size());
  std::string_view query;
  if (const size_t q = url.find('?'); q != std::string_view::npos) {
    query = url.substr(q + 1);
    url = url.substr(0, q);
  }
  const size_t slash = url.find('/');
  const std::string id = percent_decode(url.substr(0, slash));
  std::string event;
  if (slash != std::string_view::npos) event = percent_decode(url.substr(slash + 1));
  const std::string base = matrix_ref_for_id(id);
  if (base.empty()) return {};
  MatrixRef ref = *parse_matrix_ref(base);
  if (!event.empty()) {
    if (ref.kind == MatrixRefKind::User || event[0] != '$' || !sane_id(event))
      return base;
    ref.event = event;
  }
  parse_query(query, ref);
  return format_matrix_ref(ref);
}

std::string matrix_to_url(std::string_view ref_text) {
  auto ref = parse_matrix_ref(ref_text);
  if (!ref || ref->kind == MatrixRefKind::Directory) return {};
  // matrix.to keeps the sigil.
  std::string out = "https://matrix.to/#/" + std::string(1, ref->id[0]) +
        escape_id_part(std::string_view(ref->id).substr(1));
  if (!ref->event.empty())
    out += "/$" + escape_id_part(std::string_view(ref->event).substr(1));
  char sep = '?';
  for (const auto& v : ref->via) {
    out.push_back(sep);
    sep = '&';
    out += "via=" + matrix_escape(v);
  }
  return out;
}

std::string matrix_iso_time(int64_t ms) {
  if (ms <= 0) return {};
  int64_t secs = ms / 1000;
  int64_t days = secs / 86400;
  int64_t rem = secs % 86400;
  // Howard Hinnant's civil_from_days.
  int64_t z = days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  const int64_t d = doy - (153 * mp + 2) / 5 + 1;
  const int64_t m = mp < 10 ? mp + 3 : mp - 9;
  if (m <= 2) ++y;
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04lld-%02lld-%02lldT%02lld:%02lld:%02lldZ",
                static_cast<long long>(y), static_cast<long long>(m),
                static_cast<long long>(d), static_cast<long long>(rem / 3600),
                static_cast<long long>(rem / 60 % 60),
                static_cast<long long>(rem % 60));
  return buf;
}

// ---------------------------------------------------------------------------
// Documents
// ---------------------------------------------------------------------------

namespace {

struct RoomInfo {
  std::string room_id, alias, name, topic, room_type, join_rule;
  int64_t members = 0;
  bool world_readable = false;
  bool guest_can_join = false;
};

RoomInfo parse_room(std::string_view obj) {
  const std::string top = json_top_level(obj);
  RoomInfo r;
  r.room_id = json_str_field(top, "room_id");
  r.alias = json_str_field(top, "canonical_alias");
  r.name = json_str_field(top, "name");
  r.topic = json_str_field(top, "topic");
  r.room_type = json_str_field(top, "room_type");
  r.join_rule = json_str_field(top, "join_rule");
  r.members = json_int_field(top, "num_joined_members");
  r.world_readable = json_bool_field(top, "world_readable");
  r.guest_can_join = json_bool_field(top, "guest_can_join");
  return r;
}

std::string room_ref(const RoomInfo& r, std::string_view via) {
  if (!r.alias.empty()) {
    if (std::string ref = matrix_ref_for_id(r.alias); !ref.empty()) return ref;
  }
  MatrixRef ref;
  ref.kind = MatrixRefKind::RoomId;
  ref.id = r.room_id;
  if (!sane_id(ref.id) || ref.id[0] != '!') return {};
  if (const std::string server = matrix_server_name(via); !server.empty())
    ref.via.push_back(server);
  return format_matrix_ref(ref);
}

std::string room_title(const RoomInfo& r) {
  if (!r.name.empty()) return r.name;
  if (!r.alias.empty()) return r.alias;
  return r.room_id.empty() ? "(unnamed room)" : r.room_id;
}

InlineSpan link_span(std::string text, std::string href,
                     InlineRole role = InlineRole::Normal) {
  InlineSpan s;
  s.kind = InlineKind::Link;
  s.role = role;
  s.text = std::move(text);
  s.href = std::move(href);
  return s;
}

InlineSpan meta_span(std::string text) {
  InlineSpan s;
  s.role = InlineRole::Metadata;
  s.text = std::move(text);
  return s;
}

void add_paragraph(Document& doc, NodeId parent, const std::string& text) {
  if (text.empty()) return;
  NodeId p = doc.create(NodeKind::Paragraph, parent);
  doc.append_text(p, text);
}

std::string plural(int64_t n, const char* one, const char* many) {
  return std::to_string(n) + " " + (n == 1 ? one : many);
}

// One room of a directory or space: title link, topic, details.
void append_room_entry(Document& doc, NodeId thread, const RoomInfo& r,
                       std::string_view via) {
  NodeId entry = doc.create(NodeKind::Article, thread);
  doc.get(entry).label = r.room_id;
  NodeId title = doc.create(NodeKind::Header, entry);
  const std::string ref = room_ref(r, via);
  if (ref.empty()) doc.append_text(title, room_title(r), InlineKind::Strong);
  else doc.get(title).spans.push_back(link_span(room_title(r), ref));
  if (r.room_type == "m.space") doc.get(title).spans.push_back(meta_span(" (space)"));
  if (!r.topic.empty()) add_paragraph(doc, entry, first_line(r.topic, 400));
  NodeId foot = doc.create(NodeKind::Caption, entry);
  std::string detail;
  if (!r.alias.empty() && r.alias != room_title(r)) detail = r.alias + " · ";
  detail += plural(r.members, "member", "members");
  if (r.world_readable) detail += " · public history";
  doc.append_text(foot, detail);
}

NodeId heading(Document& doc, NodeId root, int level, const std::string& text) {
  NodeId h = doc.create(NodeKind::Heading, root);
  doc.get(h).level = level;
  doc.append_text(h, text);
  return h;
}

void footer(Document& doc, NodeId root) {
  add_paragraph(doc, root,
                "Matrix · read-only · Ctrl+L opens matrix:r/room:server, "
                "matrix:u/user:server or matrix:directory/server");
}

// matrix.to links (mention pills, permalinks) → native refs.
void rewrite_links(Document& doc) {
  for (Node& n : doc.nodes)
    for (InlineSpan& s : n.spans)
      if (s.kind == InlineKind::Link && s.rel != "external")
        if (std::string ref = matrix_ref_from_url(s.href); !ref.empty())
          s.href = std::move(ref);
}

std::string html_escape(std::string_view s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '\n': out += "<br>"; break;
      default: out.push_back(c);
    }
  }
  return out;
}

std::string strip_mx_reply(std::string html) {
  for (;;) {
    const size_t a = html.find("<mx-reply>");
    if (a == std::string::npos) break;
    const size_t b = html.find("</mx-reply>", a);
    html.erase(a, b == std::string::npos ? std::string::npos : b + 11 - a);
  }
  return html;
}

// Plain reply fallback: leading "> " lines, then one blank line.
std::string strip_reply_quote(std::string_view body) {
  size_t i = 0;
  bool quoted = false;
  while (i < body.size() && body[i] == '>') {
    quoted = true;
    const size_t nl = body.find('\n', i);
    if (nl == std::string_view::npos) return {};
    i = nl + 1;
  }
  if (quoted && i < body.size() && body[i] == '\n') ++i;
  return std::string(body.substr(quoted ? i : 0));
}

struct Msg {
  std::string event_id, sender, msgtype, body, html, reply_to, thread_root;
  int64_t ts = 0;
  bool encrypted = false;
};

struct Timeline {
  std::vector<Msg> msgs;  // chronological
  std::map<std::string, std::string> names;  // user id → display name
};

void collect_members(std::string_view json, std::string_view key, Timeline& t) {
  for (const auto& ev : json_object_array_after_key(json, key)) {
    const std::string top = json_top_level(ev);
    if (json_str_field(top, "type") != "m.room.member") continue;
    const std::string user = json_str_field(top, "state_key");
    const std::string name =
        json_str_field(json_top_level(json_object_after_key(ev, "content")),
                       "displayname");
    if (!user.empty() && !name.empty()) t.names[user] = first_line(name, 80);
  }
}

void collect_event(const std::string& ev, Timeline& t,
                   std::map<std::string, size_t>& by_id) {
  const std::string top = json_top_level(ev);
  const std::string type = json_str_field(top, "type");
  const std::string_view content = json_object_after_key(ev, "content");
  const std::string ctop = json_top_level(content);
  if (type == "m.room.member") {
    const std::string name = json_str_field(ctop, "displayname");
    const std::string user = json_str_field(top, "state_key");
    if (!user.empty() && !name.empty()) t.names[user] = first_line(name, 80);
    return;
  }
  if (type != "m.room.message" && type != "m.room.encrypted" &&
      type != "m.sticker")
    return;
  Msg m;
  m.event_id = json_str_field(top, "event_id");
  m.sender = json_str_field(top, "sender");
  m.ts = json_int_field(top, "origin_server_ts");
  m.encrypted = type == "m.room.encrypted";
  const std::string_view rel = json_object_after_key(content, "m.relates_to");
  const std::string rtop = json_top_level(rel);
  const std::string rel_type = json_str_field(rtop, "rel_type");
  if (rel_type == "m.replace") {
    // An edit: replace the shown text of the original, when it is here.
    const std::string target = json_str_field(rtop, "event_id");
    const std::string_view nc = json_object_after_key(content, "m.new_content");
    auto it = by_id.find(target);
    if (it != by_id.end() && !nc.empty()) {
      const std::string ntop = json_top_level(nc);
      Msg& orig = t.msgs[it->second];
      orig.body = json_str_field(ntop, "body");
      orig.html = json_str_field(ntop, "format") == "org.matrix.custom.html"
                      ? json_str_field(ntop, "formatted_body") : std::string();
    }
    return;
  }
  if (!m.encrypted) {
    m.msgtype = json_str_field(ctop, "msgtype");
    m.body = json_str_field(ctop, "body");
    if (type == "m.sticker") m.msgtype = "m.sticker";
    if (m.body.empty() && m.msgtype.empty()) return;  // redacted
    if (json_str_field(ctop, "format") == "org.matrix.custom.html")
      m.html = json_str_field(ctop, "formatted_body");
  }
  const std::string_view irt = json_object_after_key(rel, "m.in_reply_to");
  const bool fallback = json_bool_field(rtop, "is_falling_back");
  if (!irt.empty() && !fallback) m.reply_to = json_str_field(irt, "event_id");
  if (rel_type == "m.thread") m.thread_root = json_str_field(rtop, "event_id");
  if (!m.reply_to.empty()) {
    m.html = strip_mx_reply(std::move(m.html));
    m.body = strip_reply_quote(m.body);
  }
  if (m.event_id.empty()) return;
  by_id[m.event_id] = t.msgs.size();
  t.msgs.push_back(std::move(m));
}

std::string label_of(std::string_view event_id) {
  return std::string(event_id.substr(event_id.empty() || event_id[0] != '$' ? 0 : 1));
}

std::string display_time(const std::string& iso) {
  if (iso.size() < 16) return iso;
  return iso.substr(0, 10) + " " + iso.substr(11, 5) + " UTC";
}

void append_timeline(Document& doc, NodeId root, const Timeline& t,
                     const std::string& room_id,
                     const std::vector<std::string>& via) {
  NodeId thread = doc.create(NodeKind::Thread, root);
  std::set<std::string> present;
  for (const Msg& m : t.msgs) present.insert(m.event_id);
  for (const Msg& m : t.msgs) {
    NodeId msg = doc.create(NodeKind::Message, thread);
    Node& n = doc.get(msg);
    n.label = label_of(m.event_id);
    auto name_it = t.names.find(m.sender);
    const std::string name = name_it != t.names.end() ? name_it->second : m.sender;
    n.src = name.empty() ? "(unknown)" : name;
    const std::string iso = matrix_iso_time(m.ts);
    n.summary = display_time(iso);
    const std::string parent = !m.reply_to.empty() ? m.reply_to : m.thread_root;
    if (!parent.empty() && present.count(parent)) n.alt = label_of(parent);

    InlineSpan author;
    author.kind = InlineKind::Strong;
    author.text = n.src;
    if (std::string ref = matrix_ref_for_id(m.sender); !ref.empty()) {
      author.kind = InlineKind::Link;
      author.href = std::move(ref);
    }
    n.spans.push_back(std::move(author));
    if (!iso.empty()) {
      InlineSpan date;
      date.text = n.summary;
      date.datetime = iso;
      MatrixRef ev;
      ev.kind = MatrixRefKind::RoomId;
      ev.id = room_id;
      ev.event = m.event_id;
      ev.via = via;
      if (sane_id(room_id) && sane_id(m.event_id)) {
        date.kind = InlineKind::Link;
        date.href = format_matrix_ref(ev);
      }
      doc.get(msg).spans.push_back(std::move(date));
    }

    if (m.encrypted) {
      NodeId p = doc.create(NodeKind::Caption, msg);
      doc.get(p).spans.push_back(meta_span("[encrypted message]"));
      continue;
    }
    const bool media = m.msgtype == "m.image" || m.msgtype == "m.file" ||
                       m.msgtype == "m.video" || m.msgtype == "m.audio" ||
                       m.msgtype == "m.sticker";
    if (media) {
      NodeId p = doc.create(NodeKind::Caption, msg);
      const std::string what = m.msgtype == "m.sticker" ? "sticker" :
                               m.msgtype.substr(2);
      doc.get(p).spans.push_back(meta_span("[" + what + ": " + first_line(m.body, 200) + "]"));
      continue;
    }
    std::string html = !m.html.empty() ? m.html : html_escape(m.body);
    if (m.msgtype == "m.emote") html = "* " + html_escape(n.src) + " " + html;
    HtmlOptions opt;
    opt.base_url = "https://matrix.to/";
    const size_t before = doc.get(msg).children.size();
    html_append_blocks(doc, msg, html, opt);
    if (doc.get(msg).children.size() == before)
      add_paragraph(doc, msg, m.body);
  }
}

void earlier_link(Document& doc, NodeId root, const std::string& room_id,
                  const std::vector<std::string>& via, const std::string& token) {
  if (token.empty() || !sane_id(room_id)) return;
  MatrixRef ref;
  ref.kind = MatrixRefKind::RoomId;
  ref.id = room_id;
  ref.via = via;
  ref.from = token;
  NodeId p = doc.create(NodeKind::Caption, root);
  doc.get(p).spans.push_back(
      link_span("← Earlier messages", format_matrix_ref(ref), InlineRole::Metadata));
}

}  // namespace

Document matrix_directory_document(std::string_view server, std::string_view json,
                                   const MatrixRef& ref, std::string_view note) {
  Document doc;
  NodeId root = doc.create(NodeKind::Document);
  const std::string title = "Matrix rooms · " + std::string(server) +
                            (ref.query.empty() ? "" : " · “" + ref.query + "”");
  doc.meta.title = title;
  heading(doc, root, 1, title);
  const std::string top = json_top_level(json);
  const int64_t total = json_int_field(top, "total_room_count_estimate");
  if (total > 0) {
    NodeId c = doc.create(NodeKind::Caption, root);
    doc.append_text(c, "About " + plural(total, "public room", "public rooms") +
                           " on this server's directory");
  }
  if (!note.empty()) add_paragraph(doc, root, std::string(note));
  NodeId thread = doc.create(NodeKind::Thread, root);
  int count = 0;
  for (const auto& obj : json_object_array_after_key(json, "chunk")) {
    append_room_entry(doc, thread, parse_room(obj), server);
    ++count;
  }
  if (count == 0) add_paragraph(doc, root, "No public rooms returned by this server.");
  if (const std::string next = json_str_field(top, "next_batch"); !next.empty()) {
    MatrixRef more = ref;
    more.since = next;
    NodeId p = doc.create(NodeKind::Caption, root);
    doc.get(p).spans.push_back(
        link_span("More rooms →", format_matrix_ref(more), InlineRole::Metadata));
  }
  footer(doc, root);
  return doc;
}

Document matrix_room_document(const MatrixRef& ref, const MatrixRoomData& data) {
  Document doc;
  NodeId root = doc.create(NodeKind::Document);
  RoomInfo info = data.summary_json.empty() ? RoomInfo{} : parse_room(data.summary_json);
  if (info.room_id.empty()) info.room_id = data.room_id;
  if (info.alias.empty() && ref.kind == MatrixRefKind::Alias) info.alias = ref.id;
  const std::string title =
      info.name.empty() && info.alias.empty() && info.room_id.empty()
          ? ref.id : room_title(info);
  doc.meta.title = title;
  heading(doc, root, 1, title);

  NodeId caption = doc.create(NodeKind::Caption, root);
  std::vector<std::string> parts;
  if (!info.alias.empty() && info.alias != title) parts.push_back(info.alias);
  if (info.room_type == "m.space") parts.push_back("space");
  if (!data.summary_json.empty()) {
    parts.push_back(plural(info.members, "member", "members"));
    if (!info.join_rule.empty()) parts.push_back(info.join_rule);
    if (info.world_readable) parts.push_back("public history");
  }
  for (const auto& p : parts) {
    if (!doc.get(caption).spans.empty()) doc.append_text(caption, " · ");
    doc.append_text(caption, p);
  }
  MatrixRef self = ref;
  self.event.clear();
  self.from.clear();
  if (self.kind == MatrixRefKind::RoomId && self.via.empty()) self.via = data.via;
  if (const std::string web = matrix_to_url(format_matrix_ref(self)); !web.empty()) {
    if (!doc.get(caption).spans.empty()) doc.append_text(caption, " · ");
    InlineSpan s = link_span("matrix.to", web, InlineRole::Metadata);
    s.rel = "external";
    doc.get(caption).spans.push_back(std::move(s));
  }
  if (!info.topic.empty()) {
    HtmlOptions opt;
    html_append_blocks(doc, root, "<p>" + html_escape(info.topic) + "</p>", opt);
  }

  if (!data.hierarchy_json.empty()) {
    heading(doc, root, 2, "Rooms in this space");
    NodeId thread = doc.create(NodeKind::Thread, root);
    const std::string via = data.via.empty() ? matrix_id_server(info.room_id)
                                             : data.via.front();
    int count = 0;
    for (const auto& obj : json_object_array_after_key(data.hierarchy_json, "rooms")) {
      RoomInfo child = parse_room(obj);
      if (child.room_id == info.room_id) continue;  // the space itself
      append_room_entry(doc, thread, child, via);
      ++count;
    }
    if (count == 0) add_paragraph(doc, root, "This space lists no public rooms.");
  }

  const std::string_view events_json =
      !data.context_json.empty() ? std::string_view(data.context_json)
                                 : std::string_view(data.messages_json);
  if (!events_json.empty()) {
    Timeline t;
    std::map<std::string, size_t> by_id;
    collect_members(events_json, "state", t);
    std::string earlier;
    if (!data.context_json.empty()) {
      // events_before is newest first; then the event; then events_after.
      auto before = json_object_array_after_key(events_json, "events_before");
      for (auto it = before.rbegin(); it != before.rend(); ++it) collect_event(*it, t, by_id);
      const std::string_view ev = json_object_after_key(events_json, "event");
      if (!ev.empty()) collect_event(std::string(ev), t, by_id);
      for (const auto& e : json_object_array_after_key(events_json, "events_after"))
        collect_event(e, t, by_id);
      earlier = json_str_field(json_top_level(events_json), "start");
    } else {
      auto chunk = json_object_array_after_key(events_json, "chunk");
      for (auto it = chunk.rbegin(); it != chunk.rend(); ++it) collect_event(*it, t, by_id);
      earlier = json_str_field(json_top_level(events_json), "end");
    }
    heading(doc, root, 2, "Messages");
    earlier_link(doc, root, info.room_id, self.via, earlier);
    if (t.msgs.empty()) add_paragraph(doc, root, "No messages in this part of the history.");
    else append_timeline(doc, root, t, info.room_id, self.via);
  }
  if (!data.note.empty()) {
    NodeId note = doc.create(NodeKind::Note, root);
    doc.append_text(note, data.note);
  }
  footer(doc, root);
  rewrite_links(doc);
  return doc;
}

Document matrix_profile_document(std::string_view user_id, std::string_view json,
                                 std::string_view note) {
  Document doc;
  NodeId root = doc.create(NodeKind::Document);
  const std::string top = json_top_level(json);
  std::string name = first_line(json_str_field(top, "displayname"), 200);
  if (name.empty()) name = std::string(user_id);
  doc.meta.title = name;
  heading(doc, root, 1, name);
  NodeId caption = doc.create(NodeKind::Caption, root);
  doc.append_text(caption, std::string(user_id));
  if (const std::string web = matrix_to_url(matrix_ref_for_id(user_id)); !web.empty()) {
    doc.append_text(caption, " · ");
    InlineSpan s = link_span("matrix.to", web, InlineRole::Metadata);
    s.rel = "external";
    doc.get(caption).spans.push_back(std::move(s));
  }
  if (const std::string server = matrix_id_server(user_id); !server.empty()) {
    NodeId p = doc.create(NodeKind::Paragraph, root);
    doc.append_text(p, "Home server: ");
    MatrixRef dir;
    dir.kind = MatrixRefKind::Directory;
    dir.id = server;
    doc.get(p).spans.push_back(link_span(server, format_matrix_ref(dir)));
    doc.append_text(p, " (public room directory)");
  }
  if (!note.empty()) {
    NodeId n = doc.create(NodeKind::Note, root);
    doc.append_text(n, std::string(note));
  }
  footer(doc, root);
  return doc;
}

std::vector<MatrixDirectoryEntry> matrix_discover_payload(std::string_view json,
                                                          std::string_view via) {
  std::vector<MatrixDirectoryEntry> out;
  std::set<std::string> servers;
  auto note_server = [&](std::string_view id) {
    if (std::string s = matrix_id_server(id); !s.empty()) servers.insert(s);
  };
  auto add_room = [&](std::string_view obj) {
    const std::string top = json_top_level(obj);
    if (json_str_field(top, "room_id").empty() ||
        !json_str_field(top, "event_id").empty())
      return;
    const RoomInfo r = parse_room(obj);
    MatrixDirectoryEntry e;
    e.kind = r.room_type == "m.space" ? MatrixEntryKind::Space : MatrixEntryKind::Room;
    e.name = r.alias.empty() ? r.room_id : r.alias;
    e.title = first_line(room_title(r), 200);
    e.topic = first_line(r.topic, 300);
    e.ref = room_ref(r, via);
    e.members = r.members;
    if (!e.ref.empty() && sane_id(e.name)) out.push_back(std::move(e));
    note_server(r.alias);
  };
  for (const auto& obj : json_object_array_after_key(json, "chunk")) add_room(obj);
  for (const auto& obj : json_object_array_after_key(json, "rooms")) add_room(obj);
  // A bare room summary.
  if (json.find("\"chunk\"") == std::string_view::npos &&
      json.find("\"rooms\"") == std::string_view::npos)
    add_room(json);

  // Timeline senders and member events.
  std::map<std::string, std::string> people;
  auto scan_events = [&](std::string_view key) {
    for (const auto& ev : json_object_array_after_key(json, key)) {
      const std::string top = json_top_level(ev);
      if (json_str_field(top, "event_id").empty()) continue;
      const std::string sender = json_str_field(top, "sender");
      if (!sender.empty()) people.emplace(sender, std::string());
      if (json_str_field(top, "type") == "m.room.member") {
        const std::string user = json_str_field(top, "state_key");
        const std::string name = json_str_field(
            json_top_level(json_object_after_key(ev, "content")), "displayname");
        if (!user.empty()) people[user] = first_line(name, 80);
      }
    }
  };
  scan_events("chunk");
  scan_events("state");
  scan_events("events_before");
  scan_events("events_after");
  for (const auto& [user, name] : people) {
    const std::string ref = matrix_ref_for_id(user);
    if (ref.empty() || user[0] != '@') continue;
    out.push_back({MatrixEntryKind::Person, user, name.empty() ? user : name, {}, ref, 0});
    note_server(user);
  }
  if (const std::string s = matrix_server_name(via); !s.empty()) servers.insert(s);
  for (const auto& s : servers) {
    MatrixRef dir;
    dir.kind = MatrixRefKind::Directory;
    dir.id = s;
    out.push_back({MatrixEntryKind::Server, s, s, {}, format_matrix_ref(dir), 0});
  }
  return out;
}

// ---------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------

#if defined(XANATOO_MATRIX) && XANATOO_MATRIX

namespace {

std::string ua() { return user_agent("read-only Matrix client"); }

// https://host[:port][/path] without query, credentials or a trailing '/'.
std::string valid_base_url(std::string_view url) {
  if (!starts_with_ci(url, "https://")) return {};
  std::string_view rest = url.substr(8);
  while (!rest.empty() && rest.back() == '/') rest.remove_suffix(1);
  const size_t slash = rest.find('/');
  const std::string host = matrix_server_name(rest.substr(0, slash));
  if (host.empty()) return {};
  std::string out = "https://" + host;
  if (slash != std::string_view::npos) {
    for (char c : rest.substr(slash))
      if (!unreserved(c) && c != '/') return {};
    out += rest.substr(slash);
  }
  return out;
}

std::mutex g_base_mutex;
std::map<std::string, std::string> g_bases;

// Client-server API base of a server name (.well-known/matrix/client).
std::string resolve_base(const std::string& server) {
  {
    std::lock_guard lock(g_base_mutex);
    if (auto it = g_bases.find(server); it != g_bases.end()) return it->second;
  }
  std::string base = "https://" + server;
  const std::string url = base + "/.well-known/matrix/client";
  std::cerr << "Matrix: GET " << url << "\n";
  std::string err;
  if (auto r = https_get_follow(url, ua(), &err); r && r->status == 200) {
    const std::string_view hs = json_object_after_key(r->body, "m.homeserver");
    if (std::string b = valid_base_url(json_str_field(hs, "base_url")); !b.empty())
      base = b;
  }
  std::lock_guard lock(g_base_mutex);
  if (g_bases.size() > 256) g_bases.clear();
  g_bases[server] = base;
  return base;
}

struct Auth {
  std::string base;
  std::string server;  // for messages
  std::string token;
};

std::optional<Auth> configured_auth() {
  const std::string token = effective_catalog_credential(CatalogKey::MatrixAccessToken);
  const std::string hs = effective_catalog_credential(CatalogKey::MatrixHomeserver);
  if (token.empty() || hs.empty()) return std::nullopt;
  Auth a;
  a.token = token;
  if (starts_with_ci(hs, "https://")) {
    a.base = valid_base_url(hs);
    a.server = a.base.substr(8, a.base.find('/', 8) - 8);
  } else {
    a.server = matrix_server_name(hs);
    if (!a.server.empty()) a.base = resolve_base(a.server);
  }
  if (a.base.empty()) return std::nullopt;
  return a;
}

std::string default_server() {
  const std::string hs = effective_catalog_credential(CatalogKey::MatrixHomeserver);
  if (starts_with_ci(hs, "https://")) {
    const std::string b = valid_base_url(hs);
    if (!b.empty()) return b.substr(8, b.find('/', 8) - 8);
  }
  if (std::string s = matrix_server_name(hs); !s.empty()) return s;
  return "matrix.org";
}

struct Resp {
  int status = 0;
  std::string body;
  std::string errcode, error;
};

// The token goes only to the server it belongs to.
std::optional<Resp> request(const std::string& base, const std::string& path,
                            const std::optional<Auth>& auth, std::string* err,
                            std::string_view post_body = {}) {
  const std::string url = base + "/_matrix/client" + path;
  std::cerr << "Matrix: " << (post_body.empty() ? "GET " : "POST ") << url << "\n";
  std::string headers;
  if (auth && auth->base == base) headers = "Authorization: Bearer " + auth->token + "\r\n";
  if (!post_body.empty()) headers += "Content-Type: application/json\r\n";
  std::string req_err;
  auto r = https_request(post_body.empty() ? "GET" : "POST", url, ua(), headers,
                         post_body, &req_err);
  if (!r) {
    if (err) *err = req_err.empty() ? "request failed" : req_err;
    return std::nullopt;
  }
  Resp out;
  out.status = r->status;
  out.body = std::move(r->body);
  if (out.status != 200) {
    const std::string top = json_top_level(out.body);
    out.errcode = json_str_field(top, "errcode");
    out.error = json_str_field(top, "error");
  }
  return out;
}

std::string describe(const Resp& r) {
  std::string s = http_status_line(r.status);
  if (!r.errcode.empty()) s += " " + r.errcode;
  if (!r.error.empty()) s += ": " + r.error;
  return s;
}

std::string via_query(const std::vector<std::string>& via, char first) {
  std::string q;
  for (const auto& v : via) {
    q.push_back(q.empty() ? first : '&');
    q += "via=" + matrix_escape(v);
  }
  return q;
}

constexpr const char* kTokenHint =
    "Room messages and space contents need an access token: add your "
    "homeserver and an access token under Preferences → Credentials… → "
    "Matrix (or XANATOO_MATRIX_HOMESERVER / XANATOO_MATRIX_ACCESS_TOKEN). "
    "xanatoo only reads; it never joins rooms or sends messages.";

std::optional<Document> fetch_directory(const MatrixRef& ref,
                                        const std::optional<Auth>& auth,
                                        std::string* err) {
  const std::string server = ref.id.empty() ? default_server() : ref.id;
  std::string note;
  std::optional<Resp> r;
  const bool own = !auth || server == auth->server;
  if (auth && (!own || !ref.query.empty())) {
    // Other servers' directories and searches go through the token's server.
    std::string body = "{\"limit\":50";
    auto json_str = [](std::string_view s) {
      std::string o = "\"";
      for (char c : s) {
        if (c == '"' || c == '\\') o.push_back('\\');
        if (static_cast<unsigned char>(c) >= 32) o.push_back(c);
      }
      return o + "\"";
    };
    if (!ref.since.empty()) body += ",\"since\":" + json_str(ref.since);
    if (!ref.query.empty())
      body += ",\"filter\":{\"generic_search_term\":" + json_str(ref.query) + "}";
    body += "}";
    r = request(auth->base, "/v3/publicRooms" +
                    (own ? std::string() : "?server=" + matrix_escape(server)),
                auth, err, body);
  } else {
    if (!ref.query.empty())
      note = "Searching a room directory needs an access token; showing the "
             "unfiltered directory.";
    std::string path = "/v3/publicRooms?limit=50";
    if (!ref.since.empty()) path += "&since=" + matrix_escape(ref.since);
    r = request(resolve_base(server), path, auth, err);
  }
  if (!r) return std::nullopt;
  if (r->status != 200) {
    if (err)
      *err = describe(*r) + (r->status == 401 || r->errcode == "M_MISSING_TOKEN"
                                 ? " — this server's directory is not public"
                                 : "");
    return std::nullopt;
  }
  matrix_directory_observe(r->body, server);
  return matrix_directory_document(server, r->body, ref, note);
}

std::optional<Document> fetch_room(const MatrixRef& ref, const std::optional<Auth>& auth,
                                   std::string* err) {
  MatrixRoomData data;
  data.via = ref.via;
  std::string server = !ref.via.empty() ? ref.via.front() : matrix_id_server(ref.id);
  if (server.empty()) server = default_server();
  const std::string base = auth ? auth->base : resolve_base(server);

  data.room_id = ref.kind == MatrixRefKind::RoomId ? ref.id : std::string();
  if (ref.kind == MatrixRefKind::Alias) {
    auto r = request(base, "/v3/directory/room/" + matrix_escape(ref.id), auth, err);
    if (!r) return std::nullopt;
    if (r->status != 200) {
      if (err) *err = "Room " + ref.id + ": " + describe(*r);
      return std::nullopt;
    }
    data.room_id = json_str_field(r->body, "room_id");
    if (data.via.empty()) {
      // The alias's server and the first resident servers, so room-id
      // links keep working.
      if (std::string own = matrix_id_server(ref.id); !own.empty())
        data.via.push_back(own);
      const std::string_view body = r->body;
      const size_t s = body.find("\"servers\"");
      if (s != std::string_view::npos) {
        size_t i = body.find('[', s);
        while (i != std::string_view::npos && data.via.size() < 3) {
          const size_t a = body.find('"', i + 1);
          const size_t close = body.find(']', i + 1);
          if (a == std::string_view::npos || a > close) break;
          const size_t b = body.find('"', a + 1);
          if (b == std::string_view::npos) break;
          std::string v = matrix_server_name(body.substr(a + 1, b - a - 1));
          if (!v.empty() &&
              std::find(data.via.begin(), data.via.end(), v) == data.via.end())
            data.via.push_back(v);
          i = b;
        }
      }
    }
    if (!sane_id(data.room_id)) {
      if (err) *err = "Room " + ref.id + " has no room id";
      return std::nullopt;
    }
  }
  const std::string room = matrix_escape(data.room_id);
  const std::string via = via_query(data.via, '?');

  if (auto r = request(base, "/v1/room_summary/" + room + via, auth, err); r) {
    if (r->status == 200) {
      data.summary_json = r->body;
    } else if (r->status == 404 && r->errcode == "M_UNRECOGNIZED") {
      auto old = request(base, "/unstable/im.nheko.summary/rooms/" + room +
                                   "/summary" + via, auth, err);
      if (old && old->status == 200) data.summary_json = old->body;
    }
  }
  const bool space = json_str_field(json_top_level(data.summary_json), "room_type") == "m.space";
  if (!auth) {
    data.note = kTokenHint;
  } else {
    if (space) {
      auto h = request(base, "/v1/rooms/" + room + "/hierarchy?max_depth=1&limit=50" +
                                 via_query(data.via, '&'), auth, err);
      if (h && h->status == 200) {
        data.hierarchy_json = h->body;
        matrix_directory_observe(h->body, server);
      }
    }
    std::optional<Resp> m;
    if (!ref.event.empty()) {
      m = request(base, "/v3/rooms/" + room + "/context/" + matrix_escape(ref.event) +
                            "?limit=30&filter=" +
                            matrix_escape("{\"lazy_load_members\":true}"), auth, err);
    } else {
      std::string path = "/v3/rooms/" + room + "/messages?dir=b&limit=50&filter=" +
                         matrix_escape("{\"lazy_load_members\":true}");
      if (!ref.from.empty()) path += "&from=" + matrix_escape(ref.from);
      m = request(base, path, auth, err);
    }
    if (m && m->status == 200) {
      (ref.event.empty() ? data.messages_json : data.context_json) = m->body;
      matrix_directory_observe(m->body, server);
    } else if (m) {
      data.note = m->status == 403
          ? "The history of this room is only visible to its members (" +
                describe(*m) + "). xanatoo does not join rooms."
          : "Messages could not be loaded: " + describe(*m);
    }
  }
  if (data.summary_json.empty() && data.messages_json.empty() &&
      data.context_json.empty()) {
    if (err) *err = "Room " + ref.id + " is not public or not reachable from " + server;
    return std::nullopt;
  }
  if (!data.summary_json.empty()) matrix_directory_observe(data.summary_json, server);
  return matrix_room_document(ref, data);
}

std::optional<Document> fetch_user(const MatrixRef& ref, const std::optional<Auth>& auth,
                                   std::string* err) {
  const std::string server = matrix_id_server(ref.id);
  const std::string base = auth ? auth->base : resolve_base(server);
  auto r = request(base, "/v3/profile/" + matrix_escape(ref.id), auth, err);
  if (!r) return std::nullopt;
  if (r->status != 200) {
    if (err) *err = ref.id + ": " + describe(*r);
    return std::nullopt;
  }
  const std::string name =
      first_line(json_str_field(json_top_level(r->body), "displayname"), 80);
  matrix_directory_merge({{MatrixEntryKind::Person, ref.id, name.empty() ? ref.id : name,
                           {}, format_matrix_ref(ref), 0}});
  return matrix_profile_document(ref.id, r->body);
}

}  // namespace

std::optional<Document> matrix_fetch(std::string_view url, std::string* err) {
  auto ref = parse_matrix_ref(url);
  if (!ref) {
    if (err) *err = "not a matrix: address (matrix:r/room:server, matrix:u/user:server)";
    return std::nullopt;
  }
  const auto auth = configured_auth();
  switch (ref->kind) {
    case MatrixRefKind::Directory: return fetch_directory(*ref, auth, err);
    case MatrixRefKind::User: return fetch_user(*ref, auth, err);
    case MatrixRefKind::Alias:
    case MatrixRefKind::RoomId: return fetch_room(*ref, auth, err);
  }
  return std::nullopt;
}

#endif  // XANATOO_MATRIX

}  // namespace xanatoo
