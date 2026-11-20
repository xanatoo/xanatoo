// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Forgiving HTML fragment → document nodes, for remote content (feed
// entries, Hacker News comments, …). Tag soup is fine: unclosed <p> / <li>
// close implicitly, unknown wrappers (div, span, section) are unwrapped,
// script / style / iframe are dropped with their contents.

#include "xanatoo/types.hpp"

#include <functional>
#include <string>
#include <string_view>

namespace xanatoo {

// What a per-tag hook (site hints, site_hints.hpp) says about one element.
// Pointers point into the page source the hook was built for.
struct HtmlHint {
  enum class Kind {
    None,     // usual handling
    Drop,     // left out with its contents
    Chrome,   // a Chrome region with `role`
    Content,  // usual handling, but never chrome by tag / class / role
    Replace,  // HtmlOptions::hint_replace builds its nodes
  } kind = Kind::None;
  std::string role;
  const char* inner = nullptr;      // just past the start tag
  const char* inner_end = nullptr;  // the end tag
  const char* end = nullptr;        // just past the element
};

struct HtmlOptions {
  std::string base_url;    // relative href / src resolve against this
  bool gt_quotes = false;  // paragraphs starting with ">" → BlockQuote (HN)
  // Whole web pages (html_page.cpp):
  bool sections = false;     // h1–h6 open nested Section + Header (outline)
  bool anchors = false;      // id= → node label; same-page links stay "#id"
  std::string page_url;      // "same page" for anchors (default: base_url)
  // drop_chrome: drop hidden and screen-reader-only elements, alt="" images
  // and tracking pixels; page chrome (nav / aside / form / footer, landmark
  // roles, navbox / share / cookie classes) is dropped too, or becomes Chrome
  // nodes with chrome_nodes (whole pages: reader mode hides them).
  bool drop_chrome = false;
  bool chrome_nodes = false;
  // <img> at block level → Image node (remote http(s) images are fetched
  // by the app); icons (declared edge ≤ 48) and images inside quotes / list
  // items / cells → inline image spans (InlineKind::Image, linked when in
  // an <a>). Without it, or for data: URIs, an "[image: alt]" link.
  bool images = false;
  // <aside> → Aside box (content beside the flow) rather than page chrome;
  // asides with chrome classes (sidebar, share, …) stay chrome.
  bool asides = false;
  // Site hints: called for every start tag (`lt` = its '<' in the page
  // source) when set; `hint_replace` builds a Replace element's nodes
  // under `parent`.
  std::function<HtmlHint(const char* lt)> hint;
  std::function<void(Document& doc, NodeId parent, const char* lt)>
      hint_replace;
};

// Append the blocks of `html` as children of `parent`:
// p / div → Paragraph, h1–h6 → Heading, ul / ol / li → List / ListItem,
// blockquote → BlockQuote, pre → CodeBlock, table → Table, hr; inline
// a / em / strong / code / del → spans, br → line break. Images: see
// HtmlOptions::images.
void html_append_blocks(Document& doc, NodeId parent, std::string_view html,
                        const HtmlOptions& opt = {});

// `html` as a Chrome node under `parent` (`role` = what it is, kept in
// `summary`); INVALID_NODE and nothing added when it has no blocks.
NodeId html_append_chrome(Document& doc, NodeId parent, std::string role,
                          std::string_view html, const HtmlOptions& opt);

// Tags stripped, entities decoded, whitespace collapsed and trimmed.
std::string html_to_plain(std::string_view html);

// Value of attribute `name` inside a tag's attribute text
// (`href="x" class=y`), entities decoded; empty when absent.
std::string html_attr(std::string_view attrs, std::string_view name);
bool html_has_attr(std::string_view attrs, std::string_view name);

// Image URL of an <img>'s attributes: the srcset (or data-srcset) pick,
// else src, else the lazy-loading data-src / data-lazy-src / data-original
// (data: placeholders skipped); unresolved, empty when there is none.
std::string html_img_src(std::string_view attrs);
// Best candidate of a srcset for a figure: the largest width descriptor up
// to 1600w (else the smallest above), else the largest density up to 2x;
// empty when it has no usable (non-data:) entry.
std::string html_srcset_pick(std::string_view srcset);

// Index of the '>' closing the tag that starts at `lt` (quote-aware);
// npos when unterminated.
size_t html_tag_end(std::string_view html, size_t lt);

// Index just past the element whose open tag ends at `after_open`
// (nesting of the same tag counted); html.size() when unclosed.
size_t html_element_end(std::string_view html, size_t after_open,
                        std::string_view tag);

// Share of the visible text that is inside links (0..1; 0 without text).
double html_link_share(std::string_view html);

// Whether the element `tag` (attributes `attrs`, content from `open`) is a
// box of links to other pages ("Related Links", "You May Also Like"): its
// class / id or its first heading say so and at least half its text is
// links. Page mode makes such boxes Chrome with the role "related".
bool is_related_block(std::string_view html, size_t open,
                      std::string_view tag, std::string_view attrs);

// Resolve `href` against `base` (absolute, //host, /path, ?q, #f, relative).
std::string resolve_url(std::string_view base, std::string_view href);

}  // namespace xanatoo
