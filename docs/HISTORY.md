# Browsing history, snapshots and shared state — plan

Status: **plan** (2026-11-18), nothing implemented yet.

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
- `Pane::nav` / `NavHistory` — per-view Back/Forward; `NavLink` already
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

First users (small, independent wins): `minibuf_hist_url` and bookmarks
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
storing it.

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

Deferred; here is the shape so B and D do not paint it into a corner.

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
4. **C** lane function + tests, History panel rewrite, filter, paging.
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
