// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Page snapshots: what a page was when it was shown (docs/HISTORY.md "E").
// Every fetched page is kept as the document the view showed (doc_codec),
// its raw source and, as they arrive, the pictures it showed; History and
// Back / Forward show that copy instead of loading the page again.
//
// Immutable address of a snapshot, mirroring `wayback:TS/URL`:
//
//   snap:<32 hex>/<address>       e.g. snap:3fa9…c2/https://example.com/a
//
// The id is content-addressed: the first 128 bits of
// sha256("xanatoo-snapshot 1\0" address "\0" document object hash), so the
// same page with the same content is the same snapshot. The address is part
// of the ref, so the live page is known even when the snapshot has been
// evicted from the cache (`live_address`). A picture as a snapshot showed it
// is `snap:<id>/<picture URL>` (`rewrite_snapshot_images`).
//
// Storage under cache_dir(): objects in the object store (object_store.hpp),
// rows in snapshots.sqlite:
//
//   snapshots           id, address, document / raw source objects, type,
//                       service, title, first and last time seen
//   snapshot_resources  (snapshot, picture URL) → object, '' while not yet
//                       loaded; filled when the URL cache stores the bytes
//
// Safe from any thread (one connection behind a mutex). Without
// XANATOO_SQLITE refs still parse, but nothing is stored or found.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "xanatoo/types.hpp"

namespace xanatoo {

// --- Addresses (pure) ---------------------------------------------------

struct SnapshotRef {
  std::string id;   // 32 lowercase hex digits
  std::string url;  // the page's own address (never itself a snap: ref)
};

std::optional<SnapshotRef> parse_snapshot_ref(std::string_view ref);
bool is_snapshot_ref(std::string_view ref);
bool is_snapshot_id(std::string_view id);
// "snap:<id>/<url>"; a snap: `url` is unwrapped first (no nesting).
std::string snapshot_ref_string(std::string_view id, std::string_view url);
// The live address behind a snapshot address ("snap:ID/URL" → URL,
// selectors kept); anything else unchanged.
std::string live_address(std::string_view address);
// Content-addressed id of `url` with the document object `doc_hash`.
std::string snapshot_id_for(std::string_view url, std::string_view doc_hash);

// Remote picture URLs a document shows (Image nodes, inline images), in
// document order, each once.
std::vector<std::string> remote_image_srcs(const Document& doc);
// Points every remote picture at its copy in snapshot `id`
// (src → snap:<id>/<src>).
void rewrite_snapshot_images(Document& doc, std::string_view id);

// --- Store ----------------------------------------------------------------

struct SnapshotInfo {
  std::string id;
  std::string url;           // the page's address
  std::string doc;           // object hash of the encoded document
  std::string raw;           // object hash of the raw source, '' = none
  std::string content_type;
  std::string service;       // fetcher name ("Web", "Hacker News", …)
  std::string title;
  int64_t first_seen = 0;    // ms since the epoch, UTC
  int64_t last_seen = 0;
  int resources = 0;         // pictures the page showed
  int resources_kept = 0;    // of those, the ones whose bytes are kept
};

// Keep `doc` as shown at `url` (worker side). `raw`: the source it was
// read from. Pictures already in the URL cache are taken at once; the
// others when they arrive. Returns the snapshot id, empty when nothing
// was stored (no cache directory, no SQLite).
std::string snapshot_store(std::string_view url, const Document& doc,
                           const std::string* raw,
                           std::string_view content_type,
                           std::string_view service);

std::optional<SnapshotInfo> snapshot_find(std::string_view id);

struct LoadedSnapshot {
  SnapshotInfo info;
  Document doc;  // pictures rewritten to their snap: copies
  std::optional<std::string> raw;
};
// The snapshot, ready to show; nullopt when unknown or evicted (its
// document object is gone or does not decode).
std::optional<LoadedSnapshot> snapshot_load(std::string_view id);

// Snapshots of the page `url`, newest first.
std::vector<SnapshotInfo> snapshots_of(std::string_view url, size_t limit = 50);

// Where the picture `snap:<id>/<url>` is read from: the snapshot's copy,
// or (not kept) the URL cache file of `url`. A plain URL: its cache file.
std::string snapshot_image_path(std::string_view src);

// The URL cache stored the bytes of `url` (object `hash`): snapshots seen
// within the last day that still wait for that picture keep this copy.
void snapshot_note_body(std::string_view url, std::string_view hash);

// The newest copy of `url` any snapshot kept, as an object hash (empty
// when none): restores an evicted URL cache file.
std::string snapshot_latest_copy(std::string_view url);

// The URL cache file `path` of `url` is gone (pruned): put back the bytes
// it last held, or the newest copy a snapshot kept. A snap: `url`: the copy
// of its snapshot isn't there, so restore the plain picture. True when
// `path` exists afterwards.
bool snapshot_restore_body(std::string_view url, const std::string& path);

// Forget snapshots whose document object was pruned (after object_prune;
// a background thread at start-up).
void snapshot_prune_missing();

// Close the store (the next call reopens it): tests.
void snapshot_close();

}  // namespace xanatoo
