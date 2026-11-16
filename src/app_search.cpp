// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// Search bar (docs/SEARCH.md): the box right of the location field on the
// second toolbar row (shown by default). It builds a
// `search:<engine>:<query>` address and navigates to it; fetching, history
// and session restore are the ordinary page machinery. The Ctrl+L bar
// understands "<keyword> <query>" too.

#include "xanatoo/app.hpp"

#include <filesystem>
#include <iostream>

namespace xanatoo::app {

namespace {

void update_active(App& app) {
  app.search.active = xanatoo::engines_on(app.search.engines,
                                          app.view_settings.search_on,
                                          app.view_settings.search_off);
}

}  // namespace

void init_search(App& app) {
  app.search.engines = xanatoo::load_engines();
  update_active(app);
  std::cerr << "search: " << app.search.engines.size() << " engines, "
            << app.search.active.size() << " on, in use "
            << search_engine_id(app) << "\n";
}

// The engine in use: the saved one while it is on, else the first one on.
std::string search_engine_id(const App& app) {
  const auto& list =
      app.search.active.empty() ? app.search.engines : app.search.active;
  for (const xanatoo::SearchEngine& e : list)
    if (e.id == app.view_settings.search_engine) return e.id;
  return list.empty() ? std::string() : list[0].id;
}

void toggle_search_engine(App& app, const std::string& id) {
  auto& vs = app.view_settings;
  for (const xanatoo::SearchEngine& e : app.search.engines) {
    if (e.id != id) continue;
    const bool on = !xanatoo::engine_on(e, vs.search_on, vs.search_off);
    std::erase(vs.search_on, id);
    std::erase(vs.search_off, id);
    if (on != e.enabled) (on ? vs.search_on : vs.search_off).push_back(id);
    xanatoo::save_view_settings(vs);
    update_active(app);
    flash_status(app, e.name + (on ? ": in the search menu"
                                   : ": removed from the search menu"));
    return;
  }
}

void show_search_bar(App& app) {
  if (!app.search.open) app.needs_layout = true;
  app.search.open = true;
  close_prompt(app);
  app.ui.focus_search();
}

void toggle_search_bar(App& app) {
  app.search.open = !app.search.open;
  if (app.search.open)
    app.ui.focus_search();
  else
    app.ui.release_input_capture();  // keys go back to the document
  app.needs_layout = true;
}

void select_search_engine(App& app, const std::string& id) {
  for (const xanatoo::SearchEngine& e : app.search.active) {
    if (e.id != id) continue;
    app.view_settings.search_engine = id;
    xanatoo::save_view_settings(app.view_settings);
    app.ui.focus_search();
    flash_status(app, "Search engine: " + e.name);
    return;
  }
}

void search_site(App& app, const std::string& id) {
  // Regular button (not a toggle): always open the search bar and select
  // the site-specific engine. Never hide the bar.
  show_search_bar(app);
  if (!id.empty() && search_engine_id(app) != id)
    select_search_engine(app, id);
  else
    app.ui.focus_search();
}

void run_search(App& app, const std::string& text) {
  const std::string ref = xanatoo::search_ref(search_engine_id(app), text);
  if (ref.empty()) {
    flash_status(app, "Type something to search for");
    return;
  }
  app.ui.release_input_capture();
  navigate_to_url(app, ref);
}

std::string search_from_location(const App& app, const std::string& text) {
  std::string ref = xanatoo::search_from_keyword(text, app.search.active,
                                                 search_engine_id(app));
  if (ref.empty()) return {};
  // "w notes.md" must still open a file of that name.
  std::error_code ec;
  if (std::filesystem::exists(text, ec)) return {};
  return ref;
}

}  // namespace xanatoo::app
