// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
//
// Shared classic-desktop chrome drawn on the SDL canvas: floating window
// frames and the status bar. The status *content* is a pure function of the
// world (compute_status); drawing only consumes the resulting record.

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

#include <cstdio>

#include "xanatoo/app.hpp"
#include "xanatoo/audio.hpp"
#include "xanatoo/gemini.hpp"
#include "xanatoo/ebooks.hpp"
#include "xanatoo/feed.hpp"
#include "xanatoo/hackernews.hpp"
#include "xanatoo/forums.hpp"
#include "xanatoo/nntp.hpp"
#include "xanatoo/sitemap.hpp"
#include "xanatoo/wikipedia.hpp"
#include "xanatoo/lemmy.hpp"
#include "xanatoo/matrix.hpp"
#include "xanatoo/native.hpp"
#include "xanatoo/wayback.hpp"
#include "xanatoo/video_sites.hpp"
#include "xanatoo/archive.hpp"
#include "xanatoo/nostr.hpp"
#include "xanatoo/reddit.hpp"
#include "xanatoo/search_results.hpp"
#include "xanatoo/theme.hpp"
#include "xanatoo/util.hpp"

namespace xanatoo::app {

void set_chrome_scale(float scale) {
  auto px = [&](int v) { return static_cast<int>(std::lround(v * scale)); };
  kSbW = px(14);
  kSbLayoutGutter = kSbW + px(4);
  status_bar_full_h = status_bar_h = px(28);
  help_title_h = px(28);
  float_close_w = px(28);
  dock_title_h = px(20);
  kUiFontPx = px(14);
  kUiSmallFontPx = px(12);
  help_pad = px(12);
  help_edge = px(6);
  dock_gap = px(8);
  help_grip_h = px(14);
}

void apply_ui_scale(App& app) {
  if (!app.ui_scale_pending) return;
  app.ui_scale_pending = false;
  app.view_settings.clamp();
  const float next = app.ui_scale_base * app.view_settings.ui_scale;
  const float ratio = next / app.ui_scale;
  if (std::abs(ratio - 1.f) < 0.001f) {
    xanatoo::save_view_settings(app.view_settings);
    return;
  }

  keep_views(app);
  app.ui_scale = next;
  app.body_px = static_cast<int>(std::lround(app.body_px * ratio));
  app.heading_px = static_cast<int>(std::lround(app.heading_px * ratio));
  for (FloatWindow* w : {&app.help, &app.bl, &app.ol, &app.sm, &app.pg,
                         &app.vertical_tabs_win, &app.history_win, &app.styles_win, &app.downloads_win, &app.media_win,
                         &app.annotation_win, &app.revision_win,
                         &app.nostr_explorer_win,
                         &app.lemmy_explorer_win, &app.matrix_explorer_win, &app.bookmarks_win,
                         &app.subscriptions_win, &app.certs_win, &app.cookies_win, &app.identities_win, &app.related_win,
                         &app.hover_help_win}) {
    w->x = static_cast<int>(std::lround(w->x * ratio));
    w->y = static_cast<int>(std::lround(w->y * ratio));
    w->w = static_cast<int>(std::lround(w->w * ratio));
    w->h = static_cast<int>(std::lround(w->h * ratio));
    w->restore_x = static_cast<int>(std::lround(w->restore_x * ratio));
    w->restore_y = static_cast<int>(std::lround(w->restore_y * ratio));
    w->restore_w = static_cast<int>(std::lround(w->restore_w * ratio));
    w->restore_h = static_cast<int>(std::lround(w->restore_h * ratio));
  }
  for (int& width : app.dock_w)
    width = static_cast<int>(std::lround(width * ratio));

  set_chrome_scale(app.ui_scale);
  if (app.ui_font.loaded()) app.ui_font.set_size(kUiFontPx);
  if (app.ui_font_bold.loaded()) app.ui_font_bold.set_size(kUiFontPx);
  if (app.ui_font_small.loaded()) app.ui_font_small.set_size(kUiSmallFontPx);
  if (app.ui_font_small_bold.loaded())
    app.ui_font_small_bold.set_size(kUiSmallFontPx);
  apply_font_sizes(app);
  app.layout_engine.set_options(
      xanatoo::LayoutOptions::from_view(app.view_settings, app.ui_scale));
  for (Pane& pane : app.panes) pane.height_cache.clear();
  app.needs_layout = true;
  app.help_needs_layout = true;
  app.hover_help.needs_layout = true;

  app.ui.shutdown();
  if (!app.ui.init(app.window, app.sdl, app.ui_scale))
    std::cerr << "Warning: ImGui UI failed to reinitialize after scale change\n";
  if (!xanatoo::save_view_settings(app.view_settings))
    std::cerr << "Could not persist UI scale to "
              << xanatoo::view_settings_path() << "\n";
  flash_status(app, "UI scale updated");
}

void toggle_chrome(App& app) {
  app.chrome_hidden = !app.chrome_hidden;
  flash_status(app, app.chrome_hidden
                        ? "Fullscreen — use the document menu to exit"
                        : "Menus and toolbars shown");
}

void fill_doc_paper(App& app, const SDL_Rect& rect) {
  const xanatoo::Rgba& c = app.style.paper;
  SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_NONE);
  SDL_SetRenderDrawColor(app.sdl, c.r, c.g, c.b, 255);
  xanatoo::sdl::fill_rect(app.sdl, &rect);
}

std::string fit_text(xanatoo::Font& font, const std::string& text,
                     int max_w) {
  if (max_w <= 0) return {};
  if (font.measure(text) <= max_w) return text;
  const std::string ell = "…";
  const int budget = max_w - font.measure(ell);
  std::string out;
  int w = 0;
  for (size_t i = 0; i < text.size();) {
    size_t len = 1;
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c >= 0xf0) len = 4;
    else if (c >= 0xe0) len = 3;
    else if (c >= 0xc0) len = 2;
    const std::string cp = text.substr(i, len);
    const int cw = font.measure(cp);
    if (w + cw > budget) break;
    out += cp;
    w += cw;
    i += len;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out + ell;
}

SDL_Rect title_close_zone(const FloatWindow& w) {
  return SDL_Rect{w.x, w.y, float_control_w(w), float_title_h(w)};
}

SDL_Rect title_max_zone(const FloatWindow& w) {
  const int cw = float_control_w(w);
  return SDL_Rect{w.x + w.w - cw, w.y, cw, float_title_h(w)};
}

SDL_Rect title_shade_zone(const FloatWindow& w) {
  const int cw = float_control_w(w);
  return SDL_Rect{w.x + w.w - 2 * cw, w.y, cw, float_title_h(w)};
}

xanatoo::Font& title_font(App& app, const FloatWindow& w, bool bold) {
  if (w.dock != Dock::None) {
    if (bold && app.ui_font_small_bold.loaded()) return app.ui_font_small_bold;
    if (app.ui_font_small.loaded()) return app.ui_font_small;
  }
  if (bold && app.ui_font_bold.loaded()) return app.ui_font_bold;
  return app.ui_font;
}

SDL_Rect title_button_rect(App& app, const FloatWindow& win, const char* label) {
  const int th = float_title_h(win);
  const int shade_x = title_shade_zone(win).x;
  if (win.dock != Dock::None) {
    // Flush with the strip and the shade box, like the title controls.
    const int w = title_font(app, win, false).measure(label) + 12;
    return SDL_Rect{shade_x - w, win.y, w, th};
  }
  const int w = app.ui_font.measure(label) + 16;
  const int h = th - static_cast<int>(std::lround(8.f * app.ui_scale));
  return SDL_Rect{shade_x - 4 - w, win.y + (th - h) / 2, w, h};
}

void draw_title_button(App& app, const FloatWindow& win, const SDL_Rect& r,
                       const char* label, bool latched) {
  namespace th = xanatoo::theme;
  const th::Palette& pal = th::palette();
  xanatoo::Font& font = title_font(app, win, false);
  th::draw_button_face(app.sdl, r, latched, false);
  const th::Rgb ink = latched ? pal.accent_dark : pal.text;
  const int off = latched ? 1 : 0;
  app.renderer.draw_text(
      font, static_cast<float>(r.x + (r.w - font.measure(label)) / 2 + off),
      static_cast<float>(r.y + (r.h - font.line_height()) / 2 +
                         font.ascender() + off),
      label, ink.r, ink.g, ink.b);
}

SDL_Rect outline_focus_rect(App& app, const FloatWindow& w) {
  return title_button_rect(app, w, "Focus");
}

// Title colour per panel family: navigation blue, links teal, sites green,
// time amber, feeds orange, social plum, styling violet, help mint.
static xanatoo::theme::Tint window_tint(WindowRole role) {
  using xanatoo::theme::Tint;
  switch (role) {
    case WindowRole::Document: return Tint::Yellow;
    case WindowRole::Outline:
    case WindowRole::VerticalTabs:
    case WindowRole::Page: return Tint::Blue;
    case WindowRole::Backlinks:
    case WindowRole::Context: return Tint::Teal;
    case WindowRole::Sitemap:
    case WindowRole::RelatedSites: return Tint::Green;
    case WindowRole::Annotations: return Tint::Lime;
    case WindowRole::Subscriptions:
    case WindowRole::Downloads: return Tint::Orange;
    case WindowRole::History:
    case WindowRole::Revisions: return Tint::Amber;
    case WindowRole::Media: return Tint::Red;
    case WindowRole::Bookmarks: return Tint::Rose;
    case WindowRole::Reply:
    case WindowRole::NostrExplorer:
    case WindowRole::LemmyExplorer:
    case WindowRole::MatrixExplorer: return Tint::Plum;
    case WindowRole::Styles:
    case WindowRole::StyleManager: return Tint::Violet;
    case WindowRole::Help:
    case WindowRole::HoverHelp: return Tint::Mint;
    case WindowRole::Cookies:
    case WindowRole::Certificates:
    case WindowRole::Identities: return Tint::Steel;
  }
  return Tint::Yellow;
}

bool draw_floating_frame(App& app, const FloatWindow& win,
                         const std::string& title, bool active,
                         int title_reserve) {
  namespace th = xanatoo::theme;
  const SDL_Rect outer{win.x, win.y, win.w, win.h};
  const bool shaded = float_shaded(win);
  th::WindowChrome w;
  w.outer = outer;
  const int title_h = float_title_h(win);
  w.title_h = title_h;
  w.slot = float_control_w(win);
  w.flush = win.dock != Dock::None;
  w.active = active;
  w.title_pressed = win.dragging;
  w.tint = window_tint(win.role);
  w.resizable = win.dock == Dock::None;
  w.maximized = win.dock == Dock::Max;
  w.shade_enabled = float_docked(win);
  w.shaded = shaded;
  w.scale = app.ui_scale;
  w.shadow = win.dock == Dock::None;
  const SDL_Rect close_zone = title_close_zone(win);
  const SDL_Rect max_zone = title_max_zone(win);
  const SDL_Rect shade_zone = title_shade_zone(win);
  const auto hot = [&](const SDL_Rect& z) {
    return active && app.mouse_x >= z.x && app.mouse_x < z.x + z.w &&
           app.mouse_y >= z.y && app.mouse_y < z.y + z.h;
  };
  w.close_hot = hot(close_zone);
  w.max_hot = hot(max_zone);
  w.shade_hot = hot(shade_zone);
  th::draw_window_chrome(app.sdl, w);
  const th::Palette& p = th::palette();
  // Document well: paper surface in a sunken frame under the title strip.
  if (!shaded) {
    const SDL_Rect well{outer.x + 4, outer.y + title_h + 4, outer.w - 8,
                        outer.h - title_h - 4 - help_grip_h};
    th::fill(app.sdl, well, p.paper);
    th::bevel(app.sdl, well, th::Bevel::Sunken);
  }
  const th::Rgb ink = active ? th::title_tint(w.tint).text : p.text_dim;
  xanatoo::Font& font = title_font(app, win, true);
  const int title_x = close_zone.x + close_zone.w + 4;
  const int title_w = shade_zone.x - title_reserve - 8 - title_x;
  const int ty = outer.y + (title_h - font.line_height()) / 2 +
                 font.ascender();
  app.renderer.draw_text(font, static_cast<float>(title_x),
                         static_cast<float>(ty), fit_text(font, title, title_w),
                         ink.r, ink.g, ink.b);
  return !shaded;
}

void draw_client_scrollbar(App& app, const FloatWindow& w, float max_scroll) {
  if (max_scroll <= 1.f) return;
  const SDL_Rect trough = client_scrollbar_rect(app, w);
  const int mx = static_cast<int>(app.mouse_x);
  const int my = static_cast<int>(app.mouse_y);
  const bool hover = !w.scroll_dragging && mx >= trough.x &&
                     mx < trough.x + trough.w && my >= trough.y &&
                     my < trough.y + trough.h && canvas_hover(app, &w);
  xanatoo::theme::draw_vscrollbar(app.sdl, trough, w.scroll, max_scroll,
                                  static_cast<float>(trough.h), nullptr,
                                  hover, w.scroll_dragging);
}

// First running page navigation (status line + toolbar Stop button).
// "  ·  3/7": the focused view's place in its own Back / Forward trail (the
// shared split trail for the centre panes while split browsing).
static std::string nav_position(App& app) {
  if (int pos = 0, total = 0; split_browsing(app) &&
                             app.focus < kCentreViews &&
                             split_history_position(app, pos, total))
    return "  ·  " + std::to_string(pos) + "/" + std::to_string(total);
  const NavHistory& h = view_nav(app, app.focus);
  if (h.pos < 0 || h.entries.empty()) return {};
  return "  ·  " + std::to_string(h.pos + 1) + "/" +
         std::to_string(static_cast<int>(h.entries.size()));
}

static const FetchJob* loading_job(const App& app) {
  for (const FetchJob& j : app.fetches)
    if (j.purpose == FetchPurpose::Navigate) return &j;
  return nullptr;
}

StatusLine compute_status(App& app) {
  StatusLine st;
  std::string& label = st.text;
  StatusTone& tone = st.tone;
  tone = StatusTone::Hint;
  // The find bar is at its view, not here.
  const bool prompt_shown = prompt_active(app) && !prompt_is_search(app);
  if (std::string over = media_panel_hover(app, static_cast<int>(app.mouse_x),
                                            static_cast<int>(app.mouse_y));
      !over.empty() && !prompt_shown) {
    label = std::move(over);
    tone = StatusTone::Info;
  } else if (prompt_shown) {
    const Prompt& pr = app.prompt;
    tone = StatusTone::Prompt;
    const std::string shown_or_dots = pr.text.empty() ? "…" : pr.text;
    switch (pr.kind) {
      case PromptKind::OpenInWindow:
      case PromptKind::OpenFile:
      case PromptKind::SaveAs:
      case PromptKind::SavePdf:
      case PromptKind::InsertImage:
      case PromptKind::InsertFigure:
        label = prompt_label(pr.kind);
        label += app.minibuf.display_with_cursor();
        label += "    Tab complete · ↑↓ history · Enter · Esc";
        label += app.minibuf.status_hint();
        break;
      case PromptKind::UrlBar:
        label = prompt_label(pr.kind);
        label += app.minibuf.display_with_cursor();
        label += "    ↑↓ history · Enter · Esc · gemini:// or path";
        label += app.minibuf.status_hint();
        break;
      case PromptKind::InsertInclude:
        label = "Include: ";
        label += app.minibuf.display_with_cursor();
        label += "    Tab · ↑↓ · Enter · Esc  (#H · file.md · file.md#H) · Esc";
        break;
      case PromptKind::EditCite:
        label = "Citation key: " + shown_or_dots;
        label += "    Enter confirm · Esc cancel";
        break;
      case PromptKind::EditLink:
        label = "Link: " + shown_or_dots;
        label += "    (optional: href rel=glossary)";
        label += "    Enter confirm · Esc cancel";
        break;
      case PromptKind::HistoryFilter:
        label = "Filter history: " + shown_or_dots;
        label += "    Enter keep · Esc cancel";
        break;
      case PromptKind::EditSummary:
        label = "Stretch summary: " + shown_or_dots;
        label += "    Enter confirm · Esc cancel";
        break;
      case PromptKind::Replace:
      case PromptKind::Search:
      case PromptKind::None:
        break;
    }
  } else if (const FetchJob* job = loading_job(app)) {
    char secs[32];
    std::snprintf(secs, sizeof(secs), "%.1f s",
                  static_cast<double>(xanatoo::sdl::ticks() - job->started) / 1000.0);
    label = "Loading " + job->url + " …  " + secs;
    const std::string bytes = xanatoo::transfer_label(job->shared->meter);
    if (!bytes.empty()) label += "  ·  " + bytes;
    if (app.split) label += job->pane == 0 ? "  (left pane)" : "  (right pane)";
    label += "  ·  Esc stops";
    tone = StatusTone::Info;
  } else if (!app.status_notice.empty() &&
             xanatoo::sdl::ticks() < app.status_notice_until) {
    label = app.status_notice;
    tone = StatusTone::Notice;
  } else if (app.split_preview.active) {
    label = "Preview: " + path_of(app, app.panes[1]);
    label += "  ·  click the link to keep it · move away to drop it";
    tone = StatusTone::Info;
  } else if (app.dragging_block) {
    label = "Moving block — drop before/after any block (columns & lists OK)";
    tone = StatusTone::Info;
  } else if (app.dragging_col_gap) {
    label = "Resizing columns — drag to adjust relative widths";
    tone = StatusTone::Info;
  } else if (buf(app, focused_pane(app).buf).blank_tab) {
    label = "Enter an address or search";
    tone = StatusTone::Hint;
  } else if (app.diff_mode) {
    label =
        "Diff marks on — orange bars mark passages that differ · "
        "Ctrl+Shift+D toggles";
    tone = StatusTone::Warn;
  } else if (browsing(app)) {
    std::string page = path_of(app, focused_pane(app));
    if (page.empty()) page = app.gemini_current_url;
    if (!page.empty() && xanatoo::is_gemini_url(page)) {
      label = page;
      label += nav_position(app);
      label += "  ·  browse · F2 edit";
    } else {
      label = "Browse (read-only) · arrows / gamepad · A/Space open · F2 edit";
    }
    tone = StatusTone::Browse;
  } else if (!path_of(app, focused_pane(app)).empty() &&
             xanatoo::is_gemini_url(path_of(app, focused_pane(app)))) {
    label = path_of(app, focused_pane(app));
    label += nav_position(app);
    tone = StatusTone::Browse;
  } else if (app.structure_mode) {
    label =
        "Structure highlight — nested boxes + small kind tags · "
        "Ctrl+Shift+L toggles";
    tone = StatusTone::Info;
  } else if (focused_pane(app).section_focus) {
    label =
        "Focus section — body shows one section · outline picks target · "
        "Ctrl+Alt+F off";
    tone = StatusTone::Info;
  } else if (panel_has_keys(app, KeyFocus::Outline)) {
    label = "Outline — ↑/↓ · ←/→ fold · Enter jump · Esc close · "
            "typing returns to the text";
    tone = StatusTone::Info;
  } else if (app.hover.col_gap) {
    label = "Column gutter — drag to adjust relative widths";
    tone = StatusTone::Info;
  } else if (app.hover.task != xanatoo::INVALID_NODE) {
    std::string st = "task";
    if (app.hover.task_pane >= 0 && app.hover.task_pane < 2) {
      const auto& tn =
          doc_of(app, app.panes[app.hover.task_pane]).get(app.hover.task);
      st = tn.label.empty() ? "bullet" : tn.label;
    }
    if (st == "task-done")
      label = "Task — click to mark unchecked";
    else if (st == "task")
      label = "Task — click to mark done";
    else
      label = "List marker — click to convert to task checkbox";
    tone = StatusTone::Browse;
  } else if (app.hover.grip != xanatoo::INVALID_NODE) {
    label = "Drag block — drop before/after another block (columns & lists OK)";
    tone = StatusTone::Info;
  } else if (app.hover.transclude != xanatoo::INVALID_NODE) {
    label = "Include: ";
    label +=
        app.hover.transclude_src.empty() ? "(empty)" : app.hover.transclude_src;
    label += "    click to jump to source";
    tone = StatusTone::Info;
  } else if (app.hover.image != xanatoo::INVALID_NODE) {
    if (app.hover.image_card)
      label = "Collection — click to view fullscreen · right-click for "
              "layout / size / sort";
    else if (app.hover.image_handle)
      label = "Image resize handle — drag corner to scale";
    else
      label = "Image — drag corner handle to resize · scale saved in markup";
    tone = StatusTone::Info;
  } else if (app.hover.stretch != xanatoo::INVALID_NODE) {
    bool folded = true, two_body = false;
    if (app.hover.stretch_pane >= 0) {
      Pane& hp = app.panes[app.hover.stretch_pane];
      const auto& sd = doc_of(app, hp);
      folded = xanatoo::stretch_folded(sd, app.hover.stretch,
                                       &buf(app, hp.buf).folds);
      two_body = xanatoo::stretch_summary(sd, app.hover.stretch) !=
                 xanatoo::INVALID_NODE;
    }
    if (two_body)
      label = folded ? "Summary — click for the full text"
                     : "Full text — click to go back to the summary";
    else
      label = folded ? "Stretchtext — click ▸ to expand"
                     : "Stretchtext — click ▾ to collapse";
    tone = StatusTone::Info;
  } else if (app.hover.list_fold != xanatoo::INVALID_NODE) {
    const bool folded =
        app.hover.list_fold_pane >= 0 &&
        xanatoo::list_item_folded(
            doc_of(app, app.panes[app.hover.list_fold_pane]),
            app.hover.list_fold,
            &buf(app, app.panes[app.hover.list_fold_pane].buf).folds);
    label = folded ? "List — click ▸ to expand" : "List — click ▾ to fold";
    tone = StatusTone::Info;
  } else if (!app.hover.href.empty()) {
    if (app.hover.is_cite)
      label = "Cite: ";
    else if (app.hover.href[0] == '#')
      label = "Anchor: ";
    else if (app.hover.href.rfind("external:", 0) == 0)
      label = "System browser: ";
    else if (xanatoo::is_feed_url(app.hover.href))
      label = "Feed: ";
    else if (xanatoo::is_gemini_url(app.hover.href))
      label = "Gemini: ";
    else if (xanatoo::is_reddit_ref(app.hover.href))
      label = "Reddit: ";
    else if (xanatoo::is_lemmy_ref(app.hover.href))
      label = "Lemmy: ";
    else if (xanatoo::is_matrix_ref(app.hover.href))
      label = "Matrix: ";
    else if (xanatoo::is_hn_ref(app.hover.href))
      label = "Hacker News: ";
    else if (xanatoo::is_nntp_url(app.hover.href))
      label = "Usenet: ";
    else if (auto fr = xanatoo::parse_forum_ref(app.hover.href)) {
      static std::string flabel;
      flabel = std::string(xanatoo::forum_site_name(fr->site)) + ": ";
      label = flabel.c_str();
    }
    else if (xanatoo::is_youtube_ref(app.hover.href))
      label = "YouTube: ";
    else if (xanatoo::is_ia_ref(app.hover.href))
      label = "Internet Archive: ";
    else if (xanatoo::is_wayback_ref(app.hover.href))
      label = "Wayback Machine: ";
    else if (auto vs = xanatoo::parse_video_site_ref(app.hover.href)) {
      static std::string vlabel;
      vlabel = std::string(xanatoo::video_site_name(vs->site)) + ": ";
      label = vlabel.c_str();
    } else if (!xanatoo::ebook_download_url(app.hover.href).empty())
      label = "Ebook (download): ";
    else if (xanatoo::is_standard_ebooks_url(app.hover.href))
      label = "Standard Ebooks: ";
    else if (xanatoo::is_nostr_ref(app.hover.href))
      label = "Nostr: ";
    else if (xanatoo::is_wikipedia_ref(app.hover.href))
      label = "Wikipedia: ";
    else if (xanatoo::is_sitemap_ref(app.hover.href))
      label = "Site map: ";
    else if (xanatoo::is_search_ref(app.hover.href))
      label = "Search: ";
    else if (xanatoo::is_media_ref(app.hover.href))
      label = "Media: ";
    else if (xanatoo::is_playable_url(app.hover.href))
      label = "Play: ";
    else if (app.hover.href.rfind("http://", 0) == 0 ||
             app.hover.href.rfind("https://", 0) == 0)
      label = "Web: ";
    else if (app.hover.href.rfind("mailto:", 0) == 0)
      label = "Mail: ";
    else
      label = "Document: ";
    label += app.hover.href;
    if (!app.hover.rel.empty()) {
      label += "  rel=";
      label += app.hover.rel;
    }
    if (app.hover.href[0] == '#')
      label += "    click to jump";
    else if (is_in_app_remote_url(app.hover.href))
      label += "    click to open";
    else if (app.hover.href.rfind("external:", 0) == 0 ||
             app.hover.href.rfind("http://", 0) == 0 ||
             app.hover.href.rfind("https://", 0) == 0 ||
             app.hover.href.rfind("mailto:", 0) == 0)
      label += "    click to open externally";
    else
      label += "    click to open";
    tone = StatusTone::Info;
  } else if (browsing(app)) {
    label = app.gamepad
                ? "Browse · R-stick pointer · A click · LB+A caret · "
                  "D-pad nav · L-stick scroll · Start help · F2 unlock"
                : focused_read_only(app)
                ? "Read-only page — arrows move · Enter open · F1 help"
                : "Locked — arrows move · Enter open · F2 unlocks · F1 help";
    tone = StatusTone::Browse;
  } else {
    label = is_stored_document_ref(path_of(app, focused_pane(app)))
        ? "F1 help · Autosave · Ctrl+A block · Ctrl+Shift+A all · "
          "Ctrl+F find · Ctrl+B/I style · drag to select"
        : "F1 help · Ctrl+S save · Ctrl+A block · Ctrl+Shift+A all · "
          "Ctrl+F find · Ctrl+B/I style · drag to select";
    tone = StatusTone::Hint;
  }

  // Selection / document statistics shown in the right-hand panel
  // (idle status only).
  if (!prompt_shown && !app.help_mode && !app.outline_mode &&
      !buf(app, focused_pane(app).buf).blank_tab) {
    Pane& fp = focused_pane(app);
    StatsCache& sc = fp.stats;
    // Both counts walk the selection / whole document: once per layout.
    if (sc.layout_gen != 0 && sc.layout_gen == fp.layout.generation &&
        sc.buf == fp.buf && sc.sel.anchor == fp.sel.anchor &&
        sc.sel.focus == fp.sel.focus) {
      st.stats = sc.text;
    } else if (!fp.sel.empty()) {
      const std::string t = copied_text(app, fp);
      size_t words = 0, chars = 0;
      bool in_word = false;
      for (unsigned char c : t) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
          in_word = false;
        } else {
          ++chars;
          if (!in_word) {
            in_word = true;
            ++words;
          }
        }
      }
      st.stats = std::to_string(words) + " sel words · " +
                 std::to_string(chars) + " sel chars";
    } else {
      const auto ds = xanatoo::document_stats(doc_of(app, fp));
      st.stats = std::to_string(ds.words) + " words · " +
                 std::to_string(ds.chars) + " chars";
      if (ds.headings > 0)
        st.stats += " · " + std::to_string(ds.headings) + " headings";
    }
    sc.layout_gen = fp.layout.generation;
    sc.buf = fp.buf;
    sc.sel = fp.sel;
    sc.text = st.stats;
  }
  return st;
}

void draw_status_bar(App& app) {
  namespace th = xanatoo::theme;
  const th::Palette& p = th::palette();
  const int bar_h = status_bar_h;
  if (bar_h <= 0) return;  // content only
  const StatusLine st = compute_status(app);
  const SDL_Rect bar{0, app.win_h - bar_h - static_cast<int>(app.ui_bottom),
                     app.win_w, bar_h};
  th::fill(app.sdl, bar, p.face);
  th::hline(app.sdl, bar.x, bar.x + bar.w - 1, bar.y, p.highlight);

  Font& font = app.ui_font;
  const int pad_x = 8;
  const int stats_w = st.stats.empty() ? 0 : font.measure(st.stats) + 2 * pad_x;
  SDL_Rect left{bar.x + 3, bar.y + 3, bar.w - 6 - (stats_w ? stats_w + 3 : 0),
                bar_h - 5};
  SDL_Rect right{bar.x + bar.w - 3 - stats_w, left.y, stats_w, left.h};

  th::Rgb ink = p.text_dim;
  th::Rgb well = p.face;
  switch (st.tone) {
    case StatusTone::Prompt:
      well = p.field;
      ink = p.text;
      break;
    case StatusTone::Notice:
      well = p.notice;
      ink = p.notice_text;
      break;
    case StatusTone::Info:
      ink = p.accent_dark;
      break;
    case StatusTone::Browse:
      ink = th::rgb(0x2E, 0x6B, 0x3C);
      break;
    case StatusTone::Warn:
      ink = th::rgb(0x9A, 0x55, 0x10);
      break;
    case StatusTone::Hint:
      break;
  }
  th::fill(app.sdl, left, well);
  th::bevel(app.sdl, left, th::Bevel::Sunken);
  if (st.tone == StatusTone::Prompt) {
    // Focus ring on the active entry field.
    SDL_Rect ring{left.x + 1, left.y + 1, left.w - 2, left.h - 2};
    SDL_SetRenderDrawColor(app.sdl, p.accent.r, p.accent.g, p.accent.b, 255);
    xanatoo::sdl::draw_rect(app.sdl, &ring);
  }

  const int ty = left.y + (left.h - font.line_height()) / 2 + font.ascender();
  SDL_Rect clip{left.x + 2, left.y + 1, left.w - 4, left.h - 2};
  xanatoo::sdl::set_clip_rect(app.sdl, &clip);
  app.renderer.draw_text(font, static_cast<float>(left.x + pad_x),
                         static_cast<float>(ty), st.text, ink.r, ink.g, ink.b);
  xanatoo::sdl::set_clip_rect(app.sdl, nullptr);

  if (stats_w) {
    th::bevel(app.sdl, right, th::Bevel::Sunken);
    app.renderer.draw_text(font, static_cast<float>(right.x + pad_x),
                           static_cast<float>(ty), st.stats, p.text_dim.r,
                           p.text_dim.g, p.text_dim.b);
  }
}

}  // namespace xanatoo::app
