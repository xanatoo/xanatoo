// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/search.hpp"

#include "xanatoo/paths.hpp"
#include "xanatoo/sexpr.hpp"
#include "xanatoo/util.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace xanatoo {

namespace {

bool valid_id(std::string_view id) {
  if (id.empty()) return false;
  for (char c : id)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
      return false;
  return true;
}

bool unreserved(unsigned char c) {
  return std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~';
}

std::string encode(std::string_view raw, std::string_view space) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : raw) {
    if (unreserved(c))
      out += static_cast<char>(c);
    else if (c == ' ')
      out += space;
    else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

std::string collapse_blanks(std::string_view raw) {
  std::string out;
  bool blank = true;  // also trims the front
  for (char c : raw) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      blank = true;
      continue;
    }
    if (blank && !out.empty()) out += ' ';
    blank = false;
    out += c;
  }
  return out;
}

bool read_text(const std::string& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

// Lower-case host of an http(s) URL, "" for anything else.
std::string http_host(std::string_view url) {
  const size_t p = url.find("://");
  if (p == std::string_view::npos) return {};
  const std::string scheme = ascii_lower(url.substr(0, p));
  if (scheme != "http" && scheme != "https") return {};
  std::string_view h = url.substr(p + 3);
  h = h.substr(0, h.find_first_of("/?#"));
  if (const size_t at = h.rfind('@'); at != std::string_view::npos)
    h.remove_prefix(at + 1);
  h = h.substr(0, h.find(':'));
  return ascii_lower(h);
}

const SearchEngine* find_engine(const std::vector<SearchEngine>& engines,
                                std::string_view id) {
  for (const SearchEngine& e : engines)
    if (e.id == id) return &e;
  return nullptr;
}

}  // namespace

namespace {

// Built-ins that start switched off (checked 2026-11-14): they need
// credentials (Edit → Credentials) or the service refuses anonymous clients.
const std::pair<std::string_view, std::string_view> kOffByDefault[] = {
    {"igdb", "needs a Twitch client ID and token"},
    {"rawg", "needs an API key"},
    {"tmdb-movie", "needs an API key"},
    {"tmdb-tv", "needs an API key"},
    {"discogs", "needs a token"},
    {"googlebooks", "shared anonymous quota exhausted (HTTP 429)"},
    {"loc-books", "blocked (HTTP 403)"},
    {"loc-newspapers", "blocked (HTTP 403)"},
    {"loc-films", "blocked (HTTP 403)"},
    {"loc-music", "blocked (HTTP 403)"},
    {"reddit", "blocked (HTTP 403), RSS feeds discontinued"},
};

std::vector<SearchEngine> builtin_table();

}  // namespace

const std::vector<SearchEngine>& builtin_engines() {
  static const std::vector<SearchEngine> k = [] {
    std::vector<SearchEngine> v = builtin_table();
    for (SearchEngine& e : v)
      for (const auto& [id, why] : kOffByDefault)
        if (e.id == id) {
          e.enabled = false;
          e.note = std::string(why);
        }
    return v;
  }();
  return k;
}

namespace {

std::vector<SearchEngine> builtin_table() {
  // id, name, keyword, badge, url, kind, space, sites
  return {
      {"wikipedia", "Wikipedia", "w", "W",
       "https://en.wikipedia.org/w/api.php?action=query&list=search"
       "&srsearch=%s&srlimit=30&format=json&formatversion=2",
       "wikipedia", "+",
       {"wikipedia:", "wikipedia.org"}},
      {"wikipedia-de", "Wikipedia (Deutsch)", "wd", "Wd",
       "https://de.wikipedia.org/w/api.php?action=query&list=search"
       "&srsearch=%s&srlimit=30&format=json&formatversion=2",
       "wikipedia", "+",
       {"wikipedia:de:", "de.wikipedia.org"}},
      {"hn", "Hacker News", "hn", "HN",
       "https://hn.algolia.com/api/v1/search?query=%s&tags=story"
       "&hitsPerPage=30",
       "hn", "+",
       {"hn:", "news.ycombinator.com", "hn.algolia.com"}},
      {"arxiv", "arXiv", "ax", "arX",
       "https://export.arxiv.org/api/query?search_query=all:%s&max_results=50",
       "feed", "+AND+all:",
       {"arxiv.org"}},  // every word must match (a bare blank is OR)
      {"archive", "Internet Archive", "ia", "IA",
       "https://archive.org/advancedsearch.php?q=%s&fl%5B%5D=identifier"
       "&fl%5B%5D=title&fl%5B%5D=description&fl%5B%5D=mediatype"
       "&fl%5B%5D=creator&sort%5B%5D=downloads+desc&rows=40&output=json",
       "archive", "+",
       {"ia:", "archive.org"}},
      // Published media: public, unauthenticated catalog APIs.
      {"openlibrary", "Open Library", "ol", "OL",
       "https://openlibrary.org/search.json?q=%s&limit=30", "openlibrary", "+",
       {"openlibrary.org"}},
      {"tvmaze", "TVmaze", "tv", "TV",
       "https://api.tvmaze.com/search/shows?q=%s", "tvmaze", "+",
       {"tvmaze.com", "api.tvmaze.com"}},
      {"musicbrainz", "MusicBrainz (releases)", "mb", "MB",
       "https://musicbrainz.org/ws/2/release-group/?query=%s&fmt=json&limit=25",
       "musicbrainz", "+", {"musicbrainz.org"}},
      {"wikidata", "Wikidata", "wdt", "WD",
       "https://www.wikidata.org/w/api.php?action=wbsearchentities&search=%s"
       "&language=en&limit=30&format=json", "wikidata", "+",
       {"wikidata.org"}},
      {"openalex", "OpenAlex", "oa", "OA",
       "https://api.openalex.org/works?search=%s&per-page=25", "openalex", "+",
       {"openalex.org"}},
      {"crossref", "Crossref", "cr", "CR",
       "https://api.crossref.org/works?query=%s&rows=25", "crossref", "+",
       {"crossref.org", "doi.org"}},
      // More published-media sources: native JSON catalogs, no scraping.
      {"steam", "Steam games", "st", "St",
       "https://store.steampowered.com/api/storesearch/?term=%s&l=english&cc=us",
       "steam", "+", {"store.steampowered.com", "steamcommunity.com"}},
      {"igdb", "IGDB games", "ig", "IG",
       "https://api.igdb.com/v4/games?search=%s", "igdb", "+", {"igdb.com"}},
      {"anilist-anime", "AniList anime", "anime", "An",
       "https://graphql.anilist.co/?search=%s", "anilist-anime", "+",
       {"anilist.co"}},
      {"anilist-manga", "AniList manga", "manga", "Mn",
       "https://graphql.anilist.co/?search=%s", "anilist-manga", "+",
       {"anilist.co"}},
      {"rawg", "RAWG games", "rw", "Rw",
       "https://api.rawg.io/api/games?search=%s&page_size=25", "rawg", "+",
       {"rawg.io"}},
      {"tmdb-movie", "TMDB movies", "tm", "Tm",
       "https://api.themoviedb.org/3/search/movie?query=%s", "tmdb", "+",
       {"themoviedb.org"}},
      {"tmdb-tv", "TMDB series", "tt", "Tt",
       "https://api.themoviedb.org/3/search/tv?query=%s", "tmdb", "+",
       {"themoviedb.org"}},
      {"ifdb", "IFDB interactive fiction", "if", "IF",
       "https://ifdb.org/search?json&game&searchfor=%s", "ifdb", "+",
       {"ifdb.org"}},
      {"bookbrainz-work", "BookBrainz works", "bw", "Bw",
       "https://api.bookbrainz.org/1/search?q=%s&type=work&size=25&from=0",
       "bookbrainz", "+", {"bookbrainz.org"}},
      {"bookbrainz-edition", "BookBrainz editions", "be", "Be",
       "https://api.bookbrainz.org/1/search?q=%s&type=edition&size=25&from=0",
       "bookbrainz", "+", {"bookbrainz.org"}},
      {"googlebooks", "Google Books", "gb", "Gb",
       "https://www.googleapis.com/books/v1/volumes?q=%s&maxResults=25",
       "googlebooks", "+", {"books.google.com"}},
      {"loc-books", "Library of Congress books", "lc", "LC",
       "https://www.loc.gov/books/?q=%s&fo=json&c=25", "loc", "+",
       {"loc.gov"}},
      {"loc-newspapers", "Library of Congress newspapers", "ln", "Ln",
       "https://www.loc.gov/newspapers/?q=%s&fo=json&c=25", "loc", "+",
       {"loc.gov"}},
      {"loc-films", "Library of Congress film", "lf", "Lf",
       "https://www.loc.gov/film-and-videos/?q=%s&fo=json&c=25", "loc", "+",
       {"loc.gov"}},
      {"loc-music", "Library of Congress sheet music", "lm", "Lm",
       "https://www.loc.gov/notated-music/?q=%s&fo=json&c=25", "loc", "+",
       {"loc.gov"}},
      {"zdb", "ZDB periodicals", "zdb", "ZDB",
       "https://zeitschriftendatenbank.de/api/tit.jsonld?q=%s&size=25",
       "zdb", "+", {"zeitschriftendatenbank.de", "zdb-katalog.de"}},
      {"gcd", "Grand Comics Database series", "gcd", "GCD",
       "https://www.comics.org/api/series/name/%s/", "gcd", "%20",
       {"comics.org"}},
      {"discogs", "Discogs releases", "dg", "Dg",
       "https://api.discogs.com/database/search?q=%s&type=release&per_page=25",
       "discogs", "+", {"discogs.com"}},
      {"standard-ebooks", "Standard Ebooks", "se", "SE",
       "https://standardebooks.org/ebooks?query=%s", "page", "+",
       {"standardebooks.org"}},
      {"gutenberg", "Project Gutenberg", "pg", "PG",
       "https://gutendex.com/books/?search=%s", "gutendex", "+",
       {"gutenberg.org", "gutendex.com"}},
      {"reddit", "Reddit", "r", "R", "https://www.reddit.com/search.rss?q=%s",
       "feed", "+",
       {"reddit:", "reddit.com"}},
      {"lemmy", "Lemmy (lemmy.world)", "l", "Lm",
       "https://lemmy.world/api/v3/search?q=%s&type_=Posts&limit=30", "lemmy",
       "+",
       {"lemmy:", "lemmy.world"}},
      {"bluesky", "Bluesky", "bs", "Bs",
       "https://api.bsky.app/xrpc/app.bsky.feed.searchPosts?q=%s&limit=30",
       "bluesky", "+",
       {"bsky.app", "bsky.social"}},
      {"mastodon-tag", "Mastodon hashtag", "mt", "#",
       "https://mastodon.social/tags/%s.rss", "feed", "",
       {"mastodon.social"}},
      {"flickr", "Flickr tags", "fl", "Fl",
       "https://www.flickr.com/services/feeds/photos_public.gne?tags=%s"
       "&format=rss2",
       "feed", ",",
       {"flickr.com"}},
      {"marginalia", "Marginalia (small web)", "mg", "Mg",
       "https://marginalia-search.com/search?query=%s", "page", "+",
       {"marginalia-search.com", "search.marginalia.nu"}},
      {"wiby", "Wiby (old web)", "wb", "Wb", "https://wiby.me/?q=%s", "page",
       "+",
       {"wiby.me"}},
      {"curlie", "Curlie", "cu", "Cu",
       "https://curlie.org/search?q=%s&stime=92452189", "page", "+",
       {"curlie.org"}},
      {"youtube", "YouTube", "yt", "YT",
       "youtube:results?search_query=%s", "page", "+",
       {"youtube:", "youtube.com", "youtu.be"}},
      {"peertube", "PeerTube (SepiaSearch)", "pt", "PT",
       "peertube:sepiasearch.org/search?search=%s", "page", "+",
       {"peertube:", "sepiasearch.org"}},
      {"odysee", "Odysee", "od", "Od", "odysee:search?s=%s", "page", "+",
       {"odysee:", "odysee.com"}},
      {"mediaccc", "media.ccc.de", "ccc", "CCC", "mediaccc:search?q=%s",
       "page", "+",
       {"mediaccc:", "media.ccc.de"}},
      {"lesswrong", "LessWrong", "lw", "LW", "lesswrong:search?query=%s",
       "page", "+",
       {"lesswrong:", "lesswrong.com"}},
      {"digg", "Digg", "dg", "Dg", "digg:search?query=%s",
       "page", "+",
       {"digg:", "digg.com"}},
      {"dailymotion", "Dailymotion", "dm", "DM",
       "dailymotion:search?search=%s", "page", "+",
       {"dailymotion:", "dailymotion.com"}},
      {"duckduckgo", "DuckDuckGo", "dd", "DDG",
       "https://html.duckduckgo.com/html/?q=%s", "page", "+",
       {"duckduckgo.com"}},
      // Server-rendered result page with /page/<slug> links (checked
      // 2026-11-18; /api/full-text-search has JSON too).
      {"grokipedia", "Grokipedia", "gk", "Gk",
       "https://grokipedia.com/search?q=%s", "page", "+",
       {"grokipedia.com"}},
      // Free, no-key web search APIs (JSON result mappers).
      {"freeserp", "FreeSerp", "fs", "FS",
       "https://freeserp.ai/api.php?index=web&q=%s&size=30", "freeserp", "+",
       {"freeserp.ai"}},
      // Firecrawl Keyless: POST JSON, free per-IP daily quota, no signup.
      // search_plan requires a %s in the template (same pattern as igdb/anilist);
      // catalog_get overwrites the URL and sends the query in the POST body.
      {"firecrawl", "Firecrawl (keyless)", "fc", "FC",
       "https://api.firecrawl.dev/v2/search?q=%s", "firecrawl", "+",
       {"firecrawl.dev", "api.firecrawl.dev"}},
  };
}

}  // namespace

bool engine_on(const SearchEngine& e, const std::vector<std::string>& on,
               const std::vector<std::string>& off) {
  if (std::find(off.begin(), off.end(), e.id) != off.end()) return false;
  if (std::find(on.begin(), on.end(), e.id) != on.end()) return true;
  return e.enabled;
}

std::vector<SearchEngine> engines_on(const std::vector<SearchEngine>& engines,
                                     const std::vector<std::string>& on,
                                     const std::vector<std::string>& off) {
  std::vector<SearchEngine> out;
  for (const SearchEngine& e : engines)
    if (engine_on(e, on, off)) out.push_back(e);
  return out;
}

uint32_t engine_color(const SearchEngine& e) {
  if (e.color != 0) return e.color;
  // Close to each service's own favicon, so the tile is recognisable.
  static const std::pair<std::string_view, uint32_t> kColors[] = {
      {"wikipedia", 0x3a3d40},  {"wikipedia-de", 0x3a3d40},
      {"hn", 0xff6600},         {"arxiv", 0xb31b1b},
      {"archive", 0x4a4a4a},    {"openlibrary", 0x0376b8},
      {"tvmaze", 0x3c948b},     {"musicbrainz", 0xba478f},
      {"wikidata", 0x006699},   {"openalex", 0x4f46e5},
      {"crossref", 0xd8323c},   {"steam", 0x1b2838},
      {"igdb", 0x9147ff},       {"anilist-anime", 0x02a9ff},
      {"anilist-manga", 0x0b84c6}, {"rawg", 0x262626},
      {"tmdb-movie", 0x0d253f}, {"tmdb-tv", 0x0d253f},
      {"ifdb", 0x6b4e9b},       {"bookbrainz-work", 0xeb743b},
      {"bookbrainz-edition", 0xeb743b}, {"googlebooks", 0x4285f4},
      {"loc-books", 0x0f3d63},  {"loc-newspapers", 0x0f3d63},
      {"loc-films", 0x0f3d63},  {"loc-music", 0x0f3d63},
      {"zdb", 0x00589c},        {"gcd", 0xa50000},
      {"discogs", 0x333333},    {"standard-ebooks", 0x2b8a8a},
      {"gutenberg", 0x8a2c18},  {"reddit", 0xff4500},
      {"lemmy", 0x00a37a},      {"bluesky", 0x1185fe},
      {"mastodon-tag", 0x6364ff}, {"flickr", 0xff0084},
      {"marginalia", 0x9c7d4a}, {"wiby", 0x000080},
      {"curlie", 0xc2410c},     {"youtube", 0xff0000},
      {"peertube", 0xf1680d},  {"odysee", 0xe50054},
      {"mediaccc", 0x2a6e3f},
      {"dailymotion", 0x0066dc}, {"duckduckgo", 0xde5833},
      {"grokipedia", 0x111111},  {"lesswrong", 0x5f9b65},
      {"digg", 0x333333},
      {"freeserp", 0x5b21b6},  {"firecrawl", 0xf97316},
  };
  for (const auto& [id, c] : kColors)
    if (e.id == id) return c;
  // FNV-1a of the id → a hue; fixed saturation / value read well with white.
  uint32_t h = 2166136261u;
  for (unsigned char ch : e.id) h = (h ^ ch) * 16777619u;
  const float hue = static_cast<float>(h % 360u) / 60.f;
  const float s = 0.6f, v = 0.7f;
  const float c = v * s;
  const float x = c * (1.f - std::abs(std::fmod(hue, 2.f) - 1.f));
  float r = 0, g = 0, b = 0;
  switch (static_cast<int>(hue)) {
    case 0: r = c; g = x; break;
    case 1: r = x; g = c; break;
    case 2: g = c; b = x; break;
    case 3: g = x; b = c; break;
    case 4: r = x; b = c; break;
    default: r = c; b = x; break;
  }
  const float m = v - c;
  auto ch = [&](float f) {
    return static_cast<uint32_t>((f + m) * 255.f + 0.5f) & 0xff;
  };
  const uint32_t out = (ch(r) << 16) | (ch(g) << 8) | ch(b);
  return out != 0 ? out : 0x010101;
}

std::optional<uint32_t> parse_hex_color(std::string_view s) {
  if (!s.empty() && s.front() == '#') s.remove_prefix(1);
  if (s.size() != 3 && s.size() != 6) return std::nullopt;
  uint32_t v = 0;
  for (char ch : s) {
    int d;
    if (ch >= '0' && ch <= '9') d = ch - '0';
    else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
    else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
    else return std::nullopt;
    v = (v << 4) | static_cast<uint32_t>(d);
    if (s.size() == 3) v = (v << 4) | static_cast<uint32_t>(d);
  }
  return v;
}

bool is_json_search_kind(std::string_view kind) {
  return kind == "wikipedia" || kind == "hn" || kind == "archive" ||
         kind == "lemmy" || kind == "bluesky" || kind == "gutendex" ||
         kind == "openlibrary" || kind == "tvmaze" ||
         kind == "musicbrainz" || kind == "wikidata" ||
         kind == "openalex" || kind == "crossref" || kind == "steam" ||
         kind == "igdb" || kind == "rawg" || kind == "tmdb" ||
         kind == "anilist-anime" || kind == "anilist-manga" ||
         kind == "googlebooks" || kind == "loc" || kind == "zdb" ||
         kind == "ifdb" || kind == "bookbrainz" ||
         kind == "gcd" || kind == "discogs" || kind == "freeserp" ||
         kind == "firecrawl";
}

bool is_search_kind(std::string_view kind) {
  return kind == "page" || kind == "feed" || is_json_search_kind(kind);
}

std::vector<SearchEngine> parse_engines(std::string_view source,
                                        std::vector<std::string>* warnings) {
  auto warn = [&](const std::string& m) {
    if (warnings) warnings->push_back(m);
  };
  std::vector<SearchEngine> out;
  for (const SExpr& f : SExprParser(source).parse_all()) {
    if (!is_form(f, "engine")) continue;
    SearchEngine e;
    if (f.kids.size() > 1) e.id = sexpr_text(f.kids[1]);
    if (!valid_id(e.id)) {
      warn("engine id must be lower-case letters, digits and '-': \"" + e.id +
           "\"");
      continue;
    }
    e.name = e.id;
    for (size_t i = 2; i < f.kids.size(); ++i) {
      const SExpr& p = f.kids[i];
      if (p.kind != SExpr::Kind::List || p.kids.size() < 2) continue;
      const std::string key = sexpr_text(p.kids[0]);
      const std::string val = sexpr_text(p.kids[1]);
      if (key == "name") e.name = val;
      else if (key == "keyword") e.keyword = val;
      else if (key == "badge") e.badge = val;
      else if (key == "url") e.url = val;
      else if (key == "kind") {
        if (is_search_kind(val))
          e.kind = val;
        else
          warn("engine " + e.id + ": unknown kind " + val);
      }
      else if (key == "space") e.space = val;
      else if (key == "enabled") {
        const std::string v = ascii_lower(val);
        if (v == "yes" || v == "true" || v == "1" || v == "on")
          e.enabled = true;
        else if (v == "no" || v == "false" || v == "0" || v == "off")
          e.enabled = false;
        else
          warn("engine " + e.id + ": enabled needs yes or no, not " + val);
      }
      else if (key == "color") {
        if (auto c = parse_hex_color(val); c && *c != 0)
          e.color = *c;
        else
          warn("engine " + e.id + ": color needs #rrggbb, not " + val);
      }
      else if (key == "site") {
        for (size_t j = 1; j < p.kids.size(); ++j) {
          std::string site = sexpr_text(p.kids[j]);
          if (!site.empty()) e.sites.push_back(std::move(site));
        }
      }
      else warn("engine " + e.id + ": unknown property " + key);
    }
    const size_t pct = e.url.find("%s");
    if (pct == std::string::npos || e.url.find("%s", pct + 2) != std::string::npos ||
        !(starts_with(e.url, "http://") || starts_with(e.url, "https://"))) {
      warn("engine " + e.id + ": url needs an http(s) address with one %s");
      continue;
    }
    if (e.badge.empty()) e.badge = e.name.substr(0, 1);
    out.push_back(std::move(e));
  }
  return out;
}

std::vector<SearchEngine> load_engines() {
  std::vector<SearchEngine> all = builtin_engines();
  const std::string dir = user_data_dir();
  if (dir.empty()) return all;
  std::string src;
  if (!read_text((std::filesystem::path(dir) / "search.sexp").string(), src))
    return all;
  for (SearchEngine& e : parse_engines(src)) {
    auto it = std::find_if(all.begin(), all.end(), [&](const SearchEngine& b) {
      return b.id == e.id;
    });
    if (it != all.end())
      *it = std::move(e);
    else
      all.push_back(std::move(e));
  }
  return all;
}

std::string search_encode(std::string_view raw) {
  return encode(collapse_blanks(raw), "+");
}

std::string search_decode(std::string_view s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '+') {
      out += ' ';
    } else if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(
                   static_cast<unsigned char>(s[i + 1])) &&
               std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
      out += static_cast<char>(std::stoi(std::string(s.substr(i + 1, 2)), nullptr, 16));
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

bool is_search_ref(std::string_view s) { return parse_search_ref(s).has_value(); }

std::optional<SearchRef> parse_search_ref(std::string_view s) {
  trim_ascii(s);
  if (!starts_with(s, kSearchPrefix)) return std::nullopt;
  s.remove_prefix(kSearchPrefix.size());
  const size_t colon = s.find(':');
  if (colon == std::string_view::npos) return std::nullopt;
  SearchRef r;
  r.engine = std::string(s.substr(0, colon));
  r.query = std::string(s.substr(colon + 1));
  if (!valid_id(r.engine) || r.query.empty()) return std::nullopt;
  return r;
}

std::string search_ref(std::string_view engine, std::string_view raw) {
  const std::string q = search_encode(raw);
  if (q.empty() || !valid_id(engine)) return {};
  return std::string(kSearchPrefix) + std::string(engine) + ":" + q;
}

std::optional<SearchPlan> search_plan(std::string_view ref,
                                      const std::vector<SearchEngine>& engines) {
  auto r = parse_search_ref(ref);
  if (!r) return std::nullopt;
  const SearchEngine* e = find_engine(engines, r->engine);
  if (!e) return std::nullopt;
  std::string url = e->url;
  const size_t at = url.find("%s");
  if (at == std::string::npos) return std::nullopt;
  SearchPlan p;
  p.query = search_decode(r->query);
  url.replace(at, 2, encode(p.query, e->space));
  p.url = std::move(url);
  p.kind = e->kind;
  return p;
}

std::string search_target(std::string_view ref,
                          const std::vector<SearchEngine>& engines) {
  auto p = search_plan(ref, engines);
  if (!p || is_json_search_kind(p->kind)) return {};
  return p->kind == "feed" ? "feed:" + p->url : p->url;
}

std::string search_from_keyword(std::string_view text,
                                const std::vector<SearchEngine>& engines,
                                std::string_view default_engine) {
  trim_ascii(text);
  // "?" is an alias for the default search engine (Preferences / search bar).
  // Accept both "? query" and "?query".
  if (!default_engine.empty() && !text.empty() && text[0] == '?') {
    std::string_view q = text.substr(1);
    while (!q.empty() && (q[0] == ' ' || q[0] == '\t')) q.remove_prefix(1);
    if (!q.empty() && find_engine(engines, default_engine))
      return search_ref(default_engine, q);
    return {};
  }
  const size_t sp = text.find(' ');
  if (sp == std::string_view::npos) return {};
  const std::string_view word = text.substr(0, sp);
  for (const SearchEngine& e : engines)
    if (!e.keyword.empty() && e.keyword == word)
      return search_ref(e.id, text.substr(sp + 1));
  return {};
}

std::string engine_for_address(std::string_view address,
                               const std::vector<SearchEngine>& engines) {
  trim_ascii(address);
  if (auto r = parse_search_ref(address))
    return find_engine(engines, r->engine) ? r->engine : std::string();
  const std::string host = http_host(address);
  const std::string lower = ascii_lower(address);
  std::string best;
  size_t best_len = 0;
  for (const SearchEngine& e : engines) {
    for (const std::string& site : e.sites) {
      const std::string s = ascii_lower(site);
      bool hit;
      if (s.find(':') != std::string::npos)
        hit = starts_with(lower, s);
      else
        hit = !host.empty() &&
              (host == s || (host.size() > s.size() &&
                             ends_with(host, s) &&
                             host[host.size() - s.size() - 1] == '.'));
      if (hit && s.size() > best_len) {
        best = e.id;
        best_len = s.size();
      }
    }
  }
  return best;
}

std::string search_query_text(std::string_view ref) {
  auto r = parse_search_ref(ref);
  return r ? search_decode(r->query) : std::string();
}

}  // namespace xanatoo
