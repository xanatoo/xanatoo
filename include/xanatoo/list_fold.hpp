// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Folding of nested lists: every list item that hosts a list can be folded
// to its own line. The fold state is view state, not content: an override
// map per buffer (DocBuffer::list_folds) on top of the item's own default
// (Node::expanded, the `:expanded #f` / `[+]` markup).

#include "xanatoo/layout.hpp"
#include "xanatoo/types.hpp"

#include <optional>
#include <unordered_map>
#include <vector>

namespace xanatoo {

using ListFolds = std::unordered_map<NodeId, bool>;  // item → folded

// A ListItem with a non-empty nested List.
bool list_item_foldable(const Document& doc, NodeId item);
// Folded according to `folds` (may be null), else the item's markup.
bool list_item_folded(const Document& doc, NodeId item, const ListFolds* folds);
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
// The innermost folded item whose fold hides `node` (an item's own text
// stays visible), or INVALID_NODE.
NodeId folded_list_ancestor(const Document& doc, NodeId node,
                            const ListFolds* folds);

// The fold marker (▾ / ▸, or the "… 5 items" of a folded item) at a point of
// the view; LayoutResult::list_folds.
std::optional<NodeId> hit_test_list_fold(const LayoutResult& layout, float sx,
                                         float sy, float scroll_y);

}  // namespace xanatoo
