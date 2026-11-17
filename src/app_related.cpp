// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// Related Sites panel (WindowRole::RelatedSites): websites similar to
// the focused page's site (related_sites.hpp). A tool view like the
// Sitemap panel: it follows the focused pane (unless Follow is off, which
// holds the list on one site while its rows are explored), asks once per host on a worker
// thread while it is open, and keeps the answers for the session. The list
// itself is an ImGui body inside the native dockable frame (ui.cpp).

#include <algorithm>
#include <thread>

#include "xanatoo/app.hpp"
#include "xanatoo/catalog_credentials.hpp"
#include "xanatoo/native.hpp"
#include "xanatoo/sitemap.hpp"

namespace xanatoo::app {

namespace {

// Answers kept per session; older hosts are asked again when revisited.
constexpr size_t kMaxSites = 64;

// Host of the focused page as a web site: http(s) as is, native refs
// (hn:, wikipedia:, …) mapped back to their web address.
std::string focused_web_host(App& app) {
  const DocBuffer* b = find_buf(app, focused_pane(app).buf);
  if (!b) return {};
  const std::string& p = b->path;
  if (xanatoo::is_sitemap_ref(p)) return xanatoo::sitemap_ref_host(p);
  std::string host = xanatoo::url_host(p);
  if (host.empty()) host = xanatoo::url_host(xanatoo::web_url_for(p));
  if (host.empty()) return {};
  return xanatoo::related_domains(host).empty() ? std::string{} : host;
}

RelatedSite* find_site(App& app, const std::string& host) {
  for (RelatedSite& s : app.related.sites)
    if (s.host == host) return &s;
  return nullptr;
}

void start_load(RelatedSite& s) {
  s.result.reset();
  s.pending = std::make_shared<RelatedLoad>();
  // Keys are read here (main thread) so the worker touches no files.
  xanatoo::RelatedKeys keys;
  keys.webxtrend_token =
      xanatoo::effective_catalog_credential(xanatoo::CatalogKey::WebxtrendToken);
  keys.similars_key =
      xanatoo::effective_catalog_credential(xanatoo::CatalogKey::SimilarsNetKey);
#if defined(XANATOO_WEB) && XANATOO_WEB
  // Detached like page fetches: a dropped entry orphans the shared state.
  std::thread(
      [host = s.host, keys](std::shared_ptr<RelatedLoad> out) {
        out->result = xanatoo::related_fetch(host, keys);
        out->done.store(true, std::memory_order_release);
      },
      s.pending)
      .detach();
#else
  s.pending->result = xanatoo::related_fetch(s.host, keys);  // a note only
  s.pending->done.store(true, std::memory_order_release);
#endif
}

void poll_loads(App& app) {
  for (RelatedSite& s : app.related.sites) {
    if (!s.pending || !s.pending->done.load(std::memory_order_acquire))
      continue;
    s.result = std::move(s.pending->result);
    s.pending.reset();
  }
}

}  // namespace

void toggle_related_sites(App& app) {
  app.related.open = !app.related.open;
  if (app.related.open) focus_panel(app, WindowRole::RelatedSites);
}

void toggle_related_follow(App& app) {
  app.related.follow = !app.related.follow;
}

void update_related_sites(App& app) {
  poll_loads(app);
  if (!app.related.open) return;
  // Held: keep the shown site (and its answer) until Follow is on again.
  if (!app.related.follow && !app.related.host.empty()) return;
  app.related.host = focused_web_host(app);
  if (app.related.host.empty()) return;
  if (find_site(app, app.related.host)) return;
  auto& sites = app.related.sites;
  if (sites.size() >= kMaxSites) {
    // Drop the oldest finished answer (a running one finishes unseen).
    auto it = std::find_if(sites.begin(), sites.end(),
                           [](const RelatedSite& s) { return !s.pending; });
    if (it != sites.end()) sites.erase(it);
  }
  sites.emplace_back();
  sites.back().host = app.related.host;
  start_load(sites.back());
}

void refresh_related_sites(App& app) {
  RelatedSite* s = find_site(app, app.related.host);
  if (!s || s->pending) return;
  start_load(*s);
}

void related_ui_state(App& app, xanatoo::UiState& st) {
  st.related_open = app.related.open;
  if (!app.related.open) return;
  auto [x, y, w, h] = float_client_rect(app, app.related_win);
  st.related_x = x;
  st.related_y = y;
  st.related_w = w;
  st.related_h = h;
  st.related_host = app.related.host;
  st.related_follow = app.related.follow;
  st.related_behind = !app.related.follow &&
                      focused_web_host(app) != app.related.host;
  st.related_domain.clear();
  st.related_loading = false;
  st.related_similars_net = false;
  st.related_rows.clear();
  st.related_notes.clear();
  const RelatedSite* s = app.related.host.empty()
                               ? nullptr
                               : find_site(app, app.related.host);
  if (!s) return;
  st.related_loading = s->pending != nullptr;
  if (!s->result) return;
  st.related_domain = s->result->domain;
  st.related_notes = s->result->notes;
  for (const auto& site : s->result->sites) {
    xanatoo::UiState::RelatedRow row;
    row.domain = site.domain;
    row.title = site.title;
    row.description = site.description;
    row.category = site.category;
    row.source = site.source;
    row.score = static_cast<float>(site.score);
    if (site.source == "similars.net") st.related_similars_net = true;
    st.related_rows.push_back(std::move(row));
  }
}

}  // namespace xanatoo::app
