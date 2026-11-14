// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// The `//gallery` view of a page (docs/PASSAGES.md "Gallery"): every image
// of the page (or of the cut before the pipe) as a grid of cards, and
// `//gallery//item:N` one of them on its own. Pure: no SDL, no I/O.

#include "xanatoo/types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace xanatoo {

struct GalleryItem {
  std::string src;      // what the single view shows (a linked original first)
  std::string thumb;    // what the card shows (the picture in the page)
  std::string caption;  // Figure caption, alt text or link text; may be empty
  NodeId node = INVALID_NODE;  // the node it was found in
};

// Images below `scope` (INVALID_NODE = the whole document) in document
// order: Image nodes, inline image spans and links to images
// (`is_image_url`). Each picture once (by `src`).
std::vector<GalleryItem> gallery_items(const Document& doc, NodeId scope);

// The grid page: a Collection with one Image card per item, in order. Card
// i is the image node of `card_nodes[i]` (filled when given).
Document gallery_document(const std::vector<GalleryItem>& items,
                          std::string_view title,
                          std::vector<NodeId>* card_nodes = nullptr);

// Position (0-based) of the item found in `node` or showing `src`; -1 when
// none.
int gallery_index_of(const std::vector<GalleryItem>& items, NodeId node);
int gallery_index_of(const std::vector<GalleryItem>& items,
                     std::string_view src);

// `item:N` payload (1-based) → 0-based index, -1 when malformed or out of
// range; and back.
int gallery_item_index(std::string_view value, size_t count);
std::string gallery_item_value(int index);

}  // namespace xanatoo
