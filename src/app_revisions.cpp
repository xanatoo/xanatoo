// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/app.hpp"
#include "xanatoo/parse.hpp"
#include "xanatoo/revstore.hpp"
#include "xanatoo/theme.hpp"

#include <algorithm>
#include <ctime>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace xanatoo::app {
namespace {

int row_height(const App& app) {
  return std::max(20, app.ui_font.line_height() + 8);
}

std::tm revision_local_time(int64_t time) {
  const std::time_t seconds = static_cast<std::time_t>(time);
  std::tm result{};
#if defined(_WIN32)
  localtime_s(&result, &seconds);
#else
  localtime_r(&seconds, &result);
#endif
  return result;
}

bool is_subsequence(std::string_view shorter, std::string_view longer) {
  size_t matched = 0;
  for (char byte : longer) {
    if (matched == shorter.size()) break;
    if (byte == shorter[matched]) ++matched;
  }
  return matched == shorter.size();
}

std::string_view summarize_revision(std::string_view key,
                                    const DocumentRevision& revision) {
  constexpr int64_t max_snapshot_bytes = 64 * 1024;
  const auto before = load_stored_document_revision(
      key, revision.parent, max_snapshot_bytes);
  if (!before) return "Changed";
  const auto after = load_stored_document_revision(
      key, revision.id, max_snapshot_bytes);
  if (!after) return "Changed";
  if (*before == *after) return "Same content";

  std::string old_text, new_text;
  try {
    old_text = serialize_plain_text(parse_stored_document(*before));
    new_text = serialize_plain_text(parse_stored_document(*after));
  } catch (const std::invalid_argument&) {
    return "Changed";
  }
  if (old_text == new_text) return "Non-text change";
  if (old_text.size() < new_text.size() && is_subsequence(old_text, new_text))
    return "Text inserted";
  if (new_text.size() < old_text.size() && is_subsequence(new_text, old_text))
    return "Text deleted";
  return "Text edited";
}

void layout_revision_graph(App& app) {
  const auto& history = app.revision_history;
  app.revision_lanes.resize(history.size());
  app.revision_parents.assign(history.size(), -1);
  app.revision_lane_count = 0;
  std::unordered_map<std::string_view, int> rows, active;
  std::set<int> free_lanes;
  app.revision_rows.clear();
  app.revision_slots.resize(history.size());
  std::string last_day;
  for (int i = 0; i < static_cast<int>(history.size()); ++i) {
    rows.emplace(history[i].id, i);
    const auto time = revision_local_time(history[i].time);
    char day[32]{}, clock[16]{};
    std::strftime(day, sizeof day, "%Y-%m-%d", &time);
    std::strftime(clock, sizeof clock, "%H:%M:%S", &time);
    if (last_day != day) {
      app.revision_rows.emplace_back(-1, day);
      last_day = day;
    }
    app.revision_slots[i] = static_cast<int>(app.revision_rows.size());
    app.revision_rows.emplace_back(i, clock);
  }

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

}  // namespace

float revision_panel_max_scroll(App& app, int client_h) {
  return std::max(0.f, static_cast<float>(
      app.revision_rows.size() * row_height(app)) - client_h);
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
  const auto& slots = app.revision_slots;
  const auto& rows = app.revision_rows;
  const int first_slot = scroll / step;
  const int last_slot = std::min(static_cast<int>(rows.size()),
                                 (scroll + ch + step - 1) / step);
  const int first = static_cast<int>(
      std::lower_bound(slots.begin(), slots.end(), first_slot) - slots.begin());
  const int last = static_cast<int>(
      std::lower_bound(slots.begin(), slots.end(), last_slot) - slots.begin());
  const float spacing = std::min(14.f, static_cast<float>(cw) /
      (2.f * std::max(1, app.revision_lane_count)));
  auto x_of = [&](int row) {
    return cx + 9 + static_cast<int>(spacing * app.revision_lanes[row]);
  };
  auto slot_y = [&](int slot) { return cy + slot * step - scroll; };
  auto y_of = [&](int row) { return slot_y(slots[row]); };

  for (int slot = first_slot; slot < last_slot; ++slot)
    if (rows[slot].revision < 0)
      theme::fill(app.sdl, {cx + 1, slot_y(slot), cw - 12, step}, pal.face);

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
  const int label_x = cx + 16 +
      static_cast<int>(spacing * app.revision_lane_count);
  bool classify = true;
  for (int slot = first_slot; slot < last_slot; ++slot) {
    const auto& [row, stamp] = rows[slot];
    if (row < 0) {
      draw_label(label_x, slot_y(slot), stamp, pal.text_dim);
      continue;
    }
    const bool current = history[row].id == b->stored_working;
    const int radius = current ? 4 : 3;
    theme::fill(app.sdl, {x_of(row) - radius, y_of(row) + step / 2 - radius,
                          2 * radius + 1, 2 * radius + 1},
                current ? pal.accent : pal.text_dim);
    const auto& revision = history[row];
    std::string label = stamp + "  ";
    if (revision.original) label += "Original";
    else if (revision.parent.empty()) label += "Created";
    else {
      auto cached = app.revision_labels.find(revision.id);
      if (cached == app.revision_labels.end() && classify) {
        classify = false; // At most one bounded comparison per frame.
        cached = app.revision_labels.emplace(
            revision.id, summarize_revision(b->doc_key, revision)).first;
      }
      if (cached != app.revision_labels.end()) label += cached->second;
    }
    label += "  " + revision.id.substr(7, 8);
    draw_label(label_x, y_of(row), label, pal.text);
  }
  xanatoo::sdl::set_clip_rect(app.sdl, nullptr);
  draw_client_scrollbar(app, w, revision_panel_max_scroll(app, ch));
}

void click_revision_panel(App& app, int mx, int my) {
  const DocBuffer* b = find_buf(app, focused_pane(app).buf);
  if (!b || b->doc_key != app.revision_document_key) return;
  auto [cx, cy, cw, ch] = float_client_rect(app, app.revision_win);
  if (mx < cx || mx >= cx + cw - 12 || my < cy || my >= cy + ch) return;
  const int slot = (my - cy + static_cast<int>(app.revision_win.scroll)) /
                   row_height(app);
  if (slot >= static_cast<int>(app.revision_rows.size())) return;
  const int row = app.revision_rows[slot].revision;
  if (row < 0) return;
  const std::string revision = app.revision_history[row].id;
  if (b->dirty || revision != b->stored_working)
    select_buffer_revision(app, b->id, revision);
}

bool revision_panel_key(App& app, SDL_Keycode key) {
  switch (key) {
    case SDLK_ESCAPE:
      app.key_focus = KeyFocus::Document;
      return true;
    case SDLK_UP: case SDLK_DOWN:
    case SDLK_LEFT: case SDLK_RIGHT:
    case SDLK_HOME: case SDLK_END:
      break;
    default:
      return false;
  }

  const DocBuffer* b = refresh_revision_panel(app);
  const auto& history = app.revision_history;
  if (!b || history.empty()) return true;
  const auto current = std::find_if(history.begin(), history.end(),
      [&](const DocumentRevision& r) { return r.id == b->stored_working; });
  if (current == history.end()) return true;
  const int row = static_cast<int>(current - history.begin());
  const int last = static_cast<int>(history.size()) - 1;
  int target = row;
  switch (key) {
    case SDLK_UP: target = std::max(0, row - 1); break;
    case SDLK_DOWN: target = std::min(last, row + 1); break;
    case SDLK_HOME: target = 0; break;
    case SDLK_END: target = last; break;
    case SDLK_LEFT: target = app.revision_parents[row]; break;
    case SDLK_RIGHT: {
      const auto& parents = app.revision_parents;
      const auto child = std::find(parents.begin(), parents.begin() + row, row);
      if (child != parents.begin() + row)
        target = static_cast<int>(child - parents.begin());
      break;
    }
  }
  if (target < 0 || target == row) return true;

  const std::string revision = history[target].id;
  if (!select_buffer_revision(app, b->id, revision)) return true;
  refresh_revision_panel(app);
  // A pending edit can add a row above the target during the switch.
  const auto selected = std::find_if(history.begin(), history.end(),
      [&](const DocumentRevision& r) { return r.id == revision; });
  if (selected == history.end()) return true;
  FloatWindow& w = app.revision_win;
  const int client_h = std::get<3>(float_client_rect(app, w));
  const float top = static_cast<float>(
      app.revision_slots[selected - history.begin()] * row_height(app));
  const float bottom = top + row_height(app);
  if (top < w.scroll) w.scroll = top;
  else if (bottom > w.scroll + client_h) w.scroll = bottom - client_h;
  return true;
}

}  // namespace xanatoo::app
