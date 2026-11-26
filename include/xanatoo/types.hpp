// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <optional>

namespace xanatoo {

using NodeId = uint64_t;
constexpr NodeId INVALID_NODE = 0;

enum class NodeKind : uint8_t {
  Document,
  Heading,           // legacy ATX depth (prefer Header + Section)
  Header,            // unranked semantic title (XML <h>, not layout <header>)
  Article,           // self-contained composition (HTML <article>)
  Section,           // thematic grouping of blocks (HTML <section>)
  Paragraph,
  List,
  ListItem,
  Image,
  Stretch,   // expandable block
  Columns,
  Column,
  BlockQuote,      // Markdown "> …" indented quotation
  HorizontalRule,  // Markdown "---" thematic break
  Transclude,      // :::include #Target — live embedding of a passage
  CodeBlock,       // semantic preformatted code (:lang for language)
  Note,            // semantic aside / callout (not a layout box)
  DefList,         // definition list container
  Term,            // term in a definition list
  Definition,      // definition body for the preceding term
  Figure,          // media + caption as one semantic unit
  Caption,         // caption under a figure (child of Figure)
  Biblio,          // bibliography section (container)
  BiblioEntry,     // one reference; label/key matches cite keys
  Table,           // semantic grid (rows of cells; not pixel layout)
  TableRow,
  TableCell,       // level 0 = data cell, 1 = header cell
  Thread,          // discussion container (mailing list / forum thread)
  Message,         // one post or comment; may reply to another Message
  Footnote,        // note body (Paragraph children); label = key of its refs
  Collection,      // ordered set of items (Image / Figure) shown as cards
  Chrome,          // web page chrome (nav, aside, footer, …); reader mode hides it
  Aside,           // text box beside the flow (HTML <aside>, sidebar, pull quote)
  Page,            // fixed-layout page (PDF / DjVu / comic), drawn from tiles
  Summary,  // first child of a Stretch: the blocks shown instead of the rest
            // while it is folded
  Metadata,  // inline byline, provenance, or activity details for a composition
};

enum class InlineKind : uint8_t {
  Text,
  Emphasis,
  Strong,
  Strike,  // GFM ~~strikethrough~~
  Link,
  Code,
  Cite,  // citation pointer; href holds bibliography key
  FootnoteRef,  // footnote mark; href holds the Footnote key, text the mark
  Image,  // inline image: text = U+FFFC (one caret position), src / alt /
          // width / height below, href = optional link target
  Math,   // TeX math: text = U+FFFC, src = the TeX source, display = own
          // line (centred when alone in its block); see math.hpp
};

// Semantic meaning independent of the inline's behaviour (a metadata link
// remains a Link for navigation, but may use a quieter font/colour).
enum class InlineRole : uint8_t { Normal, Metadata };

// Spans that are one object in the text (U+FFFC, one caret position).
inline bool is_inline_object(InlineKind k) {
  return k == InlineKind::Image || k == InlineKind::Math;
}

// Text of an inline image span: the object replacement character.
inline constexpr const char* kInlineImageText = "\xEF\xBF\xBC";
inline constexpr uint32_t kInlineImageCp = 0xFFFC;

// Faces a span takes from enclosing markup, on top of its own kind: a link
// inside bold text is a Link marked kMarkStrong, ***x*** an Emphasis marked
// kMarkStrong. A Strong / Emphasis / Strike kind implies its own mark.
inline constexpr uint8_t kMarkStrong = 1;
inline constexpr uint8_t kMarkEmphasis = 2;
inline constexpr uint8_t kMarkStrike = 4;

// The mark a styling kind stands for (0 for every other kind).
inline uint8_t kind_mark(InlineKind k) {
  switch (k) {
    case InlineKind::Strong: return kMarkStrong;
    case InlineKind::Emphasis: return kMarkEmphasis;
    case InlineKind::Strike: return kMarkStrike;
    default: return 0;
  }
}

struct InlineSpan {
  InlineKind kind = InlineKind::Text;
  InlineRole role = InlineRole::Normal;
  uint8_t marks = 0;  // kMark* bits from enclosing markup (see above)
  std::string text;
  std::string href;   // Link target, or Cite key
  std::string rel;    // typed relation: glossary, source, contra, …
  std::string datetime;  // ISO 8601 instant for semantic <time> / dated links
  NodeId target = INVALID_NODE;
  // InlineKind::Image only.
  std::string src;      // image path / URL
  std::string alt;      // alternative text
  uint16_t width = 0;   // declared size in CSS px (0 = from the image)
  uint16_t height = 0;
  // InlineKind::Math only (the TeX source is in `src`).
  bool display = false;
};

// Typed relations of social posts (InlineSpan::rel, docs/XML.md "Social
// markup"). They label what a service already marks; plain "@word" /
// "#word" text is never guessed into one.
//   mention: a Link to an account (href = the service's profile ref)
//   tag:     a Link to a hashtag (href = that service's tag feed)
//   score:   a Metadata text span counting likes / points / replies, a
//            snapshot from when the page was fetched
inline constexpr std::string_view kRelMention = "mention";
inline constexpr std::string_view kRelTag = "tag";
inline constexpr std::string_view kRelScore = "score";

// True when `rel` (space-separated tokens, as in HTML) holds `token`.
bool rel_has(std::string_view rel, std::string_view token);

// All faces of a span: its marks plus the one its kind implies.
inline uint8_t span_marks(const InlineSpan& s) {
  return static_cast<uint8_t>(s.marks | kind_mark(s.kind));
}

// Wrap `s` in the styling kind `k` (Strong / Emphasis / Strike): plain text
// takes the kind, anything else (a link, code, another style) the mark.
inline void add_mark(InlineSpan& s, InlineKind k) {
  if (s.kind == InlineKind::Text) s.kind = k;
  else if (s.kind != k) s.marks = static_cast<uint8_t>(s.marks | kind_mark(k));
}

// Inline image span (text = kInlineImageText).
InlineSpan inline_image(std::string src, std::string alt = {},
                        int width = 0, int height = 0);

// Inline math span (text = kInlineImageText, src = TeX).
InlineSpan inline_math(std::string tex, bool display = false);

struct Node {
  NodeId id = INVALID_NODE;
  NodeKind kind = NodeKind::Paragraph;
  NodeId parent = INVALID_NODE;
  std::vector<NodeId> children;
  std::vector<InlineSpan> spans;  // leaf text content
  int level = 0;                  // legacy Heading 1-6; List ordered flag; TableCell header
  int col_span = 1;              // TableCell: occupied columns
  int row_span = 1;              // TableCell: occupied rows
  std::string alt;                // image alt text
  std::string src;                // image path
  bool expanded = true;           // for Stretch
  std::string summary;            // stretchtext label (collapsed line)
  std::string float_side;        // image, figure or table: "left", "right", or flow
  // Table: placement of a table narrower than the column ("left", "center",
  //        "right"; empty = the style sheet's `table` align).
  // TableRow / TableCell: horizontal alignment of the cell text ("left",
  //        "center", "right", "decimal"; empty = row, then column, default).
  std::string align;
  // Table / TableRow / TableCell: vertical alignment of cell content in its
  // row ("top", "middle", "bottom", "baseline"; empty = inherited).
  std::string valign;
  // Table: column alignments, one letter per column (LaTeX-style
  // `l` / `c` / `r` / `d` = decimal point), e.g. "lrrd"; missing = `l`.
  std::string cols;
  float display_scale = 1.f;      // image interactive scale (1 = natural fit)
  int width = 0;                  // Page: intrinsic size in source pixels,
  int height = 0;                 //       as thumtoo reports it (never guessed)
                                  // Table: width in percent of the column
                                  //        (0 = as wide as its content)
  std::string label;              // stable semantic anchor (links: #label)
  std::string tags;               // org-style tags without outer colons ("work:urgent")
  std::string style;              // optional named .xss variant for this subtree
  // CodeBlock: language id in `src` (e.g. "cpp"); Note: optional kind in `src`
  // Transclude: summary holds optional :excerpt phrase (range within target)
  // TableCell: level 1 = header cell, 0 = body cell
  // TableRow: level 1 = head row (HTML thead), 2 = foot row (tfoot), 0 = body
  // Thread: label = thread id; summary = subject
  // Message: label = message-id; src = from/author; summary = date (ISO);
  //          alt = in-reply-to id, or "about:#anchor" for passage comments
  //          spans (optional) = Link spans for the header: [0] author,
  //          [1] date (e.g. profile / permalink, datetime = ISO instant),
  //          [2] reply. Spans with role=metadata and text are extra header
  //          fields after the date (rel="score": "42 points"), wherever
  //          they sit. Body blocks are children.
  // Header: text title (no rank); src may hold org TODO keyword; tags for org.
  // Footnote: label = key; body blocks are children (usually Paragraphs).
  // Collection: summary = default view hint ("grid", "list", "full");
  //             items are Image / Figure children (view-local layout).
  // Article/Section: containers grouping blocks.
  // Aside: body blocks are children; src = side hint ("left" / "right",
  //        empty = the style's); floats beside the following blocks.
  // Chrome: summary = what it is ("navigation", "sidebar", "page header",
  //         …); body blocks are children. Only HTML pages make these.
  // Page: src = thumtoo location of one page (`file:///a.pdf//page:3`,
  //       `file:///a.cbz//archive:001.jpg`), width / height its size
  //       (scale-0 tile grid), alt = page label, label = "page-N" anchor.
  //       A leaf; see docs/FIXED_LAYOUT.md.
};

// Document-level semantic metadata (not layout).
struct DocumentMeta {
  std::string title;
  std::string lang;   // BCP 47, e.g. "en"
  std::string author;
  // Markdown YAML front matter, verbatim (between the --- lines) so a save
  // keeps keys xanatoo does not read; title / author / lang come from it.
  std::string front_matter;
};

// Simple arena-style store: contiguous nodes, IDs are indices+1
struct Document {
  std::vector<Node> nodes;
  NodeId root = INVALID_NODE;
  NodeId next_id = 1;
  DocumentMeta meta;

  NodeId create(NodeKind kind, NodeId parent = INVALID_NODE);
  Node& get(NodeId id);
  const Node& get(NodeId id) const;
  void add_child(NodeId parent, NodeId child);
  void append_text(NodeId node, const std::string& text,
                   InlineKind kind = InlineKind::Text);

  // Insert `child` as a sibling immediately after `after` under the same parent.
  // Returns false if after has no parent or is not found among siblings.
  bool insert_after(NodeId after, NodeId child);
};

// A Thread's title is a Header as its first child (shown in the thread's
// band, listed by the Heading Outline). Returns it, or INVALID_NODE.
NodeId thread_title(const Document& doc, NodeId thread);
// Gives `thread` that title Header (created as the first child, or its text
// replaced) and returns it; an empty title removes it from the thread.
NodeId set_thread_title(Document& doc, NodeId thread, const std::string& title);

// An Image node's link target (an HTML <a> around a block <img>) is kept as
// one empty Link span. Returns its href, or nullptr when the image's spans
// are not exactly that. Both native formats store it as an `href`.
const std::string* image_link_href(const Node& node);

// Appends, in document order, the nodes reachable from the root that have
// spans which `writes` says a serializer leaves out.
void collect_unwritten_spans(const Document& doc, bool (*writes)(const Node&),
                             std::vector<NodeId>& out);

}  // namespace xanatoo
