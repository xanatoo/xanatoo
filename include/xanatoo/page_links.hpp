// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// What a web page says about itself in <link rel=…> (and a few <a rel=…>):
// its icons, its place in a sequence (next / prev / contents …), its
// OpenSearch description, its canonical address and its versions in other
// languages. Pure: read from the raw HTML, no fetching.

#include <string>
#include <string_view>
#include <vector>

namespace xanatoo {

struct PageIcon {
  std::string url;
  std::string type;   // "image/png" … as written, lower-cased ("" = none)
  int size = 0;       // largest edge of `sizes` ("any" = 0)
  bool touch = false; // apple-touch-icon
};

// A page related to this one. `rel` is the normalised relation:
// up, first, prev, next, last, contents, index, glossary, help, search,
// author, license (see page_relation_label).
struct PageRelation {
  std::string rel;
  std::string url;
  std::string title;
};

struct OpenSearchLink {
  std::string url;    // the description document (XML)
  std::string title;
};

struct LanguageLink {
  std::string lang;  // hreflang as written ("de", "pt-BR", "x-default")
  std::string url;
  std::string title;
};

struct PageLinks {
  std::vector<PageIcon> icons;
  std::vector<PageRelation> relations;  // in page_relation_order
  std::vector<OpenSearchLink> searches;
  std::string canonical;
  std::vector<LanguageLink> languages;
  std::string lang;  // <html lang>
};

// Reads <head> (<base href> respected) and, for sequence relations a page
// did not declare there, <a rel=…> in the body. Duplicates are dropped.
PageLinks page_links(std::string_view html, std::string_view page_url);

// Relations in menu order with their labels ("Next page", "Contents").
const std::vector<std::string_view>& page_relation_order();
std::string_view page_relation_label(std::string_view rel);

// Declared icons best first for a small (16–32 px) location-bar icon:
// PNG / SVG / other raster icons near 32 px, then touch icons, ICO last.
std::vector<std::string> icon_candidates(const PageLinks& links);

// `canonical` when it names this very page (same path on the same site,
// modulo http/https, a www. / m. / amp. host prefix, a trailing slash and
// the query), so dropping tracking parameters cannot turn into "every page
// is the home page" on a misconfigured site. Empty otherwise.
std::string safe_canonical(std::string_view page_url, std::string_view canonical);

// Name of a language tag in that language where known ("de" → "Deutsch",
// "pt-BR" → "Português (BR)", "x-default" → "Default"), else the tag.
std::string language_name(std::string_view tag);
// The same in English ("de" → "German"), for fonts without the script.
std::string language_english_name(std::string_view tag);

// Accept-Language header value for preferred tags, best first:
// {"de", "en"} → "de,en;q=0.9". Empty list → empty string.
std::string accept_language_value(const std::vector<std::string>& tags);
// "de, en-GB ,fr" → {"de", "en-GB", "fr"} (invalid tags dropped).
std::vector<std::string> parse_language_list(std::string_view s);

}  // namespace xanatoo
