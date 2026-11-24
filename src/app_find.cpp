// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
//
// Find and replace in the focused view. The state is App::prompt (kinds
// Search / Replace); the bar itself is an ImGui strip (Ui::find_bar) that
// takes its height from the view, below the text and the page bar.
#include "xanatoo/app.hpp"
#include "xanatoo/util.hpp"

#include <algorithm>
#include <cmath>

namespace xanatoo::app {

namespace {

// Matches counted ("3 of 17"); more show as "1000+".
constexpr size_t kMaxFindHits = 1000;

void select_found(App& app, Pane& p, const xanatoo::Selection& hit) {
  reveal_hit(app, p, hit.anchor.node);
  p.sel = hit;
  Prompt& pr = app.prompt;
  pr.found = hit;
  pr.found_buf = p.buf;
  pr.origin = hit.anchor;
  note_program_selection(app);
  scroll_caret_into_view(app, p);
}

// The selection is the one find made (the user did not move since).
bool at_found(const App& app, const Pane& p) {
  const Prompt& pr = app.prompt;
  return pr.found_buf == p.buf && pr.found.anchor == p.sel.anchor &&
         pr.found.focus == p.sel.focus;
}

}  // namespace

void find_retype(App& app) {
  if (!prompt_is_search(app)) return;
  Pane& p = focused_pane(app);
  const xanatoo::Document& d = doc_of(app, p);
  Prompt& pr = app.prompt;
  reset_blink(app);
  // Every text is searched from the same place, so a match that still fits
  // stays and a shorter text goes back to the earlier one.
  const bool ours = at_found(app, p);
  if (!ours) pr.origin = xanatoo::find_origin(d, p.sel, false);
  if (auto hit = xanatoo::find_next(d, pr.text, pr.origin,
                                    find_skips(app, p))) {
    const xanatoo::Caret origin = pr.origin;
    select_found(app, p, *hit);
    pr.origin = origin;
    return;
  }
  if (ours && !p.sel.empty()) {  // nothing found: no match stays marked
    p.sel.anchor = p.sel.focus = pr.origin;
    pr.found = p.sel;
    note_program_selection(app);
  }
}

void find_set_texts(App& app, const std::string& arg) {
  if (!prompt_is_search(app)) return;
  const size_t cut = arg.find('\n');
  if (cut == std::string::npos) return;
  Prompt& pr = app.prompt;
  pr.fresh = false;  // the field has the keys
  pr.text2 = arg.substr(cut + 1);
  std::string text = arg.substr(0, cut);
  if (text == pr.text) return;
  pr.text = std::move(text);
  find_retype(app);
}

void find_step(App& app, bool backward) {
  if (!prompt_is_search(app) || app.prompt.text.empty()) return;
  Pane& p = focused_pane(app);
  const xanatoo::Document& d = doc_of(app, p);
  const xanatoo::Caret from = xanatoo::find_origin(d, p.sel, !backward);
  const std::vector<xanatoo::NodeId> skip = find_skips(app, p);
  const auto hit = backward
                       ? xanatoo::find_prev(d, app.prompt.text, from, skip)
                       : xanatoo::find_next(d, app.prompt.text, from, skip);
  if (hit) select_found(app, p, *hit);
  reset_blink(app);
}

void find_replace_one(App& app) {
  if (!prompt_is_search(app) || app.prompt.text.empty()) return;
  if (browsing(app)) {
    flash_status(app, no_edit_notice(app));
    return;
  }
  Pane& p = focused_pane(app);
  xanatoo::Document& d = doc_of(app, p);
  if (xanatoo::is_match(d, app.prompt.text, p.sel)) {
    edit(app);
    xanatoo::delete_selection(d, p.sel);
    if (!app.prompt.text2.empty())
      xanatoo::insert_text(d, p.sel.focus, app.prompt.text2);
    p.sel.anchor = p.sel.focus;
    app.needs_layout = true;
  }
  find_step(app, false);
}

void find_replace_all(App& app) {
  const bool open = prompt_is_search(app);
  const std::string query = open ? app.prompt.text : app.last_search;
  const std::string repl = open ? app.prompt.text2 : app.last_replace;
  if (query.empty()) {
    open_search(app, true);
    return;
  }
  if (browsing(app)) {
    flash_status(app, no_edit_notice(app));
    return;
  }
  Pane& p = focused_pane(app);
  xanatoo::Document& d = doc_of(app, p);
  const std::vector<xanatoo::NodeId> skip = find_skips(app, p);
  if (xanatoo::find_all(d, query, 1, skip).empty()) {
    flash_status(app, "“" + query + "” not found");
    return;
  }
  edit(app);
  const size_t n = xanatoo::replace_all(d, query, repl, skip);
  xanatoo::ensure_valid_caret(d, p.sel.focus);
  p.sel.anchor = p.sel.focus;
  app.needs_layout = true;
  reset_blink(app);
  flash_status(app, "Replaced " + std::to_string(n) +
                        (n == 1 ? " match" : " matches"));
}

const std::vector<xanatoo::Caret>& find_hits(App& app, Pane& p) {
  const xanatoo::Document& d = doc_of(app, p);
  const DocBuffer* b = find_buf(app, p.buf);
  const uint64_t gen = b ? b->doc_gen : 0;
  SearchTicks& st = p.search_ticks;
  if (st.query != app.prompt.text || st.doc != &d || st.buf != p.buf ||
      st.doc_gen != gen || st.nodes != d.nodes.size()) {
    st.query = app.prompt.text;
    st.doc = &d;
    st.buf = p.buf;
    st.doc_gen = gen;
    st.nodes = d.nodes.size();
    st.hits = xanatoo::find_all(d, st.query, kMaxFindHits, find_skips(app, p));
  }
  return st.hits;
}

FindStatus find_status(App& app) {
  FindStatus st;
  Pane& p = focused_pane(app);
  const std::vector<xanatoo::Caret>& hits = find_hits(app, p);
  st.count = hits.size();
  st.capped = hits.size() >= kMaxFindHits;
  if (p.sel.empty()) return st;
  const xanatoo::Caret at =
      xanatoo::find_origin(doc_of(app, p), p.sel, false);
  const auto it = std::find(hits.begin(), hits.end(), at);
  if (it != hits.end())
    st.index = static_cast<size_t>(it - hits.begin()) + 1;
  return st;
}

bool find_bar_shown(App& app) {
  if (!prompt_is_search(app) || !view_live(app, app.focus)) return false;
  for (const FloatWindow& w : app.soft_wins)
    if (w.view == app.focus) return !float_shaded(w);
  return true;
}

int find_bar_h(const App& app, const Pane& p) {
  if (!prompt_is_search(app) || &p != &app.panes[app.focus]) return 0;
  const int rows = app.prompt.kind == PromptKind::Replace ? 2 : 1;
  return std::max(20, static_cast<int>(std::lround(
                          (4.f + 30.f * static_cast<float>(rows)) *
                          app.ui_scale)));
}

SDL_Rect find_bar_rect(const App& app) {
  const Pane& p = app.panes[app.focus];
  return SDL_Rect{p.x, p.y + p.h + pane_bar_h(app, p), p.w,
                  find_bar_h(app, p)};
}

}  // namespace xanatoo::app
