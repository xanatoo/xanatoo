// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/opensearch.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "xanatoo/html_min.hpp"
#include "xanatoo/paths.hpp"
#include "xanatoo/sitemap.hpp"
#include "xanatoo/util.hpp"
#if defined(XANATOO_WEB) && XANATOO_WEB
#include "xanatoo/https.hpp"
#include "xanatoo/version.hpp"
#endif

namespace xanatoo {

namespace {

// Text of the first <name>…</name> (prefix-tolerant: <os:ShortName>).
std::string xml_text_of(std::string_view xml, const std::string& low,
                        std::string_view name) {
  const std::string open = "<" + ascii_lower(name);
  for (size_t p = low.find(open); p != std::string::npos; p = low.find(open, p + 1)) {
    const char c = p + open.size() < low.size() ? low[p + open.size()] : '\0';
    if (c != '>' && c != ' ') continue;
    size_t e = low.find('>', p);
    if (e == std::string::npos) return {};
    size_t close = low.find("</", e);
    if (close == std::string::npos) return {};
    std::string t = html_decode_entities(xml.substr(e + 1, close - e - 1));
    std::string_view v = t;
    trim_ascii(v);
    return std::string(v);
  }
  return {};
}

std::string host_of(std::string_view url) {
  std::string h = url_host(url);
  if (starts_with(h, "www.")) h.erase(0, 4);
  return h;
}

bool read_text(const std::string& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

std::string escape_sexp(std::string_view s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') out.push_back('\\');
    if (c == '\n' || c == '\r') c = ' ';
    out.push_back(c);
  }
  return out;
}

}  // namespace

std::string engine_id_for_host(std::string_view host) {
  std::string h = ascii_lower(host);
  for (std::string_view pre : {"www.", "search.", "m."})
    if (starts_with(h, pre)) h.erase(0, pre.size());
  std::string id;
  for (char c : h) {
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) id.push_back(c);
    else if (!id.empty() && id.back() != '-') id.push_back('-');
  }
  while (!id.empty() && id.back() == '-') id.pop_back();
  return id.empty() ? "site" : id;
}

std::optional<SearchEngine> parse_opensearch(std::string_view xml,
                                             std::string_view description_url,
                                             std::string* err) {
  const std::string low = ascii_lower(xml);
  if (low.find("opensearchdescription") == std::string::npos) {
    if (err) *err = "not an OpenSearch description";
    return std::nullopt;
  }
  struct Candidate {
    int rank;
    std::string url, kind;
  };
  std::vector<Candidate> found;
  for (size_t p = low.find("<url"); p != std::string::npos; p = low.find("<url", p + 4)) {
    const char c = p + 4 < low.size() ? low[p + 4] : '\0';
    if (c != ' ' && c != '\n' && c != '\t' && c != '\r') continue;
    const size_t e = html_tag_end(xml, p);
    if (e == std::string_view::npos) break;
    std::string_view attrs = xml.substr(p + 4, e - p - 4);
    const std::string type = ascii_lower(html_attr(attrs, "type"));
    const std::string method = ascii_lower(html_attr(attrs, "method"));
    if (!method.empty() && method != "get") continue;
    int rank;
    std::string kind = "page";
    if (type == "text/html" || type == "application/xhtml+xml") rank = 0;
    else if (type.find("rss+xml") != std::string::npos ||
             type.find("atom+xml") != std::string::npos) {
      rank = 1;
      kind = "feed";
    } else continue;
    std::string tmpl = html_attr(attrs, "template");
    // Mozilla's <Param name= value=> children, up to </Url>.
    const bool self_closing = e > 0 && xml[e - 1] == '/';
    if (!self_closing) {
      size_t end = low.find("</url", e);
      for (size_t q = low.find("<param", e); q != std::string::npos && q < end;
           q = low.find("<param", q + 6)) {
        // Mozilla also has <MozParam> (old, with conditions): not taken.
        const size_t qe = html_tag_end(xml, q);
        if (qe == std::string_view::npos) break;
        std::string_view pa = xml.substr(q + 6, qe - q - 6);
        const std::string name = html_attr(pa, "name");
        const std::string value = html_attr(pa, "value");
        if (name.empty()) continue;
        tmpl += tmpl.find('?') == std::string::npos ? "?" : "&";
        tmpl += name + "=" + value;
      }
    }
    // Template parameters: {searchTerms} is the query; {x?} is optional.
    std::string url;
    bool has_terms = false, bad = false;
    for (size_t i = 0; i < tmpl.size(); ++i) {
      if (tmpl[i] != '{') {
        url.push_back(tmpl[i]);
        continue;
      }
      const size_t close = tmpl.find('}', i);
      if (close == std::string::npos) {
        bad = true;
        break;
      }
      std::string name = tmpl.substr(i + 1, close - i - 1);
      const bool optional = !name.empty() && name.back() == '?';
      if (optional) name.pop_back();
      if (size_t colon = name.find(':'); colon != std::string::npos)
        name = name.substr(colon + 1);  // {moz:locale}
      if (name == "searchTerms") {
        if (has_terms) bad = true;
        url += "%s";
        has_terms = true;
      } else if (name == "inputEncoding" || name == "outputEncoding") {
        url += "UTF-8";
      } else if (name == "startPage" || name == "startIndex") {
        if (!optional) url += "1";
      } else if (name == "count") {
        if (!optional) url += "20";
      } else if (name == "language") {
        if (!optional) url += "*";
      } else if (!optional) {
        bad = true;  // a required parameter we cannot fill
      }
      i = close;
    }
    if (bad || !has_terms ||
        !(starts_with_ci(url, "http://") || starts_with_ci(url, "https://")))
      continue;
    found.push_back({rank, std::move(url), std::move(kind)});
  }
  if (found.empty()) {
    if (err) *err = "no GET search template for web pages";
    return std::nullopt;
  }
  std::stable_sort(found.begin(), found.end(),
                   [](const Candidate& a, const Candidate& b) { return a.rank < b.rank; });
  SearchEngine e;
  e.url = std::move(found[0].url);
  e.kind = std::move(found[0].kind);
  // "%20" in a template means blanks are written that way.
  e.space = "+";
  const std::string host = host_of(e.url);
  const std::string desc_host = host_of(description_url);
  e.id = engine_id_for_host(desc_host.empty() ? host : desc_host);
  e.name = xml_text_of(xml, low, "shortname");
  if (e.name.empty()) e.name = xml_text_of(xml, low, "longname");
  if (e.name.empty()) e.name = desc_host.empty() ? host : desc_host;
  if (e.name.size() > 60) e.name.resize(60);
  for (const unsigned char c : e.name) {
    if (std::isalnum(c)) {
      e.badge = std::string(1, static_cast<char>(std::toupper(c)));
      break;
    }
  }
  if (e.badge.empty()) e.badge = "?";
  if (!desc_host.empty()) e.sites.push_back(desc_host);
  if (!host.empty() && host != desc_host) e.sites.push_back(host);
  return e;
}

std::string engine_sexp(const SearchEngine& e) {
  std::string out = "(engine \"" + escape_sexp(e.id) + "\"";
  auto prop = [&](std::string_view key, std::string_view val) {
    out += "\n        (" + std::string(key) + " \"" + escape_sexp(val) + "\")";
  };
  prop("name", e.name);
  if (!e.keyword.empty()) prop("keyword", e.keyword);
  if (!e.badge.empty()) prop("badge", e.badge);
  prop("url", e.url);
  if (e.kind != "page") prop("kind", e.kind);
  if (e.space != "+") prop("space", e.space);
  if (!e.sites.empty()) {
    out += "\n        (site";
    for (const std::string& s : e.sites) out += " \"" + escape_sexp(s) + "\"";
    out += ")";
  }
  out += ")\n";
  return out;
}

bool save_user_engine(const SearchEngine& e, std::string* err) {
  const std::string dir = user_data_dir();
  if (dir.empty() || no_save()) {
    if (err) *err = "no user data directory to save in";
    return false;
  }
  if (!ensure_dir(dir)) {
    if (err) *err = "cannot create " + dir;
    return false;
  }
  const std::string path = (std::filesystem::path(dir) / "search.sexp").string();
  std::string src;
  read_text(path, src);
  // Keep the user's text as written: the new entry is appended, and of
  // two entries with one id the last wins (load_engines).
  bool replaced = false;
  for (const SearchEngine& old : parse_engines(src))
    replaced = replaced || old.id == e.id;
  if (!src.empty() && src.back() != '\n') src += "\n";
  if (replaced)
    src += ";; " + e.id + " replaced below (the last entry of an id wins)\n";
  src += ";; Added from " + (e.sites.empty() ? std::string("a web page") : e.sites[0]) +
         " (OpenSearch)\n" + engine_sexp(e);
  if (!write_file_atomic(path, src)) {
    if (err) *err = "cannot write " + path;
    return false;
  }
  return true;
}

#if defined(XANATOO_WEB) && XANATOO_WEB
std::optional<SearchEngine> fetch_opensearch(std::string_view url, std::string* err) {
  std::string e, final_url;
  auto r = https_get_follow(url, user_agent("search engine description"), &e,
                            &final_url);
  if (!r) {
    if (err) *err = e.empty() ? "request failed" : e;
    return std::nullopt;
  }
  if (r->status != 200) {
    if (err) *err = "HTTP " + std::to_string(r->status);
    return std::nullopt;
  }
  if (r->body.size() > 256 * 1024) {
    if (err) *err = "description too large";
    return std::nullopt;
  }
  return parse_opensearch(r->body, final_url.empty() ? url : final_url, err);
}
#endif

}  // namespace xanatoo
