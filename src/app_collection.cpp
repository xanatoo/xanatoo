// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// Collections: view-local layout / sort / card size per Collection node.
// Clicking a card or an image shows it in the page's gallery view
// (`//gallery//item:N`, app_gallery.cpp) in the same pane.

#include <algorithm>

#include "xanatoo/app.hpp"
#include "xanatoo/util.hpp"

namespace xanatoo::app {

namespace {

using xanatoo::CollectionLayout;
using xanatoo::CollectionSort;
using xanatoo::CollectionView;
using xanatoo::NodeId;

NodeId enclosing_collection(const xanatoo::Document& doc, NodeId id) {
  while (id != xanatoo::INVALID_NODE) {
    const xanatoo::Node& n = doc.get(id);
    if (n.id != id) return xanatoo::INVALID_NODE;
    if (n.kind == xanatoo::NodeKind::Collection) return id;
    id = n.parent;
  }
  return xanatoo::INVALID_NODE;
}

CollectionView& view_of(App& app, const xanatoo::Document& doc, NodeId coll) {
  auto it = app.collection_views.find(coll);
  if (it == app.collection_views.end())
    it = app.collection_views
             .emplace(coll, xanatoo::default_collection_view(doc.get(coll)))
             .first;
  return it->second;
}

// The pipes before `//gallery` in the pane's address (its scope).
std::vector<xanatoo::Selector> gallery_scope(const Pane& p) {
  std::vector<xanatoo::Selector> out;
  for (const auto& s : p.locator.pipes) {
    if (s.kind == xanatoo::SelectorKind::Gallery) break;
    out.push_back(s);
  }
  return out;
}

// Show image `image` (a node of the page) or `src` in the page's gallery,
// or that of the collection `coll`. False when it is in neither.
bool show_in_gallery(App& app, int pi, NodeId coll, NodeId image,
                     const std::string& src) {
  Pane& p = app.panes[pi];
  const xanatoo::Document& doc = doc_of(app, p);
  const auto items = page_gallery_items(app, doc, coll);
  int i = image != xanatoo::INVALID_NODE ? xanatoo::gallery_index_of(items, image)
                                         : -1;
  if (i < 0 && !src.empty()) i = xanatoo::gallery_index_of(items, src);
  if (i < 0) return false;
  std::vector<xanatoo::Selector> scope;
  if (coll != xanatoo::INVALID_NODE)
    scope.push_back(xanatoo::node_selector(doc, coll));
  open_gallery_item(app, pi, std::move(scope), i);
  return true;
}

}  // namespace

NodeId current_collection(App& app) {
  auto& pane = focused_pane(app);
  auto& doc = doc_of(app, pane);
  if (app.collection_target != xanatoo::INVALID_NODE &&
      enclosing_collection(doc, app.collection_target) ==
          app.collection_target)
    return app.collection_target;
  return enclosing_collection(doc, pane.sel.focus.node);
}

void collection_command(App& app, const std::string& arg) {
  auto& pane = focused_pane(app);
  auto& doc = doc_of(app, pane);
  const NodeId coll = current_collection(app);
  if (coll == xanatoo::INVALID_NODE) {
    flash_status(app, "Not in a collection");
    return;
  }
  if (arg == "view") {
    if (GalleryView* gv = pane_gallery(pane)) {  // the gallery grid itself
      if (gv->items.empty())
        flash_status(app, "No images");
      else
        open_gallery_item(app, app.focus, gallery_scope(pane), 0);
      return;
    }
    const auto items = page_gallery_items(app, doc, coll);
    if (items.empty()) {
      flash_status(app, "Collection has no images");
      return;
    }
    open_gallery_item(app, app.focus, {xanatoo::node_selector(doc, coll)}, 0);
    return;
  }
  CollectionView& v = view_of(app, doc, coll);
  if (arg == "grid") v.layout = CollectionLayout::Grid;
  else if (arg == "list") v.layout = CollectionLayout::List;
  else if (arg == "full") v.layout = CollectionLayout::Full;
  else if (arg == "small") v.thumb_px = 112.f;
  else if (arg == "medium") v.thumb_px = 160.f;
  else if (arg == "large") v.thumb_px = 240.f;
  else if (arg == "sort-doc") v.sort = CollectionSort::Document;
  else if (arg == "sort-name") v.sort = CollectionSort::Name;
  else if (arg == "sort-name-desc") v.sort = CollectionSort::NameReverse;
  else return;
  // Card heights changed: stale hints would misplace skipped blocks.
  keep_views(app);
  for (auto& p : app.panes) p.height_cache.clear();
  app.needs_layout = true;
}

bool click_collection_card(App& app, int pi, float lx, float my) {
  Pane& p = app.panes[pi];
  for (const auto& im : p.layout.images) {
    if (im.in_text) continue;  // inline: a link (editor.cpp)
    const float y0 = im.y - p.scroll_y;
    if (lx < im.x || lx > im.x + im.width || my < y0 || my > y0 + im.height)
      continue;
    // A card of the gallery grid: that image on its own.
    if (GalleryView* gv = pane_gallery(p)) {
      const auto it = std::find(gv->cards.begin(), gv->cards.end(), im.node);
      if (it == gv->cards.end()) return false;
      open_gallery_item(app, pi, gallery_scope(p),
                        static_cast<int>(it - gv->cards.begin()));
      return true;
    }
    const auto& node = doc_of(app, p).get(im.node);
    std::string link;
    for (const auto& span : node.spans)
      if (span.kind == xanatoo::InlineKind::Link && !span.href.empty()) {
        link = span.href;
        break;
      }
    // Plain document images are for editing (resize handle), not viewing.
    if (im.thumb <= 0 && link.empty()) continue;
    if (!link.empty() && (!xanatoo::is_image_url(link) || !may_be_image(link))) {
      navigate_to_url(app, link);
      return true;
    }
    return show_in_gallery(app, pi, im.group, im.node, node.src);
  }
  return false;
}

bool open_link_image(App& app, int pi, const std::string& href) {
  // Named like a picture and not known to serve a page (url_cache_kind).
  if (!xanatoo::is_image_url(href) || !may_be_image(href)) return false;
  // Relative links of a Gemini page are gemini:// resources.
  if (!link_base_url(app, pi).empty() && !xanatoo::starts_with_ci(href, "http"))
    return false;
  if (!pane_gallery(app.panes[pi]) &&
      show_in_gallery(app, pi, xanatoo::INVALID_NODE, xanatoo::INVALID_NODE,
                      href))
    return true;
  // Not one of the page's images: that image on its own, in this view.
  if (!xanatoo::is_image_address(href)) return false;
  focus_view(app, pi);
  return navigate_pane(app, pi, href);
}

void note_collection_target(App& app, int pi, float lx, float my) {
  Pane& p = app.panes[pi];
  app.collection_target = xanatoo::INVALID_NODE;
  for (const auto& im : p.layout.images) {
    if (im.group == xanatoo::INVALID_NODE) continue;
    const float y0 = im.y - p.scroll_y;
    if (lx >= im.x && lx <= im.x + im.width && my >= y0 &&
        my <= y0 + im.height) {
      app.collection_target = im.group;
      return;
    }
  }
}

}  // namespace xanatoo::app
