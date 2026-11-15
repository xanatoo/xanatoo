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
};

// A source pipe projects a document into code text. Raw is the input text,
// when available; sexpr/XML serialize the *current* semantic tree. No parser
// or document content is modified. An absent raw input gives a visible notice.
SourceViewText source_view_text(const Document& doc, SelectorKind kind,
                                std::optional<std::string_view> raw);

// Code blocks (not reparsed as Markdown) keep markup characters literal.
// Long sources are split by append_code_blocks.
Document source_view_document(const SourceViewText& source);

// Size limits of one block made by append_code_blocks.
inline constexpr size_t kCodeBlockLines = 200;
inline constexpr size_t kCodeBlockBytes = 16 * 1024;

// Appends `text` to `parent` as CodeBlocks of at most kCodeBlockLines lines
// / kCodeBlockBytes bytes, so a huge source (a 10 MB page) stays a list of
// small blocks the layout can virtualise instead of one block laid out in
// full on every relayout. Blocks end at a line end and the boundary stands
// for that newline; a line longer than the limit is cut at a UTF-8
// boundary. Text within the limits becomes one block, unchanged.
std::vector<NodeId> append_code_blocks(Document& doc, NodeId parent,
                                       std::string_view text);

}  // namespace xanatoo
