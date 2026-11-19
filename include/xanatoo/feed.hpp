// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include "xanatoo/types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xanatoo {

// RSS / Atom feed URLs that open in-app: an explicit `feed:` prefix
// (`feed:https://example.org/` also autodiscovers from an HTML page), or
// an http(s) URL that looks like a feed: *.rss / *.atom / *.rdf / *.xml,
// a /feed, /rss, /atom segment, /feeds/…, or ?feed=rss2 / ?format=atom.
bool is_feed_url(std::string_view s);

// The URL to request: "feed:" stripped; explicit http/https preserved.
std::string feed_http_url(std::string_view s);

// Root element is <rss>, <feed> (Atom) or <rdf:RDF> (RSS 1.0).
bool looks_like_feed(std::string_view body);

// A feed an HTML page announces: <link rel="alternate"
// type="application/(rss|atom|rdf)+xml" href=… title=…>.
struct FeedLink {
  std::string url;    // resolved against the page URL
  std::string title;  // the link's title attribute ("" = none)
  std::string type;   // "RSS" or "Atom"
};

// All feeds of an HTML page in document order, duplicates dropped; only
// the part before <body> is read when the page has one.
std::vector<FeedLink> feed_links(std::string_view html, std::string_view page_url);

// URL of the first of feed_links; empty when none.
std::string feed_autodiscover(std::string_view html, std::string_view page_url);

// Feed XML → Document: feed title / subtitle, then a Thread with one
// Message per entry (author, date, linked title, content via html_min).
std::optional<Document> parse_feed(std::string_view xml,
                                   std::string_view feed_url,
                                   std::string* err = nullptr);

#if defined(XANATOO_FEEDS) && XANATOO_FEEDS
// Fetch and parse; an HTML page is followed to its autodiscovered feed.
std::optional<Document> feed_fetch(std::string_view url,
                                   std::string* err = nullptr);
#endif

}  // namespace xanatoo
