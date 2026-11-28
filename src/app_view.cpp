// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>

#include "xanatoo/app.hpp"
#include "xanatoo/diff_overview.hpp"
#include "xanatoo/edit_bar.hpp"
#include "xanatoo/epub_import.hpp"
#include "xanatoo/gemini.hpp"
#include "xanatoo/lemmy.hpp"
#include "xanatoo/nostr.hpp"
#include "xanatoo/parse.hpp"
#include "xanatoo/reddit.hpp"
#include "xanatoo/sidestore.hpp"
#include "xanatoo/util.hpp"

namespace fs = std::filesystem;

namespace xanatoo::app {

// Panels laid out with app.fonts / the layout engine outside the panes
// (Help, hover help, Context) go stale when sizes or options change.
static void invalidate_side_layouts(App& app) {
  app.help_needs_layout = true;
  app.hover_help.needs_layout = true;
  app.context_needs_layout = true;
}

void apply_view_settings(App& app, bool persist) {
  keep_views(app);
  app.view_settings.clamp();
  app.layout_engine.set_options(
      xanatoo::LayoutOptions::from_view(app.view_settings, app.ui_scale));
  for (Pane& p : app.panes) p.height_cache.clear();
  app.needs_layout = true;
  invalidate_side_layouts(app);
  if (persist && !xanatoo::save_view_settings(app.view_settings))
    std::cerr << "Could not persist view settings to "
              << xanatoo::view_settings_path() << "\n";
}

void refresh_outline(App& app) {
  // Preamble row label when the document has no title: the tab label.
  std::string label = buffer_tab_label(app, focused_pane(app).buf);
  if (xanatoo::starts_with(label, "*")) label.erase(0, 1);
  xanatoo::collect_outline(doc_of(app, focused_pane(app)), app.outline,
                           &app.folded_sections, label,
                           pane_reader(app, focused_pane(app)));
  if (app.outline_sel >= static_cast<int>(app.outline.size()))
    app.outline_sel = std::max(0, static_cast<int>(app.outline.size()) - 1);
  if (app.outline_sel < 0) app.outline_sel = 0;
}

void refresh_backlinks(App& app) {
  app.backlinks.clear();
  const auto& d = doc_of(app, focused_pane(app));
  xanatoo::NodeId id = focused_pane(app).sel.focus.node;
  std::string lab = xanatoo::address_label(d, id);
  if (lab.empty()) {
    // Prefer enclosing section's header label, then any # from plain text.
    xanatoo::NodeId sec = xanatoo::enclosing_section(d, id);
    if (sec != xanatoo::INVALID_NODE) lab = xanatoo::address_label(d, sec);
  }
  if (lab.empty() && id != xanatoo::INVALID_NODE) {
    // Match jump_to_href style: use first heading text as soft anchor? skip
    lab = xanatoo::address_label(d, id);
  }
  app.backlinks_label = lab;
  if (!lab.empty()) xanatoo::collect_backlinks(d, lab, app.backlinks);
  if (app.backlinks_sel >= static_cast<int>(app.backlinks.size()))
    app.backlinks_sel = std::max(0, static_cast<int>(app.backlinks.size()) - 1);
  if (app.backlinks_sel < 0) app.backlinks_sel = 0;
}

void invalidate_height_around(App& app, Pane& pane, xanatoo::NodeId id) {
  if (id == xanatoo::INVALID_NODE) return;
  const auto& d = doc_of(app, pane);
  for (xanatoo::NodeId cur = id;
       cur != xanatoo::INVALID_NODE && cur < d.next_id;
       cur = d.get(cur).parent) {
    pane.height_cache.erase(cur);
  }
}

void drop_height_hints_for_buf(App& app, BufferId bi) {
  for (int pi : live_views(app)) {
    if (app.panes[pi].buf != bi) continue;
    app.panes[pi].height_cache.clear();
    app.panes[pi].materialised_top = 0.f;
    app.panes[pi].materialised_bottom = 0.f;
  }
}

void toggle_section_fold(App& app, xanatoo::NodeId section) {
  if (section == xanatoo::INVALID_NODE) return;
  const auto& d = doc_of(app, focused_pane(app));
  if (section >= d.next_id) return;
  const auto& n = d.get(section);
  if (n.kind != xanatoo::NodeKind::Section &&
      n.kind != xanatoo::NodeKind::Article)
    return;
  const bool now_folded = !app.folded_sections.count(section);
  if (now_folded)
    app.folded_sections.insert(section);
  else
    app.folded_sections.erase(section);
  for (int pi : live_views(app)) {
    if (app.panes[pi].buf == focused_pane(app).buf)
      invalidate_height_around(app, app.panes[pi], section);
  }
  app.needs_layout = true;
  if (app.outline_mode) refresh_outline(app);
  // Persist view-local fold in side SQLite (documents stay immutable).
  std::string doc_key = path_of(app, focused_pane(app));
  if (!doc_key.empty()) {
    std::string sk = xanatoo::section_stable_key(d, section);
    xanatoo::save_section_fold(doc_key, sk, now_folded);
  }
}

void toggle_section_focus_mode(App& app) {
  Pane& fp = focused_pane(app);
  fp.section_focus = !fp.section_focus;
  if (fp.section_focus) {
    // Seed from outline selection or caret section.
    xanatoo::NodeId sec = xanatoo::INVALID_NODE;
    if (!app.outline.empty() && app.outline_sel >= 0 &&
        app.outline_sel < static_cast<int>(app.outline.size())) {
      sec = app.outline[static_cast<size_t>(app.outline_sel)].section;
    }
    if (sec == xanatoo::INVALID_NODE)
      sec = xanatoo::enclosing_section(doc_of(app, fp), fp.sel.focus.node);
    if (sec == xanatoo::INVALID_NODE) {
      fp.section_focus = false;
      flash_status(app, "Focus: no section under caret / outline");
      return;
    }
    set_pane_cut(app, fp, sec);
    flash_status(app, "Focus section on — outline picks what you see");
  } else {
    set_pane_cut(app, fp, xanatoo::INVALID_NODE);
    flash_status(app, "Focus section off — full document");
  }
}

void reload_folds_for_focused(App& app) {
  app.folded_sections.clear();
  std::string doc_key = path_of(app, focused_pane(app));
  if (doc_key.empty()) return;
  std::unordered_set<std::string> keys;
  xanatoo::load_section_folds(doc_key, keys);
  xanatoo::folds_keys_to_ids(doc_of(app, focused_pane(app)), keys,
                             app.folded_sections);
  app.needs_layout = true;
  if (app.outline_mode) refresh_outline(app);
}

namespace {
void keep_view(App& app, Pane& p);
}  // namespace

static void keep_carets_visible(App& app, Pane& v) {
  const auto& d = doc_of(app, v);
  const auto& folds = buf(app, v.buf).folds;
  for (xanatoo::Caret* c : {&v.sel.focus, &v.sel.anchor}) {
    // Innermost first: an outer fold moves the caret on.
    for (auto id = xanatoo::fold_hiding(d, c->node, &folds);
         id != xanatoo::INVALID_NODE; id = xanatoo::fold_hiding(d, id, &folds)) {
      if (d.get(id).kind == xanatoo::NodeKind::ListItem) {
        *c = {id, 0, 0};
        continue;
      }
      // A stretch: what it shows instead. A classic one has no text left, the
      // caret comes back with it.
      const auto text = xanatoo::stretch_first_text(
          d, id, xanatoo::stretch_folded(d, id, &folds));
      if (text != xanatoo::INVALID_NODE) *c = {text, 0, 0};
    }
  }
}

// Set the state of `ids` in view `pi`'s buffer. View state; only while
// editing does a stretch's state belong to the document (`:expanded`).
static void apply_folds(App& app, int pi, const std::vector<xanatoo::NodeId>& ids,
                        bool folded) {
  Pane& p = app.panes[pi];
  auto& d = doc_of(app, p);
  const bool author = pi == app.focus && !browsing(app);
  bool edited = false;
  for (xanatoo::NodeId id : ids) {
    const bool stretch = d.get(id).kind == xanatoo::NodeKind::Stretch;
    if (author && stretch) {
      if (d.get(id).expanded == folded) {
        if (!edited) edit(app);
        edited = true;
        d.get(id).expanded = !folded;
      }
      buf(app, p.buf).folds.erase(id);
    } else {
      buf(app, p.buf).folds[id] = folded;
    }
  }
  for (int vi : live_views(app)) {
    Pane& v = app.panes[vi];
    if (v.buf != p.buf) continue;
    for (xanatoo::NodeId id : ids) invalidate_height_around(app, v, id);
    // A caret in the text going out of sight moves to what stays.
    keep_carets_visible(app, v);
  }
  app.needs_layout = true;
}

void set_fold(App& app, int pi, xanatoo::NodeId id, bool folded) {
  Pane& p = app.panes[pi];
  const auto& d = doc_of(app, p);
  const bool stretch =
      id < d.next_id && d.get(id).kind == xanatoo::NodeKind::Stretch;
  if (!stretch && !xanatoo::list_item_foldable(d, id)) return;
  if (stretch) {
    // Folding from the cue below a long text goes back to its summary; what
    // is above the stretch does not move.
    for (const auto& box : p.layout.boxes) {
      if (box.node != id || !folded) continue;
      if (p.pages.on) {
        if (box.y < p.scroll_y)
          page_goto_spread(app, p, spread_at(p, box.y));
      } else {
        p.scroll_y = std::max(0.f, std::min(p.scroll_y, box.y - 40.f));
        p.view_anchor.reset();
      }
      break;
    }
    for (int vi : live_views(app))
      if (vi != pi && app.panes[vi].buf == p.buf) keep_view(app, app.panes[vi]);
  }
  apply_folds(app, pi, {id}, folded);
}

void toggle_fold(App& app, int pi, xanatoo::NodeId id) {
  Pane& p = app.panes[pi];
  set_fold(app, pi, id,
           !xanatoo::node_folded(doc_of(app, p), id, &buf(app, p.buf).folds));
}

void set_list_folds(App& app, int pi, xanatoo::NodeId node, bool folded) {
  const auto& d = doc_of(app, app.panes[pi]);
  const auto items =
      xanatoo::foldable_list_items(d, xanatoo::outermost_list(d, node));
  if (items.empty()) return;
  keep_views(app);
  apply_folds(app, pi, items, folded);
}

void set_stretch_folds(App& app, int pi, bool folded) {
  const auto& d = doc_of(app, app.panes[pi]);
  const auto ids = xanatoo::stretches(d, d.root, true);
  if (ids.empty()) return;
  keep_views(app);
  apply_folds(app, pi, ids, folded);
}

bool reveal_folds(App& app, Pane& p, xanatoo::NodeId node) {
  const auto& d = doc_of(app, p);
  auto& folds = buf(app, p.buf).folds;
  bool any = false;
  for (auto id = xanatoo::fold_hiding(d, node, &folds);
       id != xanatoo::INVALID_NODE;
       id = xanatoo::fold_hiding(d, node, &folds)) {
    folds[id] = !xanatoo::node_folded(d, id, &folds);
    any = true;
    for (int vi : live_views(app))
      if (app.panes[vi].buf == p.buf)
        invalidate_height_around(app, app.panes[vi], id);
  }
  if (any) app.needs_layout = true;
  return any;
}

void reveal_hit(App& app, Pane& p, xanatoo::NodeId node) {
  reveal_chrome(app, p, node);
  if (reveal_folds(app, p, node)) force_full_pane_layout(app, p);
}

std::vector<xanatoo::NodeId> find_skips(App& app, Pane& p) {
  return xanatoo::hidden_stretch_blocks(doc_of(app, p),
                                        &buf(app, p.buf).folds, true);
}

std::string copied_text(App& app, Pane& p) {
  const auto& d = doc_of(app, p);
  if (p.sel.empty()) return {};
  return xanatoo::selected_text(
      d, p.sel,
      xanatoo::hidden_stretch_blocks(d, &buf(app, p.buf).folds, false));
}

xanatoo::NodeId current_list_target(App& app) {
  const auto& d = doc_of(app, focused_pane(app));
  const xanatoo::NodeId item = xanatoo::enclosing_list_item(d, app.fold_target);
  if (item == xanatoo::INVALID_NODE ||
      !xanatoo::has_foldable_list_items(d, xanatoo::outermost_list(d, item)))
    return xanatoo::INVALID_NODE;
  return item;
}

xanatoo::NodeId current_stretch_target(App& app) {
  const auto& d = doc_of(app, focused_pane(app));
  for (xanatoo::NodeId id = app.fold_target;
       id != xanatoo::INVALID_NODE && id < d.next_id; id = d.get(id).parent)
    if (xanatoo::stretch_summary(d, id) != xanatoo::INVALID_NODE) return id;
  return xanatoo::INVALID_NODE;
}

static void toggle_stretch(App& app, xanatoo::NodeId id) {
  toggle_fold(app, app.focus, id);
  Pane& p = focused_pane(app);
  const auto& d = doc_of(app, p);
  const bool folded = xanatoo::stretch_folded(d, id, &buf(app, p.buf).folds);
  if (xanatoo::stretch_summary(d, id) != xanatoo::INVALID_NODE)
    flash_status(app, folded ? "Summary" : "Full text");
  else
    flash_status(app, folded ? "Collapsed" : "Expanded");
}

void toggle_fold_at_caret(App& app) {
  auto& d = doc_of(app, focused_pane(app));
  xanatoo::NodeId id = focused_pane(app).sel.focus.node;
  if (xanatoo::list_item_foldable(d, id)) {
    toggle_fold(app, app.focus, id);
    flash_status(app, xanatoo::list_item_folded(
                          d, id, &buf(app, focused_pane(app).buf).folds)
                          ? "List item folded"
                          : "List item expanded");
    return;
  }
  while (id != xanatoo::INVALID_NODE && id < d.next_id) {
    const auto& n = d.get(id);
    if (n.kind == xanatoo::NodeKind::Stretch) {
      toggle_stretch(app, id);
      return;
    }
    if (n.kind == xanatoo::NodeKind::Section ||
        n.kind == xanatoo::NodeKind::Article) {
      toggle_section_fold(app, id);
      flash_status(app, app.folded_sections.count(id) ? "Section folded"
                                                      : "Section expanded");
      return;
    }
    id = n.parent;
  }
  flash_status(app, "No section under caret");
}

void refresh_diff(App& app) {
  app.diff_marks[0].clear();
  app.diff_marks[1].clear();
  app.diff_words[0].clear();
  app.diff_words[1].clear();
  app.diff_overview[0].clear();
  app.diff_overview[1].clear();
  if (!app.diff_mode || shared_doc(app)) return;
  const BufferId b0 = app.panes[0].buf, b1 = app.panes[1].buf;
  if (!find_buf(app, b0) || !find_buf(app, b1) || b0 == b1) return;
  xanatoo::compute_diff_marks(buf(app, b0).doc,
                              buf(app, b1).doc,
                              app.diff_marks[0], app.diff_marks[1],
                              &app.diff_words[0], &app.diff_words[1]);
  for (int i = 0; i < kCentreViews; ++i)
    app.diff_overview[i] = xanatoo::diff_overview_positions(
        doc_of(app, app.panes[i]), app.diff_marks[i]);
}

bool pane_reader(const App& app, const Pane& p) {
  return p.reader.value_or(app.view_settings.reader_mode);
}

void set_reader(App& app, Pane& p, bool on) {
  if (pane_reader(app, p) == on) {
    p.reader = on;
    return;
  }
  if (!p.view_anchor && p.scroll_y > 0.f)  // as keep_view
    p.view_anchor = view_anchor_at(app, p, 0.f, 0.f);
  p.reader = on;
  // Heights of the blocks around the chrome change with it.
  p.height_cache.clear();
  app.needs_layout = true;
  if (app.outline_mode && &p == &focused_pane(app)) refresh_outline(app);
  flash_status(app, on ? "Reader mode: page chrome hidden"
                       : "Reader mode off: whole page");
}

void reveal_chrome(App& app, Pane& p, xanatoo::NodeId target) {
  if (!pane_reader(app, p) || target == xanatoo::INVALID_NODE) return;
  const auto& d = doc_of(app, p);
  for (xanatoo::NodeId id = target; id != xanatoo::INVALID_NODE && id < d.next_id;
       id = d.get(id).parent) {
    if (d.get(id).kind == xanatoo::NodeKind::Chrome) {
      set_reader(app, p, false);
      return;
    }
  }
}

void expand_ancestors_for_node(App& app, xanatoo::NodeId target) {
  if (target == xanatoo::INVALID_NODE) return;
  reveal_chrome(app, focused_pane(app), target);
  reveal_folds(app, focused_pane(app), target);
  const auto& d = doc_of(app, focused_pane(app));
  bool any = false;
  std::string doc_key = path_of(app, focused_pane(app));
  for (xanatoo::NodeId id = target;
       id != xanatoo::INVALID_NODE && id < d.next_id; id = d.get(id).parent) {
    const auto& n = d.get(id);
    if (n.kind != xanatoo::NodeKind::Section &&
        n.kind != xanatoo::NodeKind::Article)
      continue;
    if (!app.folded_sections.count(id)) continue;
    app.folded_sections.erase(id);
    any = true;
    if (!doc_key.empty()) {
      std::string sk = xanatoo::section_stable_key(d, id);
      xanatoo::save_section_fold(doc_key, sk, false);
    }
    for (int pi : live_views(app)) {
      if (app.panes[pi].buf == focused_pane(app).buf)
        invalidate_height_around(app, app.panes[pi], id);
    }
  }
  if (any) {
    app.needs_layout = true;
    if (app.outline_mode) refresh_outline(app);
  }
}

void place_centre_rows(App& app) {
  const int band_top = dock_band_h(app, Dock::Top);
  const int top = static_cast<int>(app.ui_top) + band_top;
  const int view_h = std::max(
      1, app.win_h - status_bar_h - top - static_cast<int>(app.ui_bottom) -
             dock_band_h(app, Dock::Bottom));
  const int dock = dock_column_w(app, Dock::Left);
  const int width = std::max(64, app.win_w - dock -
                             dock_column_w(app, Dock::Right));
  TileTree tiles(0);
  if (app.split) tiles.split(0, 1, app.tile_axis, app.split_ratio);
  const TileGeometry geometry = tiles.geometry(
      TileRect{dock, top, width, view_h}, tile_gutter_size(app));
  for (const TileArea& area : geometry.tiles) {
    Pane& p = app.panes[area.view];
    const bool left_scroll = app.split &&
        app.tile_axis == TileAxis::Columns && area.view == 0;
    const int x = area.rect.x + (left_scroll ? kSbW : 0);
    const int w = std::max(1, area.rect.w - (left_scroll ? kSbW : 0));
    const int old_w = p.w, old_h = p.h;
    p.sb_left = left_scroll;
    p.x = x;
    p.w = w;
    // The edit strip of an editable document, or the service strip of a
    // service's page, sits on top of its text.
    p.head_h = std::min(pane_head_h(app, p), area.rect.h / 2);
    p.y = area.rect.y + p.head_h;
    p.h = std::max(1, area.rect.h - p.head_h - pane_foot_h(app, p));
    if (old_w != p.w || old_h != p.h) app.needs_layout = true;
  }
  if (!app.split) {
    // The hidden second view keeps its document state but draws nothing.
    Pane& p = app.panes[1];
    p.sb_left = false;
    p.x = dock;
    p.y = top;
    p.w = 0;
    p.head_h = 0;
    p.head_service = false;
    p.h = std::max(1, view_h - pane_foot_h(app, p));
  }
}

int edit_bar_h(App& app, const Pane& p) {
  const int width = p.w + (p.sb_left ? kSbW : 0);
  if (width <= 0 || app.chrome_hidden || media_fullscreen(app)) return 0;
  const DocBuffer* b = find_buf(app, p.buf);
  if (!b || b->placeholder || pane_read_only(app, p)) return 0;
  return xanatoo::edit_bar_height(static_cast<float>(width), app.ui_scale);
}

std::string service_address(App& app, const std::string& address) {
  // A search of a service's engine belongs to that service.
  if (!xanatoo::is_search_ref(address)) return address;
  std::string target = xanatoo::search_target(address, app.search.engines);
  if (!target.empty()) return target;
  // A JSON engine (Hacker News via Algolia) fetches no page: the first
  // address its `sites` claim that is a service's.
  const auto ref = xanatoo::parse_search_ref(address);
  if (!ref) return address;
  for (const xanatoo::SearchEngine& e : app.search.engines) {
    if (e.id != ref->engine) continue;
    for (const std::string& site : e.sites) {
      std::string a = site.find(':') != std::string::npos
                          ? site
                          : "https://" + site + "/";
      if (xanatoo::service_bar(a)) return a;
    }
  }
  return address;
}

const xanatoo::ServiceBar* pane_service_bar(App& app, Pane& p) {
  if (!find_buf(app, p.buf)) return nullptr;
  std::string address = xanatoo::parse_locator(pane_address(app, p)).base;
  if (address != p.service_addr) {
    p.service = xanatoo::service_bar(service_address(app, address));
    if (p.service && xanatoo::is_search_ref(address))
      for (xanatoo::ServiceRow& row : p.service->rows)  // a search: no section
        for (xanatoo::ServiceLink& l : row.links) l.current = false;
    p.service_addr = std::move(address);
  }
  return p.service ? &*p.service : nullptr;
}

int pane_head_h(App& app, Pane& p) {
  p.head_service = false;
  if (const int h = edit_bar_h(app, p); h > 0) return h;
  const int width = p.w + (p.sb_left ? kSbW : 0);
  if (width <= 0 || app.chrome_hidden || media_fullscreen(app)) return 0;
  const xanatoo::ServiceBar* bar = pane_service_bar(app, p);
  if (!bar) return 0;
  p.head_service = true;
  return xanatoo::service_bar_height(*bar, app.ui_scale);
}

int tile_gutter_size(const App& app) {
  const int px = app.diff_mode ? 22 : app.tile_mode == TileMode::Trail ? 14 : 12;
  return static_cast<int>(std::lround(px * app.ui_scale));
}

SDL_Rect tile_divider_rect(const App& app) {
  if (!app.split) return SDL_Rect{};
  const int gap = tile_gutter_size(app);
  if (app.tile_axis == TileAxis::Rows)
    return SDL_Rect{app.panes[0].x, app.panes[1].y - app.panes[1].head_h - gap,
                    app.panes[0].w, gap};
  return SDL_Rect{app.panes[1].x - gap, app.panes[0].y - app.panes[0].head_h,
                  gap, app.panes[0].h + app.panes[0].head_h};
}

SDL_Rect tile_mode_button_rect(const App& app) {
  if (!app.split) return SDL_Rect{};
  const SDL_Rect div = tile_divider_rect(app);
  const int w = static_cast<int>(std::lround(60.f * app.ui_scale));
  const int h = static_cast<int>(std::lround(22.f * app.ui_scale));
  if (app.tile_axis == TileAxis::Rows)
    return SDL_Rect{div.x + div.w - w - 8, div.y + (div.h - h) / 2, w, h};
  return SDL_Rect{div.x + (div.w - w) / 2, div.y + div.h - h - 8, w, h};
}

SDL_Rect tile_diff_track_rect(const App& app) {
  if (!app.split || !app.diff_mode) return SDL_Rect{};
  const SDL_Rect div = tile_divider_rect(app);
  const SDL_Rect button = tile_mode_button_rect(app);
  constexpr int inset = 4;
  if (app.tile_axis == TileAxis::Rows) {
    const int width = std::max(0, button.x - div.x - inset * 2);
    return SDL_Rect{div.x + inset, div.y + inset,
                    width, std::max(0, div.h - inset * 2)};
  }
  const int height = std::max(0, button.y - div.y - inset * 2);
  return SDL_Rect{div.x + inset, div.y + inset,
                  std::max(0, div.w - inset * 2), height};
}

bool scroll_diff_overview(App& app, int x, int y) {
  const SDL_Rect track = tile_diff_track_rect(app);
  if (track.w < 2 || track.h < 2 ||
      x < track.x || x >= track.x + track.w ||
      y < track.y || y >= track.y + track.h) return false;
  const bool rows = app.tile_axis == TileAxis::Rows;
  const float fraction = std::clamp(rows
      ? static_cast<float>(x - track.x) / static_cast<float>(track.w)
      : static_cast<float>(y - track.y) / static_cast<float>(track.h),
      0.f, 1.f);
  for (int i = 0; i < kCentreViews; ++i) {
    Pane& pane = app.panes[i];
    const float height = pane.layout.content_height;
    const float desired = fraction * height - static_cast<float>(pane.h) / 2.f;
    if (pane.pages.on) {
      page_goto_spread(app, pane, spread_at(pane, std::max(0.f, desired)));
    } else {
      pane.view_anchor.reset();
      pane.scroll_y = std::clamp(desired, 0.f, pane_max_scroll(app, pane));
      ensure_virtual_layout(app, pane);
    }
  }
  return true;
}

namespace {

constexpr int kAnchorPasses = 4;

void keep_view(App& app, Pane& p) {
  if (p.view_anchor || p.scroll_y <= 0.f) return;  // the top stays the top
  p.view_anchor = view_anchor_at(app, p, 0.f, 0.f);
}

// Scroll so the held position is back on its spot. True when that needs a
// new layout (the view left the materialised band).
bool restore_view_anchor(App& app, Pane& p) {
  if (!p.view_anchor || p.view_anchor->buf != p.buf) return false;
  const auto y = caret_doc_y(app, p, p.view_anchor->pos);
  if (!y) return false;
  if (p.pages.on) {  // the spread the position lies in
    p.scroll_y = spread_scroll(p, spread_at(p, *y));
    return false;
  }
  p.scroll_y =
      std::clamp(*y - p.view_anchor->view_y, 0.f, pane_max_scroll(app, p));
  return p.layout.virtualized &&
         (p.scroll_y < p.materialised_top ||
          p.scroll_y + static_cast<float>(p.h) > p.materialised_bottom);
}

}  // namespace

std::optional<ViewAnchor> view_anchor_at(App& app, Pane& p, float x,
                                         float view_y) {
  if (p.layout_buf != p.buf) return std::nullopt;  // layout of another page
  auto pos = xanatoo::nearest_caret(p.layout, x, p.scroll_y + view_y);
  if (!pos) return std::nullopt;
  auto y = caret_doc_y(app, p, *pos);
  if (!y) return std::nullopt;
  return ViewAnchor{p.buf, *pos, *y - p.scroll_y};
}

void keep_views(App& app) {
  for (int pi : live_views(app)) keep_view(app, app.panes[pi]);
}

// The buffer's folds, for layouts of that buffer only.
struct FoldScope {
  App& app;
  FoldScope(App& a, const Pane& p) : app(a) {
    app.layout_engine.set_folds(&buf(app, p.buf).folds);
  }
  ~FoldScope() { app.layout_engine.set_folds(nullptr); }
  FoldScope(const FoldScope&) = delete;
  FoldScope& operator=(const FoldScope&) = delete;
};

void layout_panes(App& app) {
  // Window views take their rects from the window placement.
  place_windows(app);
  // Clamp pane buffer indices if tabs were closed out from under them.
  for (int pi = 0; pi < 2; ++pi) {
    if (!find_buf(app, app.panes[pi].buf) && !app.tab_order.empty()) {
      app.panes[pi].buf = app.tab_order.front();
    }
  }
  place_centre_rows(app);
  // Block heights depend on width; drop hints when this pane's width changed.
  for (int pi : live_views(app)) {
    if (app.panes[pi].w != app.panes[pi].last_layout_pane_w) {
      keep_view(app, app.panes[pi]);
      app.panes[pi].height_cache.clear();
      app.panes[pi].last_layout_pane_w = app.panes[pi].w;
    }
  }
  auto layout_one = [&](Pane& pane) {
    try {
      xanatoo::ensure_valid_caret(doc_of(app, pane), pane.sel.focus);
      xanatoo::ensure_valid_caret(doc_of(app, pane), pane.sel.anchor);
      const float view_h = std::max(80.f, static_cast<float>(pane.h));
      // Paged reading lays the whole page out one column wide, then cuts it
      // into pages (no virtualization: the page count needs it all).
      const bool paged = pane_paged(app, pane) && pane.h >= 120;
      const int columns = paged ? pane_page_columns(app, pane) : 1;
      pane.pages.on = paged;
      xanatoo::LayoutViewport vp;
      if (!paged) {
        vp.top = pane.scroll_y;
        vp.bottom = pane.scroll_y + view_h;
        vp.overscan = std::max(800.f, view_h * 1.5f);
      }
      const std::unordered_map<xanatoo::NodeId, float>* hints =
          pane.height_cache.empty() || paged ? nullptr : &pane.height_cache;
      app.layout_engine.set_hide_chrome(pane_reader(app, pane));
      const GalleryStyleScope gallery_look(app, pane);
      const float layout_w = static_cast<float>(
          std::max(40, (pane.w - sb_gutter(pane)) / columns));
      const FoldScope folds(app, pane);
      pane.layout = app.layout_engine.layout(
          doc_of(app, pane), layout_w, base_of(app, pane), vp, hints,
          &app.folded_sections, pane_cut(app, pane));
      pane.layout_buf = pane.buf;
      app.layout_engine.set_hide_chrome(false);
      pane.chrome_parts = xanatoo::chrome_regions(doc_of(app, pane)).size();
      pane.nav_parts = page_nav_regions(doc_of(app, pane));
      pane.has_headings = xanatoo::document_has_headings(doc_of(app, pane));
      if (paged) {
        paginate_pane(app, pane, columns, layout_w);
        pane.materialised_top = 0.f;
        pane.materialised_bottom = pane.layout.content_height;
        return;
      }
      // Refresh height hints from measured (and skipped-stub) boxes.
      for (const auto& b : pane.layout.boxes) {
        if (b.node != xanatoo::INVALID_NODE && b.height > 0.f)
          pane.height_cache[b.node] = b.height;
      }
      if (pane.layout.virtualized) {
        pane.materialised_top = pane.layout.materialised_top;
        pane.materialised_bottom = pane.layout.materialised_bottom;
      } else if (vp.active()) {
        pane.materialised_top = vp.top - vp.overscan;
        pane.materialised_bottom = vp.bottom + vp.overscan;
      } else {
        pane.materialised_top = 0.f;
        pane.materialised_bottom = pane.layout.content_height;
      }
    } catch (const std::exception& e) {
      std::cerr << "pane layout failed: " << e.what() << "\n";
      pane.layout = {};
    }
  };
  for (int pi : live_views(app)) {
    Pane& p = app.panes[pi];
    layout_one(p);
    if (p.layout.virtualized && p.scroll_y > pane_max_scroll(app, p)) {
      // If the new document is shorter, materialise the fallback position
      // too. The first pass may have laid out a band past the new end.
      p.scroll_y = pane_max_scroll(app, p);
      layout_one(p);
    }
    // A far move leaves the materialised band: lay out there and look again.
    for (int pass = 0; pass < kAnchorPasses && restore_view_anchor(app, p);
         ++pass)
      layout_one(p);
    p.view_anchor.reset();
  }
  app.needs_layout = false;
  app.last_layout_w = app.win_w;
  app.last_layout_h = app.win_h;
  refresh_diff(app);
}

void ensure_virtual_layout(App& app, Pane& p) {
  if (p.pages.on || p.height_cache.empty()) return;
  const float view_h = std::max(80.f, static_cast<float>(p.h));
  const float margin = 64.f;
  const float vis_top = p.scroll_y;
  const float vis_bot = p.scroll_y + view_h;
  if (vis_top < p.materialised_top + margin ||
      vis_bot > p.materialised_bottom - margin) {
    // Hints of blocks entering the band may be estimates (zoom).
    keep_view(app, p);
    app.needs_layout = true;
  }
}

std::optional<float> caret_doc_y(App& app, Pane& p, const xanatoo::Caret& c) {
  const float lh = static_cast<float>(app.fonts.regular.line_height());
  auto cpos = xanatoo::caret_screen_pos(p.layout, c, 0.f, lh);
  if (cpos.valid) return cpos.y;
  const auto& d = doc_of(app, p);
  for (xanatoo::NodeId id = c.node;
       id != xanatoo::INVALID_NODE && id < d.next_id; id = d.get(id).parent) {
    for (const auto& box : p.layout.boxes) {
      if (box.node == id) return box.y;
    }
  }
  return std::nullopt;
}

void force_full_pane_layout(App& app, Pane& p) {
  if (p.pages.on) return;  // a paged layout is always whole
  try {
    xanatoo::ensure_valid_caret(doc_of(app, p), p.sel.focus);
    xanatoo::ensure_valid_caret(doc_of(app, p), p.sel.anchor);
    const GalleryStyleScope gallery_look(app, p);
    const FoldScope folds(app, p);
    p.layout = app.layout_engine.layout(
        doc_of(app, p), static_cast<float>(std::max(40, p.w - sb_gutter(p))),
        base_of(app, p), {}, nullptr, &app.folded_sections,
        pane_cut(app, p));
    for (const auto& b : p.layout.boxes) {
      if (b.node != xanatoo::INVALID_NODE && b.height > 0.f)
        p.height_cache[b.node] = b.height;
    }
    p.materialised_top = 0.f;
    p.materialised_bottom = p.layout.content_height;
  } catch (const std::exception& e) {
    std::cerr << "force full layout failed: " << e.what() << "\n";
  }
}

void scroll_caret_into_view(App& app, Pane& p) {
  auto yopt = caret_doc_y(app, p, p.sel.focus);
  if (!yopt) {
    // Target not in the current (virtualized) band — full pass then retry.
    force_full_pane_layout(app, p);
    yopt = caret_doc_y(app, p, p.sel.focus);
  }
  if (!yopt) return;
  if (p.pages.on) {
    page_goto_spread(app, p, spread_at(p, *yopt));
    return;
  }
  float doc_y = *yopt;
  float margin = 40.f;
  // Visible content height excludes menu/toolbar and status bar.
  const float view_h = std::max(80.f, static_cast<float>(p.h));
  if (doc_y - p.scroll_y < margin) {
    p.scroll_y = std::max(0.f, doc_y - margin);
  } else if (doc_y - p.scroll_y > view_h - margin) {
    p.scroll_y = doc_y - (view_h - margin);
  }
  // After a long jump, re-virtualize around the new scroll position.
  app.needs_layout = true;
  ensure_virtual_layout(app, p);
}

void scroll_caret_to_top(App& app, Pane& p) {
  auto box_top_of = [&](xanatoo::NodeId id) -> std::optional<float> {
    if (id == xanatoo::INVALID_NODE) return std::nullopt;
    for (const auto& box : p.layout.boxes) {
      if (box.node == id) return box.y;
    }
    return std::nullopt;
  };

  auto resolve_top = [&]() -> std::optional<float> {
    const auto& d = doc_of(app, p);
    // Header/Heading box, then Section, then any ancestor box.
    if (auto y = box_top_of(p.sel.focus.node)) return y;
    xanatoo::NodeId sec = xanatoo::enclosing_section(d, p.sel.focus.node);
    if (auto y = box_top_of(sec)) return y;
    for (xanatoo::NodeId id = p.sel.focus.node;
         id != xanatoo::INVALID_NODE && id < d.next_id; id = d.get(id).parent) {
      if (auto y = box_top_of(id)) return y;
    }
    // Last resort: caret baseline minus roughly one line so glyphs fit.
    if (auto cy = caret_doc_y(app, p, p.sel.focus)) {
      const float lh = static_cast<float>(app.fonts.regular.line_height());
      return std::max(0.f, *cy - lh);
    }
    return std::nullopt;
  };

  auto yopt = resolve_top();
  if (!yopt) {
    force_full_pane_layout(app, p);
    yopt = resolve_top();
  }
  if (!yopt) return;
  if (p.pages.on) {
    page_goto_spread(app, p, spread_at(p, *yopt));
    return;
  }
  // Small pad so the box edge is not flush under the chrome.
  const float margin = 8.f;
  const float view_h = std::max(80.f, static_cast<float>(p.h));
  float max_s = std::max(0.f, p.layout.content_height - view_h + 40.f);
  p.scroll_y = std::max(0.f, std::min(max_s, *yopt - margin));
  app.needs_layout = true;
  ensure_virtual_layout(app, p);
}

float pane_max_scroll(App& app, const Pane& p) {
  if (p.pages.on) {
    const size_t n = p.pages.map.spread_count();
    return spread_scroll(p, n > 0 ? n - 1 : 0);
  }
  const float view_h = static_cast<float>(p.h);
  return std::max(0.f, p.layout.content_height - view_h + 40.f);
}

void zoom_by(App& app, int steps) {
  if (steps == 0) return;
  keep_views(app);
  const int old_body_px = app.body_px;
  for (; steps > 0; --steps) {
    app.body_px = std::min(48, app.body_px + 1);
    app.heading_px = std::min(64, app.heading_px + 2);
  }
  for (; steps < 0; ++steps) {
    app.body_px = std::max(10, app.body_px - 1);
    app.heading_px = std::max(14, app.heading_px - 2);
  }
  apply_font_sizes(app);
  // Estimates: a full pass per pinch step is too slow for a whole book.
  const float scale =
      static_cast<float>(app.body_px) / static_cast<float>(old_body_px);
  for (Pane& p : app.panes)
    for (auto& [node, h] : p.height_cache) h *= scale;
  app.needs_layout = true;
}

void apply_font_sizes(App& app) {
  const int body = std::max(
      8, static_cast<int>(std::lround(app.body_px * app.style.body_scale)));
  const int heading =
      app.style.heading_scale > 0.f
          ? static_cast<int>(std::lround(body * app.style.heading_scale))
          : static_cast<int>(std::lround(app.heading_px * app.style.body_scale));
  app.fonts.set_sizes(body, heading);
  invalidate_side_layouts(app);
}

std::string body_font_path(const App& app) {
  if (!app.font_path.empty()) return app.font_path;
  return app.style.fonts.body.empty() ? app.env_font_path : std::string{};
}

// The sheet pins pick for the focused page, with the reader's colour /
// font sheets on top.
static xanatoo::StyleChoice effective_style_choice(App& app) {
  return {xanatoo::style_for_url(app.style_pins, path_of(app, focused_pane(app)),
                                 app.view_settings.style),
          app.view_settings.style_colors, app.view_settings.style_fonts};
}

static xanatoo::StyleChoice current_style_choice(const App& app) {
  const auto& style = app.style_before_preview ? *app.style_before_preview
                                               : app.style;
  return {style.name, style.colors_from, style.fonts_from};
}

static void install_style(App& app, xanatoo::DocStyle next) {
  const bool fonts_changed = !(next.fonts == app.style.fonts);
  const bool sizes_changed = next.body_scale != app.style.body_scale ||
                             next.heading_scale != app.style.heading_scale;
  app.style = std::move(next);
  if (fonts_changed &&
      !app.fonts.load(app.sdl, body_font_path(app), app.body_px,
                      app.heading_px, app.style.fonts)) {
    std::cerr << "Style fonts failed to load; using defaults\n";
    app.style.fonts = {};
    app.fonts.load(app.sdl, body_font_path(app), app.body_px, app.heading_px);
  }
  if (fonts_changed || sizes_changed) apply_font_sizes(app);
  apply_view_settings(app, false);
}

void preview_style(App& app,
                   const std::optional<std::pair<xanatoo::UiCommand,
                                                 std::string>>& hover) {
  if (hover == app.style_hover) return;
  app.style_hover = hover;
  if (hover) {
    auto choice = current_style_choice(app);
    const auto& [command, name] = *hover;
    using C = xanatoo::UiCommand;
    if (command == C::SelectStyle) choice.sheet = name;
    if (command == C::SelectStyleColors) choice.colors = name;
    if (command == C::SelectStyleFonts) choice.fonts = name;
    xanatoo::DocStyle next;
    if (xanatoo::load_style(choice, next)) {
      if (!app.style_before_preview) app.style_before_preview = app.style;
      install_style(app, std::move(next));
      return;
    }
  }
  if (app.style_before_preview) {
    install_style(app, std::move(*app.style_before_preview));
    app.style_before_preview.reset();
  }
}

// Explicit choices (set_default) save view.conf and report the result.
static bool set_style(App& app, const xanatoo::StyleChoice& choice,
                      bool set_default) {
  xanatoo::DocStyle next;
  std::string err;
  if (!xanatoo::load_style(choice, next, &err)) {
    flash_status(app, "Style: " + err);
    return false;
  }
  for (const std::string& w : next.warnings)
    std::cerr << "style warning: " << w << "\n";
  install_style(app, std::move(next));
  if (set_default && !xanatoo::save_view_settings(app.view_settings))
    std::cerr << "Could not persist view settings to "
              << xanatoo::view_settings_path() << "\n";
  std::string msg = "Style: " + app.style.title;
  auto title_of = [&](const std::string& name) {
    for (const xanatoo::StyleEntry& e : app.styles)
      if (e.name == name) return e.title;
    return name;
  };
  if (!app.style.colors_from.empty())
    msg += " · colours: " + title_of(app.style.colors_from);
  if (!app.style.fonts_from.empty())
    msg += " · fonts: " + title_of(app.style.fonts_from);
  if (!app.style.warnings.empty())
    msg += " (" + std::to_string(app.style.warnings.size()) +
           " warning(s), see stderr)";
  if (set_default) flash_status(app, msg);
  return true;
}

void apply_style(App& app, const std::string& name) {
  xanatoo::StyleChoice c = current_style_choice(app);
  c.sheet = name.empty() ? "default" : name;
  const std::string old = app.view_settings.style;
  app.view_settings.style = c.sheet;
  if (!set_style(app, c, true)) app.view_settings.style = old;
}

void apply_style_layer(App& app, unsigned group, const std::string& name) {
  const bool colors = group == xanatoo::kStyleColors;
  xanatoo::StyleChoice c = current_style_choice(app);
  (colors ? c.colors : c.fonts) = name;
  if (!name.empty()) {
    xanatoo::DocStyle check;
    std::string err;
    if (!xanatoo::load_style(name, check, &err)) {
      flash_status(app, "Style: " + err);
      return;
    }
  }
  std::string& saved =
      colors ? app.view_settings.style_colors : app.view_settings.style_fonts;
  const std::string old = saved;
  saved = name;
  if (!set_style(app, c, true)) saved = old;
}

void sync_pinned_style(App& app) {
  const xanatoo::StyleChoice wanted = effective_style_choice(app);
  // A sheet naming itself as the colour / font source is the plain sheet.
  auto layer = [&](const std::string& s) {
    return s == wanted.sheet ? std::string{} : s;
  };
  const auto current = current_style_choice(app);
  if (wanted.sheet != current.sheet ||
      layer(wanted.colors) != current.colors ||
      layer(wanted.fonts) != current.fonts) {
    xanatoo::DocStyle check;
    if (xanatoo::load_style(wanted.sheet, check)) {
      preview_style(app, std::nullopt);
      set_style(app, wanted, false);
    }
  }
}

void reload_styles(App& app) {
  app.styles = xanatoo::list_styles();
  set_style(app, current_style_choice(app), false);
  flash_status(app, "Reloaded style sheets");
}

void nav_page(App& app, bool up) {
  Pane& p = focused_pane(app);
  const float view_h = static_cast<float>(p.h);
  const float step = view_h * 0.85f;
  float max_s = std::max(0.f, p.layout.content_height - view_h + 40.f);
  if (up)
    p.scroll_y = std::max(0.f, p.scroll_y - step);
  else
    p.scroll_y = std::min(max_s, p.scroll_y + step);
  ensure_virtual_layout(app, p);
}

void nav_move_block(App& app, bool prev) {
  Pane& p = focused_pane(app);
  auto& d = doc_of(app, p);
  if (prev)
    xanatoo::move_to_prev_node(d, p.sel.focus);
  else
    xanatoo::move_to_next_node(d, p.sel.focus);
  p.sel.anchor = p.sel.focus;
  scroll_caret_into_view(app, p);
  reset_blink(app);
}

void nav_move_line(App& app, bool up) {
  Pane& p = focused_pane(app);
  auto& d = doc_of(app, p);
  if (up) {
    if (!xanatoo::move_up(p.layout, p.sel.focus, p.preferred_x))
      xanatoo::move_to_prev_node(d, p.sel.focus);
  } else {
    if (!xanatoo::move_down(p.layout, p.sel.focus, p.preferred_x))
      xanatoo::move_to_next_node(d, p.sel.focus);
  }
  p.sel.anchor = p.sel.focus;
  scroll_caret_into_view(app, p);
  reset_blink(app);
}

void nav_header(App& app, bool prev) {
  refresh_outline(app);
  if (app.outline.empty()) return;
  if (prev) {
    if (app.outline_sel > 0) --app.outline_sel;
  } else {
    if (app.outline_sel + 1 < static_cast<int>(app.outline.size()))
      ++app.outline_sel;
  }
  Pane& p = focused_pane(app);
  p.sel.focus.node = app.outline[static_cast<size_t>(app.outline_sel)].node;
  p.sel.focus.span_index = 0;
  p.sel.focus.char_index = 0;
  p.sel.anchor = p.sel.focus;
  scroll_caret_into_view(app, p);
  reset_blink(app);
}

bool toggle_stretch_at_caret(App& app) {
  Pane& p = focused_pane(app);
  const auto& d = doc_of(app, p);
  for (xanatoo::NodeId id = p.sel.focus.node;
       id != xanatoo::INVALID_NODE && id < d.next_id; id = d.get(id).parent) {
    if (d.get(id).kind == xanatoo::NodeKind::Stretch) {
      toggle_stretch(app, id);
      return true;
    }
  }
  // Also the innermost stretch (its box comes first) at the caret's height.
  auto cpos = xanatoo::caret_screen_pos(
      p.layout, p.sel.focus, p.scroll_y,
      static_cast<float>(app.fonts.regular.line_height()));
  if (cpos.valid) {
    for (const auto& box : p.layout.boxes) {
      if (box.node == xanatoo::INVALID_NODE || box.node >= d.next_id) continue;
      if (d.get(box.node).kind != xanatoo::NodeKind::Stretch) continue;
      if (cpos.y + p.scroll_y < box.y ||
          cpos.y + p.scroll_y > box.y + box.height)
        continue;
      toggle_stretch(app, box.node);
      return true;
    }
  }
  return false;
}

void collapse_or_expand_all(App& app, bool expand) {
  Pane& p = focused_pane(app);
  const auto& d = doc_of(app, p);
  std::vector<xanatoo::NodeId> ids = xanatoo::stretches(d, d.root, false);
  for (xanatoo::NodeId item : xanatoo::foldable_list_items(d, d.root))
    ids.push_back(item);
  keep_views(app);
  apply_folds(app, app.focus, ids, !expand);
  drop_height_hints_for_buf(app, p.buf);
  flash_status(app, expand ? "Expanded all" : "Collapsed all");
}

void jump_outline_sel(App& app) {
  if (app.outline.empty() || app.outline_sel < 0 ||
      app.outline_sel >= static_cast<int>(app.outline.size()))
    return;
  Pane& p = focused_pane(app);
  const auto& er = app.outline[static_cast<size_t>(app.outline_sel)];
  xanatoo::Document& d = doc_of(app, p);
  if (er.preamble) {
    // Title row: back to the very top (cover image, byline, …).
    if (p.section_focus) set_pane_cut(app, p, xanatoo::INVALID_NODE);
    if (app.needs_layout) layout_panes(app);
    p.sel = {};
    xanatoo::move_to_document_start(d, p.sel.focus);
    p.sel.anchor = p.sel.focus;
    p.scroll_y = 0.f;
    ensure_virtual_layout(app, p);  // the band may not reach the top
    reset_blink(app);
    return;
  }
  // Prefer the title Header; if the row only has a Section id (no header
  // child), land on the first text node under that section so
  // ensure_valid_caret does not relocate to the top of the document.
  xanatoo::NodeId dest = er.node;
  if (dest != xanatoo::INVALID_NODE && dest < d.next_id) {
    const auto& n = d.get(dest);
    if (n.id != dest || !xanatoo::is_text_node_kind(n.kind))
      dest = xanatoo::INVALID_NODE;
  } else {
    dest = xanatoo::INVALID_NODE;
  }
  if (dest == xanatoo::INVALID_NODE && er.section != xanatoo::INVALID_NODE) {
    std::vector<xanatoo::NodeId> texts;
    xanatoo::collect_text_nodes(d, er.section, texts);
    if (!texts.empty())
      dest = texts.front();
    else
      dest = er.section;
  }
  if (dest == xanatoo::INVALID_NODE) return;
  // Keep section-focus filter in sync with outline selection.
  if (p.section_focus) {
    xanatoo::NodeId sec = er.section;
    if (sec == xanatoo::INVALID_NODE) {
      sec = xanatoo::enclosing_section(d, dest);
    }
    set_pane_cut(app, p, sec);
  }
  expand_ancestors_for_node(app, dest);
  if (app.needs_layout) layout_panes(app);
  p.sel.focus = {dest, 0, 0};
  xanatoo::ensure_valid_caret(d, p.sel.focus);
  p.sel.anchor = p.sel.focus;
  // Outline jumps need a complete materialisation so box tops exist for
  // the target (virtualized bands often omit it until after scroll).
  if (!p.layout.boxes.empty()) {
    bool have_box = false;
    for (const auto& b : p.layout.boxes) {
      if (b.node == p.sel.focus.node) {
        have_box = true;
        break;
      }
    }
    if (!have_box) force_full_pane_layout(app, p);
  } else {
    force_full_pane_layout(app, p);
  }
  scroll_caret_to_top(app, p);
  reset_blink(app);
}

int pane_at(App& app, float mx, float my) {
  if (FloatWindow* w = float_at(app, static_cast<int>(mx), static_cast<int>(my));
      w && w->role == WindowRole::Document && view_live(app, w->view)) {
    const Pane& p = app.panes[w->view];
    if (mx >= static_cast<float>(p.x) && mx < static_cast<float>(p.x + p.w) &&
        my >= static_cast<float>(p.y) && my < static_cast<float>(p.y + p.h))
      return w->view;
  }
  if (!app.split) return 0;
  if (app.tile_axis == TileAxis::Rows)
    return (my >= static_cast<float>(app.panes[1].y - app.panes[1].head_h))
               ? 1 : 0;
  return (mx >= static_cast<float>(app.panes[1].x)) ? 1 : 0;
}

float local_x(App& app, float mx, int pi) {
  return mx - static_cast<float>(app.panes[pi].x);
}

float content_y(App& app, float window_y, int pi) {
  return window_y - static_cast<float>(app.panes[pi].y);
}

std::tuple<int, int, int, int> outline_client_rect(App& app) {
  return window_client_rect(app.ol);
}

float outline_row_step(App& app) {
  return static_cast<float>(std::max(18, app.ui_font.line_height() + 7));
}

OutlineRowGeom outline_row_geom(App& app, int level) {
  auto px = [&](int v) { return static_cast<int>(std::lround(v * app.ui_scale)); };
  OutlineRowGeom g;
  g.twist_w = px(14);
  g.twist_x = px(6) + std::max(0, level - 1) * px(14);
  g.text_x = g.twist_x + g.twist_w + px(2);
  return g;
}

}  // namespace xanatoo::app
