// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
//
// Session: what xanatoo looked like last (pages in the centre views, tabs,
// back / forward trails, history, split, scroll, panels and their docks) in
// state_dir()/session.conf. It is rewritten shortly after anything changed
// (autosave_session, per frame) and put back at the next start unless the
// command line names a document or asks for --new-session.

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include "xanatoo/app.hpp"
#include "xanatoo/snapshot.hpp"
#include "xanatoo/lemmy_directory.hpp"
#include "xanatoo/matrix_directory.hpp"
#include "xanatoo/paths.hpp"
#include "xanatoo/parse.hpp"
#include "xanatoo/util.hpp"

namespace fs = std::filesystem;

namespace xanatoo::app {

namespace {

std::string session_path() {
  const std::string dir = state_dir();
  if (dir.empty()) return {};
  return (fs::path(dir) / "session.conf").string();
}

struct PanelRef {
  const char* name;
  FloatWindow* win;
  bool open;
};

std::array<PanelRef, 23> panels(App& app) {
  return {{{"help", &app.help, app.help_mode},
           {"outline", &app.ol, app.outline_mode},
           {"backlinks", &app.bl, app.backlinks_mode},
           {"sitemap", &app.sm, app.sitemap.shown},
           {"page", &app.pg, app.page.shown},
           {"vertical-tabs", &app.vertical_tabs_win, app.vertical_tabs_open},
           {"history", &app.history_win, app.history_panel_open},
           {"styles", &app.styles_win, app.styles_panel_open},
           {"style-manager", &app.styles_manager_win,
            app.styles_manager_panel_open},
           {"bookmarks", &app.bookmarks_win, app.bookmarks_panel_open},
           {"context", &app.context_win, app.context_panel_open},
           {"subscriptions", &app.subscriptions_win, app.subscriptions_panel_open},
           {"downloads", &app.downloads_win, app.downloads_panel_open},
           {"media", &app.media_win, app.media_panel_open},
           {"annotations", &app.annotation_win, app.annotation_panel_open},
           {"revisions", &app.revision_win, app.revision_panel_open},
           {"nostr-explorer", &app.nostr_explorer_win, app.nostr_explorer_open},
           {"lemmy-explorer", &app.lemmy_explorer_win, app.lemmy_explorer_open},
           {"matrix-explorer", &app.matrix_explorer_win, app.matrix_explorer_open},
           {"certificates", &app.certs_win, app.certs.open},
           {"identities", &app.identities_win, app.identities.open},
           {"related-sites", &app.related_win, app.related.open},
           {"hover-help", &app.hover_help_win, app.hover_help.open}}};
}

int unscale(const App& app, int v) {
  return static_cast<int>(std::lround(v / app.ui_scale));
}
int rescale(const App& app, int v) {
  return static_cast<int>(std::lround(v * app.ui_scale));
}

std::vector<int> ints(const std::string& s) {
  std::vector<int> out;
  std::istringstream in(s);
  int v;
  while (in >> v) out.push_back(v);
  return out;
}

}  // namespace

namespace {

std::string session_text(App& app) {
  std::ostringstream out;
  out << "# xanatoo session (restored at the next start)\n";
  out << "history.current-tab=" << app.history.current_tab << "\n";
  out << "split=" << (app.split ? 1 : 0) << "\n";
  out << "split_ratio=" << app.split_ratio << "\n";
  out << "tile_axis=" << (app.tile_axis == TileAxis::Rows ? "rows" : "columns") << "\n";
  out << "tile_mode=" << (app.diff_mode ? "compare" :
      app.tile_mode == TileMode::Trail ? "trail" : "independent") << "\n";
  out << "focus=" << std::clamp(app.centre_focus, 0, 1) << "\n";
  for (int pi = 0; pi < kCentreViews; ++pi) {
    if (!view_live(app, pi) || !find_buf(app, app.panes[pi].buf)) continue;
    Pane& p = app.panes[pi];
    std::string addr = pane_address(app, p);
    {  // a local file, from wherever xanatoo is started next time
      const std::string base = parse_locator(addr).base;
      std::error_code ec;
      if (!base.empty() && !is_in_app_remote_url(base) &&
          fs::exists(base, ec) && !fs::path(base).is_absolute())
        addr = fs::absolute(base, ec).string() + addr.substr(base.size());
    }
    if (addr.empty() || addr.find('\n') != std::string::npos) continue;
    out << "pane" << pi << "=" << addr << "\n";
    out << "scroll" << pi << "=" << p.scroll_y << "\n";
    if (p.reader) out << "reader" << pi << "=" << (*p.reader ? 1 : 0) << "\n";
    if (p.paged) out << "paged" << pi << "=" << (*p.paged ? 1 : 0) << "\n";
  }
  std::unordered_map<BufferId, size_t> saved_tabs;
  for (BufferId id : app.tab_order) {
    const DocBuffer* b = find_buf(app, id);
    if (!b || b->path.empty() || b->path.find('\n') != std::string::npos)
      continue;
    std::string path = b->path;
    std::error_code ec;
    if (!is_in_app_remote_url(path) && fs::exists(path, ec) &&
        !fs::path(path).is_absolute())
      path = fs::absolute(path, ec).string();
    saved_tabs.emplace(id, saved_tabs.size());
    out << "tab=" << path << "\n";
    const std::string index = std::to_string(saved_tabs.at(id));
    out << "tabidentity" << index << "=" << b->history_tab << "\n";
    for (const int64_t visit : b->inherited_visits)
      out << "tabinherited" << index << "=" << visit << "\n";
    if (b->tab_nav.pos >= 0) {
      out << "tabnavpos" << index << "=" << b->tab_nav.pos << "\n";
      for (size_t ni = 0; ni < b->tab_nav.entries.size(); ++ni) {
        const NavHistoryEntry& e = b->tab_nav.entries[ni];
        if (e.url.find('\n') != std::string::npos) continue;
        const std::string key = index + "." + std::to_string(ni);
        out << "tabnav" << index << "=" << e.url << "\n";
        if (e.visit) out << "tabnavvisit" << key << "=" << e.visit << "\n";
        out << "tabnavscroll" << key << "=" << e.scroll << "\n";
        if (xanatoo::is_snapshot_id(e.snapshot))
          out << "tabnavsnap" << key << "=" << e.snapshot << "\n";
        if (!e.via.href.empty() && e.via.href.find('\n') == std::string::npos &&
            e.via.from_url.find('\n') == std::string::npos) {
          out << "tabnavvia" << key << "=" << e.via.href << "\n";
          out << "tabnavfrom" << key << "=" << e.via.from_url << "\n";
          out << "tabnavlink" << key << "=" << e.via.node << " "
              << e.via.span << "\n";
        }
      }
    }
  }
  for (BufferId id : app.tab_order) {
    const auto child = saved_tabs.find(id);
    if (child == saved_tabs.end()) continue;
    BufferId parent = buf(app, id).tab_parent;
    while (parent != kNoBuffer && !saved_tabs.contains(parent))
      parent = buf(app, parent).tab_parent;
    if (parent != kNoBuffer)
      out << "tabparent" << child->second << "=" << saved_tabs.at(parent) << "\n";
  }
  for (int pi = 0; pi < kCentreViews; ++pi) {
    const auto tab = saved_tabs.find(app.panes[pi].buf);
    if (view_live(app, pi) && tab != saved_tabs.end())
      out << "panetab" << pi << "=" << tab->second << "\n";
  }
  for (const std::string& v : app.visited)
    if (v.find('\n') == std::string::npos) out << "visit=" << v << "\n";
  for (int i = 0; i < 4; ++i)
    out << "dock_w" << i << "=" << unscale(app, app.dock_w[i]) << "\n";
  out << "lemmy.instance=" << app.lemmy_instance << "\n";
  out << "lemmy.tab=" << app.lemmy_tab << "\n";
  out << "lemmy.sort=" << app.lemmy_sort << "\n";
  out << "lemmy.instance_only=" << (app.lemmy_instance_only ? 1 : 0) << "\n";
  out << "matrix.server=" << app.matrix_server << "\n";
  out << "matrix.tab=" << app.matrix_tab << "\n";
  out << "tab-bar.visible=" << (app.tab_bar_visible ? 1 : 0) << "\n";
  out << "related-sites.follow=" << (app.related.follow ? 1 : 0) << "\n";
  for (const PanelRef& r : panels(app)) {
    const FloatWindow& w = *r.win;
    out << r.name << ".open=" << (r.open ? 1 : 0) << "\n";
    out << r.name << ".dock=" << static_cast<int>(w.dock) << " "
        << static_cast<int>(w.restore_dock) << " " << w.dock_seq << "\n";
    out << r.name << ".share=" << w.dock_share << "\n";
    out << r.name << ".shaded=" << (w.shaded ? 1 : 0) << "\n";
    out << r.name << ".geom=" << unscale(app, w.x) << " "
        << unscale(app, w.y) << " " << unscale(app, w.w) << " "
        << unscale(app, w.h) << "\n";
    out << r.name << ".free=" << unscale(app, w.restore_x) << " "
        << unscale(app, w.restore_y) << " " << unscale(app, w.restore_w)
        << " " << unscale(app, w.restore_h) << "\n";
  }
  return out.str();
}

}  // namespace

void save_session(App& app) {
  if (no_save()) return;
  const std::string path = session_path();
  if (path.empty() || !ensure_dir(state_dir())) return;
  app.session_text = session_text(app);
  write_file_atomic(path, app.session_text);
}

void autosave_session(App& app) {
  if (!app.session_live || no_save()) return;
  const Uint32 now = xanatoo::sdl::ticks();
  if (now - app.session_checked < 1000) return;
  app.session_checked = now;
  // Pages still on their way would be saved as what they replace.
  for (const FetchJob& j : app.fetches)
    if (j.purpose != FetchPurpose::Preview) return;
  for (const PendingScroll& ps : app.pending_scroll)
    if (ps.y >= 0.f) return;
  if (session_text(app) != app.session_text) save_session(app);
}

void restore_session(App& app) {
  const std::string path = session_path();
  if (path.empty()) return;
  std::ifstream in(path);
  if (!in) return;
  std::unordered_map<std::string, std::string> kv;
  std::unordered_map<std::string, std::vector<std::string>> lists;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    kv[line.substr(0, eq)] = line.substr(eq + 1);
    lists[line.substr(0, eq)].push_back(line.substr(eq + 1));
  }
  auto get = [&](const std::string& k) -> const std::string* {
    auto it = kv.find(k);
    return it == kv.end() ? nullptr : &it->second;
  };
  try {
    for (int i = 0; i < 4; ++i)
      if (const std::string* v = get("dock_w" + std::to_string(i)))
        app.dock_w[i] = std::clamp(rescale(app, std::stoi(*v)), 60, 1 << 15);
    for (const PanelRef& r : panels(app)) {
      FloatWindow& w = *r.win;
      const std::string n = r.name;
      if (const std::string* v = get(n + ".dock")) {
        const auto d = ints(*v);
        if (d.size() == 3 && d[0] >= 0 && d[0] <= 5 && d[1] >= 0 && d[1] <= 5) {
          w.dock = static_cast<Dock>(d[0]);
          w.restore_dock = static_cast<Dock>(d[1]);
          w.dock_seq = d[2];
        }
      }
      if (const std::string* v = get(n + ".shaded"))
        w.shaded = *v == "1";
      if (const std::string* v = get(n + ".share")) {
        const float share = std::stof(*v);
        if (std::isfinite(share) && share >= 0.01f && share <= 100.f)
          w.dock_share = share;
      }
      if (const std::string* v = get(n + ".geom")) {
        const auto g = ints(*v);
        if (g.size() == 4 && g[2] > 40 && g[3] > 40) {
          w.x = rescale(app, g[0]);
          w.y = rescale(app, g[1]);
          w.w = rescale(app, g[2]);
          w.h = rescale(app, g[3]);
        }
      }
      if (const std::string* v = get(n + ".free")) {
        const auto g = ints(*v);
        if (g.size() == 4 && g[2] > 40 && g[3] > 40) {
          w.restore_x = rescale(app, g[0]);
          w.restore_y = rescale(app, g[1]);
          w.restore_w = rescale(app, g[2]);
          w.restore_h = rescale(app, g[3]);
        }
      }
    }
    app.visited = lists["visit"];
    app.history.current_tab = get("history.current-tab") &&
                              *get("history.current-tab") == "1";
    const bool split = get("split") && *get("split") == "1";
    const auto& tabs = lists["tab"];
    std::vector<BufferId> restored(tabs.size(), kNoBuffer);
    auto pane_tab = [&](int pi) {
      const auto* value = get("panetab" + std::to_string(pi));
      size_t index = restored.size();
      if (value) {
        std::istringstream in(*value);
        in >> index;
      }
      return index;
    };
    for (int pi = 0; pi < kCentreViews; ++pi) {
      const std::string* a = get("pane" + std::to_string(pi));
      if (!a || (pi == 1 && !split)) continue;
      Pane& p = app.panes[pi];
      if (const std::string* v = get("reader" + std::to_string(pi)))
        p.reader = (*v == "1");
      if (const std::string* v = get("paged" + std::to_string(pi)))
        p.paged = (*v == "1");
      const size_t index = pane_tab(pi);
      if (index < restored.size() && restored[index] != kNoBuffer) {
        p.buf = restored[index];
        reset_pane_locator(p);
        auto loc = xanatoo::parse_locator(*a);
        if (loc.has_selectors()) apply_locator(app, pi, std::move(loc));
      } else {
        // A startup placeholder can be shared by both panes.
        if (pi > 0 && p.buf == app.panes[0].buf)
          p.buf = add_buffer(app, xanatoo::parse_document("", {}), {});
        view_nav(app, pi) = {};
        const bool record_visits = std::exchange(app.record_visits, false);
        const bool opened = navigate_pane(app, pi, *a);
        app.record_visits = record_visits;
        if (!opened) continue;
        if (index < restored.size()) restored[index] = p.buf;
      }
      if (const std::string* v = get("scroll" + std::to_string(pi)))
        if (!pane_paged(app, p)) {
          app.pending_scroll[pi].y = std::stof(*v);
          app.pending_scroll[pi].base = xanatoo::parse_locator(*a).base;
        }
    }
    {
      std::vector<BufferId> order;
      for (size_t i = 0; i < tabs.size(); ++i) {
        const std::string index = std::to_string(i);
        BufferId id = restored[i];
        const auto* identity = get("tabidentity" + index);
        if (id == kNoBuffer && !identity) id = find_buffer(app, tabs[i]);
        if (id == kNoBuffer) id = open_link_in_tab(app, 0, tabs[i], /*inherit_history=*/false);
        restored[i] = id;
        if (id == kNoBuffer ||
            std::find(order.begin(), order.end(), id) != order.end()) continue;
        DocBuffer& tab = buf(app, id);
        tab.tab_parent = kNoBuffer;
        if (identity && !identity->empty()) tab.history_tab = *identity;
        order.push_back(id);
        if (const auto inherited = lists.find("tabinherited" + index);
            inherited != lists.end())
          for (const std::string& value : inherited->second)
            tab.inherited_visits.push_back(std::stoll(value));

        auto it = lists.find("tabnav" + index);
        const auto* pos = get("tabnavpos" + index);
        if (it == lists.end() || !pos) continue;
        const int at = std::stoi(*pos);
        if (at < 0 || at >= static_cast<int>(it->second.size())) continue;
        NavHistory& h = tab.tab_nav;
        h.entries.clear();
        for (size_t ni = 0; ni < it->second.size(); ++ni) {
          NavHistoryEntry e;
          e.url = it->second[ni];
          const std::string key = index + "." + std::to_string(ni);
          if (const auto* v = get("tabnavvisit" + key)) e.visit = std::stoll(*v);
          if (const auto* v = get("tabnavscroll" + key)) {
            const float scroll = std::stof(*v);
            if (std::isfinite(scroll)) e.scroll = std::max(0.f, scroll);
          }
          if (const auto* v = get("tabnavsnap" + key);
              v && xanatoo::is_snapshot_id(*v))
            e.snapshot = *v;
          if (const auto* v = get("tabnavvia" + key)) {
            e.via.href = *v;
            if (const auto* from = get("tabnavfrom" + key)) e.via.from_url = *from;
            if (const auto* loc = get("tabnavlink" + key)) {
              std::istringstream in(*loc);
              in >> e.via.node >> e.via.span;
            }
          }
          h.entries.push_back(std::move(e));
        }
        h.pos = at;
        tab.origin_kind.reset();
        tab.origin_visit = 0;
      }
      for (size_t i = 0; i < restored.size(); ++i) {
        const auto* value = get("tabparent" + std::to_string(i));
        if (!value || restored[i] == kNoBuffer) continue;
        std::istringstream in(*value);
        size_t index;
        if (!(in >> index) || index >= restored.size()) continue;
        const BufferId parent = restored[index];
        BufferId ancestor = parent;
        while (ancestor != kNoBuffer && ancestor != restored[i])
          ancestor = buf(app, ancestor).tab_parent;
        if (ancestor != restored[i]) buf(app, restored[i]).tab_parent = parent;
      }
      for (BufferId id : app.tab_order)
        if (std::find(order.begin(), order.end(), id) == order.end())
          order.push_back(id);
      app.tab_order = std::move(order);
      update_title(app);
    }
    if (split && app.buffers.size() > 0) {
      app.split = true;
      if (const std::string* v = get("split_ratio"))
        app.split_ratio = std::clamp(std::stof(*v), 0.01f, 0.99f);
      if (const std::string* v = get("tile_axis"))
        app.tile_axis = *v == "rows" ? TileAxis::Rows : TileAxis::Columns;
      if (const std::string* v = get("tile_mode")) {
        app.tile_mode = *v == "trail" ? TileMode::Trail : TileMode::Independent;
        app.diff_mode = *v == "compare";
        if (app.diff_mode) refresh_diff(app);
      } else {
        // Pre-tiling sessions used a split pair as a linked browsing trail.
        app.tile_mode = TileMode::Trail;
      }
    }
    if (const std::string* v = get("focus"))
      focus_view(app, split && *v == "1" ? 1 : 0);
    if (const auto* visible = get("tab-bar.visible"))
      app.tab_bar_visible = *visible != "0";
    app.vertical_tabs_open = get("vertical-tabs.open") &&
                             *get("vertical-tabs.open") == "1";
    // Panels last: they read the pages just opened.
    if (get("help.open") && *get("help.open") == "1") app.help_mode = true;
    if (get("hover-help.open") && *get("hover-help.open") == "1" &&
        !app.hover_help.open)
      toggle_hover_help(app);
    if (get("outline.open") && *get("outline.open") == "1") {
      app.outline_mode = true;
      refresh_outline(app);
    }
    if (get("backlinks.open") && *get("backlinks.open") == "1") {
      app.backlinks_mode = true;
      refresh_backlinks(app);
    }
    if (get("sitemap.open") && *get("sitemap.open") == "1" &&
        !app.sitemap.shown)
      toggle_sitemap(app);
    if (get("page.open") && *get("page.open") == "1" && !app.page.shown)
      toggle_page_panel(app);
    if (get("history.open") && *get("history.open") == "1")
      app.history_panel_open = true;
    if (get("styles.open") && *get("styles.open") == "1")
      app.styles_panel_open = true;
    if (get("style-manager.open") && *get("style-manager.open") == "1")
      app.styles_manager_panel_open = true;
    if (get("bookmarks.open") && *get("bookmarks.open") == "1")
      app.bookmarks_panel_open = true;
    if (get("context.open") && *get("context.open") == "1")
      app.context_panel_open = true;
    if (get("subscriptions.open") && *get("subscriptions.open") == "1")
      app.subscriptions_panel_open = true;
    if (get("downloads.open") && *get("downloads.open") == "1")
      app.downloads_panel_open = true;
    if (get("annotations.open") && *get("annotations.open") == "1")
      app.annotation_panel_open = true;
    if (get("revisions.open") && *get("revisions.open") == "1")
      app.revision_panel_open = true;
    if (get("nostr-explorer.open") && *get("nostr-explorer.open") == "1") {
      app.nostr_explorer_open = true;
      refresh_nostr_catalogue(app);
    }
    if (const std::string* v = get("lemmy.instance")) {
      const std::string host = xanatoo::lemmy_instance_host(*v);
      if (!host.empty()) {
        xanatoo::lemmy_directory_add_instance(host);
        app.lemmy_instance = host;
      }
    }
    if (const std::string* v = get("lemmy.tab"))
      if (*v == "0" || *v == "1" || *v == "2" || *v == "3")
        app.lemmy_tab = (*v)[0] - '0';
    if (const std::string* v = get("lemmy.sort"))
      if (*v == "Hot" || *v == "New" || *v == "Active" ||
          *v == "TopDay" || *v == "TopWeek")
        app.lemmy_sort = *v;
    if (const std::string* v = get("lemmy.instance_only"))
      app.lemmy_instance_only = *v == "1";
    if (const std::string* v = get("related-sites.follow"))
      app.related.follow = *v == "1";
    if (get("lemmy-explorer.open") && *get("lemmy-explorer.open") == "1")
      app.lemmy_explorer_open = true;
    if (const std::string* v = get("matrix.server")) {
      const std::string server = xanatoo::matrix_server_name(*v);
      if (!server.empty()) {
        xanatoo::matrix_directory_add_server(server);
        app.matrix_server = server;
      }
    }
    if (const std::string* v = get("matrix.tab"))
      if (v->size() == 1 && (*v)[0] >= '0' && (*v)[0] <= '3')
        app.matrix_tab = (*v)[0] - '0';
    if (get("matrix-explorer.open") && *get("matrix-explorer.open") == "1")
      app.matrix_explorer_open = true;
    if (get("certificates.open") && *get("certificates.open") == "1")
      show_certificates(app);
    if (get("identities.open") && *get("identities.open") == "1")
      show_identities(app);
    // "recommendations" is the panel's first name (migration alias).
    for (const char* key : {"related-sites.open", "recommendations.open"})
      if (get(key) && *get(key) == "1") app.related.open = true;
    // The Nostr Accounts panel became the Identities panel's Nostr tab.
    if (get("nostr-accounts.open") && *get("nostr-accounts.open") == "1")
      show_identities(app, xanatoo::IdentitySection::Nostr);
    app.needs_layout = true;
    std::cerr << "session: restored from " << path << "\n";
  } catch (...) {
    std::cerr << "session: ignoring unreadable " << path << "\n";
  }
}

void apply_pending_scroll(App& app) {
  for (int pi = 0; pi < kCentreViews; ++pi) {
    PendingScroll& ps = app.pending_scroll[pi];
    if (ps.y < 0.f) continue;
    bool loading = false;
    for (const FetchJob& j : app.fetches)
      if (j.pane == pi && j.purpose == FetchPurpose::Navigate) loading = true;
    if (loading) continue;
    Pane& p = app.panes[pi];
    if (find_buf(app, p.buf) && same_page(path_of(app, p), ps.base) &&
        !app.needs_layout) {
      p.scroll_y = std::clamp(ps.y, 0.f, std::max(0.f, pane_max_scroll(app, p)));
      app.needs_layout = true;
    }
    ps = PendingScroll{};
  }
}

}  // namespace xanatoo::app
