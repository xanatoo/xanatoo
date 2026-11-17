// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// Similar websites of a domain (related_sites.hpp).

#include "xanatoo/related_sites.hpp"

#include "xanatoo/json_dom.hpp"
#include "xanatoo/util.hpp"

#if defined(XANATOO_WEB) && XANATOO_WEB
#include "xanatoo/https.hpp"
#include "xanatoo/version.hpp"
#endif

#include <algorithm>
#include <unordered_set>

namespace xanatoo {

namespace {

bool domain_chars_ok(std::string_view d) {
  if (d.empty() || d.size() > 253 || d.front() == '.' || d.back() == '.' ||
      d.front() == '-')
    return false;
  for (char c : d)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
          c == '-'))
      return false;
  return d.find("..") == std::string_view::npos;
}

std::vector<std::string_view> labels_of(std::string_view d) {
  std::vector<std::string_view> out;
  size_t start = 0;
  while (true) {
    const size_t dot = d.find('.', start);
    out.push_back(d.substr(start, dot - start));
    if (dot == std::string_view::npos) break;
    start = dot + 1;
  }
  return out;
}

bool all_digits(std::string_view s) {
  return !s.empty() && s.find_first_not_of("0123456789") == std::string_view::npos;
}

// "co.uk", "com.au", "or.jp": second-level labels under a country code that
// are registries rather than sites.
bool public_suffix_pair(std::string_view sld, std::string_view tld) {
  static constexpr std::string_view kSld[] = {
      "co", "com", "org", "net", "ac", "gov", "edu", "ne", "or", "go",
      "gob", "gouv", "mil", "nic", "ltd", "plc", "sch", "nom", "info", "biz"};
  if (tld.size() != 2) return false;
  return std::find(std::begin(kSld), std::end(kSld), sld) != std::end(kSld);
}

// "Computers_Electronics_and_Technology/Programming_and_Developer_Software"
// → "Computers Electronics and Technology › Programming and Developer Software".
std::string readable_category(std::string_view c) {
  std::string out;
  for (char ch : c) {
    if (ch == '_') out += ' ';
    else if (ch == '/') out += " \xe2\x80\xba ";
    else out += ch;
  }
  return out;
}

double json_double(const Json* v) {
  if (!v) return 0;
  if (v->type == Json::Type::Number) return v->n;
  if (v->type == Json::Type::String) return std::strtod(v->s.c_str(), nullptr);
  return 0;
}

std::string trimmed(const std::string& s) {
  std::string_view v(s);
  trim_ascii(v);
  return std::string(v);
}

}  // namespace

std::string similar_site_domain(std::string_view s) {
  trim_ascii(s);
  if (const size_t p = s.find("://"); p != std::string_view::npos)
    s = s.substr(p + 3);
  s = s.substr(0, s.find_first_of("/?#"));
  if (const size_t at = s.rfind('@'); at != std::string_view::npos)
    s = s.substr(at + 1);
  if (!s.empty() && s.front() != '[')
    s = s.substr(0, s.find(':'));
  std::string d = ascii_lower(s);
  while (!d.empty() && d.back() == '.') d.pop_back();
  if (starts_with(d, "www.")) d.erase(0, 4);
  return d;
}

std::vector<std::string> related_domains(std::string_view host) {
  const std::string d = similar_site_domain(host);
  std::vector<std::string> out;
  if (!domain_chars_ok(d)) return out;
  const auto labels = labels_of(d);
  if (labels.size() < 2) return out;  // localhost, intranet names
  if (all_digits(labels.back())) return out;  // IPv4
  for (const auto& l : labels)
    if (l.empty()) return out;
  for (size_t i = 0; i + 1 < labels.size(); ++i) {
    const size_t n = labels.size() - i;
    if (n == 2 && public_suffix_pair(labels[i], labels[i + 1])) break;
    std::string cand;
    for (size_t j = i; j < labels.size(); ++j) {
      if (j > i) cand += '.';
      cand += labels[j];
    }
    out.push_back(std::move(cand));
  }
  return out;
}

std::vector<SimilarSite> parse_similarsites_page(std::string_view html,
                                                 std::string* err) {
  std::vector<SimilarSite> out;
  constexpr std::string_view kMark = "id=\"__NEXT_DATA__\"";
  const size_t mark = html.find(kMark);
  if (mark == std::string_view::npos) {
    if (err) *err = "similarsites.com: page without site data";
    return out;
  }
  const size_t open = html.find('>', mark);
  const size_t close = open == std::string_view::npos
                           ? std::string_view::npos
                           : html.find("</script>", open);
  if (close == std::string_view::npos) {
    if (err) *err = "similarsites.com: truncated site data";
    return out;
  }
  const auto root = json_parse(html.substr(open + 1, close - open - 1));
  if (!root) {
    if (err) *err = "similarsites.com: unreadable site data";
    return out;
  }
  const Json* list = json_path(*root, "props/pageProps/siteData/SimilarSites");
  if (!list || list->type != Json::Type::Array) return out;  // unknown site
  for (const Json& e : list->items) {
    SimilarSite s;
    s.domain = similar_site_domain(json_string(json_get(e, "Site")));
    if (!domain_chars_ok(s.domain)) continue;
    s.title = trimmed(json_string(json_get(e, "Title")));
    s.description = trimmed(json_string(json_get(e, "Description")));
    s.category = readable_category(json_string(json_get(e, "Category")));
    s.score = std::clamp(json_double(json_get(e, "Grade")), 0.0, 1.0);
    s.source = "similarsites.com";
    out.push_back(std::move(s));
  }
  return out;
}

std::vector<SimilarSite> parse_webxtrend_similar(std::string_view text,
                                                 std::string* err) {
  std::vector<SimilarSite> out;
  const auto root = json_parse(text);
  if (!root) {
    if (err) *err = "webXtrend: unreadable reply";
    return out;
  }
  const std::string msg = json_string(json_get(*root, "msg"));
  const std::string error = json_string(json_get(*root, "error"));
  if (!error.empty()) {
    if (err) *err = "webXtrend: " + (msg.empty() ? error : msg);
    return out;
  }
  // The list is either the reply itself or under "data" / "results".
  const Json* list = root->type == Json::Type::Array ? &*root : nullptr;
  for (const char* key : {"data", "results", "similar", "sites"})
    if (!list)
      if (const Json* v = json_get(*root, key); v && v->type == Json::Type::Array)
        list = v;
  if (!list) return out;
  for (const Json& e : list->items) {
    SimilarSite s;
    s.domain = similar_site_domain(json_string(json_get(e, "domain")));
    if (!domain_chars_ok(s.domain)) continue;
    s.title = trimmed(json_string(json_get(e, "title")));
    s.description = trimmed(json_string(json_get(e, "description")));
    s.score = std::clamp(json_double(json_get(e, "similarity")), 0.0, 1.0);
    s.source = "webxtrend";
    out.push_back(std::move(s));
  }
  std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
    return a.score > b.score;
  });
  return out;
}

std::vector<SimilarSite> parse_similars_net(std::string_view text,
                                            std::string* err) {
  std::vector<SimilarSite> out;
  const auto root = json_parse(text);
  if (!root) {
    if (err) *err = "similars.net: unreadable reply";
    return out;
  }
  if (root->type == Json::Type::Object) {
    if (const std::string e = json_string(json_get(*root, "error")); !e.empty()) {
      if (err) *err = "similars.net: " + e;
      return out;
    }
  }
  const Json* list = root->type == Json::Type::Array ? &*root : nullptr;
  for (const char* key : {"data", "results", "sites"})
    if (!list)
      if (const Json* v = json_get(*root, key); v && v->type == Json::Type::Array)
        list = v;
  if (!list) return out;
  for (const Json& e : list->items) {
    SimilarSite s;
    s.domain = similar_site_domain(json_string(json_get(e, "domain")));
    if (s.domain.empty())
      s.domain = similar_site_domain(json_string(json_get(e, "url")));
    if (!domain_chars_ok(s.domain)) continue;
    s.title = trimmed(json_string(json_get(e, "title")));
    s.description = trimmed(json_string(json_get(e, "description")));
    std::string cat = json_string(json_get(e, "category"));
    const std::string sub = json_string(json_get(e, "subcategory"));
    if (!sub.empty()) cat += (cat.empty() ? "" : " \xe2\x80\xba ") + sub;
    s.category = cat;
    s.score = std::clamp(json_double(json_get(e, "match_score")) / 100.0, 0.0, 1.0);
    s.source = "similars.net";
    out.push_back(std::move(s));
  }
  return out;
}

void merge_similar_sites(std::vector<SimilarSite>& into,
                         const std::vector<SimilarSite>& more,
                         std::string_view self) {
  std::unordered_set<std::string> seen;
  for (const auto& s : into) seen.insert(s.domain);
  const std::string me = similar_site_domain(self);
  for (const auto& s : more) {
    if (s.domain.empty() || s.domain == me || !seen.insert(s.domain).second)
      continue;
    into.push_back(s);
  }
}

#if defined(XANATOO_WEB) && XANATOO_WEB

namespace {

const std::string& related_agent() {
  static const std::string ua = user_agent("related sites");
  return ua;
}

std::string json_quote(std::string_view s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out + "\"";
}

std::vector<SimilarSite> ask_similarsites(const std::string& domain,
                                          std::string& note) {
  std::string e;
  auto r = https_get_follow("https://similarsites.com/site/" + domain,
                            related_agent(), &e);
  if (!r) {
    note = "similarsites.com: " + (e.empty() ? std::string("no answer") : e);
    return {};
  }
  if (r->status != 200) {
    note = "similarsites.com: HTTP " + std::to_string(r->status);
    return {};
  }
  return parse_similarsites_page(r->body, &note);
}

std::vector<SimilarSite> ask_webxtrend(const std::string& domain,
                                       const std::string& token,
                                       std::string& note) {
  std::string e;
  const std::string body = "{\"token\": " + json_quote(token) +
                           ", \"query\": " + json_quote(domain) + "}";
  auto r = https_request("POST", "https://api.webxtrend.com/v1.0/similar",
                         related_agent(),
                         "Content-Type: application/json\r\n"
                         "Accept: application/json\r\n",
                         body, &e);
  if (!r) {
    note = "webXtrend: " + (e.empty() ? std::string("no answer") : e);
    return {};
  }
  auto out = parse_webxtrend_similar(r->body, &note);
  if (note.empty() && r->status != 200)
    note = "webXtrend: HTTP " + std::to_string(r->status);
  return out;
}

std::vector<SimilarSite> ask_similars_net(const std::string& domain,
                                          const std::string& key,
                                          std::string& note) {
  std::string e;
  auto r = https_request("GET", "https://similars.net/api/" + domain + "?limit=5",
                         related_agent(),
                         "Authorization: Bearer " + key +
                             "\r\nAccept: application/json\r\n",
                         {}, &e);
  if (!r) {
    note = "similars.net: " + (e.empty() ? std::string("no answer") : e);
    return {};
  }
  auto out = parse_similars_net(r->body, &note);
  if (note.empty() && r->status != 200)
    note = "similars.net: HTTP " + std::to_string(r->status);
  return out;
}

}  // namespace

RelatedResult related_fetch(std::string_view host, const RelatedKeys& keys) {
  RelatedResult res;
  const auto domains = related_domains(host);
  if (domains.empty()) {
    res.notes.push_back("Not a public web site.");
    return res;
  }
  // The most specific domain any source knows; parents only when nothing
  // was found for the subdomain.
  for (const std::string& d : domains) {
    std::vector<std::string> notes;
    std::vector<SimilarSite> sites;
    auto run = [&](std::vector<SimilarSite> got, std::string note) {
      if (!note.empty()) notes.push_back(std::move(note));
      merge_similar_sites(sites, got, d);
    };
    {
      std::string n;
      auto got = ask_similarsites(d, n);
      run(std::move(got), std::move(n));
    }
    if (!keys.webxtrend_token.empty()) {
      std::string n;
      auto got = ask_webxtrend(d, keys.webxtrend_token, n);
      run(std::move(got), std::move(n));
    }
    if (!keys.similars_key.empty()) {
      std::string n;
      auto got = ask_similars_net(d, keys.similars_key, n);
      run(std::move(got), std::move(n));
    }
    res.domain = d;
    res.notes = std::move(notes);
    if (!sites.empty()) {
      res.sites = std::move(sites);
      return res;
    }
  }
  res.domain = domains.front();
  return res;
}

#else

RelatedResult related_fetch(std::string_view host, const RelatedKeys&) {
  RelatedResult res;
  res.domain = similar_site_domain(host);
  res.notes.push_back("Related sites need a build with -DXANATOO_WEB=ON.");
  return res;
}

#endif

}  // namespace xanatoo
