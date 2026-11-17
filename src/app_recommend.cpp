// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// Recommendations panel (WindowRole::Recommendations): websites similar to
// the focused page's site (recommendations.hpp). A tool view like the
// Sitemap panel: it follows the focused pane, asks once per host on a worker
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
  return xanatoo::recommend_domains(host).empty() ? std::string{} : host;
}

RecommendSite* find_site(App& app, const std::string& host) {
  for (RecommendSite& s : app.recommend.sites)
    if (s.host == host) return &s;
  return nullptr;
}

void start_load(RecommendSite& s) {
  s.result.reset();
  s.pending = std::make_shared<RecommendLoad>();
  // Keys are read here (main thread) so the worker touches no files.
  xanatoo::RecommendKeys keys;
  keys.webxtrend_token =
      xanatoo::effective_catalog_credential(xanatoo::CatalogKey::WebxtrendToken);
  keys.similars_key =
      xanatoo::effective_catalog_credential(xanatoo::CatalogKey::SimilarsNetKey);
#if defined(XANATOO_WEB) && XANATOO_WEB
  // Detached like page fetches: a dropped entry orphans the shared state.
  std::thread(
      [host = s.host, keys](std::shared_ptr<RecommendLoad> out) {
        out->result = xanatoo::recommend_fetch(host, keys);
        out->done.store(true, std::memory_order_release);
      },
      s.pending)
      .detach();
#else
  s.pending->result = xanatoo::recommend_fetch(s.host, keys);  // a note only
  s.pending->done.store(true, std::memory_order_release);
#endif
}

void poll_loads(App& app) {
  for (RecommendSite& s : app.recommend.sites) {
    if (!s.pending || !s.pending->done.load(std::memory_order_acquire))
      continue;
    s.result = std::move(s.pending->result);
    s.pending.reset();
  }
}

}  // namespace

void toggle_recommendations(App& app) {
  app.recommend.open = !app.recommend.open;
  if (app.recommend.open) focus_panel(app, WindowRole::Recommendations);
}

void update_recommendations(App& app) {
  poll_loads(app);
  if (!app.recommend.open) return;
  app.recommend.host = focused_web_host(app);
  if (app.recommend.host.empty()) return;
  if (find_site(app, app.recommend.host)) return;
  auto& sites = app.recommend.sites;
  if (sites.size() >= kMaxSites) {
    // Drop the oldest finished answer (a running one finishes unseen).
    auto it = std::find_if(sites.begin(), sites.end(),
                           [](const RecommendSite& s) { return !s.pending; });
    if (it != sites.end()) sites.erase(it);
  }
  sites.emplace_back();
  sites.back().host = app.recommend.host;
  start_load(sites.back());
}

void refresh_recommendations(App& app) {
  RecommendSite* s = find_site(app, app.recommend.host);
  if (!s || s->pending) return;
  start_load(*s);
}

void recommend_ui_state(App& app, xanatoo::UiState& st) {
  st.recommend_open = app.recommend.open;
  if (!app.recommend.open) return;
  auto [x, y, w, h] = float_client_rect(app, app.recommend_win);
  st.recommend_x = x;
  st.recommend_y = y;
  st.recommend_w = w;
  st.recommend_h = h;
  st.recommend_host = app.recommend.host;
  st.recommend_domain.clear();
  st.recommend_loading = false;
  st.recommend_similars_net = false;
  st.recommend_rows.clear();
  st.recommend_notes.clear();
  const RecommendSite* s = app.recommend.host.empty()
                               ? nullptr
                               : find_site(app, app.recommend.host);
  if (!s) return;
  st.recommend_loading = s->pending != nullptr;
  if (!s->result) return;
  st.recommend_domain = s->result->domain;
  st.recommend_notes = s->result->notes;
  for (const auto& site : s->result->sites) {
    xanatoo::UiState::RecommendRow row;
    row.domain = site.domain;
    row.title = site.title;
    row.description = site.description;
    row.category = site.category;
    row.source = site.source;
    row.score = static_cast<float>(site.score);
    if (site.source == "similars.net") st.recommend_similars_net = true;
    st.recommend_rows.push_back(std::move(row));
  }
}

}  // namespace xanatoo::app
