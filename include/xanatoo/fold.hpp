// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Folding: every list item that hosts a list can be folded to its own line,
// and stretchtext shows either its collapsed form (a label, or the blocks of
// its Summary) or its full text. The fold state is view state, not content:
// an override map per buffer (DocBuffer::folds) on top of the node's own
// default (Node::expanded, the `:expanded #f` / `[+]` markup).

#include "xanatoo/layout.hpp"
#include "xanatoo/types.hpp"

#include <optional>
#include <unordered_map>
#include <vector>

namespace xanatoo {

using Folds = std::unordered_map<NodeId, bool>;  // node → folded

// A ListItem with a non-empty nested List.
bool list_item_foldable(const Document& doc, NodeId item);
// Folded according to `folds` (may be null), else the item's markup.
bool list_item_folded(const Document& doc, NodeId item, const Folds* folds);
// Items hidden when `item` is folded (all levels).
size_t list_item_descendants(const Document& doc, NodeId item);
// The outermost List around `node` (itself included), or INVALID_NODE.
NodeId outermost_list(const Document& doc, NodeId node);
// The ListItem `node` is (in), innermost; or INVALID_NODE.
NodeId enclosing_list_item(const Document& doc, NodeId node);
// Every foldable item at or below `root`, in document order.
std::vector<NodeId> foldable_list_items(const Document& doc, NodeId root);
// Whether there is any (asked every frame for the context menu).
bool has_foldable_list_items(const Document& doc, NodeId root);

// Two-body stretchtext: the Summary child that stands for the full text
// while folded, or INVALID_NODE (classic stretchtext, not a Stretch).
NodeId stretch_summary(const Document& doc, NodeId stretch);
// Folded according to `folds` (may be null), else the node's markup.
bool stretch_folded(const Document& doc, NodeId stretch, const Folds* folds);
// Stretches at or below `root`, in document order; only those with a Summary
// when `two_body`.
std::vector<NodeId> stretches(const Document& doc, NodeId root, bool two_body);
bool has_two_body_stretch(const Document& doc, NodeId root);
// The text node the stretch shows first in state `folded`, or INVALID_NODE
// (a classic stretch shows only its label).
NodeId stretch_first_text(const Document& doc, NodeId stretch, bool folded);
// The half of every two-body stretchtext that is out of sight; only
// Summaries when `summaries_only` (find opens folded full text, but leaves
// the summary of a text it already shows alone).
std::vector<NodeId> hidden_stretch_blocks(const Document& doc,
                                          const Folds* folds,
                                          bool summaries_only);

// Either kind of fold.
bool node_folded(const Document& doc, NodeId id, const Folds* folds);
// The innermost list item or stretch whose state hides `node` (an item's own
// text stays visible), or INVALID_NODE. Flipping its state shows the node.
NodeId fold_hiding(const Document& doc, NodeId node, const Folds* folds);

// The fold marker (▾ / ▸, the "… 5 items" of a folded item, the "Full Text"
// cue of a summary) at a point of the view; LayoutResult::fold_marks.
std::optional<NodeId> hit_test_fold(const LayoutResult& layout, float sx,
                                    float sy, float scroll_y);

}  // namespace xanatoo
