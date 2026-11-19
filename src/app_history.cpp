// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// History panel (WindowRole::History): the visit log, newest first, as a
// trail tree like `git log --graph` (docs/HISTORY.md "C"). Lanes come from
// history_trail.hpp; this file loads pages of visits, draws the rows and
// handles clicks, keys, the filter prompt and the tooltip.

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "xanatoo/app.hpp"
#include "xanatoo/paths.hpp"
#include "xanatoo/theme.hpp"
#include "xanatoo/visits.hpp"

namespace xanatoo::app {

namespace {

constexpr size_t kPageSize = 200;

float row_step(App& app) {
  return static_cast<float>(std::max(18, app.ui_font.line_height() + 7));
}

int lane_width(App& app) {
  return std::max(10, app.ui_font.line_height() * 2 / 3);
}

std::tm local_tm(int64_t ms) {
  const std::time_t t = static_cast<std::time_t>(ms / 1000);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  return tm;
}

std::string format_tm(const std::tm& tm, const char* fmt) {
  char buf[64];
  return std::strftime(buf, sizeof buf, fmt, &tm) ? std::string(buf) : "";
}

// The filter in effect: the prompt's text while it is being typed.
const std::string& effective_filter(const App& app) {
  return app.prompt.kind == PromptKind::HistoryFilter ? app.prompt.text
                                                      : app.history.filter;
}

// Entries, lanes and lines (with day headers) from `visits`.
void rebuild(App& app) {
  HistoryPanel& h = app.history;
  const bool filtered = !h.loaded_filter.empty();
  h.entries = xanatoo::collapse_page_runs(h.visits);
  h.lanes = xanatoo::history_lanes(
      xanatoo::trail_nodes(h.visits, h.entries, filtered));
  h.lines.clear();
  h.graph_cols = 1;
  int prev_day = -1;
  for (size_t i = 0; i < h.entries.size(); ++i) {
    h.graph_cols = std::max(h.graph_cols, h.lanes[i].width);
    const int64_t t = h.visits[h.entries[i].first].time;
    if (t > 0) {
      const std::tm tm = local_tm(t);
      const int day = (tm.tm_year + 1900) * 1000 + tm.tm_yday;
      if (day != prev_day) {
        HistoryLine d;
        d.day = format_tm(tm, "%A, %Y-%m-%d");
        d.after = static_cast<int>(i) - 1;
        h.lines.push_back(std::move(d));
        prev_day = day;
      }
    }
    HistoryLine l;
    l.entry = static_cast<int>(i);
    h.lines.push_back(std::move(l));
  }
  if (h.sel >= static_cast<int>(h.lines.size()))
    h.sel = static_cast<int>(h.lines.size()) - 1;
}

// Loads the newest visits again (at least as many as were loaded).
void reload(App& app) {
  HistoryPanel& h = app.history;
  const std::string filter = effective_filter(app);
  // The selection follows its visit, not its line.
  int64_t sel_id = 0;
  if (h.sel >= 0 && h.sel < static_cast<int>(h.lines.size()))
    if (const int e = h.lines[static_cast<size_t>(h.sel)].entry; e >= 0)
      sel_id = h.visits[h.entries[static_cast<size_t>(e)].first].id;
  const size_t want = std::max(
      kPageSize, filter == h.loaded_filter ? h.visits.size() : size_t{0});
  h.from_db = xanatoo::visit_log_available();
  if (h.from_db) {
    h.visits = xanatoo::visit_page(want, nullptr, filter);
    h.more = h.visits.size() == want;
  } else {
    // No visit log: this session's addresses, newest first, unlinked.
    h.visits.clear();
    for (auto it = app.visited.rbegin(); it != app.visited.rend(); ++it) {
      xanatoo::VisitRecord v;
      v.address = *it;
      if (xanatoo::visit_matches(v, filter)) h.visits.push_back(std::move(v));
    }
    h.more = false;
  }
  h.loaded_filter = filter;
  h.stale = false;
  rebuild(app);
  h.sel = -1;
  for (size_t li = 0; sel_id != 0 && li < h.lines.size(); ++li)
    if (const int e = h.lines[li].entry;
        e >= 0 && h.visits[h.entries[static_cast<size_t>(e)].first].id == sel_id)
      h.sel = static_cast<int>(li);
}

void load_more(App& app) {
  HistoryPanel& h = app.history;
  if (!h.more || h.visits.empty()) return;
  auto older = xanatoo::visit_page(kPageSize, &h.visits.back(), h.loaded_filter);
  h.more = older.size() == kPageSize;
  if (older.empty()) return;
  h.visits.insert(h.visits.end(), std::make_move_iterator(older.begin()),
                  std::make_move_iterator(older.end()));
  rebuild(app);
}

void refresh(App& app) {
  HistoryPanel& h = app.history;
  if (h.stale || effective_filter(app) != h.loaded_filter) reload(app);
}

const char* kind_label(xanatoo::VisitKind k) {
  switch (k) {
    case xanatoo::VisitKind::Nav: return "";
    case xanatoo::VisitKind::Link: return "link";
    case xanatoo::VisitKind::Address: return "typed";
    case xanatoo::VisitKind::Bookmark: return "bookmark";
    case xanatoo::VisitKind::History: return "history";
    case xanatoo::VisitKind::NewTab: return "new tab";
    case xanatoo::VisitKind::Restore: return "restored";
    case xanatoo::VisitKind::External: return "external";
    case xanatoo::VisitKind::Reload: return "reload";
  }
  return "";
}

std::string entry_label(const HistoryPanel& h, const xanatoo::TrailEntry& e) {
  const xanatoo::VisitRecord& v = h.visits[e.first];
  std::string label = v.title;
  // A run's title may only be on one of its visits (anchors get none).
  for (size_t k = 1; label.empty() && k < e.count; ++k)
    label = h.visits[e.first + k].title;
  if (label.empty())
    label = std::string(xanatoo::trail_page_of(v.address));
  return label;
}

// Visit ids the views show right now (their rows are marked).
std::unordered_set<int64_t> shown_visits(App& app) {
  std::unordered_set<int64_t> out;
  for (int pi : live_views(app)) {
    const NavHistory& n = app.panes[pi].nav;
    if (n.pos >= 0 && n.pos < static_cast<int>(n.entries.size()))
      if (const int64_t id = n.entries[static_cast<size_t>(n.pos)].visit)
        out.insert(id);
  }
  return out;
}

int line_at(App& app, const FloatWindow& w, int my) {
  auto [cx, cy, cw, ch] = float_client_rect(app, const_cast<FloatWindow&>(w));
  if (my < cy || my >= cy + ch) return -1;
  const int line = static_cast<int>(
      (static_cast<float>(my - cy) + w.scroll) / row_step(app));
  return line >= 0 && line < static_cast<int>(app.history.lines.size())
             ? line : -1;
}

// Opens the visit of `line`. With ViewSettings::history_rewinds (`invert`:
// Shift flips it) a visit still in a view's Back / Forward trail is stepped
// to there; otherwise, and in a new tab, it is opened as a new visit.
void open_line(App& app, int line, bool invert = false) {
  const HistoryPanel& h = app.history;
  if (line < 0 || line >= static_cast<int>(h.lines.size())) return;
  const int ei = h.lines[static_cast<size_t>(line)].entry;
  if (ei < 0) return;
  const xanatoo::TrailEntry& e = h.entries[static_cast<size_t>(ei)];
  const std::string address = h.visits[e.first].address;
  const bool rewind = app.view_settings.history_rewinds != invert &&
                      !app.click_new_tab && h.from_db;
  if (rewind) {
    std::vector<int64_t> ids;
    for (size_t k = 0; k < e.count; ++k) ids.push_back(h.visits[e.first + k].id);
    if (nav_rewind_to_visit(app, ids)) return;
  }
  // Rewinding to a visit no view holds any more: the view goes back to
  // that visit (no new one). Otherwise a new `history` visit continues
  // the picked one's trail (its newest visit).
  app.visit_hint = xanatoo::VisitKind::History;
  app.visit_from = h.visits[e.first].id;
  app.visit_resume = rewind;
  open_address(app, address);
  app.visit_hint.reset();
  app.visit_from = 0;
  app.visit_resume = false;
}

// The line showing the visit `id` (or, without a visit log, `address`).
int line_of(const HistoryPanel& h, int64_t id, const std::string& address) {
  for (size_t li = 0; li < h.lines.size(); ++li) {
    const int ei = h.lines[li].entry;
    if (ei < 0) continue;
    const xanatoo::VisitRecord& v =
        h.visits[h.entries[static_cast<size_t>(ei)].first];
    if (id != 0 ? v.id == id : v.address == address)
      return static_cast<int>(li);
  }
  return -1;
}

void scroll_to_line(App& app, int sel);

// Removes the visits of `line` from the log (a session address from
// `app.visited` without one). Views showing them keep their pages; their
// entries hang on the removed run's source, like the visits reached from
// it. The selection stays on the row that takes the line's place.
void remove_line(App& app, int line) {
  HistoryPanel& h = app.history;
  if (line < 0 || line >= static_cast<int>(h.lines.size())) return;
  const int ei = h.lines[static_cast<size_t>(line)].entry;
  if (ei < 0) return;
  const xanatoo::TrailEntry e = h.entries[static_cast<size_t>(ei)];
  const std::string label = entry_label(h, e);
  if (h.from_db) {
    std::vector<int64_t> ids;  // newest first: each relinks to the next
    for (size_t k = 0; k < e.count; ++k) ids.push_back(h.visits[e.first + k].id);
    const int64_t source = h.visits[e.first + e.count - 1].source;
    if (xanatoo::no_save()) {
      flash_status(app, "Not removed: --no-save");
      return;
    }
    if (xanatoo::remove_visits(ids) == 0) {
      flash_status(app, "Could not remove the visit");
      return;
    }
    for (Pane& p : app.panes)
      for (NavHistoryEntry& n : p.nav.entries)
        if (std::find(ids.begin(), ids.end(), n.visit) != ids.end())
          n.visit = source;
  } else {
    std::erase(app.visited, h.visits[e.first].address);
  }
  reload(app);
  // The row now at the removed line's place (or the last one).
  const int n = static_cast<int>(h.lines.size());
  int sel = std::min(line, n - 1);
  while (sel >= 0 && sel < n && h.lines[static_cast<size_t>(sel)].entry < 0)
    ++sel;
  if (sel >= n)
    for (sel = n - 1; sel >= 0 && h.lines[static_cast<size_t>(sel)].entry < 0;)
      --sel;
  h.sel = sel;
  if (sel >= 0) scroll_to_line(app, sel);
  flash_status(app, "Removed from history: " + label);
}

// Keeps line `sel` inside the client area.
void scroll_to_line(App& app, int sel) {
  FloatWindow& w = app.history_win;
  auto [cx, cy, cw, ch] = float_client_rect(app, w);
  const float step = row_step(app);
  const float top = static_cast<float>(sel) * step;
  if (top < w.scroll) w.scroll = top;
  if (top + step > w.scroll + static_cast<float>(ch))
    w.scroll = top + step - static_cast<float>(ch);
  w.scroll = std::max(0.f, w.scroll);
}

void fill_dot(App& app, int x, int y, int r, xanatoo::theme::Rgb c) {
  for (int dy = -r; dy <= r; ++dy) {
    int dx = 0;
    while ((dx + 1) * (dx + 1) + dy * dy <= r * r + r) ++dx;
    xanatoo::theme::hline(app.sdl, x - dx, x + dx, y + dy, c);
  }
}

// A 2px line (the second pixel to the right).
void thick_line(App& app, int x0, int y0, int x1, int y1,
                xanatoo::theme::Rgb c) {
  xanatoo::theme::line(app.sdl, x0, y0, x1, y1, c);
  xanatoo::theme::line(app.sdl, x0 + 1, y0, x1 + 1, y1, c);
}

}  // namespace

void history_changed(App& app) { app.history.stale = true; }

float history_panel_max_scroll(App& app, const FloatWindow& w) {
  auto [cx, cy, cw, ch] = float_client_rect(app, const_cast<FloatWindow&>(w));
  return std::max(0.f, static_cast<float>(app.history.lines.size()) *
                               row_step(app) + 8.f - static_cast<float>(ch));
}

void draw_history_panel(App& app) {
  if (!app.history_panel_open) return;
  FloatWindow& w = app.history_win;
  if (w.dock == Dock::None) {
    w.w = std::max(w.w, 240);
    w.h = std::max(w.h, 140);
  }
  refresh(app);
  HistoryPanel& h = app.history;
  auto [cx, cy, cw, ch] = float_client_rect(app, w);
  const float step = row_step(app);
  // Near the end of what is loaded: the next page of older visits.
  if (h.more && w.scroll + static_cast<float>(ch) + 4.f * step >
                    static_cast<float>(h.lines.size()) * step)
    load_more(app);
  const float max_s = history_panel_max_scroll(app, w);
  w.scroll = std::clamp(w.scroll, 0.f, max_s);

  std::string title = "History";
  if (const std::string& f = effective_filter(app); !f.empty())
    title += " \xE2\x80\x94 \xE2\x80\x9C" + f + "\xE2\x80\x9D";
  if (!draw_floating_frame(app, w, title,
                           panel_has_keys(app, KeyFocus::History)))
    return;

  // Pointer row (topmost window only).
  h.hover = -1;
  {
    const int mx = static_cast<int>(app.mouse_x);
    const int my = static_cast<int>(app.mouse_y);
    // Not under an open popup (the row menu): no hover, no tooltip.
    if (float_at(app, mx, my) == &w && mx >= cx && mx < cx + cw &&
        !app.ui.want_capture_mouse())
      h.hover = line_at(app, w, my);
  }

  SDL_Rect clip{cx, cy, cw, ch};
  xanatoo::sdl::set_clip_rect(app.sdl, &clip);
  const auto& pal = xanatoo::theme::palette();
  xanatoo::Font& font = app.ui_font;
  const int colw = lane_width(app);
  const int max_cols = std::max(1, (cw * 2 / 5) / colw);
  const int cols = std::min(h.graph_cols, max_cols);
  const int gx = cx + 6;
  const int time_x = gx + cols * colw + 6;
  const int time_w = font.measure("00:00") + 10;
  const int text_x = time_x + (h.from_db ? time_w : 0);
  const auto shown = shown_visits(app);
  const bool keys = panel_has_keys(app, KeyFocus::History);
  auto col_x = [&](int c) { return gx + std::min(c, cols - 1) * colw + colw / 2; };

  float row_y = static_cast<float>(cy) - w.scroll;
  for (int li = 0; li < static_cast<int>(h.lines.size()); ++li, row_y += step) {
    if (row_y > static_cast<float>(cy + ch)) break;
    if (row_y + step < static_cast<float>(cy)) continue;
    const HistoryLine& line = h.lines[static_cast<size_t>(li)];
    const int ry = static_cast<int>(row_y);
    const int rh = static_cast<int>(step);
    const int mid = ry + rh / 2;
    const int baseline =
        ry + (rh - font.line_height()) / 2 + font.ascender();
    if (line.entry < 0) {  // day header, lanes passing through
      if (line.after >= 0)
        for (int c : xanatoo::lanes_below(h.lanes[static_cast<size_t>(line.after)]))
          if (c < cols)
            thick_line(app, col_x(c), ry, col_x(c), ry + rh,
                       xanatoo::theme::lane_color(c));
      app.renderer.draw_text(font, static_cast<float>(time_x),
                             static_cast<float>(baseline),
                             fit_text(font, line.day, cx + cw - time_x - 16),
                             pal.text_dim.r, pal.text_dim.g, pal.text_dim.b);
      continue;
    }
    const auto& e = h.entries[static_cast<size_t>(line.entry)];
    const auto& lr = h.lanes[static_cast<size_t>(line.entry)];
    const xanatoo::VisitRecord& v = h.visits[e.first];
    const xanatoo::VisitRecord& oldest = h.visits[e.first + e.count - 1];
    bool current = false;
    for (size_t k = 0; k < e.count && !current; ++k)
      current = shown.count(h.visits[e.first + k].id) > 0;
    const bool sel = li == h.sel;
    if (sel || current || li == h.hover) {
      const auto bg = sel && keys ? pal.accent_soft
                      : sel || current ? pal.face_dark
                                       : pal.face_light;
      xanatoo::theme::fill(app.sdl, SDL_Rect{cx + 1, ry, cw - 2, rh}, bg);
    }
    // Graph.
    for (int c : lr.through)
      if (c < cols)
        thick_line(app, col_x(c), ry, col_x(c), ry + rh,
                   xanatoo::theme::lane_color(c));
    const int lx = col_x(lr.lane);
    const auto lc = xanatoo::theme::lane_color(lr.lane);
    if (lr.from_above) thick_line(app, lx, ry, lx, mid, lc);
    for (int m : lr.merges)
      if (m < cols)
        thick_line(app, col_x(m), ry, lx, mid, xanatoo::theme::lane_color(m));
    if (lr.to_below) thick_line(app, lx, mid, lx, ry + rh, lc);
    fill_dot(app, lx, mid, 4, lc);
    if (h.from_db && !lr.to_below)  // starts a trail: hollow dot
      fill_dot(app, lx, mid, 2, pal.highlight);
    // Time, title, how it was reached.
    if (h.from_db && v.time > 0)
      app.renderer.draw_text(
          font, static_cast<float>(time_x), static_cast<float>(baseline),
          format_tm(local_tm(v.time), "%H:%M"), pal.text_dim.r,
          pal.text_dim.g, pal.text_dim.b);
    std::string tag = kind_label(oldest.kind);
    if (e.count > 1)
      tag = (tag.empty() ? "" : tag + " ") + "\xC3\x97" +
            std::to_string(e.count);
    const int tag_w = tag.empty() ? 0 : font.measure(tag) + 12;
    const int avail = cx + cw - text_x - 16;
    const bool show_tag = tag_w > 0 && avail - tag_w > 80;
    const auto ink = current ? pal.accent_dark : pal.text;
    app.renderer.draw_text(
        font, static_cast<float>(text_x), static_cast<float>(baseline),
        fit_text(font, entry_label(h, e), avail - (show_tag ? tag_w : 0)),
        ink.r, ink.g, ink.b);
    if (show_tag)
      app.renderer.draw_text(
          font, static_cast<float>(cx + cw - 16 - tag_w + 12),
          static_cast<float>(baseline), tag, pal.text_dim.r, pal.text_dim.g,
          pal.text_dim.b);
  }
  if (h.lines.empty()) {
    const char* msg = !effective_filter(app).empty() ? "No matching visits"
                      : h.from_db ? "No visits recorded yet"
                                  : "No pages visited this session";
    app.renderer.draw_text(font, static_cast<float>(cx + 8),
                           static_cast<float>(cy + 8 + font.ascender()), msg,
                           pal.text_dim.r, pal.text_dim.g, pal.text_dim.b);
  }
  xanatoo::sdl::set_clip_rect(app.sdl, nullptr);
  draw_client_scrollbar(app, w, max_s);
}

void click_history_panel(App& app, FloatWindow& w, int my) {
  const int line = line_at(app, w, my);
  if (line < 0) return;
  app.history.sel = line;
  open_line(app, line, (SDL_GetModState() & KMOD_SHIFT) != 0);
}

bool right_click_history_panel(App& app, int mx, int my) {
  if (!app.history_panel_open) return false;
  FloatWindow& w = app.history_win;
  auto [cx, cy, cw, ch] = float_client_rect(app, w);
  if (mx < cx || mx >= cx + cw || my < cy || my >= cy + ch) return false;
  HistoryPanel& h = app.history;
  h.menu_id = 0;
  h.menu_address.clear();
  const int line = line_at(app, w, my);
  const int ei = line >= 0 ? h.lines[static_cast<size_t>(line)].entry : -1;
  if (ei < 0) return true;  // a day header or empty space: no menu
  const xanatoo::VisitRecord& v = h.visits[h.entries[static_cast<size_t>(ei)].first];
  h.sel = line;
  h.menu_id = v.id;
  h.menu_address = v.address;
  app.ui.open_history_menu(static_cast<float>(mx), static_cast<float>(my));
  return true;
}

std::string history_menu_label(App& app) {
  HistoryPanel& h = app.history;
  const int line = line_of(h, h.menu_id, h.menu_address);
  if (line < 0) return h.menu_address;
  const int ei = h.lines[static_cast<size_t>(line)].entry;
  return entry_label(h, h.entries[static_cast<size_t>(ei)]);
}

void history_action(App& app, const std::string& action) {
  HistoryPanel& h = app.history;
  const int64_t id = std::exchange(h.menu_id, 0);
  const std::string address = std::exchange(h.menu_address, {});
  if (address.empty()) return;
  if (action == "copy") {
    SDL_SetClipboardText(address.c_str());
    flash_status(app, "Copied " + address);
    return;
  }
  const int line = line_of(h, id, address);
  if (line < 0) return;  // gone meanwhile (another process removed it)
  h.sel = line;
  if (action == "open") {
    open_line(app, line);
  } else if (action == "open-other") {
    open_line(app, line, true);
  } else if (action == "open-tab") {
    app.click_new_tab = true;
    open_line(app, line);
    app.click_new_tab = false;
  } else if (action == "remove") {
    remove_line(app, line);
  }
}

bool history_panel_key(App& app, SDL_Keycode k, bool ctrl, bool shift) {
  HistoryPanel& h = app.history;
  const int n = static_cast<int>(h.lines.size());
  auto step_sel = [&](int delta) {
    if (n == 0) return;
    int s = h.sel < 0 ? (delta > 0 ? -1 : n) : h.sel;
    const int dir = delta > 0 ? 1 : -1;
    int moved = 0;
    int last_ok = h.sel;
    while (moved < std::abs(delta)) {
      s += dir;
      if (s < 0 || s >= n) break;
      if (h.lines[static_cast<size_t>(s)].entry >= 0) {
        last_ok = s;
        ++moved;
      }
    }
    if (last_ok >= 0) {
      h.sel = last_ok;
      scroll_to_line(app, h.sel);
    }
  };
  auto [cx, cy, cw, ch] = float_client_rect(app, app.history_win);
  const int page = std::max(1, static_cast<int>(static_cast<float>(ch) /
                                                row_step(app)) - 1);
  switch (k) {
    case SDLK_ESCAPE:
      if (!h.filter.empty()) {
        h.filter.clear();
        h.stale = true;
      } else {
        app.history_panel_open = false;
        app.key_focus = KeyFocus::Document;
      }
      return true;
    case SDLK_UP: step_sel(-1); return true;
    case SDLK_DOWN: step_sel(1); return true;
    case SDLK_PAGEUP: step_sel(-page); return true;
    case SDLK_PAGEDOWN: step_sel(page); return true;
    case SDLK_HOME:
      h.sel = -1;
      step_sel(1);
      return true;
    case SDLK_END:
      while (h.more) load_more(app);
      h.sel = -1;
      step_sel(-1);
      return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      app.click_new_tab = ctrl;
      open_line(app, h.sel, shift);
      app.click_new_tab = false;
      return true;
    case SDLK_DELETE:
      remove_line(app, h.sel);
      return true;
    case SDLK_SLASH:
      if (ctrl) return false;
      open_prompt(app, PromptKind::HistoryFilter, h.filter);
      app.eat_text_input = true;  // the '/' itself
      return true;
    case SDLK_f:
      if (!ctrl) return false;
      open_prompt(app, PromptKind::HistoryFilter, h.filter);
      return true;
    default:
      return false;
  }
}

std::string history_tooltip(App& app) {
  HistoryPanel& h = app.history;
  if (!app.history_panel_open || h.hover < 0 ||
      h.hover >= static_cast<int>(h.lines.size()))
    return {};
  const int ei = h.lines[static_cast<size_t>(h.hover)].entry;
  if (ei < 0) return {};
  const auto& e = h.entries[static_cast<size_t>(ei)];
  const xanatoo::VisitRecord& v = h.visits[e.first];
  const xanatoo::VisitRecord& oldest = h.visits[e.first + e.count - 1];
  std::string tip = v.address;
  if (!h.from_db) return tip;
  // When and for how long.
  {
    std::string when = format_tm(local_tm(oldest.time), "%Y-%m-%d %H:%M:%S");
    const int64_t end = v.leave_time;
    if (end > oldest.time) {
      const int64_t s = (end - oldest.time) / 1000;
      char d[48];
      if (s < 60)
        std::snprintf(d, sizeof d, ", %llds", static_cast<long long>(s));
      else if (s < 3600)
        std::snprintf(d, sizeof d, ", %lld min", static_cast<long long>(s / 60));
      else
        std::snprintf(d, sizeof d, ", %lld h %lld min",
                      static_cast<long long>(s / 3600),
                      static_cast<long long>(s / 60 % 60));
      when += d;
    }
    tip += "\n" + when;
  }
  // How it was reached.
  {
    std::string how = kind_label(oldest.kind);
    if (how.empty()) how = "navigation";
    if (oldest.source != 0) {
      for (const auto& s : h.visits)
        if (s.id == oldest.source) {
          how += " from " +
                 (s.title.empty() ? std::string(xanatoo::trail_page_of(s.address))
                                  : s.title);
          break;
        }
    }
    if (!oldest.href.empty()) how += "  (" + oldest.href + ")";
    tip += "\n" + how;
  }
  if (e.count > 1)
    tip += "\n" + std::to_string(e.count) + " visits in this page";
  // What the fetch saw.
  const int64_t obs = v.observation ? v.observation : oldest.observation;
  if (obs != h.tip_observation) {
    h.tip_observation = obs;
    h.tip_observed = obs ? xanatoo::load_observation(obs) : std::nullopt;
  }
  if (h.tip_observed) {
    const xanatoo::PageObservation& o = *h.tip_observed;
    std::string seen;
    if (o.status) seen += std::to_string(o.status);
    if (!o.content_type.empty()) seen += (seen.empty() ? "" : " ") + o.content_type;
    if (o.size >= 0)
      seen += (seen.empty() ? "" : ", ") + std::to_string(o.size) + " bytes";
    if (!o.etag.empty()) seen += ", ETag " + o.etag;
    if (!seen.empty()) tip += "\n" + seen;
    if (!o.last_modified.empty()) tip += "\nLast-Modified " + o.last_modified;
    if (!o.text_hash.empty())
      tip += "\ntext " + o.text_hash.substr(0, std::min<size_t>(o.text_hash.size(), 23));
  }
  return tip;
}

}  // namespace xanatoo::app
