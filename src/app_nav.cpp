// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <utility>

#include "xanatoo/app.hpp"
#include "xanatoo/audio.hpp"
#include "xanatoo/fixed_layout.hpp"
#include "xanatoo/epub_import.hpp"
#include "xanatoo/file_url.hpp"
#include "xanatoo/gallery.hpp"
#include "xanatoo/gemini.hpp"
#include "xanatoo/gopher.hpp"
#include "xanatoo/spartan.hpp"
#include "xanatoo/nex.hpp"
#include "xanatoo/finger.hpp"
#include "xanatoo/lemmy.hpp"
#include "xanatoo/nostr.hpp"
#include "xanatoo/parse.hpp"
#include "xanatoo/paths.hpp"
#include "xanatoo/reddit.hpp"
#include "xanatoo/revstore.hpp"
#include "xanatoo/sidestore.hpp"
#include "xanatoo/native.hpp"
#include "xanatoo/statedb.hpp"
#include "xanatoo/util.hpp"

namespace fs = std::filesystem;

namespace xanatoo::app {

namespace {

// The visit view `p` currently shows (0 = none recorded).
int64_t current_visit(const Pane& p) {
  const NavHistory& h = p.nav;
  return h.pos >= 0 && h.pos < static_cast<int>(h.entries.size())
             ? h.entries[static_cast<size_t>(h.pos)].visit
             : 0;
}

// Wall-clock milliseconds of a file time (portable: no clock_cast).
int64_t file_time_ms(fs::file_time_type t) {
  const auto sys = std::chrono::system_clock::now() +
      std::chrono::duration_cast<std::chrono::system_clock::duration>(
          t - fs::file_time_type::clock::now());
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             sys.time_since_epoch()).count();
}

// The observation of the page in buffer `b`, stored once per load. Local
// files are observed here (text hash, mtime, size); generated pages have
// none.
int64_t buffer_observation(DocBuffer& b) {
  if (b.placeholder) return 0;
  if (b.observation_id) return b.observation_id;
  if (b.observed.empty()) {
    std::error_code ec;
    if (b.path.empty() || !fs::is_regular_file(b.path, ec)) return 0;
    xanatoo::PageObservation& o = b.observed;
    o.url = canonical_address(b.path);
    o.time = xanatoo::now_epoch_ms();
    const auto mtime = fs::last_write_time(b.path, ec);
    if (!ec) o.last_modified = std::to_string(file_time_ms(mtime));
    const auto size = fs::file_size(b.path, ec);
    if (!ec) o.size = static_cast<int64_t>(size);
    o.text_hash = xanatoo::document_text_hash(b.doc);
  }
  b.observation_id = xanatoo::store_observation(b.observed);
  return b.observation_id;
}

// Visit log (visits.hpp, docs/HISTORY.md "B"): the current entry of view
// `pi`, which nav_history_push just made (`pushed`) or which was there
// already (an entry restored with the session has no visit yet; a Reload
// hint records the page again). `prev`: the visit shown before; `hint`:
// how it was asked for.
void record_visit(App& app, int pi, bool pushed, int64_t prev,
                  std::optional<xanatoo::VisitKind> hint) {
  using xanatoo::VisitKind;
  if (xanatoo::no_save() || !app.record_visits) return;
  Pane& p = app.panes[pi];
  NavHistory& h = p.nav;
  if (h.pos < 0 || h.pos >= static_cast<int>(h.entries.size())) return;
  NavHistoryEntry& cur = h.entries[static_cast<size_t>(h.pos)];
  DocBuffer* b = find_buf(app, p.buf);
  if (b && b->placeholder) {  // recorded once the page is there
    if (hint) h.pending_kind = hint;
    return;
  }
  const bool reload = hint == VisitKind::Reload;
  if (hint == VisitKind::History && h.pending_resume && h.pending_from) {
    // Back at a visit picked in the History panel: no new visit, the
    // entry is that one, so browsing on continues its trail.
    h.pending_resume = false;
    const int64_t left = pushed ? prev : cur.visit;
    if (left && left != h.pending_from)
      xanatoo::store_visit_leave(left, xanatoo::now_epoch_ms());
    cur.visit = h.pending_from;
    history_changed(app);
    return;
  }
  if (!pushed && cur.visit && !reload) return;  // the same page, shown again
  xanatoo::VisitRecord v;
  if (reload && cur.visit) {
    v.kind = VisitKind::Reload;
    v.source = cur.visit;
  } else if (!cur.via.href.empty()) {
    v.kind = VisitKind::Link;
    v.source = prev;
    v.href = cur.via.href;
    if (cur.via.node != xanatoo::INVALID_NODE)
      v.source_node = std::to_string(cur.via.node);
  } else if (hint && !reload) {
    v.kind = *hint;
    if (*hint == VisitKind::Restore)
      v.source = xanatoo::latest_visit_of(cur.url);  // across restarts
    else if (*hint == VisitKind::History && h.pending_from)
      v.source = h.pending_from;  // continues the picked visit's trail
    else if (!xanatoo::visit_kind_starts_trail(*hint))
      v.source = prev;
  } else if (b && b->origin_kind) {
    v.kind = *b->origin_kind;
    v.source = b->origin_visit;
    b->origin_kind.reset();
    b->origin_visit = 0;
  } else {
    v.kind = VisitKind::Nav;
    v.source = prev;
  }
  v.address = cur.url;
  v.time = xanatoo::now_epoch_ms();
  if (b) {
    v.observation = buffer_observation(*b);
    v.title = b->doc.meta.title;
  }
  v.window = xanatoo::visit_instance_id() + "/" + std::to_string(pi);
  const int64_t left = pushed ? prev : cur.visit;
  if (const int64_t id = xanatoo::store_visit(v)) {
    if (left) xanatoo::store_visit_leave(left, v.time);
    cur.visit = id;
    history_changed(app);
  }
}

}  // namespace

void record_reload_visit(App& app, int pi) {
  if (pi < 0 || pi >= kMaxViews) return;
  record_visit(app, pi, false, 0, xanatoo::VisitKind::Reload);
}

namespace {

// The recent addresses (History menu, session) and the URL bar's ring.
void note_visited_address(App& app, const std::string& url) {
  if (app.visited.empty() || app.visited.back() != url) {
    app.visited.push_back(url);
    if (static_cast<int>(app.visited.size()) > kMaxNavHistory)
      app.visited.erase(app.visited.begin());
  }
  // The URL bar's history is also the existing persistent URL minibuffer
  // ring. A single line per address, most recent last, honours --no-save.
  if (url.find('\n') == std::string::npos &&
      url.find('\r') == std::string::npos) {
    xanatoo::record_input_history("url", app.minibuf_hist_url, url);
  }
}

}  // namespace

void record_media_visit(App& app, int pi, const std::string& url,
                        std::optional<xanatoo::VisitKind> hint, int64_t from,
                        const NavLink& via) {
  using xanatoo::VisitKind;
  if (url.empty()) return;
  const std::string address = canonical_media_address(url);
  note_visited_address(app, address);
  if (xanatoo::no_save() || !app.record_visits) return;
  const bool in_view = pi >= 0 && pi < kMaxViews;
  const int64_t prev = in_view ? current_visit(app.panes[pi]) : 0;
  xanatoo::VisitRecord v;
  if (!via.href.empty()) {
    v.kind = VisitKind::Link;
    v.source = prev;
    v.href = via.href;
    if (via.node != xanatoo::INVALID_NODE)
      v.source_node = std::to_string(via.node);
  } else if (hint) {
    v.kind = *hint;
    if (*hint == VisitKind::History && from)
      v.source = from;  // continues the picked visit's trail
    else if (!xanatoo::visit_kind_starts_trail(*hint))
      v.source = prev;
  } else {
    v.kind = VisitKind::Nav;
    v.source = prev;
  }
  v.address = address;
  v.time = xanatoo::now_epoch_ms();
  v.title = app.audio.title;
  v.window = xanatoo::visit_instance_id() + "/" +
             (in_view ? std::to_string(pi) : std::string("media"));
  if (const int64_t id = xanatoo::store_visit(v)) {
    app.audio.visit = id;  // left when the player stops or plays another
    history_changed(app);
  }
}

void push_nav_history(App& app, int pi, const std::string& raw) {
  if (raw.empty() || pi < 0 || pi >= kMaxViews) return;
  // Local paths are stored as file: URLs, like pane_address shows them.
  const std::string url = canonical_address(raw);
  note_visited_address(app, url);
  split_history_note(app, pi, raw);  // no-op unless split browsing (paths)
  Pane& p = app.panes[pi];
  NavHistory& h = p.nav;
  NavHistoryEntry e;
  e.url = url;
  if (pane_address(app, p) == url) {
    e.sel = p.sel;
    e.scroll = p.scroll_y;
  }
  if (h.pos >= 0 && h.pos < static_cast<int>(h.entries.size()) &&
      h.pending_link.from_url == h.entries[static_cast<size_t>(h.pos)].url)
    e.via = h.pending_link;
  h.pending_link = {};
  h.restoring = -1;
  const int64_t prev = current_visit(p);
  std::optional<xanatoo::VisitKind> hint;
  std::swap(hint, h.pending_kind);
  const bool pushed = nav_history_push(h, std::move(e), kMaxNavHistory);
  record_visit(app, pi, pushed, prev, hint);
}

void sync_nav_history(App& app) {
  for (int pi : live_views(app)) {
    if (split_browsing(app) && pi < kCentreViews) continue;
    Pane& p = app.panes[pi];
    NavHistory& h = p.nav;
    const std::string visible = pane_address(app, p);
    if (h.restoring >= 0) {
      if (h.restoring >= static_cast<int>(h.entries.size()) ||
          h.restoring != h.pos) {
        h.restoring = -1;
        continue;
      }
      const NavHistoryEntry& e = h.entries[static_cast<size_t>(h.restoring)];
      if (visible != e.url || pane_fetch(app, pi)) continue;
      if (const DocBuffer* b = find_buf(app, p.buf); b && b->placeholder)
        continue;
      p.sel = e.sel;
      xanatoo::ensure_valid_caret(doc_of(app, p), p.sel.focus);
      xanatoo::ensure_valid_caret(doc_of(app, p), p.sel.anchor);
      p.scroll_y = e.scroll;
      p.view_anchor.reset();
      h.restoring = -1;
      app.needs_layout = true;
    } else {
      // Session restoration may still have a saved scroll queued for a
      // remote page. Do not overwrite it with the Loading page's top.
      if ((pi >= kCentreViews || app.pending_scroll[pi].y < 0.f) &&
          !(find_buf(app, p.buf) && buf(app, p.buf).placeholder))
        nav_history_capture(h, visible, p.sel, p.scroll_y);
    }
  }
}

const NavLink* forward_nav_link(App& app, int pi) {
  if (pi < 0 || pi >= kMaxViews ||
      (split_browsing(app) && pi < kCentreViews))
    return nullptr;
  Pane& p = app.panes[pi];
  return nav_history_forward_link(p.nav, pane_address(app, p));
}

const NavLink* pending_nav_link(App& app, int pi) {
  if (pi < 0 || pi >= kMaxViews ||
      (split_browsing(app) && pi < kCentreViews))
    return nullptr;  // Trail marks its link at once (split_trail_href)
  const FetchJob* f = pane_fetch(app, pi);
  if (!f || f->purpose != FetchPurpose::Navigate) return nullptr;
  Pane& p = app.panes[pi];
  const NavLink& l = p.nav.pending_link;
  if (l.href.empty() || l.from_url != pane_address(app, p)) return nullptr;
  return &l;
}

void update_open_tab_links(App& app, int pi) {
  Pane& p = app.panes[pi];
  OpenTabLinks& ol = p.open_tab_links;
  std::vector<std::string> pages;
  for (BufferId id : app.tab_order) {
    if (id == p.buf) continue;
    const DocBuffer* b = find_buf(app, id);
    if (!b || b->path.empty()) continue;
    pages.push_back(page_address(xanatoo::parse_locator(b->path).base));
  }
  std::sort(pages.begin(), pages.end());
  pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
  std::string key;
  for (const std::string& s : pages) key += s + '\n';
  if (ol.valid && ol.layout_gen == p.layout.generation && ol.tabs_key == key)
    return;
  ol.valid = true;
  ol.layout_gen = p.layout.generation;
  ol.tabs_key = std::move(key);
  ol.spans.clear();
  if (pages.empty()) return;
  const std::string own = xanatoo::parse_locator(pane_address(app, p)).base;
  const xanatoo::Document& d = doc_of(app, p);
  std::vector<std::pair<xanatoo::NodeId, size_t>> seen;
  for (const auto& pg : p.layout.glyphs) {
    if (pg.kind != xanatoo::InlineKind::Link ||
        pg.node == xanatoo::INVALID_NODE || pg.node >= d.next_id)
      continue;
    const std::pair<xanatoo::NodeId, size_t> k{pg.node, pg.span_index};
    if (!seen.empty() && seen.back() == k) continue;  // same run
    if (std::find(seen.begin(), seen.end(), k) != seen.end()) continue;
    seen.push_back(k);
    const xanatoo::Node& n = d.get(pg.node);
    if (pg.span_index >= n.spans.size()) continue;
    const std::string& href = n.spans[pg.span_index].href;
    if (href.empty() || href[0] == '#') continue;
    const LinkTarget t = resolve_link(app, pi, href);
    if (t.kind != LinkKind::Doc && t.kind != LinkKind::Remote) continue;
    const std::string page =
        page_address(xanatoo::parse_locator(t.url).base);
    if (page.empty() || page == own) continue;
    if (std::binary_search(pages.begin(), pages.end(), page))
      ol.spans.push_back(k);
  }
}

void update_playing_links(App& app, int pi) {
  Pane& p = app.panes[pi];
  PlayingLinks& pl = p.playing_links;
  const std::string& url =
      app.audio.state == AudioState::Idle ? std::string() : app.audio.url;
  if (pl.valid && pl.layout_gen == p.layout.generation && pl.url == url)
    return;
  pl.valid = true;
  pl.layout_gen = p.layout.generation;
  pl.url = url;
  pl.spans.clear();
  if (url.empty()) return;
  const xanatoo::Document& d = doc_of(app, p);
  std::vector<std::pair<xanatoo::NodeId, size_t>> seen;
  for (const auto& pg : p.layout.glyphs) {
    if (pg.kind != xanatoo::InlineKind::Link ||
        pg.node == xanatoo::INVALID_NODE || pg.node >= d.next_id)
      continue;
    const std::pair<xanatoo::NodeId, size_t> k{pg.node, pg.span_index};
    if (!seen.empty() && seen.back() == k) continue;  // same run
    if (std::find(seen.begin(), seen.end(), k) != seen.end()) continue;
    seen.push_back(k);
    const xanatoo::Node& n = d.get(pg.node);
    if (pg.span_index >= n.spans.size()) continue;
    const std::string& href = n.spans[pg.span_index].href;
    if (href.empty() || href[0] == '#') continue;
    // The address play_audio got for it: the base, a file: URL as its path.
    const LinkTarget t = resolve_link(app, pi, href);
    if (t.kind == LinkKind::None || t.kind == LinkKind::Anchor || t.url.empty())
      continue;
    std::string target = xanatoo::parse_locator(t.url).base;
    if (xanatoo::is_file_url(target)) target = xanatoo::file_url_path(target);
    if (target == url) pl.spans.push_back(k);
  }
}

bool can_nav_step(App& app, int dir) {
  if (split_browsing(app) && app.focus < kCentreViews)
    return split_history_can_step(app, dir);
  const NavHistory& h = focused_pane(app).nav;
  const int to = h.pos + dir;
  return h.pos >= 0 && to >= 0 && to < static_cast<int>(h.entries.size());
}

namespace {
// Moves the focused view's trail to entry `to` (Back / Forward, or a jump
// over several entries) without recording a new visit.
bool nav_go(App& app, int to) {
  Pane& p = focused_pane(app);
  NavHistory& h = p.nav;
  if (h.pos < 0 || to < 0 || to >= static_cast<int>(h.entries.size()))
    return false;
  nav_history_capture(h, pane_address(app, p), p.sel, p.scroll_y);
  // A copy: navigation may replace the view's buffer, never its trail,
  // but keep the string independent of the vector anyway.
  const std::string url = h.entries[static_cast<size_t>(to)].url;
  h.restoring = to;
  h.pending_link = {};
  if (!navigate_to_url(app, url, /*record_history=*/false)) {
    h.restoring = -1;
    return false;
  }
  h.pos = to;
  sync_nav_history(app);  // cached/local pages can be restored immediately
  return true;
}
}  // namespace

bool nav_step(App& app, int dir) {
  if (split_browsing(app) && app.focus < kCentreViews)
    return split_history_step(app, dir);
  if (!can_nav_step(app, dir)) return false;
  return nav_go(app, focused_pane(app).nav.pos + dir);
}

bool nav_rewind_to_visit(App& app, const std::vector<int64_t>& visits) {
  if (visits.empty()) return false;
  // The focused view first, then the other views.
  std::vector<int> order{app.focus};
  for (int pi : live_views(app))
    if (pi != app.focus) order.push_back(pi);
  for (int pi : order) {
    if (pi < 0 || pi >= kMaxViews) continue;
    if (split_browsing(app) && pi < kCentreViews) continue;  // split trail
    const NavHistory& h = app.panes[pi].nav;
    if (h.pos < 0) continue;
    // The entry nearest to the current one, should the page be there twice.
    int best = -1;
    for (int i = 0; i < static_cast<int>(h.entries.size()); ++i) {
      const int64_t v = h.entries[static_cast<size_t>(i)].visit;
      if (v == 0 || std::find(visits.begin(), visits.end(), v) == visits.end())
        continue;
      if (best < 0 || std::abs(i - h.pos) < std::abs(best - h.pos)) best = i;
    }
    if (best < 0) continue;
    if (pi != app.focus) focus_view(app, pi);
    if (best == h.pos) return true;  // shown already
    return nav_go(app, best);
  }
  return false;
}

namespace {
auto bookmark_it(App& app, const std::string& url) {
  const std::string want = canonical_address(url);
  return std::find_if(app.bookmarks.begin(), app.bookmarks.end(),
                      [&](const xanatoo::Bookmark& b) {
                        return b.address == url ||
                               canonical_address(b.address) == want;
                      });
}
}  // namespace

const xanatoo::Bookmark* find_bookmark(App& app, const std::string& address) {
  auto it = bookmark_it(app, address);
  return it == app.bookmarks.end() ? nullptr : &*it;
}

bool is_bookmarked(App& app, const std::string& url) {
  return !url.empty() && bookmark_it(app, url) != app.bookmarks.end();
}

void add_bookmark(App& app, xanatoo::Bookmark b) {
  if (b.address.empty()) {
    flash_status(app, "Nothing to bookmark (no path/URL)");
    return;
  }
  if (is_bookmarked(app, b.address)) {
    flash_status(app, "Already bookmarked");
    return;
  }
  const std::string what = b.quote.empty() ? b.address : "“" + b.quote + "”";
  app.bookmarks.push_back(b);
  ++app.bookmarks_gen;
  if (xanatoo::store_bookmark_add(b, app.bookmarks))
    flash_status(app, "Bookmarked " + what);
  else
    flash_status(app, "Bookmark added (could not persist to disk)");
}

void remove_bookmark(App& app, const std::string& url) {
  if (url.empty()) return;
  auto it = bookmark_it(app, url);
  if (it == app.bookmarks.end()) {
    flash_status(app, "Not in bookmarks");
    return;
  }
  const std::string stored = it->address;
  app.bookmarks.erase(it);
  ++app.bookmarks_gen;
  if (xanatoo::store_bookmark_remove(stored, app.bookmarks))
    flash_status(app, "Removed bookmark");
  else
    flash_status(app, "Bookmark removed (could not persist to disk)");
}

void poll_shared_state(App& app) {
  // An open prompt may be stepping through a ring: wait until it closes.
  const uint64_t now = xanatoo::sdl::ticks();
  if (prompt_active(app) || now - app.shared_state_poll_ms < 1000) return;
  app.shared_state_poll_ms = now;
  if (!xanatoo::statedb_changed_elsewhere()) return;
  history_changed(app);
  std::vector<xanatoo::Bookmark> marks = xanatoo::stored_bookmarks();
  auto same = [](const xanatoo::Bookmark& a, const xanatoo::Bookmark& b) {
    return a.address == b.address && a.title == b.title &&
           a.quote == b.quote && a.created == b.created;
  };
  if (!std::equal(marks.begin(), marks.end(), app.bookmarks.begin(),
                  app.bookmarks.end(), same)) {
    app.bookmarks = std::move(marks);
    ++app.bookmarks_gen;
  }
  app.minibuf_hist_url = xanatoo::stored_input_history("url");
  app.minibuf_hist_path = xanatoo::stored_input_history("path");
}

static bool is_smallnet_url(std::string_view u) {
  return xanatoo::is_gemini_url(u) || xanatoo::is_gopher_url(u) ||
         xanatoo::is_spartan_url(u) || xanatoo::is_nex_url(u) ||
         xanatoo::is_finger_url(u);
}

static std::string resolve_smallnet_url(std::string_view base,
                                        std::string_view ref) {
  if (xanatoo::is_gemini_url(base))
    return xanatoo::resolve_gemini_url(base, ref);
  if (xanatoo::is_gopher_url(base))
    return xanatoo::resolve_gopher_url(base, ref);
  if (xanatoo::is_spartan_url(base))
    return xanatoo::resolve_spartan_url(base, ref);
  if (xanatoo::is_nex_url(base))
    return xanatoo::resolve_nex_url(base, ref);
  return {};
}

bool is_inapp_doc_href(App& app, const std::string& href) {
  if (href.empty() || href[0] == '#') return false;
  if (is_in_app_remote_url(href)) return false;
  if (href.rfind("http://", 0) == 0 || href.rfind("https://", 0) == 0 ||
      href.rfind("mailto:", 0) == 0)
    return false;
  // Scheme with :// → external; bare paths and file-looking names → in-app.
  if (href.find("://") != std::string::npos) return false;
  return true;
}

std::string resolve_doc_href(App& app, const std::string& href,
                             const std::string& base_path) {
  if (href.empty()) return {};
  fs::path hp(href);
  if (hp.is_absolute()) return href;
  std::string base =
      base_path.empty() ? path_of(app, focused_pane(app)) : base_path;
  if (!base.empty() && !is_smallnet_url(base)) {
    fs::path parent = fs::path(base).parent_path();
    if (!parent.empty()) {
      std::error_code ec;
      auto canon = fs::weakly_canonical(parent / href, ec);
      if (!ec) return canon.string();
      return (parent / href).string();
    }
  }
  // Fall back to the installed data (examples/, …).
  std::string found = find_data_file(app, href.c_str());
  if (!found.empty()) return found;
  return href;
}

std::string link_base_url(App& app, int pi) {
  const std::string& page = path_of(app, app.panes[pi]);
  if (is_smallnet_url(page)) return page;
  if (!page.empty()) return {};  // local file / other service
  return app.gemini_current_url;
}

LinkTarget resolve_link(App& app, int pi, const std::string& href) {
  LinkTarget t;
  Pane& p = app.panes[pi];
  if (auto dest = xanatoo::jump_to_href(doc_of(app, p), href)) {
    t.kind = LinkKind::Anchor;
    t.dest = *dest;
    return t;
  }
  if (href.empty() || href[0] == '#') return t;
  std::string abs = href;
  const std::string base = link_base_url(app, pi);
  if (!is_smallnet_url(abs) && !base.empty()) {
    std::string r = resolve_smallnet_url(base, abs);
    if (!r.empty()) abs = std::move(r);
  }
  if (xanatoo::is_file_url(abs)) {
    // file:///a/b.md#x: '#' in the path is %23, so the first '#' is ours.
    std::string url = abs;
    if (size_t hash = url.find('#'); hash != std::string::npos) {
      t.fragment = url.substr(hash);
      url.resize(hash);
    }
    xanatoo::Locator loc = xanatoo::parse_locator(url);
    loc.base = xanatoo::file_url_path(loc.base);
    if (loc.base.empty()) return t;  // another host: nothing to open
    t.kind = LinkKind::Doc;
    t.url = xanatoo::format_locator(loc);
    return t;
  }
  if (is_in_app_remote_url(abs)) {
    t.kind = LinkKind::Remote;
    t.url = std::move(abs);
    return t;
  }
  if (is_inapp_doc_href(app, xanatoo::parse_locator(abs).base)) {
    std::string path = abs;
    if (size_t hash = path.find('#'); hash != std::string::npos) {
      t.fragment = path.substr(hash);
      path.resize(hash);
    }
    // t.url keeps the pipes: "other.md//id:ch3" opens as a cut.
    xanatoo::Locator loc = xanatoo::parse_locator(path);
    // Relative links resolve beside the page's address; an import's own
    // files (extracted media) beside the derived Markdown it was read from.
    const DocBuffer& pb = buf(app, p.buf);
    std::string resolved = resolve_doc_href(app, loc.base, pb.path);
    if (!pb.content_path.empty()) {
      std::error_code ec;
      std::string derived = resolve_doc_href(app, loc.base, pb.content_path);
      if (!fs::exists(resolved, ec) && fs::exists(derived, ec))
        resolved = std::move(derived);
    }
    loc.base = std::move(resolved);
    t.kind = LinkKind::Doc;
    t.url = xanatoo::format_locator(loc);
    return t;
  }
  t.kind = LinkKind::External;
  t.url = href;
  return t;
}

// Buffer pane `pi` loads a new page into: its own (unsaved edits go to a
// draft first), unless the other visible pane shows the same buffer or the
// draft could not be written (then a new tab).
static BufferId page_buffer(App& app, int pi) {
  Pane& p = app.panes[pi];
  const bool shared = shown_anywhere_else(app, p.buf, pi);
  const DocBuffer* b = find_buf(app, p.buf);
  if (b && !shared && stash_draft(app, p.buf)) return p.buf;
  return add_buffer(app, xanatoo::parse_document("", {}), {});
}

void show_doc_in_pane(App& app, int pi, xanatoo::Document doc,
                      const std::string& label,
                      std::optional<std::string> raw_source,
                      std::string content_path,
                      xanatoo::PageObservation observed) {
  Pane& p = app.panes[pi];
  p.buf = page_buffer(app, pi);
  DocBuffer& b = buf(app, p.buf);
  b.doc = std::move(doc);
  b.path = label;
  if (content_path.empty()) content_path = derived_content_path(label);
  b.content_path = content_path == label ? std::string() : std::move(content_path);
  b.raw_source = std::move(raw_source);
  b.observed = std::move(observed);
  b.observation_id = 0;
  b.origin_kind.reset();
  b.origin_visit = 0;
  b.dirty = false;
  b.placeholder = false;
  attach_draft(app, p.buf);
  xanatoo::Document& nd = b.doc;
  p.sel = {};
  xanatoo::ensure_valid_caret(nd, p.sel.focus);
  p.sel.anchor = p.sel.focus;
  p.scroll_y = 0.f;
  reset_pane_locator(p);  // of the old document
  // Drop stale layout boxes that still reference the previous tree —
  // otherwise the next frame can call doc.get() with dead node ids.
  p.layout = {};
  drop_height_hints_for_buf(app, p.buf);
  b.history.reset(nd, p.sel);
  note_page_seen(app, label, nd);
  refresh_views_of_buf(app, p.buf, pi, true);
  if (std::find(app.tab_order.begin(), app.tab_order.end(), p.buf) ==
      app.tab_order.end())
    app.tab_order.push_back(p.buf);
  app.needs_layout = true;
  update_title(app);
}

void open_address(App& app, const std::string& url) {
  if (app.click_new_tab)
    open_link_in_tab(app, app.focus, url);
  else
    navigate_to_url(app, url);
}

void open_link_in_tab(App& app, int pi, const std::string& href) {
  const LinkTarget t = resolve_link(app, pi, href);
  BufferId bi = kNoBuffer;
  switch (t.kind) {
    case LinkKind::None:
      return;
    case LinkKind::Anchor: {
      Pane& p = app.panes[pi];
      expand_ancestors_for_node(app, t.dest.node);
      p.sel.focus = t.dest;
      p.sel.anchor = t.dest;
      scroll_caret_into_view(app, p);
      flash_status(app, "Followed " + href);
      return;
    }
    case LinkKind::External:
      if (open_link_image(app, pi, t.url)) return;
      if (xanatoo::is_playable_url(t.url)) {
        if (play_audio(app, t.url))
          record_media_visit(app, pi, t.url, std::nullopt, 0,
                             NavLink{href, {}, xanatoo::INVALID_NODE, 0});
        return;
      }
      flash_status(app, "Opening " + t.url);
      xanatoo::open_external_url(t.url);
      return;
    case LinkKind::Doc: {
      std::string path = xanatoo::parse_locator(t.url).base;
      if (path == kDocumentsPage) {
        flash_status(app, "Use File → Local documents to open the index");
        return;
      }
      if (is_stored_original_ref(path)) {
        const std::string key = path.substr(12);
        const auto origin = xanatoo::find_document_origin(key);
        const auto body = origin ? xanatoo::load_stored_document_revision(
                                       key, origin->revision)
                                 : std::nullopt;
        if (!body) {
          flash_status(app, "Original source snapshot not found");
          return;
        }
        bi = find_buffer(app, path);
        if (bi == kNoBuffer)
          bi = add_buffer(app, xanatoo::parse_sexpr(*body), path);
        break;
      }
      std::error_code source_ec;
      if (fs::is_regular_file(path, source_ec)) {
        if (const auto saved = xanatoo::find_imported_document(
                xanatoo::file_doc_key(path)))
          path = "db:" + saved->key;
      }
      if (is_stored_document_ref(path)) {
        const auto head = xanatoo::find_stored_document(path.substr(3));
        const auto body = head ? xanatoo::load_stored_document_revision(
                                     head->key, head->working)
                               : std::nullopt;
        if (!body) {
          flash_status(app, "Local document not found");
          return;
        }
        bi = find_buffer(app, path);
        if (bi == kNoBuffer) {
          bi = add_buffer(app, xanatoo::parse_sexpr(*body), path);
          DocBuffer& b = buf(app, bi);
          b.doc_key = head->key;
          b.stored_working = head->working;
          b.stored_published = head->published;
          b.stored_title = head->title;
          b.base_id = xanatoo::content_id(
              xanatoo::serialize_document(b.doc, {}));
        }
        break;
      }
      std::error_code ec;
      if (fs::is_directory(path, ec)) {
        std::string dir = fs::weakly_canonical(path, ec).string();
        if (!ec) path = dir;
        if (!xanatoo::ends_with(path, "/")) path += '/';
      }
      bi = find_buffer(app, path);
      if (bi == kNoBuffer) {
        const std::string open_path = resolve_open_path(app, path);
        if (open_path.empty()) {
          flash_status(app, "Import failed (need pandoc on PATH)");
          return;
        }
        // The address stays the original (book.epub), not the cache file.
        bi = add_buffer(app, load_doc(app, open_path), path);
      }
      break;
    }
    case LinkKind::Remote: {
      if (const auto stored = xanatoo::find_imported_document(t.url)) {
        const std::string ref = "db:" + stored->key;
        bi = find_buffer(app, ref);
        if (bi == kNoBuffer) {
          const auto body = xanatoo::load_stored_document_revision(
              stored->key, stored->working);
          if (!body) {
            flash_status(app, "Local document not found");
            return;
          }
          bi = add_buffer(app, xanatoo::parse_sexpr(*body), ref);
          DocBuffer& b = buf(app, bi);
          b.doc_key = stored->key;
          b.stored_working = stored->working;
          b.stored_published = stored->published;
          b.stored_title = stored->title;
          b.base_id = xanatoo::content_id(
              xanatoo::serialize_document(b.doc, {}));
        }
        break;
      }
      std::string why;
      if (!remote_fetch_supported(t.url, &why)) {
        flash_status(app, why);
        return;
      }
      if (const CachedPage* c = find_cached_page(app, t.url)) {
        bi = add_buffer(app, c->doc, c->final_url);
        buf(app, bi).raw_source = c->raw_source;
        buf(app, bi).observed = c->observed;
        buf(app, bi).content_path = c->content_path;
        break;
      }
      bi = add_buffer(
          app, xanatoo::parse_document("# Loading…\n\n`" + t.url + "`\n", {}),
          t.url);
      buf(app, bi).placeholder = true;
      start_fetch(app, FetchPurpose::Tab, -1, t.url, false, bi);
      break;
    }
  }
  // Its first visit (when the tab is shown) comes from this view's page.
  if (DocBuffer* nb = find_buf(app, bi); nb && !nb->origin_kind) {
    const xanatoo::VisitKind kind =
        app.visit_hint.value_or(xanatoo::VisitKind::NewTab);
    nb->origin_kind = kind;
    nb->origin_visit = kind == xanatoo::VisitKind::History ? app.visit_from
                       : xanatoo::visit_kind_starts_trail(kind)
                           ? 0 : current_visit(app.panes[pi]);
  }
  app.visit_hint.reset();
  app.visit_from = 0;
  app.visit_resume = false;  // a new tab is a new visit
  auto& order = app.tab_order;
  if (std::find(order.begin(), order.end(), bi) == order.end()) {
    auto cur = std::find(order.begin(), order.end(), app.panes[pi].buf);
    order.insert(cur == order.end() ? cur : cur + 1, bi);
  }
  update_title(app);
  flash_status(app, "Opened in new tab: " + buf(app, bi).path);
}

std::string home_page(App& app) {
  // Fixed for now; a user-chosen home page will be looked up here first.
  std::string p = find_data_file(app, "welcome.md");
  if (p.empty()) p = xanatoo::find_default_document();
  return p;
}

std::string page_up_url(App& app) {
  Pane& fp = focused_pane(app);
  if (std::string g = gallery_up_address(app, fp); !g.empty()) return g;
  // Out of a passage first: its context needs no load.
  if (xanatoo::Locator loc = xanatoo::parse_locator(pane_address(app, fp));
      xanatoo::locator_up(loc))
    return xanatoo::format_locator(loc);
  std::string page = path_of(app, fp);
  if (page.empty()) return {};
  // Local paths go up through absolute directories.
  if (!is_in_app_remote_url(page) && page.find("://") == std::string::npos &&
      page.find(':') == std::string::npos) {
    std::error_code ec;
    fs::path abs = fs::absolute(page, ec);
    if (!ec) page = abs.lexically_normal().string();
  }
  return xanatoo::up_url(page);
}

bool navigate_to_url(App& app, std::string target, bool record_history,
                     NavLink via) {
  return navigate_pane(app, app.focus, std::move(target), record_history,
                       std::move(via));
}

bool navigate_anchor_link(App& app, int pi, const std::string& href,
                          NavLink via) {
  if (href.empty() || href[0] != '#') return false;
  xanatoo::Locator loc = xanatoo::parse_locator(pane_address(app, app.panes[pi]));
  const xanatoo::Locator anchor = xanatoo::parse_locator("page" + href);
  if (!anchor.point || loc.base.empty()) return false;
  loc.point = anchor.point;
  return navigate_pane(app, pi, xanatoo::format_locator(loc), true,
                       std::move(via));
}

static bool open_page(App& app, int pi, std::string target,
                      bool record_history);
static bool open_at(App& app, int pi, xanatoo::Locator loc,
                    bool record_history);

void refetch_page(App& app, int pi) {
  xanatoo::Locator loc =
      xanatoo::parse_locator(pane_address(app, app.panes[pi]));
  forget_cached_page(app, loc.base);
  open_at(app, pi, std::move(loc), /*record_history=*/false);
  // finish_navigate records the reloaded page as a Reload visit.
  if (pane_fetch(app, pi))
    app.panes[pi].nav.pending_kind = xanatoo::VisitKind::Reload;
}

// Pipes and fragment are view state: only the base gets opened or fetched.
bool navigate_pane(App& app, int pi, std::string target, bool record_history,
                   NavLink via) {
  std::optional<xanatoo::VisitKind> hint;
  std::swap(hint, app.visit_hint);  // only ever for this navigation
  const int64_t from = std::exchange(app.visit_from, 0);
  const bool resume = std::exchange(app.visit_resume, false);
  while (!target.empty() && (target.back() == ' ' || target.back() == '\t'))
    target.pop_back();
  if (target.empty()) return false;
  // Sound files play in the Media panel; the page stays. A Media Fragments
  // time (`#t=30`) is where it starts.
  if (const xanatoo::MediaAddress media = xanatoo::split_media_address(target);
      xanatoo::is_playable_url(media.base) &&
      !xanatoo::parse_locator(media.base).has_selectors()) {
    const std::string addr =
        media.start ? xanatoo::media_time_address(media.base, *media.start) : media.base;
    if (!play_audio(app, addr)) return false;
    if (record_history) record_media_visit(app, pi, addr, hint, from, via);
    return true;
  }
  Pane& p = app.panes[pi];
  if (record_history) {
    const std::string shown = pane_address(app, p);
    nav_history_capture(p.nav, shown, p.sel, p.scroll_y);
    p.nav.restoring = -1;  // a new navigation supersedes a pending Back
    if (!via.href.empty()) via.from_url = shown;
    p.nav.pending_link = std::move(via);
    p.nav.pending_kind = hint;
    p.nav.pending_from = from;
    p.nav.pending_resume = resume;
  }
  if (xanatoo::starts_with(target, "draft:") ||
      target == kDocumentsPage || is_stored_document_ref(target) ||
      is_stored_original_ref(target))
    return open_page(app, pi, std::move(target), record_history);
  xanatoo::Locator loc = xanatoo::parse_locator(target);
  // An image address on its own is that image in its one-image gallery.
  if (opens_as_image(loc.base) && !loc.has_selectors()) {
    loc.pipes = {{xanatoo::SelectorKind::Gallery, {}},
                 {xanatoo::SelectorKind::Item, xanatoo::gallery_item_value(0)}};
    target = xanatoo::format_locator(loc);
  }
  const bool selectors = loc.has_selectors();
  // file:///a/b.md → /a/b.md; only the path reaches the file system.
  if (xanatoo::is_file_url(loc.base)) {
    std::string path = xanatoo::file_url_path(loc.base);
    if (path.empty()) {
      flash_status(app, "Not a local file URL: " + loc.base);
      return false;
    }
    loc.base = std::move(path);
    if (!selectors) target = loc.base;
  } else {
    std::error_code ec;
    if (selectors && fs::exists(target, ec))  // a file called "a#b.md"
      return open_page(app, pi, std::move(target), record_history);
  }
  // Another view of the page on screen, nothing to load.
  if (!loc.base.empty() && same_page(loc.base, path_of(app, p)) &&
      (selectors || pane_address(app, p) != page_address(loc.base))) {
    apply_locator(app, pi, std::move(loc));
    if (record_history) push_nav_history(app, pi, pane_address(app, p));
    return true;
  }
  if (!selectors) {
    return open_page(app, pi, std::move(target), record_history);
  }
  return open_at(app, pi, std::move(loc), record_history);
}

// Load `loc.base` and show the selectors once it is there.
static bool open_at(App& app, int pi, xanatoo::Locator loc,
                    bool record_history) {
  Pane& p = app.panes[pi];
  const std::string base = loc.base;
  if (!open_page(app, pi, base, /*record_history=*/false)) return false;
  for (FetchJob& job : app.fetches)
    if (job.pane == pi && job.purpose == FetchPurpose::Navigate &&
        job.url == base) {
      job.locator = std::move(loc);  // finish_navigate shows it
      job.record_history = record_history;
      return true;
    }
  apply_locator(app, pi, std::move(loc));
  if (record_history) push_nav_history(app, pi, pane_address(app, p));
  return true;
}

static bool open_page(App& app, int pi, std::string target,
                      bool record_history) {
  Pane& p = app.panes[pi];

  if (target == kDraftsPage) {
    const int saved = app.focus;
    app.focus = pi;  // open_drafts_page shows in the focused pane
    open_drafts_page(app);
    app.focus = saved;
    if (record_history) push_nav_history(app, pi, target);
    return true;
  }
  if (target == kDocumentsPage) {
    if (!stash_or_confirm(app, p.buf)) return false;
    const int saved = app.focus;
    app.focus = pi;
    open_stored_documents_page(app);
    app.focus = saved;
    if (record_history) push_nav_history(app, pi, target);
    return true;
  }
  if (is_stored_document_ref(target)) {
    if (!open_stored_document(app, pi, target)) return false;
    if (record_history) push_nav_history(app, pi, target);
    return true;
  }
  if (is_stored_original_ref(target)) {
    if (!open_stored_original(app, pi, target)) return false;
    if (record_history) push_nav_history(app, pi, target);
    return true;
  }
  // Returning to a source alias opens its persisted editable version. The
  // immutable original is always available under db-original:doc:<id>.
  const bool remote_source = is_in_app_remote_url(target);
  std::error_code source_ec;
  if (remote_source || fs::is_regular_file(target, source_ec)) {
    const std::string source = remote_source ? target : file_doc_key(target);
    if (const auto stored = find_imported_document(source)) {
      const std::string local_ref = "db:" + stored->key;
      if (!open_stored_document(app, pi, local_ref)) return false;
      if (record_history) push_nav_history(app, pi, local_ref);
      return true;
    }
  }
  if (target == kAboutPage) {
    const int saved = app.focus;
    app.focus = pi;
    open_about_page(app);
    app.focus = saved;
    if (record_history) push_nav_history(app, pi, target);
    return true;
  }
  if (xanatoo::starts_with(target, "draft:")) {
    if (!open_draft(app, pi, target)) return false;
    if (record_history) push_nav_history(app, pi, target);
    return true;
  }
  // A remote image: a page of just that image (no fetch; the image cache
  // downloads it like any remote image). Only when known to be one: other
  // http(s) addresses named like a picture are fetched below and their
  // Content-Type decides (finish_navigate).
  if (opens_as_image(target)) {
    if (!stash_or_confirm(app, p.buf)) return false;
    show_doc_in_pane(app, pi, xanatoo::image_url_document(target), target);
    if (record_history) push_nav_history(app, pi, target);
    return true;
  }

  // Remote pages load on a worker thread; poll_fetches shows them.
  if (is_in_app_remote_url(target)) {
    std::string why;
    if (remote_fetch_supported(target, &why)) {
      if (!stash_or_confirm(app, p.buf)) return false;
      if (const CachedPage* c = find_cached_page(app, target)) {
        // Fresh from a hover preview: no second fetch.
        const std::string final_url = c->final_url;
        show_doc_in_pane(app, pi, c->doc, final_url, c->raw_source,
                         c->content_path, c->observed);
        if (xanatoo::is_gemini_url(final_url)) {
          app.gemini_current_url = final_url;
          app.edit_mode = false;
        }
        if (record_history) push_nav_history(app, pi, final_url);
        flash_status(app, "Opened " + final_url);
        return true;
      }
      start_fetch(app, FetchPurpose::Navigate, pi, target, record_history);
      return true;
    }
    flash_status(app, why);
    if (!xanatoo::is_gemini_url(target)) return false;
    // No TLS client in this build — still offer the local gemtext sample.
    std::string sample = find_data_file(app, "gemini-sample.gmi");
    if (sample.empty()) sample = find_data_file(app, "examples/gemini-sample.gmi");
    if (sample.empty() || !fs::exists(sample)) return false;
    target = sample;
  }

  // Local path (or offline fallback for gemini:// when TLS is disabled)
  std::string path = target;
  if (!fs::exists(path)) {
    std::string found = find_data_file(app, path.c_str());
    if (!found.empty()) path = found;
  }
  if (!fs::exists(path)) {
    flash_status(app, "Not found: " + target);
    if (!stash_or_confirm(app, p.buf)) return false;
    show_doc_in_pane(app, pi,
                     xanatoo::parse_document(
                         "# Could not open page\n\n`" + target +
                             "`\n\nNo such file or directory.\n",
                         "error.md"),
                     target);
    if (record_history) push_nav_history(app, pi, target);
    return true;
  }
  app.gemini_current_url.clear();
  // Directories open as a listing; one canonical spelling with a trailing
  // '/' (marks the buffer read-only, see is_foreign_path).
  if (std::error_code ec; fs::is_directory(path, ec)) {
    std::string dir = fs::weakly_canonical(path, ec).string();
    if (!ec) path = dir;
    if (!xanatoo::ends_with(path, "/")) path += '/';
  }
  // Prefer an already-open buffer for this path (avoid double-load).
  {
    std::string canon = path;
    try {
      canon = fs::weakly_canonical(path).string();
    } catch (...) {
    }
    for (const DocBuffer& b : app.buffers) {
      if (b.path.empty()) continue;
      const BufferId i = b.id;
      std::string bp = b.path;
      try {
        bp = fs::weakly_canonical(bp).string();
      } catch (...) {
      }
      if (bp == canon) {
        p.buf = i;
        p.sel = {};
        if (std::find(app.tab_order.begin(), app.tab_order.end(), i) ==
            app.tab_order.end())
          app.tab_order.push_back(i);
        xanatoo::ensure_valid_caret(doc_of(app, p), p.sel.focus);
        p.sel.anchor = p.sel.focus;
        p.scroll_y = 0.f;
        p.view_anchor.reset();
        p.layout = {};
        app.needs_layout = true;
        update_title(app);
        if (record_history) push_nav_history(app, pi, path);
        flash_status(app, "Switched to " + path);
        return true;
      }
    }
  }
  if (!stash_or_confirm(app, p.buf)) return false;
  // PDF / DjVu / comics: page sizes come from thumtoo on a worker, which may
  // take a while (docs/FIXED_LAYOUT.md); poll_fetches shows the result.
  if (xanatoo::is_fixed_layout_path(path)) {
    start_fetch(app, FetchPurpose::Navigate, pi, path, record_history);
    return true;
  }
  {
    const bool was_import = xanatoo::is_pandoc_path(path);
    std::string open_path = resolve_open_path(app, path);
    if (open_path.empty()) {
      flash_status(app, "Import failed (need pandoc on PATH)");
      return false;
    }
    // The page keeps its own address; the import's Markdown under
    // cache_dir() is only where it is read from (and media resolve).
    show_doc_in_pane(app, pi, load_doc(app, open_path), path, std::nullopt,
                     open_path);
    if (record_history) push_nav_history(app, pi, path);
    flash_status(app, was_import ? ("Imported " + path)
                               : ("Opened " + path));
  }
  return true;
}

void open_transclude_in_other_pane(App& app) {
  auto& p = focused_pane(app);
  const auto& d = doc_of(app, p);
  xanatoo::NodeId id = p.sel.focus.node;
  // Walk up to a Transclude node under the caret.
  xanatoo::NodeId tid = xanatoo::INVALID_NODE;
  for (xanatoo::NodeId cur = id; cur != xanatoo::INVALID_NODE;
       cur = d.get(cur).parent) {
    if (d.get(cur).kind == xanatoo::NodeKind::Transclude) {
      tid = cur;
      break;
    }
  }
  if (tid == xanatoo::INVALID_NODE) {
    // Fallback: first visible transclude under caret y — skip, require caret
    flash_status(app, "No include under caret");
    return;
  }
  const auto& tn = d.get(tid);
  xanatoo::NodeId local = xanatoo::resolve_transclude_local(d, tn.src);
  if (!app.split && app.focus < kCentreViews) {
    app.split = true;
    app.tile_mode = TileMode::Independent;
    app.needs_layout = true;
  }
  const int other = other_pane(app);
  if (local != xanatoo::INVALID_NODE) {
    app.panes[other].buf = p.buf;  // same doc
    app.panes[other].sel.focus = {local, 0, 0};
    app.panes[other].sel.anchor = app.panes[other].sel.focus;
    scroll_caret_into_view(app, app.panes[other]);
    flash_status(app, "Include → other pane: " + tn.src);
    return;
  }
  // External path
  std::string path_part = tn.src;
  size_t hash = path_part.find('#');
  std::string anchor;
  if (hash != std::string::npos) {
    anchor = path_part.substr(hash);
    path_part = path_part.substr(0, hash);
  }
  if (path_part.empty()) {
    flash_status(app, "Broken include: " + tn.src);
    return;
  }
  std::filesystem::path resolved = path_part;
  std::string base = content_file(buf(app, p.buf));
  if (!base.empty() && resolved.is_relative())
    resolved = std::filesystem::path(base).parent_path() / path_part;
  if (!std::filesystem::exists(resolved)) {
    flash_status(app, "Broken include (missing file): " + resolved.string());
    return;
  }
  // Load into other pane's buffer if possible — navigate_to_url uses focus.
  int saved = app.focus;
  app.focus = other;
  navigate_to_url(app, resolved.string() + (anchor.empty() ? "" : anchor));
  if (!anchor.empty()) {
    if (auto dest =
            xanatoo::jump_to_href(doc_of(app, app.panes[other]), anchor)) {
      app.panes[other].sel.focus = *dest;
      app.panes[other].sel.anchor = *dest;
      scroll_caret_into_view(app, app.panes[other]);
    }
  }
  app.focus = saved;
  flash_status(app, "Opened include in other pane: " + resolved.string());
}

void activate_at_caret(App& app) {
  if (toggle_stretch_at_caret(app)) return;
  Pane& p = focused_pane(app);
  auto& d = doc_of(app, p);
  auto cpos = xanatoo::caret_screen_pos(
      p.layout, p.sel.focus, p.scroll_y,
      static_cast<float>(app.fonts.regular.line_height()));
  if (!cpos.valid) return;
  if (auto hit =
          xanatoo::hit_test_link_ex(d, p.layout, cpos.x, cpos.y, p.scroll_y)) {
    const std::optional<std::string> href = hit->href;
    if (open_link_image(app, app.focus, *href)) return;
    if (follow_link_split(app, app.focus, *href)) return;
    if (auto dest = xanatoo::jump_to_href(d, *href)) {
      if (!split_browsing(app) &&
          navigate_anchor_link(app, app.focus, *href,
                               NavLink{*href, {}, hit->node, hit->span_index})) {
        flash_status(app, "Followed " + *href);
        reset_blink(app);
        return;
      }
      expand_ancestors_for_node(app, dest->node);
      p.sel.focus = *dest;
      p.sel.anchor = p.sel.focus;
      scroll_caret_into_view(app, p);
      flash_status(app, "Followed " + *href);
      reset_blink(app);
    } else if (!href->empty() && (*href)[0] != '#') {
      std::string abs = *href;
      const std::string base = link_base_url(app, app.focus);
      if (!is_smallnet_url(abs) && !base.empty()) {
        std::string resolved = resolve_smallnet_url(base, abs);
        if (!resolved.empty()) abs = resolved;
      }
      if (is_in_app_remote_url(abs) || xanatoo::is_file_url(abs) ||
          xanatoo::is_image_address(abs) ||
        (xanatoo::is_playable_url(abs) && !is_inapp_doc_href(app, abs))) {
        navigate_to_url(app, std::move(abs), true,
                        NavLink{*href, {}, hit->node, hit->span_index});
      } else if (is_inapp_doc_href(app, abs)) {
        navigate_to_url(app, resolve_doc_href(app, abs), true,
                        NavLink{*href, {}, hit->node, hit->span_index});
      } else {
        xanatoo::open_external_url(*href);
        flash_status(app, "Opening " + *href);
      }
    }
  }
}

}  // namespace xanatoo::app
