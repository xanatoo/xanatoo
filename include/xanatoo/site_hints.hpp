// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Site hints: small S-expression sheets (sites/*.xsite) that tell the web
// page reader what the parts of a page *are* when its markup does not say
// (no <main>, comments as plain nested lists, chrome without landmarks).
// They map foreign HTML onto xanatoo's own roles; they never style
// anything (that stays with the .xss style sheets). docs/SITEHINTS.md.
//
//   (site "slashdot.org"
//     (main "article.fhitem-story" "#commentlisting")
//     (chrome sidebar "#slashboxes")
//     (drop ".modal-content")
//     (content "nav.story-tools")
//     (thread "#commentlisting"
//       (heading "Comments")
//       (message "li.comment")
//       (title ".title a")
//       (author ".by a")
//       (date ".otherdetails" "on (.*@\\S+)")
//       (score ".score" "Score:\\s*([^)]*)")
//       (body ".commentBody")))

#include "xanatoo/types.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace xanatoo {

// One compound selector: tag, #id, .class…, [attr] / [attr=value]; empty
// parts match anything ("*" is the empty tag).
struct HintCompound {
  std::string tag;
  std::string id;
  std::vector<std::string> classes;
  std::vector<std::pair<std::string, std::string>> attrs;  // value "" = present
};
// "a b c": c inside b inside a (descendant combinator only).
struct HintSelector {
  std::vector<HintCompound> chain;
  bool empty() const { return chain.empty(); }
};
HintSelector parse_hint_selector(std::string_view s, std::string* err = nullptr);

// A message field: the first matching element inside the message (not in
// a reply), its plain text cut by an optional regex (group 1, else all).
struct HintField {
  HintSelector sel;
  std::string regex;
};

enum class HintKind {
  Main,     // the content (several: in page order, the rest is chrome)
  Chrome,   // page chrome with `role` (reader mode hides it)
  Content,  // not chrome, whatever the heuristics say
  Drop,     // left out entirely
  Thread,   // a discussion: messages → Thread / Message nodes
};

struct HintThread {
  std::string heading;  // Thread title ("Comments"); empty = none
  HintSelector message;
  HintField title, author, date, score;
  HintSelector body;
};

struct HintRule {
  HintKind kind = HintKind::Main;
  std::vector<HintSelector> sels;
  std::string role;   // Chrome
  HintThread thread;  // Thread
};

struct SiteHints {
  std::vector<std::string> hosts;  // "slashdot.org" also matches subdomains
  std::vector<HintRule> rules;
  std::string source;  // file it came from (messages)
};

// Every (site …) form of a sheet; problems go to `errors` (one per line,
// prefixed with `name`), the rest still loads.
std::vector<SiteHints> parse_site_hints(std::string_view src,
                                        std::string_view name,
                                        std::string* errors = nullptr);

// The hints for `host` ("tech.slashdot.org"), or null: sheets from
// data_dir()/sites, then user_data_dir()/sites (a later site naming the
// same host wins). Reread when a file changes; thread-safe.
std::shared_ptr<const SiteHints> site_hints_for_host(std::string_view host);

bool site_hints_match_host(const SiteHints& h, std::string_view host);

// ---- Applying hints to one page (html_page.cpp) -------------------------

// One element of a tolerant parse (implied closes as browsers do them for
// p / li / dt / dd / tr / td / th / option, stray end tags ignored, a close
// tag ends whatever is open inside it). Offsets into the page source.
struct HtmlElem {
  size_t lt = 0;     // '<' of the start tag
  size_t open = 0;   // just past its '>'
  size_t close = 0;  // start of the end tag (= end when implied)
  size_t end = 0;    // just past the end tag
  int parent = -1;
  std::string tag;
  std::string_view attrs;
};
std::vector<HtmlElem> html_elements(std::string_view html);

bool hint_matches(const HintSelector& sel, const std::vector<HtmlElem>& els,
                  int e);

struct HtmlOptions;  // html_min.hpp

// Hints bound to one page source. `html` must outlive it; every view of the
// page html_min sees must point into that same buffer.
struct PageHints {
  std::string_view html;
  std::vector<HtmlElem> els;
  // Per element: index of the rule that claims it, or -1.
  std::vector<int> rule_of;
  const SiteHints* site = nullptr;
  std::vector<int> main;  // outermost Main elements in page order
};
PageHints bind_site_hints(const SiteHints& site, std::string_view html);

// Point `opt`'s per-tag hook at `page` (drop / chrome / content / thread).
void attach_page_hints(HtmlOptions& opt, const PageHints& page);

}  // namespace xanatoo
