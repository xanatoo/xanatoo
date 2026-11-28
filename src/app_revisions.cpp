// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/app.hpp"
#include "xanatoo/revstore.hpp"
#include "xanatoo/theme.hpp"

#include <algorithm>
#include <ctime>
#include <set>
#include <unordered_map>

namespace xanatoo::app {
namespace {

int row_height(const App& app) {
  return std::max(20, app.ui_font.line_height() + 8);
}

void layout_revision_graph(App& app) {
  const auto& history = app.revision_history;
  app.revision_lanes.resize(history.size());
  app.revision_parents.assign(history.size(), -1);
  app.revision_lane_count = 0;
  std::unordered_map<std::string_view, int> rows, active;
  std::set<int> free_lanes;
  for (int i = 0; i < static_cast<int>(history.size()); ++i)
    rows.emplace(history[i].id, i);

  // Reserve each lane until its pending parent is reached. Siblings merge
  // into the lane already reserved for their shared parent.
  for (int i = 0; i < static_cast<int>(history.size()); ++i) {
    int lane;
    if (const auto previous = active.find(history[i].id);
        previous != active.end()) {
      lane = previous->second;
      active.erase(previous);
    } else if (!free_lanes.empty()) {
      lane = *free_lanes.begin();
      free_lanes.erase(free_lanes.begin());
    } else {
      lane = app.revision_lane_count++;
    }
    app.revision_lanes[i] = lane;
    const auto parent = rows.find(history[i].parent);
    if (parent != rows.end()) {
      app.revision_parents[i] = parent->second;
      if (active.emplace(parent->first, lane).second) continue;
    }
    free_lanes.insert(lane);
  }
}

const DocBuffer* refresh_revision_panel(App& app) {
  const DocBuffer* b = find_buf(app, focused_pane(app).buf);
  const bool native = b && is_stored_document_ref(b->path);
  const std::string key = native ? b->doc_key : std::string{};
  if (key != app.revision_document_key) {
    app.revision_working.clear();
    app.revision_document_key = key;
    app.revision_win.scroll = 0.f;
  }
  const auto head = native ? find_stored_document(key) : std::nullopt;
  const std::string current = head ? head->working : std::string{};
  if (current != app.revision_working || !head) {
    app.revision_working = current;
    app.revision_history = head ? stored_document_history(key)
                                : std::vector<DocumentRevision>{};
    layout_revision_graph(app);
  }
  return head ? b : nullptr;
}

std::string revision_label(const DocumentRevision& revision) {
  const std::time_t seconds = static_cast<std::time_t>(revision.time);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &seconds);
#else
  localtime_r(&seconds, &tm);
#endif
  char date[32] = {};
  std::strftime(date, sizeof date, "%Y-%m-%d %H:%M:%S", &tm);
  return std::string(revision.original ? "Original  " : "") + date + "  " +
         revision.id.substr(7, 8);
}

}  // namespace

float revision_panel_max_scroll(App& app, int client_h) {
  return std::max(0.f, static_cast<float>(
      app.revision_history.size() * row_height(app)) - client_h);
}

void draw_revision_panel(App& app) {
  if (!app.revision_panel_open) return;
  const DocBuffer* b = refresh_revision_panel(app);
  FloatWindow& w = app.revision_win;
  auto [cx, cy, cw, ch] = float_client_rect(app, w);
  w.scroll = std::clamp(w.scroll, 0.f, revision_panel_max_scroll(app, ch));
  if (!draw_floating_frame(app, w, "Document Revisions",
                           float_active(app, w))) return;
  const SDL_Rect clip{cx, cy, cw, ch};
  xanatoo::sdl::set_clip_rect(app.sdl, &clip);
  const auto& pal = xanatoo::theme::palette();
  const int step = row_height(app);
  auto draw_label = [&](int x, int y, const std::string& label,
                        const theme::Rgb& ink) {
    const int baseline = y + (step - app.ui_font.line_height()) / 2 +
                         app.ui_font.ascender();
    app.renderer.draw_text(app.ui_font, static_cast<float>(x),
        static_cast<float>(baseline), fit_text(app.ui_font, label,
        std::max(0, cx + cw - 16 - x)), ink.r, ink.g, ink.b);
  };
  if (!b) {
    draw_label(cx + 7, cy, "No revisions for this document", pal.text_dim);
    xanatoo::sdl::set_clip_rect(app.sdl, nullptr);
    return;
  }

  const auto& history = app.revision_history;
  const int scroll = static_cast<int>(w.scroll);
  const int first = scroll / step;
  const int last = std::min(static_cast<int>(history.size()),
                            (scroll + ch) / step + 1);
  const float spacing = std::min(14.f, static_cast<float>(cw) /
      (2.f * std::max(1, app.revision_lane_count)));
  auto x_of = [&](int row) {
    return cx + 9 + static_cast<int>(spacing * app.revision_lanes[row]);
  };
  auto y_of = [&](int row) { return cy + row * step - scroll; };

  for (int row = first; row < last; ++row)
    if (history[row].id == b->stored_working)
      theme::fill(app.sdl, {cx + 1, y_of(row), cw - 12, step}, pal.face_dark);

  SDL_SetRenderDrawColor(app.sdl, pal.text_dim.r, pal.text_dim.g,
                         pal.text_dim.b, 255);
  for (int row = 0; row < last; ++row) {
    const int parent = app.revision_parents[row];
    if (parent < first) continue;
    const int y = y_of(row) + step / 2;
    if (y >= cy + ch) break;
    const int parent_x = x_of(parent);
    if (y + step / 2 >= cy)
      sdl::draw_line(app.sdl, x_of(row), y, parent_x, y + step / 2);
    sdl::draw_line(app.sdl, parent_x, std::max(cy, y + step / 2), parent_x,
                   std::min(cy + ch, y_of(parent) + step / 2));
  }
  for (int row = first; row < last; ++row) {
    const bool current = history[row].id == b->stored_working;
    const int radius = current ? 4 : 3;
    theme::fill(app.sdl, {x_of(row) - radius, y_of(row) + step / 2 - radius,
                          2 * radius + 1, 2 * radius + 1},
                current ? pal.accent : pal.text_dim);
    draw_label(cx + 16 + static_cast<int>(spacing * app.revision_lane_count),
               y_of(row), revision_label(history[row]), pal.text);
  }
  xanatoo::sdl::set_clip_rect(app.sdl, nullptr);
  draw_client_scrollbar(app, w, revision_panel_max_scroll(app, ch));
}

void click_revision_panel(App& app, int mx, int my) {
  const DocBuffer* b = find_buf(app, focused_pane(app).buf);
  if (!b || b->doc_key != app.revision_document_key) return;
  auto [cx, cy, cw, ch] = float_client_rect(app, app.revision_win);
  if (mx < cx || mx >= cx + cw - 12 || my < cy || my >= cy + ch) return;
  const int row = (my - cy + static_cast<int>(app.revision_win.scroll)) /
                  row_height(app);
  if (row >= static_cast<int>(app.revision_history.size())) return;
  const std::string revision = app.revision_history[row].id;
  if (b->dirty || revision != b->stored_working)
    select_buffer_revision(app, b->id, revision);
}

}  // namespace xanatoo::app
