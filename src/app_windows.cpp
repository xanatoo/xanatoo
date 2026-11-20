// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// Floating windows (help / outline / backlinks / document): one z-ordered
// stack with shared hit-testing, drag, resize, scroll and draw dispatch.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

#include "xanatoo/app.hpp"
#include "xanatoo/cli.hpp"
#include "xanatoo/layout.hpp"
#include "xanatoo/theme.hpp"

namespace xanatoo::app {

namespace {


// Smallest free-floating size per role.
std::pair<int, int> min_size(const FloatWindow& w) {
  switch (w.role) {
    case WindowRole::Outline:
    case WindowRole::Sitemap:
      return {200, 140};
    case WindowRole::Page:
      return {160, 120};
    case WindowRole::History:
    case WindowRole::Downloads:
    case WindowRole::Backlinks:
      return {200, 120};
    case WindowRole::Media:
      return {260, 150};
    case WindowRole::Styles:
      return {220, 240};
    case WindowRole::StyleManager:
      return {420, 310};
    case WindowRole::Bookmarks:
    case WindowRole::Subscriptions:
      return {240, 180};
    case WindowRole::Context:
      return {280, 200};
    case WindowRole::HoverHelp:
      return {240, 160};
    case WindowRole::Reply:
      return {300, 240};
    case WindowRole::Annotations:
      return {260, 260};
    case WindowRole::Revisions:
      return {300, 240};
    case WindowRole::NostrExplorer:
    case WindowRole::LemmyExplorer:
    case WindowRole::MatrixExplorer:
      return {340, 260};
    case WindowRole::NostrAccounts:
    case WindowRole::Certificates:
    case WindowRole::RelatedSites:
      return {300, 240};
    case WindowRole::Help:
    case WindowRole::Document:
      break;
  }
  return {280, 160};
}

// Pointer travel before a title press on a docked window tears it off.
constexpr int kTearOffPx = 8;

bool contains(const FloatWindow& w, int mx, int my) {
  return mx >= w.x && mx < w.x + w.w && my >= w.y && my < w.y + w.h;
}

// Backlinks rows live in a padded client area just below the title.
std::tuple<int, int, int, int> backlinks_client_rect(const FloatWindow& w) {
  // Same inset as every other window (outline / help / page / sitemap).
  return window_client_rect(w);
}

float scroll_wheel_step(const FloatWindow& w) {
  return w.role == WindowRole::Help ? 48.f : 40.f;
}

// ImGui widgets own the client area (and its scrolling) of these windows.
// The Context panel only falls back to ImGui while it has no document;
// with one it draws natively and scrolls like Help.
bool imgui_scrolls(const App& app, const FloatWindow& w) {
  switch (w.role) {
    case WindowRole::Context:
      return !app.context_has_doc;
    case WindowRole::Annotations:
    case WindowRole::Reply:
    case WindowRole::Styles:
    case WindowRole::StyleManager:
    case WindowRole::Bookmarks:
    case WindowRole::Subscriptions:
    case WindowRole::NostrExplorer:
    case WindowRole::LemmyExplorer:
    case WindowRole::MatrixExplorer:
    case WindowRole::NostrAccounts:
    case WindowRole::Certificates:
    case WindowRole::RelatedSites:
      return true;
    default:
      return false;
  }
}

void clamp_scroll_min(FloatWindow& w) {
  if (w.scroll < 0.f) w.scroll = 0.f;
}

void close_window(App& app, FloatWindow& w) {
  w.dragging = false;
  w.resize = 0;
  w.scroll_dragging = false;
  if (w.role != WindowRole::Document && float_active(app, w))
    app.key_focus = KeyFocus::Document;
  switch (w.role) {
    case WindowRole::Help:
      app.help_mode = false;
      break;
    case WindowRole::Outline:
      app.outline_mode = false;
      break;
    case WindowRole::Backlinks:
      app.backlinks_mode = false;
      break;
    case WindowRole::Sitemap:
      toggle_sitemap(app);
      break;
    case WindowRole::Page:
      toggle_page_panel(app);
      break;
    case WindowRole::History:
      app.history_panel_open = false;
      if (app.key_focus == KeyFocus::History)
        app.key_focus = KeyFocus::Document;
      break;
    case WindowRole::Styles:
      app.styles_panel_open = false;
      if (app.key_focus == KeyFocus::Styles)
        app.key_focus = KeyFocus::Document;
      break;
    case WindowRole::StyleManager:
      app.styles_manager_panel_open = false;
      if (app.key_focus == KeyFocus::StyleManager)
        app.key_focus = KeyFocus::Document;
      break;
    case WindowRole::Bookmarks:
      app.bookmarks_panel_open = false;
      break;
    case WindowRole::Context:
      app.context_panel_open = false;
      app.context_pending_url.clear();
      if (app.key_focus == KeyFocus::Context)
        app.key_focus = KeyFocus::Document;
      break;
    case WindowRole::Subscriptions:
      app.subscriptions_panel_open = false;
      break;
    case WindowRole::Downloads:
      app.downloads_panel_open = false;
      break;
    case WindowRole::Media:
      close_media_panel(app);  // closing the player stops the sound
      break;
    case WindowRole::Reply:
      app.reply.open = false;
      break;
    case WindowRole::Annotations:
      app.annotation_panel_open = false;
      break;
    case WindowRole::Revisions:
      app.revision_panel_open = false;
      break;
    case WindowRole::NostrExplorer:
      app.nostr_explorer_open = false;
      break;
    case WindowRole::LemmyExplorer:
      app.lemmy_explorer_open = false;
      break;
    case WindowRole::MatrixExplorer:
      app.matrix_explorer_open = false;
      break;
    case WindowRole::NostrAccounts:
      app.nostr_accounts_open = false;
      break;
    case WindowRole::Certificates:
      app.certs.open = false;
      break;
    case WindowRole::RelatedSites:
      app.related.open = false;
      break;
    case WindowRole::HoverHelp:
      app.hover_help.open = false;
      app.ui.set_hover_tracking(false);
      break;
    case WindowRole::Document:
      close_doc_window(app, static_cast<size_t>(&w - app.soft_wins.data()));
      break;
  }
}

void begin_resize(FloatWindow& w, int bits, int mx, int my) {
  w.resize = bits;
  w.resize_start_x = mx;
  w.resize_start_y = my;
  w.resize_start_w = w.w;
  w.resize_start_h = w.h;
  w.resize_start_wx = w.x;
  w.resize_start_wy = w.y;
  w.dragging = false;
}

void begin_drag(FloatWindow& w, int mx, int my) {
  w.dragging = true;
  w.drag_dx = mx - w.x;
  w.drag_dy = my - w.y;
  w.press_x = mx;
  w.press_y = my;
}

// Scroll offset that puts the thumb's top at `top` (screen y).
static float scroll_for_thumb_top(App& app, const FloatWindow& w, float top) {
  const SDL_Rect sb = client_scrollbar_rect(app, w);
  const float max_s = float_max_scroll(app, w);
  float track = 0.f;
  xanatoo::theme::vscroll_thumb(sb, w.scroll, max_s,
                                static_cast<float>(sb.h), &track);
  if (track <= 0.f) return 0.f;
  const float t = (top - static_cast<float>(sb.y + 1)) / track;
  return std::clamp(t, 0.f, 1.f) * max_s;
}

// Press on the scrollbar strip: the thumb is grabbed where it was hit; a
// press off the thumb first centres it under the pointer, then grabs it.
// Uses the thumb draw_client_scrollbar paints.
void begin_scroll_drag(App& app, FloatWindow& w, int my) {
  const SDL_Rect sb = client_scrollbar_rect(app, w);
  const float max_s = float_max_scroll(app, w);
  SDL_Rect thumb = xanatoo::theme::vscroll_thumb(sb, w.scroll, max_s,
                                                 static_cast<float>(sb.h));
  if (my < thumb.y || my >= thumb.y + thumb.h) {
    w.scroll = scroll_for_thumb_top(
        app, w, static_cast<float>(my) - static_cast<float>(thumb.h) * 0.5f);
    thumb = xanatoo::theme::vscroll_thumb(sb, w.scroll, max_s,
                                          static_cast<float>(sb.h));
  }
  w.scroll_grab = static_cast<float>(my - thumb.y);
  w.scroll_dragging = true;
}

// ---- role-specific client actions ----

void click_backlinks(App& app, FloatWindow& w, int my) {
  const float step = backlinks_row_step(app);
  auto [cx, cy, cw, ch] = backlinks_client_rect(w);
  const int row =
      static_cast<int>((static_cast<float>(my - cy) + w.scroll) / step);
  if (row < 0 || row >= static_cast<int>(app.backlinks.size())) return;
  app.backlinks_sel = row;
  auto& pp = focused_pane(app);
  pp.sel.focus = {app.backlinks[static_cast<size_t>(row)].source, 0, 0};
  pp.sel.anchor = pp.sel.focus;
  expand_ancestors_for_node(app, pp.sel.focus.node);
  scroll_caret_into_view(app, pp);
  reset_blink(app);
  app.key_focus = KeyFocus::Document;
}

void click_outline(App& app, FloatWindow& w, int mx, int my) {
  auto [cx, cy, cw, ch] = float_client_rect(app, w);
  if (mx < cx || mx >= cx + cw || my < cy || my >= cy + ch) return;
  const float step = outline_row_step(app);
  const int row =
      static_cast<int>((static_cast<float>(my - cy) + w.scroll) / step);
  if (row < 0 || row >= static_cast<int>(app.outline.size())) return;
  app.outline_sel = row;
  const auto& er = app.outline[static_cast<size_t>(row)];
  // Disclosure hit only on the triangle (a little slack), not the text.
  const int local_x = mx - cx;
  const OutlineRowGeom g = outline_row_geom(app, er.level);
  const bool on_twist = er.has_children && local_x >= g.twist_x - 4 &&
                        local_x < g.twist_x + g.twist_w + 2;
  if (on_twist && er.section != xanatoo::INVALID_NODE) {
    toggle_section_fold(app, er.section);
  } else {
    jump_outline_sel(app);
    app.key_focus = KeyFocus::Document;  // clicked a heading: edit there
  }
}

// Stretch toggle / link follow inside the Help client area.
void click_help(App& app, FloatWindow& w, int mx, int my) {
  auto [cx, cy, cw, ch] = float_client_rect(app, w);
  const float lx = static_cast<float>(mx - cx);
  const float ly = static_cast<float>(my - cy);
  if (lx < 0 || ly < 0 || lx > cw || ly > ch) return;
  if (auto sid = xanatoo::hit_test_stretch(app.help_layout, lx, ly, w.scroll)) {
    if (app.help_doc.get(*sid).kind == xanatoo::NodeKind::Stretch) {
      app.help_doc.get(*sid).expanded = !app.help_doc.get(*sid).expanded;
      app.help_needs_layout = true;
      return;
    }
  }
  if (auto href = xanatoo::hit_test_link(app.help_doc, app.help_layout, lx, ly,
                                         w.scroll)) {
    if (!href->empty() && (*href)[0] == '#') {
      if (auto dest = xanatoo::jump_to_href(app.help_doc, *href)) {
        auto cpos = xanatoo::caret_screen_pos(
            app.help_layout, *dest, 0.f,
            static_cast<float>(app.fonts.regular.line_height()));
        if (cpos.valid) w.scroll = std::max(0.f, cpos.y - 24.f);
      }
    }
  }
}

// Context panel client point (mx, my) in layout coordinates; false when
// the panel shows no laid-out document or the point is outside.
bool context_local(App& app, int mx, int my, float& lx, float& ly,
                   bool clamp = false) {
  if (!app.context_panel_open || !app.context_has_doc ||
      !app.context_image_src.empty())
    return false;
  if (app.context_needs_layout) layout_context(app);
  auto [cx, cy, cw, ch] = float_client_rect(app, app.context_win);
  lx = static_cast<float>(mx - cx);
  ly = static_cast<float>(my - cy);
  if (clamp) {
    lx = std::clamp(lx, 0.f, static_cast<float>(cw));
    ly = std::clamp(ly, 0.f, static_cast<float>(ch));
    return true;
  }
  return lx >= 0 && ly >= 0 && lx <= cw && ly <= ch;
}

// Caret at a Context panel point; off the text, the nearest line's.
std::optional<xanatoo::Caret> context_caret(App& app, float lx, float ly) {
  const float scroll = app.context_win.scroll;
  if (auto c = xanatoo::hit_test(app.context_layout, lx, ly, scroll)) return c;
  return xanatoo::nearest_caret(app.context_layout, lx, ly + scroll);
}

// Context panel body: links open in the focused view; elsewhere a press
// starts a text selection (Shift extends it), stretchtext toggles.
void press_context(App& app, int mx, int my) {
  float lx = 0.f, ly = 0.f;
  if (!context_local(app, mx, my, lx, ly)) return;
  const bool shift = (SDL_GetModState() & KMOD_SHIFT) != 0;
  if (!shift) {
    if (auto hit = context_link_at(app, mx, my)) {
      follow_context_link(app, hit->href);
      return;
    }
  }
  if (app.click_new_tab) return;  // middle-click: links only
  const float scroll = app.context_win.scroll;
  if (!shift)
    if (auto sid = xanatoo::hit_test_stretch(app.context_layout, lx, ly, scroll)) {
      xanatoo::Node& n = app.context_doc.get(*sid);
      if (n.kind == xanatoo::NodeKind::Stretch) {
        n.expanded = !n.expanded;
        app.context_needs_layout = true;
        app.context_sel = {};
        return;
      }
    }
  const auto c = context_caret(app, lx, ly);
  if (!c) {
    app.context_sel = {};
    return;
  }
  if (!shift || app.context_sel.anchor.node == xanatoo::INVALID_NODE)
    app.context_sel.anchor = *c;
  app.context_sel.focus = *c;
  app.context_selecting = true;
}

void press_title(App& app, FloatWindow& w, int mx, int my) {
  if (mx >= w.x && mx < title_close_zone(w).x + title_close_zone(w).w) {
    close_window(app, w);
    return;
  }
  if (mx >= title_max_zone(w).x) {
    toggle_maximize(app, w);
    w.last_title_click = 0;
    return;
  }
  if (mx >= title_shade_zone(w).x) {
    toggle_shade(app, w);  // greyed out (no-op) unless docked
    w.last_title_click = 0;
    return;
  }
  if (w.role == WindowRole::Sitemap) {
    // "Page" button just left of the maximize box.
    const SDL_Rect pb = title_button_rect(app, w, "Page");
    if (mx >= pb.x - 2 && mx < pb.x + pb.w + 2) {
      click_sitemap(app, mx, my);
      return;
    }
  }
  if (w.role == WindowRole::Outline) {
    // Focus toggle sits just left of the maximize box.
    const SDL_Rect fb = outline_focus_rect(app, w);
    if (mx >= fb.x - 2 && mx < fb.x + fb.w + 2) {
      toggle_section_focus_mode(app);
      return;
    }
  }
  // Double-click maximizes / restores; a drag moves the window (a docked
  // or maximized one tears off first, see float_motion).
  const Uint32 now = xanatoo::sdl::ticks();
  if (w.last_title_click != 0 && now - w.last_title_click < 400) {
    toggle_maximize(app, w);
    w.last_title_click = 0;
    return;
  }
  w.last_title_click = now;
  begin_drag(w, mx, my);
  app.dock_preview = Dock::None;
}

// Docked / maximized window dragged by its title: float it again at its
// free size, under the pointer.
void tear_off(App& app, FloatWindow& w, int mx, int my) {
  undock_window(app, w);
  w.x = std::max(0, mx - w.w / 2);
  w.y = std::max(static_cast<int>(app.ui_top), my - float_title_h(w) / 2);
  w.drag_dx = mx - w.x;
  w.drag_dy = my - w.y;
}

void press_client(App& app, FloatWindow& w, int mx, int my) {
  // Scrollbar strip (out to the window edge when docked against it): only
  // natively scrolled roles reserve it for click-free jump/drag.
  const SDL_Rect sb = client_scrollbar_rect(app, w);
  const bool in_sb = mx >= sb.x + 2 && mx <= sb.x + sb.w + 2 &&
                     my >= sb.y && my <= sb.y + sb.h;
  if (in_sb && !imgui_scrolls(app, w)) {
    begin_scroll_drag(app, w, my);
    return;
  }
  switch (w.role) {
    case WindowRole::Backlinks:
      click_backlinks(app, w, my);
      break;
    case WindowRole::History:
      click_history_panel(app, w, my);
      break;
    case WindowRole::Styles:
    case WindowRole::StyleManager: {
      // ImGui owns the list / editor.
      break;
    }
    case WindowRole::Downloads:
      click_downloads_panel(app, mx, my);
      break;
    case WindowRole::Media:
      click_media_panel(app, mx, my);
      break;
    case WindowRole::Reply:
      break;  // ImGui owns the editor and formatting controls.
    case WindowRole::Annotations:
      break;  // ImGui owns the form widgets inside the native panel.
    case WindowRole::Bookmarks:
      break;  // ImGui owns the bookmark list inside the native panel.
    case WindowRole::Context:
      press_context(app, mx, my);  // ImGui owns it while there is no page
      break;
    case WindowRole::Subscriptions:
      break;  // ImGui owns subscription controls in its native window.
    case WindowRole::NostrExplorer:
    case WindowRole::LemmyExplorer:
    case WindowRole::MatrixExplorer:
    case WindowRole::NostrAccounts:
    case WindowRole::Certificates:
    case WindowRole::RelatedSites:
      break;  // ImGui supplies controls inside the native dockable panels.
    case WindowRole::Revisions:
      click_revision_panel(app, mx, my);
      break;
    case WindowRole::Outline:
      click_outline(app, w, mx, my);
      break;
    case WindowRole::Sitemap:
      click_sitemap(app, mx, my);
      break;
    case WindowRole::Page:
      click_page(app, mx, my);
      break;
    case WindowRole::Help:
      click_help(app, w, mx, my);
      break;
    case WindowRole::HoverHelp:
      break;  // text only
    case WindowRole::Document:
      break;  // the view takes its clicks through the pane path
  }
}

}  // namespace

std::optional<xanatoo::LinkHit> context_link_at(App& app, int mx, int my) {
  float lx = 0.f, ly = 0.f;
  if (!context_local(app, mx, my, lx, ly)) return std::nullopt;
  // Only the topmost window under the pointer takes it.
  if (float_at(app, mx, my) != &app.context_win) return std::nullopt;
  return xanatoo::hit_test_link_ex(app.context_doc, app.context_layout, lx, ly,
                                   app.context_win.scroll);
}

KeyFocus float_key_focus(const FloatWindow& w) {
  switch (w.role) {
    case WindowRole::Outline: return KeyFocus::Outline;
    case WindowRole::Backlinks: return KeyFocus::Backlinks;
    case WindowRole::Sitemap: return KeyFocus::Sitemap;
    case WindowRole::Page: return KeyFocus::Page;
    case WindowRole::History: return KeyFocus::History;
    case WindowRole::Styles: return KeyFocus::Styles;
    case WindowRole::StyleManager: return KeyFocus::StyleManager;
    case WindowRole::Context: return KeyFocus::Context;
    case WindowRole::Document: return KeyFocus::Document;
    default: return KeyFocus::Panel;
  }
}

void focus_float(App& app, const FloatWindow& w) {
  if (float_shaded(w)) {
    app.key_focus = KeyFocus::Document;
    return;
  }
  app.key_focus = float_key_focus(w);
  if (app.key_focus == KeyFocus::Panel) app.panel_focus = w.role;
}

bool float_active(const App& app, const FloatWindow& w) {
  const KeyFocus kf = float_key_focus(w);
  if (w.role == WindowRole::Document)
    return app.key_focus == KeyFocus::Document && app.focus == w.view;
  if (kf == KeyFocus::Panel)
    return app.key_focus == KeyFocus::Panel && app.panel_focus == w.role;
  return panel_has_keys(app, kf);
}

std::vector<FloatWindow*> float_stack(App& app) {
  std::vector<FloatWindow*> st;
  if (app.outline_mode) st.push_back(&app.ol);
  if (app.backlinks_mode) st.push_back(&app.bl);
  if (app.sitemap.shown) st.push_back(&app.sm);
  if (app.page.shown) st.push_back(&app.pg);
  if (app.history_panel_open) st.push_back(&app.history_win);
  if (app.annotation_panel_open) st.push_back(&app.annotation_win);
  if (app.revision_panel_open) st.push_back(&app.revision_win);
  if (app.nostr_explorer_open) st.push_back(&app.nostr_explorer_win);
  if (app.lemmy_explorer_open) st.push_back(&app.lemmy_explorer_win);
  if (app.matrix_explorer_open) st.push_back(&app.matrix_explorer_win);
  if (app.nostr_accounts_open) st.push_back(&app.nostr_accounts_win);
  if (app.certs.open) st.push_back(&app.certs_win);
  if (app.related.open) st.push_back(&app.related_win);
  if (app.styles_panel_open) st.push_back(&app.styles_win);
  if (app.styles_manager_panel_open) st.push_back(&app.styles_manager_win);
  if (app.bookmarks_panel_open) st.push_back(&app.bookmarks_win);
  if (app.context_panel_open) st.push_back(&app.context_win);
  if (app.subscriptions_panel_open) st.push_back(&app.subscriptions_win);
  if (app.help_mode) st.push_back(&app.help);
  if (app.hover_help.open) st.push_back(&app.hover_help_win);
  for (auto& sw : app.soft_wins) st.push_back(&sw);
  // Auto-opened downloads should remain visible above detached document
  // windows, just like a browser's downloads popover.
  if (app.downloads_panel_open) st.push_back(&app.downloads_win);
  if (app.media_panel_open) st.push_back(&app.media_win);
  if (app.reply.open) st.push_back(&app.reply_win);
  // Docked columns under floating windows, maximized ones on top.
  auto layer = [](const FloatWindow* w) {
    return w->dock == Dock::None ? 1 : w->dock == Dock::Max ? 2 : 0;
  };
  std::stable_sort(st.begin(), st.end(),
                   [&](const FloatWindow* a, const FloatWindow* b) {
                     if (layer(a) != layer(b)) return layer(a) < layer(b);
                     return a->z < b->z;
                   });
  return st;
}

void raise_float(App& app, FloatWindow& w) {
  w.z = ++app.float_z_top;
}

void lower_float(App& app, FloatWindow& w) {
  w.z = --app.float_z_bottom;
  if (w.role != WindowRole::Document) return;
  // Keep soft_wins back→front; `w` is stale afterwards.
  const size_t i = static_cast<size_t>(&w - app.soft_wins.data());
  if (i == 0 || i >= app.soft_wins.size()) return;
  FloatWindow sw = std::move(app.soft_wins[i]);
  app.soft_wins.erase(app.soft_wins.begin() + static_cast<std::ptrdiff_t>(i));
  app.soft_wins.insert(app.soft_wins.begin(), std::move(sw));
}

FloatWindow* float_at(App& app, int x, int y) {
  auto st = float_stack(app);
  for (auto it = st.rbegin(); it != st.rend(); ++it)
    if (contains(**it, x, y)) return *it;
  return nullptr;
}

std::optional<UiPanelLayer> float_layer_at(App& app, int x, int y) {
  if (const FloatWindow* w = float_at(app, x, y))
    return embedded_panel_layer(w->role);
  return std::nullopt;
}

// Keep pointer feedback and actual resize initiation in sync. The small
// outside-edge tolerance is intentional: thin frame edges remain grabbable.
static int float_resize_bits(const FloatWindow& w, int mx, int my) {
  if (float_shaded(w)) return 0;  // the dock is resized from its open panels
  // A docked panel's outer edge is grabbed in the gutter beside it (plus a
  // pixel of frame), never over its own or a pane's scrollbar.
  const bool in_v = my >= w.y && my < w.y + w.h;
  const bool in_h = mx >= w.x && mx < w.x + w.w;
  switch (w.dock) {
    case Dock::Left:
      return in_v && mx >= w.x + w.w - 2 && mx < w.x + w.w + dock_gap ? 1 : 0;
    case Dock::Right:
      return in_v && mx >= w.x - dock_gap && mx < w.x + 2 ? 4 : 0;
    case Dock::Top:
      return in_h && my >= w.y + w.h - 2 && my < w.y + w.h + dock_gap ? 2 : 0;
    case Dock::Bottom:
      return in_h && my >= w.y - dock_gap && my < w.y + 2 ? 8 : 0;
    case Dock::Max: return 0;
    case Dock::None: break;
  }
  if (mx < w.x - help_edge || mx > w.x + w.w + help_edge ||
      my < w.y - help_edge || my > w.y + w.h + help_edge)
    return 0;
  // Free windows: the scrollbar sits flush on the right frame, so that edge
  // reaches in by two pixels only; the left and top edges reach in by two
  // as well (the top one is the title bar, which otherwise moves the window).
  const bool on_t = my < w.y + 2;
  const bool on_l = mx < w.x + 2;
  const bool on_r = mx >= w.x + w.w - 2 &&
                    (my >= w.y + float_title_h(w) || on_t ||
                     mx > w.x + w.w);
  const bool on_b = my >= w.y + w.h - help_edge;
  return (on_r ? 1 : 0) | (on_b ? 2 : 0) | (on_l ? 4 : 0) | (on_t ? 8 : 0);
}

int float_resize_at(App& app, int mx, int my) {
  auto st = float_stack(app);
  for (auto it = st.rbegin(); it != st.rend(); ++it) {
    const FloatWindow& w = **it;
    const int bits = float_resize_bits(w, mx, my);
    if (bits || contains(w, mx, my)) return bits;
  }
  return 0;
}

std::tuple<int, int, int, int> float_client_rect(App& app,
                                                 const FloatWindow& w) {
  switch (w.role) {
    case WindowRole::Help:
      return help_client_rect(app);
    case WindowRole::Outline:
      return outline_client_rect(app);
    case WindowRole::Backlinks:
      return backlinks_client_rect(w);
    case WindowRole::Sitemap:
      return sitemap_client_rect(app);
    case WindowRole::Page:
      return page_client_rect(app);
    case WindowRole::History:
      return window_client_rect(w);
    case WindowRole::Styles:
    case WindowRole::StyleManager:
      return window_client_rect(w);
    case WindowRole::Bookmarks:
    case WindowRole::Subscriptions:
      return window_client_rect(w);
    case WindowRole::Context:
    case WindowRole::HoverHelp:
      return window_client_rect(w);
    case WindowRole::Downloads:
    case WindowRole::Media:
    case WindowRole::Reply:
    case WindowRole::Annotations:
    case WindowRole::Revisions:
    case WindowRole::NostrExplorer:
    case WindowRole::LemmyExplorer:
    case WindowRole::MatrixExplorer:
    case WindowRole::NostrAccounts:
    case WindowRole::Certificates:
    case WindowRole::RelatedSites:
      return window_client_rect(w);
    case WindowRole::Document:
      return soft_client_rect(app, w);
  }
  return {0, 0, 0, 0};
}

bool float_flush_right(const App& app, const FloatWindow& w) {
  return w.dock != Dock::None && w.x + w.w >= app.win_w;
}

SDL_Rect client_scrollbar_rect(App& app, const FloatWindow& w) {
  auto [cx, cy, cw, ch] = float_client_rect(app, w);
  const int right = float_flush_right(app, w) ? w.x + w.w : cx + cw;
  return SDL_Rect{right - 12, cy, 12, ch};
}

std::tuple<int, int, int, int> imgui_panel_rect(App& app, const FloatWindow& w) {
  auto [x, y, cw, h] = float_client_rect(app, w);
  // ImGui keeps a 1 px border right of its scrollbar: push that off screen.
  if (float_flush_right(app, w)) cw = app.win_w + 1 + 3 - x;
  return {x, y, cw, h};
}

float float_max_scroll(App& app, const FloatWindow& w) {
  auto [cx, cy, cw, ch] = float_client_rect(app, w);
  const float view_h = static_cast<float>(ch);
  switch (w.role) {
    case WindowRole::Help:
      return std::max(0.f, app.help_layout.content_height - view_h + 40.f);
    case WindowRole::HoverHelp:
      return std::max(0.f,
                      app.hover_help.layout.content_height - view_h + 24.f);
    case WindowRole::Document:
      return view_live(app, w.view) ? pane_max_scroll(app, app.panes[w.view])
                                    : 0.f;
    case WindowRole::Outline:
      return std::max(
          0.f, static_cast<float>(app.outline.size()) * outline_row_step(app) +
                   8.f - view_h);
    case WindowRole::Backlinks:
      return std::max(0.f, static_cast<float>(app.backlinks.size()) *
                                   backlinks_row_step(app) +
                               8.f - view_h);
    case WindowRole::History:
      return history_panel_max_scroll(app, w);
    case WindowRole::Styles:
    case WindowRole::StyleManager:
      return 0.f;  // ImGui handles scrolling.
    case WindowRole::Bookmarks:
    case WindowRole::Subscriptions:
      return 0.f;  // ImGui list manages its own vertical scrolling.
    case WindowRole::Context:
      if (app.context_has_doc && !app.context_image_src.empty())
        return 0.f;  // a fitted picture
      if (app.context_has_doc)
        return std::max(0.f, app.context_layout.content_height - view_h + 40.f);
      return 0.f;  // ImGui text fallback
    case WindowRole::Reply:
    case WindowRole::Annotations:
    case WindowRole::NostrExplorer:
    case WindowRole::LemmyExplorer:
    case WindowRole::MatrixExplorer:
    case WindowRole::NostrAccounts:
    case WindowRole::Certificates:
    case WindowRole::RelatedSites:
    case WindowRole::Media:
      return 0.f;
    case WindowRole::Revisions:
      return revision_panel_max_scroll(app, ch);
    case WindowRole::Downloads: {
      size_t n = 0;
      for (const auto& d : app.downloads)
        if (d->phase.load() != xanatoo::DownloadPhase::Pending &&
            d->phase.load() != xanatoo::DownloadPhase::Skipped) ++n;
      return std::max(0.f, static_cast<float>(n *
          std::max(58, app.ui_font.line_height() * 2 + 20) + 8) - view_h);
    }
    case WindowRole::Sitemap:
      return sitemap_max_scroll(app);
    case WindowRole::Page:
      return page_max_scroll(app);
  }
  return 0.f;
}

bool float_press(App& app, int mx, int my) {
  auto st = float_stack(app);
  for (auto it = st.rbegin(); it != st.rend(); ++it) {
    FloatWindow* w = *it;
    if (mx < w->x - help_edge || mx > w->x + w->w + help_edge ||
        my < w->y - help_edge || my > w->y + w->h + help_edge)
      continue;
    const bool inside = contains(*w, mx, my);
    // Free windows resize at any edge; docked ones at the inner edge.
    // This is also used by hover cursor feedback.
    const int bits = float_resize_bits(*w, mx, my);
    if (!bits && !inside) continue;
    // Clicking a panel gives it the keyboard; any other window, the document.
    // A rolled-up panel has nothing to type into.
    focus_float(app, *w);
    if (w->role == WindowRole::Document) {
      // Raise to top of the document stack; `w` is stale afterwards.
      const size_t idx = static_cast<size_t>(w - app.soft_wins.data());
      raise_soft(app, idx);
      w = &app.soft_wins.back();
      // Content clicks belong to the view (caret, links, selection, its
      // scrollbar): focus it and let the pane path handle the press.
      if (!bits && my >= w->y + float_title_h(*w) &&
          pane_at(app, static_cast<float>(mx), static_cast<float>(my)) ==
              w->view) {
        focus_view(app, w->view);
        return false;
      }
    }
    if (bits) {
      begin_resize(*w, bits, mx, my);
      return true;
    }
    if (my < w->y + float_title_h(*w)) {
      raise_float(app, *w);  // document windows were raised above already
      press_title(app, *w, mx, my);
    } else
      press_client(app, *w, mx, my);
    return true;
  }
  return false;
}

bool middle_click_float(App& app, int mx, int my) {
  FloatWindow* w = float_at(app, mx, my);
  if (!w) return false;
  // Title strip: send the window to the back of its layer.
  if (my < w->y + float_title_h(*w)) {
    lower_float(app, *w);
    return true;
  }
  switch (w->role) {
    case WindowRole::History:
    case WindowRole::Sitemap:
    case WindowRole::Page:
      break;
    case WindowRole::Context:
      if (!context_link_at(app, mx, my)) return false;
      break;
    default:
      return false;
  }
  app.click_new_tab = true;
  press_client(app, *w, mx, my);
  app.click_new_tab = false;
  return true;
}

bool float_motion(App& app, int mx, int my) {
  if (app.context_selecting) {
    float lx = 0.f, ly = 0.f;
    if (!context_local(app, mx, my, lx, ly, /*clamp=*/true)) {
      app.context_selecting = false;
      return false;
    }
    // Dragging past the top / bottom edge scrolls the panel along.
    auto [cx, cy, cw, ch] = float_client_rect(app, app.context_win);
    (void)cx; (void)cw;
    if (my < cy)
      app.context_win.scroll =
          std::max(0.f, app.context_win.scroll - 20.f);
    else if (my > cy + ch)
      app.context_win.scroll = std::min(float_max_scroll(app, app.context_win),
                                        app.context_win.scroll + 20.f);
    if (auto c = context_caret(app, lx, ly)) app.context_sel.focus = *c;
    return true;
  }
  for (FloatWindow* w : float_stack(app)) {
    if (w->dragging) {
      if (w->dock != Dock::None) {
        if (std::abs(mx - w->press_x) + std::abs(my - w->press_y) < kTearOffPx)
          return true;
        tear_off(app, *w, mx, my);
      }
      w->x = std::max(0, mx - w->drag_dx);
      w->y = std::max(static_cast<int>(app.ui_top), my - w->drag_dy);
      // Over the tab strip a drop turns a document window into a tab.
      app.tab_drop_preview =
          w->role == WindowRole::Document && over_tab_strip(app, mx, my);
      app.dock_preview =
          app.tab_drop_preview ? Dock::None : compute_dock_zone(app, mx, my);
      return true;
    }
    if (w->scroll_dragging) {
      w->scroll = scroll_for_thumb_top(
          app, *w, static_cast<float>(my) - w->scroll_grab);
      return true;
    }
    if (w->resize) {
      const int dx = mx - w->resize_start_x;
      const int dy = my - w->resize_start_y;
      if (float_docked(*w)) {
        // Column width / band height, shared by every window docked there.
        if (w->dock == Dock::Top || w->dock == Dock::Bottom) {
          int& bh = app.dock_w[w->dock == Dock::Bottom ? 3 : 2];
          const int nh = w->resize_start_h + (w->resize & 8 ? -dy : dy);
          bh = std::clamp(nh, 80, std::max(80, app.win_h * 2 / 5));
        } else {
          int& cw = app.dock_w[w->dock == Dock::Right ? 1 : 0];
          const int nw = w->resize_start_w + (w->resize & 4 ? -dx : dx);
          cw = std::clamp(nw, 160, std::max(160, app.win_w * 2 / 5));
        }
        app.needs_layout = true;
        place_windows(app);
        return true;
      }
      auto [min_w, min_h] = min_size(*w);
      if (w->resize & 1)
        w->w = std::clamp(w->resize_start_w + dx, min_w,
                          std::max(min_w, app.win_w - 20));
      if (w->resize & 2)
        w->h = std::clamp(w->resize_start_h + dy, min_h,
                          std::max(min_h, app.win_h - 40));
      // Left / top edges move the origin and keep the opposite edge put.
      if (w->resize & 4) {
        const int right = w->resize_start_wx + w->resize_start_w;
        w->w = std::clamp(w->resize_start_w - dx, min_w,
                          std::max(min_w, right));
        w->x = right - w->w;
      }
      if (w->resize & 8) {
        const int bottom = w->resize_start_wy + w->resize_start_h;
        const int top = static_cast<int>(app.ui_top);
        w->h = std::clamp(w->resize_start_h - dy, min_h,
                          std::max(min_h, bottom - top));
        w->y = bottom - w->h;
      }
      if (w->role == WindowRole::Help && (w->resize & (1 | 4)))
        app.help_needs_layout = true;
      return true;
    }
  }
  return false;
}

void float_release(App& app, int mx, int my) {
  app.context_selecting = false;
  for (FloatWindow* w : float_stack(app)) {
    // A press on a docked title that never moved keeps it docked.
    if (w->dragging && w->dock == Dock::None && app.tab_drop_preview &&
        w->role == WindowRole::Document && over_tab_strip(app, mx, my)) {
      app.tab_drop_preview = false;
      app.dock_preview = Dock::None;
      pop_in_soft(app, static_cast<size_t>(w - app.soft_wins.data()));
      return;  // the window is gone; the stack changed under the loop
    }
    if (w->dragging && w->dock == Dock::None) {
      const Dock zone = compute_dock_zone(app, mx, my);
      if (zone != Dock::None)
        dock_window(app, *w, zone, mx, my);
      else
        snap_soft_geom(app, w->x, w->y, w->w, w->h);
    } else if (w->resize && w->dock == Dock::None) {
      snap_soft_geom(app, w->x, w->y, w->w, w->h);
    }
    w->dragging = false;
    w->resize = 0;
    w->scroll_dragging = false;
  }
  app.tab_drop_preview = false;
  app.dock_preview = Dock::None;
}

bool float_wheel(App& app, int mx, int my, int wheel_y) {
  FloatWindow* w = float_at(app, mx, my);
  if (!w || w->role == WindowRole::Document) return false;  // pane scrolls
  if (imgui_scrolls(app, *w))
    return true;  // the embedded editor owns its own scroll position
  w->scroll -= static_cast<float>(wheel_y) * scroll_wheel_step(*w);
  clamp_scroll_min(*w);
  return true;
}

bool float_page_scroll(App& app, int mx, int my, bool up) {
  FloatWindow* w = float_at(app, mx, my);
  if (!w || w->role == WindowRole::Document) return false;
  auto [cx, cy, cw, ch] = float_client_rect(app, *w);
  const float view_h = static_cast<float>(std::max(1, ch));
  const float max_s = float_max_scroll(app, *w);
  const float step = std::max(1.f, view_h) * 0.85f;
  w->scroll =
      up ? std::max(0.f, w->scroll - step) : std::min(max_s, w->scroll + step);
  return true;
}

bool over_float_chrome(App& app, int x, int y) {
  FloatWindow* w = float_at(app, x, y);
  if (!w) return false;
  return w->role != WindowRole::Document ||
         pane_at(app, static_cast<float>(x), static_cast<float>(y)) != w->view;
}

bool close_focused_float(App& app) {
  auto st = float_stack(app);
  for (auto it = st.rbegin(); it != st.rend(); ++it)
    if (float_active(app, **it)) {
      close_window(app, **it);
      return true;
    }
  return false;
}

// Frame, then the view through the ordinary pane drawing (caret, overlays,
// raster, footnotes, scrollbar).
static void draw_soft_window(App& app, FloatWindow& sw) {
  if (sw.dock == Dock::None) {
    sw.w = std::max(sw.w, 280);
    sw.h = std::max(sw.h, 200);
  }
  if (!draw_floating_frame(app, sw, soft_title(app, sw), float_active(app, sw)))
    return;
  if (!view_live(app, sw.view)) return;
  const Pane& p = app.panes[sw.view];
  SDL_Rect clip{p.x, p.y, p.w, p.h};
  xanatoo::sdl::set_clip_rect(app.sdl, &clip);
  fill_doc_paper(app, clip);
  draw_pane(app, sw.view);
  xanatoo::sdl::set_clip_rect(app.sdl, nullptr);
}

void draw_floats(App& app) {
  place_windows(app);
  for (size_t i = app.soft_wins.size(); i-- > 0;)
    if (!find_buf(app, app.panes[app.soft_wins[i].view].buf))
      close_doc_window(app, i);
  // Windows opened since the last pass come to the front of their layer.
  ++app.float_pass;
  for (FloatWindow* w : float_stack(app)) {
    if (w->shown_pass + 1 != app.float_pass) raise_float(app, *w);
    w->shown_pass = app.float_pass;
  }
  for (FloatWindow* w : float_stack(app)) {
    switch (w->role) {
      case WindowRole::Outline:
        draw_outline_panel(app);
        break;
      case WindowRole::Backlinks:
        draw_backlinks_panel(app);
        break;
      case WindowRole::Sitemap:
        draw_sitemap_panel(app);
        break;
      case WindowRole::Page:
        draw_page_panel(app);
        break;
      case WindowRole::History:
        draw_history_panel(app);
        break;
      case WindowRole::Styles:
        if (draw_floating_frame(app, *w, "Style Sheets", float_active(app, *w)))
          app.ui.render_embedded_panel(app.sdl, xanatoo::UiPanelLayer::Styles);
        break;
      case WindowRole::StyleManager:
        if (draw_floating_frame(app, *w, "Manage Style Sheets",
                                float_active(app, *w)))
          app.ui.render_embedded_panel(app.sdl,
                                       xanatoo::UiPanelLayer::StyleManager);
        break;
      case WindowRole::Bookmarks:
        if (draw_floating_frame(app, *w, "Bookmarks", float_active(app, *w)))
          app.ui.render_embedded_panel(app.sdl, xanatoo::UiPanelLayer::Bookmarks);
        break;
      case WindowRole::Context:
        draw_context_window(app);
        break;
      case WindowRole::Subscriptions:
        if (draw_floating_frame(app, *w, "Subscriptions", float_active(app, *w)))
          app.ui.render_embedded_panel(app.sdl, xanatoo::UiPanelLayer::Subscriptions);
        break;
      case WindowRole::Downloads:
        draw_downloads_panel(app);
        break;
      case WindowRole::Media:
        draw_media_panel(app);
        break;
      case WindowRole::Reply:
        if (draw_floating_frame(app, *w, "Reply (mockup)", float_active(app, *w)))
          app.ui.render_embedded_panel(app.sdl, xanatoo::UiPanelLayer::Reply);
        break;
      case WindowRole::Annotations:
        if (draw_floating_frame(app, *w, "Annotations", float_active(app, *w)))
          app.ui.render_embedded_panel(app.sdl, xanatoo::UiPanelLayer::Annotations);
        break;
      case WindowRole::Revisions:
        draw_revision_panel(app);
        break;
      case WindowRole::NostrExplorer:
        if (draw_floating_frame(app, *w, "Nostr Explorer", float_active(app, *w)))
          app.ui.render_embedded_panel(app.sdl, xanatoo::UiPanelLayer::NostrExplorer);
        break;
      case WindowRole::LemmyExplorer:
        if (draw_floating_frame(app, *w, "Lemmy Explorer", float_active(app, *w)))
          app.ui.render_embedded_panel(app.sdl, xanatoo::UiPanelLayer::LemmyExplorer);
        break;
      case WindowRole::MatrixExplorer:
        if (draw_floating_frame(app, *w, "Matrix Explorer", float_active(app, *w)))
          app.ui.render_embedded_panel(app.sdl, xanatoo::UiPanelLayer::MatrixExplorer);
        break;
      case WindowRole::NostrAccounts:
        if (draw_floating_frame(app, *w, "Nostr Accounts", float_active(app, *w)))
          app.ui.render_embedded_panel(app.sdl, xanatoo::UiPanelLayer::NostrAccounts);
        break;
      case WindowRole::Certificates:
        if (draw_floating_frame(app, *w, "Certificates", float_active(app, *w)))
          app.ui.render_embedded_panel(app.sdl, xanatoo::UiPanelLayer::Certificates);
        break;
      case WindowRole::RelatedSites:
        if (draw_floating_frame(app, *w, "Related Sites", float_active(app, *w)))
          app.ui.render_embedded_panel(app.sdl, xanatoo::UiPanelLayer::RelatedSites);
        break;
      case WindowRole::Help:
        draw_help_window(app);
        break;
      case WindowRole::HoverHelp:
        draw_hover_help_window(app);
        break;
      case WindowRole::Document:
        draw_soft_window(app, *w);
        break;
    }
  }
}

}  // namespace xanatoo::app
