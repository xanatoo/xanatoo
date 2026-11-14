// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// Gallery views (docs/PASSAGES.md "Gallery"): `page//gallery` shows the
// page's images as a grid of cards (a derived Collection document, built by
// apply_gallery_locator in app_passage.cpp), `page//gallery//item:N` one of
// them filling the view. Everything here is per view: the single image is
// drawn into the pane's rect, takes the pointer, wheel and touch inside it
// and the keys while its view has the focus. Zoom and pan are view state,
// the address names only the image.

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "xanatoo/app.hpp"
#include "xanatoo/util.hpp"

namespace xanatoo::app {

namespace {

// The whole view: a single image has no scrollbar.
SDL_Rect view_rect(const Pane& p) {
  return {p.x, p.y, std::max(1, p.w), std::max(1, p.h)};
}

bool in_rect(const SDL_Rect& r, int x, int y) {
  return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

const xanatoo::GalleryItem* current_item(Pane& p) {
  GalleryView* gv = pane_gallery(p);
  if (!gv || gv->index < 0 ||
      static_cast<size_t>(gv->index) >= gv->items.size())
    return nullptr;
  return &gv->items[static_cast<size_t>(gv->index)];
}

// Edge zones: narrow full-height sides step, the middle of the top strip
// goes back to the grid.
enum class Zone { None, Up, Prev, Next };
int side_width(const SDL_Rect& r) {
  return std::min(std::max(40, r.w / 8), std::max(1, r.w / 3));
}
int top_height(const SDL_Rect& r) { return std::max(48, r.h / 7); }

Zone zone_at(const Pane& p, int x, int y) {
  const SDL_Rect r = view_rect(p);
  if (!in_rect(r, x, y)) return Zone::None;
  const int side = side_width(r);
  if (x - r.x < side) return Zone::Prev;
  if (x - r.x >= r.w - side) return Zone::Next;
  if (y - r.y < top_height(r)) return Zone::Up;
  return Zone::None;
}

// Where the picture lives: the view minus padding and the caption lines.
struct Area {
  int x, y, w, h;
  float cx() const { return x + w / 2.f; }
  float cy() const { return y + h / 2.f; }
};
Area picture_area(App& app, const Pane& p) {
  const SDL_Rect all = view_rect(p);
  const int line = app.ui_font.line_height();
  const int pad = static_cast<int>(16.f * app.ui_scale);
  const int bottom = line * 2 + pad;
  return {all.x + pad, all.y + pad, std::max(1, all.w - 2 * pad),
          std::max(1, all.h - pad - bottom)};
}

float fit_scale(const Area& a, const xanatoo::Image& img) {
  return std::min({static_cast<float>(a.w) / img.width,
                   static_cast<float>(a.h) / img.height, 1.f});
}

// Keep the picture covering the area when larger, centred when smaller.
void clamp_pan(GalleryView& gv, const Area& a, float w, float h) {
  const float mx = std::max(0.f, (w - a.w) / 2.f);
  const float my = std::max(0.f, (h - a.h) / 2.f);
  gv.pan_x = std::clamp(gv.pan_x, -mx, mx);
  gv.pan_y = std::clamp(gv.pan_y, -my, my);
}

void reset_zoom(GalleryView& gv) {
  gv.scale = 0.f;
  gv.pan_x = gv.pan_y = 0.f;
  gv.drag = false;
}

const xanatoo::Image* loaded_image(App& app, Pane& p) {
  const xanatoo::GalleryItem* item = current_item(p);
  if (!item) return nullptr;
  const xanatoo::Image& img = app.images.get(item->src, base_of(app, p));
  if (!img.ok || img.width <= 0 || img.height <= 0) return nullptr;
  return &img;
}

// Up out of the single image: the grid, at its card.
void gallery_up(App& app, int pi) {
  xanatoo::Locator loc = xanatoo::parse_locator(pane_address(app, app.panes[pi]));
  if (xanatoo::locator_up(loc))
    navigate_pane(app, pi, xanatoo::format_locator(loc), true);
}

void toggle_window_fullscreen(App& app) {
  xanatoo::sdl::set_fullscreen(app.window,
                               !xanatoo::sdl::fullscreen(app.window));
}

// The image view under a window point, -1 when none (or a panel covers it).
int image_view_at(App& app, int x, int y) {
  if (over_float_chrome(app, x, y)) return -1;
  const int pi = pane_at(app, static_cast<float>(x), static_cast<float>(y));
  if (pi < 0 || !pane_image_view(app.panes[pi])) return -1;
  return in_rect(view_rect(app.panes[pi]), x, y) ? pi : -1;
}

}  // namespace

GalleryView* pane_gallery(Pane& p) {
  if (p.gallery.buf == kNoBuffer || p.gallery.buf != p.buf ||
      p.source_buf != p.buf || !p.source_doc)
    return nullptr;
  return &p.gallery;
}

bool pane_image_view(const Pane& p) {
  return p.gallery.buf != kNoBuffer && p.gallery.buf == p.buf &&
         p.source_buf == p.buf && p.source_doc && p.gallery.index >= 0 &&
         static_cast<size_t>(p.gallery.index) < p.gallery.items.size();
}

void open_gallery_item(App& app, int pi,
                       std::vector<xanatoo::Selector> scope_pipes, int index) {
  Pane& p = app.panes[pi];
  xanatoo::Locator loc = xanatoo::parse_locator(pane_address(app, p));
  if (loc.base.empty()) return;
  loc.pipes = std::move(scope_pipes);
  loc.pipes.push_back({xanatoo::SelectorKind::Gallery, {}});
  loc.pipes.push_back(
      {xanatoo::SelectorKind::Item, xanatoo::gallery_item_value(index)});
  loc.point.reset();
  focus_view(app, pi);
  navigate_pane(app, pi, xanatoo::format_locator(loc), true);
}

void gallery_step(App& app, int pi, int delta) {
  Pane& p = app.panes[pi];
  if (!pane_image_view(p) || p.locator.pipes.empty() ||
      p.locator.pipes.back().kind != xanatoo::SelectorKind::Item)
    return;
  GalleryView& gv = p.gallery;
  const int n = static_cast<int>(gv.items.size());
  const int to = std::clamp(gv.index + delta, 0, n - 1);
  if (to == gv.index) return;
  // A slide show: the history keeps one entry for the gallery, Back leaves.
  const std::string before = pane_address(app, p);
  gv.index = to;
  reset_zoom(gv);
  p.locator.pipes.back().value = xanatoo::gallery_item_value(to);
  NavHistory& h = p.nav;
  if (h.pos >= 0 && h.pos < static_cast<int>(h.entries.size()) &&
      h.entries[static_cast<size_t>(h.pos)].url == before)
    h.entries[static_cast<size_t>(h.pos)].url = pane_address(app, p);
}

void gallery_zoom(App& app, int pi, float factor, float px, float py) {
  Pane& p = app.panes[pi];
  if (!pane_image_view(p)) return;
  const xanatoo::Image* img = loaded_image(app, p);
  if (!img) return;
  GalleryView& gv = p.gallery;
  const Area a = picture_area(app, p);
  const float fit = fit_scale(a, *img);
  const float cur = gv.scale > 0.f ? gv.scale : fit;
  const float next = std::clamp(cur * factor, 0.02f, 64.f);
  if (next <= fit * 1.001f) {
    reset_zoom(gv);
    return;
  }
  const float f = next / cur;
  const float ox = px - (a.cx() + gv.pan_x), oy = py - (a.cy() + gv.pan_y);
  gv.pan_x = px - a.cx() - ox * f;
  gv.pan_y = py - a.cy() - oy * f;
  gv.scale = next;
  clamp_pan(gv, a, img->width * next, img->height * next);
}

bool gallery_pan(App& app, int pi, float dx, float dy) {
  Pane& p = app.panes[pi];
  if (!pane_image_view(p) || p.gallery.scale <= 0.f) return false;
  const xanatoo::Image* img = loaded_image(app, p);
  if (!img) return false;
  GalleryView& gv = p.gallery;
  gv.pan_x += dx;
  gv.pan_y += dy;
  clamp_pan(gv, picture_area(app, p), img->width * gv.scale,
            img->height * gv.scale);
  return true;
}

bool gallery_pointer_down(App& app, int pi, int x, int y, int button) {
  if (pi < 0 || !pane_image_view(app.panes[pi]) ||
      !in_rect(view_rect(app.panes[pi]), x, y))
    return false;
  focus_view(app, pi);
  app.key_focus = KeyFocus::Document;
  if (button != SDL_BUTTON_LEFT) return true;
  GalleryView& gv = app.panes[pi].gallery;
  switch (zone_at(app.panes[pi], x, y)) {
    case Zone::Up: gallery_up(app, pi); break;
    case Zone::Prev: gallery_step(app, pi, -1); break;
    case Zone::Next: gallery_step(app, pi, 1); break;
    case Zone::None:
      gv.drag = true;
      gv.drag_x = x;
      gv.drag_y = y;
      break;
  }
  return true;
}

bool gallery_pointer_motion(App& app, int x, int y) {
  for (int pi : live_views(app)) {
    Pane& p = app.panes[pi];
    if (!pane_image_view(p) || !p.gallery.drag) continue;
    const float dx = static_cast<float>(x - p.gallery.drag_x);
    const float dy = static_cast<float>(y - p.gallery.drag_y);
    p.gallery.drag_x = x;
    p.gallery.drag_y = y;
    gallery_pan(app, pi, dx, dy);
    return true;
  }
  if (image_view_at(app, x, y) < 0) return false;
  // Nothing of the (hidden) grid is under the pointer.
  clear_interaction_hover(app);
  return true;
}

bool gallery_pointer_up(App& app) {
  bool any = false;
  for (int pi : live_views(app)) {
    any |= app.panes[pi].gallery.drag;
    app.panes[pi].gallery.drag = false;
  }
  return any;
}

bool gallery_wheel(App& app, int pi, int x, int y, int dy) {
  if (pi < 0 || dy == 0 || image_view_at(app, x, y) != pi) return false;
  gallery_zoom(app, pi, dy > 0 ? 1.25f : 0.8f, static_cast<float>(x),
               static_cast<float>(y));
  return true;
}

bool gallery_key(App& app, SDL_Keycode key, bool ctrl, bool alt, bool shift) {
  const int pi = app.focus;
  Pane& p = app.panes[pi];
  GalleryView* gv = pane_gallery(p);
  if (!gv || ctrl || alt) return false;
  if (!pane_image_view(p)) {  // the grid: Esc goes back to the page
    if (key != SDLK_ESCAPE || shift) return false;
    gallery_up(app, pi);
    return true;
  }
  const Area a = picture_area(app, p);
  switch (key) {
    case SDLK_ESCAPE:
    case SDLK_q:
      gallery_up(app, pi);
      break;
    case SDLK_RIGHT:
    case SDLK_DOWN:
    case SDLK_SPACE:
    case SDLK_PAGEDOWN:
    case SDLK_n:
      gallery_step(app, pi, shift && key == SDLK_SPACE ? -1 : 1);
      break;
    case SDLK_LEFT:
    case SDLK_UP:
    case SDLK_BACKSPACE:
    case SDLK_PAGEUP:
    case SDLK_p:
      gallery_step(app, pi, -1);
      break;
    case SDLK_HOME:
      gallery_step(app, pi, -gv->index);
      break;
    case SDLK_END:
      gallery_step(app, pi, static_cast<int>(gv->items.size()));
      break;
    case SDLK_PLUS:
    case SDLK_EQUALS:
    case SDLK_KP_PLUS:
      gallery_zoom(app, pi, 1.25f, a.cx(), a.cy());
      break;
    case SDLK_MINUS:
    case SDLK_KP_MINUS:
      gallery_zoom(app, pi, 0.8f, a.cx(), a.cy());
      break;
    case SDLK_0:
    case SDLK_KP_0:
      reset_zoom(*gv);
      break;
    case SDLK_1:
    case SDLK_KP_1:  // actual size
      if (const xanatoo::Image* img = loaded_image(app, p)) {
        reset_zoom(*gv);
        const float fit = fit_scale(a, *img);
        if (fit < 1.f) gallery_zoom(app, pi, 1.f / fit, a.cx(), a.cy());
      }
      break;
    case SDLK_f:
      toggle_window_fullscreen(app);
      break;
    default:
      return false;
  }
  app.eat_text_input = true;  // '+' and friends are no text
  return true;
}

void draw_gallery_image(App& app, int pi) {
  Pane& p = app.panes[pi];
  const xanatoo::GalleryItem* item = current_item(p);
  if (!item) return;
  GalleryView& gv = p.gallery;
  const SDL_Rect all = view_rect(p);
  xanatoo::sdl::set_clip_rect(app.sdl, &all);
  SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_NONE);
  SDL_SetRenderDrawColor(app.sdl, 18, 18, 20, 255);
  xanatoo::sdl::fill_rect(app.sdl, &all);

  xanatoo::Font& font = app.ui_font;
  const int line = font.line_height();
  const int pad = static_cast<int>(16.f * app.ui_scale);
  const int bottom = line * 2 + pad;
  const int avail_w = all.w - 2 * pad;
  const int bot = all.y + all.h;

  const xanatoo::Image& img = app.images.get(item->src, base_of(app, p));
  std::string dims;
  if (img.ok && img.texture && img.width > 0 && img.height > 0) {
    const Area area = picture_area(app, p);
    const float sc = gv.scale > 0.f ? gv.scale : fit_scale(area, img);
    const float fw = img.width * sc, fh = img.height * sc;
    clamp_pan(gv, area, fw, fh);
    const SDL_Rect clip{area.x, area.y, area.w, area.h};
    xanatoo::sdl::set_clip_rect(app.sdl, &clip);
    SDL_Rect dst{static_cast<int>(area.cx() + gv.pan_x - fw / 2),
                 static_cast<int>(area.cy() + gv.pan_y - fh / 2),
                 std::max(1, static_cast<int>(fw)),
                 std::max(1, static_cast<int>(fh))};
    xanatoo::sdl::copy(app.sdl, img.texture, nullptr, &dst);
    xanatoo::sdl::set_clip_rect(app.sdl, &all);
    dims = std::to_string(img.width) + "×" + std::to_string(img.height) +
           " · " + std::to_string(static_cast<int>(sc * 100.f + 0.5f)) + "%";
  } else {
    const std::string msg = fit_text(
        font, std::string(img.pending ? "Loading " : "Cannot load ") + item->src,
        avail_w);
    app.renderer.draw_text(font, static_cast<float>(all.x + pad),
                           static_cast<float>(all.y + all.h / 2), msg, 220, 120,
                           120);
  }

  const std::string counter = std::to_string(gv.index + 1) + " / " +
                              std::to_string(gv.items.size());
  const std::string hint =
      "←/→ step · wheel / pinch / + − zoom · 0 fit · 1 100% · drag pan · Esc grid";
  const int base_y = bot - bottom + pad / 2 + font.ascender();
  std::string caption =
      item->caption.empty()
          ? std::filesystem::path(item->src.substr(0, item->src.find_first_of("?#")))
                .filename()
                .string()
          : item->caption;
  const int counter_w = font.measure(counter);
  if (!dims.empty()) caption += "  (" + dims + ")";
  caption = fit_text(font, caption, avail_w - 2 * counter_w - pad);
  const int cap_w = font.measure(caption);
  app.renderer.draw_text(font, static_cast<float>(all.x + (all.w - cap_w) / 2),
                         static_cast<float>(base_y), caption, 235, 235, 235);
  app.renderer.draw_text(font,
                         static_cast<float>(all.x + all.w - pad - counter_w),
                         static_cast<float>(base_y), counter, 160, 160, 165);
  const std::string hint_fit = fit_text(font, hint, avail_w);
  const int hint_w = font.measure(hint_fit);
  app.renderer.draw_text(font, static_cast<float>(all.x + (all.w - hint_w) / 2),
                         static_cast<float>(base_y + line), hint_fit, 120, 120,
                         128);

  // Hover hints: the edge zone under the pointer shows its icon.
  int mx = 0, my = 0;
  xanatoo::sdl::mouse_position(&mx, &my);
  const Zone z = image_view_at(app, mx, my) == pi ? zone_at(p, mx, my)
                                                  : Zone::None;
  if (z != Zone::None) {
    const int side = side_width(all);
    SDL_Rect zr;
    const char* glyph;
    if (z == Zone::Up) {
      zr = {all.x + side, all.y, all.w - 2 * side, top_height(all)};
      glyph = "▲";
    } else {
      zr = {z == Zone::Prev ? all.x : all.x + all.w - side, all.y, side, all.h};
      glyph = z == Zone::Prev ? "◀" : "▶";
    }
    SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(app.sdl, 255, 255, 255, 22);
    xanatoo::sdl::fill_rect(app.sdl, &zr);
    const int d = static_cast<int>(40.f * app.ui_scale);
    const int cx = zr.x + zr.w / 2, cy = zr.y + zr.h / 2;
    SDL_Rect bg{cx - d / 2, cy - d / 2, d, d};
    SDL_SetRenderDrawColor(app.sdl, 0, 0, 0, 150);
    xanatoo::sdl::fill_rect(app.sdl, &bg);
    const std::string g = glyph;
    app.renderer.draw_text(
        font, static_cast<float>(cx - font.measure(g) / 2),
        static_cast<float>(cy - line / 2 + font.ascender()), g, 240, 240, 240);
  }
  xanatoo::sdl::set_clip_rect(app.sdl, nullptr);
}

}  // namespace xanatoo::app
