# Browsing history, snapshots and shared state — plan

Status (2026-11-18): **A done** (steps 1–2), **B + D recording done**
(step 3: visits and observations in `xanatoo.sqlite`), **C done** (step 4:
History panel as a trail tree); E, F and thumbnails are plan.

The request bundles five ideas. They depend on each other only in one
direction, so they can land as separate steps:

| # | Layer | Depends on | Size |
|---|-------|------------|------|
| A | Shared SQLite layer, safe with several processes | — | small |
| B | Visit graph: every followed link as an edge | A | medium |
| C | History panel as a time-ordered trail tree (`git log --graph`) | B | medium |
| D | Observations: *which* content a visit saw (etag, hashes) | A (B uses it) | small–medium |
| E | Snapshots: keeping that content (permanence, bookmarks) | D | large, open |
| F | History graph view: time axis, thumbnails, link edges | B, thumbnails | medium–large |

The key decision for "now or later": **record identity now, defer storage.**
Hashing what a visit saw costs a few microseconds and a 32-byte column;
keeping the bytes needs policies (size, retention, privacy) that are still
fuzzy. If B records D's fingerprints from day one, E can be added later
and every old visit can still be checked against a snapshot or the Wayback
Machine ("was this the same text?"). Without them that history is lost.

---

## Today

- `App::visited` — session list of addresses (≤ 100), saved in the session
  file; the History panel draws it flat, newest first.
- `DocBuffer::tab_nav` and `Pane::nav` — Back/Forward for tabs and floating
  documents respectively. The History panel’s **This Tab** button scopes
  the visit graph to the active tab, using the identity stored with the
  session and in the visit row’s `window` field. `NavLink`
  knows the source page, href and node of the followed link, but it is
  thrown away when the entry falls off.
- `minibuf_hist_url` — persistent URL ring, a text file rewritten on every
  visit. `bookmarks` — a text file rewritten on every change. Both are
  last-writer-wins: two running xanatoo processes silently drop each
  other's entries.
- SQLite already in use, each with its own open code: `revstore.cpp`
  (`revisions.sqlite`, content-addressed `objects`, `document_origins` with
  raw source bytes), `sidestore.cpp` (folds), `nostr_store.cpp` (cache,
  the only one with a busy timeout). None sets WAL mode.

---

## A. Shared SQLite layer

One small header `include/xanatoo/sqlite_db.hpp` (+ `.cpp`), used by all
stores:

- `Db open_db(path, schema_migrations)`: `journal_mode=WAL`,
  `synchronous=NORMAL`, `busy_timeout` (~2 s), `foreign_keys=ON`.
- Migrations by `PRAGMA user_version`: a list of SQL steps, applied in one
  `BEGIN IMMEDIATE` transaction; a newer database than the binary opens
  read-only with a warning instead of failing.
- RAII `Stmt` (the one in `revstore.cpp`, generalized: text / int64 / blob
  binds, `step`, column getters) and `Tx` (`BEGIN IMMEDIATE` … `COMMIT`,
  rollback on scope exit).
- Change detection without polling the tables: `PRAGMA data_version`
  changes when *another* connection committed. Panels check it once a
  frame (cheap) and reload only then — this is how a second process's
  visits and bookmarks show up live.

Rules for multi-process safety:
- Write transactions are short and never span a frame or a network wait.
- Never cache a row id across frames as if it were stable unless the row
  is insert-only (visits and observations are; heads are not).
- Read-modify-write goes through `BEGIN IMMEDIATE` or an `UPDATE … WHERE
  old = ?` check (revstore's optimistic heads already do this).
- WAL needs a local filesystem (no NFS); `state_dir()` is local in practice.
  Emscripten: in-memory / IDBFS with a single process, so WAL is skipped
  there. `XANATOO_SQLITE=OFF` keeps today's text-file fallbacks.

Which file: one `state_dir()/xanatoo.sqlite` for history, observations and
bookmarks (they reference each other, foreign keys want one file);
`revisions.sqlite` stays separate (it can grow large and is the document
store); `sidestore` folds move into the new file later. Caches stay in
`cache_dir()`.

Done: `include/xanatoo/sqlite_db.hpp` / `src/sqlite_db.cpp`
(`open`, `migrate`, `open_store`, `data_version`, `Db`, `Stmt`, `Tx`),
tests in `tests/sqlite_db_tests.cpp` (migration from an unversioned file,
failed step rolls back, newer schema → read-only, reader during a write,
busy writer, four threads with their own connections writing at once).
`revstore` and `sidestore` use `open_store` with their old schema as step 0
(existing files pass through it unchanged, then sit at user_version 1);
`nostr_store` uses `open` for the pragmas but keeps its column-probing
migration. Read-only paths under `--no-save` now open `ReadOnly`.

Done (step 2): `include/xanatoo/statedb.hpp` / `src/statedb.cpp`,
`state_dir()/xanatoo.sqlite` with `meta`, `bookmarks` (rowid = order
added) and `input_history` (ring, entry; rowid = recency, re-recording
replaces the row). Add / remove / record are single-row writes. On first
open the text files are imported once (`meta` marker `import.<file>`, in
one transaction, so simultaneous starters import once) and kept; the
bookmarks file is rewritten from the database after each change as a
readable export. `poll_shared_state` (once a second, not while a prompt
is open) reloads bookmarks and rings when `data_version` says another
process committed. `--no-save` opens the database read-only. Without
SQLite the text files remain the store. Schema step 3 adds
`bookmarks.folder` and the `bookmark_folders` table (docs/UI.md
"Bookmarks").

Originally planned as first users: `minibuf_hist_url` and bookmarks
move into tables, which fixes the multi-process clobbering immediately.
The text files stay as import (once) and export (File → Export bookmarks).

## B. Visit graph

A **visit** is one showing of a page in one view. The graph's nodes are
visits, not URLs — that is what makes trails a tree: the same URL reached
twice is two nodes, each with its own source. URL-level questions ("all
visits of X", "pages linking to X in my history") are an index on top.

```sql
CREATE TABLE pages (            -- one row per base address
  id INTEGER PRIMARY KEY,
  address TEXT NOT NULL UNIQUE, -- pane_address base, never path_of
  title TEXT NOT NULL DEFAULT '',
  first_seen INTEGER, last_seen INTEGER, visit_count INTEGER DEFAULT 0);

CREATE TABLE visits (
  id INTEGER PRIMARY KEY,       -- insert-only
  page INTEGER NOT NULL REFERENCES pages,
  address TEXT NOT NULL,        -- full address incl. //selector#anchor
  time INTEGER NOT NULL,        -- ms since epoch (UTC)
  leave_time INTEGER,           -- set when the view moves on (dwell time)
  source INTEGER REFERENCES visits, -- visit it was reached from, or NULL
  kind TEXT NOT NULL,           -- see below
  href TEXT,                    -- link as written on the source page
  source_node TEXT,             -- NodeId / passage address of the link
  observation INTEGER REFERENCES observations, -- D; NULL until loaded
  window TEXT NOT NULL,         -- process+window+pane instance id
  title TEXT NOT NULL DEFAULT '');
CREATE INDEX visits_time ON visits(time);
CREATE INDEX visits_page ON visits(page);
CREATE INDEX visits_source ON visits(source);
```

`kind`: `link` (followed in a document), `typed` (address bar / prompt),
`bookmark`, `history` (opened from History), `redirect`, `reload`,
`new-tab` (link opened in another tab/pane — source crosses views),
`restore` (session restore), `external` (command line / other app).
Back / Forward do **not** create visits: they return to an existing one;
an optional `returns(visit, time)` row records the revisit for dwell
statistics. Reload creates a visit with `source` = the reloaded one, so a
changed observation is visible as a pair.

Recording point: `push_nav_history` already has everything (pane, address,
`NavLink via`). It writes the visit and keeps the visit id in the
`NavHistoryEntry`, so the next push from that view knows its `source`
without guessing by URL. `SplitHistory` does the same for trail mode.
Same-document anchors (`navigate_anchor_link`) are visits with the same
page — cheap, and they show reading paths inside long pages.

Not recorded: `no_save()`, a future private window, and addresses matching
an exclude list (pref). Deleting history deletes visits (sources of later
visits become NULL — the trail breaks there, as it should). Retention: by
default keep everything (rows are ~200 bytes; 100 visits/day ≈ 7 MB/year);
a pref can cap by age.

`App::visited` becomes a view of the last N visits; the session file stops
storing it (with step 4, the panel rewrite).

### Done (step 3)

`include/xanatoo/visits.hpp` / `src/visits.cpp`, tables `pages`, `visits`,
`observations` (statedb schema step 2, as above; `kind` values `nav`,
`link`, `address`, `bookmark`, `history`, `new-tab`, `restore`,
`external`, `reload`). Recording (`src/app_nav.cpp`):

- `push_nav_history` → `record_visit`: the entry `nav_history_push` made
  current gets a visit; its id is kept in `NavHistoryEntry::visit`, so the
  next one from that view knows its source. Kind, in order: a followed
  link (`NavLink` → `href`, `source_node`); the view's
  `NavHistory::pending_kind`, moved there by `navigate_pane` from
  `App::visit_hint` (set just around the call by the location bar / URL
  prompt → `address`, or `bookmark` when the address is a bookmark;
  History panel → `history`; command line → `external`; session restore →
  `restore`, whose source is the latest earlier visit of the address, so
  trails continue across restarts); a tab opened from a link
  (`DocBuffer::origin_kind` / `origin_visit`, set by `open_link_in_tab`,
  used when the tab is first shown); otherwise `nav` from the previous
  visit. Starting kinds (`address`, `bookmark`, `external`) have no
  source; a `history` visit's source is the visit picked in the panel
  (`App::visit_from` → `NavHistory::pending_from`, or `origin_visit` for
  a new tab), so browsing on from an old visit grows that visit's branch
  instead of starting a new trail.
- Back / Forward record nothing (they return to an entry that has its
  visit). An entry without a visit (restored with the session) gets one
  when its page arrives. Placeholders ("Loading …") are not visits; the
  hint waits for the page.
- Reload (`refetch_page` → `finish_navigate`, local Reload in
  `app_frame.cpp`) → `record_reload_visit`: a `reload` visit whose source
  is the reloaded one.
- Each visit sets `leave_time` on the one the view showed before.
- Start-up: no visits for the default document before a session restore
  or for hidden views (`App::record_visits`).
- Sound and video handed to the Media panel (`navigate_pane`, an external
  link, a file on the command line) → `record_media_visit`: a visit of its
  own with the same kinds and sources (a clicked link's source is the page
  that stays in the view); `window` is the view, or `<id>/media` from the
  command line; no observation. The view's entry keeps its visit. Its id
  is `AudioPlayer::visit`; `close_player` (stop, another file, quit) sets
  its `leave_time`. The address also joins `App::visited` (History menu)
  and the URL ring. Played again from the History panel (`visit_resume`,
  also for streams found playable by the fetch, `finish_navigate`), the
  player takes the picked visit instead of recording a new one, like a
  page the view goes back to; the other mode (Shift, new tab) records a
  `history` visit.

Observations: the fetch worker fills `FetchShared::observed` in
`finish_fetch` (URL after redirects, time, status, and for web pages
content type, `ETag`, `Last-Modified` from `HttpStreamHeaders`; `raw_hash`
of the decoded body, `text_hash` = `document_text_hash`, size). It travels
with the page (`show_doc_in_pane`, `DocBuffer::observed`, `CachedPage`,
split previews) and is stored by the first visit (`observation_id`); an
identical latest observation of the same URL is reused. Local files are
observed at their visit (text hash, mtime in ms, size). Generated pages
have none.

Checked under Xvfb (isolated state): command line → link →
F5 → location bar → quit → restart gives `external`, `link` (href, node,
source 1), `reload` (source 2, same observation: same ETag and hashes),
`address` (no source), `restore` (source 4, other process).

Not yet: leave time on quit; a title for pages without `meta.title`;
`about:` and other generated pages are visits without observation
(intended); the exclude list.

## C. History panel: trail tree

Stays a list, time ordered (newest first), one row per visit, with a lane
graph left of the title like `git log --graph`:

```
● 14:32  Rust closures — The Book          ← link
│ ● 14:30  Hacker News (other tab)
● │ 14:28  Rust by Example: closures         ← link
├─╯ 14:27  HN: “Why closures …”              ← new-tab fork
● 14:20  news.ycombinator.com             ← typed
```

- Parent = `source`. Lanes are assigned in one pass newest → oldest exactly
  like git: a visit takes its child's lane, a second child forks a new lane,
  a lane ends at a visit without source (`typed`, `bookmark`) — a new trail.
- Pure function `history_lanes(visits) → rows{lane, edges}` in its own
  header with unit tests (no SDL); drawing is the panel's job.
- Day separators; collapse runs of anchors within the same page; filter
  box (title / address substring, SQL `LIKE` / FTS later); "this tab only"
  toggle (by `window`).
- Click opens, middle-click opens in new tab, hover → Context panel shows
  the observation (D) and, later, the thumbnail.
- Loading is paged by time (`WHERE time < ? ORDER BY time DESC LIMIT 200`),
  so years of history stay instant.

### Done (step 4)

`include/xanatoo/history_trail.hpp` / `src/history_trail.cpp` (pure, tests
in `tests/history_trail_tests.cpp`):

- `collapse_page_runs`: adjacent visits of one page where the newer one
  was reached from the older by an anchor jump (`link` / `nav` to another
  `#fragment`) or a `reload` become one entry (shown `×n`). A `restore` or
  the same address opened again stays a row of its own, so session breaks
  remain visible.
- `trail_nodes`: one node per entry (newest id, oldest member's source;
  sources into a run point at the run). With a filter, sources outside
  the list are dropped (only links between shown rows are drawn).
- `history_lanes`: the git pass. A visit takes the leftmost lane waiting
  for it; other lanes waiting for it merge there (diagonal); a visit no
  one waits for opens the leftmost free lane; a visit without source ends
  its lane. A source that is not loaded keeps its lane open to the bottom
  (the next page connects it). `lanes_below` gives the lines crossing a
  day header.

`visit_page(limit, after, filter)` (visits.cpp) pages by `(time, id)`, so
visits with the same millisecond are not skipped, and filters title /
address with `LIKE … ESCAPE` (`%` and `_` literal).

Panel (`src/app_history.cpp`, state `App::history`): day headers, lane
graph (colours `theme::lane_color`), time, title (or address), how it was
reached on the right (`link`, `typed`, `new tab`, `restored`, …). Hollow
dot = starts a trail (no source in the list). Rows the views show now are
marked. 200 visits per page, the next page when scrolled near the end;
stale after `record_visit` and when `poll_shared_state` sees another
process's commit (selection follows its visit). Click / Enter on a visit
still in a view's Back / Forward trail steps that trail to it (focusing
the view; `nav_rewind_to_visit`), like pressing Back several times, so no
new visit is recorded. Other visits are opened in the focused view and
its new entry takes the picked visit's id (`App::visit_resume` →
`NavHistory::pending_resume`, handled first in `record_visit`): again no
new row, and links followed from there continue that visit's branch.
Middle-click, Ctrl+Enter and the other mode (Shift) open the page anew
(kind `history`, source = the picked visit). Preference "History clicks go back in the
view" (`ViewSettings::history_rewinds`, view.conf `history_rewinds`, on)
picks the default; Shift+click / Shift+Enter does the other one. `/` or
Ctrl+F opens the filter
prompt (`PromptKind::HistoryFilter`, applied while typing, Enter keeps,
Esc cancels); Esc in the panel clears the filter, then closes. Hover
tooltip: address, time and dwell, kind and source title, href, `×n`, and
the observation (status, type, size, ETag, Last-Modified, text hash).
Without a visit log (no SQLite / no state dir) it lists `App::visited`.
Right-click on a row: menu (`UiCommand::HistoryAction` on
`HistoryPanel::menu_id`): Open, Open in New Tab, the other open mode,
Copy Address, Remove from History. ↑/↓, Page Up/Down, Home/End move
the selection and open it like a click (Shift: the other mode); Enter
opens it again. Delete removes the selected row:
`remove_visits` deletes the row's visits (a page run: all of them) and
re-links the visits reached from them to the removed one's source, so
trails stay connected; views showing them keep their pages, their
entries take that source. `store_visit` stores a source removed
meanwhile (another process) as NULL.

Not done: "this tab only" toggle (by `window`), the hover → Context panel
(the tooltip shows the observation instead), FTS. `App::visited` stays (the
History menu and the fallback), so the session file still stores it.

## D. Observations: identifying what was seen

HTTP gives no content identity, so record several independent ones and let
consumers pick:

```sql
CREATE TABLE observations (
  id INTEGER PRIMARY KEY,       -- insert-only
  url TEXT NOT NULL,            -- final URL after redirects
  time INTEGER NOT NULL,
  status INTEGER, content_type TEXT,
  etag TEXT, last_modified TEXT,      -- validators as the server sent them
  raw_hash TEXT,                -- sha256 of the body bytes (as decoded)
  text_hash TEXT,               -- sha256 of the canonical document text
  size INTEGER,
  snapshot TEXT,                -- E: content id in the object store, or NULL
  archive_url TEXT);            -- E: Wayback/archive.today copy, if known
CREATE INDEX observations_url ON observations(url, time);
```

- `raw_hash` changes on every load of most dynamic pages (nonces, ads,
  CSRF tokens), so it alone cannot say "same content". `text_hash` is the
  hash of the *imported document* serialized canonically (the S-expression
  writer, reader-mode text) — stable across such noise and comparable for
  service pages (HN, Reddit, feeds) that never had raw HTML. That is the
  one bookmarks and passages compare against.
- Local files: path + mtime + raw hash. Generated pages (`about:`,
  `history:`, `//gallery`) get no observation.
- A visit reloading an identical page (same `text_hash` and url) reuses the
  observation row instead of inserting another.
- Recording point: where the fetch result becomes a buffer (`html_page.cpp`
  / `app_buffers.cpp`); headers come from the `https.hpp` header callback.
  This needs the hashes and headers carried on the fetch result —
  the only invasive part of D.

## E. Snapshots: a permanent layer over the mutable Web

The original plan (built differently, see "Done" below):

- Storage is `revstore`'s content-addressed `objects` (sha256 → bytes),
  which already holds `document_origins` raw bytes. An observation's
  `snapshot` is a content id there. Snapshot = the raw body plus the
  canonical document (both content ids; images and other subresources as
  further objects listed in a small manifest object — WARC-like but in the
  same store). Compression (zstd) per object later.
- Policy decides what is kept, not the schema:
  - **bookmarks: always** (making a bookmark snapshots the current page);
  - pages with annotations or quotes: always;
  - history: optional, size-capped LRU (pref; default off or small);
  - never: large binaries, `no_save()`.
- **Permanent address:** `address@observation` — e.g.
  `https://x/a//p:3@sha256:ab12…` shows the snapshot instead of the live
  page; a status note says "snapshot of 2026-11-18, the live page differs /
  is gone (open live)". This joins the passage addresses
  (`docs/PASSAGES.md`) and revisions (`docs/REVISIONS.md`): same idea —
  immutable id plus a mutable human alias.
- Fallback when a snapshot was not kept: the Wayback Machine by URL +
  visit time (`archive_url`; we already speak Wayback, `docs/dev/SERVICES.md`),
  verified against `text_hash` where possible. Optionally "Save Page Now"
  on bookmark (pref, it is an outward-facing request).
- Bookmarks then become rows `(address, title, quote, created,
  observation)`; re-anchoring a passage uses the quote against the live
  page, falling back to the snapshot.

Open: content negotiation / logged-in pages (snapshots may contain private
data → the store must be treated as sensitive), dedup of near-identical
snapshots (text diffs between successive observations?), export format
(WARC for interop?).

### Done (snapshots, 2026-11-24)

The history is now what the user saw, not a list of URLs to load again.
It differs from the plan above where the plan was too narrow:

- **Object store** (`object_store.hpp`): `cache_dir()/objects/<sha256>`,
  plain files, written atomically, never changed, pruned by size (LRU,
  `kObjectCacheBytes` = 2 GiB, `prune_image_caches` at start-up). It is
  in the cache, not in `revstore`: snapshots of foreign pages are
  re-creatable cache data, revisions of the user's own documents are not.
  The URL cache hard-links every body it writes in (`UrlCacheEntry::body`,
  `urls.sqlite` migration 2), so a picture costs one copy however many
  snapshots keep it, and a newer answer (a new file, renamed over the old
  one) leaves the old object alone.
- **Document codec** (`doc_codec.hpp`): a lossless binary form of the
  `Document` as the view showed it ("XDOC" v1, varints); every node and
  span field, so a service page (HN, Reddit, feeds) with no HTML behind it
  comes back exactly. The decoder rejects anything malformed or truncated.
- **Snapshots** (`snapshot.hpp`, `cache_dir()/snapshots.sqlite`): document
  object + raw source object + type, service, title, first / last seen;
  `snapshot_resources` lists the pictures it showed and the object each
  one had. Pictures already in the URL cache are taken at once, later
  ones when they arrive (`snapshot_note_body` from `url_cache_put`, for
  snapshots seen within a day, so an old snapshot never takes a newer
  picture).
- **Immutable address** `snap:<32 hex>/<address>` instead of
  `address@observation`: the id is content-addressed (sha256 of
  "xanatoo-snapshot 1", address and document hash, 128 bits), so the same
  page with the same content is one snapshot, and the original address is
  inside the ref, so `live_address` always gives the live page back, also
  when the snapshot was evicted. Pictures of a shown snapshot are
  `snap:ID/<picture URL>` (`rewrite_snapshot_images`); `snapshot_image_path`
  reads the kept copy, else the URL cache file, so the original page and
  its snapshot share one cache. A pruned URL cache file is put back from a
  snapshot's copy before anything is downloaded (`snapshot_restore_body` in
  `remote_placeholder`).
- **Policy:** every page shown in a view (Navigate / Tab / Preview fetches
  of in-app remote pages that answered 200) is kept; subscriptions and
  downloads are not. Size-capped by the object store; `snapshot_prune_missing`
  drops rows whose document object is gone.
- **Showing one:** `PageObservation::snapshot` (statedb migration: index
  on it) ties visit → observation → snapshot; a snapshot shown again reuses
  the observation it was seen as. `NavHistoryEntry::snapshot` (session key
  `navsnap<view>.<n>`) remembers the snapshot each Back / Forward entry
  showed; `nav_go` and the History panel's `open_line` hand it to the next
  fetch through `App::open_snapshot` (taken by `start_fetch`;
  `find_cached_page` only answers with that snapshot meanwhile). The
  worker (`fetch_page`) loads the snapshot instead of the network; the view
  keeps the original address, the status line says "As seen DATE … (Reload
  shows the page now)", and Reload loads the live page (the entry then
  remembers the new snapshot). A snapshot no longer kept falls back to the
  live page with a note. A `snap:` ref typed or opened directly works the
  same; History → Copy Snapshot Address copies the ref of the page shown.
- **Bookmarks pin their snapshot:** a new bookmark takes the snapshot of
  the page as a view shows it (`Bookmark::snapshot`, `bookmarks.snapshot`
  column, `snap=` in the export file). `sync_bookmark_pins` (start-up
  before pruning, add / remove, changes from another process) makes those
  the `snapshot_pins` rows of owner "bookmark"; `object_prune` keeps every
  object `snapshot_pinned_objects` names (document, raw source, pictures)
  and does not count them against the size cap. A pin has no foreign key,
  so it holds again should the same content come back after a cache wipe.
  Bookmarks panel → "Open as Bookmarked" opens the `snap:` ref.
- Not done: Wayback fallback for evicted snapshots, compression, stylesheets / fonts /
  scripts as resources (pages are imported documents, so only pictures
  matter today).

### Later: full-text search over cache and history

Planned, not built. A snapshot is the unit to index, since it is the text
the user saw:

- `snapshots.sqlite` gets an FTS5 table `snapshot_text(id UNINDEXED,
  title, body)` filled in `snapshot_store` from the document's plain text
  (the same walk `text_hash` uses), only for new ids; deleted along with
  the snapshot row (`snapshot_prune_missing`). The SQLite build must have
  FTS5 (check `sqlite3_compileoption_used("ENABLE_FTS5")`; without it the
  search falls back to `LIKE` on titles).
- Query page `history:search?q=…`: matches joined to `snapshots` (url,
  first_seen) and to observations → visits for the trail; each result opens
  its snapshot (`snap:` ref), with an FTS `snippet()` as the note.
- History panel filter gains "search page text" next to the address /
  title filter.

## F. History graph view

A generated page `history:graph` (optionally `history:graph//day:2026-11-18`)
drawn by its own view, like the gallery:

- x = time, compressed: idle gaps longer than a few minutes collapse to a
  fixed break mark, so a reading session is not a dot on a day-wide axis.
- y = trail lanes from C's lane assignment (same function, so panel and
  graph agree), or a force/layered layout as a later option.
- Nodes = visits as thumbnails; repeated visits of one page can be linked
  by a faint "same page" line in addition to the `source` edges.
- Edges = curves from source to target, styled by `kind` (typed starts are
  marked, new-tab forks dashed).
- Zoom / pan like the single-image gallery view; click opens; hover →
  Context panel.

Needs **page thumbnails**: render the pane once after the page settled into
a small texture → PNG/WebP in `cache_dir()/thumbs/<observation>.png`
(keyed by observation, so a changed page gets a new thumbnail; a
reused observation reuses it). Missing thumbnails fall back to favicon +
title. This is useful on its own (tabs overview, bookmark panel, Context
panel hover) and can come before F.

Colours / spacing through new sheet roles (`history-graph`, …), not
hard-coded.

---

## Order of work

1. **A** `sqlite_db.hpp`, WAL + migrations; port `nostr_store`, `sidestore`,
   `revstore` opens onto it. Tests: two connections in one test process
   writing concurrently; migration from version 0.
2. **A** URL ring + bookmarks into `xanatoo.sqlite` (one-time import of the
   text files, export command); `data_version` refresh of the Bookmarks
   panel. Fixes the two-process data loss now.
3. **B + D (identity only)** `visits`, `pages`, `observations` with
   validators and both hashes; recording in `push_nav_history` and the
   fetch path; `App::visited` from the DB. Tests: recording kinds, source
   chains, observation reuse, `no_save()`.
4. **C** lane function + tests, History panel rewrite, filter, paging. (done)
5. **Thumbnails** cache + Context panel / bookmarks use.
6. **F** `history:graph` view.
7. **E** snapshots for bookmarks first, `@observation` addresses,
   Wayback fallback; then the history policy.

Steps 1–4 are the "do now" part and each is one commit-sized piece;
5–7 can wait until the identity data has accumulated and the snapshot
questions are clearer.

## Questions to settle

- One shared `xanatoo.sqlite` vs. one file per store (plan: shared for
  history / observations / bookmarks, revisions separate).
- Should Back/Forward be edges in the graph (plan: no, a `returns` log)?
- Default history retention and whether history snapshots are on at all.
- Is `text_hash` over the S-expression serialization stable enough across
  importer changes? (A version tag in the hash input — `xanatoo-text 1` —
  lets old hashes be recognized as incomparable rather than "changed".)
