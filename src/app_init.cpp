// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
//
// App lifecycle: build the initial world state / tear down platform handles.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>

#include "xanatoo/app.hpp"
#include "xanatoo/file_url.hpp"
#include "xanatoo/epub_import.hpp"
#include "xanatoo/fixed_layout.hpp"
#include "xanatoo/gemini.hpp"
#include "xanatoo/lemmy.hpp"
#include "xanatoo/nostr.hpp"
#include "xanatoo/parse.hpp"
#include "xanatoo/platform.hpp"
#include "xanatoo/reddit.hpp"
#include "xanatoo/revstore.hpp"
#include "xanatoo/sidestore.hpp"
#include "xanatoo/statedb.hpp"
#include "xanatoo/util.hpp"

namespace fs = std::filesystem;

namespace xanatoo::app {

// A command-line source path is an alias of its previously imported working
// document. Startup must honor the same alias as regular navigation.
static void add_startup_document(App& app, xanatoo::Document doc,
                                 std::string path,
                                 std::string content_path = {}) {
  if (!path.empty()) {
    const std::string source = is_in_app_remote_url(path)
        ? path : xanatoo::file_doc_key(path);
    if (const auto stored = xanatoo::find_imported_document(source)) {
      if (const auto data = xanatoo::load_stored_document_revision(
              stored->key, stored->working)) {
        const BufferId id = add_buffer(
            app, xanatoo::parse_sexpr(*data), "db:" + stored->key);
        DocBuffer& b = buf(app, id);
        b.doc_key = stored->key;
        b.stored_working = stored->working;
        b.stored_published = stored->published;
        b.stored_title = stored->title;
        b.base_id = xanatoo::content_id(
            xanatoo::serialize_document(b.doc, {}));
        return;
      }
    }
  }
  const BufferId id = add_buffer(app, std::move(doc), path);
  if (content_path != path) buf(app, id).content_path = std::move(content_path);
}

bool app_init(App& app) {
  // Load the user's chrome scale before deriving any pixel geometry.
  xanatoo::load_view_settings(app.view_settings);
  xanatoo::platform_auto_updates(app.view_settings.auto_updates);
  // Chrome metrics, UI font and the default document size follow the
  // display scale (--ui-scale, else density on Android).
  app.ui_scale_base = app.opt.ui_scale > 0.f ? app.opt.ui_scale
                                              : xanatoo::platform_default_ui_scale();
  app.ui_scale = app.ui_scale_base * app.view_settings.ui_scale;
  set_chrome_scale(app.ui_scale);
  print_startup_report(app);
  app.body_px = static_cast<int>(std::lround(app.body_px * app.ui_scale));
  app.heading_px = static_cast<int>(std::lround(app.heading_px * app.ui_scale));
  // Panel geometry defaults (px at scale 1).
  for (FloatWindow* w : {&app.help, &app.bl, &app.ol, &app.sm, &app.pg,
                         &app.history_win, &app.styles_win, &app.downloads_win,
                         &app.annotation_win, &app.revision_win,
                         &app.nostr_explorer_win, &app.nostr_accounts_win,
                         &app.bookmarks_win, &app.subscriptions_win,
                         &app.lemmy_explorer_win, &app.matrix_explorer_win,
                         &app.certs_win, &app.related_win, &app.hover_help_win}) {
    w->x = static_cast<int>(std::lround(w->x * app.ui_scale));
    w->y = static_cast<int>(std::lround(w->y * app.ui_scale));
    w->w = static_cast<int>(std::lround(w->w * app.ui_scale));
    w->h = static_cast<int>(std::lround(w->h * app.ui_scale));
  }
  for (int& w : app.dock_w)
    w = static_cast<int>(std::lround(w * app.ui_scale));
  for (FloatWindow* w : {&app.help, &app.bl, &app.ol, &app.sm, &app.pg,
                         &app.history_win, &app.styles_win, &app.downloads_win,
                         &app.annotation_win, &app.revision_win,
                         &app.nostr_explorer_win, &app.nostr_accounts_win,
                         &app.bookmarks_win, &app.subscriptions_win,
                         &app.lemmy_explorer_win, &app.matrix_explorer_win,
                         &app.certs_win, &app.related_win, &app.hover_help_win}) {
    w->restore_x = w->x;
    w->restore_y = w->y;
    w->restore_w = w->w;
    w->restore_h = w->h;
  }
  app.ol.dock = Dock::Left;  // outline starts as a side column
  app.sm.dock = Dock::Right;
  app.pg.dock = Dock::Left;  // like frames: a side column
  app.annotation_win.dock = Dock::Right;
  app.revision_win.dock = Dock::Right;
  app.nostr_explorer_win.dock = Dock::Right;
  app.nostr_accounts_win.dock = Dock::Right;
  app.certs_win.dock = Dock::Right;
  app.related_win.dock = Dock::Right;
  app.hover_help_win.dock = Dock::Right;  // beside, not over, what it explains
  // Preferred paths are optional — FontConfig resolves a full family set.
  auto existing_font = [](std::string path) {
    if (!path.empty() && !fs::exists(path)) {
      std::cerr << "Font file does not exist: " << path
                << " (falling back to FontConfig)\n";
      path.clear();
    }
    return path;
  };
  app.font_path = existing_font(app.opt.font_path);
  if (const char* env = std::getenv("XANATOO_FONT"); env && env[0] != '\0')
    app.env_font_path = existing_font(env);
  // View prefs name the style sheet, which picks the fonts.
  app.styles = xanatoo::list_styles();
  app.style_pins = xanatoo::load_style_pins();
  init_search(app);
  {
    std::string err;
    if (!xanatoo::load_style(app.view_settings.style, app.style, &err)) {
      std::cerr << "Style: " << err << " (using default)\n";
      app.view_settings.style = "default";
    }
    for (const std::string& w : app.style.warnings)
      std::cerr << "style warning: " << w << "\n";
    std::cerr << "style: " << app.view_settings.style << "\n";
  }
  app.layout_engine.set_style(&app.style);
  app.layout_engine.set_collection_views(&app.collection_views);
  app.renderer.set_style(&app.style);
  if (!app.fonts.load(app.sdl, body_font_path(app), app.body_px,
                      app.heading_px, app.style.fonts)) {
    std::cerr << "Failed to load fonts via FontConfig. "
                 "Pass a TTF with --font or install DejaVu/Liberation.\n";
    return false;
  }
  // Chrome text: same face/size as the ImGui menus, independent of the
  // document style sheet and zoom.
  if (const xanatoo::FontFace ui = xanatoo::ui_font_face(); ui.empty() ||
      !app.ui_font.load(ui, kUiFontPx, app.sdl))
    app.ui_font.load(app.fonts.regular.face(), kUiFontPx, app.sdl);
  if (const xanatoo::FontFace ui = xanatoo::ui_font_face(true); !ui.empty())
    app.ui_font_bold.load(ui, kUiFontPx, app.sdl);
  if (!app.ui_font.path().empty())
    app.ui_font_tiny.load(app.ui_font.face(), kStructTagPx, app.sdl);
  std::cerr << "chrome font: " << app.ui_font.path() << " @ " << kUiFontPx
            << " px\n";
  using xanatoo::sdl::SystemCursor;
  app.cursor_arrow = xanatoo::sdl::create_system_cursor(SystemCursor::Arrow);
  app.cursor_hand = xanatoo::sdl::create_system_cursor(SystemCursor::Hand);
  app.cursor_sizewe = xanatoo::sdl::create_system_cursor(SystemCursor::ResizeEW);
  app.cursor_sizens = xanatoo::sdl::create_system_cursor(SystemCursor::ResizeNS);
  app.cursor_sizenwse = xanatoo::sdl::create_system_cursor(SystemCursor::ResizeNWSE);
  app.cursor_sizeall = xanatoo::sdl::create_system_cursor(SystemCursor::Move);
  app.cursor_current = app.cursor_arrow;
  app.caret_blink_start = xanatoo::sdl::ticks();
  app.minibuf_hist_path = xanatoo::stored_input_history("path");
  app.minibuf_hist_url = xanatoo::stored_input_history("url");
  app.bookmarks = xanatoo::stored_bookmarks();
  app.subscriptions = load_subscriptions();
  app.annotation_layers = xanatoo::list_annotation_layers();
  xanatoo::load_hidden_annotation_layers(app.hidden_annotation_layers);
  for (const auto& layer : app.annotation_layers)
    if (layer.owner == "local" && app.active_annotation_layer.empty())
      app.active_annotation_layer = layer.id;
  load_download_history(app);
  prune_image_caches();

  {
    // file:///a/b.md (+ selectors) → the path; a file named "a#b.md" stays
    // whole when there are no selectors.
    auto local = [](std::string a) {
      xanatoo::Locator loc = xanatoo::parse_locator(a);
      if (!xanatoo::is_file_url(loc.base)) return a;
      std::string path = xanatoo::file_url_path(loc.base);
      if (path.empty()) return a;
      if (!loc.has_selectors()) return path;
      loc.base = std::move(path);
      return xanatoo::format_locator(loc);
    };
    std::string p0 = local(app.opt.file_path);
    std::string p1 = local(app.opt.file_path2);
    if (p0.empty()) {
      p0 = xanatoo::find_default_document();
      if (p0.empty())
        std::cerr << "Note: examples/welcome.md not found; starting empty.\n";
    }
    // The buffer's address is the given path (book.epub); an import's
    // derived Markdown is only where it is read from (content_path).
    auto startup = [&](const std::string& given) {
      const std::string read = resolve_open_path(app, given);
      // A failed import still loads the original for its error page.
      xanatoo::Document doc = read.empty()
          ? load_doc(app, given) : load_doc_or_placeholder(app, read);
      add_startup_document(app, std::move(doc), given,
                           read.empty() ? given : read);
    };
    startup(p0);
    if (!p1.empty()) startup(p1);
  }
  for (const auto& b : app.buffers) app.tab_order.push_back(b.id);
  update_title(app);
  app.layout_engine.set_options(
      xanatoo::LayoutOptions::from_view(app.view_settings, app.ui_scale));
  if (!app.ui.init(app.window, app.sdl, app.ui_scale)) {
    std::cerr << "Warning: ImGui UI failed to init; continuing without menus\n";
  }
  app.panes[0].buf = app.buffers.front().id;
  app.panes[1].buf = app.buffers.size() > 1 ? app.buffers.back().id
                                            : app.buffers.front().id;
  for (int pi = 0; pi < 2; ++pi) {
    Pane& p = app.panes[pi];
    xanatoo::ensure_valid_caret(buf(app, p.buf).doc, p.sel.focus);
    p.sel.anchor = p.sel.focus;
    buf(app, p.buf).history.reset(buf(app, p.buf).doc, p.sel);
  }
  {
    int nw = 0, nh = 0;
    SDL_GetWindowSize(app.window, &nw, &nh);
    if (nw >= 64 && nh >= 64) {
      app.win_w = nw;
      app.win_h = nh;
    }
  }
  const int compact_body_px = static_cast<int>(std::lround(20.f * app.ui_scale));
  if (app.win_w > 0 && app.win_w <= 720 && app.body_px < compact_body_px) {
    app.body_px = compact_body_px;
    app.heading_px = static_cast<int>(std::lround(28.f * app.ui_scale));
  }
  apply_font_sizes(app);
  open_first_gamepad(app);
  if (app.gamepad) {
    app.stick_cursor_on = true;
    note_stick_use(app);
  }
  for (int pi = 0; pi < kCentreViews; ++pi)
    if (const DocBuffer* b = find_buf(app, app.panes[pi].buf))
      push_nav_history(app, pi, b->path);
  // A remote URL or passage given on the command line loads through the navigator
  // (the pane's buffer starts as a "Loading…" placeholder).
  for (int pi = 0; pi < (app.buffers.size() > 1 ? 2 : 1); ++pi) {
    DocBuffer& b = buf(app, app.panes[pi].buf);
    std::error_code ec;
    if (!is_in_app_remote_url(b.path) &&
        (fs::exists(b.path, ec) ||
         !xanatoo::parse_locator(b.path).has_selectors()))
      continue;
    std::string u = std::move(b.path);
    b.path.clear();
    app.focus = pi;
    navigate_to_url(app, std::move(u));
    break;
  }
  app.focus = 0;
  app.split = app.buffers.size() > 1;  // auto-split when two files given
  if (app.opt.file_path.empty() && !app.opt.new_session) restore_session(app);
  app.session_live = true;
  {
    // Drafts nobody is looking at yet (untitled ones have no file to open).
    size_t hidden = 0;
    for (const xanatoo::DraftHead& d : xanatoo::list_drafts())
      if (std::none_of(app.buffers.begin(), app.buffers.end(),
                       [&](const DocBuffer& b) { return b.doc_key == d.key; }))
        ++hidden;
    if (hidden)
      flash_status(app, std::to_string(hidden) +
                            (hidden == 1 ? " unsaved draft" : " unsaved drafts") +
                            " — File → Drafts");
  }
  return true;
}

void app_shutdown(App& app) {
  // Every way out (window close, Quit, signals reaching the loop) keeps
  // unsaved edits as drafts.
  for (const DocBuffer& b : app.buffers) stash_draft(app, b.id);
  save_session(app);
  xanatoo::sdl::stop_text_input(app.window);
  release_pane_rasters(app);
  release_gallery_textures(app);
  release_page_tiles(app);
  xanatoo::release_page_client();
  app.ui.shutdown();
  if (app.gamepad) {
    xanatoo::sdl::close_gamepad(app.gamepad);
    app.gamepad = nullptr;
  }
  SDL_Cursor** cursors[] = {&app.cursor_arrow, &app.cursor_hand,
                            &app.cursor_sizewe, &app.cursor_sizens,
                            &app.cursor_sizenwse,
                            &app.cursor_sizeall};
  for (SDL_Cursor** c : cursors) {
    if (*c) xanatoo::sdl::destroy_cursor(*c);
    *c = nullptr;
  }
}

}  // namespace xanatoo::app
