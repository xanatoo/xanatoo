// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Ingo Ruhnke <grumbel@gmail.com>
#include "xanatoo/list_fold.hpp"

#include <algorithm>

namespace xanatoo {

namespace {

bool valid(const Document& doc, NodeId id) {
  return id != INVALID_NODE && id < doc.next_id && doc.get(id).id == id;
}

}  // namespace

bool list_item_foldable(const Document& doc, NodeId item) {
  if (!valid(doc, item)) return false;
  const Node& n = doc.get(item);
  if (n.kind != NodeKind::ListItem) return false;
  for (NodeId c : n.children)
    if (valid(doc, c) && doc.get(c).kind == NodeKind::List &&
        !doc.get(c).children.empty())
      return true;
  return false;
}

bool list_item_folded(const Document& doc, NodeId item,
                      const ListFolds* folds) {
  if (!list_item_foldable(doc, item)) return false;
  if (folds) {
    const auto it = folds->find(item);
    if (it != folds->end()) return it->second;
  }
  return !doc.get(item).expanded;
}

size_t list_item_descendants(const Document& doc, NodeId item) {
  size_t count = 0;
  std::vector<NodeId> stack{item};
  while (!stack.empty()) {
    const NodeId id = stack.back();
    stack.pop_back();
    if (!valid(doc, id)) continue;
    const Node& n = doc.get(id);
    if (id != item && n.kind == NodeKind::ListItem) ++count;
    stack.insert(stack.end(), n.children.begin(), n.children.end());
  }
  return count;
}

NodeId outermost_list(const Document& doc, NodeId node) {
  NodeId best = INVALID_NODE;
  for (NodeId id = node; valid(doc, id); id = doc.get(id).parent) {
    const NodeKind k = doc.get(id).kind;
    if (k == NodeKind::List)
      best = id;
    else if (k != NodeKind::ListItem)
      break;
  }
  return best;
}

NodeId enclosing_list_item(const Document& doc, NodeId node) {
  for (NodeId id = node; valid(doc, id); id = doc.get(id).parent)
    if (doc.get(id).kind == NodeKind::ListItem) return id;
  return INVALID_NODE;
}

std::vector<NodeId> foldable_list_items(const Document& doc, NodeId root) {
  std::vector<NodeId> out;
  std::vector<NodeId> stack{root};
  while (!stack.empty()) {
    const NodeId id = stack.back();
    stack.pop_back();
    if (!valid(doc, id)) continue;
    if (list_item_foldable(doc, id)) out.push_back(id);
    const Node& n = doc.get(id);
    stack.insert(stack.end(), n.children.rbegin(), n.children.rend());
  }
  return out;
}

bool has_foldable_list_items(const Document& doc, NodeId root) {
  std::vector<NodeId> stack{root};
  while (!stack.empty()) {
    const NodeId id = stack.back();
    stack.pop_back();
    if (!valid(doc, id)) continue;
    if (list_item_foldable(doc, id)) return true;
    const Node& n = doc.get(id);
    stack.insert(stack.end(), n.children.rbegin(), n.children.rend());
  }
  return false;
}

NodeId folded_list_ancestor(const Document& doc, NodeId node,
                            const ListFolds* folds) {
  if (!valid(doc, node)) return INVALID_NODE;
  for (NodeId id = doc.get(node).parent; valid(doc, id);
       id = doc.get(id).parent)
    if (list_item_folded(doc, id, folds)) return id;
  return INVALID_NODE;
}

std::optional<NodeId> hit_test_list_fold(const LayoutResult& layout, float sx,
                                         float sy, float scroll_y) {
  const float y = sy + scroll_y;
  for (const LayoutBox& b : layout.list_folds)
    if (sx >= b.x && sx < b.x + b.width && y >= b.y && y < b.y + b.height)
      return b.node;
  return std::nullopt;
}

}  // namespace xanatoo
