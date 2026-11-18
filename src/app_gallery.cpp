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
#include <vector>

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

// Edge zones: narrow full-height sides step, a thin strip along the middle
// of the top edge goes up to the page, the button in the top right corner
// to the grid.
enum class Zone { None, Up, Prev, Next, Grid };
int side_width(const SDL_Rect& r) {
  return std::min(std::max(40, r.w / 8), std::max(1, r.w / 3));
}

// The Up strip: thin, so it hides little of the picture (or the grid's
// first row) under it; `inset` is left free at both ends.
SDL_Rect up_strip(const App& app, const SDL_Rect& r, int inset) {
  const int h = std::max(32, static_cast<int>(44.f * app.ui_scale));
  return {r.x + inset, r.y, std::max(1, r.w - 2 * inset), std::min(h, r.h)};
}

// The grid button, in the top right corner (over the Next strip).
SDL_Rect grid_button(const App& app, const SDL_Rect& r) {
  const int d = static_cast<int>(36.f * app.ui_scale);
  const int m = static_cast<int>(8.f * app.ui_scale);
  return {r.x + r.w - m - d, r.y + m, d, d};
}

Zone zone_at(const App& app, const Pane& p, int x, int y) {
  const SDL_Rect r = view_rect(p);
  if (!in_rect(r, x, y)) return Zone::None;
  if (in_rect(grid_button(app, r), x, y)) return Zone::Grid;
  const int side = side_width(r);
  if (x - r.x < side) return Zone::Prev;
  if (x - r.x >= r.w - side) return Zone::Next;
  if (in_rect(up_strip(app, r, side), x, y)) return Zone::Up;
  return Zone::None;
}

// The grid (a scrolled page): its Up strip spans the middle half of the
// text area, the scrollbar excluded.
SDL_Rect grid_rect(const Pane& p) {
  return {p.x, p.y, std::max(1, p.w - sb_reserve(p)), std::max(1, p.h)};
}
SDL_Rect grid_up_strip(const App& app, const Pane& p) {
  const SDL_Rect r = grid_rect(p);
  return up_strip(app, r, r.w / 4);
}

// The gallery grid view whose Up strip is under a window point, -1 when
// none (or a panel covers it).
int grid_up_at(App& app, int x, int y) {
  if (over_float_chrome(app, x, y)) return -1;
  const int pi = pane_at(app, static_cast<float>(x), static_cast<float>(y));
  if (pi < 0) return -1;
  Pane& p = app.panes[pi];
  if (!pane_gallery(p) || pane_image_view(p)) return -1;
  return in_rect(grid_up_strip(app, p), x, y) ? pi : -1;
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

// Up out of the gallery: the page (gallery_up_address), else one level.
void gallery_up(App& app, int pi) {
  std::string to = gallery_up_address(app, app.panes[pi]);
  if (to.empty()) {
    xanatoo::Locator loc =
        xanatoo::parse_locator(pane_address(app, app.panes[pi]));
    if (!xanatoo::locator_up(loc)) return;
    to = xanatoo::format_locator(loc);
  }
  navigate_pane(app, pi, to, true);
}

// The single image's grid: `//gallery#item:N`.
void gallery_grid(App& app, int pi) {
  Pane& p = app.panes[pi];
  if (!pane_image_view(p)) return;
  xanatoo::Locator loc = xanatoo::parse_locator(pane_address(app, p));
  if (loc.pipes.empty() || loc.pipes.back().kind != xanatoo::SelectorKind::Item)
    return;
  loc.point = std::move(loc.pipes.back());
  loc.pipes.pop_back();
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

// The glow textures: white, alpha falling off from the middle of one edge
// (top for Up, left / right for Prev / Next; an ellipse touching the other
// three edges), smooth to zero.
enum class GlowEdge { Top = 0, Left = 1, Right = 2 };
SDL_Texture* glow_texture(App& app, GlowEdge edge) {
  SDL_Texture*& slot = app.gallery_glow[static_cast<int>(edge)];
  if (slot) return slot;
  const bool top = edge == GlowEdge::Top;
  const int W = top ? 128 : 48, H = top ? 48 : 128;
  std::vector<Uint8> px(static_cast<size_t>(W * H * 4));
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      float along, depth;  // along the edge (-1..1), away from it (0..1)
      if (top) {
        along = (x + 0.5f - W / 2.f) / (W / 2.f);
        depth = (y + 0.5f) / static_cast<float>(H);
      } else {
        along = (y + 0.5f - H / 2.f) / (H / 2.f);
        const float fx = (x + 0.5f) / static_cast<float>(W);
        depth = edge == GlowEdge::Left ? fx : 1.f - fx;
      }
      const float t =
          std::max(0.f, 1.f - std::sqrt(along * along + depth * depth));
      Uint8* q = &px[static_cast<size_t>((y * W + x) * 4)];
      q[0] = q[1] = q[2] = 255;
      q[3] = static_cast<Uint8>(255.f * t * t * (3.f - 2.f * t) + 0.5f);
    }
  SDL_Texture* t = SDL_CreateTexture(app.sdl, SDL_PIXELFORMAT_RGBA32,
                                     SDL_TEXTUREACCESS_STATIC, W, H);
  if (!t) return nullptr;
  SDL_UpdateTexture(t, nullptr, px.data(), W * 4);
  SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
  slot = t;
  return t;
}

// A glow over `dst` hanging from `edge`, with `glyph` centred in `band`
// (the part of it nearest the edge).
void draw_glow(App& app, GlowEdge edge, const SDL_Rect& dst,
               const SDL_Rect& band, const std::string& glyph) {
  if (SDL_Texture* glow = glow_texture(app, edge)) {
    SDL_SetTextureAlphaMod(glow, 110);
    xanatoo::sdl::copy(app.sdl, glow, nullptr, &dst);
  }
  xanatoo::Font& font = app.ui_font;
  const float gx = static_cast<float>(band.x + (band.w - font.measure(glyph)) / 2);
  const float gy = static_cast<float>(band.y + (band.h - font.line_height()) / 2 +
                                      font.ascender());
  app.renderer.draw_text(font, gx + 1.f, gy + 1.f, glyph, 0, 0, 0);
  app.renderer.draw_text(font, gx, gy, glyph, 250, 250, 250);
}

// The hovered Up strip: a radial glow hanging from the top edge with ▲ in
// its middle (nothing while the pointer is elsewhere); taller than the strip
// so the light fades out just below it.
void draw_up_strip(App& app, const SDL_Rect& strip) {
  draw_glow(app, GlowEdge::Top, {strip.x, strip.y, strip.w, strip.h * 2},
            strip, "▲");
}

// The hovered Prev / Next zone: the same glow as the Up one (as deep, as
// long as the Up strip is wide, up to the view height), from the middle of
// the view's left or right edge, with ◀ / ▶.
void draw_side_glow(App& app, const SDL_Rect& all, bool next) {
  const SDL_Rect up = up_strip(app, all, side_width(all));
  const int band = up.h;
  const int depth = band * 2;
  const int len = std::max(1, std::min(all.h, up.w));
  const int x = next ? all.x + all.w - depth : all.x;
  const int y = all.y + (all.h - len) / 2;
  const SDL_Rect bx{next ? all.x + all.w - band : all.x, y, band, len};
  draw_glow(app, next ? GlowEdge::Right : GlowEdge::Left, {x, y, depth, len},
            bx, next ? "▶" : "◀");
}

// A 3×3 grid of squares on a dark disc-ish square; brighter when hovered.
void draw_grid_button(App& app, const SDL_Rect& b, bool hot) {
  SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(app.sdl, 0, 0, 0, hot ? 190 : 120);
  xanatoo::sdl::fill_rect(app.sdl, &b);
  const int inset = b.w / 4;
  const int cell = std::max(2, (b.w - 2 * inset) / 3);
  const int gap = std::max(1, cell / 4);
  const int sq = std::max(1, cell - gap);
  const Uint8 c = hot ? 250 : 200;
  SDL_SetRenderDrawColor(app.sdl, c, c, c, 255);
  for (int row = 0; row < 3; ++row)
    for (int col = 0; col < 3; ++col) {
      const SDL_Rect r{b.x + inset + col * cell, b.y + inset + row * cell, sq,
                       sq};
      xanatoo::sdl::fill_rect(app.sdl, &r);
    }
}

}  // namespace

GalleryStyleScope::GalleryStyleScope(App& a, Pane& p) : app(a) {
  if (!pane_gallery(p) || pane_image_view(p)) return;
  style = xanatoo::gallery_view_style(app.style);
  app.layout_engine.set_style(&*style);
  app.renderer.set_style(&*style);
}

GalleryStyleScope::~GalleryStyleScope() {
  if (!style) return;
  app.layout_engine.set_style(&app.style);
  app.renderer.set_style(&app.style);
}

void release_gallery_textures(App& app) {
  for (SDL_Texture*& t : app.gallery_glow) {
    if (t) SDL_DestroyTexture(t);
    t = nullptr;
  }
}

void draw_gallery_grid_chrome(App& app, int pi) {
  Pane& p = app.panes[pi];
  if (!pane_gallery(p) || pane_image_view(p)) return;
  int mx = 0, my = 0;
  xanatoo::sdl::mouse_position(&mx, &my);
  const SDL_Rect r = grid_rect(p);
  xanatoo::sdl::set_clip_rect(app.sdl, &r);
  if (grid_up_at(app, mx, my) == pi) draw_up_strip(app, grid_up_strip(app, p));
}

std::string gallery_up_address(App& app, Pane& p) {
  if (!pane_image_view(p)) return {};
  xanatoo::Locator loc = xanatoo::parse_locator(pane_address(app, p));
  if (!xanatoo::locator_up(loc)) return {};
  // The whole page: at the image itself, not just the collection it is in.
  const xanatoo::GalleryItem& item =
      p.gallery.items[static_cast<size_t>(p.gallery.index)];
  const xanatoo::Document& d = buf(app, p.buf).doc;
  if (loc.pipes.empty() && item.node != xanatoo::INVALID_NODE &&
      item.node <= d.nodes.size())
    loc.point = xanatoo::node_selector(d, item.node);
  return xanatoo::format_locator(loc);
}

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
  if (button == SDL_BUTTON_LEFT && pi >= 0 && grid_up_at(app, x, y) == pi) {
    focus_view(app, pi);
    app.key_focus = KeyFocus::Document;
    gallery_up(app, pi);
    return true;
  }
  if (pi < 0 || !pane_image_view(app.panes[pi]) ||
      !in_rect(view_rect(app.panes[pi]), x, y))
    return false;
  focus_view(app, pi);
  app.key_focus = KeyFocus::Document;
  if (button != SDL_BUTTON_LEFT) return true;
  GalleryView& gv = app.panes[pi].gallery;
  switch (zone_at(app, app.panes[pi], x, y)) {
    case Zone::Up: gallery_up(app, pi); break;
    case Zone::Grid: gallery_grid(app, pi); break;
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
  // Nothing of the (hidden, or under the Up strip) grid is under the pointer.
  if (image_view_at(app, x, y) < 0 && grid_up_at(app, x, y) < 0) return false;
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
    case SDLK_g:
      gallery_grid(app, pi);
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

namespace {

// A picture not drawable yet: "Loading …" with the download's progress, or
// "Cannot load …", across the middle of `all`.
void draw_image_wait(App& app, const SDL_Rect& all, int pad,
                     const std::string& src, const xanatoo::Image& img) {
  xanatoo::Font& font = app.ui_font;
  const int avail_w = all.w - 2 * pad;
  const bool remote = xanatoo::is_remote_image(src);
  if (img.pending && remote) want_image_now(app, src);
  const ImageLoadState st = img.pending && remote ? image_load_state(app, src)
                                                  : ImageLoadState{};
  std::string head = !img.pending  ? "Cannot load "
                     : !remote     ? "Preparing "
                     : st.queued   ? "Waiting to load "
                                   : "Loading ";
  const std::string msg = fit_text(font, head + src, avail_w);
  const int msg_y = all.y + all.h / 2;
  if (img.pending)
    app.renderer.draw_text(font, static_cast<float>(all.x + pad),
                           static_cast<float>(msg_y), msg, 200, 200, 205);
  else
    app.renderer.draw_text(font, static_cast<float>(all.x + pad),
                           static_cast<float>(msg_y), msg, 220, 120, 120);
  if (img.pending) {
    const int bar_h = std::max(4, static_cast<int>(5.f * app.ui_scale));
    const int bw = std::min(avail_w, static_cast<int>(480.f * app.ui_scale));
    const int bar_y = msg_y - font.descender() + pad / 2;
    draw_image_progress_bar(app, SDL_Rect{all.x + pad, bar_y, bw, bar_h}, st,
                            xanatoo::Rgba{60, 60, 66, 255});
    const std::string bytes =
        xanatoo::transfer_label(st.received, st.expected, 0);
    if (!bytes.empty())
      app.renderer.draw_text(font, static_cast<float>(all.x + pad),
                             static_cast<float>(bar_y + bar_h + pad / 2 +
                                                font.ascender()),
                             bytes, 160, 160, 168);
  }
}

std::string file_name_of(const std::string& src) {
  return std::filesystem::path(src.substr(0, src.find_first_of("?#")))
      .filename()
      .string();
}

}  // namespace

void draw_context_image(App& app, const SDL_Rect& all, const std::string& src,
                        const std::string& base) {
  if (all.w <= 0 || all.h <= 0) return;
  xanatoo::sdl::set_clip_rect(app.sdl, &all);
  SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_NONE);
  const xanatoo::Rgba bg = xanatoo::gallery_view_style(app.style).paper;
  SDL_SetRenderDrawColor(app.sdl, bg.r, bg.g, bg.b, 255);
  xanatoo::sdl::fill_rect(app.sdl, &all);

  xanatoo::Font& font = app.ui_font;
  const int line = font.line_height();
  const int pad = static_cast<int>(8.f * app.ui_scale);
  const xanatoo::Image& img = app.images.get(src, base);
  std::string caption = file_name_of(src);
  if (caption.empty()) caption = src;
  if (img.ok && img.texture && img.width > 0 && img.height > 0) {
    const Area area{all.x + pad, all.y + pad, std::max(1, all.w - 2 * pad),
                    std::max(1, all.h - 2 * pad - line)};
    const float sc = fit_scale(area, img);
    const float fw = img.width * sc, fh = img.height * sc;
    SDL_Rect dst{static_cast<int>(area.cx() - fw / 2),
                 static_cast<int>(area.cy() - fh / 2),
                 std::max(1, static_cast<int>(fw)),
                 std::max(1, static_cast<int>(fh))};
    xanatoo::sdl::copy(app.sdl, img.texture, nullptr, &dst);
    caption += "  (" + std::to_string(img.width) + "×" +
               std::to_string(img.height) + " · " +
               std::to_string(static_cast<int>(sc * 100.f + 0.5f)) + "%)";
  } else {
    draw_image_wait(app, all, pad, src, img);
  }
  caption = fit_text(font, caption, all.w - 2 * pad);
  const int cap_w = font.measure(caption);
  app.renderer.draw_text(
      font, static_cast<float>(all.x + (all.w - cap_w) / 2),
      static_cast<float>(all.y + all.h - pad - line + font.ascender()),
      caption, 235, 235, 235);
  xanatoo::sdl::set_clip_rect(app.sdl, nullptr);
}

void draw_gallery_image(App& app, int pi) {
  Pane& p = app.panes[pi];
  const xanatoo::GalleryItem* item = current_item(p);
  if (!item) return;
  GalleryView& gv = p.gallery;
  const SDL_Rect all = view_rect(p);
  xanatoo::sdl::set_clip_rect(app.sdl, &all);
  SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_NONE);
  // The sheet's gallery background, as behind the grid.
  const xanatoo::Rgba bg = xanatoo::gallery_view_style(app.style).paper;
  SDL_SetRenderDrawColor(app.sdl, bg.r, bg.g, bg.b, 255);
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
    draw_image_wait(app, all, pad, item->src, img);
  }

  const std::string counter = std::to_string(gv.index + 1) + " / " +
                              std::to_string(gv.items.size());
  const std::string hint =
      "←/→ step · wheel / pinch / + − zoom · 0 fit · 1 100% · drag pan · g grid · Esc page";
  const int base_y = bot - bottom + pad / 2 + font.ascender();
  std::string caption =
      item->caption.empty() ? file_name_of(item->src) : item->caption;
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
  const Zone z = image_view_at(app, mx, my) == pi ? zone_at(app, p, mx, my)
                                                  : Zone::None;
  draw_grid_button(app, grid_button(app, all), z == Zone::Grid);
  if (z == Zone::Up) draw_up_strip(app, up_strip(app, all, side_width(all)));
  if (z == Zone::Prev || z == Zone::Next)
    draw_side_glow(app, all, z == Zone::Next);
  xanatoo::sdl::set_clip_rect(app.sdl, nullptr);
}

}  // namespace xanatoo::app
