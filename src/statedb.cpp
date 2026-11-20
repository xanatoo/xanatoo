// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/statedb.hpp"
#include "xanatoo/minibuffer.hpp"
#include "xanatoo/paths.hpp"

#ifndef XANATOO_SQLITE
#define XANATOO_SQLITE 1
#endif
#if XANATOO_SQLITE
#include "xanatoo/sqlite_db.hpp"
#endif

#include <chrono>
#include <filesystem>
#include <fstream>

namespace xanatoo {
namespace {

std::string g_db_path;    // empty = state_dir()/xanatoo.sqlite
std::string g_text_dir;   // empty = state_dir()

std::string text_dir() { return g_text_dir.empty() ? state_dir() : g_text_dir; }

std::string bookmarks_file() {
  const std::string dir = text_dir();
  return dir.empty() ? std::string()
                     : (std::filesystem::path(dir) / "bookmarks").string();
}

std::string ring_file(std::string_view name) {
  const std::string dir = text_dir();
  return dir.empty() ? std::string()
                     : dir + "/minibuf-" + std::string(name) + ".history";
}

// Folder lines: "#folder<TAB>path" (comments to older readers).
constexpr std::string_view kFolderLine = "#folder\t";

std::vector<Bookmark> read_bookmarks_file(std::vector<std::string>* folders = nullptr) {
  std::vector<Bookmark> out;
  const std::string path = bookmarks_file();
  if (path.empty()) return out;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (starts_with(line, kFolderLine)) {
      if (folders) folders->push_back(line.substr(kFolderLine.size()));
    } else if (auto b = parse_bookmark_line(line)) {
      out.push_back(std::move(*b));
    }
  }
  if (folders) *folders = bookmark_folder_list(out, *folders);
  return out;
}

bool write_bookmarks_file(const std::vector<Bookmark>& entries,
                          const std::vector<std::string>& folders) {
  if (no_save()) return true;
  const std::string path = bookmarks_file();
  if (path.empty() || !ensure_dir(text_dir())) return false;
  std::string text =
      "# xanatoo bookmarks — one address per line, then optional\n"
      "# tab-separated title= / quote= / at= / folder= fields;\n"
      "# #folder<TAB>path lines list the folders\n"
      "# (an export: xanatoo keeps them in xanatoo.sqlite)\n";
  for (const std::string& f : bookmark_folder_list(entries, folders))
    text += std::string(kFolderLine) + f + '\n';
  for (const auto& e : entries)
    if (!e.address.empty()) text += format_bookmark_line(e) + '\n';
  return write_file_atomic(path, text);
}

// The folder list of the text file (the store when there is no database).
std::vector<std::string> read_folders_or_empty() {
  std::vector<std::string> folders;
  read_bookmarks_file(&folders);
  return folders;
}

std::vector<std::string> read_ring_file(std::string_view name) {
  std::vector<std::string> out;
  const std::string path = ring_file(name);
  if (path.empty()) return out;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line))
    if (!line.empty() && line[0] != '#') out.push_back(line);
  return out;
}

void write_ring_file(std::string_view name, const std::vector<std::string>& hist,
                     size_t max_entries) {
  if (no_save()) return;
  const std::string path = ring_file(name);
  if (path.empty() || !ensure_dir(text_dir())) return;
  const size_t start = hist.size() > max_entries ? hist.size() - max_entries : 0;
  std::string text;
  for (size_t i = start; i < hist.size(); ++i) text += hist[i] + '\n';
  write_file_atomic(path, text);
}

#if XANATOO_SQLITE
sqlite3* g_db = nullptr;
bool g_db_failed = false;
int64_t g_data_version = -1;

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

const sqlite::Migrations kSteps = {
    "CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
    // Insertion order is rowid order.
    "CREATE TABLE bookmarks ("
    "  id INTEGER PRIMARY KEY, address TEXT NOT NULL UNIQUE,"
    "  title TEXT NOT NULL DEFAULT '', quote TEXT NOT NULL DEFAULT '',"
    "  created TEXT NOT NULL DEFAULT '', time INTEGER NOT NULL);"
    // Recency is rowid order: recording an entry again replaces its row.
    "CREATE TABLE input_history ("
    "  ring TEXT NOT NULL, entry TEXT NOT NULL, time INTEGER NOT NULL,"
    "  PRIMARY KEY (ring, entry));",
    // Browsing history (visits.hpp, docs/HISTORY.md B / D). Visits and
    // observations are insert-only (leave_time aside).
    "CREATE TABLE pages ("
    "  id INTEGER PRIMARY KEY, address TEXT NOT NULL UNIQUE,"
    "  title TEXT NOT NULL DEFAULT '', first_seen INTEGER NOT NULL,"
    "  last_seen INTEGER NOT NULL, visit_count INTEGER NOT NULL DEFAULT 0);"
    "CREATE TABLE observations ("
    "  id INTEGER PRIMARY KEY, url TEXT NOT NULL, time INTEGER NOT NULL,"
    "  status INTEGER NOT NULL DEFAULT 0, content_type TEXT NOT NULL DEFAULT '',"
    "  etag TEXT NOT NULL DEFAULT '', last_modified TEXT NOT NULL DEFAULT '',"
    "  raw_hash TEXT NOT NULL DEFAULT '', text_hash TEXT NOT NULL DEFAULT '',"
    "  size INTEGER NOT NULL DEFAULT -1,"
    "  snapshot TEXT, archive_url TEXT);"
    "CREATE INDEX observations_url ON observations(url, time);"
    "CREATE TABLE visits ("
    "  id INTEGER PRIMARY KEY,"
    "  page INTEGER NOT NULL REFERENCES pages(id),"
    "  address TEXT NOT NULL, time INTEGER NOT NULL,"
    "  leave_time INTEGER,"
    "  source INTEGER REFERENCES visits(id) ON DELETE SET NULL,"
    "  kind TEXT NOT NULL, href TEXT NOT NULL DEFAULT '',"
    "  source_node TEXT NOT NULL DEFAULT '',"
    "  observation INTEGER REFERENCES observations(id) ON DELETE SET NULL,"
    "  window TEXT NOT NULL DEFAULT '', title TEXT NOT NULL DEFAULT '');"
    "CREATE INDEX visits_time ON visits(time);"
    "CREATE INDEX visits_page ON visits(page);"
    "CREATE INDEX visits_source ON visits(source);"
    "CREATE INDEX visits_address ON visits(address, time);",
    // Bookmark folders: a bookmark's folder path, and the folder list (empty
    // folders exist too).
    "ALTER TABLE bookmarks ADD COLUMN folder TEXT NOT NULL DEFAULT '';"
    "CREATE TABLE bookmark_folders (path TEXT PRIMARY KEY);",
};

bool imported(sqlite3* d, const std::string& what) {
  sqlite::Stmt s(d, "SELECT 1 FROM meta WHERE key = ?;", {"import." + what});
  return s.row();
}

bool mark_imported(sqlite3* d, const std::string& what) {
  sqlite::Stmt s(d, "INSERT OR REPLACE INTO meta(key, value) VALUES(?, ?);",
                 {"import." + what, std::to_string(now_ms())});
  return s.run();
}

// One-time import of the old text files; the marker makes it happen once
// even with several processes starting at the same moment.
void import_text_files(sqlite3* d) {
  sqlite::Tx tx(d);
  if (!tx.ok()) return;
  if (!imported(d, "bookmarks")) {
    const int64_t t = now_ms();
    std::vector<std::string> folders;
    for (const Bookmark& b : read_bookmarks_file(&folders)) {
      sqlite::Stmt s(d, "INSERT OR IGNORE INTO bookmarks"
                        "(address, title, quote, created, time, folder) "
                        "VALUES(?,?,?,?,?,?);");
      s.bind(1, b.address).bind(2, b.title).bind(3, b.quote)
          .bind(4, b.created).bind(5, t).bind(6, b.folder).run();
    }
    for (const std::string& f : folders) {
      sqlite::Stmt s(d, "INSERT OR IGNORE INTO bookmark_folders(path) VALUES(?);",
                     {f});
      s.run();
    }
    mark_imported(d, "bookmarks");
  }
  for (const char* ring : {"url", "path"}) {
    const std::string what = std::string("minibuf-") + ring;
    if (imported(d, what)) continue;
    const int64_t t = now_ms();
    for (const std::string& e : read_ring_file(ring)) {
      sqlite::Stmt s(d, "INSERT OR REPLACE INTO input_history(ring, entry, time) "
                        "VALUES(?,?,?);");
      s.bind(1, ring).bind(2, e).bind(3, t).run();
    }
    mark_imported(d, what);
  }
  tx.commit();
}

}  // namespace

sqlite3* statedb_connection() {
  if (g_db) return g_db;
  if (g_db_failed) return nullptr;
  std::string path = g_db_path;
  sqlite::OpenOptions opt;
  if (path.empty()) {
    const std::string dir = state_dir();
    if (!dir.empty()) path = dir + "/xanatoo.sqlite";
    if (!path.empty() && !no_save() && !ensure_dir(dir)) path.clear();
  }
  // --no-save reads an existing database but never creates or changes it.
  if (no_save()) opt.mode = sqlite::OpenMode::ReadOnly;
  g_db = path.empty() ? nullptr : sqlite::open_store(path, kSteps, opt);
  if (!g_db) {
    g_db_failed = true;
    return nullptr;
  }
  if (!no_save()) import_text_files(g_db);
  g_data_version = sqlite::data_version(g_db);
  return g_db;
}

namespace {
sqlite3* db() { return statedb_connection(); }
#endif

}  // namespace

void statedb_use_path(std::string db_path, std::string dir) {
  g_db_path = std::move(db_path);
  g_text_dir = std::move(dir);
#if XANATOO_SQLITE
  if (g_db) sqlite3_close(g_db);
  g_db = nullptr;
  g_db_failed = false;
  g_data_version = -1;
#endif
}

bool statedb_changed_elsewhere() {
#if XANATOO_SQLITE
  sqlite3* d = db();
  if (!d) return false;
  const int64_t v = sqlite::data_version(d);
  if (v == g_data_version) return false;
  g_data_version = v;
  return true;
#else
  return false;
#endif
}

std::vector<Bookmark> stored_bookmarks() {
#if XANATOO_SQLITE
  if (sqlite3* d = db()) {
    std::vector<Bookmark> out;
    sqlite::Stmt s(d, "SELECT address, title, quote, created, folder "
                      "FROM bookmarks ORDER BY id;");
    while (s.row()) {
      Bookmark b;
      b.address = s.text(0);
      b.title = s.text(1);
      b.quote = s.text(2);
      b.created = s.text(3);
      b.folder = s.text(4);
      out.push_back(std::move(b));
    }
    return out;
  }
#endif
  return read_bookmarks_file();
}

bool store_bookmark_add(const Bookmark& b, const std::vector<Bookmark>& all) {
  if (no_save()) return true;
#if XANATOO_SQLITE
  if (sqlite3* d = db()) {
    sqlite::Tx tx(d);
    if (!tx.ok()) return false;
    sqlite::Stmt s(d, "INSERT OR IGNORE INTO bookmarks"
                      "(address, title, quote, created, time, folder) "
                      "VALUES(?,?,?,?,?,?);");
    if (!s.bind(1, b.address).bind(2, b.title).bind(3, b.quote)
             .bind(4, b.created).bind(5, now_ms()).bind(6, b.folder).run())
      return false;
    for (std::string f = b.folder; !f.empty(); f = bookmark_folder_parent(f)) {
      sqlite::Stmt fst(d, "INSERT OR IGNORE INTO bookmark_folders(path) VALUES(?);",
                      {f});
      if (!fst.run()) return false;
    }
    if (!tx.commit()) return false;
    write_bookmarks_file(stored_bookmarks(), stored_bookmark_folders());  // export
    return true;
  }
#endif
  (void)b;
  return write_bookmarks_file(all, bookmark_folder_list(all, read_folders_or_empty()));
}

bool store_bookmark_remove(std::string_view address,
                           const std::vector<Bookmark>& all) {
  if (no_save()) return true;
#if XANATOO_SQLITE
  if (sqlite3* d = db()) {
    sqlite::Stmt s(d, "DELETE FROM bookmarks WHERE address = ?;", {address});
    if (!s.run()) return false;
    write_bookmarks_file(stored_bookmarks(), stored_bookmark_folders());
    return true;
  }
#endif
  (void)address;
  return write_bookmarks_file(all, read_folders_or_empty());
}

std::vector<std::string> stored_bookmark_folders() {
#if XANATOO_SQLITE
  if (sqlite3* d = db()) {
    std::vector<std::string> out;
    sqlite::Stmt s(d, "SELECT path FROM bookmark_folders;");
    while (s.row()) out.push_back(s.text(0));
    return bookmark_folder_list(stored_bookmarks(), out);
  }
#endif
  return read_folders_or_empty();
}

bool store_bookmark_changes(const std::vector<Bookmark>& changed,
                            const std::vector<std::string>& folders,
                            const std::vector<Bookmark>& all) {
  if (no_save()) return true;
#if XANATOO_SQLITE
  if (sqlite3* d = db()) {
    sqlite::Tx tx(d);
    if (!tx.ok()) return false;
    for (const Bookmark& b : changed) {
      sqlite::Stmt s(d, "UPDATE bookmarks SET title = ?, folder = ? "
                        "WHERE address = ?;");
      if (!s.bind(1, b.title).bind(2, b.folder).bind(3, b.address).run())
        return false;
    }
    {
      sqlite::Stmt s(d, "DELETE FROM bookmark_folders;");
      if (!s.run()) return false;
    }
    for (const std::string& f : bookmark_folder_list(all, folders)) {
      sqlite::Stmt s(d, "INSERT OR IGNORE INTO bookmark_folders(path) VALUES(?);",
                     {f});
      if (!s.run()) return false;
    }
    if (!tx.commit()) return false;
    write_bookmarks_file(stored_bookmarks(), stored_bookmark_folders());
    return true;
  }
#endif
  (void)changed;
  return write_bookmarks_file(all, folders);
}

std::vector<std::string> stored_input_history(std::string_view name,
                                              size_t max_entries) {
#if XANATOO_SQLITE
  if (sqlite3* d = db()) {
    std::vector<std::string> out;
    sqlite::Stmt s(d, "SELECT entry FROM (SELECT entry, rowid AS r "
                      "FROM input_history WHERE ring = ? "
                      "ORDER BY rowid DESC LIMIT ?) ORDER BY r;");
    s.bind(1, name).bind(2, static_cast<int64_t>(max_entries));
    while (s.row()) out.push_back(s.text(0));
    return out;
  }
#endif
  std::vector<std::string> out = read_ring_file(name);
  if (out.size() > max_entries)
    out.erase(out.begin(), out.end() - static_cast<std::ptrdiff_t>(max_entries));
  return out;
}

void record_input_history(std::string_view name, std::vector<std::string>& hist,
                          std::string_view entry, size_t max_entries) {
  if (entry.empty()) return;
  push_minibuffer_history(hist, entry, max_entries);
  if (no_save()) return;
#if XANATOO_SQLITE
  if (sqlite3* d = db()) {
    sqlite::Tx tx(d);
    sqlite::Stmt put(d, "INSERT OR REPLACE INTO input_history(ring, entry, time) "
                        "VALUES(?,?,?);");
    put.bind(1, name).bind(2, entry).bind(3, now_ms()).run();
    sqlite::Stmt trim(d, "DELETE FROM input_history WHERE ring = ?1 AND rowid "
                         "NOT IN (SELECT rowid FROM input_history WHERE "
                         "ring = ?1 ORDER BY rowid DESC LIMIT ?2);");
    trim.bind(1, name).bind(2, static_cast<int64_t>(max_entries)).run();
    tx.commit();
    return;
  }
#endif
  write_ring_file(name, hist, max_entries);
}

}  // namespace xanatoo
