// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/gallery.hpp"

#include "xanatoo/autolink.hpp"
#include "xanatoo/util.hpp"

#include <algorithm>
#include <charconv>

namespace xanatoo {

namespace {

std::string plain(const Node& n) {
  std::string t;
  for (const auto& sp : n.spans) t += sp.text;
  return t;
}

// "[image: alt]" labels (feeds, Nostr): the alt text; "[image]": nothing.
std::string link_caption(std::string text) {
  if (starts_with(text, "[image: ") && !text.empty() && text.back() == ']')
    return text.substr(8, text.size() - 9);
  if (starts_with(text, "[image")) return {};
  return text;
}

std::string image_caption(const Document& doc, const Node& image) {
  if (image.parent != INVALID_NODE) {
    const Node& parent = doc.get(image.parent);
    if (parent.kind == NodeKind::Figure)
      for (NodeId c : parent.children)
        if (doc.get(c).kind == NodeKind::Caption) {
          std::string t = plain(doc.get(c));
          if (!t.empty()) return t;
        }
  }
  return image.alt;
}

std::vector<uint32_t> code_points(std::string_view s) {
  std::vector<uint32_t> out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    const auto c = static_cast<unsigned char>(s[i]);
    const int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3
                  : (c >> 3) == 30 ? 4 : 1;
    uint32_t cp = len == 1 ? c : c & (0x7f >> len);
    for (int k = 1; k < len && i + k < s.size(); ++k)
      cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3f);
    out.push_back(cp);
    i += static_cast<size_t>(len);
  }
  return out;
}

// Bare image URLs in running text (shown as links by display-time
// autolinking, so they are image links to the reader too).
template <typename F>
void text_image_urls(const std::string& text, F&& f) {
  if (text.find("http") == std::string::npos) return;
  const std::vector<uint32_t> cps = code_points(text);
  for (const auto& [b, e] : find_urls(cps)) {
    std::string url;
    for (size_t i = b; i < e; ++i) autolink_append_utf8(url, cps[i]);
    if (is_image_url(url)) f(std::move(url));
  }
}

}  // namespace

std::vector<GalleryItem> gallery_items(const Document& doc, NodeId scope) {
  std::vector<GalleryItem> items;
  const auto add = [&](GalleryItem it) {
    if (it.src.empty()) return;
    if (std::any_of(items.begin(), items.end(),
                    [&](const GalleryItem& o) { return o.src == it.src; }))
      return;
    if (it.thumb.empty()) it.thumb = it.src;
    items.push_back(std::move(it));
  };
  const NodeId start = scope != INVALID_NODE ? scope : doc.root;
  if (start == INVALID_NODE) return items;
  std::vector<NodeId> stack{start};
  while (!stack.empty()) {
    const NodeId id = stack.back();
    stack.pop_back();
    if (id == INVALID_NODE || id >= doc.next_id) continue;
    const Node& n = doc.get(id);
    if (n.id != id) continue;
    if (n.kind == NodeKind::Image && !n.src.empty()) {
      // A linked original (thumbnail pages) is what the viewer shows.
      GalleryItem it{n.src, n.src, image_caption(doc, n), id};
      for (const auto& span : n.spans)
        if (span.kind == InlineKind::Link && is_image_url(span.href)) {
          it.src = span.href;
          break;
        }
      add(std::move(it));
    }
    for (const auto& s : n.spans) {
      if (s.kind == InlineKind::Image && !s.src.empty())
        // A thumbnail linking to its original (gallery pages put these in
        // list items, where images are inline): the viewer shows the
        // original, and a click on the link finds it by that URL.
        add({is_image_url(s.href) ? s.href : s.src, s.src,
             link_caption(s.alt), id});
      else if (s.kind == InlineKind::Link && is_image_url(s.href) &&
               n.kind != NodeKind::Image)
        add({s.href, s.href, link_caption(s.text), id});
      else if (s.kind != InlineKind::Link && s.kind != InlineKind::Code &&
               s.kind != InlineKind::Image && n.kind != NodeKind::CodeBlock)
        text_image_urls(s.text, [&](std::string url) {
          add({url, url, {}, id});
        });
    }
    stack.insert(stack.end(), n.children.rbegin(), n.children.rend());
  }
  return items;
}

Document gallery_document(const std::vector<GalleryItem>& items,
                          std::string_view title,
                          std::vector<NodeId>* card_nodes) {
  Document doc;
  const NodeId root = doc.create(NodeKind::Document);
  doc.meta.title = std::string(title);
  if (card_nodes) card_nodes->clear();
  // A line above the cards: what this is, and a place for the caret.
  const NodeId p = doc.create(NodeKind::Paragraph, root);
  doc.append_text(p,
                  items.empty() ? std::string("No images on this page.")
                  : items.size() == 1 ? std::string("1 image")
                                      : std::to_string(items.size()) + " images",
                  InlineKind::Emphasis);
  if (items.empty()) return doc;
  const NodeId coll = doc.create(NodeKind::Collection, root);
  doc.get(coll).summary = "grid";
  doc.get(coll).label = "gallery";
  for (const GalleryItem& it : items) {
    const NodeId img = doc.create(NodeKind::Image, coll);
    Node& n = doc.get(img);
    n.src = it.thumb;
    n.alt = it.caption;
    if (card_nodes) card_nodes->push_back(img);
  }
  return doc;
}

int gallery_index_of(const std::vector<GalleryItem>& items, NodeId node) {
  for (size_t i = 0; i < items.size(); ++i)
    if (items[i].node == node) return static_cast<int>(i);
  return -1;
}

int gallery_index_of(const std::vector<GalleryItem>& items,
                     std::string_view src) {
  for (size_t i = 0; i < items.size(); ++i)
    if (items[i].src == src || items[i].thumb == src)
      return static_cast<int>(i);
  return -1;
}

int gallery_item_index(std::string_view value, size_t count) {
  int n = 0;
  const char* end = value.data() + value.size();
  const auto [ptr, ec] = std::from_chars(value.data(), end, n);
  if (ec != std::errc{} || ptr != end || n < 1 ||
      static_cast<size_t>(n) > count)
    return -1;
  return n - 1;
}

std::string gallery_item_value(int index) { return std::to_string(index + 1); }

bool is_image_address(std::string_view url) {
  return (starts_with_ci(url, "https://") || starts_with_ci(url, "http://")) &&
         is_image_url(url);
}

Document image_url_document(std::string_view url) {
  Document doc;
  const NodeId root = doc.create(NodeKind::Document);
  std::string_view path = url.substr(0, url.find_first_of("?#"));
  const size_t slash = path.rfind('/');
  std::string name(slash == std::string_view::npos ? path
                                                   : path.substr(slash + 1));
  if (name.empty()) name = std::string(url);
  doc.meta.title = name;
  const NodeId img = doc.create(NodeKind::Image, root);
  doc.get(img).src = std::string(url);
  doc.get(img).alt = name;
  return doc;
}

}  // namespace xanatoo
