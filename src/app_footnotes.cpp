// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// Footnote overlay: the notes of the footnote marks visible in a pane show in
// a strip along the pane's bottom edge, laid out with the document style like
// the foot of a printed page. A wide text column holds several columns of
// notes side by side, balanced by height. Notes whose own block is on screen
// are left out, and so are the last notes when their marks would sit under
// the strip (fit_footnote_strip); a press on a link in a note follows it, elsewhere on a note
// jumps to the note. The note of the mark under the pointer is washed in
// the style's `mark` colour. A thick rule and a slightly darker sheet set
// the strip apart from the page above it.

#include <algorithm>
#include <utility>

#include "xanatoo/app.hpp"
#include "xanatoo/editor.hpp"
#include "xanatoo/util.hpp"

namespace xanatoo::app {

namespace {

constexpr float kMaxShare = 0.4f;   // of the pane height
constexpr float kPadPx = 8.f;       // above the first note (× ui scale)
constexpr int kFadePx = 12;         // fade where notes are cut off
constexpr float kMinColumnEm = 18.f;  // narrowest note column
constexpr float kGapEm = 2.f;         // between note columns
constexpr size_t kMaxColumns = 6;
constexpr float kRulePx = 3.f;        // divider above the strip (× ui scale)
constexpr int kShadePct = 5;          // strip sheet: paper this % toward ink
constexpr int kShadowPx = 6;          // soft shadow onto the page above

xanatoo::Rgba mix(xanatoo::Rgba a, xanatoo::Rgba b, int pct) {
  auto m = [&](uint8_t x, uint8_t y) {
    return static_cast<uint8_t>((x * (100 - pct) + y * pct) / 100);
  };
  return {m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), 255};
}

// Columns for the first `count` notes of the laid-out ones: whole notes
// balanced by height; sets columns, content_h and shown.
void arrange_columns(FootnoteOverlay& fo, size_t count) {
  count = std::min(count, fo.note_y.size());
  fo.columns.clear();
  fo.content_h = 0.f;
  fo.shown = count;
  if (count == 0) return;
  std::vector<float> heights(fo.note_h.begin(), fo.note_h.begin() + count);
  heights.back() = fo.note_bottom[count - 1] - fo.note_y[count - 1];
  const std::vector<size_t> starts =
      xanatoo::balance_columns(heights, fo.max_columns);
  for (size_t c = 0; c < starts.size(); ++c) {
    const size_t end = c + 1 < starts.size() ? starts[c + 1] : count;
    FootnoteOverlay::Column col;
    col.first = starts[c];
    col.x = fo.left + static_cast<float>(c) * (fo.column_w + fo.gap);
    col.y0 = fo.note_y[col.first];
    float bot = col.y0;
    for (size_t k = col.first; k < end; ++k)
      bot = std::max(bot, fo.note_bottom[k]);
    col.h = bot - col.y0 + 2.f * fo.pad;
    fo.content_h = std::max(fo.content_h, col.h);
    fo.columns.push_back(col);
  }
}

// Lay out the notes for `keys` in columns across the pane's text column.
void build_overlay(App& app, Pane& pane, std::vector<std::string> keys,
                   int width) {
  FootnoteOverlay& fo = pane.footnotes;
  fo.notes.clear();
  fo.note_y.clear();
  fo.note_bottom.clear();
  fo.columns.clear();
  fo.doc = xanatoo::extract_footnotes(doc_of(app, pane), keys, &fo.notes);
  const xanatoo::Document& notes = fo.doc;
  const auto& tops = notes.get(notes.root).children;

  // As many columns as fit the text column the pane's own layout uses.
  const auto [left, text_w] =
      app.layout_engine.text_column(static_cast<float>(width));
  const float em = static_cast<float>(app.fonts.regular.pixel_size());
  const float gap = kGapEm * em;
  size_t n = static_cast<size_t>((text_w + gap) / (kMinColumnEm * em + gap));
  n = std::clamp<size_t>(n, 1, std::max<size_t>(1, tops.size()));
  n = std::min(n, kMaxColumns);
  const float col_w = std::max(
      40.f, (text_w - static_cast<float>(n - 1) * gap) / static_cast<float>(n));

  const xanatoo::LayoutOptions saved = app.layout_engine.options();
  xanatoo::LayoutOptions o = saved;
  o.margin_x = 0.f;
  o.margin_pct = 0.f;
  o.measure_chars = 0;
  o.margin_y = kPadPx * app.ui_scale;
  app.layout_engine.set_options(o);
  fo.layout = app.layout_engine.layout(notes, col_w, base_of(app, pane));
  app.layout_engine.set_options(saved);

  // Top of each note (top-level blocks of `notes`, in order) and the bottom
  // of its last line; a column ends one pad below its last line, not after
  // the blocks' trailing space.
  fo.note_y.assign(tops.size(), 0.f);
  for (const auto& b : fo.layout.boxes) {
    auto it = std::find(tops.begin(), tops.end(), b.node);
    if (it != tops.end()) fo.note_y[it - tops.begin()] = b.y;
  }
  std::vector<float> bottom = fo.note_y;
  const float descent = static_cast<float>(-app.fonts.regular.descender());
  for (const auto& g : fo.layout.glyphs) {
    auto it = std::upper_bound(fo.note_y.begin(), fo.note_y.end(), g.y);
    if (it == fo.note_y.begin()) continue;
    float& bot = bottom[(it - fo.note_y.begin()) - 1];
    bot = std::max(bot, g.y + descent);
  }
  fo.note_h.assign(tops.size(), 0.f);
  for (size_t k = 0; k < tops.size(); ++k)
    fo.note_h[k] =
        (k + 1 < tops.size() ? fo.note_y[k + 1] : bottom[k]) - fo.note_y[k];
  fo.note_bottom = std::move(bottom);
  fo.column_w = col_w;
  fo.gap = gap;
  fo.pad = o.margin_y;
  fo.left = left;
  fo.max_columns = n;
  fo.keys = std::move(keys);
  fo.layout_gen = pane.layout.generation;
  fo.width = width;
  // Strip height for each count of leading notes.
  fo.strip_h.assign(tops.size() + 1, 0.f);
  for (size_t k = 1; k <= tops.size(); ++k) {
    arrange_columns(fo, k);
    fo.strip_h[k] = fo.content_h;
  }
  arrange_columns(fo, tops.size());
}

// Screen x range [x0, x1) of column `c`: half the gap to each neighbour,
// the strip's edge for the outer columns.
std::pair<int, int> column_span(const Pane& pane, const SDL_Rect& strip,
                                size_t c) {
  const FootnoteOverlay& fo = pane.footnotes;
  const auto& col = fo.columns[c];
  const int x0 = c == 0 ? strip.x
                        : pane.x + static_cast<int>(col.x - fo.gap * 0.5f);
  const int x1 = c + 1 == fo.columns.size()
                     ? strip.x + strip.w
                     : pane.x + static_cast<int>(col.x + fo.column_w +
                                                 fo.gap * 0.5f);
  return {std::max(x0, strip.x), std::min(x1, strip.x + strip.w)};
}

// Column under pane-local x `lx` and the layout y of viewport y `my` in it.
struct NotePoint {
  size_t column = 0;
  float y = 0.f;  // in fo.layout
  bool inside = false;  // within the column's notes (not below its end)
};
NotePoint note_point(const FootnoteOverlay& fo, float lx, float my) {
  NotePoint np;
  if (fo.columns.empty()) return np;
  while (np.column + 1 < fo.columns.size() &&
         lx >= fo.columns[np.column + 1].x - fo.gap * 0.5f)
    ++np.column;
  const auto& col = fo.columns[np.column];
  np.y = my - fo.y - fo.pad + col.y0;
  np.inside = my - fo.y < col.h;
  return np;
}

}  // namespace

void draw_footnote_overlay(App& app, int pi, const SDL_Rect& clip) {
  Pane& pane = app.panes[pi];
  FootnoteOverlay& fo = pane.footnotes;
  fo.h = 0.f;
  if (!app.view_settings.footnote_overlay) return;

  const float top = pane.scroll_y;
  const float bottom = top + static_cast<float>(clip.h);
  std::vector<xanatoo::FootnoteMark> marks = xanatoo::visible_footnote_marks(
      doc_of(app, pane), pane.layout, top, bottom);
  // Top to bottom, so the notes the strip leaves out are always the last.
  std::stable_sort(marks.begin(), marks.end(),
                   [](const auto& a, const auto& b) { return a.y < b.y; });
  std::vector<std::string> keys;
  std::vector<float> mark_top, mark_bottom;
  for (const auto& m : marks) {
    if (xanatoo::find_footnote(doc_of(app, pane), m.key) ==
        xanatoo::INVALID_NODE)
      continue;  // no note to show
    const xanatoo::Font& f = app.fonts.pick(pane.layout.glyphs[m.glyph].font);
    keys.push_back(m.key);
    mark_top.push_back(m.y - static_cast<float>(f.ascender()));
    mark_bottom.push_back(m.y - static_cast<float>(f.descender()));
  }
  if (keys.empty()) return;
  const int width = std::max(40, pane.w - sb_gutter(pane));
  if (keys != fo.keys || fo.layout_gen != pane.layout.generation ||
      fo.width != width)
    build_overlay(app, pane, std::move(keys), width);
  if (fo.strip_h.size() < 2) return;

  // Leave out the notes of marks the strip would cover, at a height that
  // follows from the scroll position alone (no feedback from the strip).
  const float max_h = static_cast<float>(clip.h) * kMaxShare;
  std::vector<float> strip_h = fo.strip_h;
  for (float& sh : strip_h) sh = std::min(sh, max_h);
  const xanatoo::FootnoteStripFit fit = xanatoo::fit_footnote_strip(
      mark_top, mark_bottom, strip_h, bottom, max_h);
  if (fit.notes != fo.shown) arrange_columns(fo, fit.notes);
  if (fo.content_h < 1.f) return;

  const int h = static_cast<int>(fit.height);
  const SDL_Rect strip{clip.x, clip.y + clip.h - h, clip.w, h};

  // Step aside while the caret is under the strip so typing stays visible.
  if (!browsing(app) && pi == app.focus) {
    const auto cp = xanatoo::caret_screen_pos(
        pane.layout, pane.sel.focus, pane.scroll_y - static_cast<float>(pane.y),
        static_cast<float>(app.fonts.regular.line_height()));
    if (cp.valid && cp.y + 4.f > static_cast<float>(strip.y)) return;
  }

  const xanatoo::DocStyle& st = app.style;
  xanatoo::sdl::set_clip_rect(app.sdl, &clip);
  SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_BLEND);
  for (int k = 1; k <= kShadowPx; ++k) {  // soft shadow onto the text above
    SDL_SetRenderDrawColor(app.sdl, 0, 0, 0,
                           static_cast<Uint8>(48 * (kShadowPx + 1 - k) /
                                              (kShadowPx * k)));
    xanatoo::sdl::draw_line(app.sdl, strip.x, strip.y - k,
                            strip.x + strip.w - 1, strip.y - k);
  }
  // A sheet of its own, a shade off the page, under a thick rule.
  const xanatoo::Rgba sheet = mix(st.paper, st.ink, kShadePct);
  SDL_SetRenderDrawColor(app.sdl, sheet.r, sheet.g, sheet.b, 255);
  xanatoo::sdl::fill_rect(app.sdl, &strip);
  xanatoo::Rgba rule = st[xanatoo::BlockRole::Footnote].border;
  if (!rule.visible()) rule = mix(st.paper, st.ink, 45);
  const int rule_h = std::max(2, static_cast<int>(kRulePx * app.ui_scale));
  const SDL_Rect rr{strip.x, strip.y, strip.w, std::min(rule_h, strip.h)};
  SDL_SetRenderDrawColor(app.sdl, rule.r, rule.g, rule.b,
                         std::max<Uint8>(rule.a, 160));
  xanatoo::sdl::fill_rect(app.sdl, &rr);

  const auto hot =
      std::find(fo.notes.begin(), fo.notes.end(), hovered_footnote(app, pi));
  for (size_t c = 0; c < fo.columns.size(); ++c) {
    // Each column shows its run of notes; the clip keeps the next column's
    // notes (further down the same layout) out of it.
    const auto& col = fo.columns[c];
    const auto [x0, x1] = column_span(pane, strip, c);
    const int ch = std::min(strip.h, static_cast<int>(col.h + 0.5f));
    const SDL_Rect cr{x0, strip.y, x1 - x0, ch};
    if (cr.w <= 0 || cr.h <= 0) continue;
    xanatoo::sdl::set_clip_rect(app.sdl, &cr);
    const float scroll = col.y0 - fo.pad - static_cast<float>(strip.y);
    app.renderer.draw(fo.layout, scroll, static_cast<float>(pane.x) + col.x);
    // The note of the hovered mark, when it is in this column.
    const size_t end =
        c + 1 < fo.columns.size() ? fo.columns[c + 1].first : fo.shown;
    if (hot != fo.notes.end()) {
      const size_t k = static_cast<size_t>(hot - fo.notes.begin());
      if (k >= col.first && k < end && k < fo.note_bottom.size()) {
        const float pad = 0.25f * kPadPx * app.ui_scale;
        const int y0 = static_cast<int>(fo.note_y[k] - scroll - pad);
        const int y1 = static_cast<int>(fo.note_bottom[k] - scroll + pad);
        const SDL_Rect hr{
            static_cast<int>(static_cast<float>(pane.x) + col.x - pad),
            y0, static_cast<int>(fo.column_w + 2.f * pad), y1 - y0};
        SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(app.sdl, st.mark.r, st.mark.g, st.mark.b,
                               st.mark.a);
        xanatoo::sdl::fill_rect(app.sdl, &hr);
      }
    }
    // The hovered link, when it is one of the strip's.
    if (app.hover.link_pane == pi && app.hover.link_in_footnotes) {
      SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_BLEND);
      const float ox = static_cast<float>(pane.x) + col.x;
      for (const auto& pg : fo.layout.glyphs) {
        if (pg.node != app.hover.link_node ||
            pg.span_index != app.hover.link_span ||
            pg.auto_link != app.hover.link_auto)
          continue;
        xanatoo::Font& font = app.fonts.pick(pg.font);
        const xanatoo::Glyph& g = font.glyph(pg.codepoint);
        const int adv = g.advance > 0 ? static_cast<int>(std::lround(g.advance_px()))
                                      : g.width + 1;
        const int uy = static_cast<int>(pg.y + 3.f - scroll);
        SDL_SetRenderDrawColor(app.sdl, 20, 80, 190, 255);
        const int gx = static_cast<int>(pg.x + ox);
        xanatoo::sdl::draw_line(app.sdl, gx, uy, gx + adv - 1, uy);
        xanatoo::sdl::draw_line(app.sdl, gx, uy + 1, gx + adv - 1, uy + 1);
      }
    }
    if (col.h > static_cast<float>(h)) {
      // More notes than fit: fade the cut edge into the paper.
      SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_BLEND);
      const int y0 = strip.y + strip.h - kFadePx;
      for (int k = 0; k < kFadePx; ++k) {
        SDL_SetRenderDrawColor(app.sdl, sheet.r, sheet.g, sheet.b,
                               static_cast<Uint8>(255 * (k + 1) / kFadePx));
        xanatoo::sdl::draw_line(app.sdl, x0, y0 + k, x1 - 1, y0 + k);
      }
    }
  }
  xanatoo::sdl::set_clip_rect(app.sdl, &clip);

  fo.y = static_cast<float>(strip.y - pane.y);
  fo.h = static_cast<float>(h);
  fo.w = static_cast<float>(strip.w);
}

bool in_footnote_overlay(const App& app, int pi, float lx, float my) {
  const FootnoteOverlay& fo = app.panes[pi].footnotes;
  return fo.h > 0.f && lx >= 0.f && lx < fo.w && my >= fo.y &&
         my < fo.y + fo.h;
}

bool click_footnote_overlay(App& app, int pi, float lx, float my) {
  if (!in_footnote_overlay(app, pi, lx, my)) return false;
  Pane& pane = app.panes[pi];
  const FootnoteOverlay& fo = pane.footnotes;
  if (fo.columns.empty()) return true;
  const NotePoint np = note_point(fo, lx, my);
  const size_t c = np.column;
  const auto& col = fo.columns[c];
  const size_t end =
      c + 1 < fo.columns.size() ? fo.columns[c + 1].first : fo.shown;
  const float cy = np.y;  // y inside the laid-out notes
  size_t k = col.first;
  while (k + 1 < end && fo.note_y[k + 1] <= cy) ++k;
  if (k >= fo.notes.size()) return true;
  const xanatoo::Document& doc = doc_of(app, pane);
  const xanatoo::Node& fn = doc.get(fo.notes[k]);
  if (fn.id != fo.notes[k] || fn.kind != xanatoo::NodeKind::Footnote)
    return true;
  const std::string key = fn.label;
  if (auto dest = xanatoo::jump_to_href(doc, xanatoo::footnote_href(key))) {
    expand_ancestors_for_node(app, dest->node);
    pane.sel.focus = *dest;
    pane.sel.anchor = *dest;
    scroll_caret_into_view(app, pane);
    flash_status(app, "Footnote " + key);
  }
  return true;
}

std::optional<xanatoo::LinkHit> footnote_overlay_link(const App& app, int pi,
                                                      float lx, float my) {
  if (!in_footnote_overlay(app, pi, lx, my)) return std::nullopt;
  const FootnoteOverlay& fo = app.panes[pi].footnotes;
  const NotePoint np = note_point(fo, lx, my);
  if (!np.inside) return std::nullopt;
  const float x = lx - fo.columns[np.column].x;
  if (x < 0.f || x > fo.column_w) return std::nullopt;
  return xanatoo::hit_test_link_ex(fo.doc, fo.layout, x, np.y, 0.f);
}

xanatoo::NodeId hovered_footnote(App& app, int pi) {
  constexpr std::string_view kPrefix = "#fn:";
  if (app.hover.link_pane != pi || pi < 0 || pi >= kMaxViews ||
      !xanatoo::starts_with(app.hover.href, kPrefix))
    return xanatoo::INVALID_NODE;
  return xanatoo::find_footnote(doc_of(app, app.panes[pi]),
                                app.hover.href.substr(kPrefix.size()));
}

}  // namespace xanatoo::app
