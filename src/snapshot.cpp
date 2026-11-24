// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/snapshot.hpp"

#include "xanatoo/doc_codec.hpp"
#include "xanatoo/object_store.hpp"
#include "xanatoo/paths.hpp"
#include "xanatoo/url_cache.hpp"
#include "xanatoo/util.hpp"

#ifndef XANATOO_SQLITE
#define XANATOO_SQLITE 1
#endif
#if XANATOO_SQLITE
#include "xanatoo/sqlite_db.hpp"
#endif

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace xanatoo {
namespace {

constexpr std::string_view kScheme = "snap:";
constexpr size_t kIdLen = 32;
// Pictures arriving later than this after a snapshot was last seen are
// not its pictures any more (the page may show others by now).
constexpr int64_t kLateBodyMs = 24ll * 60 * 60 * 1000;

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool is_lower_hex(std::string_view s) {
  for (char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

// A picture a snapshot can keep: one the image fetcher downloads.
bool keepable_src(std::string_view src) {
  return !src.empty() && has_image_fetch_scheme(src);
}

std::optional<std::string> read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream ss;
  ss << in.rdbuf();
  if (in.bad()) return std::nullopt;
  return std::move(ss).str();
}

// The object holding the URL cache's bytes of `url`, stored now if the
// cache file predates the object store. Empty when the cache has none.
std::string cached_body_object(const std::string& url) {
  if (auto e = url_cache_find(url)) {
    if (e->status != 0 && e->status != 200) return {};
    if (!e->body.empty() && object_exists(e->body)) return e->body;
  }
  const std::string path = url_cache_body_path(url);
  std::error_code ec;
  if (path.empty() || !fs::is_regular_file(path, ec)) return {};
  const auto bytes = read_file(path);
  if (!bytes || bytes->empty()) return {};
  const std::string hash = object_hash(*bytes);
  return object_put_file(path, hash) ? hash : std::string();
}

#if XANATOO_SQLITE
const sqlite::Migrations kSteps = {
    "CREATE TABLE IF NOT EXISTS snapshots ("
    "  id TEXT PRIMARY KEY, url TEXT NOT NULL,"
    "  doc TEXT NOT NULL, raw TEXT NOT NULL DEFAULT '',"
    "  content_type TEXT NOT NULL DEFAULT '',"
    "  service TEXT NOT NULL DEFAULT '', title TEXT NOT NULL DEFAULT '',"
    "  first_seen INTEGER NOT NULL, last_seen INTEGER NOT NULL);"
    "CREATE INDEX IF NOT EXISTS snapshots_url ON snapshots(url, last_seen);"
    "CREATE TABLE IF NOT EXISTS snapshot_resources ("
    "  snapshot TEXT NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE,"
    "  url TEXT NOT NULL, object TEXT NOT NULL DEFAULT '',"
    "  PRIMARY KEY (snapshot, url));"
    "CREATE INDEX IF NOT EXISTS snapshot_resources_url"
    "  ON snapshot_resources(url);",
    // Pins (bookmarks): no foreign key, a pin outlives an evicted snapshot
    // and holds again should the same content come back.
    "CREATE TABLE snapshot_pins ("
    "  owner TEXT NOT NULL, snapshot TEXT NOT NULL,"
    "  PRIMARY KEY (owner, snapshot));",
};

std::mutex g_mutex;
sqlite::Db g_db;
std::string g_db_path;

// Caller holds g_mutex. nullptr: no cache directory.
sqlite3* store() {
  const std::string dir = cache_dir();
  if (dir.empty()) return nullptr;
  const std::string path = (fs::path(dir) / "snapshots.sqlite").string();
  if (g_db && g_db_path == path) return g_db;
  g_db = sqlite::Db();
  g_db_path = path;
  if (!ensure_dir(dir)) return nullptr;
  g_db = sqlite::Db(sqlite::open_store(path, kSteps));
  return g_db;
}

constexpr const char* kInfoCols =
    "s.id, s.url, s.doc, s.raw, s.content_type, s.service, s.title, "
    "s.first_seen, s.last_seen, "
    "(SELECT COUNT(*) FROM snapshot_resources r WHERE r.snapshot = s.id), "
    "(SELECT COUNT(*) FROM snapshot_resources r WHERE r.snapshot = s.id "
    "   AND r.object != '')";

SnapshotInfo info_row(const sqlite::Stmt& s) {
  SnapshotInfo i;
  i.id = s.text(0);
  i.url = s.text(1);
  i.doc = s.text(2);
  i.raw = s.text(3);
  i.content_type = s.text(4);
  i.service = s.text(5);
  i.title = s.text(6);
  i.first_seen = s.int64(7);
  i.last_seen = s.int64(8);
  i.resources = static_cast<int>(s.int64(9));
  i.resources_kept = static_cast<int>(s.int64(10));
  return i;
}
#endif

// snap:ID/URL → file path, memoized (ImageCache asks every frame). Found
// copies stay valid (objects never change); misses are retried after a
// picture arrived (g_generation).
std::mutex g_memo_mutex;
std::unordered_map<std::string, std::string> g_found;
std::unordered_map<std::string, uint64_t> g_missed;
std::atomic<uint64_t> g_generation{1};

}  // namespace

// --- Addresses -------------------------------------------------------------

bool is_snapshot_id(std::string_view id) {
  return id.size() == kIdLen && is_lower_hex(id);
}

std::optional<SnapshotRef> parse_snapshot_ref(std::string_view ref) {
  if (!starts_with(ref, kScheme)) return std::nullopt;
  ref.remove_prefix(kScheme.size());
  if (ref.size() < kIdLen + 2 || ref[kIdLen] != '/') return std::nullopt;
  const std::string_view id = ref.substr(0, kIdLen);
  if (!is_snapshot_id(id)) return std::nullopt;
  std::string url = live_address(ref.substr(kIdLen + 1));
  if (url.empty()) return std::nullopt;
  return SnapshotRef{std::string(id), std::move(url)};
}

bool is_snapshot_ref(std::string_view ref) {
  return parse_snapshot_ref(ref).has_value();
}

std::string snapshot_ref_string(std::string_view id, std::string_view url) {
  std::string out(kScheme);
  out += id;
  out += '/';
  out += live_address(url);
  return out;
}

std::string live_address(std::string_view address) {
  // Unwrap repeatedly: a nested ref names the same live page.
  while (starts_with(address, kScheme) &&
         address.size() > kScheme.size() + kIdLen + 1 &&
         address[kScheme.size() + kIdLen] == '/' &&
         is_snapshot_id(address.substr(kScheme.size(), kIdLen)))
    address.remove_prefix(kScheme.size() + kIdLen + 1);
  return std::string(address);
}

std::string snapshot_id_for(std::string_view url, std::string_view doc_hash) {
  std::string key = "xanatoo-snapshot 1";
  key.push_back('\0');
  key += url;
  key.push_back('\0');
  key += doc_hash;
  return sha256_hex(key).substr(0, kIdLen);
}

std::vector<std::string> remote_image_srcs(const Document& doc) {
  std::vector<std::string> out;
  std::unordered_set<std::string> seen;
  auto add = [&](const std::string& src) {
    const std::string live = live_address(src);
    if (keepable_src(live) && seen.insert(live).second) out.push_back(live);
  };
  for (const Node& n : doc.nodes) {
    if (n.kind == NodeKind::Image) add(n.src);
    for (const InlineSpan& sp : n.spans)
      if (sp.kind == InlineKind::Image) add(sp.src);
  }
  return out;
}

void rewrite_snapshot_images(Document& doc, std::string_view id) {
  auto fix = [&](std::string& src) {
    if (keepable_src(live_address(src))) src = snapshot_ref_string(id, src);
  };
  for (Node& n : doc.nodes) {
    if (n.kind == NodeKind::Image) fix(n.src);
    for (InlineSpan& sp : n.spans)
      if (sp.kind == InlineKind::Image) fix(sp.src);
  }
}

// --- Store -----------------------------------------------------------------

std::string snapshot_store(std::string_view url_in, const Document& doc,
                           const std::string* raw,
                           std::string_view content_type,
                           std::string_view service) {
#if XANATOO_SQLITE
  const std::string url = live_address(url_in);
  if (url.empty()) return {};
  const std::string doc_hash = object_put(encode_document(doc));
  if (doc_hash.empty()) return {};
  const std::string raw_hash =
      raw && !raw->empty() ? object_put(*raw) : std::string();
  const std::string id = snapshot_id_for(url, doc_hash);
  // The pictures' bytes first: the URL cache has its own lock.
  std::vector<std::pair<std::string, std::string>> resources;
  for (std::string& src : remote_image_srcs(doc)) {
    std::string hash = cached_body_object(src);
    resources.emplace_back(std::move(src), std::move(hash));
  }
  const int64_t now = now_ms();
  std::lock_guard<std::mutex> lock(g_mutex);
  sqlite3* d = store();
  if (!d) return {};
  sqlite::Tx tx(d);
  {
    sqlite::Stmt s(d,
                   "INSERT INTO snapshots(id, url, doc, raw, content_type, "
                   "service, title, first_seen, last_seen) "
                   "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) "
                   "ON CONFLICT(id) DO UPDATE SET last_seen = excluded.last_seen,"
                   " raw = CASE WHEN snapshots.raw = '' THEN excluded.raw"
                   "        ELSE snapshots.raw END;");
    s.bind(1, id).bind(2, url).bind(3, doc_hash).bind(4, raw_hash)
        .bind(5, content_type).bind(6, service).bind(7, doc.meta.title)
        .bind(8, now).bind(9, now);
    if (!s.run()) return {};
  }
  sqlite::Stmt ins(d,
                   "INSERT INTO snapshot_resources(snapshot, url, object) "
                   "VALUES (?, ?, ?) ON CONFLICT(snapshot, url) DO UPDATE "
                   "SET object = excluded.object "
                   "WHERE snapshot_resources.object = '';");
  for (const auto& [src, hash] : resources) {
    ins.bind(1, id).bind(2, src).bind(3, hash);
    ins.run();
    ins.reset();
  }
  if (!tx.commit()) return {};
  g_generation.fetch_add(1);
  return id;
#else
  (void)url_in, (void)doc, (void)raw, (void)content_type, (void)service;
  return {};
#endif
}

std::optional<SnapshotInfo> snapshot_find(std::string_view id) {
#if XANATOO_SQLITE
  if (!is_snapshot_id(id)) return std::nullopt;
  std::lock_guard<std::mutex> lock(g_mutex);
  sqlite3* d = store();
  if (!d) return std::nullopt;
  const std::string sql =
      std::string("SELECT ") + kInfoCols + " FROM snapshots s WHERE s.id = ?;";
  sqlite::Stmt s(d, sql.c_str(), {id});
  if (!s.row()) return std::nullopt;
  return info_row(s);
#else
  (void)id;
  return std::nullopt;
#endif
}

std::optional<LoadedSnapshot> snapshot_load(std::string_view id) {
  auto info = snapshot_find(id);
  if (!info) return std::nullopt;
  const auto bytes = object_read(info->doc);
  if (!bytes) return std::nullopt;
  auto doc = decode_document(*bytes);
  if (!doc) return std::nullopt;
  LoadedSnapshot out;
  out.info = std::move(*info);
  out.doc = std::move(*doc);
  rewrite_snapshot_images(out.doc, out.info.id);
  object_touch(out.info.doc);
  if (!out.info.raw.empty()) {
    out.raw = object_read(out.info.raw);
    object_touch(out.info.raw);
  }
#if XANATOO_SQLITE
  // Seen again: keep its pictures from being pruned first.
  std::vector<std::string> kept;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (sqlite3* d = store()) {
      sqlite::Stmt s(d,
                     "SELECT object FROM snapshot_resources WHERE snapshot = ?"
                     " AND object != '';",
                     {out.info.id});
      while (s.row()) kept.push_back(s.text(0));
    }
  }
  for (const std::string& h : kept) object_touch(h);
#endif
  return out;
}

std::vector<SnapshotInfo> snapshots_of(std::string_view url, size_t limit) {
  std::vector<SnapshotInfo> out;
#if XANATOO_SQLITE
  std::lock_guard<std::mutex> lock(g_mutex);
  sqlite3* d = store();
  if (!d) return out;
  const std::string sql = std::string("SELECT ") + kInfoCols +
                          " FROM snapshots s WHERE s.url = ?"
                          " ORDER BY s.last_seen DESC LIMIT ?;";
  sqlite::Stmt s(d, sql.c_str());
  s.bind(1, live_address(url)).bind(2, static_cast<int64_t>(limit));
  while (s.row()) out.push_back(info_row(s));
#else
  (void)url, (void)limit;
#endif
  return out;
}

std::string snapshot_image_path(std::string_view src) {
  const auto ref = parse_snapshot_ref(src);
  if (!ref) return url_cache_body_path(src);
  const std::string key(src);
  const uint64_t gen = g_generation.load();
  {
    std::lock_guard<std::mutex> lock(g_memo_mutex);
    if (auto it = g_found.find(key); it != g_found.end()) {
      std::error_code ec;
      if (fs::is_regular_file(it->second, ec)) return it->second;
      g_found.erase(it);  // pruned meanwhile
    } else if (auto m = g_missed.find(key); m != g_missed.end() && m->second == gen) {
      return url_cache_body_path(ref->url);
    }
  }
  std::string hash;
#if XANATOO_SQLITE
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (sqlite3* d = store()) {
      sqlite::Stmt s(d,
                     "SELECT object FROM snapshot_resources WHERE snapshot = ?"
                     " AND url = ?;",
                     {ref->id, ref->url});
      if (s.row()) hash = s.text(0);
    }
  }
#endif
  std::string path = hash.empty() ? std::string() : object_path(hash);
  std::error_code ec;
  std::lock_guard<std::mutex> lock(g_memo_mutex);
  if (!path.empty() && fs::is_regular_file(path, ec)) {
    g_missed.erase(key);
    g_found[key] = path;
    return path;
  }
  g_missed[key] = gen;
  return url_cache_body_path(ref->url);
}

void snapshot_note_body(std::string_view url, std::string_view hash) {
#if XANATOO_SQLITE
  if (!is_object_hash(hash)) return;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    sqlite3* d = store();
    if (!d) return;
    sqlite::Stmt s(d,
                   "UPDATE snapshot_resources SET object = ? WHERE url = ?"
                   " AND object = '' AND snapshot IN (SELECT id FROM snapshots"
                   " WHERE last_seen >= ?);");
    s.bind(1, hash).bind(2, url).bind(3, now_ms() - kLateBodyMs);
    s.run();
    if (sqlite3_changes(d) == 0) return;
  }
  g_generation.fetch_add(1);
#else
  (void)url, (void)hash;
#endif
}

std::string snapshot_latest_copy(std::string_view url) {
#if XANATOO_SQLITE
  std::vector<std::string> hashes;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    sqlite3* d = store();
    if (!d) return {};
    sqlite::Stmt s(d,
                   "SELECT r.object FROM snapshot_resources r JOIN snapshots s"
                   " ON s.id = r.snapshot WHERE r.url = ? AND r.object != ''"
                   " ORDER BY s.last_seen DESC LIMIT 8;",
                   {url});
    while (s.row()) hashes.push_back(s.text(0));
  }
  for (const std::string& h : hashes)
    if (object_exists(h)) return h;
#else
  (void)url;
#endif
  return {};
}

bool snapshot_restore_body(std::string_view url_in, const std::string& path) {
  if (path.empty()) return false;
  const std::string url = live_address(url_in);
  std::string hash;
  if (auto e = url_cache_find(url))
    if (!e->body.empty() && object_exists(e->body)) hash = e->body;
  if (hash.empty()) hash = snapshot_latest_copy(url);
  if (hash.empty() || !object_link_to(hash, path)) return false;
  std::error_code ec;
  return fs::is_regular_file(path, ec);
}

void snapshot_prune_missing() {
#if XANATOO_SQLITE
  std::vector<std::string> gone;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    sqlite3* d = store();
    if (!d) return;
    sqlite::Stmt s(d, "SELECT id, doc FROM snapshots;");
    while (s.row())
      if (!object_exists(s.text(1))) gone.push_back(s.text(0));
    if (gone.empty()) return;
    sqlite::Tx tx(d);
    sqlite::Stmt del(d, "DELETE FROM snapshots WHERE id = ?;");
    sqlite::Stmt del_res(d, "DELETE FROM snapshot_resources WHERE snapshot = ?;");
    for (const std::string& id : gone) {
      del_res.bind(1, id);
      del_res.run();
      del_res.reset();
      del.bind(1, id);
      del.run();
      del.reset();
    }
    tx.commit();
  }
  g_generation.fetch_add(1);
#endif
}

void snapshot_set_pins(std::string_view owner,
                       const std::vector<std::string>& ids) {
#if XANATOO_SQLITE
  std::lock_guard<std::mutex> lock(g_mutex);
  sqlite3* d = store();
  if (!d) return;
  sqlite::Tx tx(d);
  if (!tx.ok()) return;
  {
    sqlite::Stmt del(d, "DELETE FROM snapshot_pins WHERE owner = ?;", {owner});
    if (!del.run()) return;
  }
  sqlite::Stmt put(d, "INSERT OR IGNORE INTO snapshot_pins(owner, snapshot) "
                      "VALUES(?, ?);");
  for (const std::string& id : ids) {
    if (!is_snapshot_id(id)) continue;
    put.bind(1, owner).bind(2, id);
    if (!put.run()) return;
    put.reset();
  }
  tx.commit();
#else
  (void)owner;
  (void)ids;
#endif
}

std::vector<std::string> snapshot_pins(std::string_view owner) {
  std::vector<std::string> out;
#if XANATOO_SQLITE
  std::lock_guard<std::mutex> lock(g_mutex);
  sqlite3* d = store();
  if (!d) return out;
  sqlite::Stmt s(d, "SELECT snapshot FROM snapshot_pins WHERE owner = ? "
                    "ORDER BY snapshot;", {owner});
  while (s.row()) out.push_back(s.text(0));
#else
  (void)owner;
#endif
  return out;
}

std::unordered_set<std::string> snapshot_pinned_objects() {
  std::unordered_set<std::string> out;
#if XANATOO_SQLITE
  std::lock_guard<std::mutex> lock(g_mutex);
  sqlite3* d = store();
  if (!d) return out;
  sqlite::Stmt s(d, "SELECT doc, raw FROM snapshots WHERE id IN "
                    "(SELECT snapshot FROM snapshot_pins);");
  while (s.row())
    for (int c = 0; c < 2; ++c)
      if (std::string h = s.text(c); is_object_hash(h)) out.insert(std::move(h));
  sqlite::Stmt r(d, "SELECT object FROM snapshot_resources WHERE snapshot IN "
                    "(SELECT snapshot FROM snapshot_pins);");
  while (r.row())
    if (std::string h = r.text(0); is_object_hash(h)) out.insert(std::move(h));
#endif
  return out;
}

void snapshot_close() {
#if XANATOO_SQLITE
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_db = sqlite::Db();
    g_db_path.clear();
  }
#endif
  std::lock_guard<std::mutex> memo(g_memo_mutex);
  g_found.clear();
  g_missed.clear();
}

}  // namespace xanatoo
