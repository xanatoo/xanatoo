// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// Touchscreen gestures, browser style: tap = left click (follow links,
// place caret), drag = scroll with momentum, long press = the touch right
// click: once the finger has rested long enough (not on the lift), the word
// under it is selected and the context menu opens; dragging on from there
// closes the menu and extends the selection, lifting opens it again.
// Two-finger pinch = zoom. Touches on ImGui chrome (URL bar, search bar,
// tabs, toolbar, panel forms) are left to ImGui, which sees SDL's
// touch-synthesised mouse events; held still there, the touch turns into a
// right click, which opens that chrome's context menu.
// Gutters (tile divider, dock splits, dock edges) are thin: besides a finger
// right on one, two fingers placed either side of a gutter grab it and drag
// it along with their midpoint. Pinches stay within one page, so a pair of
// fingers straddling a gutter is never a pinch.
// Small window frames are fiddly too: while one finger holds a free window's
// title, a second finger anywhere spans it, each window edge following the
// finger on its side (spread to grow, pinch to shrink, both to move).
// Lifting the second finger leaves the title finger dragging again.
// A finger has no hover: when the last one lifts, pointer_leave clears
// every hover effect, which a mouse would end by moving away.

#include <algorithm>
#include <cmath>

#include "xanatoo/app.hpp"
#include "xanatoo/editor.hpp"
#include "xanatoo/layout.hpp"
#include "xanatoo/theme.hpp"
#include "xanatoo/touch_gutter.hpp"

namespace xanatoo::app {

namespace {

constexpr float kSlopPx = 10.f;         // movement before a touch is a pan
constexpr Uint32 kLongPressMs = 500;    // hold this long → selection / menu
constexpr Uint32 kFlingMaxIdleMs = 80;  // finger rested → no fling
constexpr float kFlingMinSpeed = 60.f;  // px/s to start a fling
constexpr float kFlingStopSpeed = 15.f;
constexpr float kFlingTau = 0.4f;  // s; velocity decays as exp(-t/tau)

TouchPoint* find_finger(Touch& t, SDL_FingerID id) {
  for (int i = 0; i < t.count; ++i)
    if (t.fingers[i].id == id) return &t.fingers[i];
  return nullptr;
}

float finger_distance(const Touch& t) {
  return std::hypot(t.fingers[0].x - t.fingers[1].x,
                    t.fingers[0].y - t.fingers[1].y);
}

// Scroll the gesture target by dy; false when it is at an end (or gone).
bool scroll_target(App& app, float dy) {
  Touch& t = app.touch;
  if (t.on_float) {
    FloatWindow* w = float_at(app, static_cast<int>(t.start_x),
                              static_cast<int>(t.start_y));
    if (!w) return false;
    const float s =
        std::clamp(w->scroll + dy, 0.f, float_max_scroll(app, *w));
    const bool moved = s != w->scroll;
    w->scroll = s;
    return moved;
  }
  Pane& p = app.panes[t.pane];
  if (p.pages.on) return false;  // pages turn on release (touch_finger_up)
  const float s = std::clamp(p.scroll_y + dy, 0.f, pane_max_scroll(app, p));
  const bool moved = s != p.scroll_y;
  p.scroll_y = s;
  ensure_virtual_layout(app, p);
  return moved;
}

bool tile_gutter_at(const App& app, int x, int y) {
  const SDL_Rect rc = tile_divider_rect(app);
  return x >= rc.x && x < rc.x + rc.w && y >= rc.y && y < rc.y + rc.h;
}

enum class Gutter { None, DockSplit, DockEdge, Tile };

Gutter gutter_at(App& app, int x, int y) {
  if (dock_split_at(app, x, y) != DockSplitAxis::None)
    return Gutter::DockSplit;
  if (dock_edge_at(app, x, y)) return Gutter::DockEdge;
  if (tile_gutter_at(app, x, y) &&
      !float_at(app, x, y) && !float_resize_at(app, x, y))
    return Gutter::Tile;
  return Gutter::None;
}

// Drop what the first finger's press started (scrollbar, frame, split or
// selection drag) without the release side effects: no click, no docking
// or snapping of a dragged window.
void cancel_press(App& app) {
  app.dragging_sel = false;
  app.dragging_scroll = false;
  app.dragging_diff_overview = false;
  app.context_selecting = false;
  app.quote_click = {};
  app.dragging_image = false;
  app.resize_image = xanatoo::INVALID_NODE;
  app.dragging_block = false;
  app.drag_block_id = xanatoo::INVALID_NODE;
  app.drop_target_id = xanatoo::INVALID_NODE;
  app.drop_line_y = -1.f;
  app.dragging_col_gap = false;
  app.col_gap_left = app.col_gap_right = xanatoo::INVALID_NODE;
  page_pointer_up(app);
  gallery_pointer_up(app);
  app.dragging_split = false;
  dock_split_release(app);
  for (FloatWindow* w : float_stack(app)) {
    w->dragging = false;
    w->span = false;
    w->resize = 0;
    w->scroll_dragging = false;
  }
  app.tab_drop_preview = false;
  app.dock_preview = Dock::None;
}

// Dismiss a menu opened by the first finger when its gesture changes.
void close_touch_menu(App& app) {
  Touch& t = app.touch;
  if (!t.menu_open) return;
  app.ui.close_popups();
  t.menu_open = false;
}

// Two fingers either side of a gutter: grab it at the point between them.
// Whatever the first finger started gives way: it often lands on a pane
// scrollbar or a panel frame right next to the gutter.
bool begin_gutter_drag(App& app) {
  Touch& t = app.touch;
  const TouchPoint& a = t.fingers[0];
  const TouchPoint& b = t.fingers[1];
  Gutter gutter = Gutter::None;
  const auto g = segment_hit_near_middle(
      a.x, a.y, b.x, b.y, [&](int x, int y) {
        if (y < app.ui_top || y >= app.win_h - status_bar_h)
          return false;
        gutter = gutter_at(app, x, y);
        return gutter != Gutter::None &&
               !app.ui.captures_point(static_cast<float>(x),
                                      static_cast<float>(y),
                                      float_layer_at(app, x, y), t.menu_open);
      });
  if (!g) return false;
  close_touch_menu(app);
  app.ui.touch_long_press(false);
  if (t.mode == TouchMode::Direct || t.mode == TouchMode::Select)
    cancel_press(app);
  t.flinging = false;
  if (t.mode == TouchMode::Pan) t.velocity = 0.f;
  const float mx = (a.x + b.x) * 0.5f, my = (a.y + b.y) * 0.5f;
  t.gutter_dx = static_cast<float>(g->first) + 0.5f - mx;
  t.gutter_dy = static_cast<float>(g->second) + 0.5f - my;
  t.mode = TouchMode::GutterDrag;
  const auto [x, y] = *g;
  switch (gutter) {
    case Gutter::DockSplit: dock_split_press(app, x, y); break;
    case Gutter::DockEdge: float_press(app, x, y); break;
    case Gutter::Tile: tile_split_press(app, x, y); break;
    case Gutter::None: break;
  }
  return true;
}

// Where the grabbed gutter point is now (the fingers' midpoint + offset).
std::pair<int, int> gutter_point(const Touch& t) {
  const float mx = (t.fingers[0].x + t.fingers[1].x) * 0.5f;
  const float my = (t.fingers[0].y + t.fingers[1].y) * 0.5f;
  return {static_cast<int>(std::floor(mx + t.gutter_dx)),
          static_cast<int>(std::floor(my + t.gutter_dy))};
}

// Decide what a first finger at (x, y) is for.
void begin_gesture(App& app, float x, float y) {
  Touch& t = app.touch;
  t.flinging = false;
  t.velocity = 0.f;
  t.start_x = x;
  t.start_y = y;
  t.start_ticks = xanatoo::sdl::ticks();
  t.last_move_ticks = t.start_ticks;
  t.on_float = false;
  t.on_image = false;
  t.menu_open = false;
  t.ui_moved = false;
  const int xi = static_cast<int>(x), yi = static_cast<int>(y);
  if (app.ui.captures_point(x, y, float_layer_at(app, xi, yi)) ||
      y < app.ui_top) {
    t.mode = TouchMode::UiHold;
    return;
  }
  if (yi >= app.win_h - status_bar_h) {
    t.mode = TouchMode::Ignored;
    return;
  }
  if (FloatWindow* w = float_at(app, xi, yi);
      w && over_float_chrome(app, xi, yi)) {
    // Client area scrolls/taps; title, frame and scrollbar strip drag.
    auto [cx, cy, cw, ch] = float_client_rect(app, *w);
    const SDL_Rect sb = client_scrollbar_rect(app, *w);
    if (xi >= cx && xi < std::min(cx + cw, sb.x + 2) && yi >= cy &&
        yi < cy + ch) {
      t.on_float = true;
      t.mode = TouchMode::Pending;
    } else {
      t.mode = TouchMode::Direct;
      pointer_left_down(app, xi, yi, false);
    }
    return;
  }
  t.pane = pane_at(app, x, y);
  const Pane& p = app.panes[t.pane];
  if (pane_image_view(p)) {  // a single gallery image: pan, swipe, pinch
    focus_view(app, t.pane);
    t.on_image = true;
    t.mode = TouchMode::Image;
    return;
  }
  const bool on_scrollbar = xi >= sb_x(p) && xi < sb_x(p) + kSbW;
  const SDL_Rect divider = tile_divider_rect(app);
  const SDL_Rect badge = tile_mode_button_rect(app);
  const auto within = [&](const SDL_Rect& rc, int pad = 0) {
    return xi >= rc.x - pad && xi < rc.x + rc.w + pad &&
           yi >= rc.y - pad && yi < rc.y + rc.h + pad;
  };
  const bool on_badge = app.split && within(badge);
  const bool on_gutter = (app.split && within(divider, 3)) || on_badge ||
                         gutter_at(app, xi, yi) != Gutter::None;
  if (on_scrollbar || on_gutter) {
    t.mode = TouchMode::Direct;
    pointer_left_down(app, xi, yi, false);
    t.menu_open = on_badge && !float_resize_at(app, xi, yi);
    return;
  }
  t.mode = TouchMode::Pending;
}

// The context menu at the pressed spot, while the finger still rests.
void open_touch_menu(App& app) {
  Touch& t = app.touch;
  pointer_right_down(app, static_cast<int>(t.start_x),
                     static_cast<int>(t.start_y), false);
  // ImGui still holds the finger as a left press: the lift must not click
  // a menu item that opened under it.
  app.ui.touch_long_press(false);
  t.menu_open = true;
}

// Long press on pane text: select the word and open the context menu;
// dragging on extends the selection. Elsewhere (no text, a panel's client)
// only the menu opens.
void begin_select(App& app) {
  Touch& t = app.touch;
  if (t.on_float) {
    t.mode = TouchMode::Hold;
    open_touch_menu(app);
    return;
  }
  Pane& p = app.panes[t.pane];
  const float lx = local_x(app, t.start_x, t.pane);
  const float my = content_y(app, t.start_y, t.pane);
  auto hit = xanatoo::hit_test(p.layout, lx, my, p.scroll_y);
  if (!hit) {
    t.mode = TouchMode::Hold;  // nothing to select: just the menu
    open_touch_menu(app);
    return;
  }
  leave_prompt(app);
  focus_view(app, t.pane);
  p.sel.focus = *hit;
  p.sel.anchor = *hit;
  xanatoo::select_word_at(doc_of(app, p), p.sel);
  p.preferred_x = lx;
  app.dragging_sel = true;
  reset_blink(app);
  t.mode = TouchMode::Select;
  open_touch_menu(app);
}

void begin_pinch(App& app) {
  Touch& t = app.touch;
  t.flinging = false;
  t.pinch_dist = finger_distance(t);
  t.pinch_body_px = app.body_px;
  t.pinch_anchor.reset();
  if (t.on_image) {
    t.mode = TouchMode::ImagePinch;
    return;
  }
  if (!t.on_float) {
    const float mx = (t.fingers[0].x + t.fingers[1].x) * 0.5f;
    const float my = (t.fingers[0].y + t.fingers[1].y) * 0.5f;
    t.pinch_anchor = view_anchor_at(app, app.panes[t.pane],
                                    local_x(app, mx, t.pane),
                                    content_y(app, my, t.pane));
  }
  t.mode = TouchMode::Pinch;
}

}  // namespace

bool is_touch_mouse_event(const SDL_Event& ev) {
  using xanatoo::sdl::EventKind;
  switch (xanatoo::sdl::event_kind(ev)) {
    case EventKind::MouseMotion:
      return ev.motion.which == SDL_TOUCH_MOUSEID;
    case EventKind::MouseButtonDown:
    case EventKind::MouseButtonUp:
      return ev.button.which == SDL_TOUCH_MOUSEID;
    case EventKind::MouseWheel:
      return ev.wheel.which == SDL_TOUCH_MOUSEID;
    default:
      return false;
  }
}

// Only touchscreens: touchpad coordinates are not window positions, and
// mouse-synthesised touches would duplicate the mouse handlers.
static bool is_screen_touch(const SDL_TouchFingerEvent& tf) {
  return xanatoo::sdl::touch_id(tf) != SDL_MOUSE_TOUCHID &&
         SDL_GetTouchDeviceType(xanatoo::sdl::touch_id(tf)) ==
             SDL_TOUCH_DEVICE_DIRECT;
}

void touch_finger_down(App& app, const SDL_TouchFingerEvent& tf) {
  if (!is_screen_touch(tf)) return;
  Touch& t = app.touch;
  if (t.count >= 2 || find_finger(t, xanatoo::sdl::finger_id(tf))) return;
  const float x = tf.x * static_cast<float>(app.win_w);
  const float y = tf.y * static_cast<float>(app.win_h);
  t.fingers[t.count++] = TouchPoint{xanatoo::sdl::finger_id(tf), x, y};
  if (t.count == 1) {
    app.pointer_away = false;  // ImGui follows this finger again
    begin_gesture(app, x, y);
    return;
  }
  if (t.mode == TouchMode::Ignored) return;
  if (begin_gutter_drag(app)) return;
  if (t.mode == TouchMode::UiHold) {
    t.mode = TouchMode::Ignored;  // two fingers on chrome: no right click
  } else if (t.mode == TouchMode::Direct && float_span_begin(app)) {
    t.mode = TouchMode::FloatSpan;
  } else if (t.mode != TouchMode::Select && t.mode != TouchMode::Direct) {
    close_touch_menu(app);
    begin_pinch(app);
  }
}

void touch_finger_motion(App& app, const SDL_TouchFingerEvent& tf) {
  if (!is_screen_touch(tf)) return;
  Touch& t = app.touch;
  TouchPoint* f = find_finger(t, xanatoo::sdl::finger_id(tf));
  if (!f) return;
  const float x = tf.x * static_cast<float>(app.win_w);
  const float y = tf.y * static_cast<float>(app.win_h);
  const float dx = x - f->x;
  const float dy = y - f->y;
  f->x = x;
  f->y = y;
  const bool primary = f == &t.fingers[0];
  switch (t.mode) {
    case TouchMode::UiHold:  // ImGui drags it (a slider, a tab, a scroll)
      if (std::hypot(x - t.start_x, y - t.start_y) >= kSlopPx)
        t.ui_moved = true;
      break;
    case TouchMode::Hold:  // the menu is open: wait for the lift
      break;
    case TouchMode::Pending:
      if (std::hypot(x - t.start_x, y - t.start_y) < kSlopPx) break;
      t.mode = TouchMode::Pan;
      // Start from the origin so the content stays under the finger.
      scroll_target(app, t.start_y - y);
      t.last_move_ticks = xanatoo::sdl::ticks();
      break;
    case TouchMode::Pan: {
      if (!primary) break;
      scroll_target(app, -dy);
      const Uint32 now = xanatoo::sdl::ticks();
      const float dt =
          static_cast<float>(std::max<Uint32>(1, now - t.last_move_ticks)) /
          1000.f;
      // Bias toward the latest movement; a fling follows the final swipe.
      t.velocity = 0.8f * (-dy / dt) + 0.2f * t.velocity;
      t.last_move_ticks = now;
      break;
    }
    case TouchMode::Select:
      if (!primary) break;
      if (t.menu_open) {  // the finger rests on the word, under the menu
        if (std::hypot(x - t.start_x, y - t.start_y) < kSlopPx) break;
        close_touch_menu(app);
      }
      pointer_motion(app, static_cast<int>(x), static_cast<int>(y), false);
      break;
    case TouchMode::Direct:
      if (primary)
        pointer_motion(app, static_cast<int>(x), static_cast<int>(y), false);
      break;
    case TouchMode::Pinch:
      if (t.pinch_dist > 0.f) {
        const float scale = finger_distance(t) / t.pinch_dist;
        const int target = static_cast<int>(
            std::lround(static_cast<float>(t.pinch_body_px) * scale));
        if (target != app.body_px && t.pinch_anchor)
          app.panes[t.pane].view_anchor = t.pinch_anchor;
        zoom_by(app, target - app.body_px);
      }
      break;
    case TouchMode::ImagePinch:
      if (t.pinch_dist > 0.f) {
        const float distance = finger_distance(t);
        const float factor = distance / t.pinch_dist;
        t.pinch_dist = distance;
        const float mx = (t.fingers[0].x + t.fingers[1].x) * 0.5f;
        const float my = (t.fingers[0].y + t.fingers[1].y) * 0.5f;
        // Both fingers moving together drag the picture along.
        gallery_pan(app, t.pane, dx * 0.5f, dy * 0.5f);
        gallery_zoom(app, t.pane, factor, mx, my);
      }
      break;
    case TouchMode::Image:
      if (primary) gallery_pan(app, t.pane, dx, dy);
      break;
    case TouchMode::GutterDrag: {
      const auto [gx, gy] = gutter_point(t);
      pointer_motion(app, gx, gy, false);
      break;
    }
    case TouchMode::FloatSpan:
      float_span_motion(app);
      break;
    case TouchMode::Idle:
    case TouchMode::Ignored:
      break;
  }
}

void touch_finger_up(App& app, const SDL_TouchFingerEvent& tf) {
  if (!is_screen_touch(tf)) return;
  Touch& t = app.touch;
  TouchPoint* f = find_finger(t, xanatoo::sdl::finger_id(tf));
  if (!f) return;
  const bool primary = f == &t.fingers[0];
  const int xi = static_cast<int>(f->x), yi = static_cast<int>(f->y);
  switch (t.mode) {
    case TouchMode::Pending:
      if (primary) {  // tap
        pointer_left_down(app, static_cast<int>(t.start_x),
                          static_cast<int>(t.start_y), false);
        pointer_left_up(app, static_cast<int>(t.start_x),
                        static_cast<int>(t.start_y));
      }
      break;
    case TouchMode::Pan:
      if (primary && !t.on_float && app.panes[t.pane].pages.on) {
        // A swipe turns the page: left / up → next, right / down → back.
        const float dx = f->x - t.start_x, dy = f->y - t.start_y;
        const float need = 48.f * app.ui_scale;
        const bool horizontal = std::abs(dx) >= std::abs(dy);
        const float d = horizontal ? dx : dy;
        if (std::abs(d) >= need) page_turn(app, app.panes[t.pane], d < 0 ? 1 : -1);
      } else if (primary) {
        const Uint32 now = xanatoo::sdl::ticks();
        t.flinging = now - t.last_move_ticks <= kFlingMaxIdleMs &&
                     std::abs(t.velocity) >= kFlingMinSpeed;
        t.fling_ticks = now;
      }
      break;
    case TouchMode::Select:
      // After a drag the menu opens again. Target the pressed spot: it lies
      // inside the selected word, so the menu keeps the extended selection
      // for Copy & co.
      if (primary) {
        pointer_left_up(app, xi, yi);
        if (!t.menu_open)
          pointer_right_down(app, static_cast<int>(t.start_x),
                             static_cast<int>(t.start_y), false);
      }
      break;
    case TouchMode::Hold:    // the menu opened when the timer fired
    case TouchMode::UiHold:  // a tap: ImGui clicked it
      break;
    case TouchMode::Direct:
      if (primary) pointer_left_up(app, xi, yi);
      break;
    case TouchMode::GutterDrag: {  // either finger lets go of the gutter
      const auto [gx, gy] = gutter_point(t);
      pointer_left_up(app, gx, gy);
      t.mode = TouchMode::Ignored;
      break;
    }
    case TouchMode::FloatSpan:
      if (primary) {  // the title finger let go: the window settles
        float_span_end(app, false);
        pointer_left_up(app, xi, yi);
      } else {  // the title finger alone drags the window on
        float_span_end(app, true);
        t.mode = TouchMode::Direct;
      }
      break;
    case TouchMode::Pinch:  // the remaining finger does nothing
    case TouchMode::ImagePinch:
      t.mode = TouchMode::Ignored;
      break;
    case TouchMode::Image:
      if (primary && pane_image_view(app.panes[t.pane])) {
        const float dx = f->x - t.start_x, dy = f->y - t.start_y;
        const bool zoomed = app.panes[t.pane].gallery.scale > 0.f;
        if (std::hypot(dx, dy) < kSlopPx) {
          // Tap: the edge zones (previous / next / up to the grid).
          const int sx = static_cast<int>(t.start_x);
          const int sy = static_cast<int>(t.start_y);
          gallery_pointer_down(app, t.pane, sx, sy, SDL_BUTTON_LEFT);
          gallery_pointer_up(app);
        } else if (!zoomed && std::abs(dx) >= 48.f * app.ui_scale &&
                   std::abs(dx) > std::abs(dy)) {
          gallery_step(app, t.pane, dx < 0 ? 1 : -1, true);  // swipe
        }
      }
      break;
    case TouchMode::Idle:
    case TouchMode::Ignored:
      break;
  }
  // A lifted primary ends single-finger gestures; ignore the other finger.
  if (primary) t.mode = TouchMode::Ignored;
  *f = t.fingers[t.count - 1];
  --t.count;
  if (t.count == 0) {
    t.mode = TouchMode::Idle;
    // A finger has no hover: once it lifts, nothing stays highlighted
    // under where it was (links, tooltips, title buttons, ImGui items).
    pointer_leave(app);
  }
}

void draw_touch_hold(const App& app) {
  const Touch& t = app.touch;
  if (t.mode != TouchMode::Pending &&
      (t.mode != TouchMode::UiHold || t.ui_moved))
    return;

  namespace th = xanatoo::theme;
  const auto& p = th::palette();
  const auto px = [&](int size) {
    return static_cast<int>(std::lround(size * app.ui_scale));
  };
  const int width = px(48);
  const int height = px(8);
  if (width < 3 || height < 3 || app.win_w < width || app.win_h < height)
    return;
  // Keep the indicator clear of the finger, even near the top edge.
  const int x = static_cast<int>(t.start_x);
  const int y = static_cast<int>(t.start_y);
  const int above = y - px(28) - height;
  const SDL_Rect track{
      std::clamp(x - width / 2, 0, app.win_w - width),
      std::clamp(above >= 0 ? above : y + px(28), 0, app.win_h - height),
      width, height};
  const auto elapsed = std::min<Uint32>(
      xanatoo::sdl::ticks() - t.start_ticks, kLongPressMs);
  const int fill_width = 1 + (width - 3) * static_cast<int>(elapsed) /
                                 static_cast<int>(kLongPressMs);
  th::fill(app.sdl, track, p.dark);
  th::fill(app.sdl, {track.x + 1, track.y + 1, width - 2, height - 2},
           p.face_light);
  th::fill(app.sdl, {track.x + 1, track.y + 1, fill_width, height - 2},
           p.accent_dark);
}

void update_touch(App& app) {
  Touch& t = app.touch;
  const Uint32 now = xanatoo::sdl::ticks();
  if (t.mode == TouchMode::Pending && now - t.start_ticks >= kLongPressMs)
    begin_select(app);
  if (t.mode == TouchMode::UiHold && !t.ui_moved &&
      now - t.start_ticks >= kLongPressMs) {
    app.ui.touch_long_press(true);
    t.mode = TouchMode::Ignored;
  }
  if (!t.flinging) return;
  const float dt = static_cast<float>(now - t.fling_ticks) / 1000.f;
  if (dt <= 0.f) return;
  t.fling_ticks = now;
  if (!scroll_target(app, t.velocity * dt)) {
    t.flinging = false;
    return;
  }
  t.velocity *= std::exp(-dt / kFlingTau);
  if (std::abs(t.velocity) < kFlingStopSpeed) t.flinging = false;
}

}  // namespace xanatoo::app
