// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
// Local Nostr catalogue snapshot for the Nostr Explorer and the Identities
// panel's Nostr tab.

#include <algorithm>

#include "xanatoo/app.hpp"
#include "xanatoo/nostr.hpp"
#include "xanatoo/nostr_keys.hpp"
#include "xanatoo/nostr_store.hpp"
#include "xanatoo/paths.hpp"

namespace xanatoo::app {

void refresh_nostr_catalogue(App& app) {
  constexpr int kPageSize = 100;
  app.nostr_stats = nostr_cache_stats();
  app.nostr_people = nostr_cached_people(app.nostr_search, kPageSize + 1,
                                         app.nostr_page * kPageSize);
  app.nostr_events = nostr_cached_events(app.nostr_search, kPageSize + 1,
                                         app.nostr_page * kPageSize);
  app.nostr_groups = nostr_cached_groups(app.nostr_search, kPageSize + 1,
                                         app.nostr_page * kPageSize);
  app.nostr_people_more = app.nostr_people.size() > kPageSize;
  app.nostr_events_more = app.nostr_events.size() > kPageSize;
  app.nostr_groups_more = app.nostr_groups.size() > kPageSize;
  if (app.nostr_people_more) app.nostr_people.resize(kPageSize);
  if (app.nostr_events_more) app.nostr_events.resize(kPageSize);
  if (app.nostr_groups_more) app.nostr_groups.resize(kPageSize);
  refresh_nostr_identities(app);
}

void refresh_nostr_identities(App& app) {
  app.nostr_keys = nostr_keys_list(state_dir());
  app.nostr_identities = nostr_local_identities();
  // A key whose label went missing (cleared cache) gets one back.
  bool relabelled = false;
  for (const auto& k : app.nostr_keys)
    if (std::none_of(app.nostr_identities.begin(), app.nostr_identities.end(),
                     [&](const auto& i) { return i.pubkey == k.pubkey; }))
      relabelled |= nostr_add_watch_identity(
          nostr_bech32("npub", k.pubkey).substr(0, 16) + "…", k.pubkey);
  if (relabelled) app.nostr_identities = nostr_local_identities();
  if (!app.nostr_active_identity.empty() &&
      std::none_of(app.nostr_identities.begin(), app.nostr_identities.end(),
                   [&](const auto& i) { return i.pubkey == app.nostr_active_identity; }))
    app.nostr_active_identity.clear();
  if (app.nostr_active_identity.empty())
    app.nostr_active_identity = nostr_active_local_identity();
  if (app.nostr_active_identity.empty() && !app.nostr_identities.empty())
    app.nostr_active_identity = app.nostr_identities.front().pubkey;
}

}  // namespace xanatoo::app
