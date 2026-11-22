// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// Search bar (docs/SEARCH.md): the box right of the location field on the
// second toolbar row (shown by default). It builds a
// `search:<engine>:<query>` address and navigates to it; fetching, history
// and session restore are the ordinary page machinery. The Ctrl+L bar
// understands "<keyword> <query>" too.

#include "xanatoo/app.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <thread>

#include "xanatoo/opensearch.hpp"
#include "xanatoo/page_links.hpp"
#if defined(XANATOO_HTTPS) && XANATOO_HTTPS
#include "xanatoo/https.hpp"
#endif

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
  leave_prompt(app);
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

void apply_preferred_languages(const App& app) {
#if defined(XANATOO_HTTPS) && XANATOO_HTTPS
  xanatoo::set_accept_language(
      xanatoo::accept_language_value(app.view_settings.languages));
#else
  (void)app;
#endif
}

void set_preferred_languages(App& app, std::vector<std::string> tags) {
  app.view_settings.languages = std::move(tags);
  xanatoo::save_view_settings(app.view_settings);
  apply_preferred_languages(app);
  std::string shown;
  for (const std::string& t : app.view_settings.languages)
    shown += (shown.empty() ? "" : ", ") + t;
  flash_status(app, shown.empty()
                        ? "Languages: default (en-US, en)"
                        : "Languages: " + shown + " (reload a page to apply)");
}

void add_search_engine_from(App& app, const std::string& url) {
  if (url.empty()) return;
  if (app.search.adding) {
    flash_status(app, "Still reading a search engine description");
    return;
  }
#if defined(XANATOO_WEB) && XANATOO_WEB
  app.search.adding = std::make_shared<OpenSearchLoad>();
  app.search.adding_url = url;
  flash_status(app, "Reading the site's search engine …");
  std::thread(
      [url](std::shared_ptr<OpenSearchLoad> out) {
        out->engine = xanatoo::fetch_opensearch(url, &out->error);
        out->done.store(true, std::memory_order_release);
      },
      app.search.adding)
      .detach();
#else
  flash_status(app, "Adding search engines needs a build with web support");
#endif
}

void poll_search_engine_adds(App& app) {
  auto load = app.search.adding;
  if (!load || !load->done.load(std::memory_order_acquire)) return;
  app.search.adding.reset();
  if (!load->engine) {
    flash_status(app, "Cannot add the search engine: " + load->error);
    return;
  }
  xanatoo::SearchEngine e = std::move(*load->engine);
  // A built-in or earlier engine of that id stays unless it searches this
  // very site (then the new description replaces it).
  for (const xanatoo::SearchEngine& old : app.search.engines) {
    if (old.id != e.id || old.url == e.url) continue;
    const bool same_site = std::find(old.sites.begin(), old.sites.end(),
                                     e.sites.empty() ? "" : e.sites[0]) !=
                           old.sites.end();
    if (!same_site) {
      for (int n = 2; n < 100; ++n) {
        const std::string id = e.id + "-" + std::to_string(n);
        if (std::none_of(app.search.engines.begin(), app.search.engines.end(),
                         [&](const xanatoo::SearchEngine& x) { return x.id == id; })) {
          e.id = id;
          break;
        }
      }
    }
  }
  std::string err;
  const bool saved = xanatoo::save_user_engine(e, &err);
  if (saved) {
    app.search.engines = xanatoo::load_engines();
  } else {
    // No place to keep it (or --no-save): this session only.
    std::erase_if(app.search.engines,
                  [&](const xanatoo::SearchEngine& x) { return x.id == e.id; });
    app.search.engines.push_back(e);
  }
  // Switched on even if the user had an engine of this id switched off.
  std::erase(app.view_settings.search_off, e.id);
  update_active(app);
  app.view_settings.search_engine = e.id;
  xanatoo::save_view_settings(app.view_settings);
  flash_status(app, "Added search engine " + e.name +
                        (saved ? "" : " for this session (" + err + ")"));
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
