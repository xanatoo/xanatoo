// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// Help panel (WindowRole::HoverHelp): explains the GUI element under the
// pointer. ImGui widgets report themselves through ImGui's item hooks
// (Ui::hover_item); canvas chrome is found here with the hit-tests its
// clicks and cursor use. The texts live in ui_help.cpp.

#include <algorithm>

#include "xanatoo/app.hpp"
#include "xanatoo/parse.hpp"
#include "xanatoo/ui_help.hpp"

namespace xanatoo::app {

namespace {

// Session / help-key names of the panel roles.
const char* role_key(WindowRole role) {
  switch (role) {
    case WindowRole::Help: return "help";
    case WindowRole::Outline: return "outline";
    case WindowRole::Backlinks: return "backlinks";
    case WindowRole::Sitemap: return "sitemap";
    case WindowRole::Page: return "page";
    case WindowRole::History: return "history";
    case WindowRole::Styles: return "styles";
    case WindowRole::StyleManager: return "style-manager";
    case WindowRole::Bookmarks: return "bookmarks";
    case WindowRole::Downloads: return "downloads";
    case WindowRole::Media: return "media";
    case WindowRole::Reply: return "reply";
    case WindowRole::Annotations: return "annotations";
    case WindowRole::Context: return "context";
    case WindowRole::Revisions: return "revisions";
    case WindowRole::NostrExplorer: return "nostr-explorer";
    case WindowRole::NostrAccounts: return "nostr-accounts";
    case WindowRole::LemmyExplorer: return "lemmy-explorer";
    case WindowRole::Subscriptions: return "subscriptions";
    case WindowRole::Certificates: return "certificates";
    case WindowRole::RelatedSites: return "related-sites";
    case WindowRole::MatrixExplorer: return "matrix-explorer";
    case WindowRole::HoverHelp: return "hover-help";
    case WindowRole::Document: return "document";
  }
  return "document";
}

bool within(const SDL_Rect& r, int x, int y, int pad = 0) {
  return x >= r.x - pad && x < r.x + r.w + pad && y >= r.y - pad &&
         y < r.y + r.h + pad;
}

// Title bar of `w` (press_title's zones, in its order).
const char* title_key(App& app, const FloatWindow& w, int mx) {
  const SDL_Rect close = title_close_zone(w);
  if (mx >= w.x && mx < close.x + close.w) return "@float-close";
  if (mx >= title_max_zone(w).x) return "@float-max";
  if (mx >= title_shade_zone(w).x) return "@float-shade";
  if (w.role == WindowRole::Sitemap) {
    const SDL_Rect pb = title_button_rect(app, w, "Page");
    if (mx >= pb.x - 2 && mx < pb.x + pb.w + 2) return "@sitemap-page";
  }
  if (w.role == WindowRole::Outline) {
    const SDL_Rect fb = outline_focus_rect(app, w);
    if (mx >= fb.x - 2 && mx < fb.x + fb.w + 2) return "@outline-focus";
  }
  return w.role == WindowRole::Document ? "@doc-window-title" : "@float-title";
}

// The parts of a document view (centre tile or document window).
std::string pane_key(App& app, int pi, int mx, int my) {
  if (const char* k = gallery_help_key(app, pi, mx, my)) return k;
  const Pane& p = app.panes[pi];
  if (sb_reserve(p) > 0 && mx >= sb_x(p) && mx < sb_x(p) + kSbW &&
      my >= p.y && my < p.y + p.h)
    return "@scrollbar";
  const Hover& h = app.hover;
  if (h.col_gap) return "@col-gap";
  if (h.image_handle) return "@image-handle";
  if (h.grip != xanatoo::INVALID_NODE) return "@grip";
  if (!h.bookmark.empty()) return "@bookmark-mark";
  if (!h.annotation.empty()) return "@annotation-tab";
  if (h.quote >= 0) return "@quote-arrow";
  if (h.chrome_notice) return "@reader-notice";
  if (h.task != xanatoo::INVALID_NODE) return "@task";
  if (h.stretch != xanatoo::INVALID_NODE) return "@stretch";
  if (h.image != xanatoo::INVALID_NODE)
    return h.image_card ? "@image-card" : "@image";
  if (!h.href.empty()) return "@link";
  if (h.transclude != xanatoo::INVALID_NODE) return "@transclude";
  Pane& pp = app.panes[pi];
  if (pane_gallery(pp)) return "@gallery";
  return browsing(app) ? "@document" : "@document-edit";
}

void set_shown(App& app, std::string key, std::string tip,
               const xanatoo::UiHelpTopic* topic, std::string_view label) {
  HoverHelp& hh = app.hover_help;
  if (hh.has_doc && key == hh.key && tip == hh.tip) return;
  hh.key = std::move(key);
  hh.tip = std::move(tip);
  hh.doc = xanatoo::parse_document(
      xanatoo::ui_help_markdown(topic, label, hh.tip), {});
  hh.has_doc = true;
  hh.needs_layout = true;
  app.hover_help_win.scroll = 0.f;
}

// An ImGui label: its topic, else (for a real widget) the label itself.
bool show_label(App& app, const std::string& label, const std::string& tip,
                bool need_topic) {
  for (const std::string& k : xanatoo::ui_help_keys(label))
    if (const xanatoo::UiHelpTopic* t = xanatoo::find_ui_help(k)) {
      set_shown(app, k, tip, t, label);
      return true;
    }
  if (need_topic ||
      (xanatoo::ui_help_visible_label(label).empty() && tip.empty()))
    return false;
  set_shown(app, label, tip, nullptr, label);
  return true;
}

void show_key(App& app, const std::string& key) {
  set_shown(app, key, "", xanatoo::find_ui_help(key), key);
}

}  // namespace

const FloatWindow* float_resize_window_at(App& app, int x, int y) {
  if (!float_resize_at(app, x, y)) return nullptr;
  auto st = float_stack(app);
  for (auto it = st.rbegin(); it != st.rend(); ++it) {
    const FloatWindow& w = **it;
    const int reach = std::max(help_edge, dock_gap);
    if (x >= w.x - reach && x <= w.x + w.w + reach &&
        y >= w.y - reach && y <= w.y + w.h + reach)
      return &w;
  }
  return nullptr;
}

std::string hover_help_key_at(App& app, int mx, int my) {
  if (dock_split_at(app, mx, my) != DockSplitAxis::None) return "@dock-split";
  if (const FloatWindow* w = float_resize_window_at(app, mx, my))
    return float_docked(*w) ? "@dock-edge" : "@float-resize";
  if (FloatWindow* w = float_at(app, mx, my)) {
    if (my < w->y + float_title_h(*w)) return title_key(app, *w, mx);
    if (w->role != WindowRole::Document)
      return std::string("@panel:") + role_key(w->role);
    const int pi = pane_at(app, static_cast<float>(mx), static_cast<float>(my));
    return pi == w->view ? pane_key(app, pi, mx, my) : "@float-title";
  }
  if (status_bar_h > 0 &&
      my >= app.win_h - status_bar_h - static_cast<int>(app.ui_bottom) &&
      my < app.win_h - static_cast<int>(app.ui_bottom))
    return "@status-bar";
  if (app.split) {
    if (within(tile_mode_button_rect(app), mx, my)) return "@tile-mode";
    if (app.diff_mode && within(tile_diff_track_rect(app), mx, my))
      return "@tile-diff-track";
    if (within(tile_divider_rect(app), mx, my, 2)) return "@tile-divider";
  }
  for (int pi : live_views(app))
    if (const char* k = page_help_key(app, pi, mx, my)) return k;
  const int pi = pane_at(app, static_cast<float>(mx), static_cast<float>(my));
  if (pi >= 0) return pane_key(app, pi, mx, my);
  return {};
}

void toggle_hover_help(App& app) {
  app.hover_help.open = !app.hover_help.open;
  if (app.hover_help.open) {
    show_key(app, "@panel:hover-help");
  } else if (float_active(app, app.hover_help_win)) {
    app.key_focus = KeyFocus::Document;
  }
  app.ui.set_hover_tracking(app.hover_help.open);
}

void update_hover_help(App& app) {
  app.ui.set_hover_tracking(app.hover_help.open);
  if (!app.hover_help.open) return;
  const int mx = static_cast<int>(app.mouse_x);
  const int my = static_cast<int>(app.mouse_y);
  if (mx < 0 || my < 0 || mx >= app.win_w || my >= app.win_h) return;
  // Inside its own client area the text holds still to be read / scrolled.
  const FloatWindow* top = float_at(app, mx, my);
  if (top == &app.hover_help_win && my >= top->y + float_title_h(*top) &&
      !float_resize_window_at(app, mx, my))
    return;
  const xanatoo::UiHoverItem& item = app.ui.hover_item();
  if (!item.label.empty() && show_label(app, item.label, item.tip, false))
    return;
  const bool imgui_owns = app.ui.captures_point(
      app.mouse_x, app.mouse_y, float_layer_at(app, mx, my));
  if (imgui_owns && !top) {
    // Menu bar, toolbars, popups: the window, when it has a text.
    if (!item.window.empty()) show_label(app, item.window, "", true);
    return;
  }
  const std::string key = hover_help_key_at(app, mx, my);
  if (!key.empty()) show_key(app, key);
}

void draw_hover_help_window(App& app) {
  HoverHelp& hh = app.hover_help;
  FloatWindow& w = app.hover_help_win;
  if (!hh.open) return;
  if (w.dock == Dock::None) {
    w.w = std::max(w.w, 240);
    w.h = std::max(w.h, 160);
  }
  if (!hh.has_doc) show_key(app, "@panel:hover-help");
  auto [cx, cy, cw, ch] = float_client_rect(app, w);
  if (hh.needs_layout || hh.layout_w != cw) {
    hh.layout = app.layout_engine.layout(
        hh.doc, static_cast<float>(std::max(120, cw)));
    hh.layout_w = cw;
    hh.needs_layout = false;
  }
  const float max_s =
      std::max(0.f, hh.layout.content_height - static_cast<float>(ch) + 24.f);
  w.scroll = std::clamp(w.scroll, 0.f, max_s);
  if (!draw_floating_frame(app, w, "Help", float_active(app, w))) return;
  SDL_Rect clip{cx, cy, cw, ch};
  xanatoo::sdl::set_clip_rect(app.sdl, &clip);
  const float draw_scroll = w.scroll - static_cast<float>(cy);
  app.renderer.draw_backdrop(hh.layout, clip, draw_scroll,
                             static_cast<float>(cx));
  app.renderer.draw(hh.layout, draw_scroll, static_cast<float>(cx));
  xanatoo::sdl::set_clip_rect(app.sdl, nullptr);
  draw_client_scrollbar(app, w, max_s);
}

}  // namespace xanatoo::app
