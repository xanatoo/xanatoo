// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/types.hpp"
#include <stdexcept>
#include <cassert>
#include <algorithm>

namespace xanatoo {

NodeId Document::create(NodeKind kind, NodeId parent) {
  NodeId id = next_id++;
  Node n;
  n.id = id;
  n.kind = kind;
  n.parent = parent;
  nodes.push_back(std::move(n));
  if (parent != INVALID_NODE) {
    add_child(parent, id);
  }
  if (kind == NodeKind::Document) {
    root = id;
  }
  return id;
}

namespace {
// Stable sentinel so concurrent get() calls do not clobber each other's refs.
Node& invalid_node_mut() {
  static Node dummy = [] {
    Node n;
    n.id = INVALID_NODE;
    n.kind = NodeKind::Paragraph;
    n.parent = INVALID_NODE;
    return n;
  }();
  // Keep dummy inert even if a caller writes through a non-const ref.
  dummy.children.clear();
  dummy.spans.clear();
  dummy.id = INVALID_NODE;
  dummy.kind = NodeKind::Paragraph;
  dummy.parent = INVALID_NODE;
  return dummy;
}
const Node& invalid_node_const() {
  static const Node dummy = [] {
    Node n;
    n.id = INVALID_NODE;
    n.kind = NodeKind::Paragraph;
    n.parent = INVALID_NODE;
    return n;
  }();
  return dummy;
}
}  // namespace

Node& Document::get(NodeId id) {
  size_t idx = static_cast<size_t>(id - 1);
  if (id == INVALID_NODE || idx >= nodes.size())
    return invalid_node_mut();
  return nodes[idx];
}

const Node& Document::get(NodeId id) const {
  size_t idx = static_cast<size_t>(id - 1);
  if (id == INVALID_NODE || idx >= nodes.size())
    return invalid_node_const();
  return nodes[idx];
}

void Document::add_child(NodeId parent, NodeId child) {
  Node& p = get(parent);
  Node& c = get(child);
  if (p.id != parent || c.id != child) return;
  p.children.push_back(child);
  c.parent = parent;
}

void Document::append_text(NodeId node, const std::string& text,
                           InlineKind kind) {
  Node& n = get(node);
  if (n.id != node) return;
  InlineSpan span;
  span.kind = kind;
  span.text = text;
  n.spans.push_back(std::move(span));
}

bool Document::insert_after(NodeId after, NodeId child) {
  Node& after_n = get(after);
  if (after_n.id != after || after_n.parent == INVALID_NODE) return false;
  Node& parent = get(after_n.parent);
  if (parent.id != after_n.parent) return false;
  Node& ch = get(child);
  if (ch.id != child) return false;
  auto it = std::find(parent.children.begin(), parent.children.end(), after);
  if (it == parent.children.end()) return false;
  ch.parent = after_n.parent;
  parent.children.insert(it + 1, child);
  return true;
}

}  // namespace xanatoo

namespace xanatoo {

bool rel_has(std::string_view rel, std::string_view token) {
  size_t i = 0;
  while (i < rel.size()) {
    while (i < rel.size() && (rel[i] == ' ' || rel[i] == '\t')) ++i;
    size_t j = i;
    while (j < rel.size() && rel[j] != ' ' && rel[j] != '\t') ++j;
    if (j > i && rel.substr(i, j - i) == token) return true;
    i = j;
  }
  return false;
}

InlineSpan inline_image(std::string src, std::string alt, int width,
                        int height) {
  InlineSpan s;
  s.kind = InlineKind::Image;
  s.text = kInlineImageText;
  s.src = std::move(src);
  s.alt = std::move(alt);
  s.width = static_cast<uint16_t>(std::clamp(width, 0, 4096));
  s.height = static_cast<uint16_t>(std::clamp(height, 0, 4096));
  return s;
}

InlineSpan inline_math(std::string tex, bool display) {
  InlineSpan s;
  s.kind = InlineKind::Math;
  s.text = kInlineImageText;
  s.src = std::move(tex);
  s.display = display;
  return s;
}

NodeId thread_title(const Document& doc, NodeId thread) {
  const Node& t = doc.get(thread);
  if (t.id != thread || t.kind != NodeKind::Thread || t.children.empty())
    return INVALID_NODE;
  const NodeId first = t.children.front();
  return doc.get(first).kind == NodeKind::Header ? first : INVALID_NODE;
}

NodeId set_thread_title(Document& doc, NodeId thread, const std::string& title) {
  NodeId h = thread_title(doc, thread);
  if (title.empty()) {
    if (h != INVALID_NODE) {
      Node& t = doc.get(thread);
      t.children.erase(t.children.begin());
      doc.get(h).parent = INVALID_NODE;  // detached
    }
    return INVALID_NODE;
  }
  if (h == INVALID_NODE) {
    Node& t = doc.get(thread);
    if (t.id != thread || t.kind != NodeKind::Thread) return INVALID_NODE;
    h = doc.create(NodeKind::Header);
    Node& tt = doc.get(thread);
    tt.children.insert(tt.children.begin(), h);
    doc.get(h).parent = thread;
  }
  doc.get(h).spans.clear();
  doc.append_text(h, title);
  return h;
}

const std::string* image_link_href(const Node& node) {
  if (node.kind != NodeKind::Image || node.spans.size() != 1) return nullptr;
  const InlineSpan& sp = node.spans[0];
  const bool plain_link = sp.kind == InlineKind::Link && sp.text.empty() &&
      !sp.href.empty() && sp.marks == 0 && sp.role == InlineRole::Normal &&
      sp.rel.empty() && sp.datetime.empty() && sp.target == INVALID_NODE &&
      sp.src.empty() && sp.alt.empty() && !sp.width && !sp.height &&
      !sp.display;
  return plain_link ? &sp.href : nullptr;
}

void collect_unwritten_spans(const Document& doc, bool (*writes)(const Node&),
                             std::vector<NodeId>& out) {
  if (doc.root == INVALID_NODE) return;
  std::vector<NodeId> stack{doc.root};
  while (!stack.empty()) {
    const NodeId id = stack.back();
    stack.pop_back();
    const Node& n = doc.get(id);
    if (n.id != id) continue;
    if (!n.spans.empty() && !writes(n)) out.push_back(id);
    stack.insert(stack.end(), n.children.rbegin(), n.children.rend());
  }
}

}  // namespace xanatoo
