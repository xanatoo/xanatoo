// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// Identity panel (WindowRole::Identities): Gemini client certificates,
// which servers treat as accounts. Creates them, chooses which one a page or
// a whole server sees, renames and deletes them. A page that asks for a
// certificate (status 60/61/62) opens the panel on itself. The store and
// scope matching live in gemini_identity.cpp; certificates are made in the
// TLS backend (tls_make_client_identity).

#include <ctime>
#include <iostream>

#include "xanatoo/app.hpp"
#include "xanatoo/gemini.hpp"
#include "xanatoo/gemini_identity.hpp"
#include "xanatoo/paths.hpp"
#include "xanatoo/tls.hpp"

namespace xanatoo::app {

namespace {

std::string pane_gemini_url(App& app, int pi) {
  if (!view_live(app, pi)) return {};
  const std::string& path = path_of(app, app.panes[pi]);
  return xanatoo::is_gemini_url(path) ? path : std::string{};
}

std::string today() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[16];
  std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
  return buf;
}

// "a\nb" → {a, b}
std::pair<std::string, std::string> split_arg(const std::string& arg) {
  const size_t nl = arg.find('\n');
  if (nl == std::string::npos) return {arg, {}};
  return {arg.substr(0, nl), arg.substr(nl + 1)};
}

bool report(App& app, bool ok, const std::string& err, const std::string& what) {
  if (!ok) {
    app.identities.error = err;
    flash_status(app, what + ": " + err);
  }
  refresh_identities(app);
  return ok;
}

// Shown pages below `scope` go out with another certificate now: reload.
void reload_scope(App& app, const std::string& scope) {
  if (scope.empty()) return;
  for (int pi : live_views(app)) {
    const std::string url = pane_gemini_url(app, pi);
    if (url.empty() || !xanatoo::gemini_scope_covers(scope, url)) continue;
    forget_cached_page(app, url);
    start_fetch(app, FetchPurpose::Navigate, pi, url, /*record_history=*/false);
  }
}

// Make a new identity; returns its sha256 (empty on failure).
std::string create_identity(App& app, std::string name) {
  while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
    name.pop_back();
  while (!name.empty() && (name.front() == ' ' || name.front() == '\t'))
    name.erase(name.begin());
  if (name.empty()) {
    report(app, false, "give the identity a name", "No identity created");
    return {};
  }
  std::string err;
  auto made = xanatoo::tls_make_client_identity(name, &err);
  if (!made) {
    report(app, false, err, "No identity created");
    return {};
  }
  auto sha = xanatoo::tls_certificate_sha256(made->cert_pem);
  if (!sha) {
    report(app, false, "certificate has no fingerprint", "No identity created");
    return {};
  }
  xanatoo::GeminiIdentity id;
  id.sha256 = *sha;
  id.name = name;
  id.created = today();
  id.cert_pem = std::move(made->cert_pem);
  id.key_pem = std::move(made->key_pem);
  if (!report(app,
              xanatoo::gemini_identity_add(xanatoo::state_dir(),
                                           !xanatoo::no_save(), std::move(id),
                                           &err),
              err, "No identity created"))
    return {};
  app.identities.selected = *sha;
  flash_status(app, "Created identity " + name);
  return *sha;
}

void use_identity(App& app, const std::string& sha, const std::string& scope) {
  if (sha.empty() || scope.empty()) return;
  std::string err;
  if (!report(app,
              xanatoo::gemini_identity_use(xanatoo::state_dir(),
                                           !xanatoo::no_save(), sha, scope, &err),
              err, "Identity not used"))
    return;
  flash_status(app, "Identity used for " + scope);
  reload_scope(app, scope);
}

}  // namespace

void refresh_identities(App& app) {
  app.identities.error.clear();
  app.identities.ids = xanatoo::gemini_identities_list(xanatoo::state_dir(),
                                                      &app.identities.error);
  app.identities.loaded = true;
}

void show_identities(App& app) {
  app.identities.open = true;
  focus_panel(app, WindowRole::Identities);
  refresh_identities(app);
  if (const auto* id = pane_gemini_identity(app, app.focus))
    app.identities.selected = id->sha256;
}

void note_gemini_identity(App& app, const FetchJob& job) {
  const FetchShared& r = *job.shared;
  if (r.identity_status == 0) return;
  // Only a page the user asked for opens the panel.
  if (job.purpose != FetchPurpose::Navigate && job.purpose != FetchPurpose::Tab)
    return;
  app.identities.request_url = r.final_url.empty() ? job.url : r.final_url;
  app.identities.request_status = r.identity_status;
  app.identities.request_meta = r.identity_meta;
  std::cerr << "gemini: " << app.identities.request_url << " asks for a client "
            << "certificate (status " << r.identity_status << ")\n";
  show_identities(app);
}

const xanatoo::GeminiIdentity* pane_gemini_identity(App& app, int pi) {
  const std::string url = pane_gemini_url(app, pi);
  if (url.empty()) return nullptr;
  if (!app.identities.loaded) refresh_identities(app);
  return xanatoo::gemini_identity_for(app.identities.ids, url);
}

void identity_command(App& app, xanatoo::UiCommand cmd, const std::string& arg) {
  using C = xanatoo::UiCommand;
  const std::string url = pane_gemini_url(app, app.focus);
  std::string err;
  switch (cmd) {
    case C::IdentitySelect:
      app.identities.selected = arg;
      break;
    case C::IdentityCreate:
      create_identity(app, arg);
      break;
    case C::IdentityCreateHere: {
      const std::string sha = create_identity(app, arg);
      if (!sha.empty() && !url.empty())
        use_identity(app, sha, xanatoo::gemini_scope_of_site(url));
      break;
    }
    case C::IdentityUseSite:
      if (!url.empty()) use_identity(app, arg, xanatoo::gemini_scope_of_site(url));
      break;
    case C::IdentityUsePage:
      if (!url.empty()) use_identity(app, arg, xanatoo::gemini_scope_of_page(url));
      break;
    case C::IdentityStopHere:
      if (url.empty()) break;
      if (report(app,
                 xanatoo::gemini_identity_stop_using(xanatoo::state_dir(),
                                                     !xanatoo::no_save(), url,
                                                     &err),
                 err, "Identity kept")) {
        flash_status(app, "This page goes out without a certificate now");
        reload_scope(app, xanatoo::gemini_scope_of_page(url));
      }
      break;
    case C::IdentityDropScope: {
      const auto [sha, scope] = split_arg(arg);
      if (report(app,
                 xanatoo::gemini_identity_drop_scope(xanatoo::state_dir(),
                                                     !xanatoo::no_save(), sha,
                                                     scope, &err),
                 err, "Scope kept"))
        reload_scope(app, scope);
      break;
    }
    case C::IdentityRename: {
      const auto [sha, name] = split_arg(arg);
      if (name.empty()) break;
      report(app,
             xanatoo::gemini_identity_rename(xanatoo::state_dir(),
                                             !xanatoo::no_save(), sha, name, &err),
             err, "Identity not renamed");
      break;
    }
    case C::IdentityDelete: {
      std::vector<std::string> scopes;
      for (const auto& id : app.identities.ids)
        if (id.sha256 == arg) scopes = id.scopes;
      if (!report(app,
                  xanatoo::gemini_identity_remove(xanatoo::state_dir(),
                                                  !xanatoo::no_save(), arg, &err),
                  err, "Identity not deleted"))
        break;
      if (app.identities.selected == arg) app.identities.selected.clear();
      flash_status(app, "Deleted the identity; its accounts are gone for good");
      for (const auto& s : scopes) reload_scope(app, s);
      break;
    }
    default:
      break;
  }
}

void identity_ui_state(App& app, xanatoo::UiState& st) {
  st.identities_open = app.identities.open;
  st.identity_page = pane_gemini_url(app, app.focus);
  const xanatoo::GeminiIdentity* active =
      st.identity_page.empty() ? nullptr : pane_gemini_identity(app, app.focus);
  st.identity_active_name = active ? active->name : std::string{};
  if (!st.site_identity_tip.empty() && active)
    st.site_identity_tip += " · identity: " + active->name;
  if (!app.identities.open) return;
  auto [x, y, w, h] = imgui_panel_rect(app, app.identities_win);
  st.identities_x = x;
  st.identities_y = y;
  st.identities_w = w;
  st.identities_h = h;
  st.identity_selected = app.identities.selected;
  st.identity_error = app.identities.error;
  st.identity_no_save = xanatoo::no_save();
  st.identity_page_site = st.identity_page.empty() ?
      std::string{} : xanatoo::gemini_scope_of_site(st.identity_page);
  st.identity_page_scope = st.identity_page.empty() ?
      std::string{} : xanatoo::gemini_scope_of_page(st.identity_page);
  const bool asked = !st.identity_page.empty() &&
      st.identity_page == app.identities.request_url;
  st.identity_request_status = asked ? app.identities.request_status : 0;
  st.identity_request_meta = asked ? app.identities.request_meta : std::string{};
  st.identity_rows.clear();
  for (const auto& id : app.identities.ids) {
    xanatoo::UiState::IdentityRow row;
    row.sha256 = id.sha256;
    row.name = id.name;
    row.created = id.created;
    row.scopes = id.scopes;
    row.saved = id.saved;
    if (!st.identity_page.empty())
      row.scope_here = xanatoo::gemini_identity_scope_for(id, st.identity_page);
    row.active_here = active && active->sha256 == id.sha256;
    st.identity_rows.push_back(std::move(row));
  }
}

}  // namespace xanatoo::app
