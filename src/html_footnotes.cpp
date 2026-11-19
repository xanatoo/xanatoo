// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/html_footnotes.hpp"

#include "xanatoo/util.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace xanatoo {

namespace {

bool is_alnum(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
         (c >= 'A' && c <= 'Z');
}

// Leading blanks, line breaks and back arrows ("↑", "↩", "^").
std::string_view skip_back_marks(std::string_view s) {
  for (;;) {
    if (!s.empty() && (s[0] == ' ' || s[0] == '\t' || s[0] == '\n' ||
                       s[0] == '^')) {
      s.remove_prefix(1);
    } else if (starts_with(s, "\xC2\xA0")) {               // no-break space
      s.remove_prefix(2);
    } else if (starts_with(s, "\xE2\x86\x91") ||           // ↑
               starts_with(s, "\xE2\x86\xA9") ||           // ↩
               starts_with(s, "\xE2\x80\x8B")) {           // zero-width space
      s.remove_prefix(3);
    } else {
      return s;
    }
  }
}

// The "[mark]" of a reference link: 1–16 bytes, no brackets inside.
bool bracket_mark(std::string_view text, std::string* mark) {
  trim_ascii(text);
  if (text.size() < 3 || text.front() != '[' || text.back() != ']')
    return false;
  std::string_view m = text.substr(1, text.size() - 2);
  trim_ascii(m);
  if (m.empty() || m.size() > 16 ||
      m.find_first_of("[]") != std::string_view::npos)
    return false;
  mark->clear();
  for (char c : m) {
    if (c == ' ' || c == '\t' || c == '\n') {
      if (!mark->empty() && mark->back() != '-') mark->push_back('-');
    } else {
      mark->push_back(c);
    }
  }
  return !mark->empty();
}

// A link at the start of a note that leads back to the text: an arrow,
// or a short "a" / "12" per reference (Wikipedia's "↑ a b").
bool is_back_link(const InlineSpan& sp, const std::string& own_label) {
  if (sp.kind != InlineKind::Link || sp.href.empty() || sp.href[0] != '#')
    return false;
  if (std::string_view(sp.href).substr(1) == own_label) return true;
  std::string_view t = sp.text;
  trim_ascii(t);
  if (t.empty()) return true;
  if (skip_back_marks(t).size() < t.size()) return true;
  if (t.size() > 3) return false;
  for (char c : t)
    if (!is_alnum(c)) return false;
  return true;
}

void strip_back_links(std::vector<InlineSpan>& spans,
                      const std::string& own_label) {
  size_t cut = 0;
  while (cut < spans.size()) {
    InlineSpan& sp = spans[cut];
    if (is_back_link(sp, own_label)) {
      ++cut;
      continue;
    }
    if (sp.kind == InlineKind::Text) {
      std::string_view rest = skip_back_marks(sp.text);
      if (rest.empty()) {
        ++cut;
        continue;
      }
      sp.text = std::string(rest);
    }
    break;
  }
  spans.erase(spans.begin(), spans.begin() + static_cast<std::ptrdiff_t>(cut));
}

// Grokipedia puts the source's host right after its title link
// ("Title…nist.gov"): set it apart.
void separate_hosts(std::vector<InlineSpan>& spans) {
  for (size_t i = 1; i < spans.size(); ++i) {
    InlineSpan& sp = spans[i];
    if (sp.kind != InlineKind::Text || spans[i - 1].kind != InlineKind::Link ||
        sp.text.empty() || !is_alnum(sp.text[0]))
      continue;
    std::string_view t = sp.text;
    trim_ascii(t);
    if (t.size() == sp.text.size() && t.find('.') != std::string_view::npos &&
        t.find_first_of(" \t\n") == std::string_view::npos)
      sp.text = " \xC2\xB7 " + sp.text;
  }
}

}  // namespace

int html_link_footnotes(Document& doc) {
  // Ordered-list items with an id: possible notes.
  std::unordered_map<std::string, NodeId> items;
  std::unordered_set<std::string> used;  // footnote keys taken
  for (const Node& n : doc.nodes) {
    if (n.id == INVALID_NODE) continue;
    if (n.kind == NodeKind::Footnote) used.insert(n.label);
    if (n.kind != NodeKind::ListItem || n.label.empty()) continue;
    const Node& list = doc.get(n.parent);
    if (list.id == n.parent && list.kind == NodeKind::List)
      items.emplace(n.label, n.id);
  }
  if (items.empty()) return 0;

  std::unordered_map<NodeId, std::string> key_of;
  auto unique_key = [&](std::string base) {
    std::string key = base;
    for (int i = 2; used.count(key); ++i) key = base + "-" + std::to_string(i);
    used.insert(key);
    return key;
  };

  // Reference marks. A mark may come as several adjacent spans of one link.
  for (Node& n : doc.nodes) {
    if (n.id == INVALID_NODE) continue;
    std::vector<InlineSpan>& spans = n.spans;
    for (size_t i = 0; i < spans.size(); ++i) {
      const InlineSpan& sp = spans[i];
      if (sp.kind != InlineKind::Link || sp.href.size() < 2 ||
          sp.href[0] != '#')
        continue;
      auto it = items.find(sp.href.substr(1));
      if (it == items.end()) continue;
      size_t end = i + 1;
      std::string text = sp.text;
      while (end < spans.size() && spans[end].kind == InlineKind::Link &&
             spans[end].href == sp.href)
        text += spans[end++].text;
      std::string mark;
      if (!bracket_mark(text, &mark)) continue;
      auto k = key_of.find(it->second);
      if (k == key_of.end())
        k = key_of.emplace(it->second, unique_key(std::move(mark))).first;
      InlineSpan ref;
      ref.kind = InlineKind::FootnoteRef;
      ref.href = k->second;
      ref.text = k->second;
      spans.erase(spans.begin() + static_cast<std::ptrdiff_t>(i + 1),
                  spans.begin() + static_cast<std::ptrdiff_t>(end));
      spans[i] = std::move(ref);
    }
  }
  if (key_of.empty()) return 0;

  // Lists holding a referenced item, in document order.
  std::vector<NodeId> lists;
  std::unordered_set<NodeId> seen;
  for (const auto& [item, key] : key_of) {
    const NodeId l = doc.get(item).parent;
    if (seen.insert(l).second) lists.push_back(l);
  }
  std::sort(lists.begin(), lists.end());

  std::unordered_map<std::string, std::string> renamed;  // old label → key
  int made = 0;
  for (NodeId l : lists) {
    const std::vector<NodeId> kids = doc.get(l).children;
    int pos = 0;
    for (NodeId c : kids) {
      if (doc.get(c).id != c || doc.get(c).kind != NodeKind::ListItem)
        continue;
      ++pos;
      auto k = key_of.find(c);
      std::string key = k != key_of.end()
                            ? k->second
                            : unique_key(std::to_string(pos));
      // The item's own text becomes the note's first paragraph.
      std::vector<InlineSpan> spans = std::move(doc.get(c).spans);
      doc.get(c).spans.clear();
      NodeId first = INVALID_NODE;
      if (!spans.empty()) {
        first = doc.create(NodeKind::Paragraph);
        doc.get(first).parent = c;
        doc.get(first).spans = std::move(spans);
        auto& ch = doc.get(c).children;
        ch.insert(ch.begin(), first);
      } else {
        for (NodeId p : doc.get(c).children)
          if (doc.get(p).id == p && doc.get(p).kind == NodeKind::Paragraph) {
            first = p;
            break;
          }
      }
      Node& fn = doc.get(c);
      if (!fn.label.empty()) renamed[fn.label] = key;
      if (first != INVALID_NODE) {
        strip_back_links(doc.get(first).spans, fn.label);
        separate_hosts(doc.get(first).spans);
      }
      fn.kind = NodeKind::Footnote;
      fn.level = 0;
      fn.label = std::move(key);
      ++made;
    }
    // The notes take the list's place.
    Node& list = doc.get(l);
    const NodeId parent = list.parent;
    std::vector<NodeId> moved = std::move(list.children);
    list.children.clear();
    list.parent = INVALID_NODE;
    list.id = INVALID_NODE;
    Node& p = doc.get(parent);
    for (size_t i = 0; i < p.children.size(); ++i) {
      if (p.children[i] != l) continue;
      p.children.erase(p.children.begin() + static_cast<std::ptrdiff_t>(i));
      p.children.insert(p.children.begin() + static_cast<std::ptrdiff_t>(i),
                        moved.begin(), moved.end());
      break;
    }
    for (NodeId c : moved) doc.get(c).parent = parent;
  }

  // Other links to a note ("see note 3") follow it into the footnote space.
  for (Node& n : doc.nodes) {
    if (n.id == INVALID_NODE) continue;
    for (InlineSpan& sp : n.spans) {
      if (sp.kind != InlineKind::Link || sp.href.size() < 2 || sp.href[0] != '#')
        continue;
      auto it = renamed.find(sp.href.substr(1));
      if (it != renamed.end()) sp.href = footnote_href(it->second);
    }
  }
  return made;
}

}  // namespace xanatoo
