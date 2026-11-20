// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/theme.hpp"

#include <algorithm>
#include <cmath>

namespace xanatoo::theme {

const Palette& palette() {
  static const Palette p;
  return p;
}

namespace {

void set(SDL_Renderer* r, Rgb c, uint8_t a = 255) {
  SDL_SetRenderDrawColor(r, c.r, c.g, c.b, a);
}

Rgb lerp(Rgb a, Rgb b, float t) {
  auto m = [t](uint8_t x, uint8_t y) {
    return static_cast<uint8_t>(
        static_cast<float>(x) +
        (static_cast<float>(y) - static_cast<float>(x)) * t + 0.5f);
  };
  return Rgb{m(a.r, b.r), m(a.g, b.g), m(a.b, b.b)};
}

}  // namespace

void fill(SDL_Renderer* r, const SDL_Rect& rc, Rgb c, uint8_t a) {
  SDL_BlendMode old;
  SDL_GetRenderDrawBlendMode(r, &old);
  SDL_SetRenderDrawBlendMode(
      r, a == 255 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
  set(r, c, a);
  xanatoo::sdl::fill_rect(r, &rc);
  SDL_SetRenderDrawBlendMode(r, old);
}

void hline(SDL_Renderer* r, int x0, int x1, int y, Rgb c) {
  set(r, c);
  xanatoo::sdl::draw_line(r, x0, y, x1, y);
}

void vline(SDL_Renderer* r, int x, int y0, int y1, Rgb c) {
  set(r, c);
  xanatoo::sdl::draw_line(r, x, y0, x, y1);
}

void line(SDL_Renderer* r, int x0, int y0, int x1, int y1, Rgb c) {
  set(r, c);
  xanatoo::sdl::draw_line(r, x0, y0, x1, y1);
}

Rgb lane_color(int i) {
  static constexpr Rgb kLanes[] = {
      rgb(0x34, 0x65, 0xA4),  // blue (accent_dark)
      rgb(0x4E, 0x9A, 0x06),  // green
      rgb(0xC4, 0x6A, 0x00),  // orange
      rgb(0x75, 0x50, 0x7B),  // plum
      rgb(0x06, 0x98, 0x9A),  // teal
      rgb(0xA4, 0x00, 0x00),  // red
  };
  constexpr int n = static_cast<int>(sizeof kLanes / sizeof kLanes[0]);
  return kLanes[((i % n) + n) % n];
}

void vgradient(SDL_Renderer* r, const SDL_Rect& rc, Rgb top, Rgb bottom) {
  if (rc.h <= 0 || rc.w <= 0) return;
  for (int i = 0; i < rc.h; ++i) {
    const float t = rc.h > 1 ? static_cast<float>(i) / (rc.h - 1) : 0.f;
    hline(r, rc.x, rc.x + rc.w - 1, rc.y + i, lerp(top, bottom, t));
  }
}

void bevel(SDL_Renderer* r, const SDL_Rect& rc, Bevel style) {
  const Palette& p = palette();
  const int x0 = rc.x, y0 = rc.y, x1 = rc.x + rc.w - 1, y1 = rc.y + rc.h - 1;
  Rgb tl = p.highlight, br = p.shadow;
  if (style == Bevel::Sunken) std::swap(tl, br);
  if (style == Bevel::Etched) {
    // Two-tone groove: dark line with a light line right below/after it.
    hline(r, x0, x1, y0, p.shadow);
    vline(r, x0, y0, y1, p.shadow);
    hline(r, x0 + 1, x1, y1, p.highlight);
    vline(r, x1, y0 + 1, y1, p.highlight);
    return;
  }
  hline(r, x0, x1 - 1, y0, tl);
  vline(r, x0, y0, y1 - 1, tl);
  hline(r, x0, x1, y1, br);
  vline(r, x1, y0, y1, br);
}

void drop_shadow(SDL_Renderer* r, const SDL_Rect& rc, int offset) {
  SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
  for (int i = 0; i < offset; ++i) {
    SDL_SetRenderDrawColor(r, 0, 0, 0, static_cast<Uint8>(46 - i * 12));
    SDL_Rect s{rc.x + 2 + i, rc.y + 2 + i, rc.w, rc.h};
    xanatoo::sdl::fill_rect(r, &s);
  }
  SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

void draw_grip_dots(SDL_Renderer* r, const SDL_Rect& rc, float scale,
                    int count) {
  const Palette& p = palette();
  const int d = std::max(2, static_cast<int>(std::lround(2.f * scale)));
  const int step = d * 3;
  const bool vertical = rc.h > rc.w;
  const int across = vertical ? rc.w : rc.h;
  const int along = vertical ? rc.h : rc.w;
  if (across < d + 1) return;
  count = std::min(count, std::max(0, (along - d) / step + 1));
  if (count <= 0) return;
  const int span = (count - 1) * step + d;
  const int a0 = (along - span) / 2;
  const int c0 = (across - d - 1) / 2;
  for (int i = 0; i < count; ++i) {
    const int x = rc.x + (vertical ? c0 : a0 + i * step);
    const int y = rc.y + (vertical ? a0 + i * step : c0);
    fill(r, SDL_Rect{x + 1, y + 1, d, d}, p.shadow);
    fill(r, SDL_Rect{x, y, d, d}, p.highlight);
  }
}

void draw_button_face(SDL_Renderer* r, const SDL_Rect& rc, bool pressed,
                      bool hot) {
  const Palette& p = palette();
  if (rc.w <= 0 || rc.h <= 0) return;
  Rgb top = hot ? p.highlight : p.face_light;
  Rgb bot = hot ? lerp(p.highlight, p.face_dark, 0.55f) : p.face_dark;
  if (pressed) std::swap(top, bot);
  if (rc.w < 7 || rc.h < 7) {
    vgradient(r, rc, top, bot);
    bevel(r, rc, pressed ? Bevel::Sunken : Bevel::Raised);
    return;
  }
  // GTK2 Clearlooks-inspired raised button: a restrained gradient, rounded
  // 2px outline, and an inner light edge. Keep the four outer corner pixels
  // clear so the title-bar yellow or grey parent surface shows through.
  const int x0 = rc.x, y0 = rc.y;
  const int x1 = rc.x + rc.w - 1, y1 = rc.y + rc.h - 1;
  vgradient(r, SDL_Rect{x0 + 1, y0 + 1, rc.w - 2, rc.h - 2}, top, bot);
  const Rgb border = pressed ? p.dark : p.shadow;
  hline(r, x0 + 2, x1 - 2, y0, border);
  hline(r, x0 + 2, x1 - 2, y1, border);
  vline(r, x0, y0 + 2, y1 - 2, border);
  vline(r, x1, y0 + 2, y1 - 2, border);
  set(r, border);
  xanatoo::sdl::draw_point(r, x0 + 1, y0 + 1);
  xanatoo::sdl::draw_point(r, x1 - 1, y0 + 1);
  xanatoo::sdl::draw_point(r, x0 + 1, y1 - 1);
  xanatoo::sdl::draw_point(r, x1 - 1, y1 - 1);
  const Rgb inner = pressed ? p.shadow : p.highlight;
  hline(r, x0 + 2, x1 - 2, y0 + 1, inner);
  vline(r, x0 + 1, y0 + 2, y1 - 2, inner);
}

void draw_scroll_thumb(SDL_Renderer* r, const SDL_Rect& rc, bool pressed,
                       bool hover) {
  if (rc.w <= 0 || rc.h <= 0) return;
  const Palette& p = palette();
  const Rgb edge = pressed ? p.accent_dark : p.scroll_thumb_edge;
  if (rc.w < 4 || rc.h < 4) {
    fill(r, rc, edge);
    return;
  }
  // Separate blue-grey thumb from the almost-white trough. The dark outline
  // stays visible even over section/search markers behind the movable thumb.
  // Hovered it lightens; held it turns accent blue (visible under a finger).
  Rgb top = hover ? p.scroll_thumb_hot_top : p.scroll_thumb_top;
  Rgb bottom = hover ? p.scroll_thumb_hot_bottom : p.scroll_thumb_bottom;
  if (pressed) {
    top = p.accent_dark;
    bottom = p.accent;
  }
  vgradient(r, SDL_Rect{rc.x + 1, rc.y + 1, rc.w - 2, rc.h - 2}, top, bottom);
  const int x1 = rc.x + rc.w - 1, y1 = rc.y + rc.h - 1;
  hline(r, rc.x, x1, rc.y, edge);
  hline(r, rc.x, x1, y1, edge);
  vline(r, rc.x, rc.y + 1, y1 - 1, edge);
  vline(r, x1, rc.y + 1, y1 - 1, edge);
  if (rc.w > 6 && rc.h > 6) {
    hline(r, rc.x + 2, x1 - 2, rc.y + 1,
          pressed ? p.shadow : p.highlight);
    vline(r, rc.x + 1, rc.y + 2, y1 - 2,
          pressed ? p.shadow : p.highlight);
  }
}

void draw_window_chrome(SDL_Renderer* r, const WindowChrome& w) {
  const Palette& p = palette();
  const SDL_Rect& o = w.outer;
  if (w.shadow) drop_shadow(r, o);
  // Frame + client face
  fill(r, o, p.face);
  // Title strip
  SDL_Rect title{o.x + 1, o.y + 1, o.w - 2, w.title_h - 1};
  if (w.active)
    vgradient(r, title, p.tab_top, p.tab_bottom);
  else
    vgradient(r, title, p.tab_idle_top, p.tab_idle_bottom);
  // Light edge under the top line, dark line below the strip
  hline(r, title.x, title.x + title.w - 1, title.y, p.highlight);
  hline(r, o.x + 1, o.x + o.w - 2, o.y + w.title_h,
        w.active ? Rgb{0x9A, 0x74, 0x08} : p.shadow);
  // Outer frame
  SDL_SetRenderDrawColor(r, p.frame.r, p.frame.g, p.frame.b, 255);
  xanatoo::sdl::draw_rect(r, &o);
  const auto px = [&](int v) { return static_cast<int>(v * w.scale + 0.5f); };
  const int slot = w.slot > 0 ? w.slot : px(28);
  const int side = std::min(px(20), std::max(px(12), w.title_h - px(6)));
  // Close at the title's left edge; maximize / restore at its right edge,
  // shade just left of it. Flush controls cover their whole zone, sharing
  // the frame line and resting on the line under the strip.
  auto control_rect = [&](int from_right) {
    const int zone_x = from_right < 0 ? o.x : o.x + o.w - (from_right + 1) * slot;
    if (w.flush) return SDL_Rect{zone_x, o.y, slot, w.title_h};
    return SDL_Rect{zone_x + (slot - side) / 2,
                    o.y + (w.title_h - side) / 2, side, side};
  };
  // Matching, centred 20px controls inside the existing 28px hit zones.
  if (w.close_box) {
    const SDL_Rect cb = control_rect(-1);
    draw_button_face(r, cb, false, w.close_hot);
    set(r, p.text);
    const int cx = cb.x + cb.w / 2, cy = cb.y + cb.h / 2;
    const int arm = std::max(2, px(4));
    xanatoo::sdl::draw_line(r, cx - arm, cy - arm, cx + arm, cy + arm);
    xanatoo::sdl::draw_line(r, cx - arm, cy + arm, cx + arm, cy - arm);
  }
  // Maximize / restore box in the rightmost hit zone (28px)
  if (w.max_box) {
    const SDL_Rect mb = control_rect(0);
    draw_button_face(r, mb, false, w.max_hot);
    set(r, p.text);
    const int a = std::max(2, px(4));
    const int cx = mb.x + mb.w / 2, cy = mb.y + mb.h / 2;
    if (w.maximized) {  // two overlapping squares
      SDL_Rect s1{cx - a, cy - a + 2, a * 2 - 1, a * 2 - 1};
      SDL_Rect s2{cx - a + 2, cy - a, a * 2 - 1, a * 2 - 1};
      xanatoo::sdl::draw_rect(r, &s1);
      xanatoo::sdl::draw_rect(r, &s2);
    } else {
      SDL_Rect sq{cx - a, cy - a, a * 2 + 1, a * 2 + 1};
      xanatoo::sdl::draw_rect(r, &sq);
      xanatoo::sdl::draw_line(r, sq.x, sq.y + 1, sq.x + sq.w - 1, sq.y + 1);
    }
  }
  // Shade box, direction-free (a docked panel may roll towards any side):
  // a lone bold bar rolls the window up to its title; the same bar with
  // two thin text lines under it rolls it out again. No side edges, so it
  // never reads as the maximize square. Greyed out where it does nothing.
  if (w.shade_box) {
    const SDL_Rect sb = control_rect(1);
    draw_button_face(r, sb, false, w.shade_enabled && w.shade_hot);
    set(r, w.shade_enabled ? p.text : p.face_dark);
    const int a = std::max(2, px(4));
    const int t = std::max(2, px(2));  // bar thickness
    const int cx = sb.x + sb.w / 2, cy = sb.y + sb.h / 2;
    const auto bar = [&](int y, int h) {
      const SDL_Rect b{cx - a, y, a * 2 + 1, h};
      xanatoo::sdl::fill_rect(r, &b);
    };
    if (w.shaded) {
      bar(cy - a, t);
      bar(cy, 1);
      bar(cy + a, 1);
    } else {
      bar(cy - t / 2, t);
    }
  }
  // Resize grip (SE): diagonal ridges in light/dark pairs
  if (w.resizable) {
    const int gx = o.x + o.w - 3, gy = o.y + o.h - 3;
    for (int i = 0; i < 3; ++i) {
      set(r, p.shadow);
      xanatoo::sdl::draw_line(r, gx - i * 4 - 1, gy, gx, gy - i * 4 - 1);
      set(r, p.highlight);
      xanatoo::sdl::draw_line(r, gx - i * 4, gy, gx, gy - i * 4);
    }
  }
}

SDL_Rect vscroll_thumb(const SDL_Rect& trough, float scroll, float max_scroll,
                       float view_h, float* track_out) {
  const float th = static_cast<float>(trough.h - 2);
  const float thumb_h = std::min(
      th, std::max(18.f, th * (view_h / std::max(1.f, view_h + max_scroll))));
  const float track = std::max(0.f, th - thumb_h);
  const float ratio =
      max_scroll > 0.f ? std::clamp(scroll / max_scroll, 0.f, 1.f) : 0.f;
  if (track_out) *track_out = track;
  return SDL_Rect{trough.x + 1, trough.y + 1 + static_cast<int>(ratio * track),
                  trough.w - 2, static_cast<int>(thumb_h)};
}

void draw_vscrollbar(SDL_Renderer* r, const SDL_Rect& trough, float scroll,
                     float max_scroll, float view_h, SDL_Rect* thumb_out,
                     bool hover, bool pressed) {
  const Palette& p = palette();
  fill(r, trough, hover || pressed ? p.scroll_trough_hot : p.scroll_trough);
  bevel(r, trough, Bevel::Sunken);
  if (max_scroll <= 1.f) return;
  const SDL_Rect thumb = vscroll_thumb(trough, scroll, max_scroll, view_h);
  draw_scroll_thumb(r, thumb, pressed, hover);
  if (thumb_out) *thumb_out = thumb;
}

void draw_gutter(SDL_Renderer* r, const SDL_Rect& rc, Rgb base, Hot hot) {
  if (rc.w <= 0 || rc.h <= 0) return;
  const Palette& p = palette();
  fill(r, rc, hot == Hot::Pressed ? p.gutter_pressed
              : hot == Hot::Hover ? p.gutter_hot : base);
  bevel(r, rc, hot == Hot::Pressed ? Bevel::Sunken : Bevel::Raised);
}

}  // namespace xanatoo::theme
