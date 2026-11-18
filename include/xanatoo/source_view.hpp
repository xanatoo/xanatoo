// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include "xanatoo/locator.hpp"
#include "xanatoo/types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xanatoo {

struct SourceViewText {
  std::string text;
  std::string language;
  // Non-empty when the text is incomplete: the document tree holds content
  // the format cannot express (invalid structure, e.g. text on a section).
  std::string warning;
};

// A source pipe projects a document into code text. Raw is the input text,
// when available; sexpr/XML serialize the *current* semantic tree. No parser
// or document content is modified. An absent raw input gives a visible notice.
SourceViewText source_view_text(const Document& doc, SelectorKind kind,
                                std::optional<std::string_view> raw);

// A code block (not reparsed as Markdown) keeps markup characters literal;
// a warning comes first, as a warning note.
Document source_view_document(const SourceViewText& source);

// The warning for nodes whose spans a serializer left out (empty if none).
std::string unwritten_spans_warning(const Document& doc,
                                    const std::vector<NodeId>& nodes);

}  // namespace xanatoo
