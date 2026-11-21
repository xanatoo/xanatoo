// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// Identity panel (WindowRole::Identities), one tab per protocol.
// Gemini: client certificates, which servers treat as accounts. Creates them,
// chooses which one a page or a whole server sees, renames and deletes them.
// A page that asks for a certificate (status 60/61/62) opens the panel on
// itself. The store and scope matching live in gemini_identity.cpp;
// certificates are made in the TLS backend (tls_make_client_identity).
// Nostr: watch-only accounts (public keys, no signing) in nostr_store; one is
// the active account. The tab follows the focused page's protocol.

#include <ctime>
#include <iostream>

#include "xanatoo/app.hpp"
#include "xanatoo/gemini.hpp"
#include "xanatoo/gemini_identity.hpp"
#include "xanatoo/nostr.hpp"
#include "xanatoo/nostr_store.hpp"
#include "xanatoo/paths.hpp"
#include "xanatoo/tls.hpp"

namespace xanatoo::app {

namespace {

std::string pane_gemini_url(App& app, int pi) {
  if (!view_live(app, pi)) return {};
  const std::string& path = path_of(app, app.panes[pi]);
  return xanatoo::is_gemini_url(path) ? path : std::string{};
}

// "a\nb" → {a, b}
std::pair<std::string, std::string> split_arg(const std::string& arg) {
  const size_t nl = arg.find('\n');
  if (nl == std::string::npos) return {arg, {}};
  return {arg.substr(0, nl), arg.substr(nl + 1)};
}

// Protocol of view `pi`'s page: 0 none, 1 Gemini, 2 Nostr.
int page_kind(App& app, int pi) {
  if (!view_live(app, pi)) return 0;
  const std::string& path = path_of(app, app.panes[pi]);
  if (xanatoo::is_gemini_url(path)) return 1;
  if (xanatoo::is_nostr_ref(path)) return 2;
  return 0;
}

xanatoo::IdentitySection section_of(int kind, xanatoo::IdentitySection other) {
  return kind == 1 ? xanatoo::IdentitySection::Gemini :
         kind == 2 ? xanatoo::IdentitySection::Nostr : other;
}

// Hex key of a saved-account reference (npub1…, nprofile1…, hex).
std::string nostr_hex(const std::string& ref) {
  if (auto pk = xanatoo::nostr_decode_pubkey(ref)) return *pk;
  std::string t = ref;
  while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
  while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(t.begin());
  if (t.size() != 64) return {};
  for (char& c : t) {
    if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return {};
  }
  return t;
}

const NostrLocalIdentity* nostr_account(const App& app, const std::string& pk) {
  for (const auto& i : app.nostr_identities)
    if (i.pubkey == pk) return &i;
  return nullptr;
}

void nostr_command(App& app, xanatoo::UiCommand cmd, const std::string& arg) {
  using C = xanatoo::UiCommand;
  app.identities.error.clear();
  switch (cmd) {
    case C::NostrAddWatchIdentity: {
      auto [label, ref] = split_arg(arg);
      const std::string pk = nostr_hex(ref);
      if (label.empty() && !pk.empty())
        label = xanatoo::nostr_bech32("npub", pk).substr(0, 16) + "…";
      const bool renaming = !pk.empty() && nostr_account(app, pk);
      if (pk.empty())
        app.identities.error = "Not a Nostr public key (npub1… or 64 hex digits)";
      else if (xanatoo::no_save())
        app.identities.error = "--no-save: Nostr accounts are not saved";
      else if (!nostr_add_watch_identity(label, pk))
        app.identities.error = "The local Nostr database is not available";
      if (!app.identities.error.empty()) {
        flash_status(app, "No Nostr account saved: " + app.identities.error);
        break;
      }
      refresh_nostr_identities(app);
      app.identities.nostr_shown = pk;
      const NostrLocalIdentity* a = nostr_account(app, pk);
      flash_status(app, std::string(renaming ? "Renamed Nostr account " :
                                               "Saved Nostr account ") +
                            (a ? a->label : std::string{}));
      break;
    }
    case C::NostrSelectIdentity:
      if (nostr_account(app, arg) && nostr_set_active_local_identity(arg))
        app.nostr_active_identity = arg;
      else if (nostr_account(app, arg))
        flash_status(app, "Active Nostr account not saved");
      break;
    case C::NostrIdentityShow:
      app.identities.nostr_shown = arg;
      break;
    case C::NostrIdentityRemove:
      if (!nostr_remove_local_identity(arg)) {
        app.identities.error = "Nostr account not removed";
        flash_status(app, app.identities.error);
        break;
      }
      if (app.nostr_active_identity == arg) app.nostr_active_identity.clear();
      if (app.identities.nostr_shown == arg) app.identities.nostr_shown.clear();
      refresh_nostr_identities(app);
      flash_status(app, "Removed the Nostr account (only its local label)");
      break;
    default:
      break;
  }
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
  app.identities.page_kind = page_kind(app, app.focus);
  show_identities(app, section_of(app.identities.page_kind,
                                  app.identities.section));
}

void show_identities(App& app, xanatoo::IdentitySection section) {
  app.identities.open = true;
  app.identities.section = section;
  focus_panel(app, WindowRole::Identities);
  refresh_identities(app);
  refresh_nostr_identities(app);
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
  app.identities.section = xanatoo::IdentitySection::Gemini;
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
    case C::ToggleNostrAccounts:
      if (app.identities.open &&
          app.identities.section == xanatoo::IdentitySection::Nostr)
        app.identities.open = false;
      else
        show_identities(app, xanatoo::IdentitySection::Nostr);
      break;
    case C::IdentitySection:
      app.identities.section = arg == "nostr" ?
          xanatoo::IdentitySection::Nostr : xanatoo::IdentitySection::Gemini;
      app.identities.error.clear();
      break;
    case C::NostrAddWatchIdentity:
    case C::NostrSelectIdentity:
    case C::NostrIdentityShow:
    case C::NostrIdentityRemove:
      nostr_command(app, cmd, arg);
      break;
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
  // The tab follows the focused page when it moves to another protocol.
  const int kind = page_kind(app, app.focus);
  if (kind != app.identities.page_kind) {
    app.identities.page_kind = kind;
    app.identities.section = section_of(kind, app.identities.section);
  }
  st.identity_section = app.identities.section;
  if (!app.identities.open) return;
  st.nostr_shown_identity = app.identities.nostr_shown;
  st.nostr_page_pubkey.clear();
  if (kind == 2)
    if (auto pk = xanatoo::nostr_decode_pubkey(path_of(app, app.panes[app.focus])))
      st.nostr_page_pubkey = *pk;
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
