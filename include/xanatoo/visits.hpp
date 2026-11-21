// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Browsing history as a graph (docs/HISTORY.md "B", "D"): every showing of a
// page in a view is a visit, linked to the visit it was reached from; every
// fetch records an observation of what the page was (validators, hashes).
// Stored in state_dir()/xanatoo.sqlite (statedb.hpp). Without a database
// (no SQLite, --no-save) nothing is recorded and the store calls return 0 /
// nothing.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xanatoo {

struct Document;

// How a visit was reached.
enum class VisitKind : uint8_t {
  Nav,       // another in-view navigation (panels, Up, gallery steps, …)
  Link,      // a link followed in the source page
  Address,   // an address chosen in the chrome (location bar, menus)
  Bookmark,  // a bookmark opened
  History,   // opened from the History panel
  NewTab,    // a link opened in a new tab (source is in another view)
  Restore,   // reopened by session restore
  External,  // command line / another program
  Reload,    // the same page loaded again (new observation)
};
const char* visit_kind_name(VisitKind k);
std::optional<VisitKind> parse_visit_kind(std::string_view s);
// Starts a new trail: the visit has no source.
bool visit_kind_starts_trail(VisitKind k);

// What a fetch of a page returned. `time` 0 = nothing observed.
struct PageObservation {
  std::string url;           // final URL after redirects (or file: URL)
  int64_t time = 0;          // ms since the epoch, UTC
  int status = 0;            // HTTP status (0: not HTTP)
  std::string content_type;
  std::string etag;          // validators as the server sent them
  std::string last_modified;
  std::string raw_hash;      // "sha256:…" of the body (decoded text)
  std::string text_hash;     // document_text_hash of the imported page
  int64_t size = -1;         // body bytes, -1 unknown
  bool empty() const { return time == 0; }
};

// "sha256:…" over the document's text in reading order, whitespace
// collapsed, prefixed with a version tag ("xanatoo-text 1"): stable across
// markup noise (nonces, ads, attribute churn) that changes every raw body.
// Empty for an empty document.
std::string document_text_hash(const Document& doc);

struct VisitRecord {
  int64_t id = 0;
  std::string address;       // full address (pane_address)
  int64_t time = 0;          // ms since the epoch, UTC
  int64_t leave_time = 0;    // 0 = still shown / unknown
  int64_t source = 0;        // visit it was reached from, 0 = none
  VisitKind kind = VisitKind::Nav;
  std::string href;          // link as written on the source page
  std::string source_node;   // link's node id on the source page
  int64_t observation = 0;
  std::string window;        // process instance + "/" + view index
  std::string title;
};

int64_t now_epoch_ms();
// Random id of this process, for VisitRecord::window.
const std::string& visit_instance_id();

// Stores `o`, or returns the latest observation of the same URL when it
// is identical (same hashes and validators). 0 when not stored.
int64_t store_observation(const PageObservation& o);
std::optional<PageObservation> load_observation(int64_t id);
// Stores the visit and updates its page row; returns its id or 0.
int64_t store_visit(const VisitRecord& v);
// The view moved on from visit `id` at `time`.
void store_visit_leave(int64_t id, int64_t time);
// Latest visit of the page `address` (exact address), 0 when none.
int64_t latest_visit_of(std::string_view address);
// True when a visit of the page `page` (a locator base, as the pages table
// keys it) is in the log: visited links (docs/STYLESHEETS.md).
bool page_visited(std::string_view page);
// Deletes the visits `ids` (in order); visits reached from one of them
// take its source, so trails stay connected. Returns how many went.
size_t remove_visits(const std::vector<int64_t>& ids);
// True when the visit log can be read (SQLite built in, state_dir usable).
bool visit_log_available();
// Up to `limit` visits, newest first, older than `before` (ms; 0 = now).
std::vector<VisitRecord> recent_visits(size_t limit, int64_t before = 0);

// A page of the visit log for the History panel: newest first, after
// (older than) the visit `after` (0 = from the newest; ties in time are
// ordered by id, so paging loses nothing), and with a non-empty `filter`
// only visits whose title or address contains it (ASCII case-insensitive).
std::vector<VisitRecord> visit_page(size_t limit, const VisitRecord* after,
                                    std::string_view filter = {});

}  // namespace xanatoo
