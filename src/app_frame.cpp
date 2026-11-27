// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>

#include "xanatoo/app.hpp"
#include "xanatoo/audio.hpp"
#include "xanatoo/reddit_credentials.hpp"
#include "xanatoo/catalog_credentials.hpp"
#include "xanatoo/platform.hpp"
#include "xanatoo/ebooks.hpp"
#include "xanatoo/epub_import.hpp"
#include "xanatoo/file_url.hpp"
#include "xanatoo/gemini.hpp"
#include "xanatoo/html_page.hpp"
#include "xanatoo/lemmy.hpp"
#include "xanatoo/lemmy_directory.hpp"
#include "xanatoo/matrix_directory.hpp"
#include "xanatoo/catalog_credentials.hpp"
#include "xanatoo/nostr.hpp"
#include "xanatoo/parse.hpp"
#include "xanatoo/pseudo_favicon.hpp"
#include "xanatoo/revstore.hpp"
#include "xanatoo/reddit.hpp"
#include "xanatoo/scrollbar_geometry.hpp"
#include "xanatoo/sidestore.hpp"
#include "xanatoo/snapshot.hpp"
#include "xanatoo/theme.hpp"
#include "xanatoo/time.hpp"
#include "xanatoo/util.hpp"
#include "xanatoo/native.hpp"
#include "xanatoo/paths.hpp"
#include "xanatoo/wayback.hpp"
#include "xanatoo/wikipedia.hpp"

namespace fs = std::filesystem;

namespace xanatoo::app {

void apply_window_size(App& app, int nw, int nh) {
  if (nw < 64 || nh < 64 || (nw == app.win_w && nh == app.win_h)) return;
  if (nh < app.win_h) app.reveal_caret_after_layout = true;
  app.win_w = nw;
  app.win_h = nh;
  app.needs_layout = true;
  place_windows(app);
}

void sync_window_size(App& app) {
  int nw = 0, nh = 0;
  SDL_GetWindowSize(app.window, &nw, &nh);
  apply_window_size(app, nw, nh);
  // A docked window opened, closed or moved: the pane area changed.
  const Pane& last = app.split ? app.panes[1] : app.panes[0];
  if (app.panes[0].x != dock_column_w(app, Dock::Left) +
                              (app.panes[0].sb_left ? kSbW : 0) ||
      // The edit strip of an editable document sits above y (head_h).
      app.panes[0].y - app.panes[0].head_h !=
          static_cast<int>(app.ui_top) + dock_band_h(app, Dock::Top) ||
      last.x + last.w != app.win_w - dock_column_w(app, Dock::Right))
    app.needs_layout = true;
}

void relayout_if_needed(App& app) {
  if (app.needs_layout && app.win_w >= 64 && app.win_h >= 64) {
    layout_panes(app);
    if (app.reveal_caret_after_layout) {
      app.reveal_caret_after_layout = false;
      scroll_caret_into_view(app, focused_pane(app));
    }
    {
      std::string dk = path_of(app, focused_pane(app));
      if (dk != app.last_fold_doc_key) {
        app.last_fold_doc_key = dk;
        reload_folds_for_focused(app);
      }
    }
    if (app.outline_mode) refresh_outline(app);
    if (app.backlinks_mode) refresh_backlinks(app);
  }
}

// Icons share the image cache and its asynchronous remote fetcher.
// Accent of the native service a tab's page belongs to (0 = none). A pure
// memo of service_bar(), which parses the address.
static uint32_t service_tab_color(const std::string& path) {
  static std::unordered_map<std::string, uint32_t> memo;
  if (const auto it = memo.find(path); it != memo.end()) return it->second;
  if (memo.size() > 1024) memo.clear();
  const auto bar = xanatoo::service_bar(path);
  return memo[path] = bar ? bar->color : 0u;
}

static SDL_Texture* address_icon(App& app, const std::string& address) {
  std::string base = xanatoo::parse_locator(address).base;
  std::string web = xanatoo::web_url_for(base);
  if (!web.empty()) base = web;
  if (xanatoo::starts_with_ci(base, "http://") ||
      xanatoo::starts_with_ci(base, "https://")) {
    const size_t start = base.find("://") + 3;
    const size_t end = base.find_first_of("/?#", start);
    if (end == start || start >= base.size()) return nullptr;
    // Icons the site declared (<link rel=icon>, learnt from its pages)
    // first, best first; /favicon.ico when none of them decodes.
    if (auto it = app.site_icons.find(xanatoo::url_host(base));
        it != app.site_icons.end()) {
      for (const std::string& url : it->second) {
        const auto& image = app.images.get(url);
        if (image.ok) return image.texture;
        if (image.pending) return nullptr;
      }
    }
    base = base.substr(0, end) + "/favicon.ico";
  } else {
    // thumtoo can render the first page of a local PDF as a thumbnail.
    if (!xanatoo::ends_with(xanatoo::ascii_lower(base), ".pdf")) return nullptr;
  }
  const bool remote = xanatoo::is_remote_image(base);
  const auto& image = remote ? app.images.get(base) : app.images.thumb(base, {}, 128);
  return image.ok ? image.texture : nullptr;
}

static void build_history_menu(App& app, xanatoo::UiState& state) {
  // The toolbar's history lists must describe the very same trail used by
  // nav_step(), including the shared trail while split browsing.
  std::vector<std::string> urls;
  int pos = -1;
  if (split_browsing(app) && app.focus < kCentreViews) {
    int one_based = 0, total = 0;
    if (!split_history_position(app, one_based, total)) return;
    pos = one_based - 1;
    for (const SplitHistoryLevel& level : app.split_history.levels) {
      if (level.opened.empty()) {
        urls.push_back(level.page.url);
      } else {
        for (const SplitHistoryEntry& e : level.opened)
          urls.push_back(e.url);
      }
    }
  } else {
    const NavHistory& h = view_nav(app, app.focus);
    pos = h.pos;
    for (const NavHistoryEntry& e : h.entries) urls.push_back(e.url);
  }
  if (pos < 0 || pos >= static_cast<int>(urls.size())) return;
  state.back_history = xanatoo::history_menu_items(urls, pos, -1);
  state.forward_history = xanatoo::history_menu_items(urls, pos, 1);
}

// Fill the chrome-facing snapshot of app state for this frame (also drives the
// software gamepad pointer, which synthesises mouse events).
static void build_ui_state(App& app, xanatoo::UiState& ui_state) {
  sync_pinned_style(app);
  ui_state.dirty =
      buf(app, focused_pane(app).buf).dirty;
  const auto& focused_path = path_of(app, focused_pane(app));
  ui_state.can_publish =
      (focused_path.empty() || is_stored_document_ref(focused_path)) &&
      !buf(app, focused_pane(app).buf).placeholder;
  std::error_code source_ec;
  ui_state.can_make_local_copy =
      (is_stored_original_ref(focused_path) ||
       is_in_app_remote_url(focused_path) ||
       fs::is_regular_file(focused_path, source_ec)) &&
      !buf(app, focused_pane(app).buf).placeholder;
  ui_state.has_path = !path_of(app, focused_pane(app)).empty();
  {
    const Pane& p = focused_pane(app);
    if (p.source_buf == p.buf && p.source_doc &&
        p.locator_buf == p.buf && !p.locator.pipes.empty() &&
        xanatoo::is_source_pipe(p.locator.pipes.front().kind))
      switch (p.locator.pipes.front().kind) {
        case xanatoo::SelectorKind::SourceRaw:
          ui_state.source_mode = "source-raw";
          break;
        case xanatoo::SelectorKind::SourceXml:
          ui_state.source_mode = "source-xml";
          break;
        default: break;
      }
  }
  ui_state.gallery = pane_gallery(focused_pane(app)) != nullptr;
  ui_state.split = app.split;
  ui_state.trail = app.split && app.tile_mode == TileMode::Trail;
  ui_state.tiles_stacked = app.tile_axis == TileAxis::Rows;
  ui_state.loading = std::any_of(
      app.fetches.begin(), app.fetches.end(), [](const FetchJob& j) {
        return j.purpose == FetchPurpose::Navigate;
      });
  ui_state.outline = app.outline_mode;
  ui_state.section_focus = focused_pane(app).section_focus;
  ui_state.justify = app.view_settings.justify;
  ui_state.footnote_overlay = app.view_settings.footnote_overlay;
  ui_state.bookmark_marks = app.view_settings.bookmark_marks;
  ui_state.heading_ticks = app.view_settings.heading_ticks;
  ui_state.hover_preview = app.view_settings.hover_preview;
  ui_state.history_rewinds = app.view_settings.history_rewinds;
  ui_state.auto_links = app.view_settings.auto_links;
  ui_state.auto_images = app.view_settings.auto_images;
  ui_state.auto_updates = app.view_settings.auto_updates;
  ui_state.reader_default = app.view_settings.reader_mode;
  ui_state.reader = pane_reader(app, focused_pane(app));
  ui_state.paged = pane_paged(app, focused_pane(app));
  ui_state.paged_default = app.view_settings.paged;
  ui_state.page_columns = app.view_settings.page_columns;
  ui_state.has_chrome = focused_pane(app).chrome_parts > 0;
  ui_state.preferences_open = app.preferences_open;
  ui_state.download_confirm_open = app.download_confirm.open;
  ui_state.download_confirm_url = app.download_confirm.url;
  ui_state.credentials_open = app.credentials_open;
  ui_state.backlinks = app.backlinks_mode;
  ui_state.sitemap = app.sitemap.shown;
  ui_state.page_panel = app.page.shown;
  ui_state.has_nav = page_has_navigation(app);
  ui_state.has_headings = focused_pane(app).has_headings;
#if defined(XANATOO_WEB) && XANATOO_WEB
  ui_state.has_site = sitemap_button_shown(app);
#endif
  ui_state.searchbar = app.search.open;
  ui_state.audio = app.audio.state != AudioState::Idle;
  ui_state.audio_playing = app.audio.state == AudioState::Playing;
  ui_state.audio_ab = app.audio.ab;
  ui_state.audio_loop = app.audio.loop;
  ui_state.audio_video = app.audio.video_w > 0;
  ui_state.audio_fullscreen = media_fullscreen(app);
  ui_state.audio_controls_hidden = app.media_controls_hidden;
  ui_state.search_engine = search_engine_id(app);
  ui_state.site_search = xanatoo::engine_for_address(
      path_of(app, focused_pane(app)), app.search.active);
  // The page's service strip has a search field for it already.
  if (const Pane& fp = focused_pane(app);
      !ui_state.site_search.empty() && fp.head_h > 0 && fp.head_service &&
      fp.service &&
      xanatoo::engine_for_address(service_address(app, fp.service_addr),
                                  app.search.active) == ui_state.site_search)
    ui_state.site_search.clear();
  ui_state.offered_engines.clear();
  if (xanatoo::engine_for_address(path_of(app, focused_pane(app)),
                                  app.search.engines).empty())
    for (const xanatoo::OpenSearchLink& o : focused_page_info(app).links.searches)
      ui_state.offered_engines.push_back(
          {o.url, o.title.empty() ? xanatoo::url_host(o.url) : o.title});
  ui_state.adding_engine = app.search.adding != nullptr;
  ui_state.search_engines.clear();
  for (const auto& e : app.search.engines)
    ui_state.search_engines.push_back(
        {e.id, e.name, e.keyword, e.badge, xanatoo::engine_color(e),
         xanatoo::engine_on(e, app.view_settings.search_on,
                            app.view_settings.search_off),
         e.note});
  ui_state.diff = app.diff_mode;
  ui_state.can_diff = !shared_doc(app) &&
                      app.panes[0].buf != app.panes[1].buf;
  ui_state.structure = app.structure_mode;
  ui_state.view_line_spacing = app.view_settings.line_spacing;
  ui_state.view_margin_pct = app.view_settings.margin_pct;
  ui_state.view_measure_chars = app.view_settings.measure_chars;
  ui_state.ui_scale = app.view_settings.ui_scale;
  ui_state.styles.clear();
  for (const auto& e : app.styles) ui_state.styles.emplace_back(e.name, e.title);
  ui_state.style = app.style.name;
  ui_state.default_style = app.view_settings.style;
  ui_state.style_colors = app.style.colors_from;
  ui_state.style_fonts = app.style.fonts_from;
  ui_state.style_entries = app.styles;
  ui_state.style_pins = app.style_pins;
  ui_state.style_save_generation = app.style_save_generation;
  ui_state.style_pin_candidates = xanatoo::style_pin_keys(path_of(app, focused_pane(app)));
  ui_state.browse = browsing(app);
  ui_state.can_edit = !focused_read_only(app);
  ui_state.chrome_hidden = app.chrome_hidden || media_fullscreen(app);
  ui_state.virtual_keyboard = app.virtual_keyboard;
  ui_state.help = app.help_mode;
  ui_state.hover_help = app.hover_help.open;
  ui_state.selection = !focused_pane(app).sel.empty();
  ui_state.context_comment_native = app.context_comment_native;
  ui_state.context_comment_web = app.context_comment_web;
  ui_state.context_image_address = app.context_image_address;
  ui_state.context_save_pdf = !app.context_pdf_path.empty();
  ui_state.context_link = app.context_link;
  ui_state.context_link_href = app.context_link_href;
  ui_state.context_link_web = app.context_link_web;
  ui_state.context_link_rel = app.context_link_rel;
  ui_state.context_link_text = app.context_link_text;
  ui_state.context_blocks.clear();
  ui_state.context_article = ui_state.context_section = -1;
  if (find_buf(app, app.context_blocks_buf) && app.context_blocks_pane >= 0 &&
      app.panes[app.context_blocks_pane].buf == app.context_blocks_buf) {
    const xanatoo::Document& d = buf(app, app.context_blocks_buf).doc;
    for (const xanatoo::NodeId n : app.context_blocks) {
      if (n >= d.next_id || d.get(n).id != n) {
        ui_state.context_blocks.clear();
        break;
      }
      const auto kind = d.get(n).kind;
      const int at = static_cast<int>(ui_state.context_blocks.size());
      if (kind == xanatoo::NodeKind::Article && ui_state.context_article < 0)
        ui_state.context_article = at;
      if (kind == xanatoo::NodeKind::Section && ui_state.context_section < 0)
        ui_state.context_section = at;
      std::string label(xanatoo::block_kind_name(kind));
      const std::string title = xanatoo::block_title(d, n);
      if (!title.empty()) label += " \xC2\xB7 " + title;  // ·
      ui_state.context_blocks.push_back(std::move(label));
    }
    if (ui_state.context_blocks.empty())
      ui_state.context_article = ui_state.context_section = -1;
  }
  ui_state.context_can_reply =
      app.context_reply_buffer != kNoBuffer &&
      app.context_reply_node != xanatoo::INVALID_NODE;
  ui_state.context_quote_items.clear();
  for (const auto& q : app.context_quotes)
    ui_state.context_quote_items.push_back(q.label);
  // Edit strips: every centre tile with one, and the focused document
  // window's (an unfocused window paints a plain band, draw_soft_window).
  ui_state.edit_bars.clear();
  for (const int vi : live_views(app)) {
    const Pane& ep = app.panes[vi];
    if (ep.head_h <= 0 || ep.head_service) continue;
    if (vi >= kCentreViews && vi != app.focus) continue;
    const DocBuffer* eb = find_buf(app, ep.buf);
    if (!eb) continue;
    xanatoo::UiEditBar bar;
    bar.view = vi;
    bar.x = ep.x - (ep.sb_left ? kSbW : 0);
    bar.y = ep.y - ep.head_h;
    bar.w = ep.w + (ep.sb_left ? kSbW : 0);
    bar.h = ep.head_h;
    bar.focused = vi == app.focus;
    bar.floating = vi >= kCentreViews;
    bar.locked = pane_locked(app, ep);
    bar.dirty = eb->dirty;
    bar.selection = bar.focused && !ep.sel.empty();
    ui_state.edit_bars.push_back(bar);
  }
  // Service strips: the same places, for the pages of native services.
  ui_state.service_bars.clear();
  for (const int vi : live_views(app)) {
    Pane& sp = app.panes[vi];
    if (sp.head_h <= 0 || !sp.head_service || !sp.service) continue;
    if (vi >= kCentreViews && vi != app.focus) continue;
    const xanatoo::ServiceBar& s = *sp.service;
    xanatoo::UiServiceBar bar;
    bar.view = vi;
    bar.x = sp.x - (sp.sb_left ? kSbW : 0);
    bar.y = sp.y - sp.head_h;
    bar.w = sp.w + (sp.sb_left ? kSbW : 0);
    bar.h = sp.head_h;
    bar.floating = vi >= kCentreViews;
    bar.id = s.id;
    bar.name = s.name;
    bar.home = s.home;
    bar.color = s.color;
    bar.rows = s.rows;
    const std::string target = service_address(app, sp.service_addr);
    bar.icon = address_icon(app, target);
    if (!bar.icon && !s.home.empty()) bar.icon = address_icon(app, s.home);
    bar.search_engine = xanatoo::engine_for_address(target, app.search.active);
    for (const auto& e : app.search.active)
      if (e.id == bar.search_engine) bar.search_name = e.name;
    ui_state.service_bars.push_back(std::move(bar));
  }
  ui_state.find_open = find_bar_shown(app) && !media_fullscreen(app);
  if (ui_state.find_open) {
    const SDL_Rect r = find_bar_rect(app);
    const FindStatus st = find_status(app);
    ui_state.find_x = r.x;
    ui_state.find_y = r.y;
    ui_state.find_w = r.w;
    ui_state.find_h = r.h;
    ui_state.find_replace = app.prompt.kind == PromptKind::Replace;
    ui_state.find_text = app.prompt.text;
    ui_state.find_replacement = app.prompt.text2;
    ui_state.find_focus_seq = app.find_focus_seq;
    ui_state.find_select_all = app.prompt.fresh;
    ui_state.find_count = st.count;
    ui_state.find_capped = st.capped;
    ui_state.find_index = st.index;
    ui_state.find_can_replace = !browsing(app);
  }
  ui_state.reply_open = app.reply.open;
  ui_state.reply_generation = app.reply.generation;
  ui_state.reply_author = app.reply.author;
  ui_state.reply_page = app.reply.page;
  ui_state.reply_label = app.reply.label;
  ui_state.reply_excerpt = app.reply.excerpt;
  ui_state.reply_text = app.reply.text;
  reply_ui_state(app, ui_state);
  if (app.reply.open) {
    auto [x, y, w, h] = window_client_rect(app.reply_win, false);
    ui_state.reply_x = x;
    ui_state.reply_y = y;
    ui_state.reply_w = w;
    ui_state.reply_h = h;
  }
  {
    auto& fd = doc_of(app, focused_pane(app));
    auto nid = focused_pane(app).sel.focus.node;
    // Guard: caret can briefly point at a node from a replaced tree.
    bool nid_ok = false;
    if (nid != xanatoo::INVALID_NODE) {
      try {
        (void)fd.get(nid);
        nid_ok = true;
      } catch (...) {
        xanatoo::ensure_valid_caret(fd, focused_pane(app).sel.focus);
        focused_pane(app).sel.anchor = focused_pane(app).sel.focus;
        nid = focused_pane(app).sel.focus.node;
        try {
          (void)fd.get(nid);
          nid_ok = nid != xanatoo::INVALID_NODE;
        } catch (...) {
          nid_ok = false;
        }
      }
    }
    ui_state.on_list_item =
        nid_ok && fd.get(nid).kind == xanatoo::NodeKind::ListItem;
    ui_state.in_stretch =
        nid_ok && xanatoo::enclosing_stretch(fd, nid) != xanatoo::INVALID_NODE;
  }
  for (auto& bar : ui_state.edit_bars) {
    if (!bar.focused) continue;
    bar.on_list_item = ui_state.on_list_item;
    bar.in_stretch = ui_state.in_stretch;
  }
  ui_state.doc_label = path_of(app, focused_pane(app)).empty()
                           ? std::string("untitled")
                           : pane_address(app, focused_pane(app));
  ui_state.can_nav_back = can_nav_step(app, -1);
  ui_state.snapshot_address.clear();
  if (const DocBuffer* b = find_buf(app, focused_pane(app).buf);
      b && !b->placeholder && !b->observed.snapshot.empty())
    ui_state.snapshot_address = xanatoo::snapshot_ref_string(
        b->observed.snapshot,
        xanatoo::parse_locator(pane_address(app, focused_pane(app))).base);
  build_history_menu(app, ui_state);
  {
    const std::string& path = path_of(app, focused_pane(app));
    ui_state.alternate_url = xanatoo::alternate_view_url(path);
    ui_state.web_page_url = pane_web_url(app, focused_pane(app));
    ui_state.browser_url = pane_browser_url(app, focused_pane(app));
    ui_state.native_view = !ui_state.alternate_url.empty() &&
                           xanatoo::native_url_for(path).empty();
    if (auto snap = xanatoo::wayback_ref_from_snapshot_url(path)) {
      ui_state.archive_view = true;
      ui_state.archive_url = snap->url;
    } else {
      ui_state.archive_view = false;
      ui_state.archive_url = xanatoo::wayback_lookup_ref(path);
    }
    ui_state.up_url = page_up_url(app);
    ui_state.page_languages.clear();
    {
      const PageInfoCache& info = focused_page_info(app);
      bool other = false, marked = false;
      for (const xanatoo::LanguageLink& l : info.links.languages) {
        // The site's own name for it (title="Deutsch"), else ours.
        std::string label = l.title.empty() || l.title.size() > 40
                                ? xanatoo::language_name(l.lang) : l.title;
        if (label != l.lang) label += "  (" + l.lang + ")";
        std::string fallback = xanatoo::language_english_name(l.lang);
        if (fallback != l.lang) fallback += "  (" + l.lang + ")";
        const bool current = l.url == info.path;
        marked = marked || current;
        other = other || !current;
        ui_state.page_languages.push_back(
            {l.url, std::move(label), std::move(fallback), current});
      }
      // No link names this very URL: the page's own <html lang> is current.
      if (!marked && !info.links.lang.empty())
        for (size_t i = 0; i < info.links.languages.size(); ++i)
          if (xanatoo::ascii_lower(info.links.languages[i].lang) ==
              xanatoo::ascii_lower(info.links.lang)) {
            ui_state.page_languages[i].current = true;
            break;
          }
      if (!other) ui_state.page_languages.clear();
    }
    ui_state.languages.clear();
    for (const std::string& t : app.view_settings.languages)
      ui_state.languages += (ui_state.languages.empty() ? "" : ", ") + t;
    ui_state.page_feeds.clear();
    for (const xanatoo::FeedLink& f : page_feeds(app))
      ui_state.page_feeds.push_back(
          {f.url, f.title.empty() ? f.type + " feed" : f.title + " (" + f.type + ")"});
  }
  if (ui_state.services.empty()) {
    // Quick access to the in-app clients, grouped by what they speak. A
    // protocol with many hosts opens its directory page (examples/services/,
    // all of it linked from services.md) instead of one arbitrary host.
    // Greyed out when `probe` (else `target`) needs a feature not built in.
    struct Entry {
      const char* group;  // heading before this entry, or nullptr
      const char* label;
      const char* icon;
      const char* target;
      bool example;
      const char* probe;
    };
    static const Entry kServices[] = {
        {"Reference", "Wikipedia", "language", "wikipedia:Main_Page", false, nullptr},
        {nullptr, "Internet Archive (Prelinger films)", "history", "ia:prelinger", false,
         nullptr},
        {nullptr, "Standard Ebooks", "reader", "https://standardebooks.org/ebooks", false,
         nullptr},
        {"Repositories", "GitHub", "code", "github:", false, nullptr},
        {nullptr, "GitLab", "code", "gitlab:", false, nullptr},
        {"News and forums", "Hacker News", "site-feed", "hn:", false, nullptr},
        {nullptr, "Lobsters", "link", "lobsters:", false, nullptr},
        {nullptr, "Digg", "bookmark", "digg:", false, nullptr},
        {nullptr, "LessWrong", "note", "lesswrong:", false, nullptr},
        {nullptr, "Reddit (r/popular)", "related", "reddit:r/popular/", false, nullptr},
        {nullptr, "4chan (boards)", "image", "fourchan:", false, nullptr},
        {nullptr, "Lemmy instances", "lemmy", "services/lemmy.md", true,
         "lemmy:lemmy.world/"},
        {nullptr, "Discourse forums", "context", "services/discourse.md", true,
         "discourse:discourse.nixos.org"},
        {nullptr, "Flarum forums", "quote", "services/flarum.md", true,
         "flarum:discuss.flarum.org"},
        {nullptr, "Stack Exchange sites", "lists", "services/stackexchange.md", true,
         "stackexchange:stackoverflow.com"},
        {"Social and chat", "Bluesky (feeds, tags, people)", "identities", "services/bluesky.md",
         true, "bsky:bsky.app"},
        {nullptr, "Nostr (people, groups, projects)", "site-nostr", "services/nostr.md",
         true, nullptr},
        {nullptr, "Matrix homeservers", "matrix", "services/matrix.md", true,
         "matrix:directory/matrix.org"},
        {"Video", "YouTube (Google for Developers)", "media", "youtube:@GoogleDevelopers",
         false, nullptr},
        {nullptr, "media.ccc.de (talks)", "media", "mediaccc:", false, nullptr},
        {nullptr, "PeerTube instances", "media", "services/peertube.md", true,
         "peertube:video.blender.org"},
        {"Small net", "Gemini capsules", "gemini", "services/gemini.md", true,
         "gemini://geminiprotocol.net/"},
        {nullptr, "Gopher holes", "site-folder", "services/gopher.md", true,
         "gopher://gopher.floodgap.com/1/"},
        {nullptr, "Spartan hosts", "site-web", "services/spartan.md", true,
         "spartan://spartan.mozz.us/"},
        {nullptr, "Nex hosts", "site-web", "services/nex.md", true, "nex://nightfall.city/"},
        {nullptr, "Finger hosts", "identities", "finger.md", true, "finger://tilde.town/"},
        {nullptr, "Usenet servers", "context", "usenet.md", true, "news://news.gmane.io/"},
        {"Directories", "All services", "sitemap", "services.md", true, nullptr},
        {nullptr, "Directory (links and feeds)", "site-folder", "directory.md", true, nullptr},
        {nullptr, "Books and long reads", "reader", "books.md", true, nullptr},
        {nullptr, "Feeds (RSS / Atom)", "site-feed", "feeds.md", true, nullptr},
    };
    for (const Entry& e : kServices) {
      xanatoo::UiService s;
      s.label = e.label;
      s.icon = e.icon;
      s.target = e.target;
      s.example = e.example;
      if (e.group) s.group = e.group;
      s.tip = e.example ? std::string("examples/") + e.target : e.target;
      const char* probe = e.probe ? e.probe : e.example ? nullptr : e.target;
      std::string why;
      if (probe && !remote_fetch_supported(probe, &why)) {
        s.enabled = false;
        s.tip = why;
      }
      ui_state.services.push_back(std::move(s));
    }
  }
  if (const auto coll = current_collection(app); coll != xanatoo::INVALID_NODE) {
    ui_state.collection = true;
    auto it = app.collection_views.find(coll);
    const xanatoo::CollectionView v =
        it != app.collection_views.end()
            ? it->second
            : xanatoo::default_collection_view(
                  doc_of(app, focused_pane(app)).get(coll));
    ui_state.collection_layout = static_cast<int>(v.layout);
    ui_state.collection_sort = static_cast<int>(v.sort);
    ui_state.collection_thumb = v.thumb_px;
  }
  ui_state.list_fold = 0;
  if (const auto item = current_list_target(app);
      item != xanatoo::INVALID_NODE) {
    const auto& ld = doc_of(app, focused_pane(app));
    ui_state.list_fold =
        !xanatoo::list_item_foldable(ld, item) ? 1
        : xanatoo::list_item_folded(
              ld, item, &buf(app, focused_pane(app).buf).folds)
            ? 3
            : 2;
  }
  {
    // Two-body stretchtext on the page: looked up once per document state.
    Pane& fp = focused_pane(app);
    const auto& sd = doc_of(app, fp);
    const DocBuffer* sb = find_buf(app, fp.buf);
    const SummaryScan now{fp.buf, &sd, sb ? sb->doc_gen : 0, sd.nodes.size()};
    if (app.summary_scan != now) {
      app.summary_scan = now;
      app.has_summaries = xanatoo::has_two_body_stretch(sd, sd.root);
    }
    ui_state.has_summaries = app.has_summaries;
    ui_state.stretch_fold = 0;
    if (const auto id = current_stretch_target(app);
        id != xanatoo::INVALID_NODE)
      ui_state.stretch_fold =
          xanatoo::stretch_folded(sd, id, &buf(app, fp.buf).folds) ? 2 : 1;
  }
  ui_state.can_nav_forward = can_nav_step(app, 1);
  ui_state.vertical_tabs_open = app.vertical_tabs_open;
  ui_state.tab_bar_visible = app.tab_bar_visible;
  ui_state.history_open = app.history_panel_open;
  ui_state.recent_visited.clear();
  for (size_t i = app.visited.size(), n = 0; i > 0 && n < 25; --i, ++n)
    ui_state.recent_visited.push_back(app.visited[i - 1]);
  ui_state.downloads_open = app.downloads_panel_open;
  ui_state.media_open = app.media_panel_open;
  ui_state.history_menu = {};
  if (!app.history.menu_address.empty()) {
    ui_state.history_menu.address = app.history.menu_address;
    ui_state.history_menu.label = history_menu_label(app);
    ui_state.history_menu.from_db = app.history.menu_id != 0;
    ui_state.history_menu.rewinds = app.view_settings.history_rewinds;
  }
  ui_state.media_menu = {};
  if (app.media_menu.open && app.audio.state != AudioState::Idle) {
    auto& mm = ui_state.media_menu;
    mm.title = app.audio.title;
    mm.time = xanatoo::format_audio_time(app.media_menu.t);
    mm.time_bookmarked = !app.media_menu.mark.empty();
    mm.file_bookmarked = !app.audio.file_mark.empty();
    mm.seekable = app.audio.duration > 0 && app.audio.state != AudioState::Loading &&
                  app.audio.state != AudioState::Failed;
    mm.ab = app.audio.ab;
    mm.loop = app.audio.loop;
    mm.video = app.audio.video_w > 0;
    mm.fullscreen = media_fullscreen(app);
    mm.controls_hidden = app.media_controls_hidden;
    const MediaMenu& m = app.media_menu;
    if (m.sec_b > m.sec_a) {
      mm.section = xanatoo::format_audio_time(m.sec_a) + "\xE2\x80\x93" +
                   xanatoo::format_audio_time(m.sec_b);
      mm.section_bookmarked = !m.sec_mark.empty();
      mm.section_selected = app.audio.ab &&
                            xanatoo::media_range_address("", m.sec_a, m.sec_b) ==
                                xanatoo::media_range_address("", app.audio.ab_a, app.audio.ab_b);
    }
  }
  ui_state.download_menu = {};
  if (const auto& d = app.download_menu) {
    const auto phase = d->phase.load(std::memory_order_acquire);
    ui_state.download_menu.name = d->name;
    ui_state.download_menu.url = d->url;
    ui_state.download_menu.active = phase == xanatoo::DownloadPhase::Active;
    if (phase == xanatoo::DownloadPhase::Complete) {
      std::error_code ec;
      ui_state.download_menu.name = d->final_name;
      ui_state.download_menu.path = d->path;
      ui_state.download_menu.complete =
          !d->path.empty() && std::filesystem::exists(d->path, ec);
    }
  }
  ui_state.shaded_panels = 0;
  for (const FloatWindow* w : float_stack(app)) {
    const unsigned bit = xanatoo::ui_panel_bit(embedded_panel_layer(w->role));
    if (float_shaded(*w)) ui_state.shaded_panels |= bit;
    if (w->dock == Dock::Left) ui_state.left_docked_panels |= bit;
  }
  ui_state.styles_open = app.styles_panel_open;
  ui_state.styles_manager_open = app.styles_manager_panel_open;
  if (app.styles_panel_open) {
    auto [x, y, w, h] = imgui_panel_rect(app, app.styles_win);
    ui_state.styles_x = x;
    ui_state.styles_y = y;
    ui_state.styles_w = w;
    ui_state.styles_h = h;
  }
  if (app.styles_manager_panel_open) {
    auto [x, y, w, h] = imgui_panel_rect(app, app.styles_manager_win);
    ui_state.styles_manager_x = x;
    ui_state.styles_manager_y = y;
    ui_state.styles_manager_w = w;
    ui_state.styles_manager_h = h;
  }
  ui_state.bookmarks_open = app.bookmarks_panel_open;
  ui_state.context_open = app.context_panel_open;
  if (app.bookmarks_panel_open) {
    auto [x, y, w, h] = imgui_panel_rect(app, app.bookmarks_win);
    ui_state.bookmarks_x = x;
    ui_state.bookmarks_y = y;
    ui_state.bookmarks_w = w;
    ui_state.bookmarks_h = h;
  }
  if (app.context_panel_open) {
    auto [x, y, w, h] = imgui_panel_rect(app, app.context_win);
    ui_state.context_x = x;
    ui_state.context_y = y;
    ui_state.context_w = w;
    ui_state.context_h = h;
    refresh_context_panel(app);
    ui_state.context_has_doc = app.context_has_doc;
    ui_state.context_kind = app.context_kind;
    ui_state.context_title = app.context_title;
    ui_state.context_body = app.context_body;
    ui_state.context_meta = app.context_meta;
    ui_state.context_can_jump = app.context_can_jump;
    ui_state.context_jump_pane = app.context_jump_pane;
    ui_state.context_jump_link = app.context_jump_link;
    ui_state.context_jump_to_source = app.context_jump_to_source;
  }
  ui_state.bookmarks = app.bookmarks;
  ui_state.bookmark_folders = app.bookmark_folders;
  ui_state.subscriptions_open = app.subscriptions_panel_open;
  if (app.subscriptions_panel_open) {
    auto [x, y, w, h] = imgui_panel_rect(app, app.subscriptions_win);
    ui_state.subscriptions_x = x;
    ui_state.subscriptions_y = y;
    ui_state.subscriptions_w = w;
    ui_state.subscriptions_h = h;
  }
  ui_state.subscriptions.reserve(app.subscriptions.size());
  for (const auto& s : app.subscriptions)
    ui_state.subscriptions.push_back({s.address, s.title, s.error,
                                      s.interval_minutes, s.checked_at,
                                      s.unread, s.initialized, s.pending_job != 0});
  ui_state.subscription_address = subscription_source_address(
      pane_address(app, focused_pane(app)));
  ui_state.current_subscribable = subscribable_source(ui_state.subscription_address) &&
                                  remote_fetch_supported(ui_state.subscription_address, nullptr);
  ui_state.current_subscribed = std::any_of(app.subscriptions.begin(),
      app.subscriptions.end(), [&](const auto& s) {
        return s.address == ui_state.subscription_address;
      });
  ui_state.annotations_panel_open = app.annotation_panel_open;
  ui_state.revisions_panel_open = app.revision_panel_open;
  ui_state.nostr_explorer_open = app.nostr_explorer_open;
  ui_state.lemmy_explorer_open = app.lemmy_explorer_open;
  ui_state.lemmy_tab = app.lemmy_tab;
  ui_state.lemmy_search = app.lemmy_search;
  ui_state.lemmy_instance = app.lemmy_instance;
  ui_state.lemmy_sort = app.lemmy_sort;
  ui_state.lemmy_instance_only = app.lemmy_instance_only;
  ui_state.lemmy_supported = remote_fetch_supported("lemmy:lemmy.world/c/technology", nullptr);
  if (app.lemmy_explorer_open) {
    auto [x, y, w, h] = imgui_panel_rect(app, app.lemmy_explorer_win);
    ui_state.lemmy_explorer_x = x;
    ui_state.lemmy_explorer_y = y;
    ui_state.lemmy_explorer_w = w;
    ui_state.lemmy_explorer_h = h;
    ui_state.lemmy_entries = xanatoo::lemmy_directory_list();
  }
  ui_state.matrix_explorer_open = app.matrix_explorer_open;
  ui_state.matrix_tab = app.matrix_tab;
  ui_state.matrix_search = app.matrix_search;
  ui_state.matrix_server = app.matrix_server;
  ui_state.matrix_supported = remote_fetch_supported("matrix:directory/matrix.org", nullptr);
  if (app.matrix_explorer_open) {
    auto [x, y, w, h] = imgui_panel_rect(app, app.matrix_explorer_win);
    ui_state.matrix_explorer_x = x;
    ui_state.matrix_explorer_y = y;
    ui_state.matrix_explorer_w = w;
    ui_state.matrix_explorer_h = h;
    ui_state.matrix_entries = xanatoo::matrix_directory_list();
    // Credentials live in a file read on demand: check about once a second.
    const uint64_t now = xanatoo::sdl::ticks();
    if (app.matrix_token_checked == 0 || now - app.matrix_token_checked > 1000) {
      app.matrix_token_checked = now == 0 ? 1 : now;
      app.matrix_has_token =
          !xanatoo::effective_catalog_credential(xanatoo::CatalogKey::MatrixAccessToken).empty() &&
          !xanatoo::effective_catalog_credential(xanatoo::CatalogKey::MatrixHomeserver).empty();
    }
    ui_state.matrix_has_token = app.matrix_has_token;
  }
  ui_state.nostr_tab = app.nostr_tab;
  ui_state.nostr_search = app.nostr_search;
  ui_state.nostr_page = app.nostr_page;
  ui_state.nostr_people_more = app.nostr_people_more;
  ui_state.nostr_events_more = app.nostr_events_more;
  ui_state.nostr_groups_more = app.nostr_groups_more;
  ui_state.nostr_stats = app.nostr_stats;
  ui_state.nostr_people = app.nostr_people;
  ui_state.nostr_events = app.nostr_events;
  ui_state.nostr_groups = app.nostr_groups;
  ui_state.nostr_identities = app.nostr_identities;
  ui_state.nostr_selected_identity = app.nostr_active_identity;
  if (app.nostr_explorer_open) {
    auto [x, y, w, h] = imgui_panel_rect(app, app.nostr_explorer_win);
    ui_state.nostr_explorer_x = x;
    ui_state.nostr_explorer_y = y;
    ui_state.nostr_explorer_w = w;
    ui_state.nostr_explorer_h = h;
  }
  {
    const DocBuffer* b = find_buf(app, focused_pane(app).buf);
    ui_state.can_show_revisions = b && !!find_stored_document(b->doc_key);
  }
  if (app.annotation_panel_open) {
    auto [x, y, w, h] = imgui_panel_rect(app, app.annotation_win);
    ui_state.annotation_x = x;
    ui_state.annotation_y = y;
    ui_state.annotation_w = w;
    ui_state.annotation_h = h;
  }
  ui_state.annotation_note_created_gen = app.annotation_note_created_gen;
  ui_state.annotation_layers.clear();
  for (const auto& layer : app.annotation_layers)
    ui_state.annotation_layers.push_back({
        layer.id, layer.name, layer.owner,
        !app.hidden_annotation_layers.contains(layer.id),
        layer.id == app.active_annotation_layer, layer.annotations.size()});
  ui_state.annotations.clear();
  ui_state.annotation_history.clear();
  ui_state.annotation_history_layer.clear();
  if (app.annotation_panel_open) {
    Pane& pane = focused_pane(app);
    update_annotation_marks(app, pane);
    const DocBuffer* source = find_buf(app, pane.buf);
    std::string page = path_of(app, pane);
    if (page.empty()) page = app.gemini_current_url;
    const std::string key = source && !source->doc_key.empty()
                                ? source->doc_key : page;
    if (source && pane.source_buf != pane.buf && !key.empty()) {
      for (const auto& layer : app.annotation_layers) {
        if (app.hidden_annotation_layers.contains(layer.id)) continue;
        for (const auto& annotation : layer.annotations) {
          if (std::none_of(annotation.targets.begin(), annotation.targets.end(),
                           [&](const auto& target) {
                             return target.document_key == key ||
                                    target.document_key == page;
                           })) continue;
          const bool resolved = annotation_marks_fresh(pane) &&
              std::any_of(pane.annotation_marks.items.begin(),
                          pane.annotation_marks.items.end(),
                          [&](const AnnotationMark& mark) {
                            return mark.layer_id == layer.id &&
                                   mark.annotation_id == annotation.id;
                          });
          ui_state.annotations.push_back({
              annotation.id, layer.id, layer.owner, annotation.kind,
              annotation.body, resolved,
              annotation.id == app.selected_annotation_id});
        }
      }
    }
    const auto active = std::find_if(app.annotation_layers.begin(),
                                      app.annotation_layers.end(),
                                      [&](const auto& layer) {
      return layer.id == app.active_annotation_layer && layer.owner == "local";
    });
    if (active != app.annotation_layers.end()) {
      if (app.annotation_history_cached_layer != active->id ||
          app.annotation_history_cached_head != active->revision) {
        app.annotation_history_cached_layer = active->id;
        app.annotation_history_cached_head = active->revision;
        app.annotation_history_cache =
            xanatoo::annotation_layer_history(*active, 24);
      }
      ui_state.annotation_history_layer = active->id;
      for (const auto& revision : app.annotation_history_cache)
        ui_state.annotation_history.push_back({
            revision.id, revision.time, revision.annotation_count});
    }
  }
  ui_state.location_icon = address_icon(app, ui_state.doc_label);
  cert_ui_state(app, ui_state);
#if XANATOO_HTTP_COOKIES
  ui_state.cookies_open = app.cookies_open;
  if (app.cookies_open) {
    const auto& address = path_of(app, focused_pane(app));
    auto url = xanatoo::parse_http_url(address);
    if (!url) url = xanatoo::parse_http_url(xanatoo::page_web_url(address));
    if (app.cookie_page_url != address && app.cookie_site.empty())
      app.cookie_reveal = false;
    const uint64_t now = xanatoo::sdl::ticks();
    if (now >= app.cookie_refresh_at || app.cookie_page_url != address) {
      app.cookie_snapshot = xanatoo::http_cookie_snapshot();
      app.cookie_preview = url ? xanatoo::http_cookie_header(*url) : std::string();
      app.cookie_refresh_at = now + 500;
      app.cookie_page_url = address;
    }
    auto [x, y, w, h] = imgui_panel_rect(app, app.cookies_win);
    ui_state.cookies_x = x;
    ui_state.cookies_y = y;
    ui_state.cookies_w = w;
    ui_state.cookies_h = h;
    ui_state.cookie_snapshot = &app.cookie_snapshot;
    if (url) ui_state.cookie_current = xanatoo::cookie_origin(*url);
    ui_state.cookie_selected = app.cookie_site.empty() ?
        ui_state.cookie_current : app.cookie_site;
    ui_state.cookie_reveal = app.cookie_reveal;
    if (app.cookie_reveal && ui_state.cookie_selected == ui_state.cookie_current)
      ui_state.cookie_preview = app.cookie_preview;
    ui_state.cookie_error = app.cookie_error;
    ui_state.cookies_read_only = xanatoo::no_save();
  }
#endif
  identity_ui_state(app, ui_state);
  related_ui_state(app, ui_state);
  // Imported EPUBs are stored as content.md beside their extracted media.
  // Use the first image in that document as its cover when present.
  const std::string current = path_of(app, focused_pane(app));
  const std::string current_file = content_file(buf(app, focused_pane(app).buf));
  if (!ui_state.location_icon &&
      current_file.find("/epub-import/") != std::string::npos) {
    const auto& doc = doc_of(app, focused_pane(app));
    for (const auto& node : doc.nodes) {
      if (node.kind != xanatoo::NodeKind::Image || node.src.empty()) continue;
      const auto& cover = app.images.thumb(
          node.src, fs::path(current_file).parent_path().string(), 128);
      if (cover.ok) ui_state.location_icon = cover.texture;
      break;
    }
  }
  ui_state.location_pseudo_icon.clear();
  ui_state.location_engine.clear();
  if (!ui_state.location_icon) {
    const std::string base = xanatoo::parse_locator(ui_state.doc_label).base;
    bool is_dir = false;
    if (!current.empty() && !base.empty() &&
        base.find("://") == std::string::npos) {
      std::error_code ec;
      is_dir = fs::is_directory(current, ec);
    }
    ui_state.location_pseudo_icon = xanatoo::pseudo_favicon(base, is_dir);
    if (ui_state.location_pseudo_icon == "search")
      if (auto ref = xanatoo::parse_search_ref(base))
        ui_state.location_engine = ref->engine;
  }
  ui_state.bookmark_icons.clear();
  for (const auto& bookmark : app.bookmarks)
    ui_state.bookmark_icons.push_back(
        xanatoo::parse_locator(bookmark.address).base == current && ui_state.location_icon
            ? ui_state.location_icon : address_icon(app, bookmark.address));
  {
    // With a selection the star bookmarks the selection.
    ui_state.current_bookmarked =
        focused_pane(app).sel.empty() &&
        (is_bookmarked(app, pane_address(app, focused_pane(app))) ||
         is_bookmarked(app, focused_canonical(app)));
    ui_state.page_bookmarked =
        is_bookmarked(app, pane_address(app, focused_pane(app))) ||
        is_bookmarked(app, focused_canonical(app));
    ui_state.page_bookmarks = bookmarks_in_page(app, focused_pane(app));
    ui_state.bookmark_menu = {};
    if (const xanatoo::Bookmark* b = find_bookmark(app, app.bookmark_menu)) {
      ui_state.bookmark_menu.address = b->address;
      ui_state.bookmark_menu.label = xanatoo::bookmark_label(*b);
      ui_state.bookmark_menu.cut = cut_address(b->address);
      ui_state.bookmark_menu.folder = b->folder;
    }
  }
  ui_state.tab_labels.clear();
  ui_state.tab_ids.clear();
  ui_state.tab_roots.clear();
  ui_state.tab_icons.clear();
  ui_state.tab_editable.clear();
  ui_state.tab_service_color.clear();
  ui_state.active_tab = 0;
  for (int ti = 0; ti < static_cast<int>(app.tab_order.size()); ++ti) {
    const BufferId bi = app.tab_order[static_cast<size_t>(ti)];
    ui_state.tab_labels.push_back(buffer_tab_label(app, bi));
    ui_state.tab_ids.push_back(bi);
    ui_state.tab_roots.push_back(tab_root(app, bi));
    const DocBuffer* tab_buffer = find_buf(app, bi);
    ui_state.tab_icons.push_back(tab_buffer ? address_icon(app, tab_buffer->path) : nullptr);
    ui_state.tab_editable.push_back(buffer_takes_edits(app, bi) ? 1 : 0);
    ui_state.tab_service_color.push_back(
        tab_buffer ? service_tab_color(service_address(app, tab_buffer->path)) : 0);
    if (bi == app.panes[app.centre_focus].buf) ui_state.active_tab = ti;
  }
  ui_state.urlbar_entries.clear();
  for (int ti = 0; ti < static_cast<int>(app.tab_order.size()); ++ti) {
    if (const DocBuffer* b = find_buf(app, app.tab_order[static_cast<size_t>(ti)]);
        b && !b->path.empty())
      ui_state.urlbar_entries.push_back(
          {b->path, ui_state.tab_labels[static_cast<size_t>(ti)],
           xanatoo::UrlBarSource::Tab, ti});
  }
  for (auto it = app.bookmarks.rbegin(); it != app.bookmarks.rend(); ++it)
    ui_state.urlbar_entries.push_back(
        {it->address, xanatoo::bookmark_label(*it),
         xanatoo::UrlBarSource::Bookmark});
  for (auto it = app.minibuf_hist_url.rbegin();
       it != app.minibuf_hist_url.rend(); ++it)
    ui_state.urlbar_entries.push_back(
        {*it, {}, xanatoo::UrlBarSource::History});
  ui_state.has_floating = std::any_of(
      app.soft_wins.begin(), app.soft_wins.end(),
      [](const FloatWindow& w) { return w.role == WindowRole::Document; });
  // Right-stick software cursor (extra pointer; never touches OS mouse).
  bool stick_active = false;
  if (app.gamepad) {
    auto axis_e = [&](xanatoo::sdl::GamepadAxis a) -> float {
      Sint16 v = xanatoo::sdl::gamepad_axis(app.gamepad, a);
      const float dead = 8000.f;
      if (v > -dead && v < dead) return 0.f;
      return static_cast<float>(v) / 32767.f;
    };
    if (!app.stick_cursor_init) {
      app.stick_cx = static_cast<float>(app.win_w) * 0.5f;
      app.stick_cy = static_cast<float>(app.win_h) * 0.5f;
      app.stick_cursor_init = true;
    }
    app.stick_cursor_on = true;
    float rx = axis_e(xanatoo::sdl::AxisRightX);
    float ry = axis_e(xanatoo::sdl::AxisRightY);
    if (rx != 0.f || ry != 0.f) {
      // Non-linear response: fine aim near centre, faster at full tilt.
      auto curve = [](float v) {
        float s = (v < 0.f) ? -1.f : 1.f;
        float a = std::fabs(v);
        return s * a * a;  // quadratic
      };
      rx = curve(rx);
      ry = curve(ry);
      const float speed = 11.f + 0.014f * static_cast<float>(app.win_w);
      app.stick_cx += rx * speed;
      app.stick_cy += ry * speed;
      if (app.stick_cx < 0.f) app.stick_cx = 0.f;
      if (app.stick_cy < 0.f) app.stick_cy = 0.f;
      if (app.stick_cx > static_cast<float>(app.win_w - 1))
        app.stick_cx = static_cast<float>(app.win_w - 1);
      if (app.stick_cy > static_cast<float>(app.win_h - 1))
        app.stick_cy = static_cast<float>(app.win_h - 1);
      stick_active = true;
    }
    if (app.stick_lmb || app.stick_rmb) stick_active = true;
    if (stick_active) note_stick_use(app);
    if (stick_active) {
      const int ix = static_cast<int>(app.stick_cx);
      const int iy = static_cast<int>(app.stick_cy);
      static int last_ix = -1, last_iy = -1;
      if (ix != last_ix || iy != last_iy || app.stick_lmb || app.stick_rmb) {
        last_ix = ix;
        last_iy = iy;
        // Synthetic events at stick position — OS cursor stays put.
        xanatoo::sdl::push_mouse_motion(app.window, ix, iy, app.stick_lmb,
                                        app.stick_rmb);
      }
    }
  }
  ui_state.stick_cursor = app.stick_cursor_on;
  ui_state.stick_active = stick_active;
  ui_state.stick_cx = app.stick_cx;
  ui_state.stick_cy = app.stick_cy;
  ui_state.stick_lmb = app.stick_lmb;
  ui_state.stick_rmb = app.stick_rmb;
}

// Commands that edit the focused document or replace its buffer; browse
// mode (foreign or read-only content) refuses them. Everything else (views,
// panels, navigation, toggles) runs in either mode, so a new command is
// only listed here when it changes the document.
static bool changes_document(xanatoo::UiCommand cmd) {
  using C = xanatoo::UiCommand;
  switch (cmd) {
    case C::RevertToSaved:
    case C::Undo: case C::Redo: case C::Cut: case C::Paste:
    case C::Replace: case C::ReplaceOne: case C::ReplaceAll:
    case C::VirtualKey:
    case C::Bold: case C::Italic: case C::Code: case C::Strike:
    case C::Cite: case C::Link:
    case C::Heading1: case C::Heading2: case C::Heading3:
    case C::Heading4: case C::Heading5: case C::Heading6:
    case C::MakeHeader: case C::ParagraphLevel:
    case C::ToggleBlockQuote: case C::ToggleOrderedList:
    case C::ToggleUnorderedList: case C::ToggleTask:
    case C::IndentList: case C::OutdentList:
    case C::InsertParagraph: case C::InsertHr: case C::InsertImage:
    case C::InsertFigure: case C::InsertTable: case C::InsertColumns:
    case C::InsertDefList: case C::InsertCodeBlock: case C::InsertNote:
    case C::InsertTransclude: case C::InsertBiblioEntry:
    case C::DuplicateBlock: case C::MoveBlockUp: case C::MoveBlockDown:
    case C::MoveSectionUp: case C::MoveSectionDown:
    case C::PromoteSection: case C::DemoteSection:
    case C::WrapSection: case C::WrapArticle: case C::UnwrapSectioning:
    case C::WrapStretch: case C::UnwrapStretch: case C::EditStretchSummary:
      return true;
    default:
      return false;
  }
}

// Apply one toolbar / menu command to the world. `arg` is the optional
// payload (URL, tab index, virtual key, example name).
namespace {

// The start of a message's own text (not its header or nested replies),
// whitespace folded, cut to about two lines for the Reply panel.
std::string message_excerpt(const xanatoo::Document& d, xanatoo::NodeId msg) {
  std::string out;
  for (xanatoo::NodeId c : d.get(msg).children) {
    if (d.get(c).kind == xanatoo::NodeKind::Message) continue;
    for (char ch : xanatoo::plain_text(d, c)) {
      const bool space = ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r';
      if (space) {
        if (!out.empty() && out.back() != ' ') out.push_back(' ');
      } else {
        out.push_back(ch);
      }
    }
    if (!out.empty() && out.back() != ' ') out.push_back(' ');
    if (out.size() > 200) break;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  if (out.size() > 160) {
    out.resize(160);
    while (!out.empty() &&
           (static_cast<unsigned char>(out.back()) & 0xc0) == 0x80)
      out.pop_back();
    if (!out.empty() && static_cast<unsigned char>(out.back()) >= 0xc0)
      out.pop_back();  // a lead byte whose continuation was cut
    out += "…";
  }
  return out;
}

}  // namespace

void execute_command(App& app, xanatoo::UiCommand cmd, const std::string& arg) {
  using C = xanatoo::UiCommand;
  auto& p = focused_pane(app);
  auto& d = doc_of(app, p);

  // Browse mode: ignore commands that change the document.
  if (browsing(app) && changes_document(cmd)) {
    flash_status(app, no_edit_notice(app));
    cmd = C::None;
  }

  switch (cmd) {
    case C::FindText:
      find_set_texts(app, arg);
      break;
    case C::FindNext:
    case C::FindPrev:
      find_set_texts(app, arg);
      find_step(app, cmd == C::FindPrev);
      break;
    case C::FindClose:
      close_prompt(app);
      app.ui.release_input_capture();
      app.key_focus = KeyFocus::Document;
      reset_blink(app);
      break;
    case C::ReplaceOne:
      find_set_texts(app, arg);
      find_replace_one(app);
      break;
    case C::JumpQuote: {
      const size_t k = static_cast<size_t>(std::atoi(arg.c_str()));
      if (app.context_quote_buf != p.buf || k >= app.context_quotes.size())
        break;
      const auto item = app.context_quotes[k];
      jump_quote(app, app.focus, item.link, item.to_source);
      break;
    }
    case C::ContextJump:
      if (app.context_can_jump && app.context_jump_pane >= 0 &&
          app.context_jump_link >= 0)
        jump_quote(app, app.context_jump_pane,
                   static_cast<size_t>(app.context_jump_link),
                   app.context_jump_to_source);
      break;
    case C::OpenReply: {
      const xanatoo::NodeId id = app.context_reply_node;
      if (app.context_reply_buffer != p.buf ||
          id == xanatoo::INVALID_NODE || id >= d.next_id ||
          d.get(id).kind != xanatoo::NodeKind::Message) break;
      const auto& message = d.get(id);
      app.reply.open = true;
      focus_panel(app, WindowRole::Reply);
      app.reply.buffer = p.buf;
      app.reply.node = id;
      app.reply.page = path_of(app, p);
      app.reply.label = message.label;
      app.reply.author = message.src;
      app.reply.excerpt = message_excerpt(d, id);
      app.reply.text.clear();
      if (!app.nostr_reply.pending) app.nostr_reply.status.clear();
      app.reply.anchor_y = app.context_reply_y;
      ++app.reply.generation;
      app.reply_win.scroll = 0.f;
      snap_soft_geom(app, app.reply_win.x, app.reply_win.y,
                     app.reply_win.w, app.reply_win.h);
      place_windows(app);
      break;
    }
    case C::ShowReplyTarget:
      if (app.reply.open && !app.reply.label.empty()) {
        int target = -1;
        for (int vi : live_views(app))
          if (app.panes[vi].buf == app.reply.buffer &&
              (target < 0 || vi == app.focus))
            target = vi;
        Pane* p = target >= 0 ? &app.panes[target] : nullptr;
        auto dest = p ? xanatoo::jump_to_href(doc_of(app, *p), "#id:" + app.reply.label)
                      : std::nullopt;
        if (dest) {
          reveal_hit(app, *p, dest->node);
          p->sel = xanatoo::Selection{*dest, *dest};
          scroll_caret_into_view(app, *p);
        } else {
          flash_status(app, "The message is not on a page shown now");
        }
      }
      break;
    case C::EditReply:
      if (app.reply.open) app.reply.text = arg;
      break;
    case C::SendReply:
      if (app.reply.open) {
        if (!arg.empty()) app.reply.text = arg;
        send_reply(app);
      }
      break;
    case C::Quit:
      if (stash_all_or_confirm(app)) app.running = false;
      break;
    case C::NewDocument:
    case C::NewTab: {
      // Both open a blank, editable document in a new tab, never over the
      // page in view. New Tab leaves it to the address bar (type a URL, or
      // click into the page and write); New Document puts it in Local
      // documents at once and selects the title to type over.
      const bool document = cmd == C::NewDocument;
      const BufferId bi =
          add_buffer(app, xanatoo::parse_document("# Untitled\n", {}), {});
      app.tab_order.push_back(bi);
      Pane& np = app.panes[app.centre_focus];
      if (app.focus != app.centre_focus) focus_view(app, app.centre_focus);
      select_view_tab(app, app.centre_focus, bi);
      np.sel = {};
      np.scroll_y = 0.f;
      np.layout = {};
      xanatoo::Document& nd = buf(app, bi).doc;
      xanatoo::ensure_valid_caret(nd, np.sel.focus);
      np.sel.anchor = np.sel.focus;
      if (document && np.sel.focus.node != xanatoo::INVALID_NODE)
        xanatoo::select_node(nd, np.sel, np.sel.focus.node);
      buf(app, bi).history.reset(nd, np.sel);
      app.needs_layout = true;
      if (document) {
        const bool stored = save_stored_buffer(app, bi);
        app.key_focus = KeyFocus::Document;
        flash_status(app, stored ? "New local document — type its title"
                                 : "Local database unavailable; document "
                                   "remains in memory");
      } else {
        app.ui.focus_url_bar();
        flash_status(app, "New tab — type an address, or write here");
      }
      update_title(app);
      break;
    }
    case C::OpenDocument:
      open_prompt(app, PromptKind::OpenFile);
      break;
    case C::OpenUrlBar:
      leave_prompt(app);
      app.ui.focus_url_bar();
      break;
    case C::CollectionView:
      collection_command(app, arg);
      break;
    case C::ListFold: {
      const xanatoo::NodeId item = current_list_target(app);
      if (item == xanatoo::INVALID_NODE) break;
      if (arg == "toggle")
        toggle_fold(app, app.focus, item);
      else
        set_list_folds(app, app.focus, item, arg == "fold-all");
      break;
    }
    case C::StretchFold:
      if (arg == "toggle") {
        const xanatoo::NodeId id = current_stretch_target(app);
        if (id != xanatoo::INVALID_NODE) toggle_fold(app, app.focus, id);
      } else {
        set_stretch_folds(app, app.focus, arg == "summaries");
      }
      break;
    case C::ToggleNative: {
      // Explicit only: web pages never switch to the native form on their
      // own, and native refs are never rerouted from https:// URLs.
      const std::string other = xanatoo::alternate_view_url(path_of(app, p));
      if (other.empty())
        flash_status(app, "No native view for this page");
      else
        navigate_to_url(app, other);
      break;
    }
    case C::ToggleArchive: {
      const std::string& cur = path_of(app, p);
      if (auto snap = xanatoo::wayback_ref_from_snapshot_url(cur))
        navigate_to_url(app, snap->url);
      else if (std::string ref = xanatoo::wayback_lookup_ref(cur); !ref.empty())
        navigate_to_url(app, ref);
      else
        flash_status(app, "No archived version for this page");
      break;
    }
    case C::OpenPageFeed:
      open_page_feed(app, arg);
      break;
    case C::ToggleReader:
      if (p.chrome_parts == 0 && !pane_reader(app, p))
        flash_status(app, "Reader mode: this page has no page chrome to hide");
      else
        set_reader(app, p, !pane_reader(app, p));
      break;
    case C::TogglePaged:
      set_paged(app, p, !pane_paged(app, p));
      break;
    case C::TogglePagedDefault: {
      keep_views(app);
      app.view_settings.paged = !app.view_settings.paged;
      xanatoo::save_view_settings(app.view_settings);
      for (Pane& v : app.panes)
        if (!v.paged) v.height_cache.clear();
      app.needs_layout = true;
      flash_status(app, app.view_settings.paged
                            ? "Paged reading is the default"
                            : "Scrolling is the default");
      break;
    }
    case C::PageColumns: {
      keep_views(app);
      app.view_settings.page_columns = std::clamp(std::atoi(arg.c_str()), 0, 3);
      xanatoo::save_view_settings(app.view_settings);
      app.needs_layout = true;
      break;
    }
    case C::NavigateUrl:
      xanatoo::platform_release_keyboard(app.window);  // URL typed; page is read-only
      if (!arg.empty()) {
        app.visit_hint = find_bookmark(app, arg) ? xanatoo::VisitKind::Bookmark
                                                 : xanatoo::VisitKind::Address;
        navigate_to_url(app, complete_location(app, arg));
        app.visit_hint.reset();
      }
      break;
    case C::NavigateHistoryUrl:  // History menu: an address visited before
      if (!arg.empty()) {
        app.visit_hint = xanatoo::VisitKind::History;
        navigate_to_url(app, arg);
        app.visit_hint.reset();
      }
      break;
    case C::StopLoading:
      if (cancel_fetches(app)) flash_status(app, "Stopped loading");
      break;
    case C::NavBack:
      if (!nav_step(app, -1)) flash_status(app, "No previous page");
      break;
    case C::NavForward:
      if (!nav_step(app, 1)) flash_status(app, "No next page");
      break;
    case C::NavJump: {
      const int steps = std::clamp(std::atoi(arg.c_str()), -kMaxNavHistory,
                                   kMaxNavHistory);
      if (steps == 0) break;
      const bool ok = split_browsing(app) && app.focus < kCentreViews
          ? split_history_jump(app, steps) : nav_step(app, steps);
      if (!ok) flash_status(app, "History destination no longer available");
      break;
    }
    case C::GoHome: {
      const std::string home = home_page(app);
      if (home.empty())
        flash_status(app, "Home page not found (welcome.md)");
      else
        navigate_to_url(app, home);
      break;
    }
    case C::GoUp: {
      const std::string up = page_up_url(app);
      if (up.empty())
        flash_status(app, "Already at the top");
      else
        navigate_to_url(app, up);
      break;
    }
    case C::RestoreClosedTab:
      if (restore_closed_tab(app)) {
        if (prompt_active(app)) close_prompt(app);
        app.ui.release_input_capture();
        app.key_focus = KeyFocus::Document;
        reset_blink(app);
        flash_status(app, "Tab restored");
      } else {
        flash_status(app, "No closed tabs to restore");
      }
      break;
    case C::CloseTab: {
      int tab_i = -1;
      if (!arg.empty()) {
        try {
          tab_i = std::stoi(arg);
        } catch (...) {
          tab_i = -1;
        }
      }
      if (tab_i < 0 || tab_i >= static_cast<int>(app.tab_order.size())) {
        // Close focused pane's buffer tab
        for (int ti = 0; ti < static_cast<int>(app.tab_order.size()); ++ti) {
          if (app.tab_order[static_cast<size_t>(ti)] ==
              app.panes[app.centre_focus].buf) {
            tab_i = ti;
            break;
          }
        }
      }
      if (tab_i < 0 || app.tab_order.empty()) break;
      const BufferId bi = app.tab_order[static_cast<size_t>(tab_i)];
      if (!stash_or_confirm(app, bi)) break;
      close_tab(app, bi);
      flash_status(app, "Tab closed");
      break;
    }
    case C::CloseOtherTabs:
    case C::CloseTabsRight:
    case C::CloseTabsLeft: {
      size_t target;
      try {
        target = std::stoul(arg);
      } catch (...) {
        break;
      }
      if (target >= app.tab_order.size()) break;
      std::vector<BufferId> closing;
      for (size_t i = 0; i < app.tab_order.size(); ++i)
        if ((cmd == C::CloseOtherTabs && i != target) ||
            (cmd == C::CloseTabsRight && i > target) ||
            (cmd == C::CloseTabsLeft && i < target))
          closing.push_back(app.tab_order[i]);
      for (BufferId id : closing) {
        if (!stash_or_confirm(app, id)) break;
        close_tab(app, id);
      }
      break;
    }
    case C::SortTabsByTitle: {
      auto title = [&](BufferId id) {
        std::string label = buffer_tab_label(app, id);
        if (const DocBuffer* b = find_buf(app, id); b && b->dirty)
          label.erase(0, 1);
        return xanatoo::ascii_lower(label);
      };
      std::stable_sort(app.tab_order.begin(), app.tab_order.end(),
                       [&](BufferId a, BufferId b) { return title(a) < title(b); });
      break;
    }
    case C::SelectTab: {
      int tab_i = 0;
      try {
        tab_i = std::stoi(arg);
      } catch (...) {
        break;
      }
      if (tab_i < 0 || tab_i >= static_cast<int>(app.tab_order.size())) break;
      const BufferId bi = app.tab_order[static_cast<size_t>(tab_i)];
      focus_view(app, app.centre_focus);  // tabs are the centre's buffers
      select_view_tab(app, app.centre_focus, bi);
      break;
    }
    case C::OpenBlock:
    case C::OpenBlockInWindow:
      open_context_block(app, static_cast<size_t>(std::atoi(arg.c_str())),
                         cmd == C::OpenBlockInWindow);
      break;
    case C::CopyBlockAddress:
      if (const auto i = std::optional<size_t>(
              static_cast<size_t>(std::atoi(arg.c_str())));
          *i < app.context_blocks.size() &&
          app.context_blocks_pane >= 0 &&
          app.panes[app.context_blocks_pane].buf == app.context_blocks_buf) {
        const std::string addr =
            block_address(app, app.panes[app.context_blocks_pane],
                          app.context_blocks[*i]);
        SDL_SetClipboardText(addr.c_str());
        flash_status(app, "Copied " + addr);
      }
      break;
    case C::NextTab:
    case C::PrevTab:
      if (!app.tab_order.empty()) {
        int cur = 0;
        for (int ti = 0; ti < static_cast<int>(app.tab_order.size()); ++ti)
          if (app.tab_order[static_cast<size_t>(ti)] ==
              app.panes[app.centre_focus].buf) {
            cur = ti;
            break;
          }
        const int ntabs = static_cast<int>(app.tab_order.size());
        if (cmd == C::NextTab)
          cur = (cur + 1) % ntabs;
        else
          cur = (cur - 1 + ntabs) % ntabs;
        focus_view(app, app.centre_focus);
        select_view_tab(app, app.centre_focus,
                        app.tab_order[static_cast<size_t>(cur)]);
      }
      break;
    case C::ReorderTabs: {
      std::vector<BufferId> order;
      size_t at = 0;
      while (at <= arg.size()) {
        size_t comma = arg.find(',', at);
        if (comma == std::string::npos) comma = arg.size();
        try {
          order.push_back(static_cast<BufferId>(
              std::stoul(arg.substr(at, comma - at))));
        } catch (...) {
          order.clear();
          break;
        }
        at = comma + 1;
      }
      auto sorted = [](std::vector<BufferId> v) {
        std::sort(v.begin(), v.end());
        return v;
      };
      if (!order.empty() && sorted(order) == sorted(app.tab_order))
        app.tab_order = order;
      break;
    }
    case C::TearTab: {
      unsigned long id = 0;
      int mx = 0, my = 0;
      if (std::sscanf(arg.c_str(), "%lu,%d,%d", &id, &mx, &my) != 3) break;
      app.dock_preview = Dock::None;
      tear_tab(app, static_cast<BufferId>(id), mx, my);
      break;
    }
    case C::PopOut:
      pop_out_focused(app);
      break;
    case C::PopIn: {
      // A window's context menu names its buffer explicitly. File → Move
      // Window to Tab uses the focused document window, or the topmost
      // document window; never accidentally convert a utility panel.
      BufferId requested = kNoBuffer;
      if (!arg.empty()) {
        try { requested = static_cast<BufferId>(std::stoul(arg)); }
        catch (...) { break; }
      }
      size_t target = app.soft_wins.size();
      for (size_t i = app.soft_wins.size(); i-- > 0;) {
        const FloatWindow& w = app.soft_wins[i];
        if (w.role != WindowRole::Document ||
            (requested != kNoBuffer && app.panes[w.view].buf != requested))
          continue;
        if (requested != kNoBuffer || w.view == app.focus) {
          target = i;
          break;
        }
        if (target == app.soft_wins.size()) target = i;
      }
      if (target != app.soft_wins.size()) pop_in_soft(app, target);
      else flash_status(app, "No document window to move to a tab");
      break;
    }
    case C::OpenDrafts:
      navigate_to_url(app, kDraftsPage);
      break;
    case C::OpenStoredDocuments:
      navigate_to_url(app, kDocumentsPage);
      break;
    case C::OpenAbout:
      navigate_to_url(app, kAboutPage);
      break;
    case C::ToggleTabBar:
      keep_views(app);
      app.tab_bar_visible = !app.tab_bar_visible;
      break;
    case C::ToggleVerticalTabs:
      app.vertical_tabs_open = !app.vertical_tabs_open;
      if (app.vertical_tabs_open)
        focus_panel(app, WindowRole::VerticalTabs);
      else if (float_active(app, app.vertical_tabs_win))
        app.key_focus = KeyFocus::Document;
      break;
    case C::ToggleHistory:
      app.history_panel_open = !app.history_panel_open;
      if (app.history_panel_open) {
        app.key_focus = KeyFocus::History;
      } else if (app.key_focus == KeyFocus::History) {
        app.key_focus = KeyFocus::Document;
      }
      break;
    case C::ToggleMedia:
      toggle_media_panel(app);
      break;
    case C::ToggleDownloads:
      app.downloads_panel_open = !app.downloads_panel_open;
      if (app.downloads_panel_open) focus_panel(app, WindowRole::Downloads);
      break;
    case C::ToggleStylesPanel:
      app.styles_panel_open = !app.styles_panel_open;
      if (app.styles_panel_open) {
        app.key_focus = KeyFocus::Styles;
      } else if (app.key_focus == KeyFocus::Styles) {
        app.key_focus = KeyFocus::Document;
      }
      break;
    case C::ToggleStyleManagerPanel:
      app.styles_manager_panel_open = !app.styles_manager_panel_open;
      if (app.styles_manager_panel_open) {
        app.key_focus = KeyFocus::StyleManager;
      } else if (app.key_focus == KeyFocus::StyleManager) {
        app.key_focus = KeyFocus::Document;
      }
      break;
    case C::ToggleBookmarks:
      app.bookmarks_panel_open = !app.bookmarks_panel_open;
      if (app.bookmarks_panel_open) focus_panel(app, WindowRole::Bookmarks);
      break;
    case C::ToggleContextPanel:
      app.context_panel_open = !app.context_panel_open;
      if (app.context_panel_open) {
        focus_float(app, app.context_win);
      } else {
        app.context_pending_url.clear();
        if (app.key_focus == KeyFocus::Context)
          app.key_focus = KeyFocus::Document;
      }
      break;
    case C::ToggleSubscriptions:
      app.subscriptions_panel_open = !app.subscriptions_panel_open;
      if (app.subscriptions_panel_open) focus_panel(app, WindowRole::Subscriptions);
      break;
    case C::Subscribe:
      add_subscription(app, arg.empty() ? parse_locator(pane_address(app, p)).base : arg);
      break;
    case C::Unsubscribe:
      remove_subscription(app, arg);
      break;
    case C::SubscriptionOpen:
      open_subscription(app, arg);
      break;
    case C::SubscriptionOpenInTab:
      if (!arg.empty()) {
        mark_subscription_read(app, arg);
        open_link_in_tab(app, app.focus, arg);
      }
      break;
    case C::SubscriptionMarkRead:
      mark_subscription_read(app, arg);
      break;
    case C::SubscriptionRefresh:
      request_subscription_refresh(app, arg);
      break;
    case C::SubscriptionInterval: {
      const size_t newline = arg.find('\n');
      if (newline != std::string::npos) {
        const std::string minutes = arg.substr(newline + 1);
        try {
          set_subscription_interval(app, arg.substr(0, newline), std::stoi(minutes));
        } catch (const std::exception&) {}  // malformed command, no change
      }
      break;
    }
    case C::ToggleAnnotationsPanel:
      app.annotation_panel_open = !app.annotation_panel_open;
      if (app.annotation_panel_open) focus_panel(app, WindowRole::Annotations);
      break;
    case C::ToggleRevisionsPanel:
      if (app.revision_panel_open) {
        app.revision_panel_open = false;
      } else {
        const DocBuffer* b = find_buf(app, p.buf);
        if (!b || !find_stored_document(b->doc_key)) {
          flash_status(app, "Open a local database document to view its revisions");
          break;
        }
        app.revision_selected.clear();
        app.revision_working.clear();
        app.revision_published.clear();
        app.revision_win.scroll = 0.f;
        app.revision_panel_open = true;
        focus_panel(app, WindowRole::Revisions);
      }
      break;
    case C::ToggleNostrExplorer:
      app.nostr_explorer_open = !app.nostr_explorer_open;
      if (app.nostr_explorer_open) {
        refresh_nostr_catalogue(app);
        focus_panel(app, WindowRole::NostrExplorer);
      }
      break;
    case C::ToggleLemmyExplorer:
      app.lemmy_explorer_open = !app.lemmy_explorer_open;
      if (app.lemmy_explorer_open) focus_panel(app, WindowRole::LemmyExplorer);
      break;
    case C::LemmySetTab:
      if (arg.size() == 1 && arg[0] >= '0' && arg[0] <= '3')
        app.lemmy_tab = arg[0] - '0';
      break;
    case C::LemmySetSearch:
      app.lemmy_search = arg.substr(0, 200);
      break;
    case C::LemmySetInstance: {
      const std::string host = xanatoo::lemmy_instance_host(arg);
      if (!host.empty()) app.lemmy_instance = host;
      break;
    }
    case C::LemmyAddInstance: {
      const std::string host = xanatoo::lemmy_instance_host(arg);
      if (host.empty()) {
        flash_status(app, "Enter a valid Lemmy instance hostname");
      } else {
        xanatoo::lemmy_directory_add_instance(host);
        app.lemmy_instance = host;
        flash_status(app, "Added Lemmy instance " + host);
      }
      break;
    }
    case C::LemmySetSort:
      if (arg == "Hot" || arg == "New" || arg == "Active" ||
          arg == "TopDay" || arg == "TopWeek")
        app.lemmy_sort = arg;
      break;
    case C::LemmyToggleInstanceOnly:
      app.lemmy_instance_only = !app.lemmy_instance_only;
      break;
    case C::ToggleMatrixExplorer:
      app.matrix_explorer_open = !app.matrix_explorer_open;
      app.matrix_token_checked = 0;
      if (app.matrix_explorer_open) focus_panel(app, WindowRole::MatrixExplorer);
      break;
    case C::MatrixSetTab:
      if (arg.size() == 1 && arg[0] >= '0' && arg[0] <= '3')
        app.matrix_tab = arg[0] - '0';
      break;
    case C::MatrixSetSearch:
      app.matrix_search = arg.substr(0, 200);
      break;
    case C::MatrixSetServer:
    case C::MatrixAddServer: {
      std::string_view in = arg;
      xanatoo::trim_ascii(in);
      if (xanatoo::starts_with_ci(in, "https://")) in.remove_prefix(8);
      const std::string server = xanatoo::matrix_server_name(in.substr(0, in.find('/')));
      if (server.empty()) {
        flash_status(app, "Enter a valid Matrix server name");
      } else {
        if (cmd == C::MatrixAddServer) {
          xanatoo::matrix_directory_add_server(server);
          flash_status(app, "Added Matrix server " + server);
        }
        app.matrix_server = server;
      }
      break;
    }
    case C::ToggleCookies:
    case C::CookieSelect:
    case C::CookieSiteToggle:
    case C::CookieToggle:
    case C::CookieDelete:
    case C::CookieReveal:
#if XANATOO_HTTP_COOKIES
      if (cmd == C::ToggleCookies) {
        app.cookies_open = !app.cookies_open;
        app.cookie_site.clear();
        app.cookie_reveal = false;
        app.cookie_refresh_at = 0;
        if (app.cookies_open) focus_panel(app, WindowRole::Cookies);
      } else if (cmd == C::CookieSelect) {
        app.cookie_site = arg;
        app.cookie_reveal = false;
      } else if (cmd == C::CookieReveal) {
        app.cookie_reveal = !app.cookie_reveal;
      } else {
        bool ok = false;
        if (cmd == C::CookieSiteToggle) {
          const auto site = std::ranges::find(app.cookie_snapshot.sites, arg,
                                              &xanatoo::CookieSite::origin);
          ok = xanatoo::set_cookie_site(arg, site == app.cookie_snapshot.sites.end() ||
                                            !site->enabled);
        } else {
          size_t index = 0;
          const auto [end, ec] = std::from_chars(arg.data(), arg.data() + arg.size(), index);
          if (ec == std::errc{} && end == arg.data() + arg.size() &&
              index < app.cookie_snapshot.cookies.size()) {
            const auto& cookie = app.cookie_snapshot.cookies[index];
            ok = xanatoo::edit_http_cookie(cookie, cmd == C::CookieDelete, !cookie.enabled);
          }
        }
        app.cookie_refresh_at = 0;
        app.cookie_error = ok ? "" : "Could not update the cookie store.";
      }
#endif
      break;
    case C::ToggleCertificates:
      // From the address bar's site icon (arg = its server): show that row;
      // a second click closes the panel again, like a browser's popover.
      if (app.certs.open && (arg.empty() || arg == app.certs.selected))
        app.certs.open = false;
      else
        show_certificates(app, arg);
      break;
    case C::CertSelect:
      app.certs.selected = arg;
      break;
    case C::ToggleIdentities:
      if (app.identities.open)
        app.identities.open = false;
      else
        show_identities(app);
      break;
    case C::ToggleNostrAccounts:
    case C::IdentitySection:
    case C::NostrAddWatchIdentity:
    case C::NostrSelectIdentity:
    case C::NostrIdentityShow:
    case C::NostrIdentityRemove:
    case C::NostrCreateKey:
    case C::NostrImportKey:
    case C::NostrSetKeyPassword:
    case C::NostrUnlockKey:
    case C::NostrLockKey:
    case C::NostrDeleteKey:
    case C::NostrCopyNsec:
    case C::NostrPublishNote:
    case C::IdentitySelect:
    case C::IdentityCreate:
    case C::IdentityCreateHere:
    case C::IdentityUseSite:
    case C::IdentityUsePage:
    case C::IdentityStopHere:
    case C::IdentityDropScope:
    case C::IdentityRename:
    case C::IdentityDelete:
      identity_command(app, cmd, arg);
      break;
    case C::ToggleRelatedSites:
      toggle_related_sites(app);
      break;
    case C::RelatedOpen:
      if (!arg.empty()) open_address(app, arg);
      break;
    case C::RelatedOpenTab:
      if (!arg.empty()) open_link_in_tab(app, app.focus, arg);
      break;
    case C::RelatedFollow:
      toggle_related_follow(app);
      break;
    case C::RelatedRefresh:
      refresh_related_sites(app);
      break;
    case C::CertAccept:
      accept_gemini_cert(app, arg);
      break;
    case C::CertReject:
      reject_gemini_cert(app, arg);
      break;
    case C::CertForget:
      forget_gemini_cert(app, arg);
      break;
    case C::NostrSearchLocal:
      app.nostr_search = arg;
      app.nostr_page = 0;
      refresh_nostr_catalogue(app);
      break;
    case C::NostrShowPeople:
      app.nostr_tab = 0;
      app.nostr_page = 0;
      refresh_nostr_catalogue(app);
      break;
    case C::NostrShowEvents:
      app.nostr_tab = 1;
      app.nostr_page = 0;
      refresh_nostr_catalogue(app);
      break;
    case C::NostrShowGroups:
      app.nostr_tab = 2;
      app.nostr_page = 0;
      refresh_nostr_catalogue(app);
      break;
    case C::NostrRefreshLocal:
      refresh_nostr_catalogue(app);
      break;
    case C::NostrPrevPage:
      if (app.nostr_page > 0) --app.nostr_page;
      refresh_nostr_catalogue(app);
      break;
    case C::NostrNextPage:
      if (app.nostr_tab == 0 ? app.nostr_people_more :
          app.nostr_tab == 1 ? app.nostr_events_more : app.nostr_groups_more)
        ++app.nostr_page;
      refresh_nostr_catalogue(app);
      break;
    case C::ToggleAnnotationLayer:
      if (app.hidden_annotation_layers.contains(arg))
        app.hidden_annotation_layers.erase(arg);
      else
        app.hidden_annotation_layers.insert(arg);
      xanatoo::save_annotation_layer_visibility(
          arg, !app.hidden_annotation_layers.contains(arg));
      ++app.annotation_layers_gen;
      break;
    case C::SelectAnnotationLayer:
      for (const auto& layer : app.annotation_layers)
        if (layer.id == arg && layer.owner == "local")
          app.active_annotation_layer = arg;
      break;
    case C::NewAnnotationLayer: {
      const auto layer = xanatoo::create_annotation_layer(
          "local", "My annotations " + std::to_string(app.annotation_layers.size() + 1));
      if (!layer) {
        flash_status(app, "Could not create annotation layer (revision store unavailable)");
        break;
      }
      app.active_annotation_layer = layer->id;
      app.annotation_layers.push_back(*layer);
      ++app.annotation_layers_gen;
      flash_status(app, "Created a local annotation layer");
      break;
    }
    case C::AddAnnotationHighlight:
    case C::AddAnnotationNote: {
      const bool is_note = cmd == C::AddAnnotationNote;
      if (is_note && arg.empty()) {
        flash_status(app, "Write a note before attaching it");
        break;
      }
      if (p.sel.empty() || p.source_buf == p.buf) {
        flash_status(app, "Select document text to annotate");
        break;
      }
      const std::string address = bookmark_for_view(app, p, false).address;
      if (address.empty()) break;
      const DocBuffer& source = buf(app, p.buf);
      const std::string key = !source.doc_key.empty() ? source.doc_key
                              : !source.path.empty() ? source.path
                                                     : std::string();
      if (key.empty()) {
        flash_status(app, "Document has no persistent identity yet");
        break;
      }
      auto layer = std::find_if(app.annotation_layers.begin(),
                                app.annotation_layers.end(),
                                [&](const auto& l) {
                                  return l.id == app.active_annotation_layer &&
                                         l.owner == "local";
                                });
      if (layer == app.annotation_layers.end()) {
        auto created = xanatoo::create_annotation_layer("local", "My annotations");
        if (!created) {
          flash_status(app, "Cannot save annotations (revision store unavailable)");
          break;
        }
        app.active_annotation_layer = created->id;
        app.annotation_layers.push_back(*created);
        layer = std::prev(app.annotation_layers.end());
      }
      xanatoo::Annotation mark;
      mark.id = xanatoo::new_annotation_id();
      mark.kind = is_note ? "note" : "highlight";
      if (is_note) mark.body = arg;
      mark.targets.push_back({
          key, xanatoo::content_id(xanatoo::serialize_stored_document(d)), address});
      const std::string id = mark.id;
      if (!xanatoo::add_annotation(*layer, std::move(mark))) {
        flash_status(app, "Could not save annotation; no document changes made");
        break;
      }
      app.selected_annotation_id = id;
      if (is_note) ++app.annotation_note_created_gen;
      if (app.hidden_annotation_layers.erase(layer->id))
        xanatoo::save_annotation_layer_visibility(layer->id, true);
      ++app.annotation_layers_gen;
      flash_status(app, is_note ? "Attached note to document passage"
                                : "Highlighted selection in local annotation layer");
      break;
    }
    case C::SelectAnnotation: {
      app.selected_annotation_id = arg;
      select_annotation(app, app.focus, arg);
      break;
    }
    case C::UpdateAnnotationNote:
    case C::RemoveAnnotation: {
      const std::string id = cmd == C::RemoveAnnotation
                                 ? arg : app.selected_annotation_id;
      auto layer = std::find_if(app.annotation_layers.begin(),
                                app.annotation_layers.end(),
                                [&](const auto& candidate) {
        return candidate.owner == "local" &&
            std::any_of(candidate.annotations.begin(),
                        candidate.annotations.end(),
                        [&](const auto& annotation) {
                          return annotation.id == id;
                        });
      });
      if (layer == app.annotation_layers.end()) {
        flash_status(app, "Annotation is not in an editable local layer");
        break;
      }
      const bool removed = cmd == C::RemoveAnnotation;
      const bool saved = removed
          ? xanatoo::remove_annotation(*layer, id)
          : xanatoo::update_annotation_body(*layer, id, arg);
      if (!saved) {
        flash_status(app, "Could not save annotation revision");
        break;
      }
      if (removed && app.selected_annotation_id == id)
        app.selected_annotation_id.clear();
      ++app.annotation_layers_gen;
      flash_status(app, removed ? "Annotation removed (history preserved)"
                                : "Annotation note saved to layer history");
      break;
    }
    case C::RestoreAnnotationLayerVersion: {
      const size_t sep = arg.find('\n');
      if (sep == std::string::npos) break;
      const std::string layer_id = arg.substr(0, sep);
      const std::string revision_id = arg.substr(sep + 1);
      auto layer = std::find_if(app.annotation_layers.begin(),
                                app.annotation_layers.end(),
                                [&](const auto& candidate) {
        return candidate.id == layer_id && candidate.owner == "local";
      });
      if (layer == app.annotation_layers.end() || revision_id.empty() ||
          !xanatoo::restore_annotation_layer_revision(*layer, revision_id)) {
        flash_status(app, "Could not restore layer revision (possibly changed elsewhere)");
        break;
      }
      const bool selected_exists = std::any_of(
          layer->annotations.begin(), layer->annotations.end(),
          [&](const auto& a) { return a.id == app.selected_annotation_id; });
      if (!selected_exists) app.selected_annotation_id.clear();
      ++app.annotation_layers_gen;
      flash_status(app, "Restored annotation layer as a new revision");
      break;
    }
    case C::AddBookmark: {
      xanatoo::Bookmark bm = bookmark_for_view(app, p, false);
      bm.folder = xanatoo::normalize_bookmark_folder(arg);  // panel: its folder
      add_bookmark(app, std::move(bm));
      break;
    }
    case C::MoveBookmark:
    case C::RetitleBookmark:
    case C::MoveBookmarkFolder: {
      const size_t nl = arg.find('\n');
      if (nl == std::string::npos) break;
      const std::string a = arg.substr(0, nl), b = arg.substr(nl + 1);
      if (cmd == C::MoveBookmark) move_bookmark(app, a, b);
      else if (cmd == C::RetitleBookmark) retitle_bookmark(app, a, b);
      else move_bookmark_folder(app, a, b);
      break;
    }
    case C::NewBookmarkFolder:
      create_bookmark_folder(app, arg);
      break;
    case C::RemoveBookmarkFolder:
      remove_bookmark_folder(app, arg);
      break;
    case C::BookmarkPage: {
      // URL chrome refers to the page, never the document text selection.
      xanatoo::Bookmark bm = bookmark_for_view(app, p, false);
      bm.address = pane_address(app, p);
      // The page's own address without tracking parameters (rel=canonical).
      if (std::string c = focused_canonical(app); !c.empty()) bm.address = c;
      bm.title.clear();
      bm.quote.clear();
      add_bookmark(app, std::move(bm));
      break;
    }
    case C::BookmarkPosition:
      add_bookmark(app, bookmark_for_view(app, p, true));
      break;
    case C::RemoveBookmark: {
      std::string url = arg;
      if (url.empty()) {
        url = pane_address(app, p);
        if (std::string c = focused_canonical(app);
            !c.empty() && !is_bookmarked(app, url))
          url = c;
      }
      remove_bookmark(app, url);
      break;
    }
    case C::Save: {
      if (p.source_buf == p.buf && p.source_doc) {
        flash_status(app, "Source view is read-only — return to the document to save");
        break;
      }
      const std::string spath = path_of(app, p);
      if (app.invalid_xml_paths.contains(spath)) {
        flash_status(app, "Invalid XML is read-only — fix the file and reload");
      } else if (app.binary_paths.contains(spath)) {
        flash_status(app, "Binary file: nothing to save");
      } else if (make_stored_copy(app, p.buf) &&
                 save_stored_buffer(app, focused_pane(app).buf)) {
        app.discard_armed = false;
        flash_status(app, "Saved working document in local database");
      } else {
        flash_status(app, "Could not save to local database; original was not changed");
      }
      break;
    }
    case C::EditLocalCopy:
      if (make_stored_copy(app, p.buf))
        flash_status(app, "Editing local database copy; original preserved");
      else
        flash_status(app, "Could not make local copy; original preserved");
      break;
    case C::ViewOriginal: {
      const std::string path = path_of(app, p);
      if (is_stored_document_ref(path) &&
          find_document_origin(path.substr(3)) &&
          navigate_pane(app, app.focus, "db-original:" + path.substr(3)))
        flash_status(app, "Viewing immutable original; Back returns to working copy");
      else
        flash_status(app, "This document has no imported original");
      break;
    }
    case C::ViewSourceRendered:
    case C::ViewSourceRaw:
    case C::ViewSourceXml: {
      // These are pane-local representations of the already loaded buffer.
      // Do not re-open draft:/db:/remote pages just to change projection.
      const std::string previous = pane_address(app, p);
      xanatoo::Locator loc;
      if (cmd == C::ViewSourceRaw)
        loc.pipes.push_back({xanatoo::SelectorKind::SourceRaw, {}});
      else if (cmd == C::ViewSourceXml)
        loc.pipes.push_back({xanatoo::SelectorKind::SourceXml, {}});
      if (!previous.empty())
        nav_history_capture(view_nav(app, app.focus), previous, p.sel, p.scroll_y);
      apply_locator(app, app.focus, std::move(loc));
      if (!previous.empty())
        push_nav_history(app, app.focus, pane_address(app, p));
      break;
    }
    case C::ToggleGallery: {
      // In: the images of what the view shows (its cut). Out: that again.
      xanatoo::Locator loc = xanatoo::parse_locator(pane_address(app, p));
      if (loc.base.empty()) break;
      const auto g = std::find_if(
          loc.pipes.begin(), loc.pipes.end(), [](const xanatoo::Selector& s) {
            return s.kind == xanatoo::SelectorKind::Gallery ||
                   xanatoo::is_source_pipe(s.kind);
          });
      const bool in = pane_gallery(p) != nullptr;
      const bool gallery =
          g != loc.pipes.end() && g->kind == xanatoo::SelectorKind::Gallery;
      loc.pipes.erase(g, loc.pipes.end());
      loc.point.reset();
      if (!in) {
        loc.pipes.push_back({xanatoo::SelectorKind::Gallery, {}});
      } else if (gallery && !loc.pipes.empty()) {
        loc.point = std::move(loc.pipes.back());  // the page, at its scope
        loc.pipes.pop_back();
      }
      navigate_pane(app, app.focus, xanatoo::format_locator(loc), true);
      break;
    }
    case C::Publish:
      if (path_of(app, p).empty() ||
          is_stored_document_ref(path_of(app, p))) {
        if (publish_stored_buffer(app, p.buf))
          flash_status(app, "Published local snapshot (no remote destination configured)");
        else
          flash_status(app, "Could not publish working document to local database");
      } else {
        flash_status(app, "Only database-native documents can be published locally");
      }
      break;
    case C::SaveAs: {
      const std::string path = path_of(app, p);
      std::string suggested = "untitled.md";
      if (is_in_app_remote_url(path)) {
        suggested = "page.md";
      } else if (!path.empty() && !is_stored_document_ref(path) &&
                 !is_stored_original_ref(path)) {
        const fs::path fp(path);
        suggested = (fp.parent_path() /
                     (fp.stem().string() + "-export.md")).string();
      }
      open_prompt(app, PromptKind::SaveAs, std::move(suggested));
      break;
    }
    case C::ExportXml:
    case C::ExportMarkdown: {
      const char* ext = cmd == C::ExportXml ? ".xml" : ".md";
      std::string cur = path_of(app, p);
      std::string target;
      if (cur.empty() || is_stored_document_ref(cur) ||
          is_stored_original_ref(cur) || is_in_app_remote_url(cur)) {
        target = std::string("untitled") + ext;
      } else {
        std::filesystem::path fp(cur);
        target = fp.stem().string() + "-export" + ext;
        if (fp.has_parent_path() && !fp.parent_path().empty())
          target = (fp.parent_path() / (fp.stem().string() + "-export" + ext)).string();
      }
      open_prompt(app, PromptKind::SaveAs, std::move(target));
      break;
    }
    case C::Reload:
    case C::ForceReload:
    case C::RevertToSaved: {
      std::string& rpath = path_of(app, p);
      if (rpath.empty()) break;
      if (rpath == kDraftsPage) {
        open_drafts_page(app);
        break;
      }
      if (rpath == kDocumentsPage) {
        open_stored_documents_page(app);
        break;
      }
      if (is_stored_document_ref(rpath)) {
        const std::string stable_ref = rpath;
        if (!open_stored_document(app, app.focus, stable_ref))
          flash_status(app, "Could not reload local document");
        else
          flash_status(app, "Reloaded local working revision");
        break;
      }
      if (is_stored_original_ref(rpath)) {
        const std::string stable_ref = rpath;
        if (!open_stored_original(app, app.focus, stable_ref))
          flash_status(app, "Could not reload immutable original");
        break;
      }
      if (rpath == kAboutPage) {
        open_about_page(app);
        break;
      }
      if (cmd == C::RevertToSaved) {
        revert_buffer(app, p.buf);
      } else if (buf(app, p.buf).dirty && !is_foreign_path(rpath)) {
        // Reloading would just restore the draft again.
        stash_draft(app, p.buf);
        flash_status(app, "Unsaved changes are kept as a draft — "
                          "File → Revert to Saved drops them");
        break;
      }
      // Force: converted pages are converted afresh, downloaded books
      // downloaded again; a plain Reload reuses what is still current.
      if (cmd == C::ForceReload) {
        if (xanatoo::looks_like_local_path(rpath) && xanatoo::is_pandoc_path(rpath))
          xanatoo::invalidate_import(rpath);
#if defined(XANATOO_EBOOKS) && XANATOO_EBOOKS
        if (xanatoo::is_ebook_url(rpath)) xanatoo::ebook_forget_download(rpath);
#endif
      }
      if (is_in_app_remote_url(rpath)) {
        refetch_page(app, app.focus);
        break;
      }
      xanatoo::clear_external_doc_cache();
      buf(app, p.buf).doc = load_doc(app, rpath);
      buf(app, p.buf).dirty = false;
      buf(app, p.buf).observed = {};  // observed afresh by the visit
      buf(app, p.buf).observation_id = 0;
      attach_draft(app, p.buf);
      buf(app, p.buf).history.reset(
          buf(app, p.buf).doc, p.sel);
      refresh_views_of_buf(app, p.buf, -1, true);
      drop_height_hints_for_buf(app, p.buf);
      if (cmd != C::RevertToSaved) record_reload_visit(app, app.focus);
      reset_blink(app);
      update_title(app);
      app.needs_layout = true;
      flash_status(app, (cmd == C::RevertToSaved ? "Reverted to "
                         : cmd == C::ForceReload ? "Force reloaded "
                                                 : "Reloaded ") +
                            rpath);
      break;
    }
    case C::Undo:
      if (buf(app, p.buf).history.undo(d, p.sel)) {
        xanatoo::ensure_valid_caret(d, p.sel.focus);
        p.sel.anchor = p.sel.focus;
        p.layout = {};
        refresh_views_of_buf(app, p.buf, app.focus, false);
        reset_blink(app);
        drop_height_hints_for_buf(app, p.buf);
        app.needs_layout = true;
      }
      break;
    case C::Redo:
      if (buf(app, p.buf).history.redo(d, p.sel)) {
        xanatoo::ensure_valid_caret(d, p.sel.focus);
        p.sel.anchor = p.sel.focus;
        p.layout = {};
        refresh_views_of_buf(app, p.buf, app.focus, false);
        reset_blink(app);
        drop_height_hints_for_buf(app, p.buf);
        app.needs_layout = true;
      }
      break;
    case C::Cut: {
      std::string text = xanatoo::selected_text(d, p.sel);
      if (!text.empty()) {
        SDL_SetClipboardText(text.c_str());
        edit(app);
        xanatoo::delete_selection(d, p.sel);
        app.needs_layout = true;
        reset_blink(app);
      }
      break;
    }
    case C::SelectBookmark:
      if (!arg.empty()) select_bookmark(app, app.focus, arg);
      break;
    case C::OpenLinkInTab:
      if (!arg.empty()) open_link_in_tab(app, app.focus, arg);
      break;
    case C::OpenLinkInTrail:
      if (!arg.empty()) open_link_in_trail(app, app.focus, arg);
      break;
    case C::DownloadUrl:
      if (!arg.empty()) start_download(app, arg);
      break;
    case C::SavePdf: {
      if (app.context_pdf_path.empty()) break;
      fs::path name = fs::path(app.context_pdf_name).filename();
      if (!xanatoo::ends_with(xanatoo::ascii_lower(name.string()), ".pdf"))
        name = "document.pdf";
      open_prompt(app, PromptKind::SavePdf,
                  (fs::path(xanatoo::downloads_dir()) / name).string());
      app.prompt.source_path = app.context_pdf_path;
      break;
    }
    case C::ConfirmDownload:
      if (!app.download_confirm.url.empty()) {
        const std::string url = app.download_confirm.url;
        app.download_confirm = {};
        begin_download(app, url, {});
      }
      break;
    case C::DownloadToFolder:
      if (!app.download_confirm.url.empty()) {
        if (download_folder_dialog_available()) {
          const std::string url = app.download_confirm.url;
          app.download_confirm = {};
          pick_download_folder(app, url);
        } else {
          const std::string url = app.download_confirm.url;
          app.download_confirm = {};
          begin_download(app, url, {});
        }
      }
      break;
    case C::CancelDownload:
      app.download_confirm = {};
      flash_status(app, "Download cancelled");
      break;
    case C::DownloadAction:
      download_action(app, arg);
      break;
    case C::HistoryAction:
      history_action(app, arg);
      break;
    case C::MediaAction:
      media_action(app, arg);
      break;
    case C::OpenImageInTab:
      if (!arg.empty()) open_image_in_tab(app, app.focus, arg);
      break;
    case C::CopyImage:
      if (!arg.empty()) {
        const std::string error = app.images.copy_to_clipboard(arg);
        flash_status(app, error.empty() ? "Copied image" : error);
      }
      break;
    case C::CopyText:
      if (!arg.empty()) {
        SDL_SetClipboardText(arg.c_str());
        flash_status(app, "Copied " + arg);
      }
      break;
    case C::CopyPageUrl: {
      const std::string url = pane_address(app, p);
      if (url.empty()) {
        flash_status(app, "No page URL");
        break;
      }
      SDL_SetClipboardText(url.c_str());
      flash_status(app, "Copied " + url);
      break;
    }
    case C::OpenInBrowser: {
      const std::string url = pane_browser_url(app, p);
      if (url.empty()) {
        flash_status(app, "No web address for this page");
        break;
      }
      xanatoo::open_external_url(url);
      flash_status(app, "Opened " + url + " in the browser");
      break;
    }
    case C::CopyPageWebUrl: {
      const std::string url = pane_web_url(app, p);
      if (url.empty()) {
        flash_status(app, "No web address for this page");
        break;
      }
      SDL_SetClipboardText(url.c_str());
      flash_status(app, "Copied " + url);
      break;
    }
    case C::Copy: {
      // The Context panel's own selection while it has the keyboard.
      if (panel_has_keys(app, KeyFocus::Context) && app.context_has_doc &&
          !app.context_sel.empty()) {
        const std::string text =
            xanatoo::selected_text(app.context_doc, app.context_sel);
        if (!text.empty()) SDL_SetClipboardText(text.c_str());
        break;
      }
      std::string text = copied_text(app, p);
      if (!text.empty()) SDL_SetClipboardText(text.c_str());
      break;
    }
    case C::Paste:
      if (SDL_HasClipboardText()) {
        char* clip = SDL_GetClipboardText();
        if (clip && clip[0]) {
          edit(app);
          if (!p.sel.empty()) xanatoo::delete_selection(d, p.sel);
          xanatoo::paste_plain_text(d, p.sel.focus, clip);
          p.sel.anchor = p.sel.focus;
          app.needs_layout = true;
        }
        if (clip) SDL_free(clip);
      }
      break;
    case C::SelectAll:
      xanatoo::select_all(d, p.sel);
      break;
    case C::ExpandSelection:
      xanatoo::expand_selection(d, p.sel);
      break;
    case C::SelectBlock:
      if (p.sel.focus.node != xanatoo::INVALID_NODE)
        xanatoo::select_node(d, p.sel, p.sel.focus.node);
      break;
    case C::DuplicateBlock:
      edit(app);
      if (auto nid = xanatoo::duplicate_block(d, p.sel.focus.node)) {
        p.sel.focus.node = *nid;
        p.sel.focus.span_index = 0;
        p.sel.focus.char_index = 0;
        p.sel.anchor = p.sel.focus;
        app.needs_layout = true;
        reset_blink(app);
      }
      break;
    case C::MoveBlockUp:
      edit(app);
      if (xanatoo::move_block_up(d, p.sel.focus.node)) {
        app.needs_layout = true;
        reset_blink(app);
      }
      break;
    case C::MoveBlockDown:
      edit(app);
      if (xanatoo::move_block_down(d, p.sel.focus.node)) {
        app.needs_layout = true;
        reset_blink(app);
      }
      break;
    case C::Find:
      open_search(app, false);
      break;
    case C::Replace:
      open_search(app, true);
      break;
    case C::ReplaceAll:
      find_set_texts(app, arg);
      find_replace_all(app);
      break;
    case C::Bold:
      if (!p.sel.empty()) {
        edit(app);
        xanatoo::apply_style_to_selection(d, p.sel,
                                          xanatoo::InlineKind::Strong);
        app.needs_layout = true;
      }
      break;
    case C::Italic:
      if (!p.sel.empty()) {
        edit(app);
        xanatoo::apply_style_to_selection(d, p.sel,
                                          xanatoo::InlineKind::Emphasis);
        app.needs_layout = true;
      }
      break;
    case C::Strike:
      if (!p.sel.empty()) {
        edit(app);
        xanatoo::apply_style_to_selection(d, p.sel,
                                          xanatoo::InlineKind::Strike);
        app.needs_layout = true;
      }
      break;
    case C::Code:
      if (!p.sel.empty()) {
        edit(app);
        xanatoo::apply_style_to_selection(d, p.sel, xanatoo::InlineKind::Code);
        app.needs_layout = true;
      }
      break;
    case C::Link:
      if (!p.sel.empty()) {
        std::string text = xanatoo::selected_text(d, p.sel);
        edit(app);
        xanatoo::apply_style_to_selection(d, p.sel, xanatoo::InlineKind::Link,
                                          "#" + text);
        open_prompt(app, PromptKind::EditLink, "#" + text);
        app.prompt.node = p.sel.focus.node;
        app.prompt.span = p.sel.focus.span_index;
        app.needs_layout = true;
      }
      break;
    case C::Cite:
      if (!p.sel.empty()) {
        std::string text = xanatoo::selected_text(d, p.sel);
        std::string key = text;
        for (char& ch : key) {
          if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
          else if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')))
            ch = '-';
        }
        while (!key.empty() && key.front() == '-') key.erase(key.begin());
        while (!key.empty() && key.back() == '-') key.pop_back();
        if (key.empty()) key = "key";
        if (key.size() > 32) key.resize(32);
        edit(app);
        xanatoo::apply_style_to_selection(d, p.sel, xanatoo::InlineKind::Cite,
                                          key);
        open_prompt(app, PromptKind::EditCite, key);
        app.prompt.node = p.sel.focus.node;
        app.prompt.span = p.sel.focus.span_index;
        app.needs_layout = true;
      }
      break;
    case C::InsertBiblioEntry: {
      edit(app);
      std::string key = "key";
      if (xanatoo::insert_biblio_entry(d, p.sel.focus, key)) {
        p.sel.anchor = p.sel.focus;
        // Open key-edit on the entry label via status: reuse link_edit for
        // label (label is not a span; set it when Enter confirms after
        // typing).
        open_prompt(app, PromptKind::EditCite, key);
        app.prompt.node = p.sel.focus.node;
        app.prompt.span = 0;
        app.needs_layout = true;
      }
      break;
    }
    case C::InsertParagraph:
      edit(app);
      if (xanatoo::insert_sibling_after(d, p.sel.focus,
                                        xanatoo::NodeKind::Paragraph)) {
        p.sel.anchor = p.sel.focus;
        app.needs_layout = true;
      }
      break;
    case C::InsertHr:
      edit(app);
      if (xanatoo::insert_sibling_after(d, p.sel.focus,
                                        xanatoo::NodeKind::HorizontalRule)) {
        p.sel.anchor = p.sel.focus;
        app.needs_layout = true;
      }
      break;
    case C::InsertNote:
      edit(app);
      if (xanatoo::insert_sibling_after(d, p.sel.focus,
                                        xanatoo::NodeKind::Note)) {
        p.sel.anchor = p.sel.focus;
        app.needs_layout = true;
      }
      break;
    case C::InsertCodeBlock:
      edit(app);
      if (xanatoo::insert_sibling_after(d, p.sel.focus,
                                        xanatoo::NodeKind::CodeBlock)) {
        p.sel.anchor = p.sel.focus;
        app.needs_layout = true;
      }
      break;
    case C::InsertTable:
      edit(app);
      if (xanatoo::insert_table(d, p.sel.focus)) {
        p.sel.anchor = p.sel.focus;
        app.needs_layout = true;
      }
      break;
    case C::InsertDefList:
      edit(app);
      if (xanatoo::insert_deflist(d, p.sel.focus)) {
        p.sel.anchor = p.sel.focus;
        app.needs_layout = true;
      }
      break;
    case C::InsertColumns:
      edit(app);
      if (xanatoo::insert_columns(d, p.sel.focus, 2)) {
        p.sel.anchor = p.sel.focus;
        app.needs_layout = true;
      }
      break;
    case C::InsertTransclude: {
      std::string t;
      if (!p.sel.empty()) {
        t = xanatoo::selected_text(d, p.sel);
        if (!t.empty() && t[0] != '#') t = "#" + t;
      }
      open_prompt(app, PromptKind::InsertInclude, std::move(t));
      break;
    }
    case C::InsertImage:
      open_prompt(app, PromptKind::InsertImage);
      break;
    case C::InsertFigure:
      open_prompt(app, PromptKind::InsertFigure);
      break;
    case C::Heading1:
      edit(app);
      if (xanatoo::set_heading_level(d, p.sel.focus, 1))
        app.needs_layout = true;
      break;
    case C::Heading2:
      edit(app);
      if (xanatoo::set_heading_level(d, p.sel.focus, 2))
        app.needs_layout = true;
      break;
    case C::Heading3:
      edit(app);
      if (xanatoo::set_heading_level(d, p.sel.focus, 3))
        app.needs_layout = true;
      break;
    case C::Heading4:
      edit(app);
      if (xanatoo::set_heading_level(d, p.sel.focus, 4))
        app.needs_layout = true;
      break;
    case C::Heading5:
      edit(app);
      if (xanatoo::set_heading_level(d, p.sel.focus, 5))
        app.needs_layout = true;
      break;
    case C::Heading6:
      edit(app);
      if (xanatoo::set_heading_level(d, p.sel.focus, 6))
        app.needs_layout = true;
      break;
    case C::MakeHeader:
      edit(app);
      if (xanatoo::make_header(d, p.sel.focus)) {
        app.needs_layout = true;
        flash_status(app, "Heading");
      }
      break;
    case C::WrapSection:
      edit(app);
      if (xanatoo::wrap_in_section(d, p.sel.focus)) {
        app.needs_layout = true;
        flash_status(app, "Wrapped in section");
      }
      break;
    case C::WrapArticle:
      edit(app);
      if (xanatoo::wrap_in_article(d, p.sel.focus)) {
        app.needs_layout = true;
        flash_status(app, "Wrapped in article");
      }
      break;
    case C::UnwrapSectioning:
      edit(app);
      if (xanatoo::unwrap_sectioning(d, p.sel.focus)) {
        app.needs_layout = true;
        flash_status(app, "Unwrapped section/article");
      }
      break;
    case C::PromoteSection:
      edit(app);
      if (xanatoo::promote_section(d, p.sel.focus)) {
        app.needs_layout = true;
        flash_status(app, "Section promoted");
      } else {
        flash_status(app, "Cannot promote section");
      }
      break;
    case C::DemoteSection:
      edit(app);
      if (xanatoo::demote_section(d, p.sel.focus)) {
        app.needs_layout = true;
        flash_status(app, "Section demoted");
      } else {
        flash_status(app,
                     "Cannot demote section (need previous section sibling)");
      }
      break;
    case C::MoveSectionUp:
      edit(app);
      if (xanatoo::move_section_up(d, p.sel.focus)) {
        app.needs_layout = true;
        flash_status(app, "Section moved up");
      }
      break;
    case C::MoveSectionDown:
      edit(app);
      if (xanatoo::move_section_down(d, p.sel.focus)) {
        app.needs_layout = true;
        flash_status(app, "Section moved down");
      }
      break;
    case C::ParagraphLevel:
      edit(app);
      if (xanatoo::set_heading_level(d, p.sel.focus, 0))
        app.needs_layout = true;
      break;
    case C::ToggleUnorderedList:
      edit(app);
      if (xanatoo::toggle_list(d, p.sel.focus, false)) app.needs_layout = true;
      break;
    case C::ToggleOrderedList:
      edit(app);
      if (xanatoo::toggle_list(d, p.sel.focus, true)) app.needs_layout = true;
      break;
    case C::ToggleTask:
      edit(app);
      if (xanatoo::toggle_task_item(d, p.sel.focus)) app.needs_layout = true;
      break;
    case C::ToggleBlockQuote: {
      edit(app);
      auto& node = d.get(p.sel.focus.node);
      if (node.kind == xanatoo::NodeKind::BlockQuote) {
        node.kind = xanatoo::NodeKind::Paragraph;
      } else if (node.kind == xanatoo::NodeKind::Paragraph ||
                 node.kind == xanatoo::NodeKind::Heading ||
                 node.kind == xanatoo::NodeKind::ListItem ||
                 node.kind == xanatoo::NodeKind::Note) {
        node.kind = xanatoo::NodeKind::BlockQuote;
        node.level = 0;
      }
      app.needs_layout = true;
      break;
    }
    case C::IndentList:
      edit(app);
      if (xanatoo::indent_list_item(d, p.sel.focus)) {
        p.sel.anchor = p.sel.focus;
        reveal_folds(app, p, p.sel.focus.node);
        app.needs_layout = true;
      }
      break;
    case C::OutdentList:
      edit(app);
      if (xanatoo::outdent_list_item(d, p.sel.focus)) {
        p.sel.anchor = p.sel.focus;
        reveal_folds(app, p, p.sel.focus.node);
        app.needs_layout = true;
      }
      break;
    case C::WrapStretch: {
      edit(app);
      std::string summary = xanatoo::selected_text(d, p.sel);
      if (xanatoo::wrap_block_in_stretch(d, p.sel.focus.node, summary)) {
        app.needs_layout = true;
      }
      break;
    }
    case C::EditStretchSummary: {
      xanatoo::NodeId sid = xanatoo::enclosing_stretch(d, p.sel.focus.node);
      if (sid != xanatoo::INVALID_NODE) {
        open_prompt(app, PromptKind::EditSummary, d.get(sid).summary);
        app.prompt.node = sid;
      }
      break;
    }
    case C::UnwrapStretch: {
      edit(app);
      if (auto first = xanatoo::unwrap_stretch(d, p.sel.focus.node)) {
        p.sel.focus.node = *first;
        p.sel.focus.span_index = 0;
        p.sel.focus.char_index = 0;
        p.sel.anchor = p.sel.focus;
        app.needs_layout = true;
        reset_blink(app);
      }
      break;
    }
    case C::ToggleSplit:
      if (app.split && app.tile_mode == TileMode::Independent && !app.diff_mode) {
        app.split = false;
        if (app.focus == 1) focus_view(app, 0);
      } else {
        if (!app.split) {
          // A new tile starts as a second view on the same document.
          app.panes[1].buf = app.panes[0].buf;
          app.panes[1].sel = app.panes[0].sel;
          app.panes[1].scroll_y = app.panes[0].scroll_y;
        }
        app.split = true;
        app.tile_mode = TileMode::Independent;
        app.diff_mode = false;
        refresh_diff(app);
      }
      app.needs_layout = true;
      break;
    case C::ToggleTrail:
      if (app.split && app.tile_mode == TileMode::Trail) {
        app.split = false;
        if (app.focus == 1) focus_view(app, 0);
      } else
        enter_tile_trail(app);
      app.needs_layout = true;
      break;
    case C::TilesIndependent:
      app.split = true;
      app.tile_mode = TileMode::Independent;
      app.diff_mode = false;
      refresh_diff(app);
      app.needs_layout = true;
      break;
    case C::TilesTrail:
      enter_tile_trail(app);
      break;
    case C::TilesCompare:
      if (shared_doc(app) || app.buffers.size() < 2) {
        flash_status(app, "Compare requires two different documents");
      } else {
        app.split = true;
        app.tile_mode = TileMode::Independent;
        app.diff_mode = true;
        refresh_diff(app);
        app.needs_layout = true;
      }
      break;
    case C::TilesColumns:
    case C::TilesRows:
      app.tile_axis = cmd == C::TilesRows ? TileAxis::Rows : TileAxis::Columns;
      app.needs_layout = true;
      break;
    case C::SwapPanes:
    case C::JoinLeftPane:
    case C::JoinRightPane: {
      if (!app.split) break;
      // An active Trail hover temporarily replaces the second view with a
      // preview. Put its real document back before swapping or collapsing.
      revert_split_preview(app);
      app.split_preview.hover.clear();
      app.split_preview.shown.clear();
      app.split_preview.pending.clear();
      if (cmd != C::JoinLeftPane) {
        // A Pane owns selection, history, source projections and render
        // caches. Swap the whole view, then restore its physical viewport.
        // This also works for source views and independently scrolled tabs.
        std::swap(app.panes[0], app.panes[1]);
        for (FetchJob& job : app.fetches)
          if (job.purpose == FetchPurpose::Navigate &&
              job.pane >= 0 && job.pane < kCentreViews)
            job.pane = 1 - job.pane;
        for (int i = 0; i < kCentreViews; ++i) {
          Pane& v = app.panes[i];
          v.layout = {};
          v.layout_buf = kNoBuffer;
          v.height_cache.clear();
          v.raster.valid = false;
        }
      }
      app.split_history = {};
      app.split_trail = {};
      if (cmd != C::SwapPanes) {
        app.split = false;
        app.tile_mode = TileMode::Independent;
        app.diff_mode = false;
        cancel_fetches(app, 1);  // the discarded view must not update later
        focus_view(app, 0);
        const BufferId kept = app.panes[0].buf;
        if (find_buf(app, kept) &&
            std::find(app.tab_order.begin(), app.tab_order.end(), kept) ==
                app.tab_order.end())
          app.tab_order.push_back(kept);
        flash_status(app, "Joined panes");
      } else {
        flash_status(app, "Swapped panes");
      }
      refresh_diff(app);
      app.needs_layout = true;
      update_title(app);
      break;
    }
    case C::ToggleOutline:
      app.outline_mode = !app.outline_mode;
      if (app.outline_mode) {
        app.key_focus = KeyFocus::Outline;
        refresh_outline(app);
        app.outline_sel = 0;
        app.ol.scroll = 0.f;
      }
      app.needs_layout = true;
      layout_panes(app);
      break;
    case C::ToggleSitemap:
      toggle_sitemap(app);
      break;
    case C::TogglePage:
      toggle_page_panel(app);
      break;
    case C::ToggleSearchBar:
      toggle_search_bar(app);
      break;
    case C::AudioToggle:
      audio_toggle(app);
      break;
    case C::AudioStop:
      audio_stop(app);
      break;
    case C::AudioSkip:
      audio_skip(app, std::atof(arg.c_str()));
      break;
    case C::AudioAB:
      audio_toggle_ab(app);
      break;
    case C::AudioLoop:
      audio_toggle_loop(app);
      break;
    case C::AudioFullscreen:
      toggle_media_fullscreen(app);
      break;
    case C::AudioControls:
      toggle_media_controls(app);
      break;
    case C::FocusSearchBar:
      show_search_bar(app);
      break;
    case C::Search:
      run_search(app, arg);
      break;
    case C::SelectSearchEngine:
      select_search_engine(app, arg);
      break;
    case C::ToggleSearchEngine:
      toggle_search_engine(app, arg);
      break;
    case C::SearchSite:
      search_site(app, arg);
      break;
    case C::AddSearchEngine:
      add_search_engine_from(app, arg);
      break;
    case C::SetLanguages:
      set_preferred_languages(app, xanatoo::parse_language_list(arg));
      break;
    case C::SitemapPrev:
    case C::SitemapNext:
      sitemap_step(app, cmd == C::SitemapNext ? 1 : -1);
      break;
    case C::ToggleBacklinks:
      app.backlinks_mode = !app.backlinks_mode;
      if (app.backlinks_mode) {
        app.key_focus = KeyFocus::Backlinks;
        refresh_backlinks(app);
        app.backlinks_sel = 0;
        app.bl.scroll = 0.f;
        if (app.backlinks_label.empty())
          flash_status(app, "Backlinks: place caret on a labelled node");
        else
          flash_status(app, "Backlinks to #" + app.backlinks_label + " (" +
                                std::to_string(app.backlinks.size()) + ")");
      }
      break;
    case C::OpenTranscludeOther:
      open_transclude_in_other_pane(app);
      break;
    case C::ToggleDiff:
      if (shared_doc(app) || app.buffers.size() < 2) {
        flash_status(app,
                     "Diff needs two files — Examples → Diff demo, or "
                     "xanatoo a.md b.md");
      } else {
        app.diff_mode = !app.diff_mode;
        if (app.diff_mode) {
          app.split = true;
          app.tile_mode = TileMode::Independent;
        }
        refresh_diff(app);
        app.needs_layout = true;
        flash_status(app, app.diff_mode ? "Diff marks on" : "Diff marks off");
      }
      break;
    case C::ToggleStructure:
      app.structure_mode = !app.structure_mode;
      flash_status(app, app.structure_mode ? "Structure highlight on"
                                           : "Structure highlight off");
      break;
    case C::ToggleLock: {
      if (focused_read_only(app)) {
        flash_status(app, kReadOnlyNotice);
        break;
      }
      const bool lock = !pane_locked(app, p);
      set_pane_locked(app, p, lock);
      if (lock) {
        // Clear any status prompts so they do not trap keys
        leave_prompt(app);
        flash_status(app, "Locked — read only · F2 unlocks");
      } else {
        flash_status(app, "Unlocked — editing");
      }
      break;
    }
    case C::ToggleChrome:
      toggle_chrome(app);
      break;
    case C::ToggleHoverHelp:
      toggle_hover_help(app);
      break;
    case C::ToggleHelp:
      app.help_mode = !app.help_mode;
      if (app.help_mode) {
        focus_panel(app, WindowRole::Help);
        ensure_help_doc(app);
        app.help_needs_layout = true;
      }
      break;
    case C::ToggleVirtualKeyboard:
      app.virtual_keyboard = !app.virtual_keyboard;
      if (app.virtual_keyboard && browsing(app) && !focused_read_only(app)) {
        set_pane_locked(app, p, false);
        flash_status(app, "Virtual keyboard — unlocked for editing");
      } else {
        flash_status(app, app.virtual_keyboard ? "Virtual keyboard on"
                                               : "Virtual keyboard off");
      }
      break;
    case C::VirtualKey: {
      if (browsing(app)) {
        flash_status(app, no_edit_notice(app));
        break;
      }
      if (arg.empty()) break;
      Pane& vp = focused_pane(app);
      xanatoo::Document& vd = doc_of(app, vp);
      if (arg == "\b") {
        edit(app);
        if (!vp.sel.empty())
          xanatoo::delete_selection(vd, vp.sel);
        else
          xanatoo::delete_backward(vd, vp.sel.focus);
        vp.sel.anchor = vp.sel.focus;
        app.needs_layout = true;
        scroll_caret_into_view(app, vp);
        reset_blink(app);
      } else if (arg == "\n") {
        edit(app);
        if (!vp.sel.empty()) xanatoo::delete_selection(vd, vp.sel);
        xanatoo::split_block(vd, vp.sel.focus);
        vp.sel.anchor = vp.sel.focus;
        app.needs_layout = true;
        scroll_caret_into_view(app, vp);
        reset_blink(app);
      } else {
        edit(app);
        if (!vp.sel.empty()) xanatoo::delete_selection(vd, vp.sel);
        xanatoo::insert_text(vd, vp.sel.focus, arg);
        vp.sel.anchor = vp.sel.focus;
        app.needs_layout = true;
        scroll_caret_into_view(app, vp);
        reset_blink(app);
      }
      break;
    }
    case C::OpenFormatDoc: {
      std::string path = find_data_file(app, "FORMAT.md");
      if (path.empty()) path = find_data_file(app, "FORMAT.md");
      open_browse(app, path, "FORMAT.md — format spec");
      break;
    }
    case C::OpenSemanticDoc: {
      open_browse(app, find_data_file(app, "examples/semantic.xml"),
                  "semantic.xml — demo");
      break;
    }
    case C::OpenInWindow:
      open_prompt(app, PromptKind::OpenInWindow);
      break;
    case C::OpenExample: {
      std::string name = arg;
      if (name.empty()) break;
      // examples/<name> under the data dir (paths.hpp).
      std::string path = find_data_file(app, name.c_str());
      if (path.empty())
        path = find_data_file(app, (std::string("examples/") + name).c_str());
      if (path.empty() || !fs::exists(path)) {
        flash_status(app, "Example not found: " + name);
        std::cerr << "Example not found: " << name
                  << " (see --data-dir)\n";
        break;
      }
      // Load into the focused pane (not a soft window). On R36S 640×480
      // floating windows are too tight for org tables / gemtext samples.
      if (!navigate_to_url(app, path, /*record_history=*/true)) {
        flash_status(app, "Failed to open: " + name);
        break;
      }
      {
        std::string title = name;
        try {
          title = fs::path(path).filename().string();
        } catch (...) {
        }
        flash_status(app, "Opened example: " + title);
      }
      break;
    }
    case C::LoadDiffDemo: {
      if (!stash_all_or_confirm(app)) break;
      std::string p0 = find_data_file(app, "diff-v1.md");
      std::string p1 = find_data_file(app, "diff-v2.md");
      if (p0.empty()) p0 = find_data_file(app, "examples/diff-v1.md");
      if (p1.empty()) p1 = find_data_file(app, "examples/diff-v2.md");
      if (p0.empty() || p1.empty() || !fs::exists(p0) || !fs::exists(p1)) {
        flash_status(app,
                     "Diff demo files not found (diff-v1.md / diff-v2.md)");
        std::cerr << "Diff demo: examples/diff-v1.md and diff-v2.md required\n";
        break;
      }
      // Drop floats — their layouts refer to old trees.
      for (const FloatWindow& w : app.soft_wins) release_view(app, w.view);
      app.soft_wins.clear();
      clear_interaction_hover(app);
      app.buffers.clear();
      const BufferId d0 = add_buffer(app, load_doc(app, p0), p0);
      const BufferId d1 = add_buffer(app, load_doc(app, p1), p1);
      app.tab_order = {d0, d1};
      app.panes[0].buf = d0;
      app.panes[1].buf = d1;
      app.split = true;
      app.tile_mode = TileMode::Independent;
      focus_view(app, 0);
      for (int pi = 0; pi < kCentreViews; ++pi) {
        Pane& dp = app.panes[pi];
        DocBuffer& db = buf(app, dp.buf);
        xanatoo::ensure_valid_caret(db.doc, dp.sel.focus);
        dp.sel.anchor = dp.sel.focus;
        dp.scroll_y = 0.f;
        db.history.reset(db.doc, dp.sel);
      }
      app.diff_mode = true;
      refresh_diff(app);
      app.needs_layout = true;
      update_title(app);
      flash_status(app, "Diff demo — orange bars mark changed blocks");
      std::cout << "Diff demo: " << p0 << " | " << p1 << "\n";
      break;
    }
    case C::ZoomIn:
      zoom_by(app, 1);
      break;
    case C::ZoomOut:
      zoom_by(app, -1);
      break;
    case C::ViewLineSpacingTight:
      app.view_settings.line_spacing = xanatoo::ViewSettings::kLineTight;
      apply_view_settings(app);
      flash_status(app, "Line spacing: tight");
      break;
    case C::ViewLineSpacingNormal:
      app.view_settings.line_spacing = xanatoo::ViewSettings::kLineNormal;
      apply_view_settings(app);
      flash_status(app, "Line spacing: normal");
      break;
    case C::ViewLineSpacingRelaxed:
      app.view_settings.line_spacing = xanatoo::ViewSettings::kLineRelaxed;
      apply_view_settings(app);
      flash_status(app, "Line spacing: relaxed");
      break;
    case C::ViewLineSpacingLoose:
      app.view_settings.line_spacing = xanatoo::ViewSettings::kLineLoose;
      apply_view_settings(app);
      flash_status(app, "Line spacing: loose");
      break;
    case C::ViewMargins:
      try {
        app.view_settings.margin_pct = std::stof(arg);
      } catch (...) {
        break;
      }
      apply_view_settings(app);
      flash_status(app, "Margins: " + arg + "%");
      break;
    case C::ViewMeasure:
      try {
        app.view_settings.measure_chars = std::stoi(arg);
      } catch (...) {
        break;
      }
      apply_view_settings(app);
      flash_status(app, app.view_settings.measure_chars > 0
                            ? "Text width: " + arg + " characters"
                            : std::string("Text width: full"));
      break;
    case C::ToggleSectionFocus:
      toggle_section_focus_mode(app);
      break;
    case C::SelectStyle:
      if (!arg.empty()) apply_style(app, arg);
      break;
    case C::SelectStyleColors:
      apply_style_layer(app, xanatoo::kStyleColors, arg);
      break;
    case C::SelectStyleFonts:
      apply_style_layer(app, xanatoo::kStyleFonts, arg);
      break;
    case C::ReloadStyles:
      reload_styles(app);
      break;
    case C::StyleCreate: {
      const auto pos = arg.find('\n');
      if (pos == std::string::npos) break;
      std::string err;
      if (xanatoo::create_user_style(arg.substr(0, pos), arg.substr(pos + 1), &err)) {
        reload_styles(app);
        flash_status(app, "Created style " + arg.substr(0, pos));
      } else flash_status(app, "Style: " + err);
      break;
    }
    case C::StyleSave: {
      const auto pos = arg.find('\n');
      if (pos == std::string::npos) break;
      std::string err;
      if (xanatoo::save_user_style(arg.substr(0, pos), arg.substr(pos + 1), &err)) {
        ++app.style_save_generation;
        reload_styles(app);
        flash_status(app, "Saved style " + arg.substr(0, pos));
      } else flash_status(app, "Style: " + err);
      break;
    }
    case C::StyleDelete: {
      std::string err;
      if (xanatoo::delete_user_style(arg, &err)) {
        for (auto it = app.style_pins.begin(); it != app.style_pins.end();) {
          if (it->second == arg) it = app.style_pins.erase(it);
          else ++it;
        }
        xanatoo::save_style_pins(app.style_pins);
        if (app.view_settings.style == arg) app.view_settings.style = "default";
        if (app.view_settings.style_colors == arg)
          app.view_settings.style_colors.clear();
        if (app.view_settings.style_fonts == arg)
          app.view_settings.style_fonts.clear();
        xanatoo::save_view_settings(app.view_settings);
        sync_pinned_style(app);
        reload_styles(app);
        flash_status(app, "Deleted style " + arg);
      } else flash_status(app, "Style: " + err);
      break;
    }
    case C::StylePin: {
      const auto pos = arg.find('\n');
      if (pos == std::string::npos ||
          !xanatoo::valid_style_name(arg.substr(pos + 1))) break;
      std::string key = arg.substr(0, pos);
      std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      if (!xanatoo::valid_style_pin_key(key)) {
        flash_status(app, "Style pin: use domain:host or service:name");
        break;
      }
      app.style_pins[std::move(key)] = arg.substr(pos + 1);
      if (!xanatoo::save_style_pins(app.style_pins))
        flash_status(app, "Cannot save style assignments");
      sync_pinned_style(app);
      break;
    }
    case C::StyleUnpin:
      app.style_pins.erase(arg);
      if (!xanatoo::save_style_pins(app.style_pins))
        flash_status(app, "Cannot save style assignments");
      sync_pinned_style(app);
      break;
    case C::ToggleFootnoteOverlay:
      app.view_settings.footnote_overlay = !app.view_settings.footnote_overlay;
      xanatoo::save_view_settings(app.view_settings);
      flash_status(app, app.view_settings.footnote_overlay
                            ? "Footnote overlay on"
                            : "Footnote overlay off");
      break;
    case C::ToggleBookmarkMarks:
      app.view_settings.bookmark_marks = !app.view_settings.bookmark_marks;
      xanatoo::save_view_settings(app.view_settings);
      flash_status(app, app.view_settings.bookmark_marks
                            ? "Bookmarks marked in the page"
                            : "Bookmarks not marked in the page");
      break;
    case C::ToggleHeadingTicks:
      app.view_settings.heading_ticks = !app.view_settings.heading_ticks;
      xanatoo::save_view_settings(app.view_settings);
      flash_status(app, app.view_settings.heading_ticks
                            ? "Headings marked on the scrollbar"
                            : "Headings not marked on the scrollbar");
      break;
    case C::ToggleAutoUpdates:
      app.view_settings.auto_updates = !app.view_settings.auto_updates;
      xanatoo::save_view_settings(app.view_settings);
      xanatoo::platform_auto_updates(app.view_settings.auto_updates);
      flash_status(app, app.view_settings.auto_updates
                            ? "Automatic update checks on"
                            : "Automatic update checks off");
      break;
    case C::ToggleAutoLinks:
      app.view_settings.auto_links = !app.view_settings.auto_links;
      apply_view_settings(app, true);
      flash_status(app, app.view_settings.auto_links
                            ? "Web addresses in text are links"
                            : "Web addresses in text are plain text");
      break;
    case C::ToggleAutoImages:
      app.view_settings.auto_images = !app.view_settings.auto_images;
      apply_view_settings(app, true);
      flash_status(app, app.view_settings.auto_images
                            ? "Image addresses in text show the image"
                            : "Image addresses in text show only the address");
      break;
    case C::ToggleHoverPreview:
      app.view_settings.hover_preview = !app.view_settings.hover_preview;
      xanatoo::save_view_settings(app.view_settings);
      flash_status(app, app.view_settings.hover_preview
                            ? "Link previews on hover on"
                            : "Link previews on hover off");
      break;
    case C::ToggleHistoryRewinds:
      app.view_settings.history_rewinds = !app.view_settings.history_rewinds;
      xanatoo::save_view_settings(app.view_settings);
      flash_status(app, app.view_settings.history_rewinds
                            ? "History clicks go back to the visit"
                            : "History clicks open the page as a new visit");
      break;
    case C::ToggleReaderDefault: {
      // Views that follow the default switch with it (keep_views first).
      keep_views(app);
      app.view_settings.reader_mode = !app.view_settings.reader_mode;
      xanatoo::save_view_settings(app.view_settings);
      for (Pane& v : app.panes)
        if (!v.reader) v.height_cache.clear();
      app.needs_layout = true;
      if (app.outline_mode) refresh_outline(app);
      flash_status(app, app.view_settings.reader_mode
                            ? "Reader mode is the default"
                            : "Whole pages are the default");
      break;
    }
    case C::TogglePreferences:
      app.preferences_open = !app.preferences_open;
      break;
    case C::ToggleCredentials:
      app.credentials_open = !app.credentials_open;
      break;
    case C::SaveCatalogCredentials: {
      const auto values = xanatoo::parse_catalog_credentials_command(arg);
      const bool saved = values && xanatoo::save_catalog_credentials(*values);
      flash_status(app, saved ? "Catalog credentials saved locally"
                              : "Could not save catalog credentials");
      break;
    }
    case C::ClearCatalogCredentials:
      flash_status(app, xanatoo::save_catalog_credentials({})
                            ? "Saved catalog credentials removed"
                            : "Could not remove catalog credentials");
      break;
    case C::SaveRedditCredentials: {
      const size_t split = arg.find('\n');
      const bool saved = split != std::string::npos &&
          xanatoo::save_reddit_credentials(
              {arg.substr(0, split), arg.substr(split + 1)});
      flash_status(app, saved ? "Reddit credentials saved for future requests"
                              : "Could not save Reddit credentials");
      break;
    }
    case C::ClearRedditCredentials:
      flash_status(app, xanatoo::save_reddit_credentials({})
                           ? "Saved Reddit credentials removed"
                           : "Could not remove Reddit credentials");
      break;
    case C::SetUiScale:
      try {
        app.view_settings.ui_scale = std::stof(arg);
        app.ui_scale_pending = true;
      } catch (...) {
      }
      break;
    case C::ToggleJustify:
      app.view_settings.justify = !app.view_settings.justify;
      apply_view_settings(app);
      if (app.view_settings.justify) {
        if (app.view_settings.measure_chars == 0)
          flash_status(
              app, "Justify on — Layout→Text width 72 for book-like width");
        else
          flash_status(app, "Justify on");
      } else {
        flash_status(app, "Justify off");
      }
      break;
    default:
      break;
  }
}

void run_chrome(App& app) {
  // Content only keeps the status line while a prompt or notice needs it.
  const bool status_prompt = prompt_active(app) && !prompt_is_search(app);
  status_bar_h = app.chrome_hidden && !status_prompt &&
                         xanatoo::sdl::ticks() >= app.status_notice_until
                     ? 0
                     : status_bar_full_h;
  // ImGui controls inside floating panels use absolute screen coordinates.
  // Snap/dock frames *before* reading those coordinates: draw_floats() also
  // places windows, but doing it only there leaves the child controls at the
  // pre-snap position when a dragged/resized panel reaches a screen edge.
  place_windows(app);
  place_centre_rows(app);  // so does the find bar, below a view
  xanatoo::UiState ui_state;
  build_ui_state(app, ui_state);
  ui_state.pointer_x = ui_state.stick_active ? ui_state.stick_cx : app.mouse_x;
  ui_state.pointer_y = ui_state.stick_active ? ui_state.stick_cy : app.mouse_y;
  ui_state.pointer_away = app.pointer_away && !ui_state.stick_active;
  ui_state.pointer_native_top = float_layer_at(
      app, static_cast<int>(ui_state.pointer_x),
      static_cast<int>(ui_state.pointer_y));
  {
    bool tile_strip = false;
    for (const auto& bar : ui_state.edit_bars) tile_strip |= !bar.floating;
    for (const auto& bar : ui_state.service_bars) tile_strip |= !bar.floating;
    if (tile_strip)
      ui_state.native_panel_order.push_back(xanatoo::UiPanelLayer::EditBar);
  }
  if (ui_state.find_open && app.focus < kCentreViews)
    ui_state.native_panel_order.push_back(xanatoo::UiPanelLayer::FindBar);
  for (const FloatWindow* w : float_stack(app)) {
    const auto layer = float_ui_layer(app, *w);
    if (layer != xanatoo::UiPanelLayer::Chrome)
      ui_state.native_panel_order.push_back(layer);
  }
  auto ui_frame = app.ui.begin_frame(ui_state);
  app.ui_top = ui_frame.top_reserved;
  app.ui_bottom = ui_frame.bottom_reserved;
  place_centre_rows(app);  // the toolbar may have changed height
  if (!ui_state.find_open || app.ui.find_field_active())
    app.prompt.pending = false;
  // A tab dragged off the strip previews where it would dock.
  {
    const auto hot = ui_frame.context_block_hot;
    app.context_block_hot =
        hot >= 0 && static_cast<size_t>(hot) < app.context_blocks.size()
            ? app.context_blocks[static_cast<size_t>(hot)]
            : xanatoo::INVALID_NODE;
  }
  if (ui_frame.tab_drag_out) {
    app.dock_preview = compute_dock_zone(app, static_cast<int>(app.mouse_x),
                                         static_cast<int>(app.mouse_y));
    app.tab_drag_preview = true;
  } else if (app.tab_drag_preview) {
    app.tab_drag_preview = false;
    app.dock_preview = Dock::None;
  }
  // A tool on another view's edit strip acts on that view.
  if (ui_frame.edit_bar_view >= 0 && ui_frame.edit_bar_view != app.focus &&
      view_live(app, ui_frame.edit_bar_view)) {
    focus_view(app, ui_frame.edit_bar_view);
    app.key_focus = KeyFocus::Document;
  }
  execute_command(app, ui_frame.cmd, ui_frame.arg);
  // The find bar keeps the keys in its field.
  using C = xanatoo::UiCommand;
  const bool find_cmd =
      ui_frame.cmd == C::FindText || ui_frame.cmd == C::FindNext ||
      ui_frame.cmd == C::FindPrev || ui_frame.cmd == C::ReplaceOne ||
      (ui_frame.cmd == C::ReplaceAll && !ui_frame.arg.empty());
  // A tab press selects it but may turn into a drag (reorder / tear off),
  // so its mouse capture stays.
  if (ui_frame.cmd != xanatoo::UiCommand::None && !find_cmd &&
      ui_frame.cmd != xanatoo::UiCommand::SelectTab &&
      ui_frame.cmd != xanatoo::UiCommand::ReorderTabs &&
      ui_frame.cmd != xanatoo::UiCommand::EditReply &&
      ui_frame.cmd != xanatoo::UiCommand::LemmySetSearch &&
      ui_frame.cmd != xanatoo::UiCommand::MatrixSetSearch &&
      ui_frame.cmd != xanatoo::UiCommand::NostrSearchLocal &&
      // Engine checkboxes: the menu stays open to tick several.
      ui_frame.cmd != xanatoo::UiCommand::ToggleSearchEngine)
    app.ui.release_input_capture();
  update_hover_help(app);
}

void poll_gamepad_axes(App& app) {
  if (app.gamepad) {
    auto axis = [&](xanatoo::sdl::GamepadAxis a) -> float {
      Sint16 v = xanatoo::sdl::gamepad_axis(app.gamepad, a);
      const float dead = 8000.f;
      if (v > -dead && v < dead) return 0.f;
      return static_cast<float>(v) / 32767.f;
    };
    // Left stick Y: scroll focused pane (unchanged).
    float ly = axis(xanatoo::sdl::AxisLeftY);
    if (ly != 0.f) {
      note_stick_use(app);
      Pane& p = focused_pane(app);
      const float max_s = pane_max_scroll(app, p);
      p.scroll_y = std::max(0.f, std::min(max_s, p.scroll_y + ly * 18.f));
      ensure_virtual_layout(app, p);  // the band may not reach the new view
    }
    // Triggers as page up/down
    static bool lt_held = false, rt_held = false;
    float lt = axis(xanatoo::sdl::AxisLeftTrigger);
    float rt = axis(xanatoo::sdl::AxisRightTrigger);
    if (lt > 0.f || rt > 0.f) note_stick_use(app);
    if (lt > 0.5f) {
      if (!lt_held) {
        nav_page(app, true);
        lt_held = true;
      }
    } else {
      lt_held = false;
    }
    if (rt > 0.5f) {
      if (!rt_held) {
        nav_page(app, false);
        rt_held = true;
      }
    } else {
      rt_held = false;
    }
  } else {
    app.stick_cursor_on = false;
    app.stick_cursor_init = false;
  }
}

// Calls `fn(x0, x1, top, baseline)` (layout coordinates) for each line-run
// of consecutive glyphs accepted by `match`.
static bool is_reply_target(App& app, Pane& pane) {
  if (!app.reply.open || pane.buf != app.reply.buffer ||
      path_of(app, pane) != app.reply.page) return false;
  const auto& d = doc_of(app, pane);
  const auto matches = [&](xanatoo::NodeId id) {
    if (id == xanatoo::INVALID_NODE || id >= d.next_id) return false;
    const auto& n = d.get(id);
    return n.id == id && n.kind == xanatoo::NodeKind::Message &&
           n.label == app.reply.label;
  };
  if (matches(app.reply.node)) return true;
  // A fetched page may have been reparsed in the same buffer, reassigning
  // numeric node IDs. Service comment labels are the stable identity.
  if (!app.reply.label.empty())
    for (const auto& n : d.nodes)
      if (matches(n.id)) {
        app.reply.node = n.id;
        return true;
      }
  return false;
}

static const xanatoo::LayoutBox* reply_card_box(App& app, Pane& pane) {
  if (!is_reply_target(app, pane)) return nullptr;
  for (const auto& card : pane.layout.message_cards)
    if (card.box.node == app.reply.node) return &card.box;
  return nullptr;
}

template <typename Match, typename Fn>
static void for_each_link_run(App& app, const Pane& pane, Match match,
                              Fn fn) {
  float run_x0 = 0.f, run_x1 = 0.f, run_top = 0.f, run_base = 0.f,
        run_bot = 0.f;
  bool in_run = false;
  auto flush = [&]() {
    if (in_run) fn(run_x0, run_x1, run_top, run_base, run_bot);
    in_run = false;
  };
  for (const auto& pg : pane.layout.glyphs) {
    if (!match(pg)) {
      flush();
      continue;
    }
    xanatoo::Font& font = app.fonts.pick(pg.font);
    const xanatoo::Glyph& g = font.glyph(pg.codepoint);
    float adv = g.advance > 0 ? g.advance_px()
                              : static_cast<float>(g.width + 1);
    float top = pg.y - static_cast<float>(font.ascender());
    float bot = top + static_cast<float>(font.line_height());
    if (in_run && std::abs(pg.y - run_base) < 0.5f && pg.x <= run_x1 + 2.f) {
      run_x1 = pg.x + adv;
      run_top = std::min(run_top, top);
      run_bot = std::max(run_bot, bot);
      continue;
    }
    flush();
    run_x0 = pg.x;
    run_x1 = pg.x + adv;
    run_top = top;
    run_bot = bot;
    run_base = pg.y;
    in_run = true;
  }
  flush();
}

// Hover affordances, caret, focus bar, diff marks, structure overlay, drop
// indicators — everything drawn over the text inside the pane clip.
static void draw_pane_overlays(App& app, int i) {
  [[maybe_unused]] const float lh =
      static_cast<float>(app.fonts.regular.line_height());
  [[maybe_unused]] Pane& pane = app.panes[i];
  [[maybe_unused]] const int clip_y = pane.y;
  [[maybe_unused]] const float py = static_cast<float>(pane.y);
  [[maybe_unused]] const float ox = static_cast<float>(pane.x);
  [[maybe_unused]] const float draw_scroll = pane.scroll_y - py;
  // Compose target: persistent amber wash around the precise Message card,
  // independent of the caret/selection and without editing foreign content.
  if (const auto* box = reply_card_box(app, pane)) {
    app.reply.anchor_y = box->y;
    SDL_Rect r{static_cast<int>(box->x + ox),
               static_cast<int>(box->y - draw_scroll),
               static_cast<int>(box->width),
               static_cast<int>(box->height)};
    if (r.w > 0 && r.h > 0) {
      SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_BLEND);
      SDL_SetRenderDrawColor(app.sdl, 235, 160, 40, 38);
      xanatoo::sdl::fill_rect(app.sdl, &r);
      SDL_SetRenderDrawColor(app.sdl, 200, 115, 22, 220);
      xanatoo::sdl::draw_rect(app.sdl, &r);
    }
  }
  // Hover affordances for interactive elements in this pane
  {
    SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_BLEND);
    // Split trail: the left link whose target the right pane shows gets a
    // steady wash and a ▸ at the edge towards the right pane.
    if (const std::string& trail = split_trail_href(app, i); !trail.empty()) {
      const auto& tdoc = doc_of(app, pane);
      SDL_Rect last{};
      for_each_link_run(
          app, pane,
          [&](const xanatoo::PositionedGlyph& pg) {
            if (pg.kind != xanatoo::InlineKind::Link) return false;
            const xanatoo::Node& n = tdoc.get(pg.node);
            return pg.span_index < n.spans.size() &&
                   n.spans[pg.span_index].href == trail;
          },
          [&](float x0, float x1, float top, float base, float bot) {
            SDL_Rect r{static_cast<int>(x0 + ox) - 2,
                       static_cast<int>(top - draw_scroll) - 2,
                       static_cast<int>(x1 - x0) + 4,
                       static_cast<int>(bot - top) + 4};
            SDL_SetRenderDrawColor(app.sdl, 230, 160, 30, 70);
            xanatoo::sdl::fill_rect(app.sdl, &r);
            const int uy = static_cast<int>(base + 3.f - draw_scroll);
            SDL_SetRenderDrawColor(app.sdl, 200, 120, 10, 255);
            xanatoo::sdl::draw_line(app.sdl, r.x + 1, uy, r.x + r.w - 2, uy);
            last = r;
          });
      if (last.w > 0) {
        // ▸ at the pane's right edge, level with the link's last line.
        const int h = std::max(4, last.h / 3);
        const int x = pane.x + pane.w - sb_reserve(pane) - h - 3;
        const int cy = last.y + last.h / 2;
        SDL_SetRenderDrawColor(app.sdl, 200, 120, 10, 255);
        for (int k = 0; k < h; ++k)
          xanatoo::sdl::draw_line(app.sdl, x + k, cy - (h - k), x + k, cy + (h - k));
      }
    }
    // Links whose page another tab already shows: a dotted underline.
    update_open_tab_links(app, i);
    if (!pane.open_tab_links.spans.empty()) {
      const auto& spans = pane.open_tab_links.spans;
      const auto ad = xanatoo::theme::palette().accent_dark;
      for_each_link_run(
          app, pane,
          [&](const xanatoo::PositionedGlyph& pg) {
            if (pg.kind != xanatoo::InlineKind::Link) return false;
            return std::find(spans.begin(), spans.end(),
                             std::pair{pg.node, pg.span_index}) != spans.end();
          },
          [&](float x0, float x1, float, float base, float) {
            const int uy = static_cast<int>(base + 3.f - draw_scroll);
            const int a = static_cast<int>(x0 + ox), b = static_cast<int>(x1 + ox);
            SDL_SetRenderDrawColor(app.sdl, ad.r, ad.g, ad.b, 200);
            for (int x = a; x < b; x += 4)
              xanatoo::sdl::draw_line(app.sdl, x, uy + 1, std::min(x + 1, b - 1),
                                      uy + 1);
          });
    }
    // The link whose file is in the Media panel: a yellow wash (the lit
    // title-tab colours), kept while the player holds it.
    update_playing_links(app, i);
    if (!pane.playing_links.spans.empty()) {
      const auto& spans = pane.playing_links.spans;
      const auto& pal = xanatoo::theme::palette();
      for_each_link_run(
          app, pane,
          [&](const xanatoo::PositionedGlyph& pg) {
            if (pg.kind != xanatoo::InlineKind::Link) return false;
            return std::find(spans.begin(), spans.end(),
                             std::pair{pg.node, pg.span_index}) != spans.end();
          },
          [&](float x0, float x1, float top, float base, float bot) {
            SDL_Rect r{static_cast<int>(x0 + ox) - 2,
                       static_cast<int>(top - draw_scroll) - 2,
                       static_cast<int>(x1 - x0) + 4,
                       static_cast<int>(bot - top) + 4};
            SDL_SetRenderDrawColor(app.sdl, pal.tab_top.r, pal.tab_top.g,
                                   pal.tab_top.b, 80);
            xanatoo::sdl::fill_rect(app.sdl, &r);
            const int uy = static_cast<int>(base + 3.f - draw_scroll);
            SDL_SetRenderDrawColor(app.sdl, pal.tab_bottom.r, pal.tab_bottom.g,
                                   pal.tab_bottom.b, 255);
            xanatoo::sdl::draw_line(app.sdl, r.x + 1, uy, r.x + r.w - 2, uy);
            xanatoo::sdl::draw_line(app.sdl, r.x + 1, uy + 1, r.x + r.w - 2,
                                    uy + 1);
          });
    }
    // A link just clicked whose page is still loading: a pulsing wash until
    // the new page replaces this one (or the fetch fails / is stopped).
    if (const NavLink* pend = pending_nav_link(app, i)) {
      const xanatoo::Document& d = doc_of(app, pane);
      const bool exact = pend->node != xanatoo::INVALID_NODE &&
                         pend->node < d.next_id &&
                         d.get(pend->node).id == pend->node &&
                         pend->span < d.get(pend->node).spans.size() &&
                         d.get(pend->node).spans[pend->span].href == pend->href;
      const auto accent = xanatoo::theme::palette().accent;
      const float phase = static_cast<float>(xanatoo::sdl::ticks() % 1200) /
                          1200.f * 6.2831853f;
      const Uint8 fill_a =
          static_cast<Uint8>(85.f + 45.f * std::sin(phase));
      for_each_link_run(
          app, pane,
          [&](const xanatoo::PositionedGlyph& pg) {
            if (pg.kind != xanatoo::InlineKind::Link ||
                pg.node == xanatoo::INVALID_NODE) return false;
            if (exact)
              return pg.node == pend->node && pg.span_index == pend->span;
            const xanatoo::Node& n = d.get(pg.node);
            return pg.span_index < n.spans.size() &&
                   n.spans[pg.span_index].href == pend->href;
          },
          [&](float x0, float x1, float top, float base, float bot) {
            SDL_Rect r{static_cast<int>(x0 + ox) - 2,
                       static_cast<int>(top - draw_scroll) - 2,
                       static_cast<int>(x1 - x0) + 4,
                       static_cast<int>(bot - top) + 4};
            SDL_SetRenderDrawColor(app.sdl, accent.r, accent.g, accent.b,
                                   fill_a);
            xanatoo::sdl::fill_rect(app.sdl, &r);
            const int uy = static_cast<int>(base + 3.f - draw_scroll);
            SDL_SetRenderDrawColor(app.sdl, accent.r, accent.g, accent.b, 255);
            xanatoo::sdl::draw_line(app.sdl, r.x + 1, uy, r.x + r.w - 2, uy);
            xanatoo::sdl::draw_line(app.sdl, r.x + 1, uy + 1, r.x + r.w - 2,
                                    uy + 1);
          });
    }
    // Ordinary Back/Forward: wash the precise link which led to the next
    // history stop. It is navigation state, not a document style or edit.
    NavLink split_next;
    const NavLink* next = forward_nav_link(app, i);
    if (!next) {
      split_next.href = split_history_forward_href(app, i);
      if (!split_next.href.empty()) next = &split_next;
    }
    if (next) {
      const xanatoo::Document& d = doc_of(app, pane);
      const bool exact = next->node != xanatoo::INVALID_NODE &&
                         next->node < d.next_id &&
                         d.get(next->node).id == next->node &&
                         next->span < d.get(next->node).spans.size() &&
                         d.get(next->node).spans[next->span].href == next->href;
      const auto accent = xanatoo::theme::palette().accent;
      for_each_link_run(
          app, pane,
          [&](const xanatoo::PositionedGlyph& pg) {
            if (pg.kind != xanatoo::InlineKind::Link ||
                pg.node == xanatoo::INVALID_NODE) return false;
            if (exact)
              return pg.node == next->node && pg.span_index == next->span;
            const xanatoo::Node& n = d.get(pg.node);
            return pg.span_index < n.spans.size() &&
                   n.spans[pg.span_index].href == next->href;
          },
          [&](float x0, float x1, float top, float base, float bot) {
            SDL_Rect r{static_cast<int>(x0 + ox) - 2,
                       static_cast<int>(top - draw_scroll) - 2,
                       static_cast<int>(x1 - x0) + 4,
                       static_cast<int>(bot - top) + 4};
            SDL_SetRenderDrawColor(app.sdl, accent.r, accent.g, accent.b, 58);
            xanatoo::sdl::fill_rect(app.sdl, &r);
            const int uy = static_cast<int>(base + 3.f - draw_scroll);
            SDL_SetRenderDrawColor(app.sdl, accent.r, accent.g, accent.b, 220);
            xanatoo::sdl::draw_line(app.sdl, r.x + 1, uy, r.x + r.w - 2, uy);
          });
    }
    // Bookmarks inside the page, under the `#` passage of this view's
    // address (a bookmark just opened is that passage, washed once).
    update_bookmark_marks(app, pane);
    update_annotation_marks(app, pane);
    // Not drawn while an edit waits for them to be found again: the old
    // offsets would wash the wrong text.
    const bool bm_fresh = bookmark_marks_fresh(pane);
    for (const BookmarkMark& bm : pane.bookmark_marks.items) {
      if (!bm_fresh) break;
      const xanatoo::Selection& m = bm.range;
      if (m.empty()) continue;
      if (std::find_if(pane.marks.begin(), pane.marks.end(),
                       [&](const xanatoo::Selection& o) {
                         return o.anchor == m.anchor && o.focus == m.focus;
                       }) != pane.marks.end())
        continue;
      const xanatoo::Rgba& c = app.style.bookmark;
      const auto rects = xanatoo::selection_rects(
          pane.layout, m, draw_scroll, app.renderer.glyph_extents());
      app.renderer.draw_selection(rects, ox, c.r, c.g, c.b, c.a);
    }
    if (annotation_marks_fresh(pane)) {
      // Independent layers compose over the source; they never insert spans
      // or otherwise modify the document tree.
      const xanatoo::Rgba& c = app.style.bookmark;
      for (const AnnotationMark& mark : pane.annotation_marks.items) {
        if (mark.range.empty()) continue;
        const auto rects = xanatoo::selection_rects(
            pane.layout, mark.range, draw_scroll,
            app.renderer.glyph_extents());
        app.renderer.draw_selection(rects, ox, c.r, c.g, c.b, c.a);
        if (mark.annotation_id == app.selected_annotation_id)
          app.renderer.draw_selection(rects, ox, c.r, c.g, c.b, c.a);
      }
      // Note tabs are independently hit-tested; their geometry is shared
      // with app_events so clicking the tab opens the note editor.
      for (const AnnotationIcon& icon : annotation_icons(app, pane)) {
        const AnnotationMark& mark = pane.annotation_marks.items[icon.item];
        const bool hot = app.hover.annotation_pane == i &&
                         app.hover.annotation == mark.annotation_id;
        const int x = static_cast<int>(ox + icon.x);
        const int y = static_cast<int>(py + icon.y);
        const SDL_Rect tab{x, y, static_cast<int>(icon.w),
                           static_cast<int>(icon.h - 2.f)};
        SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(app.sdl, c.r, c.g, c.b, hot ? 255 : 220);
        xanatoo::sdl::fill_rect(app.sdl, &tab);
        xanatoo::sdl::draw_line(app.sdl, x + 5, y + 8, x + 7, y + 10);
      }
    }
    for (const BookmarkMark& bm : pane.bookmark_marks.items) {
      if (!bm_fresh) break;
      if (!bm.range.empty()) continue;
      // A reading position: a bar with a tab at its top, like a ribbon.
      const auto cp = xanatoo::caret_screen_pos(pane.layout, bm.range.focus,
                                                draw_scroll, lh);
      if (!cp.valid) continue;
      const xanatoo::Rgba& c = app.style.bookmark;
      SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_BLEND);
      SDL_SetRenderDrawColor(app.sdl, c.r, c.g, c.b, 220);
      const int x = static_cast<int>(cp.x + ox);
      const int top = static_cast<int>(cp.y - cp.height * 0.8f);
      const SDL_Rect bar{x - 1, top, 3, static_cast<int>(cp.height)};
      const SDL_Rect tab{x - 4, top - 4, 9, 5};
      xanatoo::sdl::fill_rect(app.sdl, &bar);
      xanatoo::sdl::fill_rect(app.sdl, &tab);
    }
    // A ribbon in the margin per bookmark: says what the wash is and takes
    // the clicks (select, right-click menu).
    for (const BookmarkIcon& ic : bookmark_icons(app, pane)) {
      const bool hot = app.hover.bookmark_pane == i &&
                       app.hover.bookmark ==
                           pane.bookmark_marks.items[ic.item].address;
      const xanatoo::Rgba& c = app.style.bookmark;
      const int x = static_cast<int>(ox + ic.x);
      const int y = static_cast<int>(py + ic.y);
      const int w = static_cast<int>(ic.w), h = static_cast<int>(ic.h);
      const auto shade = [&](int v) { return static_cast<Uint8>(v * 3 / 4); };
      SDL_SetRenderDrawColor(app.sdl, hot ? shade(c.r) : c.r,
                             hot ? shade(c.g) : c.g, hot ? shade(c.b) : c.b,
                             hot ? 255 : 210);
      // Body down to the notch, then two legs narrowing to its tip.
      const int notch = std::max(3, w / 2);
      const SDL_Rect body{x, y, w, h - notch};
      xanatoo::sdl::fill_rect(app.sdl, &body);
      for (int k = 0; k < notch; ++k) {
        const int yy = y + h - notch + k;
        const int leg = std::max(1, w / 2 - k);
        xanatoo::sdl::draw_line(app.sdl, x, yy, x + leg - 1, yy);
        xanatoo::sdl::draw_line(app.sdl, x + w - leg, yy, x + w - 1, yy);
      }
    }
    // The `#` passage of this view's address.
    for (const xanatoo::Selection& m : pane.marks) {
      const xanatoo::Rgba& c = app.style.mark;
      const auto rects = xanatoo::selection_rects(
          pane.layout, m, draw_scroll, app.renderer.glyph_extents());
      app.renderer.draw_selection(rects, ox, c.r, c.g, c.b, c.a);
    }
    // Quote links: underlined originals, margin arrows, the jumped link.
    draw_quote_marks(app, i, ox, py, draw_scroll);
    // ... and its target on the right: the same wash and a ◂ at the edge
    // towards the left pane.
    if (const xanatoo::NodeId target = split_trail_target(app, i);
        target != xanatoo::INVALID_NODE) {
      SDL_Rect first{};
      for_each_link_run(
          app, pane,
          [&](const xanatoo::PositionedGlyph& pg) { return pg.node == target; },
          [&](float x0, float x1, float top, float, float bot) {
            SDL_Rect r{static_cast<int>(x0 + ox) - 2,
                       static_cast<int>(top - draw_scroll) - 2,
                       static_cast<int>(x1 - x0) + 4,
                       static_cast<int>(bot - top) + 4};
            SDL_SetRenderDrawColor(app.sdl, 230, 160, 30, 70);
            xanatoo::sdl::fill_rect(app.sdl, &r);
            if (first.w == 0) first = r;
          });
      if (first.w > 0) {
        const int h = std::max(4, first.h / 3);
        const int x = pane.x + 3;
        const int cy = first.y + first.h / 2;
        SDL_SetRenderDrawColor(app.sdl, 200, 120, 10, 255);
        for (int k = 0; k < h; ++k)
          xanatoo::sdl::draw_line(app.sdl, x + h - 1 - k, cy - (h - k),
                             x + h - 1 - k, cy + (h - k));
      }
    }
    // Link / cite run: soft wash + thicker underline
    if (app.hover.link_pane == i && !app.hover.link_in_footnotes &&
        app.hover.link_node != xanatoo::INVALID_NODE &&
        !app.hover.href.empty()) {
      for_each_link_run(
          app, pane,
          [&](const xanatoo::PositionedGlyph& pg) {
            return pg.node == app.hover.link_node &&
                   pg.span_index == app.hover.link_span &&
                   pg.auto_link == app.hover.link_auto &&
                   (pg.kind == xanatoo::InlineKind::Link ||
                    pg.kind == xanatoo::InlineKind::Cite ||
                    pg.kind == xanatoo::InlineKind::FootnoteRef);
          },
          [&](float x0, float x1, float top, float base, float bot) {
            int x = static_cast<int>(x0 + ox) - 2;
            int y = static_cast<int>(top - draw_scroll) - 2;
            int w = static_cast<int>(x1 - x0) + 4;
            int h = static_cast<int>(bot - top) + 4;
            SDL_SetRenderDrawColor(app.sdl, 40, 110, 220, 45);
            SDL_Rect r{x, y, w, h};
            xanatoo::sdl::fill_rect(app.sdl, &r);
            int uy = static_cast<int>(base + 3.f - draw_scroll);
            SDL_SetRenderDrawColor(app.sdl, 20, 80, 190, 255);
            xanatoo::sdl::draw_line(app.sdl, x + 1, uy, x + w - 2, uy);
            xanatoo::sdl::draw_line(app.sdl, x + 1, uy + 1, x + w - 2, uy + 1);
          });
    }
    // Fold marker wash (list item, stretch)
    {
      const xanatoo::NodeId hot =
          app.hover.list_fold_pane == i ? app.hover.list_fold
          : app.hover.stretch_pane == i ? app.hover.stretch
                                        : xanatoo::INVALID_NODE;
      SDL_SetRenderDrawColor(app.sdl, 40, 130, 90, 50);
      for (const auto& box : pane.layout.fold_marks) {
        if (hot == xanatoo::INVALID_NODE || box.node != hot) continue;
        SDL_Rect r{pane.x + static_cast<int>(box.x) - 2,
                   static_cast<int>(box.y - pane.scroll_y + py),
                   static_cast<int>(box.width) + 4,
                   static_cast<int>(box.height)};
        xanatoo::sdl::fill_rect(app.sdl, &r);
      }
    }
    // Image outline + emphasized resize handle
    if (app.hover.image_pane == i && app.hover.image != xanatoo::INVALID_NODE) {
      for (const auto& pi : pane.layout.images) {
        if (pi.node != app.hover.image) continue;
        if (pi.thumb > 0) break;  // collection card: hand cursor only
        int ix = pane.x + static_cast<int>(pi.x);
        int iy = static_cast<int>(pi.y - pane.scroll_y + py);
        int iw = static_cast<int>(pi.width);
        int ih = static_cast<int>(pi.height);
        SDL_SetRenderDrawColor(app.sdl, 25, 90, 170, 180);
        SDL_Rect hover_rect{ix, iy, iw, ih};
        xanatoo::sdl::draw_rect(app.sdl, &hover_rect);
        const int hs = app.hover.image_handle ? 14 : 10;
        SDL_Rect handle{ix + iw - hs, iy + ih - hs, hs, hs};
        SDL_SetRenderDrawColor(app.sdl, app.hover.image_handle ? 15 : 25,
                               app.hover.image_handle ? 70 : 90,
                               app.hover.image_handle ? 200 : 170, 255);
        xanatoo::sdl::fill_rect(app.sdl, &handle);
        if (app.hover.image_handle) {
          SDL_SetRenderDrawColor(app.sdl, 255, 255, 255, 220);
          xanatoo::sdl::draw_rect(app.sdl, &handle);
        }
        break;
      }
    }
    // Column gutter highlight
    if (app.hover.col_gap_pane == i && app.hover.col_gap) {
      float mx = app.mouse_x - static_cast<float>(pane.x);
      float my = content_y(app, app.mouse_y, i);
      for (const auto& gap : pane.layout.column_gaps) {
        float gx = gap.x;
        float gy0 = gap.y - pane.scroll_y;
        float gy1 = gy0 + gap.height;
        if (mx >= gx - 5.f && mx <= gx + 5.f && my >= gy0 && my <= gy1) {
          int gx_i = pane.x + static_cast<int>(gx);
          int gy_i = static_cast<int>(gy0 + py);
          int gh = static_cast<int>(gap.height);
          SDL_SetRenderDrawColor(app.sdl, 25, 90, 170, 90);
          SDL_Rect wide{gx_i - 4, gy_i, 9, gh};
          xanatoo::sdl::fill_rect(app.sdl, &wide);
          SDL_SetRenderDrawColor(app.sdl, 25, 90, 170, 255);
          SDL_Rect bar{gx_i - 1, gy_i, 3, gh};
          xanatoo::sdl::fill_rect(app.sdl, &bar);
          break;
        }
      }
    }
    // Task checkbox zone
    if (app.hover.task_pane == i && app.hover.task != xanatoo::INVALID_NODE) {
      for (const auto& box : pane.layout.boxes) {
        if (box.node != app.hover.task) continue;
        int x = pane.x + static_cast<int>(box.x);
        int y = static_cast<int>(box.y - pane.scroll_y + py);
        int h = static_cast<int>(box.height);
        SDL_SetRenderDrawColor(app.sdl, 40, 140, 80, 55);
        SDL_Rect r{x, y, 26, h};
        xanatoo::sdl::fill_rect(app.sdl, &r);
        SDL_SetRenderDrawColor(app.sdl, 30, 120, 70, 220);
        xanatoo::sdl::draw_rect(app.sdl, &r);
        break;
      }
    }
    // Block drag grip zone (drawn in the left gutter, not over text)
    if (app.hover.grip_pane == i && app.hover.grip != xanatoo::INVALID_NODE) {
      for (const auto& box : pane.layout.boxes) {
        if (box.node != app.hover.grip) continue;
        int gw = static_cast<int>(kBlockGripW);
        int x = pane.x + static_cast<int>(box.x) - gw;
        int y = static_cast<int>(box.y - pane.scroll_y + py);
        int h = static_cast<int>(box.height);
        SDL_SetRenderDrawColor(app.sdl, 80, 120, 180, 50);
        SDL_Rect r{x, y, gw, h};
        xanatoo::sdl::fill_rect(app.sdl, &r);
        // Stronger grip dots centered in the gutter
        int gx = x + gw / 2 - 2;
        int gy = y + h / 2;
        SDL_SetRenderDrawColor(app.sdl, 40, 90, 160, 255);
        for (int d = -6; d <= 6; d += 6) {
          SDL_Rect dot{gx, gy + d, 4, 4};
          xanatoo::sdl::fill_rect(app.sdl, &dot);
        }
        break;
      }
    }
    // The block a context-menu "Open …" item would cut the view to.
    if (app.context_block_hot != xanatoo::INVALID_NODE &&
        app.context_blocks_pane == i && pane.buf == app.context_blocks_buf) {
      for (const auto& box : pane.layout.boxes) {
        if (box.node != app.context_block_hot) continue;
        SDL_Rect r{pane.x + static_cast<int>(box.x) - 4,
                   static_cast<int>(box.y - pane.scroll_y + py) - 2,
                   static_cast<int>(box.width) + 8,
                   static_cast<int>(box.height) + 4};
        SDL_SetRenderDrawColor(app.sdl, 80, 120, 180, 36);
        xanatoo::sdl::fill_rect(app.sdl, &r);
        SDL_SetRenderDrawColor(app.sdl, 60, 100, 170, 200);
        xanatoo::sdl::draw_rect(app.sdl, &r);
        break;
      }
    }
    // Transclude region
    if (app.hover.transclude_pane == i &&
        app.hover.transclude != xanatoo::INVALID_NODE) {
      for (const auto& box : pane.layout.boxes) {
        if (box.node != app.hover.transclude) continue;
        int x = pane.x + static_cast<int>(box.x);
        int y = static_cast<int>(box.y - pane.scroll_y + py);
        int w = static_cast<int>(box.width);
        int h = static_cast<int>(box.height);
        SDL_SetRenderDrawColor(app.sdl, 180, 120, 40, 40);
        SDL_Rect r{x, y, w, h};
        xanatoo::sdl::fill_rect(app.sdl, &r);
        SDL_SetRenderDrawColor(app.sdl, 180, 120, 40, 200);
        xanatoo::sdl::draw_rect(app.sdl, &r);
        break;
      }
    }
    SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_NONE);
  }

  // Logical structure overlay: nested outlined boxes by NodeKind + small corner tags
  if (app.structure_mode) {
    SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_BLEND);
    const auto& ddoc = buf(app, pane.buf).doc;
    auto kind_label = [&ddoc](const xanatoo::Node& n) -> const char* {
      using K = xanatoo::NodeKind;
      switch (n.kind) {
        case K::Heading: {
          static thread_local char buf[4];
          int lv = n.level;
          if (lv < 1) lv = 1;
          if (lv > 6) lv = 6;
          buf[0] = 'h';
          buf[1] = static_cast<char>('0' + lv);
          buf[2] = 0;
          return buf;
        }
        case K::Header: {
          // Show interop depth (org stars / ATX) or section nesting.
          static thread_local char buf[12];
          int lv = n.level;
          if (lv < 1) {
            xanatoo::NodeId pid = n.parent;
            while (pid != xanatoo::INVALID_NODE && pid < ddoc.next_id &&
                   static_cast<size_t>(pid - 1) < ddoc.nodes.size()) {
              const auto& pn = ddoc.get(pid);
              if (pn.kind == K::Section || pn.kind == K::Article) ++lv;
              pid = pn.parent;
            }
          }
          if (lv < 1) lv = 1;
          if (lv > 9) lv = 9;
          buf[0] = 'h';
          buf[1] = 'e';
          buf[2] = 'a';
          buf[3] = 'd';
          buf[4] = 'e';
          buf[5] = 'r';
          buf[6] = static_cast<char>('0' + lv);
          buf[7] = 0;
          return buf;
        }
        case K::Article:
          return "article";
        case K::Section:
          return "section";
        case K::Paragraph:
          return "para";
        case K::List:
          return n.level == 1 ? "ol" : "ul";
        case K::ListItem:
          if (n.label == "task") return "task";
          if (n.label == "task-done") return "done";
          return "li";
        case K::BlockQuote:
          return "quote";
        case K::Note:
          return "note";
        case K::CodeBlock:
          return "code";
        case K::Table:
          return "table";
        case K::TableRow:
          return "row";
        case K::TableCell:
          return n.level > 0 ? "th" : "td";
        case K::DefList:
          return "deflist";
        case K::Term:
          return "term";
        case K::Definition:
          return "def";
        case K::Stretch:
          return "stretch";
        case K::Summary:
          return "summary";
        case K::Columns:
          return "columns";
        case K::Column:
          return "column";
        case K::Image:
          return "image";
        case K::Figure:
          return "figure";
        case K::Collection:
          return "collection";
        case K::Caption:
          return "caption";
        case K::Biblio:
          return "biblio";
        case K::BiblioEntry:
          return "ref";
        case K::HorizontalRule:
          return "hr";
        case K::Transclude:
          return "include";
        case K::Thread:
          return "thread";
        case K::Message:
          return "message";
        default:
          return nullptr;
      }
    };
    // Small chrome-face tags sit inside each box's top-left corner, so they
    // follow the nesting indent instead of piling up at the right edge.
    xanatoo::Font& tag_font =
        app.ui_font_tiny.loaded() ? app.ui_font_tiny : app.ui_font;
    const int tag_lh = tag_font.line_height();
    // Nesting depth: parent chain up to the Document.
    auto depth_of = [&ddoc](xanatoo::NodeId id) {
      int d = 0;
      for (xanatoo::NodeId pid = ddoc.get(id).parent;
           pid != xanatoo::INVALID_NODE && pid < ddoc.next_id &&
           static_cast<size_t>(pid - 1) < ddoc.nodes.size();
           pid = ddoc.get(pid).parent)
        ++d;
      return d;
    };
    for (const auto& box : pane.layout.boxes) {
      if (box.node == xanatoo::INVALID_NODE) continue;
      const auto& node = ddoc.get(box.node);
      const auto kind = node.kind;
      // Skip pure container shells that span the whole doc
      if (kind == xanatoo::NodeKind::Document) continue;
      uint8_t r = 120, g = 120, b = 130, a_bar = 170, a_wash = 22;
      switch (kind) {
        case xanatoo::NodeKind::Heading:
        case xanatoo::NodeKind::Header:
          r = 40;
          g = 90;
          b = 180;
          a_wash = 36;
          break;
        case xanatoo::NodeKind::Article:
          r = 60;
          g = 100;
          b = 160;
          a_wash = 30;
          break;
        case xanatoo::NodeKind::Section:
          r = 70;
          g = 110;
          b = 170;
          a_wash = 26;
          break;
        case xanatoo::NodeKind::Paragraph:
          r = 100;
          g = 110;
          b = 120;
          a_wash = 18;
          break;
        case xanatoo::NodeKind::List:
        case xanatoo::NodeKind::ListItem:
          r = 50;
          g = 140;
          b = 90;
          break;
        case xanatoo::NodeKind::BlockQuote:
          r = 140;
          g = 155;
          b = 180;
          break;
        case xanatoo::NodeKind::Note:
          r = 200;
          g = 140;
          b = 40;
          break;
        case xanatoo::NodeKind::CodeBlock:
          r = 160;
          g = 100;
          b = 50;
          a_wash = 40;
          break;
        case xanatoo::NodeKind::Table:
        case xanatoo::NodeKind::TableRow:
        case xanatoo::NodeKind::TableCell:
          r = 80;
          g = 120;
          b = 160;
          break;
        case xanatoo::NodeKind::DefList:
        case xanatoo::NodeKind::Term:
        case xanatoo::NodeKind::Definition:
          r = 120;
          g = 80;
          b = 160;
          break;
        case xanatoo::NodeKind::Stretch:
        case xanatoo::NodeKind::Summary:
          r = 180;
          g = 80;
          b = 120;
          break;
        case xanatoo::NodeKind::Columns:
        case xanatoo::NodeKind::Column:
          r = 70;
          g = 130;
          b = 150;
          break;
        case xanatoo::NodeKind::Image:
        case xanatoo::NodeKind::Figure:
        case xanatoo::NodeKind::Caption:
          r = 150;
          g = 70;
          b = 100;
          break;
        case xanatoo::NodeKind::Biblio:
        case xanatoo::NodeKind::BiblioEntry:
          r = 90;
          g = 50;
          b = 140;
          break;
        case xanatoo::NodeKind::HorizontalRule:
          r = 160;
          g = 160;
          b = 160;
          break;
        case xanatoo::NodeKind::Transclude:
          r = 180;
          g = 120;
          b = 40;
          break;
        case xanatoo::NodeKind::Thread:
        case xanatoo::NodeKind::Message:
          r = 40;
          g = 130;
          b = 140;
          a_wash = 32;
          break;
        default:
          break;
      }
      int by = static_cast<int>(box.y - pane.scroll_y + py);
      int bh = static_cast<int>(box.height);
      if (by + bh < clip_y || by > app.win_h - status_bar_h) continue;
      // Each box is outlined at its own geometry; outer boxes grow outward
      // by 2 px per level above the deepest (capped), so every box visibly
      // encloses its children.
      const int depth = depth_of(box.node);
      const int grow = 2 * (6 - std::min(depth, 6));
      const int bx = pane.x + static_cast<int>(box.x) + 0;
      int bw = static_cast<int>(box.width);
      if (bw < 4) continue;
      SDL_Rect frame{std::max(bx - grow, pane.x + 1), by, bw + 2 * grow, bh};
      if (frame.x + frame.w > pane.x + pane.w - 12)
        frame.w = pane.x + pane.w - 12 - frame.x;
      SDL_SetRenderDrawColor(app.sdl, r, g, b, a_wash);
      xanatoo::sdl::fill_rect(app.sdl, &frame);
      SDL_SetRenderDrawColor(app.sdl, r, g, b, a_bar);
      xanatoo::sdl::draw_rect(app.sdl, &frame);
      SDL_Rect mark{frame.x, by, 3, bh};
      xanatoo::sdl::fill_rect(app.sdl, &mark);

      // Tag in the top-left corner, clipped to the box width. Plain
      // paragraphs and table-row shells stay untagged (the frame is enough).
      const char* tag = kind_label(node);
      if (tag && kind != xanatoo::NodeKind::TableRow &&
          kind != xanatoo::NodeKind::Paragraph && bh >= tag_lh &&
          tag_font.loaded()) {
        int tw = tag_font.measure(tag);
        if (tw + 8 > bw) continue;
        SDL_SetRenderDrawColor(app.sdl, 248, 246, 240, 215);
        SDL_Rect chip{frame.x + 3, by + 1, tw + 4, std::min(bh - 2, tag_lh)};
        if (chip.h > 4) xanatoo::sdl::fill_rect(app.sdl, &chip);
        float baseline = static_cast<float>(chip.y + chip.h) - 3.f;
        if (baseline < static_cast<float>(clip_y) + 8.f)
          baseline = static_cast<float>(clip_y) + 8.f;
        app.renderer.draw_text(tag_font, static_cast<float>(chip.x + 2),
                               baseline, tag, r, g, b);
      }
    }
    SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_NONE);
  }

  if (i == app.focus) {
    auto cpos =
        xanatoo::caret_screen_pos(pane.layout, pane.sel.focus, draw_scroll, lh);
    if (cpos.valid) pane.preferred_x = cpos.x;
    // Browse mode keeps the caret implicit (selection anchor / focus,
    // Space / Enter target) and draws none. Edit mode: blinking caret.
    if (!browsing(app) &&
        ((xanatoo::sdl::ticks() - app.caret_blink_start) % 1000) < 600) {
      app.renderer.draw_caret(cpos, ox);
    }
  }

  // Focus indicator: thin bar at top of focused pane content
  if (i == app.focus) {
    SDL_SetRenderDrawColor(app.sdl, 25, 90, 170, 255);
    SDL_Rect bar{pane.x, clip_y, pane.w, 3};
    xanatoo::sdl::fill_rect(app.sdl, &bar);
  }

  // Version-diff marks (orange bars on changed text blocks); the diff is
  // between the two centre views.
  if (app.diff_mode && !shared_doc(app) && i < kCentreViews) {
    const auto& marks = app.diff_marks[i];
    if (!marks.empty()) {
      SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_BLEND);
      for (const auto& box : pane.layout.boxes) {
        bool hit = false;
        for (xanatoo::NodeId id : marks) {
          if (id == box.node) {
            hit = true;
            break;
          }
        }
        if (!hit) continue;
        // Content → screen: same convention as structure overlay (+ ui_top).
        int by = static_cast<int>(box.y - pane.scroll_y + py);
        int bh = static_cast<int>(box.height);
        if (by + bh < clip_y || by > app.win_h - status_bar_h) continue;
        SDL_SetRenderDrawColor(app.sdl, 200, 120, 40, 200);
        SDL_Rect mark{pane.x + 2, by, 4, bh};
        xanatoo::sdl::fill_rect(app.sdl, &mark);
        SDL_SetRenderDrawColor(app.sdl, 200, 120, 40, 40);
        SDL_Rect wash{pane.x, by, pane.w - 6, bh};
        xanatoo::sdl::fill_rect(app.sdl, &wash);
      }
      SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_NONE);
    }
    // Intra-block word-level highlights for modified (similar) blocks
    const auto& words = app.diff_words[i];
    if (!words.empty()) {
      SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_BLEND);
      const auto& ddoc = buf(app, pane.buf).doc;
      for (const auto& g : pane.layout.glyphs) {
        if (g.node == xanatoo::INVALID_NODE) continue;
        // Map glyph to plain-text character index within its node
        size_t gch = 0;
        {
          const auto& gn = ddoc.get(g.node);
          for (size_t si = 0; si < g.span_index && si < gn.spans.size(); ++si) {
            for (size_t ci = 0; ci < gn.spans[si].text.size();) {
              unsigned char c =
                  static_cast<unsigned char>(gn.spans[si].text[ci]);
              if (c < 0x80)
                ++ci;
              else if ((c & 0xe0) == 0xc0)
                ci += 2;
              else if ((c & 0xf0) == 0xe0)
                ci += 3;
              else if ((c & 0xf8) == 0xf0)
                ci += 4;
              else
                ++ci;
              ++gch;
            }
          }
          gch += g.char_index;
        }
        bool hit = false;
        for (const auto& wm : words) {
          if (wm.node == g.node && gch >= wm.char_begin && gch < wm.char_end) {
            hit = true;
            break;
          }
        }
        if (!hit) continue;
        int gx = pane.x + static_cast<int>(g.x);
        int gy =
            static_cast<int>(g.y - pane.scroll_y + py - lh * 0.85f);
        // Width: advance to next glyph on the same baseline/node when possible.
        int gw = 8;
        {
          const float baseline = g.y;
          float next_x = g.x + 8.f;
          for (const auto& ng : pane.layout.glyphs) {
            if (ng.node != g.node) continue;
            if (std::abs(ng.y - baseline) > 1.f) continue;
            if (ng.x > g.x + 0.5f && ng.x < next_x) next_x = ng.x;
          }
          int adv = static_cast<int>(next_x - g.x);
          if (adv > 2 && adv < 48) gw = adv;
        }
        if (gy + static_cast<int>(lh) < clip_y || gy > app.win_h - status_bar_h)
          continue;
        SDL_SetRenderDrawColor(app.sdl, 220, 160, 40, 110);
        SDL_Rect r{gx, gy, gw, static_cast<int>(lh)};
        xanatoo::sdl::fill_rect(app.sdl, &r);
      }
      SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_NONE);
    }
  }

  // Column gutters (subtle vertical handles). A table's rule already shows
  // where its columns meet: its handle only appears while being dragged.
  const xanatoo::Document& gap_doc = doc_of(app, pane);
  for (const auto& gap : pane.layout.column_gaps) {
    int gx = pane.x + static_cast<int>(gap.x);
    int gy = static_cast<int>(gap.y - pane.scroll_y + py);
    int gh = static_cast<int>(gap.height);
    bool active = app.dragging_col_gap && gap.left == app.col_gap_left;
    if (!active && gap.columns != xanatoo::INVALID_NODE &&
        gap_doc.get(gap.columns).kind == xanatoo::NodeKind::Table)
      continue;
    SDL_SetRenderDrawColor(app.sdl, active ? 25 : 190, active ? 90 : 188,
                           active ? 170 : 180, 255);
    SDL_Rect gbar{gx - 1, gy, 3, gh};
    xanatoo::sdl::fill_rect(app.sdl, &gbar);
  }

  // Block drag-drop: insertion line + source highlight
  if (app.dragging_block && i == app.focus) {
    // Dim the dragged block
    for (const auto& box : pane.layout.boxes) {
      if (box.node != app.drag_block_id) continue;
      SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_BLEND);
      SDL_SetRenderDrawColor(app.sdl, 80, 140, 220, 60);
      SDL_Rect r{pane.x + static_cast<int>(box.x),
                 static_cast<int>(box.y - pane.scroll_y + py),
                 static_cast<int>(box.width), static_cast<int>(box.height)};
      xanatoo::sdl::fill_rect(app.sdl, &r);
      SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_NONE);
    }
    if (app.drop_line_y >= 0.f) {
      int ly = static_cast<int>(app.drop_line_y - pane.scroll_y + py);
      SDL_SetRenderDrawColor(app.sdl, 25, 90, 170, 255);
      SDL_Rect line{pane.x + 24, ly - 1, pane.w - 40, 3};
      xanatoo::sdl::fill_rect(app.sdl, &line);
    }
  } else if (i == app.focus && !app.dragging_block &&
             app.hover.bookmark.empty()) {
    // Subtle grip dots in the left gutter (not over the text)
    // mouse_y is window space; boxes are content space.
    float adj_y = content_y(app, app.mouse_y, i) + pane.scroll_y;
    float lx = app.mouse_x - static_cast<float>(pane.x);
    for (const auto& box : pane.layout.boxes) {
      if (adj_y < box.y || adj_y > box.y + box.height) continue;
      if (lx < box.x - kBlockGripW || lx >= box.x) continue;
      const auto& bn = doc_of(app, pane).get(box.node);
      if (bn.kind == xanatoo::NodeKind::Document ||
          bn.kind == xanatoo::NodeKind::Columns ||
          bn.kind == xanatoo::NodeKind::Column ||
          bn.kind == xanatoo::NodeKind::List)
        continue;
      int gx = pane.x + static_cast<int>(box.x) -
               static_cast<int>(kBlockGripW) / 2 - 1;
      int gy = static_cast<int>(box.y - pane.scroll_y + py +
                                box.height * 0.5f);
      SDL_SetRenderDrawColor(app.sdl, 160, 158, 150, 255);
      for (int d = -6; d <= 6; d += 6) {
        SDL_Rect dot{gx, gy + d, 3, 3};
        xanatoo::sdl::fill_rect(app.sdl, &dot);
      }
      break;
    }
  }
}

// Section / heading positions of a pane's document for the scrollbar.
// Every block the layout skipped still has a stub box, so only the inside
// of a stub is estimated (from height hints); nodes with no box outside a
// stub were not laid out at all (section focus) and get no mark.
// `block_y`, when given, gets the (estimated) top of every block on the way.
static std::vector<SectionMark> build_section_marks(
    App& app, Pane& pane,
    std::unordered_map<xanatoo::NodeId, float>* block_y) {
  std::vector<SectionMark> marks;
  const auto& d = doc_of(app, pane);
  if (d.root == xanatoo::INVALID_NODE || d.root >= d.next_id) return marks;
  std::unordered_map<xanatoo::NodeId, const xanatoo::LayoutBox*> box_of;
  box_of.reserve(pane.layout.boxes.size());
  for (const auto& b : pane.layout.boxes)
    if (b.node != xanatoo::INVALID_NODE) box_of.emplace(b.node, &b);
  const float lh = static_cast<float>(app.fonts.regular.line_height());
  auto heading_level = [&](const xanatoo::Node& sec, int depth) {
    for (xanatoo::NodeId c : sec.children) {
      const auto& h = d.get(c);
      if (h.kind == xanatoo::NodeKind::Header ||
          h.kind == xanatoo::NodeKind::Heading)
        return h.level > 0 ? std::min(8, static_cast<int>(h.level)) : depth;
    }
    return depth;
  };
  // y: running position, only meaningful while `est` (inside a stub).
  std::function<void(xanatoo::NodeId, int, bool, float&)> visit =
      [&](xanatoo::NodeId id, int depth, bool est, float& y) {
        if (id == xanatoo::INVALID_NODE || id >= d.next_id) return;
        const auto& n = d.get(id);
        if (n.id != id) return;
        auto it = box_of.find(id);
        const xanatoo::LayoutBox* box = it != box_of.end() ? it->second : nullptr;
        if (!box && !est) {
          // Box-less container (Document, List, …) or a block the layout
          // left out: only materialised descendants count.
          float unused = 0.f;
          for (xanatoo::NodeId c : n.children) visit(c, depth, false, unused);
          return;
        }
        const float y0 = box ? box->y : y;
        if (block_y) block_y->emplace(id, y0);
        const bool sec = n.kind == xanatoo::NodeKind::Section ||
                         n.kind == xanatoo::NodeKind::Article;
        const bool parent_sec =
            n.parent != xanatoo::INVALID_NODE &&
            (d.get(n.parent).kind == xanatoo::NodeKind::Section ||
             d.get(n.parent).kind == xanatoo::NodeKind::Article);
        if (sec) {
          marks.push_back(SectionMark{y0, heading_level(n, depth + 1)});
        } else if ((n.kind == xanatoo::NodeKind::Header ||
                    n.kind == xanatoo::NodeKind::Heading) &&
                   !parent_sec) {
          const int lv = static_cast<int>(n.level);
          marks.push_back(SectionMark{y0, lv > 0 ? std::min(8, lv)
                                                 : std::max(1, depth)});
        }
        const bool folded = sec && app.folded_sections.count(id);
        float end = y0;
        if (!folded && !n.children.empty()) {
          bool stub = false;
          if (box) {
            stub = true;
            for (xanatoo::NodeId c : n.children)
              if (box_of.count(c)) {
                stub = false;
                break;
              }
          }
          float cy = y0;
          for (xanatoo::NodeId c : n.children)
            visit(c, sec ? depth + 1 : depth, est || stub, cy);
          end = cy;
        }
        if (box) {
          y = box->y + box->height;
        } else if (auto hit = pane.height_cache.find(id);
                   hit != pane.height_cache.end()) {
          y = y0 + hit->second;
        } else {
          y = end > y0 ? end : y0 + lh;
        }
      };
  float y = 0.f;
  visit(d.root, 0, false, y);
  return marks;
}

// The pane's ScrollMarks, rebuilt only for a new layout (or buffer); with
// `block_y` also the block tops.
static const ScrollMarks& section_marks(App& app, Pane& pane,
                                        bool block_y = false) {
  ScrollMarks& sm = pane.scroll_marks;
  if (sm.layout_gen != pane.layout.generation || sm.buf != pane.buf ||
      sm.layout_gen == 0) {
    sm.layout_gen = pane.layout.generation;
    sm.buf = pane.buf;
    sm.has_block_y = false;
    sm.block_y.clear();
    sm.sections = build_section_marks(app, pane, nullptr);
  }
  if (block_y && !sm.has_block_y) {
    build_section_marks(app, pane, &sm.block_y);
    sm.has_block_y = true;
  }
  return sm;
}

// Classic scrollbar in the strip excluded from the content clip.
bool canvas_hover(App& app, const FloatWindow* owner) {
  if (app.pointer_away) return false;
  const int mx = static_cast<int>(app.mouse_x);
  const int my = static_cast<int>(app.mouse_y);
  const FloatWindow* front = float_at(app, mx, my);
  if (front && front != owner) return false;
  return !app.ui.captures_point(app.mouse_x, app.mouse_y,
                                float_layer_at(app, mx, my));
}

static void draw_pane_scrollbar(App& app, int i) {
  [[maybe_unused]] const float lh =
      static_cast<float>(app.fonts.regular.line_height());
  [[maybe_unused]] Pane& pane = app.panes[i];
  const float max_scroll = pane_max_scroll(app, pane);
  // Clearlooks-style full-height track, chapter/search markers and thumb.
  {
    const float view_h = std::max(1.f, static_cast<float>(pane.h));
    // Include the bottom reading pad represented by pane_max_scroll, so a
    // short page with a small scroll range still has a draggable thumb.
    float content_h = std::max(view_h + max_scroll, pane.layout.content_height);
    const auto geom = xanatoo::scrollbar_geometry(
        view_h, content_h, pane.scroll_y, max_scroll);
    const float thumb_y = static_cast<float>(pane.y) + geom.thumb_top;
    int sx = sb_x(pane);
    int top = pane.y;
    int vh = static_cast<int>(view_h);

    // Track (sunken trough)
    namespace th = xanatoo::theme;
    const th::Palette& pal = th::palette();
    SDL_Rect track_r{sx, top, kSbW, vh};
    // Pointer feedback: the hovered bar and thumb light up, a held thumb
    // turns accent blue (a touch drag shows which bar it grabbed).
    const int mx = static_cast<int>(app.mouse_x);
    const int my = static_cast<int>(app.mouse_y);
    const FloatWindow* owner = nullptr;
    for (const FloatWindow& w : app.soft_wins)
      if (w.view == i) owner = &w;
    const bool pressed = app.dragging_scroll && app.scroll_drag_pane == i;
    const bool hover = !pressed && mx >= sx && mx < sx + kSbW && my >= top &&
                       my < top + vh && canvas_hover(app, owner);
    th::fill(app.sdl, track_r,
             hover || pressed ? pal.scroll_trough_hot : pal.scroll_trough);
    th::bevel(app.sdl, track_r, th::Bevel::Sunken);

    // Metadata marks on the track (doc Y → track fraction).
    auto mark_at = [&](float doc_y, int r, int g, int b) {
      if (content_h < 1.f) return;
      float frac = std::max(0.f, std::min(1.f, doc_y / content_h));
      int my = top + static_cast<int>(frac * std::max(0.f, view_h - 1.f));
      my = std::clamp(my, top + 1, top + std::max(1, vh - 2));
      SDL_SetRenderDrawColor(app.sdl, r, g, b, 255);
      SDL_Rect tick{sx + 1, my - 1, kSbW - 2, 2};
      xanatoo::sdl::fill_rect(app.sdl, &tick);
    };
    // Chapter / section breaks, from this pane's own document and layout.
    if (app.view_settings.heading_ticks)
      for (const SectionMark& m : section_marks(app, pane).sections) {
        if (m.level <= 1)
          mark_at(m.y, 25, 90, 170);
        else
          mark_at(m.y, 140, 160, 190);
      }
    // Bookmarks inside the page: from the layout where it holds the text,
    // else from the estimated position of the block.
    if (bookmark_marks_fresh(pane) && !pane.bookmark_marks.items.empty()) {
      const auto& block_y = section_marks(app, pane, true).block_y;
      const xanatoo::Document& bd = doc_of(app, pane);
      const xanatoo::Rgba& c = app.style.bookmark;
      for (const BookmarkMark& bm : pane.bookmark_marks.items) {
        const auto cpos = xanatoo::caret_screen_pos(
            pane.layout, bm.range.anchor, 0.f,
            static_cast<float>(app.fonts.regular.line_height()));
        float y = cpos.valid ? cpos.y : -1.f;
        for (xanatoo::NodeId n = bm.range.anchor.node;
             y < 0.f && n != xanatoo::INVALID_NODE; n = bd.get(n).parent)
          if (auto it = block_y.find(n); it != block_y.end()) y = it->second;
        if (y >= 0.f) mark_at(y, c.r, c.g, c.b);
      }
    }
    if (annotation_marks_fresh(pane) && !pane.annotation_marks.items.empty()) {
      const auto& block_y = section_marks(app, pane, true).block_y;
      const xanatoo::Document& ad = doc_of(app, pane);
      const xanatoo::Rgba& c = app.style.bookmark;
      for (const AnnotationMark& mark : pane.annotation_marks.items) {
        auto cp = xanatoo::caret_screen_pos(
            pane.layout, mark.range.anchor, 0.f,
            static_cast<float>(app.fonts.regular.line_height()));
        float y = cp.valid ? cp.y : -1.f;
        for (xanatoo::NodeId n = mark.range.anchor.node;
             y < 0.f && n != xanatoo::INVALID_NODE; n = ad.get(n).parent)
          if (auto it = block_y.find(n); it != block_y.end()) y = it->second;
        if (y >= 0.f) mark_at(y, c.r, c.g, c.b);
      }
    }
    // Search hits (while the find bar is open).
    if (prompt_is_search(app) && !app.prompt.text.empty() &&
        &pane == &app.panes[app.focus]) {
      const std::vector<xanatoo::Caret>& hits = find_hits(app, pane);
      const size_t shown = std::min<size_t>(hits.size(), 48);
      for (const xanatoo::Caret& hit : std::span(hits).first(shown)) {
        auto cpos = xanatoo::caret_screen_pos(
            pane.layout, hit, 0.f,
            static_cast<float>(app.fonts.regular.line_height()));
        if (cpos.valid) mark_at(cpos.y, 200, 120, 40);
      }
    }
    // Caret position on the focused pane.
    if (&pane == &app.panes[app.focus]) {
      auto cpos = xanatoo::caret_screen_pos(
          pane.layout, pane.sel.focus, 0.f,
          static_cast<float>(app.fonts.regular.line_height()));
      if (cpos.valid) mark_at(cpos.y, 40, 140, 70);
    }

    // Thumb
    SDL_Rect thumb{sx + 1, static_cast<int>(thumb_y), kSbW - 2,
                   std::max(1, static_cast<int>(geom.thumb_height))};
    th::draw_scroll_thumb(app.sdl, thumb, pressed,
                          hover && my >= thumb.y && my < thumb.y + thumb.h);
    // Grip ridges (BeOS-style) when the thumb is tall enough.
    if (thumb.h >= 24) {
      const int gy = thumb.y + thumb.h / 2;
      for (int k = -1; k <= 1; ++k) {
        th::hline(app.sdl, thumb.x + 3, thumb.x + thumb.w - 4, gy + k * 3,
                  pal.shadow);
        th::hline(app.sdl, thumb.x + 3, thumb.x + thumb.w - 4, gy + k * 3 + 1,
                  pal.highlight);
      }
    }
    // The target's tick is drawn *over* the thumb so it stays visible even
    // when the comment is at the current scroll position. The block map also
    // approximates positions outside the virtualised layout band.
    if (is_reply_target(app, pane)) {
      float y = app.reply.anchor_y;
      if (const auto* box = reply_card_box(app, pane))
        y = app.reply.anchor_y = box->y;
      if (y < 0.f) {
        const auto& block_y = section_marks(app, pane, true).block_y;
        if (auto it = block_y.find(app.reply.node); it != block_y.end())
          y = it->second;
      }
      if (y >= 0.f) {
        mark_at(y - 2.f, 255, 178, 48);
        mark_at(y, 204, 94, 12);
        mark_at(y + 2.f, 255, 178, 48);
      }
    }
  }
}

// Document layer of pane i — paper, selection, text — into the current render
// target, clipped to `area`. Content y maps to target y - `scroll`.
static void draw_pane_layer(App& app, int i, const SDL_Rect& area, float ox,
                            float scroll) {
  Pane& pane = app.panes[i];
  const GalleryStyleScope gallery_look(app, pane);
  xanatoo::sdl::set_clip_rect(app.sdl, &area);
  SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_NONE);
  app.renderer.draw_backdrop(pane.layout, area, scroll, ox);
  xanatoo::SelectionWash wash;
  if (!pane.sel.empty()) {
    wash.rects = xanatoo::selection_rects(pane.layout, pane.sel, scroll,
                                          app.renderer.glyph_extents());
    // Leap-search match uses amber so it is distinct from a user selection.
    wash.color = prompt_is_search(app) && i == app.focus
                     ? xanatoo::Rgba{230, 190, 70, 150}
                     : app.renderer.selection_color();
  }
  const auto& visited = pane.visited_links.glyphs;
  app.renderer.draw(pane.layout, scroll, ox, &wash,
                    visited.size() == pane.layout.glyphs.size() ? &visited
                                                                : nullptr);
}

void release_raster(PaneRaster& r) {
  for (SDL_Texture*& t : r.tex) {
    if (t) SDL_DestroyTexture(t);
    t = nullptr;
  }
  r.w = r.h = 0;
  r.valid = false;
}

void release_pane_rasters(App& app) {
  for (Pane& p : app.panes) release_raster(p.raster);
}

static bool ensure_raster_textures(App& app, PaneRaster& r, int w, int h) {
  if (r.tex[0] && r.tex[1] && r.w == w && r.h == h) return true;
  release_raster(r);
  for (SDL_Texture*& t : r.tex) {
    t = SDL_CreateTexture(app.sdl, SDL_PIXELFORMAT_ARGB8888,
                          SDL_TEXTUREACCESS_TARGET, w, h);
    if (!t) {
      release_raster(r);
      return false;
    }
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_NONE);
    // Nearest: scroll-blit must not re-filter the cached text (LINEAR does).
    xanatoo::sdl::set_nearest(t);
  }
  r.w = w;
  r.h = h;
  return true;
}

// Bring pane i's raster up to date for texture-space `scroll` (content y −
// scroll = texture y). Unchanged content scrolled by whole pixels is shifted
// into the other texture and only the exposed strip is drawn; anything else
// redraws the whole texture. False → render targets unavailable.
static bool update_pane_raster(App& app, int i, int w, int h, float scroll) {
  if (!xanatoo::sdl::target_supported(app.sdl)) return false;
  Pane& pane = app.panes[i];
  PaneRaster& r = pane.raster;
  if (!ensure_raster_textures(app, r, w, h)) return false;

  const xanatoo::Selection sel = pane.sel.empty() ? xanatoo::Selection{}
                                                  : pane.sel;
  const bool tint = !sel.empty() && prompt_is_search(app) && i == app.focus;
  update_visited_links(app, i);
  const uint64_t visited = pane.visited_links.version;
  const bool same_content = r.valid && r.visited_version == visited &&
                            r.layout_gen == pane.layout.generation &&
                            r.sel.anchor == sel.anchor &&
                            r.sel.focus == sel.focus && r.search_tint == tint;
  if (same_content && r.scroll == scroll) return true;

  SDL_Texture* prev_target = SDL_GetRenderTarget(app.sdl);
  const float d = scroll - r.scroll;
  const int dy = static_cast<int>(d);
  if (same_content && d == static_cast<float>(dy) && std::abs(dy) < r.h) {
    // dy > 0: content moved up, keep the lower part and expose the bottom.
    SDL_Texture* src = r.tex[r.cur];
    r.cur ^= 1;
    SDL_SetRenderTarget(app.sdl, r.tex[r.cur]);
    xanatoo::sdl::set_clip_rect(app.sdl, nullptr);
    const int keep = r.h - std::abs(dy);
    const SDL_Rect from{0, std::max(dy, 0), r.w, keep};
    const SDL_Rect to{0, std::max(-dy, 0), r.w, keep};
    xanatoo::sdl::copy(app.sdl, src, &from, &to);
    const SDL_Rect strip = dy > 0 ? SDL_Rect{0, keep, r.w, dy}
                                  : SDL_Rect{0, 0, r.w, -dy};
    draw_pane_layer(app, i, strip, 0.f, scroll);
  } else {
    SDL_SetRenderTarget(app.sdl, r.tex[r.cur]);
    draw_pane_layer(app, i, SDL_Rect{0, 0, r.w, r.h}, 0.f, scroll);
  }
  xanatoo::sdl::set_clip_rect(app.sdl, nullptr);
  SDL_SetRenderTarget(app.sdl, prev_target);

  r.valid = true;
  r.layout_gen = pane.layout.generation;
  r.scroll = scroll;
  r.sel = sel;
  r.search_tint = tint;
  r.visited_version = visited;
  return true;
}

// Text, selection and the three layers above, then the scrollbar.
void draw_pane(App& app, int i) {
  Pane& pane = app.panes[i];
  if (pane_image_view(pane)) {  // `//gallery//item:N`: the picture alone
    draw_gallery_image(app, i);
    draw_preview_badge(app, i);
    draw_load_indicator(app, i);
    return;
  }
  pane.scroll_y = std::min(pane.scroll_y, pane_max_scroll(app, pane));
  // Paged: always at the top of a spread.
  if (pane.pages.on)
    pane.scroll_y = spread_scroll(
        pane, spread_at(pane, pane.scroll_y + pane.pages.stride * 0.5f));

  const int clip_y = pane.y;
  SDL_Rect clip{pane.x, clip_y, std::max(1, pane.w - sb_reserve(pane)),
                std::max(1, pane.h)};

  // Draw at a whole-pixel scroll so the cached raster can be blitted; the
  // fractional part (touch / fling / scrollbar drag) stays in pane.scroll_y.
  const float exact_scroll = pane.scroll_y;
  pane.scroll_y = std::round(exact_scroll);

  // screen_y = content_y - scroll_y + pane.y
  const float draw_scroll = pane.scroll_y - static_cast<float>(pane.y);
  // Same mapping relative to the clip top (texture space).
  const float tex_scroll = draw_scroll + static_cast<float>(clip_y);
  if (update_pane_raster(app, i, clip.w, clip.h, tex_scroll)) {
    xanatoo::sdl::set_clip_rect(app.sdl, nullptr);
    xanatoo::sdl::copy(app.sdl, pane.raster.tex[pane.raster.cur], nullptr, &clip);
    xanatoo::sdl::set_clip_rect(app.sdl, &clip);
  } else {
    update_visited_links(app, i);
    draw_pane_layer(app, i, clip, static_cast<float>(pane.x), draw_scroll);
  }
  draw_page_tiles(app, i, clip, draw_scroll);
  draw_image_loading(app, i, clip, draw_scroll);

  draw_pane_overlays(app, i);
  if (pane_gallery(pane)) draw_gallery_grid_chrome(app, i);
  draw_footnote_overlay(app, i, clip);

  // Scrollbar lives in the strip excluded from the content clip.
  xanatoo::sdl::set_clip_rect(app.sdl, nullptr);

  if (pane.pages.on)
    draw_page_chrome(app, i);
  else
    draw_pane_scrollbar(app, i);
  draw_preview_badge(app, i);
  draw_load_indicator(app, i);
  pane.scroll_y = exact_scroll;
}

void draw_panes(App& app) {
  // Style paper is also the window clear colour behind the panes.
  app.renderer.clear(app.style.paper.r, app.style.paper.g, app.style.paper.b);
  int pane_count = app.split ? 2 : 1;
  for (int i = 0; i < pane_count; ++i) draw_pane(app, i);
}

void draw_split_chrome(App& app) {
  xanatoo::sdl::set_clip_rect(app.sdl, nullptr);
  if (app.split) {
    namespace th = xanatoo::theme;
    const SDL_Rect div = tile_divider_rect(app);
    const th::Palette& pal = th::palette();
    const th::Rgb background = app.diff_mode
        ? th::rgb(0xDB, 0xBD, 0x98)
        : app.tile_mode == TileMode::Trail ? pal.accent_soft : pal.face_dark;
    // Same hit-test as the drag and the resize cursor (present_frame).
    const int mx = static_cast<int>(app.mouse_x);
    const int my = static_cast<int>(app.mouse_y);
    const auto within = [&](const SDL_Rect& r, int pad = 0) {
      return mx >= r.x - pad && mx < r.x + r.w + pad &&
             my >= r.y - pad && my < r.y + r.h + pad;
    };
    const bool over_badge = within(tile_mode_button_rect(app));
    const bool hover_ok = canvas_hover(app);
    const th::Hot hot = app.dragging_split ? th::Hot::Pressed
        : hover_ok && !over_badge && within(div, 2) &&
                  !(app.diff_mode && within(tile_diff_track_rect(app)))
            ? th::Hot::Hover
            : th::Hot::None;
    th::fill(app.sdl, div, hot == th::Hot::Pressed ? pal.gutter_pressed
                           : hot == th::Hot::Hover ? pal.gutter_hot
                                                   : background);
    const th::Rgb lead = hot == th::Hot::Pressed ? pal.shadow : pal.highlight;
    const th::Rgb trail = hot == th::Hot::Pressed ? pal.highlight : pal.shadow;
    if (app.tile_axis == TileAxis::Columns) {
      th::vline(app.sdl, div.x, div.y, div.y + div.h - 1, lead);
      th::vline(app.sdl, div.x + div.w - 1, div.y, div.y + div.h - 1, trail);
    } else {
      th::hline(app.sdl, div.x, div.x + div.w - 1, div.y, lead);
      th::hline(app.sdl, div.x, div.x + div.w - 1, div.y + div.h - 1, trail);
    }
    if (!app.diff_mode) th::draw_grip_dots(app.sdl, div, app.ui_scale);
    if (app.diff_mode) {
      const SDL_Rect track = tile_diff_track_rect(app);
      if (track.w >= 2 && track.h >= 2) {
        th::fill(app.sdl, track, pal.face_light);
        const bool rows = app.tile_axis == TileAxis::Rows;
        const int along = rows ? track.w : track.h;
        const int across = rows ? track.h : track.w;
        SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_BLEND);
        for (int pi = 0; pi < kCentreViews; ++pi) {
          const int first_lane = (across * pi) / 2;
          const int lane = std::max(1, across * (pi + 1) / 2 - first_lane);
          for (float position : app.diff_overview[pi]) {
            const int at = std::clamp(static_cast<int>(std::lround(
                position * (along - 1))), 0, along - 1);
            SDL_Rect marker = rows
                ? SDL_Rect{track.x + at, track.y + first_lane, 2, lane}
                : SDL_Rect{track.x + first_lane, track.y + at, lane, 2};
            SDL_SetRenderDrawColor(app.sdl, 189, 100, 25, 210);
            xanatoo::sdl::fill_rect(app.sdl, &marker);
          }
          const Pane& pane = app.panes[pi];
          const float height = std::max(static_cast<float>(pane.h),
                                        pane.layout.content_height);
          const float begin = std::clamp(pane.scroll_y / height, 0.f, 1.f);
          const float end = std::clamp(
              (pane.scroll_y + static_cast<float>(pane.h)) / height, 0.f, 1.f);
          const int start = static_cast<int>(std::lround(begin * (along - 1)));
          const int extent = std::max(2, static_cast<int>(std::lround(
              (end - begin) * (along - 1))));
          SDL_Rect viewport = rows
              ? SDL_Rect{track.x + start, track.y + first_lane,
                         std::min(extent, along - start), lane}
              : SDL_Rect{track.x + first_lane, track.y + start,
                         lane, std::min(extent, along - start)};
          SDL_SetRenderDrawColor(app.sdl, 47, 104, 158, 180);
          xanatoo::sdl::draw_rect(app.sdl, &viewport);
        }
        SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_NONE);
      }
    }
    // The visible badge is also the mode selector (click/right-click). It
    // straddles the gutter so even a thin independent divider is usable.
    const SDL_Rect badge = tile_mode_button_rect(app);
    th::fill(app.sdl, badge,
             hover_ok && over_badge && !app.dragging_split ? pal.gutter_hot
                                                           : background);
    th::bevel(app.sdl, badge, th::Bevel::Raised);
    const char* label = app.diff_mode ? "DIFF" :
        app.tile_mode == TileMode::Trail ? "TRAIL" : "TILES";
    const float text_x = static_cast<float>(badge.x +
        (badge.w - app.ui_font.measure(label)) / 2);
    const float baseline = static_cast<float>(badge.y +
        (badge.h - app.ui_font.line_height()) / 2 + app.ui_font.ascender());
    app.renderer.draw_text(app.ui_font, text_x, baseline, label,
                           pal.text.r, pal.text.g, pal.text.b);
  }
  if (app.split && app.tile_axis == TileAxis::Columns && shared_doc(app) &&
      app.panes[0].sel.focus.node != xanatoo::INVALID_NODE &&
      app.panes[0].sel.focus.node == app.panes[1].sel.focus.node) {
    float lh = static_cast<float>(app.fonts.regular.line_height());
    // draw_scroll matches pane rendering (content → screen under the toolbar).
    const float ds0 = app.panes[0].scroll_y - static_cast<float>(app.panes[0].y);
    const float ds1 = app.panes[1].scroll_y - static_cast<float>(app.panes[1].y);
    auto c0 = xanatoo::caret_screen_pos(app.panes[0].layout,
                                        app.panes[0].sel.focus, ds0, lh);
    auto c1 = xanatoo::caret_screen_pos(app.panes[1].layout,
                                        app.panes[1].sel.focus, ds1, lh);
    if (c0.valid && c1.valid) {
      int x0 = app.panes[0].x + static_cast<int>(c0.x);
      int y0 = static_cast<int>(c0.y);
      int x1 = app.panes[1].x + static_cast<int>(c1.x);
      int y1 = static_cast<int>(c1.y);
      // Clamp into content area below toolbar / above status bar
      int min_y = app.panes[0].y;
      int max_y = app.panes[0].y + app.panes[0].h;
      if (y0 >= min_y && y0 < max_y && y1 >= min_y && y1 < max_y) {
        SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(app.sdl, 25, 90, 170, 160);
        // Straight connector + soft endpoints
        xanatoo::sdl::draw_line(app.sdl, x0, y0, x1, y1);
        xanatoo::sdl::draw_line(app.sdl, x0, y0 + 1, x1, y1 + 1);
        auto dot = [&](int x, int y) {
          SDL_Rect r{x - 3, y - 3, 7, 7};
          xanatoo::sdl::fill_rect(app.sdl, &r);
        };
        dot(x0, y0);
        dot(x1, y1);
        SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_NONE);
      }
    }
  }
}

void draw_outline_panel(App& app) {
  if (app.outline_mode) {
    if (app.ol.dock == Dock::None) {
      app.ol.w = std::max(app.ol.w, 200);
      app.ol.h = std::max(app.ol.h, 140);
    }
    const float step = outline_row_step(app);
    auto [cx, cy, cw, ch] = outline_client_rect(app);
    {
      float content_h = static_cast<float>(app.outline.size()) * step + 8.f;
      float max_s = std::max(0.f, content_h - static_cast<float>(ch));
      if (app.ol.scroll > max_s) app.ol.scroll = max_s;
    }

    namespace th = xanatoo::theme;
    const th::Palette& pal = th::palette();
    xanatoo::Font& font = app.ui_font;
    xanatoo::Font& bold =
        app.ui_font_bold.loaded() ? app.ui_font_bold : app.ui_font;

    std::string ol_title = "Outline";
    const bool section_focus = focused_pane(app).section_focus;
    if (section_focus && app.outline_sel >= 0 &&
        app.outline_sel < static_cast<int>(app.outline.size())) {
      const std::string& tx =
          app.outline[static_cast<size_t>(app.outline_sel)].text;
      if (!tx.empty()) ol_title += " — " + tx;
    }
    const SDL_Rect fb = outline_focus_rect(app, app.ol);
    // Title tab + selection are lit only while the outline has the keyboard.
    const bool keys = panel_has_keys(app, KeyFocus::Outline);
    const bool open = draw_floating_frame(app, app.ol, ol_title, keys,
                                          title_shade_zone(app.ol).x - fb.x);
    // Focus toggle (section isolation): latched push button left of shade.
    draw_title_button(app, app.ol, fb, "Focus", section_focus);
    if (!open) return;

    SDL_Rect clip{cx, cy, cw, ch};
    xanatoo::sdl::set_clip_rect(app.sdl, &clip);
    const bool scrolls =
        static_cast<float>(app.outline.size()) * step + 8.f >
        static_cast<float>(ch);
    const int text_right = cw - (scrolls ? 16 : 4);
    // Row i spans [cy + i·step − scroll, +step) — click_outline relies on it.
    float row_y = static_cast<float>(cy) - app.ol.scroll;
    for (int i = 0; i < static_cast<int>(app.outline.size()); ++i, row_y += step) {
      if (row_y > static_cast<float>(cy + ch)) break;
      if (row_y + step < static_cast<float>(cy)) continue;
      const auto& e = app.outline[static_cast<size_t>(i)];
      const bool sel = i == app.outline_sel;
      const int ry = static_cast<int>(row_y);
      const int rh = static_cast<int>(step);
      const OutlineRowGeom g = outline_row_geom(app, e.level);
      const bool lit = sel && keys;
      if (sel)
        th::fill(app.sdl, SDL_Rect{cx + 1, ry, text_right - 1, rh},
                 keys ? pal.accent : pal.face_dark);
      const th::Rgb ink = lit ? pal.highlight : pal.text;
      const th::Rgb dim = lit ? pal.accent_soft : pal.text_dim;

      // Articles (self-contained pieces: posts, feed items) are bold;
      // sections inside them and plain section headings are regular.
      xanatoo::Font& f = e.article || e.preamble ? bold : font;
      const float baseline = static_cast<float>(
          ry + (rh - f.line_height()) / 2 + f.ascender());
      // Disclosure triangle (drawn, so it never depends on font coverage).
      if (e.has_children) {
        const int tcx = cx + g.twist_x + g.twist_w / 2;
        const int tcy = ry + rh / 2;
        SDL_SetRenderDrawColor(app.sdl, dim.r, dim.g, dim.b, 255);
        const int t = std::max(4, g.twist_w * 2 / 7);  // half extent
        for (int k = 0; k <= t; ++k) {
          if (e.folded)  // ▸ pointing right
            xanatoo::sdl::draw_line(app.sdl, tcx - t / 2 + k, tcy - t + k,
                               tcx - t / 2 + k, tcy + t - k);
          else  // ▾ pointing down
            xanatoo::sdl::draw_line(app.sdl, tcx - t + k, tcy - t / 2 + k,
                               tcx + t - k, tcy - t / 2 + k);
        }
      }
      const int text_w = text_right - g.text_x - 4;
      const th::Rgb tink = e.folded && !lit ? pal.text_dim : ink;
      app.renderer.draw_text(f, static_cast<float>(cx + g.text_x), baseline,
                             fit_text(f, e.text, text_w), tink.r, tink.g,
                             tink.b);
    }
    if (app.outline.empty()) {
      app.renderer.draw_text(
          font, static_cast<float>(cx + 8),
          static_cast<float>(cy + 6 + font.ascender()),
          "No headings in this document", pal.text_dim.r, pal.text_dim.g,
          pal.text_dim.b);
    }
    xanatoo::sdl::set_clip_rect(app.sdl, nullptr);

    {
      const float content_h =
          static_cast<float>(app.outline.size()) * step + 8.f;
      const float max_s = std::max(0.f, content_h - static_cast<float>(ch));
      draw_client_scrollbar(app, app.ol, max_s);
    }
  }
}

void draw_backlinks_panel(App& app) {
  if (app.backlinks_mode) {
    if (app.bl.dock == Dock::None) {
      app.bl.w = std::max(app.bl.w, 200);
      app.bl.h = std::max(app.bl.h, 120);
    }
    const float step = backlinks_row_step(app);
    const int title_h = float_title_h(app.bl);
    const int pad = 8;
    int cx = app.bl.x + pad, cy = app.bl.y + title_h + 4;
    int cw = app.bl.w - pad * 2, ch = app.bl.h - title_h - pad - 4;
    {
      float content_h = static_cast<float>(app.backlinks.size()) * step + 8.f;
      float max_s = std::max(0.f, content_h - static_cast<float>(ch));
      if (app.bl.scroll > max_s) app.bl.scroll = max_s;
    }
    if (!draw_floating_frame(app, app.bl,
                             app.backlinks_label.empty()
                                 ? "Backlinks (no label under caret)"
                                 : ("Backlinks → #" + app.backlinks_label),
                             panel_has_keys(app, KeyFocus::Backlinks)))
      return;
    SDL_Rect clip{cx, cy, cw, ch};
    xanatoo::sdl::set_clip_rect(app.sdl, &clip);
    xanatoo::Font& font = app.ui_font;
    const auto& pal = xanatoo::theme::palette();
    const int text_off =
        (static_cast<int>(step) - font.line_height()) / 2 + font.ascender();
    float ty = static_cast<float>(cy + text_off) - app.bl.scroll;
    if (app.backlinks.empty()) {
      app.renderer.draw_text(font, static_cast<float>(cx + 8), ty,
                             "No links to this label", pal.text_dim.r,
                             pal.text_dim.g, pal.text_dim.b);
    }
    for (int i = 0; i < static_cast<int>(app.backlinks.size()); ++i) {
      if (ty > static_cast<float>(cy + ch) + step) break;
      if (ty + step >= static_cast<float>(cy)) {
        const auto& e = app.backlinks[static_cast<size_t>(i)];
        std::string line = e.kind;
        if (!e.rel.empty()) line += ":" + e.rel;
        line += "  ";
        line += e.preview;
        const bool sel = i == app.backlinks_sel;
        const bool lit = sel && panel_has_keys(app, KeyFocus::Backlinks);
        if (sel) {
          SDL_Rect hi{cx + 1, static_cast<int>(ty) - text_off, cw - 2,
                      static_cast<int>(step)};
          xanatoo::theme::fill(app.sdl, hi, lit ? pal.accent : pal.face_dark);
        }
        const auto ink = lit ? pal.highlight : pal.text;
        app.renderer.draw_text(font, static_cast<float>(cx + 8), ty,
                               fit_text(font, line, cw - 28), ink.r, ink.g,
                               ink.b);
      }
      ty += step;
    }
    xanatoo::sdl::set_clip_rect(app.sdl, nullptr);
    {
      const float content_h =
          static_cast<float>(app.backlinks.size()) * step + 8.f;
      const float max_s = std::max(0.f, content_h - static_cast<float>(ch));
      draw_client_scrollbar(app, app.bl, max_s);
    }
  }
}

void draw_help_window(App& app) {
  if (app.help_mode) {
    ensure_help_doc(app);
    if (app.help.dock == Dock::None) {
      app.help.w = std::max(app.help.w, 280);
      app.help.h = std::max(app.help.h, 200);
    }
    if (app.help_needs_layout) layout_help(app);

    // Clamp scroll
    {
      auto [cx, cy, cw, ch] = help_client_rect(app);
      float max_s = std::max(
          0.f, app.help_layout.content_height - static_cast<float>(ch) + 40.f);
      if (app.help.scroll > max_s) app.help.scroll = max_s;
    }

    if (!draw_floating_frame(app, app.help, "Help — xanatoo",
                             float_active(app, app.help))) return;

    auto [cx, cy, cw, ch] = help_client_rect(app);
    SDL_Rect clip{cx, cy, cw, ch};
    xanatoo::sdl::set_clip_rect(app.sdl, &clip);
    // screen_y = content_y - scroll + client_y  ⇒  draw_scroll = scroll -
    // client_y (mirrors pane path: draw_scroll = scroll_y - ui_top)
    const float help_draw_scroll = app.help.scroll - static_cast<float>(cy);
    app.renderer.draw_backdrop(app.help_layout, clip, help_draw_scroll,
                               static_cast<float>(cx));
    app.renderer.draw(app.help_layout, help_draw_scroll,
                      static_cast<float>(cx));
    xanatoo::sdl::set_clip_rect(app.sdl, nullptr);

    {
      const float max_s = std::max(
          0.f, app.help_layout.content_height - static_cast<float>(ch) + 40.f);
      draw_client_scrollbar(app, app.help, max_s);
    }
  }
}

// Soft orange curve tying a link or quote in a view to its target in the
// Context panel, with a dot at each end. Horizontal tangents at both ends.
static void draw_connector(App& app, float x0, float y0, float x1, float y1) {
  SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(app.sdl, 220, 140, 40, 160);
  // Cubic Bezier through (x0,y0), (c0x,y0), (c1x,y1), (x1,y1): control
  // points pull toward a mid gutter.
  const float mx = (x0 + x1) * 0.5f;
  const float c0x = x0 + (mx - x0) * 0.55f;
  const float c1x = x1 - (x1 - mx) * 0.55f;
  const int steps = 24;
  float px = x0, py = y0;
  for (int i = 1; i <= steps; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(steps);
    const float u = 1.f - t;
    const float bx = u * u * u * x0 + 3.f * u * u * t * c0x +
                     3.f * u * t * t * c1x + t * t * t * x1;
    const float by = u * u * u * y0 + 3.f * u * u * t * y0 +
                     3.f * u * t * t * y1 + t * t * t * y1;
    xanatoo::sdl::draw_line(app.sdl, static_cast<int>(px),
                            static_cast<int>(py), static_cast<int>(bx),
                            static_cast<int>(by));
    // Slight thickness: a parallel stroke.
    xanatoo::sdl::draw_line(app.sdl, static_cast<int>(px),
                            static_cast<int>(py) + 1, static_cast<int>(bx),
                            static_cast<int>(by) + 1);
    px = bx;
    py = by;
  }
  auto dot = [&](float x, float y) {
    SDL_Rect r{static_cast<int>(x) - 2, static_cast<int>(y) - 2, 5, 5};
    xanatoo::sdl::fill_rect(app.sdl, &r);
  };
  SDL_SetRenderDrawColor(app.sdl, 220, 140, 40, 200);
  dot(x0, y0);
  dot(x1, y1);
  SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_NONE);
}

void draw_context_window(App& app) {
  if (!app.context_panel_open) return;
  if (app.context_win.dock == Dock::None) {
    app.context_win.w = std::max(app.context_win.w, 280);
    app.context_win.h = std::max(app.context_win.h, 200);
  }
  // Title reflects what is being previewed, but always keeps the "Context"
  // prefix so it is not mistaken for another panel ("Context: Page" vs the
  // "Page Navigation" panel).
  std::string title = "Context";
  if (!app.context_kind.empty() && app.context_kind != "Context") {
    title += ": ";
    title += app.context_kind;
  }
  if (!app.context_title.empty()) {
    title += " — ";
    title += app.context_title;  // draw_floating_frame fits it (fit_text)
  }

  if (!app.context_has_doc) {
    // Fall back to the ImGui text body (empty state / external link note).
    if (draw_floating_frame(app, app.context_win, title,
                            float_active(app, app.context_win)))
      app.ui.render_embedded_panel(app.sdl, xanatoo::UiPanelLayer::Context);
    return;
  }

  if (!app.context_image_src.empty()) {
    app.context_win.scroll = 0.f;
    if (!draw_floating_frame(app, app.context_win, title,
                             float_active(app, app.context_win)))
      return;
    auto [cx, cy, cw, ch] = float_client_rect(app, app.context_win);
    draw_context_image(app, SDL_Rect{cx, cy, cw, ch}, app.context_image_src,
                       app.context_image_base);
    return;
  }

  if (app.context_needs_layout) layout_context(app);

  {
    auto [cx, cy, cw, ch] = float_client_rect(app, app.context_win);
    float max_s = std::max(
        0.f, app.context_layout.content_height - static_cast<float>(ch) + 40.f);
    if (app.context_win.scroll > max_s) app.context_win.scroll = max_s;
    if (app.context_win.scroll < 0.f) app.context_win.scroll = 0.f;
  }

  if (!draw_floating_frame(app, app.context_win, title,
                           float_active(app, app.context_win)))
    return;

  auto [cx, cy, cw, ch] = float_client_rect(app, app.context_win);
  SDL_Rect clip{cx, cy, cw, ch};
  xanatoo::sdl::set_clip_rect(app.sdl, &clip);
  const float draw_scroll =
      app.context_win.scroll - static_cast<float>(cy);
  app.renderer.draw_backdrop(app.context_layout, clip, draw_scroll,
                             static_cast<float>(cx));
  xanatoo::SelectionWash wash;
  if (!app.context_sel.empty()) {
    wash.rects = xanatoo::selection_rects(app.context_layout, app.context_sel,
                                          draw_scroll,
                                          app.renderer.glyph_extents());
    wash.color = app.renderer.selection_color();
  }
  app.renderer.draw(app.context_layout, draw_scroll, static_cast<float>(cx),
                    &wash);

  // The footnote whose mark is hovered: wash its block (boxes of the note
  // and everything inside it) in the style's `mark` colour.
  if (app.context_footnote != xanatoo::INVALID_NODE) {
    const xanatoo::Document& d = app.context_doc;
    auto inside = [&](xanatoo::NodeId id) {
      for (int guard = 0; id != xanatoo::INVALID_NODE && guard < 256; ++guard) {
        if (id == app.context_footnote) return true;
        const xanatoo::Node& n = d.get(id);
        if (n.id != id) return false;
        id = n.parent;
      }
      return false;
    };
    float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
    bool any = false;
    for (const auto& b : app.context_layout.boxes) {
      if (!inside(b.node)) continue;
      if (!any) {
        x0 = b.x; y0 = b.y; x1 = b.x + b.width; y1 = b.y + b.height;
        any = true;
      } else {
        x0 = std::min(x0, b.x); y0 = std::min(y0, b.y);
        x1 = std::max(x1, b.x + b.width); y1 = std::max(y1, b.y + b.height);
      }
    }
    if (any) {
      const xanatoo::Rgba m = app.style.mark;
      const SDL_Rect hr{static_cast<int>(x0) + cx,
                        static_cast<int>(y0 - draw_scroll),
                        static_cast<int>(x1 - x0), static_cast<int>(y1 - y0)};
      SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_BLEND);
      SDL_SetRenderDrawColor(app.sdl, m.r, m.g, m.b, m.a);
      xanatoo::sdl::fill_rect(app.sdl, &hr);
    }
  }

  // Highlight the source span of the quote currently shown in Context.
  std::vector<xanatoo::SelRect> source_rects;
  if (app.context_jump_pane >= 0 && app.context_jump_pane < kMaxViews &&
      app.context_jump_link >= 0) {
    Pane& qp = app.panes[app.context_jump_pane];
    if (DocBuffer* b = find_buf(app, qp.buf)) {
      const auto& links = quote_links_of(*b);
      if (static_cast<size_t>(app.context_jump_link) < links.size()) {
        const xanatoo::QuoteLink& l =
            links[static_cast<size_t>(app.context_jump_link)];
        source_rects = xanatoo::selection_rects(
            app.context_layout, l.source, draw_scroll,
            app.renderer.glyph_extents());
        app.renderer.draw_selection(source_rects, static_cast<float>(cx),
                                    220, 140, 40, 110);
      }
    }
  }

  // The message a hovered in-page link points at ("reply to …"): its card,
  // up to the first reply nested in it, washed and outlined in the
  // connector's colour; the curve ends at its header line, on the card's
  // left (`msg_x`) or right (`msg_x1`) edge, whichever faces the link.
  bool msg_shown = false;
  float msg_x = 0.f, msg_x1 = 0.f, msg_y = 0.f;
  if (app.context_message != xanatoo::INVALID_NODE) {
    const xanatoo::LayoutBox* card = nullptr;
    for (const auto& mc : app.context_layout.message_cards)
      if (mc.box.node == app.context_message) card = &mc.box;
    if (card) {
      float bottom = card->y + card->height;
      for (const auto& mc : app.context_layout.message_cards)
        if (mc.box.node != card->node && mc.box.y > card->y &&
            mc.box.y < bottom)
          bottom = mc.box.y;
      const SDL_Rect r{static_cast<int>(card->x) + cx,
                       static_cast<int>(card->y - draw_scroll),
                       static_cast<int>(card->width),
                       static_cast<int>(bottom - card->y)};
      SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_BLEND);
      SDL_SetRenderDrawColor(app.sdl, 220, 140, 40, 40);
      xanatoo::sdl::fill_rect(app.sdl, &r);
      SDL_SetRenderDrawColor(app.sdl, 220, 140, 40, 200);
      xanatoo::sdl::draw_rect(app.sdl, &r);
      msg_x = static_cast<float>(r.x);
      msg_x1 = static_cast<float>(r.x + r.w);
      msg_y = static_cast<float>(r.y) +
              std::min(static_cast<float>(r.h),
                       static_cast<float>(app.fonts.regular.line_height())) *
                  0.5f;
      for (const auto& pg : app.context_layout.glyphs) {
        if (pg.node != app.context_message) continue;
        xanatoo::Font& font = app.fonts.pick(pg.font);
        msg_y = pg.y - draw_scroll -
                static_cast<float>(font.ascender()) +
                static_cast<float>(font.line_height()) * 0.5f;
        break;
      }
      msg_shown = msg_y >= static_cast<float>(cy) &&
                  msg_y <= static_cast<float>(cy + ch);
    }
  }
  xanatoo::sdl::set_clip_rect(app.sdl, nullptr);

  {
    const float max_s = std::max(
        0.f, app.context_layout.content_height - static_cast<float>(ch) + 40.f);
    draw_client_scrollbar(app, app.context_win, max_s);
  }

  // Connector line between the quote in the main pane and its source in
  // the Context panel. Soft orange curve so the relation is obvious without
  // fighting the text.
  if (!source_rects.empty() && app.context_jump_pane >= 0 &&
      app.context_jump_pane < kMaxViews && app.context_jump_link >= 0) {
    Pane& qp = app.panes[app.context_jump_pane];
    if (DocBuffer* b = find_buf(app, qp.buf)) {
      const auto& links = quote_links_of(*b);
      if (static_cast<size_t>(app.context_jump_link) < links.size()) {
        const xanatoo::QuoteLink& l =
            links[static_cast<size_t>(app.context_jump_link)];
        const float py = static_cast<float>(qp.y);
        const float ox = static_cast<float>(qp.x);
        const float q_draw_scroll = qp.scroll_y - py;
        const auto quote_rects = xanatoo::selection_rects(
            qp.layout, l.quote, q_draw_scroll, app.renderer.glyph_extents());
        if (!quote_rects.empty()) {
          // Representative points: mid-right of the first quote rect,
          // mid-left of the first source rect (already in screen space for y;
          // x needs the origin applied).
          const auto& qr = quote_rects.front();
          const auto& sr = source_rects.front();
          const float x0 = qr.x + ox + qr.w;
          const float y0 = qr.y + qr.h * 0.5f;
          const float x1 = sr.x + static_cast<float>(cx);
          const float y1 = sr.y + sr.h * 0.5f;
          // Clip-ish: only draw when both ends look on-screen.
          const bool ok0 = y0 >= py && y0 <= py + static_cast<float>(qp.h) &&
                           x0 >= ox && x0 <= ox + static_cast<float>(qp.w);
          const bool ok1 = y1 >= static_cast<float>(cy) &&
                           y1 <= static_cast<float>(cy + ch) &&
                           x1 >= static_cast<float>(cx) &&
                           x1 <= static_cast<float>(cx + cw);
          if (ok0 && ok1) draw_connector(app, x0, y0, x1, y1);
        }
      }
    }
  }

  // Connector from the hovered link to the message it points at, from the
  // link's end that faces the panel.
  if (msg_shown && app.hover.link_pane >= 0 &&
      app.hover.link_pane < kMaxViews && !app.hover.link_in_footnotes &&
      app.hover.link_node != xanatoo::INVALID_NODE) {
    const Pane& lp = app.panes[app.hover.link_pane];
    const float py = static_cast<float>(lp.y);
    const float ox = static_cast<float>(lp.x);
    bool found = false;
    float lx0 = 0.f, lx1 = 0.f, ly = 0.f;
    for_each_link_run(
        app, lp,
        [&](const xanatoo::PositionedGlyph& pg) {
          return pg.node == app.hover.link_node &&
                 pg.span_index == app.hover.link_span &&
                 pg.auto_link == app.hover.link_auto &&
                 pg.kind == xanatoo::InlineKind::Link;
        },
        [&](float x0, float x1, float top, float, float bot) {
          if (found) return;
          found = true;
          lx0 = x0 + ox;
          lx1 = x1 + ox;
          ly = (top + bot) * 0.5f - (lp.scroll_y - py);
        });
    const bool panel_right = msg_x >= (lx0 + lx1) * 0.5f;
    if (found && ly >= py && ly <= py + static_cast<float>(lp.h))
      draw_connector(app, panel_right ? lx1 : lx0, ly,
                     panel_right ? msg_x : msg_x1, msg_y);
  }
}

void draw_dock_preview(App& app) {
  if (app.tab_drop_preview) {
    const int h = static_cast<int>(28.f * app.ui_scale);
    SDL_Rect strip{0, static_cast<int>(app.ui_top) - h, app.win_w, h};
    SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(app.sdl, 40, 100, 180, 90);
    xanatoo::sdl::fill_rect(app.sdl, &strip);
    SDL_SetRenderDrawColor(app.sdl, 40, 100, 180, 200);
    xanatoo::sdl::draw_rect(app.sdl, &strip);
    SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_NONE);
  }
  if (app.dock_preview != Dock::None) {
    auto [dx, dy, dw, dh] = dock_rect(app, app.dock_preview);
    if (dw > 0 && dh > 0) {
      SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_BLEND);
      SDL_SetRenderDrawColor(app.sdl, 40, 100, 180, 70);
      SDL_Rect zone{dx, dy, dw, dh};
      xanatoo::sdl::fill_rect(app.sdl, &zone);
      SDL_SetRenderDrawColor(app.sdl, 40, 100, 180, 180);
      xanatoo::sdl::draw_rect(app.sdl, &zone);
      SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_NONE);
      const char* label = "";
      switch (app.dock_preview) {
        case Dock::Left:
          label = "Dock left";
          break;
        case Dock::Right:
          label = "Dock right";
          break;
        case Dock::Top:
          label = "Dock top";
          break;
        case Dock::Bottom:
          label = "Dock bottom";
          break;
        case Dock::Max:
          label = "Maximize";
          break;
        case Dock::None:
          break;
      }
      if (label[0]) {
        int tw = app.ui_font.measure(label);
        float lx = static_cast<float>(dx + (dw - tw) / 2);
        float ly = static_cast<float>(dy + dh / 2);
        app.renderer.draw_text(app.ui_font, lx, ly, label, 30, 60, 120);
      }
    }
  }
}

// Alt text of the hovered block image (its description, shown instead of
// under the picture), wrapped to tooltip lines; empty when there is none.
static std::string image_alt_tooltip(App& app) {
  if (app.hover.image == xanatoo::INVALID_NODE || app.hover.image_handle ||
      app.hover.image_card || app.hover.image_pane < 0 ||
      app.hover.image_pane >= static_cast<int>(std::size(app.panes)))
    return {};
  const xanatoo::Document& d = doc_of(app, app.panes[app.hover.image_pane]);
  if (app.hover.image > d.nodes.size()) return {};
  const std::string& alt = d.get(app.hover.image).alt;
  if (alt.empty()) return {};
  // Word wrap at ~72 bytes (the tooltip cuts lines past 96).
  std::string out;
  size_t line = 0;
  size_t i = 0;
  while (i < alt.size()) {
    size_t e = alt.find(' ', i);
    if (e == std::string::npos) e = alt.size();
    const size_t n = e - i;
    if (line > 0 && line + 1 + n > 72) {
      out += '\n';
      line = 0;
    } else if (line > 0) {
      out += ' ';
      ++line;
    }
    out.append(alt, i, n);
    line += n;
    i = e + 1;
  }
  return out;
}

void draw_hover_tooltip(App& app) {
  {
    const bool prompt = prompt_has_keys(app) || app.help_mode;
    std::string tip;
    if (!prompt) {
      if (app.hover.stretch != xanatoo::INVALID_NODE ||
          app.hover.list_fold != xanatoo::INVALID_NODE) {
        const int hp = app.hover.stretch_pane;
        const xanatoo::NodeId sid = app.hover.stretch;
        tip = "Click to expand / collapse";
        if (sid != xanatoo::INVALID_NODE && hp >= 0) {
          const auto& hd = doc_of(app, app.panes[hp]);
          if (xanatoo::stretch_summary(hd, sid) != xanatoo::INVALID_NODE)
            tip = xanatoo::stretch_folded(
                      hd, sid, &buf(app, app.panes[hp].buf).folds)
                      ? "Click to show the full text"
                      : "Click to go back to the summary";
        }
      } else if (!app.hover.datetime.empty()) {
        tip = xanatoo::timestamp_hover_text(
            app.hover.datetime, static_cast<int64_t>(std::time(nullptr)));
        if (!app.hover.href.empty()) tip += "\nClick: " + app.hover.href;
      } else if (std::string alt = image_alt_tooltip(app); !alt.empty()) {
        tip = std::move(alt);
        if (!app.hover.href.empty()) tip += "\nClick: " + app.hover.href;
      } else if (!app.hover.href.empty()) {
        tip = app.hover.href;
        if (!app.hover.preview.empty() && app.hover.href[0] == '#') {
          tip += "  —  ";
          tip += app.hover.preview;
        }
        if (!app.hover.rel.empty()) {
          tip += "  ·  rel=";
          tip += app.hover.rel;
        }
      } else if (app.hover.image_handle) {
        tip = "Drag to resize image";
      } else if (app.hover.col_gap) {
        tip = "Drag to resize columns";
      } else if (app.hover.task != xanatoo::INVALID_NODE) {
        tip = "Click to toggle task";
      } else if (app.hover.quote >= 0 && app.hover.quote_pane >= 0) {
        tip = quote_tooltip(app, app.hover.quote_pane,
                            static_cast<size_t>(app.hover.quote));
      } else if (std::string h = history_tooltip(app); !h.empty()) {
        tip = std::move(h);
      } else if (!app.hover.bookmark.empty()) {
        const xanatoo::Bookmark* b = find_bookmark(app, app.hover.bookmark);
        tip = "Bookmark: " + (b ? xanatoo::bookmark_label(*b)
                                : app.hover.bookmark) +
              "  ·  right-click for more";
      } else if (app.hover.grip != xanatoo::INVALID_NODE) {
        tip = "Drag to move block";
      } else if (app.hover.transclude != xanatoo::INVALID_NODE) {
        tip = app.hover.transclude_src.empty()
                  ? "Include"
                  : ("Include: " + app.hover.transclude_src);
      }
    }
    if (!tip.empty()) {
      std::vector<std::string> lines;
      size_t pos = 0;
      do {
        const size_t end = tip.find('\n', pos);
        std::string line = tip.substr(pos, end == std::string::npos
                                              ? std::string::npos : end - pos);
        if (line.size() > 96) line = line.substr(0, 93) + "…";
        lines.push_back(std::move(line));
        if (end == std::string::npos) break;
        pos = end + 1;
      } while (pos < tip.size());
      int tw = 0;
      for (const auto& line : lines)
        tw = std::max(tw, app.ui_font.measure(line));
      const int pad_x = 10, pad_y = 6;
      int box_w = tw + pad_x * 2;
      int box_h = app.ui_font.line_height() * static_cast<int>(lines.size()) +
                  pad_y * 2;
      int bx = static_cast<int>(app.mouse_x) + 14;
      int by = static_cast<int>(app.mouse_y) - box_h - 10;
      if (bx + box_w > app.win_w - 4) bx = app.win_w - box_w - 4;
      if (bx < 4) bx = 4;
      if (by < static_cast<int>(app.ui_top) + 4)
        by = static_cast<int>(app.mouse_y) + 18;
      if (by + box_h > app.win_h - 32) by = app.win_h - 32 - box_h;

      SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_BLEND);
      SDL_SetRenderDrawColor(app.sdl, 28, 36, 50, 235);
      SDL_Rect box{bx, by, box_w, box_h};
      xanatoo::sdl::fill_rect(app.sdl, &box);
      // Accent by kind
      if (app.hover.stretch != xanatoo::INVALID_NODE ||
          app.hover.list_fold != xanatoo::INVALID_NODE)
        SDL_SetRenderDrawColor(app.sdl, 40, 140, 90, 255);
      else if (app.hover.col_gap || app.hover.image_handle)
        SDL_SetRenderDrawColor(app.sdl, 40, 100, 180, 255);
      else
        SDL_SetRenderDrawColor(app.sdl, 90, 150, 220, 255);
      xanatoo::sdl::draw_rect(app.sdl, &box);
      SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_NONE);
      float tip_baseline =
          static_cast<float>(by + pad_y + app.ui_font.ascender());
      for (const auto& line : lines) {
        app.renderer.draw_text(app.ui_font, static_cast<float>(bx + pad_x),
                               tip_baseline, line, 235, 240, 250);
        tip_baseline += app.ui_font.line_height();
      }
    }
  }
}

void present_frame(App& app) {
  const int mx = static_cast<int>(app.mouse_x);
  const int my = static_cast<int>(app.mouse_y);
  const auto floats = float_stack(app);
  const bool moving_float = std::any_of(floats.begin(), floats.end(),
      [](const FloatWindow* w) { return w->dragging; });
  const bool resizing_float = std::any_of(floats.begin(), floats.end(),
      [](const FloatWindow* w) { return w->resize != 0; });
  const bool native_drag = moving_float || resizing_float ||
      app.dock_split_drag.side != Dock::None || app.dragging_split ||
      app.dragging_diff_overview;
  // The menu bar, popups and embedded ImGui controls own their own cursors.
  // In particular, don't turn the text-edit I-beam back into an arrow. An
  // active native drag owns its pointer even when it leaves its starting area.
  if (native_drag || !app.ui.captures_point(
          app.mouse_x, app.mouse_y, float_layer_at(app, mx, my))) {
    SDL_Cursor* pointer = app.cursor_arrow;
    const auto resizing = [&](int bits) -> SDL_Cursor* {
      // bit 1=right, 2=bottom, 4=left, 8=top: a corner names its own cursor.
      if ((bits & (1 | 4)) && (bits & (2 | 8))) {
        if (bits & 8) return bits & 4 ? app.cursor_size_nw : app.cursor_size_ne;
        return bits & 4 ? app.cursor_size_sw : app.cursor_size_se;
      }
      if (bits & (1 | 4)) return app.cursor_sizewe;
      if (bits & (2 | 8)) return app.cursor_sizens;
      return nullptr;
    };
    const DockSplitAxis divider = app.dock_split_drag.side != Dock::None
        ? (app.dock_split_drag.side == Dock::Top ||
           app.dock_split_drag.side == Dock::Bottom
               ? DockSplitAxis::Horizontal : DockSplitAxis::Vertical)
        : dock_split_at(app, mx, my);
    int edge = 0;
    for (FloatWindow* w : floats)
      if (w->resize) {
        edge = w->resize;
        break;
      }
    if (!edge) edge = float_resize_at(app, mx, my);
    if (app.dragging_split) {
      pointer = app.tile_axis == TileAxis::Rows
                    ? app.cursor_sizens : app.cursor_sizewe;
    } else if (app.dragging_diff_overview) {
      pointer = app.cursor_hand;
    } else if (moving_float) {
      pointer = app.cursor_sizeall;
    } else if (resizing_float) {
      pointer = resizing(edge);
    } else if (divider == DockSplitAxis::Horizontal) {
      pointer = app.cursor_sizewe;
    } else if (divider == DockSplitAxis::Vertical) {
      pointer = app.cursor_sizens;
    } else if (edge) {
      pointer = resizing(edge);
    } else if (context_link_at(app, mx, my)) {
      pointer = app.cursor_hand;
    } else if (!float_at(app, mx, my)) {
      const auto within = [&](const SDL_Rect& r, int pad = 0) {
        return mx >= r.x - pad && mx < r.x + r.w + pad &&
               my >= r.y - pad && my < r.y + r.h + pad;
      };
      if (app.split && within(tile_mode_button_rect(app))) {
        pointer = app.cursor_hand;
      } else if (app.split && within(tile_divider_rect(app), 2) &&
                  !(app.diff_mode && within(tile_diff_track_rect(app)))) {
        pointer = app.tile_axis == TileAxis::Rows
                      ? app.cursor_sizens : app.cursor_sizewe;
      } else if (app.split && app.diff_mode &&
                  within(tile_diff_track_rect(app))) {
        pointer = app.cursor_hand;
      } else if (app.hover.col_gap) {
        pointer = app.cursor_sizewe;
      } else if (app.hover.image_handle) {
        pointer = app.cursor_size_se;
      } else if (app.hover.grip != xanatoo::INVALID_NODE) {
        pointer = app.cursor_sizeall;
      } else if (!app.hover.href.empty() || !app.hover.bookmark.empty() ||
                 !app.hover.annotation.empty() || app.hover.quote >= 0 ||
                 app.hover.stretch != xanatoo::INVALID_NODE ||
                 app.hover.list_fold != xanatoo::INVALID_NODE ||
                 app.hover.image != xanatoo::INVALID_NODE ||
                 app.hover.task != xanatoo::INVALID_NODE || app.hover.chrome_notice ||
                 app.hover.transclude != xanatoo::INVALID_NODE) {
        pointer = app.cursor_hand;
      }
    }
    set_cursor(app, pointer);
  }
  app.ui.end_frame(app.sdl);
  draw_touch_hold(app);
  if (stick_cursor_visible(app)) {
    draw_stick_cursor(app, app.sdl, static_cast<int>(app.stick_cx),
                      static_cast<int>(app.stick_cy),
                      app.stick_lmb || app.stick_rmb);
  }
  app.renderer.present();
  apply_ui_scale(app);
}

// Offer a selection the user just made in the focused view as the primary
// selection (middle-click paste in other programs): a drag once the button
// is released, double / shift clicks, Shift+arrows, Select All. Selections
// set by the program (history restore, passage addresses, undo, find) and
// switching to a view or buffer that already has one are only noted. An
// emptied selection leaves the last text in place, as X11 programs do.
static void publish_primary_selection(App& app) {
  const bool user = std::exchange(app.primary_sel_input, false);
  if (app.dragging_sel || app.focus < 0 || app.focus >= kMaxViews) return;
  Pane& p = app.panes[app.focus];
  App::PrimarySel& last = app.primary_sel;
  if (last.pane == app.focus && last.buf == p.buf &&
      last.sel.anchor == p.sel.anchor && last.sel.focus == p.sel.focus)
    return;
  const bool same_view = last.pane == app.focus && last.buf == p.buf;
  last = {app.focus, p.buf, p.sel};
  if (!user || !same_view || p.sel.empty() || !find_buf(app, p.buf)) return;
  const std::string text = copied_text(app, p);
  if (!text.empty()) xanatoo::sdl::set_primary_selection(text);
}

void note_program_selection(App& app) {
  if (app.focus < 0 || app.focus >= kMaxViews) return;
  const Pane& p = app.panes[app.focus];
  app.primary_sel = {app.focus, p.buf, p.sel};
}

void run_frame(App& app) {
  SDL_Event ev;
  while (SDL_PollEvent(&ev)) handle_event(app, ev);
  update_touch(app);
  sync_nav_history(app);  // capture scroll before a completed fetch swaps pages
  poll_fetches(app);
  poll_download_folder_picks(app);
  poll_subscriptions(app);
  update_audio(app);
  sync_nav_history(app);  // apply Back/Forward restore after remote loads
  autosave_drafts(app);
  autosave_session(app);
  poll_shared_state(app);
  update_split_preview(app);
  update_sitemap(app);
  poll_search_engine_adds(app);
  update_related_sites(app);
  update_nostr_post(app);
  update_page_panel(app);
  sync_window_size(app);
  relayout_if_needed(app);
  run_chrome(app);
  // UI commands (Open, Diff demo, New tab, …) often replace documents after
  // the first layout pass. Relayout before draw so glyphs/carets never
  // reference node ids from the previous document tree.
  // Stick scrolling may leave the materialised band: scroll before the
  // relayout so the exposed raster strip is drawn from a complete layout.
  poll_gamepad_axes(app);
  relayout_if_needed(app);
  apply_pending_scroll(app);
  publish_primary_selection(app);  // after menu / toolbar commands
  poll_images(app);
  pump_page_tiles(app);
  if (!media_fullscreen(app)) {  // all under the picture
    draw_panes(app);
    draw_dock_gutters(app);
    draw_split_chrome(app);
  }
  // Freeze ImGui's draw lists before painting native frames. Rich panel forms
  // now composite inside float_stack rather than across everything at the end.
  app.ui.prepare_render();
  if (!media_fullscreen(app))
    app.ui.render_embedded_panel(app.sdl, xanatoo::UiPanelLayer::EditBar);
  if (app.focus < kCentreViews && !media_fullscreen(app))
    app.ui.render_embedded_panel(app.sdl, xanatoo::UiPanelLayer::FindBar);
  draw_floats(app);
  // The status line is fixed chrome and rejects pointer input in its strip.
  // Paint it after free windows too, so a very tall panel cannot cover it.
  draw_status_bar(app);
  draw_dock_preview(app);
  draw_hover_tooltip(app);
  draw_media_fullscreen(app);  // over everything but ImGui's menus
  present_frame(app);
}

}  // namespace xanatoo::app
