// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
//
// Classic desktop look (BeOS R5 / GNOME 2 Clearlooks): one palette (plain
// data) and stateless drawing primitives used by every window / bar that is
// drawn on the SDL canvas. The ImGui chrome (menus, toolbar) reads the same
// palette so the two never drift apart.
#pragma once

#include "xanatoo/sdl.hpp"

#include <cstdint>
#include <string>

namespace xanatoo::theme {

struct Rgb {
  uint8_t r = 0, g = 0, b = 0;
};

constexpr Rgb rgb(int r, int g, int b) {
  return Rgb{static_cast<uint8_t>(r), static_cast<uint8_t>(g),
             static_cast<uint8_t>(b)};
}

struct Palette {
  // Surfaces
  Rgb face = rgb(0xEA, 0xEC, 0xEB);        // Clearlooks grey window face
  Rgb face_light = rgb(0xFA, 0xFB, 0xFA);  // button gradient top, menus
  Rgb face_dark = rgb(0xD5, 0xD9, 0xD7);   // button gradient bottom
  Rgb field = rgb(0xFF, 0xFF, 0xFF);       // text entry / sunken well
  Rgb scroll_trough = rgb(0xE7, 0xEA, 0xEC);  // light recessed scrollbar track
  Rgb scroll_thumb_top = rgb(0xC1, 0xD0, 0xDA);  // cool raised thumb
  Rgb scroll_thumb_bottom = rgb(0x8D, 0xA6, 0xB7);
  Rgb scroll_thumb_edge = rgb(0x5B, 0x73, 0x85);
  // Pointer feedback: a hovered trough / thumb, and a held thumb.
  Rgb scroll_trough_hot = rgb(0xD6, 0xE3, 0xEE);
  Rgb scroll_thumb_hot_top = rgb(0xD6, 0xE5, 0xF0);
  Rgb scroll_thumb_hot_bottom = rgb(0x9C, 0xBA, 0xD0);
  // Gutters (tile divider, dock gutters) under the pointer / being dragged.
  Rgb gutter_hot = rgb(0xBC, 0xD7, 0xF0);
  Rgb gutter_pressed = rgb(0x6F, 0xA3, 0xD4);
  Rgb paper = rgb(0xF8, 0xF6, 0xF0);       // document canvas
  // Bevel edges
  Rgb highlight = rgb(0xFF, 0xFF, 0xFF);
  Rgb shadow = rgb(0x9D, 0xA5, 0xA5);
  Rgb dark = rgb(0x65, 0x6F, 0x73);
  Rgb frame = rgb(0x88, 0x93, 0x96);
  // Ink
  Rgb text = rgb(0x2E, 0x34, 0x36);
  Rgb text_dim = rgb(0x65, 0x6D, 0x70);
  // Window title tab (BeOS yellow when active)
  Rgb tab_top = rgb(0xFF, 0xE0, 0x5A);
  Rgb tab_bottom = rgb(0xF2, 0xBE, 0x1A);
  Rgb tab_idle_top = rgb(0xEE, 0xF0, 0xEF);
  Rgb tab_idle_bottom = rgb(0xC9, 0xCD, 0xCC);
  Rgb tab_text = rgb(0x2A, 0x24, 0x08);
  // Selection / accent (GNOME 2 blue)
  Rgb accent = rgb(0x4E, 0x88, 0xB8);
  Rgb accent_dark = rgb(0x34, 0x65, 0xA4);
  Rgb accent_soft = rgb(0xD2, 0xE6, 0xF8);
  // Tooltip / notice (BeOS-style pale yellow)
  Rgb notice = rgb(0xFF, 0xFA, 0xD2);
  Rgb notice_text = rgb(0x3A, 0x30, 0x10);
};

const Palette& palette();

// Title strip hue of a window. Each panel family gets its own colour: a
// saturated gradient while the window is active, a faint hint of it on the
// grey idle strip. Yellow (the BeOS tab) stays with documents.
enum class Tint : uint8_t {
  Yellow, Blue, Teal, Green, Lime, Orange, Amber, Red, Rose, Plum, Violet,
  Mint, Steel
};

struct TitleTint {
  Rgb top, bottom;            // active gradient
  Rgb line;                   // rule under the active strip
  Rgb text;                   // active title ink
  Rgb idle_top, idle_bottom;  // muted gradient out of focus
};

const TitleTint& title_tint(Tint t);

enum class Bevel { Raised, Sunken, Etched };

void fill(SDL_Renderer* r, const SDL_Rect& rc, Rgb c, uint8_t a = 255);
void hline(SDL_Renderer* r, int x0, int x1, int y, Rgb c);
void vline(SDL_Renderer* r, int x, int y0, int y1, Rgb c);
void line(SDL_Renderer* r, int x0, int y0, int x1, int y1, Rgb c);
// Colour of lane `i` in trail graphs (History panel), repeating.
Rgb lane_color(int i);
void vgradient(SDL_Renderer* r, const SDL_Rect& rc, Rgb top, Rgb bottom);
// 1px frame (inside `rc`); Raised = light top-left / dark bottom-right.
void bevel(SDL_Renderer* r, const SDL_Rect& rc, Bevel style);
// Soft drop shadow for floating windows.
void drop_shadow(SDL_Renderer* r, const SDL_Rect& rc, int offset = 3);
// A row of small raised dots centred in `rc` along its long side: the grip
// mark of a draggable gutter / divider.
void draw_grip_dots(SDL_Renderer* r, const SDL_Rect& rc, float scale = 1.f,
                    int count = 5);

struct WindowChrome {
  SDL_Rect outer{};       // whole window incl. frame
  int title_h = 28;       // height of the title strip
  int slot = 0;           // control hit-zone width (0: 28px · scale)
  bool flush = false;     // controls fill their zone + the strip (docked)
  bool active = true;     // tinted tab vs. muted
  Tint tint = Tint::Yellow;  // panel family colour (`title_tint`)
  bool close_box = true;  // left-aligned close widget (hit zone: first 28px)
  bool resizable = true;  // SE grip
  bool max_box = true;    // right-aligned maximize / restore (last 28px)
  bool maximized = false;  // shows the restore glyph
  bool shade_box = true;   // roll-up widget left of the maximize box
  bool shade_enabled = true;  // false: greyed out (window not docked)
  bool shaded = false;     // shows the roll-down glyph
  bool close_hot = false;
  bool max_hot = false;
  bool shade_hot = false;
  float scale = 1.f;      // ui scale: close box size
  bool shadow = true;     // drop shadow (free windows only; docked sit flat)
};

// Frame, title strip (gradient), close box and resize grip. The window
// client area is `outer` minus title strip minus 1px frame; the title text
// is drawn by the caller (needs the font) at title_text_x/baseline.
void draw_window_chrome(SDL_Renderer* r, const WindowChrome& w);

// Classic vertical scrollbar (trough + bevelled thumb). `value`/`range` are
// content-space; returns thumb rect through `thumb_out` when not null.
// `hover`: the pointer is over the bar; `pressed`: its thumb is held.
// The thumb rect draw_vscrollbar paints, for pointer hit-tests and drags.
// `track_out`: the distance the thumb's top travels (0 when it cannot move).
SDL_Rect vscroll_thumb(const SDL_Rect& trough, float scroll, float max_scroll,
                       float view_h, float* track_out = nullptr);

void draw_vscrollbar(SDL_Renderer* r, const SDL_Rect& trough, float scroll,
                     float max_scroll, float view_h,
                     SDL_Rect* thumb_out = nullptr, bool hover = false,
                     bool pressed = false);

// Pointer state of a draggable strip (gutter, divider, scrollbar).
enum class Hot { None, Hover, Pressed };

// A gutter face: `base` fill and raised bevel, lit while hovered and
// sunken in the accent colour while dragged (touch shows where it landed).
void draw_gutter(SDL_Renderer* r, const SDL_Rect& rc, Rgb base, Hot hot);

// Raised push-button face; `pressed` flips to sunken + darker face.
void draw_button_face(SDL_Renderer* r, const SDL_Rect& rc, bool pressed = false,
                      bool hot = false);
// High-contrast scrollbar thumb, separate from the ordinary button face.
void draw_scroll_thumb(SDL_Renderer* r, const SDL_Rect& rc, bool pressed = false,
                       bool hover = false);

}  // namespace xanatoo::theme
