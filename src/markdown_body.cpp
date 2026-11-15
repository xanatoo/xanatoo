// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/markdown_body.hpp"

#include "xanatoo/html_min.hpp"
#include "xanatoo/parse.hpp"
#include "xanatoo/util.hpp"

#include <string>

namespace xanatoo {
namespace {

// Kinds plain Markdown (CommonMark + GFM + footnotes) produces.
bool plain_kind(NodeKind k) {
  switch (k) {
    case NodeKind::Heading:
    case NodeKind::Header:
    case NodeKind::Section:
    case NodeKind::Paragraph:
    case NodeKind::List:
    case NodeKind::ListItem:
    case NodeKind::Image:
    case NodeKind::BlockQuote:
    case NodeKind::HorizontalRule:
    case NodeKind::CodeBlock:
    case NodeKind::Table:
    case NodeKind::TableRow:
    case NodeKind::TableCell:
    case NodeKind::Footnote:
      return true;
    default:
      return false;
  }
}

bool local_target(std::string_view url) {
  return starts_with_ci(url, "file:") || starts_with(url, "/") ||
         starts_with(url, "\\") || starts_with(url, "~");
}

// Absolute URL for `href`, "" when it must not be followed.
std::string foreign_url(std::string_view base, std::string_view href) {
  if (href.empty() || href[0] == '#') return std::string(href);
  std::string url = base.empty() ? std::string(href) : resolve_url(base, href);
  if (local_target(url)) return {};
  return url;
}

void clean_spans(std::vector<InlineSpan>& spans, std::string_view base) {
  for (InlineSpan& s : spans) {
    s.target = INVALID_NODE;  // node ids of the scratch document
    switch (s.kind) {
      case InlineKind::Math: {
        const char* d = s.display ? "$$" : "$";
        s.text = d + s.src + d;
        s.src.clear();
        s.kind = InlineKind::Text;
        s.display = false;
        break;
      }
      case InlineKind::Cite:
        if (s.text.empty()) s.text = "[@" + s.href + "]";
        s.kind = InlineKind::Text;
        s.href.clear();
        break;
      case InlineKind::Link:
        s.href = foreign_url(base, s.href);
        if (s.href.empty()) s.kind = InlineKind::Text;
        break;
      case InlineKind::Image:
        s.src = foreign_url(base, s.src);
        if (!s.href.empty()) s.href = foreign_url(base, s.href);
        if (s.src.empty()) {
          s.kind = InlineKind::Text;
          s.text = s.alt;
        }
        break;
      default:
        break;
    }
  }
}

void copy_clean(const Document& src, NodeId id, Document& dst, NodeId parent,
                std::string_view base) {
  const Node& n = src.get(id);
  if (n.kind == NodeKind::Page) return;  // a local fixed-layout file
  if (n.kind == NodeKind::Transclude) {
    NodeId p = dst.create(NodeKind::Paragraph, parent);
    dst.append_text(p, ":::include " + n.src);
    return;
  }
  NodeId into = parent;
  if (plain_kind(n.kind) || !n.spans.empty()) {
    const bool keep = plain_kind(n.kind);
    if (n.kind == NodeKind::Image) {
      const std::string url = foreign_url(base, n.src);
      if (url.empty()) {
        if (!n.alt.empty()) {
          NodeId p = dst.create(NodeKind::Paragraph, parent);
          dst.append_text(p, n.alt);
        }
        return;
      }
    }
    into = dst.create(keep ? n.kind : NodeKind::Paragraph, parent);
    Node& c = dst.get(into);
    c.spans = n.spans;
    clean_spans(c.spans, base);
    if (keep) {
      c.level = n.level;
      c.col_span = n.col_span;
      c.row_span = n.row_span;
      c.alt = n.alt;
      c.src = n.kind == NodeKind::Image ? foreign_url(base, n.src) : n.src;
      c.summary = n.summary;
      c.float_side = n.float_side;
      c.display_scale = n.display_scale;
      if (n.kind == NodeKind::Footnote) c.label = n.label;
    }
    // A non-plain block with text became a paragraph; its children follow
    // it as siblings.
    if (!keep) into = parent;
  }
  for (NodeId ch : n.children) copy_clean(src, ch, dst, into, base);
}

}  // namespace

void append_foreign_markdown(Document& doc, NodeId parent, std::string_view md,
                             std::string_view base_url) {
  // The leading newline keeps a "---" first line from being front matter.
  std::string source = "\n";
  source.append(md);
  const Document sub = parse_markup(source);
  if (sub.root == INVALID_NODE) return;
  for (NodeId ch : sub.get(sub.root).children)
    copy_clean(sub, ch, doc, parent, base_url);
}

}  // namespace xanatoo
