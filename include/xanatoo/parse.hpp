// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include "xanatoo/types.hpp"
#include <string>
#include <string_view>
#include <vector>

namespace xanatoo {

// Minimal Markdown-ish parser for the proposal sample syntax.
// Supports:
//   # Heading
//   ## Heading 2
//   paragraphs (blank-line separated)
//   *emphasis*  **strong**
//   [link text](#target) or [link text](url)
//   :::stretch summary="..." ... :::
//     (a :::summary ... ::: in it is shown instead of the rest while
//     collapsed)
//   - item  (nested by indentation; "- [ ] " / "- [x] " task items;
//     "- [+] " an item folded by default, see fold.hpp)
//   :::columns / :::column  (containers)
//   :::include #Anchor | path.md#Anchor
//   ![alt](src)
//
// Not a full CommonMark implementation — deliberately small.
Document parse_markup(std::string_view source);

// Serialize a document back to the same lightweight markup dialect.
// Round-trips the structures we parse; unknown/richer structure is
// flattened conservatively.
std::string serialize_markup(const Document& doc);

// --- S-expression format (read only) ------------------------------------
//
// The older nested form of the document model. Xanatoo still reads it
// (.sexp files, revisions stored before XML became the canonical format)
// but no longer writes it: XML is the one complete serialization. E.g.:
//
//   (doc
//     (h 1 "Title")
//     (p "plain " (em "emph") " " (strong "bold") " "
//        (code "x") " " (a "#anchor" "link"))
//     (stretch :summary "What?" :expanded #f
//       (p "body"))
//     (stretch :expanded #f
//       (summary (p "brief"))   ; shown instead of the rest while collapsed
//       (p "full text"))
//     (columns
//       (column :weight 1.5 (p "left"))
//       (column (p "right")))
//     (img "pic.png" :alt "Alt" :scale 1.2)
//     (include "#Heading")
//     (include "other.md#Section")
//     (ul (li "bullet") (li "parent" (ul (li "nested"))))
//     (ul (li :expanded #f "folded" (ul (li "nested"))))
//     (ol (li 1 "one") (li 2 "two"))
//     (quote "quoted")
//     (hr))
//
// Keywords: :summary :expanded :weight :scale :alt
// Booleans: #t #f   Strings: "…" with \\ \" \n \t
// Line comments: ; …
//
// File extensions that prefer sexpr: .sexp .glyph .ss
// Content that starts with '(' (after whitespace/comments) also parses
// as sexpr when loaded.

bool path_uses_sexpr(std::string_view path);
bool looks_like_sexpr(std::string_view source);

Document parse_sexpr(std::string_view source);

// The document store's bytes (revisions, drafts, imported originals): XML,
// or S-expressions in revisions written before XML became canonical.
Document parse_stored_document(std::string_view data);
std::string serialize_stored_document(const Document& doc);

// --- Native XML document format (canonical) ------------------------------
//
// .xml is xanatoo's canonical serialization of the semantic tree (every
// field the model has; the document store writes it too). It is not a
// general-purpose HTML/XML importer. Only elements, quoted attributes, and the five predefined XML
// entities plus numeric character references are supported. In particular,
// declarations, DTDs, namespaces, comments, CDATA and processing instructions
// are rejected. Invalid input throws std::invalid_argument (with byte offset).
// See docs/XML.md for the exact vocabulary and round-trip rules.
bool path_uses_xml(std::string_view path);
Document parse_xml(std::string_view source);
// The element name a node is written as ("section", "image", …).
const char* xml_element_name(const Node& node);
// `unwritten`, when given, receives the nodes whose inline spans the format
// cannot hold (text on a container, as left by an importer) and so are not
// in the output. Callers should warn about them rather than lose them
// silently. Text XML cannot hold (malformed UTF-8, control characters)
// throws std::invalid_argument, or with XmlInvalid::Replace is written as
// U+FFFD (the document store, which must not fail on a page's odd bytes).
enum class XmlInvalid : uint8_t { Throw, Replace };
std::string serialize_xml(const Document& doc,
                          std::vector<NodeId>* unwritten = nullptr,
                          XmlInvalid invalid = XmlInvalid::Throw);

// --- Gemini gemtext -------------------------------------------------------
//
// Line-oriented text/gemini format (projectgemini.io):
//   # / ## / ###  headings
//   * item         unordered list
//   > quote
//   => url [label] link line
//   ```            preformatted toggle (optional alt text on opener)
//   plain lines    each non-empty line → paragraph
//
// Extensions .gmi / .gemini prefer gemtext. Content-type text/gemini from
// a Gemini response should call parse_gemtext directly.

bool path_uses_gemtext(std::string_view path);
bool looks_like_gemtext(std::string_view source);  // heuristic only

Document parse_gemtext(std::string_view source);

// Plain text (.txt) keeps its lines: blank-line separated chunks become
// code blocks (plain_text_document, html_page.hpp). The writer joins every
// block's text with blank lines, so a .txt round-trips as text, never as
// Markdown.
bool path_uses_plain_text(std::string_view path);
std::string serialize_plain_text(const Document& doc);
// Best-effort export (lossy vs full Markdown model).
std::string serialize_gemtext(const Document& doc);

// Dispatch: native XML by .xml path; sexpr by content ('(' first), gemtext,
// org, HTML by path; otherwise Markdown-ish markup. A .sexp path that does
// not hold an S-expression is XML (the drafts of a .sexp document), and
// serialize_document writes XML for it: S-expressions are read only.
Document parse_document(std::string_view source, std::string_view path = {});
std::string serialize_document(const Document& doc, std::string_view path = {});

}  // namespace xanatoo
