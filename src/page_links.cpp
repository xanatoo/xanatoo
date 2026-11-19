// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/page_links.hpp"

#include <algorithm>
#include <cstdlib>
#include <optional>

#include "xanatoo/html_min.hpp"
#include "xanatoo/util.hpp"

namespace xanatoo {

namespace {

std::vector<std::string> rel_tokens(std::string_view rel) {
  std::vector<std::string> out;
  std::string low = ascii_lower(rel);
  size_t p = 0;
  while (p < low.size()) {
    size_t e = low.find_first_of(" \t\r\n\f", p);
    if (e == std::string::npos) e = low.size();
    if (e > p) out.push_back(low.substr(p, e - p));
    p = e + 1;
  }
  return out;
}

std::string trimmed(std::string s) {
  std::string_view v = s;
  trim_ascii(v);
  return std::string(v);
}

// Normalised relation of one rel token; "" = not a page relation.
std::string_view relation_of(std::string_view t) {
  if (t == "up" || t == "parent") return "up";
  if (t == "first" || t == "start" || t == "begin" || t == "top") return "first";
  if (t == "prev" || t == "previous") return "prev";
  if (t == "next") return "next";
  if (t == "last" || t == "end") return "last";
  if (t == "contents" || t == "toc") return "contents";
  if (t == "index") return "index";
  if (t == "glossary") return "glossary";
  if (t == "help") return "help";
  if (t == "search") return "search";
  if (t == "author") return "author";
  if (t == "license" || t == "copyright") return "license";
  return {};
}

int largest_size(std::string_view sizes) {
  int best = 0;
  std::string low = ascii_lower(sizes);
  size_t p = 0;
  while (p < low.size()) {
    size_t e = low.find(' ', p);
    if (e == std::string::npos) e = low.size();
    std::string tok = low.substr(p, e - p);
    if (size_t x = tok.find('x'); x != std::string::npos) {
      const int w = std::atoi(tok.substr(0, x).c_str());
      const int h = std::atoi(tok.substr(x + 1).c_str());
      best = std::max(best, std::max(w, h));
    }
    p = e + 1;
  }
  return best;
}

bool usable_href(std::string_view h) {
  return !h.empty() && !starts_with_ci(h, "javascript:") &&
         !starts_with_ci(h, "data:") && h != "#";
}

void add_relation(PageLinks& out, std::string_view rel, std::string url,
                  std::string title) {
  for (const PageRelation& r : out.relations)
    if (r.rel == rel) return;  // the first one wins
  out.relations.push_back({std::string(rel), std::move(url), std::move(title)});
}

void sort_relations(PageLinks& out) {
  const auto& order = page_relation_order();
  auto rank = [&](const PageRelation& r) {
    return std::find(order.begin(), order.end(), r.rel) - order.begin();
  };
  std::stable_sort(out.relations.begin(), out.relations.end(),
                   [&](const PageRelation& a, const PageRelation& b) {
                     return rank(a) < rank(b);
                   });
}

// Text inside the <a> that starts at `open_end` (tags dropped), short.
std::string anchor_text(std::string_view html, size_t open_end) {
  size_t close = ascii_lower(html.substr(open_end, 2000)).find("</a");
  std::string_view inner = html.substr(open_end, close == std::string::npos ? 0 : close);
  std::string text;
  bool in_tag = false;
  for (char c : inner) {
    if (c == '<') in_tag = true;
    else if (c == '>') in_tag = false;
    else if (!in_tag) text.push_back(c == '\n' || c == '\t' || c == '\r' ? ' ' : c);
  }
  text = html_decode_entities(text);
  std::string out;
  for (char c : text)
    if (c != ' ' || (!out.empty() && out.back() != ' ')) out.push_back(c);
  return trimmed(out);
}

struct HostPath {
  std::string host, path, rest;  // rest = query (without the fragment)
};

std::optional<HostPath> split_web(std::string_view url) {
  std::string_view u = url;
  if (starts_with_ci(u, "https://")) u.remove_prefix(8);
  else if (starts_with_ci(u, "http://")) u.remove_prefix(7);
  else return std::nullopt;
  u = u.substr(0, u.find('#'));
  size_t slash = u.find_first_of("/?");
  HostPath hp;
  hp.host = ascii_lower(u.substr(0, slash));
  if (slash != std::string_view::npos) {
    std::string_view r = u.substr(slash);
    size_t q = r.find('?');
    hp.path = std::string(r.substr(0, q));
    if (q != std::string_view::npos) hp.rest = std::string(r.substr(q));
  }
  for (std::string_view pre : {"www.", "m.", "amp."})
    if (starts_with(hp.host, pre)) hp.host.erase(0, pre.size());
  if (size_t colon = hp.host.find(':'); colon != std::string::npos)
    hp.host.erase(colon);  // :443 / :80 against the bare host
  while (!hp.path.empty() && hp.path.back() == '/') hp.path.pop_back();
  if (ends_with(hp.path, "/amp")) hp.path.resize(hp.path.size() - 4);
  return hp;
}

}  // namespace

const std::vector<std::string_view>& page_relation_order() {
  static const std::vector<std::string_view> k = {
      "up", "first", "prev", "next", "last", "contents", "index",
      "glossary", "help", "search", "author", "license"};
  return k;
}

std::string_view page_relation_label(std::string_view rel) {
  if (rel == "up") return "Up";
  if (rel == "first") return "First page";
  if (rel == "prev") return "Previous page";
  if (rel == "next") return "Next page";
  if (rel == "last") return "Last page";
  if (rel == "contents") return "Contents";
  if (rel == "index") return "Index";
  if (rel == "glossary") return "Glossary";
  if (rel == "help") return "Help";
  if (rel == "search") return "Search";
  if (rel == "author") return "Author";
  if (rel == "license") return "License";
  return rel;
}

PageLinks page_links(std::string_view html, std::string_view page_url) {
  PageLinks out;
  const std::string low = ascii_lower(html);
  size_t body = low.find("<body");
  if (body == std::string::npos) body = low.size();
  std::string base(page_url);

  if (size_t h = low.find("<html"); h != std::string::npos && h < body) {
    size_t e = html_tag_end(html, h);
    if (e != std::string_view::npos)
      out.lang = trimmed(html_attr(html.substr(h + 5, e - h - 5), "lang"));
  }
  if (size_t b = low.find("<base"); b != std::string::npos && b < body) {
    size_t e = html_tag_end(html, b);
    if (e != std::string_view::npos) {
      std::string href = trimmed(html_attr(html.substr(b + 5, e - b - 5), "href"));
      if (usable_href(href)) base = resolve_url(page_url, href);
    }
  }

  auto seen_url = [](const auto& v, const std::string& url) {
    for (const auto& x : v)
      if (x.url == url) return true;
    return false;
  };

  for (size_t p = low.find("<link"); p != std::string::npos && p < body;
       p = low.find("<link", p + 5)) {
    const char after = p + 5 < low.size() ? low[p + 5] : '\0';
    if (after != ' ' && after != '\t' && after != '\n' && after != '\r' &&
        after != '/')
      continue;  // <linkfoo>
    size_t e = html_tag_end(html, p);
    if (e == std::string_view::npos) break;
    std::string_view attrs = html.substr(p + 5, e - p - 5);
    const std::vector<std::string> rels = rel_tokens(html_attr(attrs, "rel"));
    if (rels.empty()) continue;
    std::string href = trimmed(html_attr(attrs, "href"));
    if (!usable_href(href)) continue;
    const std::string url = resolve_url(base, href);
    const std::string type = ascii_lower(trimmed(html_attr(attrs, "type")));
    const std::string title = trimmed(html_attr(attrs, "title"));
    auto has = [&](std::string_view t) {
      return std::find(rels.begin(), rels.end(), t) != rels.end();
    };

    if (has("icon") || has("apple-touch-icon") ||
        has("apple-touch-icon-precomposed")) {
      if (!seen_url(out.icons, url))
        out.icons.push_back({url, type, largest_size(html_attr(attrs, "sizes")),
                             !has("icon")});
      continue;
    }
    if (has("canonical")) {
      if (out.canonical.empty()) out.canonical = url;
      continue;
    }
    if (has("search") && type.find("opensearchdescription") != std::string::npos) {
      if (!seen_url(out.searches, url)) out.searches.push_back({url, title});
      continue;
    }
    if (has("alternate")) {
      const std::string lang = trimmed(html_attr(attrs, "hreflang"));
      // Feeds are feed_links' (feed.hpp); other alternates without a
      // language (print, media) are not offered.
      if (!lang.empty() && (type.empty() || type.find("html") != std::string::npos)) {
        bool dup = false;
        for (const LanguageLink& l : out.languages)
          dup = dup || ascii_lower(l.lang) == ascii_lower(lang);
        if (!dup) out.languages.push_back({lang, url, title});
      }
      continue;
    }
    for (const std::string& t : rels)
      if (std::string_view r = relation_of(t); !r.empty())
        add_relation(out, r, url, title);
  }

  // Pagination often lives only in the body: <a rel="next" href=…>.
  for (size_t p = low.find("<a", body); p != std::string::npos;
       p = low.find("<a", p + 2)) {
    const char after = p + 2 < low.size() ? low[p + 2] : '\0';
    if (after != ' ' && after != '\t' && after != '\n' && after != '\r')
      continue;
    size_t e = html_tag_end(html, p);
    if (e == std::string_view::npos) break;
    std::string_view attrs = html.substr(p + 2, e - p - 2);
    if (low.substr(p, e - p).find("rel") == std::string::npos) continue;
    const std::vector<std::string> rels = rel_tokens(html_attr(attrs, "rel"));
    std::string href = trimmed(html_attr(attrs, "href"));
    if (rels.empty() || !usable_href(href) || href[0] == '#') continue;
    for (const std::string& t : rels) {
      std::string_view r = relation_of(t);
      // In running text "author", "help", "index" or "search" mostly mark
      // ordinary links; only the sequence ones and the license are kept.
      if (r != "next" && r != "prev" && r != "first" && r != "last" &&
          r != "up" && r != "license")
        continue;
      std::string text = anchor_text(html, e + 1);
      if (text.size() > 80) text = text.substr(0, 77) + "...";
      add_relation(out, r, resolve_url(base, href), std::move(text));
    }
  }
  sort_relations(out);
  return out;
}

std::vector<std::string> icon_candidates(const PageLinks& links) {
  struct Ranked {
    int rank;
    const PageIcon* icon;
  };
  std::vector<Ranked> r;
  for (const PageIcon& i : links.icons) {
    const std::string low = ascii_lower(i.url.substr(0, i.url.find('?')));
    const bool ico = i.type.find("icon") != std::string::npos ||
                     ends_with(low, ".ico");
    const bool svg = i.type.find("svg") != std::string::npos || ends_with(low, ".svg");
    int rank;
    if (ico) rank = 3000;
    else if (i.touch) rank = 2000 + i.size;
    else if (svg) rank = 1000;
    else if (i.size == 0) rank = 500;  // raster, size not given
    else rank = i.size >= 32 ? i.size - 32 : 400 + (32 - i.size);
    r.push_back({rank, &i});
  }
  std::stable_sort(r.begin(), r.end(),
                   [](const Ranked& a, const Ranked& b) { return a.rank < b.rank; });
  std::vector<std::string> out;
  for (const Ranked& x : r) out.push_back(x.icon->url);
  return out;
}

std::string safe_canonical(std::string_view page_url, std::string_view canonical) {
  if (canonical.empty() || canonical == page_url) return {};
  auto a = split_web(page_url);
  auto b = split_web(canonical);
  if (!a || !b || a->host != b->host || a->path != b->path) return {};
  // Canonical may drop the query, never invent one.
  if (!b->rest.empty() && b->rest != a->rest) return {};
  return std::string(canonical);
}

namespace {

struct LanguageNames {
  std::string_view code, native, english;
};

const LanguageNames kLanguages[] = {
    {"af", "Afrikaans", "Afrikaans"},     {"ar", "العربية", "Arabic"},
    {"be", "Беларуская", "Belarusian"},   {"bg", "Български", "Bulgarian"},
    {"bn", "বাংলা", "Bengali"},             {"bs", "Bosanski", "Bosnian"},
    {"ca", "Català", "Catalan"},          {"cs", "Čeština", "Czech"},
    {"cy", "Cymraeg", "Welsh"},           {"da", "Dansk", "Danish"},
    {"de", "Deutsch", "German"},          {"el", "Ελληνικά", "Greek"},
    {"en", "English", "English"},         {"eo", "Esperanto", "Esperanto"},
    {"es", "Español", "Spanish"},         {"et", "Eesti", "Estonian"},
    {"eu", "Euskara", "Basque"},          {"fa", "فارسی", "Persian"},
    {"fi", "Suomi", "Finnish"},           {"fr", "Français", "French"},
    {"fy", "Frysk", "Frisian"},           {"ga", "Gaeilge", "Irish"},
    {"gl", "Galego", "Galician"},         {"gu", "ગુજરાતી", "Gujarati"},
    {"he", "עברית", "Hebrew"},            {"hi", "हिन्दी", "Hindi"},
    {"hr", "Hrvatski", "Croatian"},       {"hu", "Magyar", "Hungarian"},
    {"hy", "Հայերեն", "Armenian"},        {"id", "Bahasa Indonesia", "Indonesian"},
    {"is", "Íslenska", "Icelandic"},      {"it", "Italiano", "Italian"},
    {"ja", "日本語", "Japanese"},          {"ka", "ქართული", "Georgian"},
    {"kk", "Қазақ", "Kazakh"},            {"km", "ខ្មែរ", "Khmer"},
    {"kn", "ಕನ್ನಡ", "Kannada"},             {"ko", "한국어", "Korean"},
    {"lo", "ລາວ", "Lao"},                 {"lt", "Lietuvių", "Lithuanian"},
    {"lv", "Latviešu", "Latvian"},        {"mk", "Македонски", "Macedonian"},
    {"ml", "മലയാളം", "Malayalam"},        {"mr", "मराठी", "Marathi"},
    {"ms", "Bahasa Melayu", "Malay"},     {"my", "မြန်မာ", "Burmese"},
    {"nb", "Norsk bokmål", "Norwegian Bokmål"},
    {"ne", "नेपाली", "Nepali"},             {"nl", "Nederlands", "Dutch"},
    {"nn", "Norsk nynorsk", "Norwegian Nynorsk"},
    {"no", "Norsk", "Norwegian"},         {"pa", "ਪੰਜਾਬੀ", "Punjabi"},
    {"pl", "Polski", "Polish"},           {"pt", "Português", "Portuguese"},
    {"ro", "Română", "Romanian"},         {"ru", "Русский", "Russian"},
    {"si", "සිංහල", "Sinhala"},            {"sk", "Slovenčina", "Slovak"},
    {"sl", "Slovenščina", "Slovenian"},   {"sq", "Shqip", "Albanian"},
    {"sr", "Српски", "Serbian"},          {"sv", "Svenska", "Swedish"},
    {"sw", "Kiswahili", "Swahili"},       {"ta", "தமிழ்", "Tamil"},
    {"te", "తెలుగు", "Telugu"},             {"th", "ไทย", "Thai"},
    {"tl", "Tagalog", "Tagalog"},         {"tr", "Türkçe", "Turkish"},
    {"uk", "Українська", "Ukrainian"},    {"ur", "اردو", "Urdu"},
    {"uz", "Oʻzbek", "Uzbek"},            {"vi", "Tiếng Việt", "Vietnamese"},
    {"zh", "中文", "Chinese"},
};

std::string with_region(std::string_view name, std::string_view tag) {
  std::string out(name);
  const size_t dash = tag.find_first_of("-_");
  if (dash != std::string_view::npos) {
    std::string region(tag.substr(dash + 1));
    if (region.size() == 2)
      for (char& c : region)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    out += " (" + region + ")";
  }
  return out;
}

const LanguageNames* find_language(std::string_view tag) {
  const std::string low = ascii_lower(tag);
  const std::string primary = low.substr(0, low.find_first_of("-_"));
  for (const LanguageNames& l : kLanguages)
    if (primary == l.code) return &l;
  return nullptr;
}

}  // namespace

std::string language_name(std::string_view tag) {
  if (ascii_lower(tag) == "x-default") return "Default";
  const LanguageNames* l = find_language(tag);
  return l ? with_region(l->native, tag) : std::string(tag);
}

std::string language_english_name(std::string_view tag) {
  if (ascii_lower(tag) == "x-default") return "Default";
  const LanguageNames* l = find_language(tag);
  return l ? with_region(l->english, tag) : std::string(tag);
}

std::string accept_language_value(const std::vector<std::string>& tags) {
  std::string out;
  int q = 10;
  for (const std::string& t : tags) {
    if (!out.empty()) out += ",";
    out += t;
    if (q < 10) out += ";q=0." + std::to_string(q);
    if (q > 1) --q;
  }
  return out;
}

std::vector<std::string> parse_language_list(std::string_view s) {
  std::vector<std::string> out;
  size_t p = 0;
  while (p <= s.size()) {
    size_t e = s.find_first_of(", ;", p);
    if (e == std::string_view::npos) e = s.size();
    std::string_view t = s.substr(p, e - p);
    trim_ascii(t);
    bool ok = !t.empty() && t.size() <= 35 && t.front() != '-' && t.back() != '-';
    for (char c : t)
      ok = ok && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-');
    if (ok && std::find(out.begin(), out.end(), std::string(t)) == out.end())
      out.emplace_back(t);
    // "de;q=0.8": skip the weight, order is what counts.
    if (e < s.size() && s[e] == ';') {
      e = s.find(',', e);
      if (e == std::string_view::npos) break;
    }
    p = e + 1;
  }
  return out;
}

}  // namespace xanatoo
