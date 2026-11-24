// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/url_cache.hpp"
#include "xanatoo/object_store.hpp"
#include "xanatoo/paths.hpp"
#include "xanatoo/snapshot.hpp"
#include "xanatoo/util.hpp"

#ifndef XANATOO_SQLITE
#define XANATOO_SQLITE 1
#endif
#if XANATOO_SQLITE
#include "xanatoo/sqlite_db.hpp"
#endif

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>

namespace fs = std::filesystem;

namespace xanatoo {
namespace {

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool write_body(const std::string& path, std::string_view bytes) {
  std::error_code ec;
  fs::create_directories(fs::path(path).parent_path(), ec);
  const std::string tmp = path + ".part";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
      std::cerr << "url cache: cannot write " << tmp << "\n";
      fs::remove(tmp, ec);
      return false;
    }
  }
  fs::rename(tmp, path, ec);
  if (ec) fs::remove(tmp, ec);
  return !ec;
}

#if XANATOO_SQLITE
const sqlite::Migrations kSteps = {
    "CREATE TABLE IF NOT EXISTS responses ("
    "  url TEXT PRIMARY KEY, final_url TEXT NOT NULL DEFAULT '',"
    "  status INTEGER NOT NULL DEFAULT 0,"
    "  content_type TEXT NOT NULL DEFAULT '',"
    "  etag TEXT NOT NULL DEFAULT '', last_modified TEXT NOT NULL DEFAULT '',"
    "  fetched INTEGER NOT NULL, size INTEGER NOT NULL DEFAULT -1);"
    "CREATE INDEX IF NOT EXISTS responses_fetched ON responses(fetched);",
    // 2: the body's object hash (snapshots keep pictures by it).
    "ALTER TABLE responses ADD COLUMN body TEXT NOT NULL DEFAULT '';",
};

std::mutex g_mutex;
sqlite::Db g_db;
std::string g_db_path;  // the file g_db has open ("" = none)

// The open store for the current cache_dir(), reopened when that changes
// (tests, --cache-dir). Caller holds g_mutex. nullptr: no cache directory.
sqlite3* store() {
  const std::string dir = cache_dir();
  if (dir.empty()) return nullptr;
  const std::string path = (fs::path(dir) / "urls.sqlite").string();
  if (g_db && g_db_path == path) return g_db;
  g_db = sqlite::Db();
  g_db_path = path;
  if (!ensure_dir(dir)) return nullptr;
  g_db = sqlite::Db(sqlite::open_store(path, kSteps));
  return g_db;
}

void put_row(sqlite3* d, const UrlCacheEntry& e) {
  sqlite::Stmt s(d,
                 "INSERT OR REPLACE INTO responses(url, final_url, status, "
                 "content_type, etag, last_modified, fetched, size, body) "
                 "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);");
  s.bind(1, e.url).bind(2, e.final_url).bind(3, e.status)
      .bind(4, e.content_type).bind(5, e.etag).bind(6, e.last_modified)
      .bind(7, e.fetched).bind(8, e.size).bind(9, e.body);
  s.run();
}
#endif

}  // namespace

std::string url_cache_body_path(std::string_view url) {
  // FNV-1a: stable across builds, unlike std::hash.
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : url) {
    h ^= c;
    h *= 1099511628211ull;
  }
  char name[32];
  std::snprintf(name, sizeof(name), "%016llx",
                static_cast<unsigned long long>(h));
  const std::string cache = cache_dir();
  if (cache.empty()) return {};
  return (fs::path(cache) / "urls" / name).string();
}

bool url_cache_put(UrlCacheEntry e, const std::string_view* body) {
  if (e.url.empty()) return false;
  if (e.final_url.empty()) e.final_url = e.url;
  if (e.fetched == 0) e.fetched = now_ms();
  bool ok = true;
  if (body) {
    e.size = static_cast<int64_t>(body->size());
    const std::string hash = object_hash(*body);
    for (const std::string& key : {e.url, e.final_url}) {
      const std::string path = url_cache_body_path(key);
      if (path.empty() || !write_body(path, *body)) ok = false;
      else if (e.body.empty() && object_put_file(path, hash)) e.body = hash;
      if (e.final_url == e.url) break;
    }
  }
#if XANATOO_SQLITE
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    sqlite3* d = store();
    if (!d) return false;
    sqlite::Tx tx(d);
    put_row(d, e);
    if (e.final_url != e.url) {
      UrlCacheEntry target = e;
      target.url = e.final_url;
      put_row(d, target);
    }
    if (!tx.commit()) return false;
  }
  // Outside g_mutex: the snapshot store takes its own lock and reads ours.
  if (!e.body.empty()) {
    snapshot_note_body(e.url, e.body);
    if (e.final_url != e.url) snapshot_note_body(e.final_url, e.body);
  }
  return ok;
#else
  return ok;
#endif
}

std::optional<UrlCacheEntry> url_cache_find(std::string_view url) {
#if XANATOO_SQLITE
  std::lock_guard<std::mutex> lock(g_mutex);
  sqlite3* d = store();
  if (!d) return std::nullopt;
  sqlite::Stmt s(d,
                 "SELECT url, final_url, status, content_type, etag, "
                 "last_modified, fetched, size, body FROM responses WHERE url = ?;",
                 {url});
  if (!s.row()) return std::nullopt;
  UrlCacheEntry e;
  e.url = s.text(0);
  e.final_url = s.text(1);
  e.status = static_cast<int>(s.int64(2));
  e.content_type = s.text(3);
  e.etag = s.text(4);
  e.last_modified = s.text(5);
  e.fetched = s.int64(6);
  e.size = s.int64(7);
  e.body = s.text(8);
  return e;
#else
  (void)url;
  return std::nullopt;
#endif
}

UrlKind url_cache_kind(std::string_view url) {
  const auto e = url_cache_find(url);
  if (!e || e->content_type.empty()) return UrlKind::Unknown;
  // An error page (404 …) says nothing about what the address serves.
  if (e->status != 0 && e->status != 200) return UrlKind::Unknown;
  return content_type_is_image(e->content_type) ? UrlKind::Image
                                                : UrlKind::Other;
}

bool content_type_is_image(std::string_view content_type) {
  std::string type = ascii_lower(content_type.substr(0, content_type.find(';')));
  std::string_view t = type;
  trim_ascii(t);
  for (std::string_view known :
       {"image/png", "image/jpeg", "image/jpg", "image/pjpeg", "image/gif",
        "image/webp", "image/bmp", "image/x-ms-bmp", "image/tiff",
        "image/avif", "image/jxl", "image/svg+xml"})
    if (t == known) return true;
  return false;
}

void url_cache_prune_rows(int64_t max_age_ms) {
#if XANATOO_SQLITE
  std::lock_guard<std::mutex> lock(g_mutex);
  sqlite3* d = store();
  if (!d) return;
  sqlite::Stmt s(d, "DELETE FROM responses WHERE fetched < ?;");
  s.bind(1, now_ms() - max_age_ms);
  s.run();
#else
  (void)max_age_ms;
#endif
}

void url_cache_close() {
#if XANATOO_SQLITE
  std::lock_guard<std::mutex> lock(g_mutex);
  g_db = sqlite::Db();
  g_db_path.clear();
#endif
}

}  // namespace xanatoo
