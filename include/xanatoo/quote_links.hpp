// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include "xanatoo/editor.hpp"
#include "xanatoo/types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace xanatoo {

// Automatic transclusion in discussion threads, see docs/QUOTES.md.
//
// Forums have no transclusion: a reply quotes by copying text ("> …" or a
// <blockquote>). find_quote_links recovers the link after the fact by
// searching the thread for the quoted text. The result is derived data (like
// layout); the document is never changed.

struct QuoteLink {
  NodeId quoting_msg = INVALID_NODE;  // Message that contains the quote
  Selection quote;                    // the quoted text in quoting_msg
  NodeId source_msg = INVALID_NODE;   // Message that holds the original
  Selection source;                   // the original text in source_msg
  float score = 0.f;  // 1 = exact (after normalization); below 1 = fuzzy
  bool via_quote = false;  // matched only a quote in source_msg (the origin
                           // is not in the thread); source_msg quoted it too
};

// Every quote in the Message nodes below `root` (INVALID_NODE = doc.root)
// that matches text of another Message in the same set. Quotes are
// BlockQuote nodes and, inside other text nodes, lines starting with '>'
// (raw Markdown bodies from Reddit / Lemmy). "...", "…" and "[...]" split
// a quote into pieces that must match in order. Unmatched or too short
// quotes are left out. Order: by quote position in the document.
std::vector<QuoteLink> find_quote_links(const Document& doc,
                                        NodeId root = INVALID_NODE);

// The normalized form matching compares: words lower-cased, everything
// between them one space, no leading / trailing space. Exposed for tests.
std::string quote_normalize(std::string_view utf8);

}  // namespace xanatoo
