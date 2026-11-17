// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Related / similar websites of a domain (Recommendations panel). Sources,
// tried in order and merged by domain:
//   - similarsites.com: public site pages, no key; the list is the JSON the
//     page embeds (`__NEXT_DATA__` → props/pageProps/siteData/SimilarSites).
//   - webXtrend (api.webxtrend.com/v1.0/similar): free account token,
//     100 calls a day; only asked when a token is configured.
//   - similars.net (/api/{domain}): free key, 1000 requests a month, at most
//     five results; only asked when a key is configured. Their terms ask for
//     a link back to similars.net next to the results.
// Parsers are pure and tested offline; `recommend_fetch` needs XANATOO_WEB.

#include <string>
#include <string_view>
#include <vector>

namespace xanatoo {

struct SimilarSite {
  std::string domain;       // bare host, lower case ("lwn.net")
  std::string title;
  std::string description;
  std::string category;     // human readable, may be empty
  double score = 0;         // 0..1, higher is closer; 0 when unknown
  std::string source;       // "similarsites.com", "webxtrend", "similars.net"
};

// Domains to ask for `host`, most specific first: "www." is dropped and a
// subdomain falls back to its parent ("en.wikipedia.org" → that, then
// "wikipedia.org"), never down to a bare public suffix ("co.uk"). Empty for
// IP addresses, "localhost" and other single-label names.
std::vector<std::string> recommend_domains(std::string_view host);

// Lower-case bare host of a site entry ("https://Www.X.org/" → "x.org").
std::string similar_site_domain(std::string_view s);

// similarsites.com "/site/<domain>" HTML page. An empty list is a valid
// answer (the site is unknown there); `err` is set only when the page is not
// in the expected shape.
std::vector<SimilarSite> parse_similarsites_page(std::string_view html,
                                                 std::string* err = nullptr);
// webXtrend /v1.0/similar reply.
std::vector<SimilarSite> parse_webxtrend_similar(std::string_view json,
                                                 std::string* err = nullptr);
// similars.net /api/{domain} reply.
std::vector<SimilarSite> parse_similars_net(std::string_view json,
                                            std::string* err = nullptr);

// Append `more` to `into`, skipping domains already listed and `self`.
void merge_similar_sites(std::vector<SimilarSite>& into,
                         const std::vector<SimilarSite>& more,
                         std::string_view self = {});

struct RecommendKeys {
  std::string webxtrend_token;
  std::string similars_key;
};

struct RecommendResult {
  std::string domain;                 // the domain the list belongs to
  std::vector<SimilarSite> sites;
  std::vector<std::string> notes;     // one line per source that failed
};

// Blocking: ask every usable source for `host` (worker thread only).
RecommendResult recommend_fetch(std::string_view host, const RecommendKeys& keys);

}  // namespace xanatoo
