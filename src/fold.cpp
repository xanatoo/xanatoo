// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Ingo Ruhnke <grumbel@gmail.com>
#include "xanatoo/fold.hpp"

#include "xanatoo/util.hpp"

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
                      const Folds* folds) {
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

NodeId stretch_summary(const Document& doc, NodeId stretch) {
  if (!valid(doc, stretch)) return INVALID_NODE;
  const Node& n = doc.get(stretch);
  if (n.kind != NodeKind::Stretch) return INVALID_NODE;
  for (NodeId c : n.children)
    if (valid(doc, c) && doc.get(c).kind == NodeKind::Summary) return c;
  return INVALID_NODE;
}

bool stretch_folded(const Document& doc, NodeId stretch, const Folds* folds) {
  if (!valid(doc, stretch) || doc.get(stretch).kind != NodeKind::Stretch)
    return false;
  if (folds) {
    const auto it = folds->find(stretch);
    if (it != folds->end()) return it->second;
  }
  return !doc.get(stretch).expanded;
}

std::vector<NodeId> stretches(const Document& doc, NodeId root, bool two_body) {
  std::vector<NodeId> out;
  std::vector<NodeId> stack{root};
  while (!stack.empty()) {
    const NodeId id = stack.back();
    stack.pop_back();
    if (!valid(doc, id)) continue;
    const Node& n = doc.get(id);
    if (n.kind == NodeKind::Stretch &&
        (!two_body || stretch_summary(doc, id) != INVALID_NODE))
      out.push_back(id);
    stack.insert(stack.end(), n.children.rbegin(), n.children.rend());
  }
  return out;
}

bool has_two_body_stretch(const Document& doc, NodeId root) {
  std::vector<NodeId> stack{root};
  while (!stack.empty()) {
    const NodeId id = stack.back();
    stack.pop_back();
    if (!valid(doc, id)) continue;
    if (stretch_summary(doc, id) != INVALID_NODE) return true;
    const Node& n = doc.get(id);
    stack.insert(stack.end(), n.children.rbegin(), n.children.rend());
  }
  return false;
}

NodeId stretch_first_text(const Document& doc, NodeId stretch, bool folded) {
  const NodeId summary = stretch_summary(doc, stretch);
  std::vector<NodeId> text;
  if (folded) {
    if (summary != INVALID_NODE) collect_text_nodes(doc, summary, text);
  } else if (valid(doc, stretch)) {
    for (NodeId c : doc.get(stretch).children)
      if (c != summary && text.empty()) collect_text_nodes(doc, c, text);
  }
  return text.empty() ? INVALID_NODE : text.front();
}

std::vector<NodeId> hidden_stretch_blocks(const Document& doc,
                                          const Folds* folds,
                                          bool summaries_only) {
  std::vector<NodeId> out;
  for (NodeId id : stretches(doc, doc.root, true)) {
    const NodeId summary = stretch_summary(doc, id);
    if (!stretch_folded(doc, id, folds)) {
      out.push_back(summary);
    } else if (!summaries_only) {
      for (NodeId c : doc.get(id).children)
        if (c != summary) out.push_back(c);
    }
  }
  return out;
}

bool node_folded(const Document& doc, NodeId id, const Folds* folds) {
  return list_item_folded(doc, id, folds) || stretch_folded(doc, id, folds);
}

NodeId fold_hiding(const Document& doc, NodeId node, const Folds* folds) {
  if (!valid(doc, node)) return INVALID_NODE;
  NodeId below = node;
  for (NodeId id = doc.get(node).parent; valid(doc, id);
       below = id, id = doc.get(id).parent) {
    if (list_item_folded(doc, id, folds)) return id;
    if (doc.get(id).kind != NodeKind::Stretch) continue;
    const bool in_summary = below == stretch_summary(doc, id);
    if (stretch_folded(doc, id, folds) != in_summary) return id;
  }
  return INVALID_NODE;
}

std::optional<NodeId> hit_test_fold(const LayoutResult& layout, float sx,
                                         float sy, float scroll_y) {
  const float y = sy + scroll_y;
  for (const LayoutBox& b : layout.fold_marks)
    if (sx >= b.x && sx < b.x + b.width && y >= b.y && y < b.y + b.height)
      return b.node;
  return std::nullopt;
}

}  // namespace xanatoo
