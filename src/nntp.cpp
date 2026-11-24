// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
//
// Usenet / NNTP: refs (RFC 5538 URIs), wire formats (LIST ACTIVE, OVER,
// articles with MIME), and the documents built from them. Pure; the
// network side is nntp_fetch.cpp.

#include "xanatoo/nntp.hpp"

#include "forum_common.hpp"
#include "xanatoo/util.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>

namespace xanatoo {

using namespace forum;

namespace {

bool all_digits(std::string_view s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) {
    return std::isdigit(static_cast<unsigned char>(c));
  });
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string percent_decode(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && hex_value(s[i + 1]) >= 0 &&
        hex_value(s[i + 2]) >= 0) {
      out.push_back(static_cast<char>(hex_value(s[i + 1]) * 16 + hex_value(s[i + 2])));
      i += 2;
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

// Path segment: keeps what RFC 5538 allows unescaped in group names and
// Message-IDs, except '/', '?', '#', '%' and brackets ("//" would start a
// locator selector).
std::string percent_encode(std::string_view s) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  for (char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (std::isalnum(c) || std::string_view("-._~!$&'()*+,;=:@").find(ch) !=
                               std::string_view::npos) {
      out.push_back(ch);
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 15]);
    }
  }
  return out;
}

std::string strip_brackets(std::string_view s) {
  trim_ascii(s);
  if (s.size() >= 2 && s.front() == '<' && s.back() == '>')
    s = s.substr(1, s.size() - 2);
  return std::string(s);
}

std::string default_server_for(std::string_view group) {
  if (starts_with(group, "gmane.") || starts_with(group, "gwene."))
    return kNntpGmaneServer;
  return kNntpDefaultServer;
}

int default_port(bool tls) { return tls ? 563 : 119; }

}  // namespace

// ---- refs ------------------------------------------------------------------

bool is_nntp_url(std::string_view s) {
  for (std::string_view p : {"news:", "nntp:", "nntps:", "snews:"})
    if (starts_with_ci(s, p) && s.size() > p.size()) return true;
  return false;
}

std::optional<NntpRef> parse_nntp_url(std::string_view s) {
  if (!is_nntp_url(s)) return std::nullopt;
  NntpRef r;
  const size_t colon = s.find(':');
  r.scheme = ascii_lower(s.substr(0, colon));
  r.tls = r.scheme == "nntps" || r.scheme == "snews";
  r.port = default_port(r.tls);
  std::string_view rest = s.substr(colon + 1);
  if (size_t h = rest.find('#'); h != std::string_view::npos) {
    r.fragment = std::string(rest.substr(h + 1));
    rest = rest.substr(0, h);
  }
  std::string_view query;
  if (size_t q = rest.find('?'); q != std::string_view::npos) {
    query = rest.substr(q + 1);
    rest = rest.substr(0, q);
  }
  std::string_view path = rest;
  if (starts_with(rest, "//")) {
    rest.remove_prefix(2);
    const size_t slash = rest.find('/');
    std::string_view auth = rest.substr(0, slash);
    path = slash == std::string_view::npos ? std::string_view() : rest.substr(slash + 1);
    if (size_t at = auth.rfind('@'); at != std::string_view::npos)
      auth = auth.substr(at + 1);  // user@ is ignored (no AUTHINFO)
    if (size_t c = auth.rfind(':'); c != std::string_view::npos &&
                                    all_digits(auth.substr(c + 1))) {
      r.port = std::atoi(std::string(auth.substr(c + 1)).c_str());
      auth = auth.substr(0, c);
      if (r.port <= 0 || r.port > 65535) return std::nullopt;
    }
    r.host = ascii_lower(auth);
    if (r.host.empty()) return std::nullopt;
  } else if (r.scheme == "nntp" || r.scheme == "nntps") {
    return std::nullopt;  // nntp: always names its server
  }
  while (!path.empty() && path.back() == '/') path.remove_suffix(1);
  const std::string decoded = percent_decode(path);
  if (decoded.empty()) {
    r.view = NntpView::Groups;
  } else if (decoded.find('@') != std::string::npos) {
    r.view = NntpView::MessageId;
    r.message_id = strip_brackets(decoded);
  } else if (decoded.find_first_of("*?[") != std::string::npos) {
    r.view = NntpView::Groups;
    r.wildmat = decoded;
  } else if (size_t sl = decoded.find('/'); sl != std::string::npos) {
    if (!all_digits(std::string_view(decoded).substr(sl + 1))) return std::nullopt;
    r.view = NntpView::Article;
    r.group = decoded.substr(0, sl);
    r.article = std::atoll(decoded.c_str() + sl + 1);
    if (r.group.empty() || r.article <= 0) return std::nullopt;
  } else {
    r.view = NntpView::Group;
    r.group = decoded;
  }
  if (r.group.find_first_of(" \t\r\n") != std::string::npos ||
      r.message_id.find_first_of(" \t\r\n<>") != std::string::npos)
    return std::nullopt;
  if (r.host.empty()) r.host = default_server_for(r.group);
  for (std::string_view kv : {query}) {
    while (!kv.empty()) {
      const size_t amp = kv.find('&');
      std::string_view item = kv.substr(0, amp);
      if (starts_with(item, "page=") && all_digits(item.substr(5)))
        r.page = std::max(1, std::atoi(std::string(item.substr(5)).c_str()));
      if (amp == std::string_view::npos) break;
      kv = kv.substr(amp + 1);
    }
  }
  return r;
}

std::string nntp_url_string(const NntpRef& r) {
  std::string scheme = r.tls ? "nntps" : (r.view == NntpView::Article ? "nntp" : "news");
  std::string out = scheme + "://" + r.host;
  if (r.port != default_port(r.tls)) out += ":" + std::to_string(r.port);
  out += "/";
  switch (r.view) {
    case NntpView::Groups: out += percent_encode(r.wildmat); break;
    case NntpView::Group: out += percent_encode(r.group); break;
    case NntpView::Article:
      out += percent_encode(r.group) + "/" + std::to_string(r.article);
      break;
    case NntpView::MessageId: out += percent_encode(r.message_id); break;
  }
  if (r.view == NntpView::Group && r.page > 1)
    out += "?page=" + std::to_string(r.page);
  if (!r.fragment.empty()) out += "#" + r.fragment;
  return out;
}

namespace {
NntpRef on_server(const NntpRef& on, NntpView v) {
  NntpRef r;
  r.view = v;
  r.host = on.host;
  r.port = on.port;
  r.tls = on.tls;
  r.scheme = on.scheme;
  return r;
}
}  // namespace

std::string nntp_group_url(const NntpRef& on, std::string_view group, int page) {
  NntpRef r = on_server(on, NntpView::Group);
  r.group = std::string(group);
  r.page = page;
  return nntp_url_string(r);
}

std::string nntp_article_url(const NntpRef& on, std::string_view group,
                             int64_t number) {
  NntpRef r = on_server(on, NntpView::Article);
  r.group = std::string(group);
  r.article = number;
  r.fragment = "id:" + std::to_string(number);
  return nntp_url_string(r);
}

std::string nntp_msgid_url(const NntpRef& on, std::string_view message_id) {
  NntpRef r = on_server(on, NntpView::MessageId);
  r.message_id = std::string(message_id);
  return nntp_url_string(r);
}

std::string nntp_groups_url(const NntpRef& on, std::string_view wildmat) {
  NntpRef r = on_server(on, NntpView::Groups);
  r.wildmat = std::string(wildmat);
  return nntp_url_string(r);
}

namespace {
// "comp.lang.c" → "comp.lang.*"; "comp.lang.*" → "comp.*"; "comp.*" → "".
std::string parent_hierarchy(std::string_view name) {
  if (ends_with(name, ".*")) name.remove_suffix(2);
  const size_t dot = name.rfind('.');
  if (dot == std::string_view::npos) return {};
  return std::string(name.substr(0, dot)) + ".*";
}
}  // namespace

std::string nntp_up_url(const NntpRef& r) {
  switch (r.view) {
    case NntpView::Groups:
      if (r.wildmat.empty()) return {};
      return nntp_groups_url(r, parent_hierarchy(r.wildmat));
    case NntpView::Group: return nntp_groups_url(r, parent_hierarchy(r.group));
    case NntpView::Article: return nntp_group_url(r, r.group);
    case NntpView::MessageId: return nntp_groups_url(r, "");
  }
  return {};
}

// ---- wildmat ---------------------------------------------------------------

namespace {
bool wild_one(std::string_view p, std::string_view t) {
  size_t pi = 0, ti = 0, star_p = std::string_view::npos, star_t = 0;
  auto class_match = [&](size_t& i, char c) {
    // p[i] == '[' : returns whether c matches, i moves past ']'.
    size_t j = i + 1;
    bool neg = j < p.size() && (p[j] == '^' || p[j] == '!');
    if (neg) ++j;
    bool hit = false, first = true;
    while (j < p.size() && (first || p[j] != ']')) {
      first = false;
      if (j + 2 < p.size() && p[j + 1] == '-' && p[j + 2] != ']') {
        if (c >= p[j] && c <= p[j + 2]) hit = true;
        j += 3;
      } else {
        if (c == p[j]) hit = true;
        ++j;
      }
    }
    i = j < p.size() ? j + 1 : j;
    return hit != neg;
  };
  while (ti < t.size()) {
    if (pi < p.size() && p[pi] == '*') {
      star_p = pi++;
      star_t = ti;
      continue;
    }
    if (pi < p.size()) {
      size_t next = pi;
      bool ok = false;
      if (p[pi] == '?') {
        ok = true;
        next = pi + 1;
      } else if (p[pi] == '[') {
        ok = class_match(next, t[ti]);
      } else {
        ok = p[pi] == t[ti];
        next = pi + 1;
      }
      if (ok) {
        pi = next;
        ++ti;
        continue;
      }
    }
    if (star_p == std::string_view::npos) return false;
    pi = star_p + 1;
    ti = ++star_t;
  }
  while (pi < p.size() && p[pi] == '*') ++pi;
  return pi == p.size();
}
}  // namespace

bool nntp_wildmat_match(std::string_view pattern, std::string_view text) {
  if (pattern.empty()) return true;
  bool matched = false;
  while (true) {
    const size_t comma = pattern.find(',');
    std::string_view one = pattern.substr(0, comma);
    const bool neg = !one.empty() && one[0] == '!';
    if (neg) one.remove_prefix(1);
    if (wild_one(one, text)) matched = !neg;  // the last match decides
    if (comma == std::string_view::npos) break;
    pattern = pattern.substr(comma + 1);
  }
  return matched;
}

// ---- LIST / OVER -----------------------------------------------------------

namespace {
template <typename Fn>
void for_lines(std::string_view text, Fn fn) {
  size_t i = 0;
  while (i < text.size()) {
    size_t e = text.find('\n', i);
    if (e == std::string_view::npos) e = text.size();
    std::string_view l = text.substr(i, e - i);
    if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
    fn(l);
    i = e + 1;
  }
}

std::vector<std::string_view> split_tabs(std::string_view l) {
  std::vector<std::string_view> f;
  size_t i = 0;
  while (true) {
    const size_t t = l.find('\t', i);
    f.push_back(l.substr(i, t == std::string_view::npos ? std::string_view::npos : t - i));
    if (t == std::string_view::npos) break;
    i = t + 1;
  }
  return f;
}
}  // namespace

std::vector<NntpGroupInfo> parse_nntp_active(std::string_view text) {
  std::vector<NntpGroupInfo> out;
  for_lines(text, [&](std::string_view l) {
    std::string_view f[4];
    int n = 0;
    size_t i = 0;
    while (n < 4 && i < l.size()) {
      while (i < l.size() && l[i] == ' ') ++i;
      size_t e = l.find(' ', i);
      if (e == std::string_view::npos) e = l.size();
      if (e > i) f[n++] = l.substr(i, e - i);
      i = e;
    }
    if (n < 3 || !all_digits(f[1]) || !all_digits(f[2])) return;
    NntpGroupInfo g;
    g.name = std::string(f[0]);
    g.high = std::atoll(std::string(f[1]).c_str());
    g.low = std::atoll(std::string(f[2]).c_str());
    if (n == 4 && !f[3].empty()) g.status = f[3][0];
    out.push_back(std::move(g));
  });
  return out;
}

void apply_nntp_descriptions(std::vector<NntpGroupInfo>& groups,
                             std::string_view text) {
  std::unordered_map<std::string, size_t> at;
  for (size_t i = 0; i < groups.size(); ++i) at[groups[i].name] = i;
  for_lines(text, [&](std::string_view l) {
    const size_t sep = l.find_first_of(" \t");
    if (sep == std::string_view::npos) return;
    auto it = at.find(std::string(l.substr(0, sep)));
    if (it == at.end()) return;
    std::string_view d = l.substr(sep);
    trim_ascii(d);
    if (d == "?" || d == "No description.") return;  // INN placeholders
    groups[it->second].description = decode_mime_header(d);
  });
}

std::vector<std::string> parse_message_ids(std::string_view s) {
  std::vector<std::string> out;
  size_t i = 0;
  while ((i = s.find('<', i)) != std::string_view::npos) {
    const size_t e = s.find('>', i + 1);
    if (e == std::string_view::npos) break;
    std::string id(s.substr(i + 1, e - i - 1));
    if (!id.empty() && id.find_first_of(" \t<") == std::string::npos)
      out.push_back(std::move(id));
    i = e + 1;
  }
  return out;
}

std::optional<NntpOverview> parse_nntp_overview_line(std::string_view line) {
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
  auto f = split_tabs(line);
  if (f.size() < 6 || !all_digits(f[0])) return std::nullopt;
  NntpOverview o;
  o.number = std::atoll(std::string(f[0]).c_str());
  o.subject = decode_mime_header(f[1]);
  o.from = decode_mime_header(f[2]);
  o.date = std::string(f[3]);
  o.message_id = strip_brackets(f[4]);
  o.references = parse_message_ids(f[5]);
  if (f.size() > 6) o.bytes = std::atoll(std::string(f[6]).c_str());
  if (f.size() > 7) o.lines = std::atoll(std::string(f[7]).c_str());
  return o;
}

std::vector<NntpOverview> parse_nntp_overview(std::string_view text) {
  std::vector<NntpOverview> out;
  for_lines(text, [&](std::string_view l) {
    if (auto o = parse_nntp_overview_line(l)) out.push_back(std::move(*o));
  });
  return out;
}

// ---- dates -----------------------------------------------------------------

namespace {
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}
}  // namespace

std::string nntp_date_iso(std::string_view date) {
  std::string_view d = date;
  if (size_t c = d.find(','); c != std::string_view::npos && c < 10)
    d.remove_prefix(c + 1);
  std::vector<std::string> tok;
  {
    size_t i = 0;
    while (i < d.size()) {
      while (i < d.size() && (d[i] == ' ' || d[i] == '\t')) ++i;
      size_t e = i;
      while (e < d.size() && d[e] != ' ' && d[e] != '\t') ++e;
      if (e > i) tok.emplace_back(d.substr(i, e - i));
      i = e;
    }
  }
  if (tok.size() < 4) return {};
  static const char* kMonths[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                  "jul", "aug", "sep", "oct", "nov", "dec"};
  int mon = 0;
  for (int k = 0; k < 12; ++k)
    if (starts_with(ascii_lower(tok[1]), kMonths[k])) mon = k + 1;
  if (!all_digits(tok[0]) || mon == 0 || !all_digits(tok[2])) return {};
  const int day = std::atoi(tok[0].c_str());
  int year = std::atoi(tok[2].c_str());
  if (tok[2].size() <= 2) year += year < 50 ? 2000 : 1900;
  int hh = 0, mm = 0, ss = 0;
  if (std::sscanf(tok[3].c_str(), "%d:%d:%d", &hh, &mm, &ss) < 2) return {};
  int offset_min = 0;
  if (tok.size() > 4) {
    const std::string& z = tok[4];
    if ((z[0] == '+' || z[0] == '-') && z.size() == 5 &&
        all_digits(std::string_view(z).substr(1))) {
      const int v = std::atoi(z.c_str() + 1);
      offset_min = (v / 100) * 60 + v % 100;
      if (z[0] == '-') offset_min = -offset_min;
    } else {
      static const std::pair<const char*, int> kZones[] = {
          {"EST", -5}, {"EDT", -4}, {"CST", -6}, {"CDT", -5},
          {"MST", -7}, {"MDT", -6}, {"PST", -8}, {"PDT", -7}};
      for (const auto& [name, h] : kZones)
        if (z == name) offset_min = h * 60;
    }
  }
  if (day < 1 || day > 31 || hh > 23 || mm > 59 || ss > 60) return {};
  int64_t t = days_from_civil(year, static_cast<unsigned>(mon),
                              static_cast<unsigned>(day)) * 86400 +
              hh * 3600 + mm * 60 + std::min(ss, 59) - offset_min * 60;
  return iso_from_unix(t);
}

// ---- MIME ------------------------------------------------------------------

namespace {
std::string base64_decode(std::string_view s) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
  };
  std::string out;
  uint32_t acc = 0;
  int bits = 0;
  for (char c : s) {
    const int v = val(c);
    if (v < 0) continue;  // '=', line breaks
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((acc >> bits) & 0xFF));
    }
  }
  return out;
}

std::string qp_decode(std::string_view s, bool header) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '=' && i + 2 < s.size() && hex_value(s[i + 1]) >= 0 &&
        hex_value(s[i + 2]) >= 0) {
      out.push_back(static_cast<char>(hex_value(s[i + 1]) * 16 + hex_value(s[i + 2])));
      i += 2;
    } else if (c == '=' && !header) {
      // Soft line break: "=\r\n" / "=\n" (trailing blanks allowed).
      size_t j = i + 1;
      while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
      if (j < s.size() && s[j] == '\r') ++j;
      if (j < s.size() && s[j] == '\n') i = j;
      else if (j >= s.size()) i = j;
      else out.push_back(c);
    } else if (c == '_' && header) {
      out.push_back(' ');
    } else {
      out.push_back(c);
    }
  }
  return out;
}

// to_utf8 converts the western code pages only; anything else that is not
// UTF-8 is read as Latin-1, so no invalid UTF-8 reaches the document.
std::string charset_to_utf8(std::string bytes, std::string_view charset) {
  std::string out = to_utf8(std::move(bytes), ascii_lower(charset));
  if (is_valid_utf8(out)) return out;
  return to_utf8(std::move(out), "iso-8859-1");
}

// Parameter `name` of a Content-Type / Content-Disposition value.
std::string mime_param(std::string_view value, std::string_view name) {
  const std::string lower = ascii_lower(value);
  size_t at = 0;
  while ((at = lower.find(name, at)) != std::string::npos) {
    const bool start_ok = at == 0 || lower[at - 1] == ';' || lower[at - 1] == ' ' ||
                          lower[at - 1] == '\t';
    size_t eq = at + name.size();
    while (eq < lower.size() && lower[eq] == ' ') ++eq;
    if (!start_ok || eq >= lower.size() || lower[eq] != '=') {
      at += name.size();
      continue;
    }
    size_t v = eq + 1;
    while (v < value.size() && value[v] == ' ') ++v;
    if (v < value.size() && value[v] == '"') {
      const size_t e = value.find('"', v + 1);
      return std::string(value.substr(v + 1, e == std::string_view::npos ? std::string_view::npos : e - v - 1));
    }
    size_t e = v;
    while (e < value.size() && value[e] != ';' && value[e] != ' ' && value[e] != '\t') ++e;
    return std::string(value.substr(v, e - v));
  }
  return {};
}

std::string mime_type(std::string_view value) {
  std::string_view t = value.substr(0, value.find(';'));
  trim_ascii(t);
  return ascii_lower(t);
}

using Headers = std::vector<std::pair<std::string, std::string>>;

std::string find_header(const Headers& h, std::string_view name) {
  for (const auto& [k, v] : h)
    if (ascii_lower(k) == ascii_lower(name)) return v;
  return {};
}

// Splits "headers\n\nbody" (LF only); unfolds continuation lines.
std::string_view split_headers(std::string_view raw, Headers& out) {
  size_t i = 0;
  while (i < raw.size()) {
    size_t e = raw.find('\n', i);
    if (e == std::string_view::npos) e = raw.size();
    std::string_view l = raw.substr(i, e - i);
    i = e + 1;
    if (l.empty()) return i <= raw.size() ? raw.substr(i) : std::string_view();
    if ((l[0] == ' ' || l[0] == '\t') && !out.empty()) {
      trim_ascii(l);
      out.back().second += " ";
      out.back().second += l;
      continue;
    }
    const size_t colon = l.find(':');
    if (colon == std::string_view::npos) continue;
    std::string_view v = l.substr(colon + 1);
    trim_ascii(v);
    out.emplace_back(std::string(l.substr(0, colon)), std::string(v));
  }
  return {};
}

std::string decode_transfer(std::string_view body, std::string_view cte) {
  const std::string enc = ascii_lower(cte);
  if (enc == "base64") return base64_decode(body);
  if (enc == "quoted-printable") return qp_decode(body, false);
  return std::string(body);
}

struct TextPart {
  bool found = false;
  bool html = false;
  bool flowed = false;
  std::string text;
};

// The first text/plain part (text/html when there is none) of one MIME
// entity; other parts are listed in `attachments`.
void walk_mime(const Headers& h, std::string_view body, TextPart& plain,
               TextPart& html, std::vector<std::string>& attachments,
               int depth) {
  const std::string ct_raw = find_header(h, "Content-Type");
  const std::string type = ct_raw.empty() ? "text/plain" : mime_type(ct_raw);
  const std::string disp = find_header(h, "Content-Disposition");
  const bool attached = starts_with_ci(disp, "attachment");
  if (starts_with(type, "multipart/") && depth < 8) {
    const std::string boundary = mime_param(ct_raw, "boundary");
    if (boundary.empty()) return;
    const std::string delim = "--" + boundary;
    size_t at = body.find(delim);
    while (at != std::string_view::npos) {
      size_t start = body.find('\n', at);
      if (start == std::string_view::npos) break;
      if (body.substr(at + delim.size(), 2) == "--") break;  // closing delimiter
      ++start;
      size_t next = body.find("\n" + delim, start);
      std::string_view part = body.substr(
          start, next == std::string_view::npos ? std::string_view::npos : next - start);
      Headers ph;
      std::string_view pbody = split_headers(part, ph);
      walk_mime(ph, pbody, plain, html, attachments, depth + 1);
      if (next == std::string_view::npos) break;
      at = next + 1;
    }
    return;
  }
  const std::string charset = mime_param(ct_raw, "charset");
  if (!attached && type == "text/plain" && !plain.found) {
    plain.found = true;
    plain.flowed = ascii_lower(mime_param(ct_raw, "format")) == "flowed";
    plain.text = charset_to_utf8(
        decode_transfer(body, find_header(h, "Content-Transfer-Encoding")), charset);
    return;
  }
  if (!attached && type == "text/html" && !html.found) {
    html.found = true;
    html.html = true;
    html.text = charset_to_utf8(
        decode_transfer(body, find_header(h, "Content-Transfer-Encoding")), charset);
    return;
  }
  if (type == "multipart/alternative" || type.empty()) return;
  std::string name = mime_param(disp, "filename");
  if (name.empty()) name = mime_param(ct_raw, "name");
  name = decode_mime_header(name);
  attachments.push_back(name.empty() ? type : name + " (" + type + ")");
}
}  // namespace

std::string decode_mime_header(std::string_view s) {
  std::string out;
  size_t i = 0;
  bool last_encoded = false;
  std::string gap;  // white space after an encoded word, dropped before another
  while (i < s.size()) {
    const size_t start = s.find("=?", i);
    if (start == std::string_view::npos) {
      out += gap;
      out += to_utf8(std::string(s.substr(i)), "");
      gap.clear();
      break;
    }
    const size_t q1 = s.find('?', start + 2);
    const size_t q2 = q1 == std::string_view::npos ? q1 : s.find('?', q1 + 1);
    const size_t end = q2 == std::string_view::npos ? q2 : s.find("?=", q2 + 1);
    if (end == std::string_view::npos || q2 != q1 + 2) {
      out += gap;
      gap.clear();
      out += to_utf8(std::string(s.substr(i, start + 2 - i)), "");
      i = start + 2;
      last_encoded = false;
      continue;
    }
    std::string_view between = s.substr(i, start - i);
    const bool blank = between.find_first_not_of(" \t\r\n") == std::string_view::npos;
    if (!(last_encoded && blank)) out += to_utf8(std::string(between), "");
    std::string charset = std::string(s.substr(start + 2, q1 - start - 2));
    if (size_t star = charset.find('*'); star != std::string::npos)
      charset.resize(star);  // RFC 2231 language suffix
    const char enc = static_cast<char>(std::toupper(static_cast<unsigned char>(s[q1 + 1])));
    std::string_view text = s.substr(q2 + 1, end - q2 - 1);
    std::string bytes = enc == 'B' ? base64_decode(text) : qp_decode(text, true);
    out += charset_to_utf8(std::move(bytes), charset);
    i = end + 2;
    last_encoded = true;
    gap.clear();
  }
  return out;
}

std::string mail_display_name(std::string_view from) {
  std::string_view f = from;
  trim_ascii(f);
  if (size_t lt = f.find('<'); lt != std::string_view::npos && lt > 0) {
    std::string_view name = f.substr(0, lt);
    trim_ascii(name);
    if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
      name = name.substr(1, name.size() - 2);
    if (!name.empty()) return std::string(name);
  }
  if (size_t lp = f.find('('); lp != std::string_view::npos) {
    const size_t rp = f.rfind(')');
    if (rp != std::string_view::npos && rp > lp + 1)
      return std::string(f.substr(lp + 1, rp - lp - 1));
  }
  if (size_t lt = f.find('<'); lt != std::string_view::npos) {
    const size_t gt = f.find('>', lt);
    return std::string(f.substr(lt + 1, gt == std::string_view::npos ? std::string_view::npos : gt - lt - 1));
  }
  return std::string(f);
}

std::string strip_reply_prefix(std::string_view subject) {
  std::string_view s = subject;
  trim_ascii(s);
  for (bool again = true; again;) {
    again = false;
    for (std::string_view p : {"re:", "aw:", "sv:", "antw:", "fwd:", "fw:"}) {
      if (starts_with_ci(s, p)) {
        s.remove_prefix(p.size());
        trim_ascii(s);
        again = true;
      }
    }
    // "Re[2]:" / "Re^2:"
    if (starts_with_ci(s, "re[") || starts_with_ci(s, "re^")) {
      size_t c = s.find(':');
      if (c != std::string_view::npos && c < 8) {
        s.remove_prefix(c + 1);
        trim_ascii(s);
        again = true;
      }
    }
  }
  return std::string(s);
}

std::string NntpArticle::header(std::string_view name) const {
  return find_header(headers, name);
}

NntpArticle parse_nntp_article(std::string_view raw_in) {
  std::string raw;
  raw.reserve(raw_in.size());
  for (char c : raw_in)
    if (c != '\r') raw.push_back(c);
  NntpArticle a;
  std::string_view body = split_headers(raw, a.headers);
  a.subject = decode_mime_header(a.header("Subject"));
  a.from = decode_mime_header(a.header("From"));
  a.date = a.header("Date");
  a.message_id = strip_brackets(a.header("Message-ID"));
  a.references = parse_message_ids(a.header("References"));
  if (a.references.empty()) {
    // In-Reply-To (mailing-list gateways) as a single parent.
    auto irt = parse_message_ids(a.header("In-Reply-To"));
    if (!irt.empty()) a.references.push_back(irt.front());
  }
  {
    std::string ng = a.header("Newsgroups");
    size_t i = 0;
    while (i < ng.size()) {
      size_t e = ng.find(',', i);
      if (e == std::string::npos) e = ng.size();
      std::string_view g = std::string_view(ng).substr(i, e - i);
      trim_ascii(g);
      if (!g.empty()) a.newsgroups.emplace_back(g);
      i = e + 1;
    }
  }
  a.xref = a.header("Xref");
  TextPart plain, html;
  if (find_header(a.headers, "Content-Type").empty() &&
      find_header(a.headers, "MIME-Version").empty()) {
    plain.found = true;
    plain.text = to_utf8(std::string(body), "");
  } else {
    walk_mime(a.headers, body, plain, html, a.attachments, 0);
  }
  if (plain.found) {
    a.body = std::move(plain.text);
    a.flowed = plain.flowed;
  } else if (html.found) {
    a.body = std::move(html.text);
    a.html = true;
  }
  return a;
}

int64_t nntp_xref_number(std::string_view xref, std::string_view group) {
  size_t i = 0;
  while (i < xref.size()) {
    while (i < xref.size() && xref[i] == ' ') ++i;
    size_t e = xref.find(' ', i);
    if (e == std::string_view::npos) e = xref.size();
    std::string_view tok = xref.substr(i, e - i);
    const size_t colon = tok.rfind(':');
    if (colon != std::string_view::npos && tok.substr(0, colon) == group &&
        all_digits(tok.substr(colon + 1)))
      return std::atoll(std::string(tok.substr(colon + 1)).c_str());
    i = e;
  }
  return 0;
}

// ---- body ------------------------------------------------------------------

namespace {
struct BodyLine {
  int depth = 0;       // quote depth
  std::string text;
};

// Quote depth of a line ("> > x", ">> x") and the text after the markers.
BodyLine quoted_line(std::string_view l) {
  BodyLine b;
  size_t i = 0;
  while (i < l.size()) {
    if (l[i] == '>') {
      ++b.depth;
      ++i;
    } else if (l[i] == ' ' && i + 1 < l.size() && l[i + 1] == '>' && b.depth > 0) {
      ++i;
    } else {
      break;
    }
  }
  if (b.depth > 0 && i < l.size() && l[i] == ' ') ++i;
  b.text = std::string(l.substr(i));
  return b;
}

bool looks_like_code(const std::vector<BodyLine>& para) {
  size_t indented = 0;
  for (const BodyLine& l : para)
    if (starts_with(l.text, "\t") || starts_with(l.text, "    ") ||
        starts_with(l.text, "diff ") || starts_with(l.text, "@@ ") ||
        starts_with(l.text, "+++ ") || starts_with(l.text, "--- a/"))
      ++indented;
  return para.size() > 1 && indented * 2 >= para.size();
}

std::string join_lines(const std::vector<BodyLine>& para, size_t from = 0) {
  std::string s;
  for (size_t i = from; i < para.size(); ++i) {
    if (i > from) s += '\n';
    s += para[i].text;
  }
  return s;
}

void add_code(Document& doc, NodeId parent, const std::string& text) {
  NodeId code = doc.create(NodeKind::CodeBlock, parent);
  doc.append_text(code, text);
}
}  // namespace

void append_usenet_body(Document& doc, NodeId parent, std::string_view body,
                        bool flowed) {
  std::vector<std::string_view> raw;
  for_lines(body, [&](std::string_view l) { raw.push_back(l); });
  while (!raw.empty() && raw.back().find_first_not_of(" \t") == std::string_view::npos)
    raw.pop_back();
  // Signature: after the last "-- " line (not too far from the end).
  size_t sig = raw.size();
  for (size_t i = raw.size(); i-- > 0;) {
    if (raw[i] == "-- " || raw[i] == "--") {
      if (raw.size() - i <= 16) sig = i;
      break;
    }
  }
  // Logical lines (flowed: soft breaks joined, RFC 3676).
  std::vector<BodyLine> lines;
  bool joining = false;
  for (size_t i = 0; i < sig; ++i) {
    BodyLine b = quoted_line(raw[i]);
    if (flowed) {
      if (b.depth == 0 && starts_with(b.text, " ")) b.text.erase(0, 1);  // space-stuffed
      const bool soft = !b.text.empty() && b.text.back() == ' ' && b.text != "-- ";
      if (joining && !lines.empty() && lines.back().depth == b.depth) {
        lines.back().text += b.text;
      } else {
        lines.push_back(b);
      }
      joining = soft;  // the trailing space stays between the joined words
      continue;
    }
    lines.push_back(std::move(b));
  }
  // A git patch ("---" then a diffstat / "diff --git"): one code block.
  size_t patch = lines.size();
  for (size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].depth == 0 && (lines[i].text == "---" || starts_with(lines[i].text, "diff --git "))) {
      bool has_diff = false;
      for (size_t j = i; j < lines.size() && !has_diff; ++j)
        has_diff = lines[j].depth == 0 && starts_with(lines[j].text, "diff --git ");
      if (has_diff) {
        patch = i;
        break;
      }
    }
  }
  // Blocks: runs of equal "quoted or not" separated by blank lines.
  std::vector<BodyLine> para;
  auto flush = [&] {
    if (para.empty()) return;
    if (para.front().depth > 0) {
      std::string text;
      for (size_t i = 0; i < para.size(); ++i) {
        if (i) text += '\n';
        for (int d = 1; d < para[i].depth; ++d) text += "> ";
        text += para[i].text;
      }
      NodeId q = doc.create(NodeKind::BlockQuote, parent);
      doc.append_text(q, text);
    } else if (looks_like_code(para)) {
      add_code(doc, parent, join_lines(para));
    } else {
      NodeId p = doc.create(NodeKind::Paragraph, parent);
      doc.append_text(p, join_lines(para));
    }
    para.clear();
  };
  for (size_t i = 0; i < patch; ++i) {
    const BodyLine& l = lines[i];
    const bool blank = l.text.find_first_not_of(" \t") == std::string::npos;
    if (blank) {
      // "> " alone keeps a quoted block together only up to the blank.
      flush();
      continue;
    }
    if (!para.empty() && (para.front().depth > 0) != (l.depth > 0)) flush();
    para.push_back(l);
  }
  flush();
  if (patch < lines.size()) {
    std::vector<BodyLine> rest(lines.begin() + static_cast<std::ptrdiff_t>(patch), lines.end());
    std::string text;
    for (size_t i = 0; i < rest.size(); ++i) {
      if (i) text += '\n';
      for (int d = 0; d < rest[i].depth; ++d) text += "> ";
      text += rest[i].text;
    }
    add_code(doc, parent, text);
  }
  if (sig < raw.size()) {
    std::string text;
    for (size_t i = sig + 1; i < raw.size(); ++i) {
      if (!text.empty()) text += '\n';
      text += std::string(raw[i]);
    }
    if (text.find_first_not_of(" \t\n") != std::string::npos) {
      NodeId p = doc.create(NodeKind::Paragraph, parent);
      add_meta(doc, p, "-- \n" + text);
    }
  }
}

// ---- documents -------------------------------------------------------------

namespace {
const char* kSite = "Usenet";

std::string server_label(const NntpRef& r) {
  std::string s = r.host;
  if (r.port != default_port(r.tls)) s += ":" + std::to_string(r.port);
  return s;
}

void add_date(Document& doc, NodeId cap, const std::string& raw_date) {
  const std::string iso = nntp_date_iso(raw_date);
  InlineSpan date;
  date.role = InlineRole::Metadata;
  date.text = iso.empty() ? raw_date : short_time(iso);
  date.datetime = iso;
  doc.get(cap).spans.push_back(std::move(date));
}

std::string thousands(int64_t n) {
  std::string s = std::to_string(n);
  for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
  return s;
}
// Server, then the hierarchy steps of the group or wildmat the page shows:
// "host · comp.* · comp.lang.* · comp.lang.c" (the page's own step current).
std::vector<ServiceLink> nav_for(const NntpRef& r, std::string_view last_group,
                                 bool group_current) {
  std::vector<ServiceLink> nav;
  nav.push_back({server_label(r), nntp_groups_url(r, ""),
                 r.view == NntpView::Groups && r.wildmat.empty()});
  std::string name(last_group);
  if (r.view == NntpView::Groups) name = r.wildmat;
  if (name.empty()) return nav;
  // Hierarchy steps of `name`.
  std::string_view n = name;
  if (ends_with(n, ".*")) n.remove_suffix(2);
  size_t dot = 0;
  while ((dot = n.find('.', dot)) != std::string_view::npos) {
    const std::string h = std::string(n.substr(0, dot)) + ".*";
    nav.push_back({h, nntp_groups_url(r, h), false});
    ++dot;
  }
  if (r.view == NntpView::Groups) {
    nav.push_back({name, "", true});
  } else {
    nav.push_back({std::string(n), nntp_group_url(r, n), group_current});
  }
  return nav;
}

}  // namespace

ServiceBar nntp_service_bar(const NntpRef& r) {
  ServiceBar bar;
  bar.id = "nntp:" + r.host;
  bar.name = kSite;
  bar.home = nntp_groups_url(r, "");
  bar.color = 0x6b4fbb;
  const bool group_page = r.view == NntpView::Group;
  bar.rows.push_back({"", nav_for(r, r.view == NntpView::Groups ? "" : r.group, group_page)});
  return bar;
}

Document nntp_groups_document(std::vector<NntpGroupInfo> groups,
                              const NntpRef& ref) {
  Document doc;
  const std::string title = ref.wildmat.empty()
                                ? "Newsgroups on " + server_label(ref)
                                : ref.wildmat + " on " + server_label(ref);
  NodeId thread = start_document(doc, title, "groups");
  groups.erase(std::remove_if(groups.begin(), groups.end(),
                              [&](const NntpGroupInfo& g) {
                                return !nntp_wildmat_match(ref.wildmat, g.name);
                              }),
               groups.end());
  std::sort(groups.begin(), groups.end(),
            [](const NntpGroupInfo& a, const NntpGroupInfo& b) { return a.name < b.name; });
  // Folding: only for "" and "PREFIX*" wildmats.
  std::string prefix = ref.wildmat;
  bool foldable = prefix.empty() ||
                  (ends_with(prefix, "*") &&
                   prefix.substr(0, prefix.size() - 1).find_first_of("*?[,!") == std::string::npos);
  if (!prefix.empty()) prefix.pop_back();
  struct Entry {
    std::string name;       // group, or hierarchy "x.y.*"
    bool hierarchy = false;
    size_t groups = 0;
    int64_t articles = 0;
    const NntpGroupInfo* info = nullptr;
  };
  std::vector<Entry> entries;
  if (foldable && groups.size() > kNntpMaxListed) {
    std::map<std::string, Entry> fold;
    for (const NntpGroupInfo& g : groups) {
      std::string_view rest = std::string_view(g.name).substr(prefix.size());
      const size_t dot = rest.find('.', 1);
      if (dot == std::string_view::npos) {
        Entry e;
        e.name = g.name;
        e.info = &g;
        fold[g.name + "\x01"] = e;  // a group of its own, beside its hierarchy
        continue;
      }
      const std::string key = prefix + std::string(rest.substr(0, dot));
      Entry& e = fold[key];
      e.name = key + ".*";
      e.hierarchy = true;
      e.groups++;
      e.articles += g.count();
      if (e.groups == 1) e.info = &g;
    }
    for (auto& [k, e] : fold) {
      if (e.hierarchy && e.groups == 1) {  // a lone group: list it directly
        e.name = e.info->name;
        e.hierarchy = false;
      }
      entries.push_back(e);
    }
  } else {
    for (const NntpGroupInfo& g : groups) {
      if (entries.size() >= kNntpMaxListed) break;
      Entry e;
      e.name = g.name;
      e.info = &g;
      entries.push_back(e);
    }
  }
  NodeId summary = doc.create(NodeKind::Paragraph, doc.root);
  add_meta(doc, summary, thousands(static_cast<int64_t>(groups.size())) +
                             (groups.size() == 1 ? " group" : " groups"));
  if (!foldable && groups.size() > entries.size())
    add_meta(doc, summary, ", the first " + std::to_string(entries.size()) +
                               " shown; narrow the pattern");
  // The summary sits before the thread.
  auto& kids = doc.get(doc.root).children;
  if (kids.size() >= 2 && kids.back() == summary) {
    kids.pop_back();
    kids.insert(kids.end() - 1, summary);
  }
  for (const Entry& e : entries) {
    NodeId art = doc.create(NodeKind::Article, thread);
    doc.get(art).label = e.name;
    NodeId head = doc.create(NodeKind::Header, art);
    NodeId cap = doc.create(NodeKind::Caption, art);
    if (e.hierarchy) {
      add_link(doc, head, e.name, nntp_groups_url(ref, e.name));
      add_meta(doc, cap, thousands(static_cast<int64_t>(e.groups)) + " groups · " +
                             thousands(e.articles) + " articles");
      continue;
    }
    add_link(doc, head, e.name, nntp_group_url(ref, e.name));
    add_meta(doc, cap, thousands(e.info->count()) +
                           (e.info->count() == 1 ? " article" : " articles"));
    if (e.info->status == 'm') add_meta(doc, cap, " · moderated");
    if (e.info->status == 'n') add_meta(doc, cap, " · no posting");
    if (!e.info->description.empty()) {
      add_sep(doc, cap);
      add_meta(doc, cap, e.info->description);
    }
  }
  if (groups.empty())
    add_note(doc, thread, ref.wildmat.empty() ? "No groups." : "No groups match " + ref.wildmat + ".");
  return doc;
}

namespace {
std::string thread_root(const NntpOverview& o) {
  return o.references.empty() ? o.message_id : o.references.front();
}
}  // namespace

Document nntp_group_document(const std::vector<NntpOverview>& over,
                             const NntpRef& ref, int64_t first, int64_t low,
                             int64_t total) {
  Document doc;
  NodeId thread = start_document(doc, ref.group, ref.group);
  // Threads in this window: root id → member indices (window order).
  std::map<std::string, std::vector<size_t>> by_root;
  std::set<std::string> ids;
  for (size_t i = 0; i < over.size(); ++i) {
    by_root[thread_root(over[i])].push_back(i);
    ids.insert(over[i].message_id);
  }
  std::vector<const std::vector<size_t>*> threads;
  for (const auto& [root, members] : by_root) threads.push_back(&members);
  auto newest = [&](const std::vector<size_t>* m) {
    int64_t n = 0;
    for (size_t i : *m) n = std::max(n, over[i].number);
    return n;
  };
  std::sort(threads.begin(), threads.end(),
            [&](auto* a, auto* b) { return newest(a) > newest(b); });
  for (const std::vector<size_t>* m : threads) {
    const NntpOverview& head = over[m->front()];
    const NntpOverview* last = &head;
    for (size_t i : *m)
      if (over[i].number > last->number) last = &over[i];
    NodeId art = doc.create(NodeKind::Article, thread);
    doc.get(art).label = std::to_string(head.number);
    NodeId h = doc.create(NodeKind::Header, art);
    const std::string subject = head.references.empty()
                                    ? head.subject
                                    : strip_reply_prefix(head.subject);
    add_link(doc, h, subject.empty() ? "(no subject)" : subject,
             nntp_article_url(ref, ref.group, head.number));
    NodeId cap = doc.create(NodeKind::Caption, art);
    add_meta(doc, cap, mail_display_name(head.from));
    add_sep(doc, cap);
    add_date(doc, cap, last->date);
    add_sep(doc, cap);
    add_meta(doc, cap, count(static_cast<long long>(m->size()), "message", "messages"));
    if (!head.references.empty() && !ids.count(head.references.front()))
      add_meta(doc, cap, " · continues an older thread");
  }
  if (over.empty()) add_note(doc, thread, total == 0 ? "No articles." : "No articles on this page.");
  // Pager: page 1 = newest.
  const bool older = first > low;
  if (ref.page > 1 || older) {
    NodeId nav = doc.create(NodeKind::Paragraph, doc.root);
    if (ref.page > 1) {
      add_link(doc, nav, "← Newer", nntp_group_url(ref, ref.group, ref.page - 1));
      doc.append_text(nav, " · Page " + std::to_string(ref.page));
    }
    if (older) {
      if (ref.page > 1) doc.append_text(nav, " · ");
      add_link(doc, nav, "Older →", nntp_group_url(ref, ref.group, ref.page + 1));
    }
  }
  NodeId info = doc.create(NodeKind::Paragraph, doc.root);
  add_meta(doc, info, thousands(total) + " articles on " + server_label(ref));
  return doc;
}

std::vector<int64_t> nntp_thread_members(const std::vector<NntpOverview>& over,
                                         int64_t number) {
  const NntpOverview* self = nullptr;
  for (const NntpOverview& o : over)
    if (o.number == number) self = &o;
  if (!self) return {};
  const std::string root = thread_root(*self);
  std::vector<int64_t> out;
  for (const NntpOverview& o : over) {
    if (o.message_id == root ||
        std::find(o.references.begin(), o.references.end(), root) != o.references.end())
      out.push_back(o.number);
  }
  return out;
}

namespace {
void add_article_body(Document& doc, NodeId msg, const NntpArticle& a) {
  if (a.html) append_html(doc, msg, a.body, "", false);
  else append_usenet_body(doc, msg, a.body, a.flowed);
  for (const std::string& att : a.attachments)
    add_note(doc, msg, "Attachment: " + att);
}

// Group of the article for links: the one asked about, else the first.
std::string article_group(const NntpArticle& a, const NntpRef& ref) {
  if (!ref.group.empty()) return ref.group;
  return a.newsgroups.empty() ? std::string() : a.newsgroups.front();
}
}  // namespace

Document nntp_thread_document(const std::vector<NntpArticle>& articles,
                              const NntpRef& ref) {
  Document doc;
  std::string title;
  for (const NntpArticle& a : articles) {
    if (a.number == ref.article || title.empty())
      title = strip_reply_prefix(a.subject);
    if (a.references.empty()) {
      title = a.subject;
      break;
    }
  }
  if (title.empty()) title = ref.group;
  NodeId thread = start_document(doc, title, ref.group);
  // Reply tree: parent = the nearest reference present in this thread.
  std::unordered_map<std::string, size_t> by_id;
  for (size_t i = 0; i < articles.size(); ++i) by_id[articles[i].message_id] = i;
  std::vector<std::vector<size_t>> children(articles.size());
  std::vector<size_t> tops;
  std::vector<long> parent(articles.size(), -1);
  for (size_t i = 0; i < articles.size(); ++i) {
    const auto& refs = articles[i].references;
    for (size_t k = refs.size(); k-- > 0;) {
      auto it = by_id.find(refs[k]);
      if (it != by_id.end() && it->second != i) {
        parent[i] = static_cast<long>(it->second);
        break;
      }
    }
    if (parent[i] < 0) tops.push_back(i);
    else children[static_cast<size_t>(parent[i])].push_back(i);
  }
  auto by_number = [&](size_t a, size_t b) { return articles[a].number < articles[b].number; };
  std::sort(tops.begin(), tops.end(), by_number);
  for (auto& c : children) std::sort(c.begin(), c.end(), by_number);
  const std::string norm_title = strip_reply_prefix(title);
  std::vector<bool> seen(articles.size(), false);
  std::function<void(size_t)> emit = [&](size_t i) {
    if (seen[i]) return;
    seen[i] = true;
    const NntpArticle& a = articles[i];
    MsgHead h;
    h.label = std::to_string(a.number);
    h.author = mail_display_name(a.from);
    h.iso = nntp_date_iso(a.date);
    h.permalink = nntp_article_url(ref, ref.group, a.number);
    if (parent[i] >= 0) {
      h.reply_to = std::to_string(articles[static_cast<size_t>(parent[i])].number);
      h.indent = true;
    }
    NodeId msg = add_message(doc, thread, h);
    if (strip_reply_prefix(a.subject) != norm_title && !a.subject.empty()) {
      NodeId s = doc.create(NodeKind::Paragraph, msg);
      doc.append_text(s, a.subject, InlineKind::Strong);
    }
    if (parent[i] < 0 && !a.references.empty()) {
      NodeId p = doc.create(NodeKind::Paragraph, msg);
      add_meta(doc, p, "In reply to ");
      add_link(doc, p, "an earlier message", nntp_msgid_url(ref, a.references.back()),
               InlineRole::Metadata);
    }
    add_article_body(doc, msg, a);
    for (size_t c : children[i]) emit(c);
  };
  for (size_t t : tops) emit(t);
  for (size_t i = 0; i < articles.size(); ++i) emit(i);  // cycles, just in case
  if (articles.empty()) add_note(doc, thread, "Article not found.");
  return doc;
}

Document nntp_article_document(const NntpArticle& a, const NntpRef& ref) {
  Document doc;
  const std::string group = article_group(a, ref);
  NodeId thread =
      start_document(doc, a.subject.empty() ? "(no subject)" : a.subject, group);
  MsgHead h;
  h.label = a.message_id;
  h.author = mail_display_name(a.from);
  h.iso = nntp_date_iso(a.date);
  h.permalink = nntp_msgid_url(ref, a.message_id);
  NodeId msg = add_message(doc, thread, h);
  NodeId s = doc.create(NodeKind::Paragraph, msg);
  doc.append_text(s, a.subject, InlineKind::Strong);
  NodeId meta = doc.create(NodeKind::Paragraph, msg);
  for (const std::string& g : a.newsgroups) {
    add_sep(doc, meta);
    add_link(doc, meta, g, nntp_group_url(ref, g), InlineRole::Metadata);
  }
  if (!a.references.empty()) {
    add_sep(doc, meta);
    add_link(doc, meta, "in reply to", nntp_msgid_url(ref, a.references.back()),
             InlineRole::Metadata);
  }
  for (const std::string& g : a.newsgroups) {
    if (const int64_t n = nntp_xref_number(a.xref, g); n > 0) {
      add_sep(doc, meta);
      add_link(doc, meta, "thread", nntp_article_url(ref, g, n), InlineRole::Metadata);
      break;
    }
  }
  add_article_body(doc, msg, a);
  return doc;
}

}  // namespace xanatoo
