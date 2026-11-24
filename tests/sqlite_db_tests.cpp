// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
//
// Tests for the shared SQLite layer (sqlite_db.hpp): migrations, WAL,
// several connections writing at once, data_version change detection.
// Part of xanatoo_tests (test_sqlite_db is called from unit_tests.cpp).

#ifndef XANATOO_SQLITE
#define XANATOO_SQLITE 1
#endif

#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <fstream>

#include "xanatoo/paths.hpp"
#include "xanatoo/parse.hpp"
#include "xanatoo/statedb.hpp"
#include "xanatoo/visits.hpp"
#if XANATOO_SQLITE
#include "xanatoo/sqlite_db.hpp"
#endif

namespace {
int g_passed = 0;
int g_failed = 0;
void expect(bool cond, const char* expr, const char* file, int line) {
  if (cond) {
    ++g_passed;
    return;
  }
  ++g_failed;
  std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
}
}  // namespace

#define EXPECT(c) expect(!!(c), #c, __FILE__, __LINE__)
#define EXPECT_EQ(a, b) expect((a) == (b), #a " == " #b, __FILE__, __LINE__)

#if XANATOO_SQLITE
namespace {

namespace fs = std::filesystem;
using namespace xanatoo::sqlite;

struct TempDb {
  fs::path path;
  explicit TempDb(const char* name) {
    path = fs::temp_directory_path() /
           (std::string("xanatoo-sqlite-") + name + "-" +
            std::to_string(std::hash<std::thread::id>{}(
                std::this_thread::get_id()) % 100000) + ".sqlite");
    remove();
  }
  ~TempDb() { remove(); }
  void remove() {
    std::error_code ec;
    for (const char* ext : {"", "-wal", "-shm", "-journal"})
      fs::remove(path.string() + ext, ec);
  }
  std::string str() const { return path.string(); }
};

int64_t count(sqlite3* db, const char* table) {
  Stmt s(db, (std::string("SELECT COUNT(*) FROM ") + table + ";").c_str());
  return s.row() ? s.int64(0) : -1;
}

void test_migrations() {
  TempDb t("migrate");
  // A database from before versioning: the table exists, user_version 0.
  {
    Db db(open(t.str()));
    EXPECT(db);
    EXPECT(exec(db, "CREATE TABLE a (x TEXT);"
                    "INSERT INTO a VALUES('old');"));
    EXPECT_EQ(user_version(db), 0);
  }
  const Migrations steps = {
      "CREATE TABLE IF NOT EXISTS a (x TEXT);",
      "ALTER TABLE a ADD COLUMN y INTEGER NOT NULL DEFAULT 7;",
  };
  {
    Db db(open_store(t.str(), steps));
    EXPECT(db);
    EXPECT_EQ(user_version(db), 2);
    Stmt s(db, "SELECT x, y FROM a;");
    EXPECT(s.row());
    EXPECT_EQ(s.text(0), "old");
    EXPECT_EQ(s.int64(1), 7);
  }
  // Reopening is a no-op (the ALTER would fail if it ran twice).
  {
    Db db(open_store(t.str(), steps));
    EXPECT(db);
    EXPECT_EQ(user_version(db), 2);
    Stmt journal(db, "PRAGMA journal_mode;");
    EXPECT(journal.row() && journal.text(0) == "wal");
  }
  // A failing step rolls back everything, user_version included.
  {
    Migrations bad = steps;
    bad.push_back("CREATE TABLE b (z);");
    bad.push_back("THIS IS NOT SQL;");
    Db db(open(t.str()));
    EXPECT(migrate(db, bad) == MigrateResult::Failed);
    EXPECT_EQ(user_version(db), 2);
    Stmt b(db, "SELECT name FROM sqlite_master WHERE name = 'b';");
    EXPECT(!b.row());
  }
  // Written by a newer xanatoo: opened read-only, nothing changed.
  {
    Db db(open(t.str()));
    EXPECT(exec(db, "PRAGMA user_version=9;"));
  }
  {
    std::fprintf(stderr, "(expected warning follows)\n");
    Db db(open_store(t.str(), steps));
    EXPECT(db);
    EXPECT_EQ(user_version(db), 9);
    EXPECT(!exec(db, "INSERT INTO a(x) VALUES('new');"));
    EXPECT_EQ(count(db, "a"), 1);
  }
  // Existing-only modes do not create files.
  TempDb missing("missing");
  OpenOptions rw;
  rw.mode = OpenMode::ReadWrite;
  EXPECT(!open(missing.str(), rw));
  EXPECT(!fs::exists(missing.path));
}

void test_statements() {
  TempDb t("stmt");
  Db db(open_store(t.str(), {"CREATE TABLE r (k TEXT, v, b BLOB);"}));
  EXPECT(db);
  {
    Stmt s(db, "INSERT INTO r VALUES(?,?,?);");
    EXPECT(s.bind(1, std::string_view()).bind_opt(2, std::optional<int64_t>())
               .bind_blob(3, std::string("a\0b", 3)).run());
    s.reset();
    EXPECT(s.bind(1, "two").bind_opt(2, std::optional<std::string>("x"))
               .bind_blob(3, "").run());
  }
  Stmt q(db, "SELECT k, v, b, k IS NULL FROM r ORDER BY rowid;");
  EXPECT(q.row());
  EXPECT_EQ(q.int64(3), 0);  // empty string, not NULL
  EXPECT(q.is_null(1));
  EXPECT_EQ(q.blob(2), std::string("a\0b", 3));
  EXPECT(q.row());
  EXPECT_EQ(q.text(0), "two");
  EXPECT_EQ(q.text(1), "x");
  EXPECT(!q.row());
  EXPECT(!Stmt(db, "SELECT nonsense FROM nowhere;").valid());
}

void test_connections() {
  TempDb t("conns");
  const Migrations steps = {"CREATE TABLE IF NOT EXISTS n (v INTEGER);"};
  Db a(open_store(t.str(), steps));
  Db b(open_store(t.str(), steps));
  EXPECT(a && b);

  // data_version moves only for commits by *other* connections.
  const int64_t b0 = data_version(b);
  EXPECT(exec(b, "INSERT INTO n VALUES(1);"));
  EXPECT_EQ(data_version(b), b0);
  const int64_t a0 = data_version(a);
  EXPECT(exec(b, "INSERT INTO n VALUES(2);"));
  EXPECT(data_version(a) != a0);
  EXPECT_EQ(count(a, "n"), 2);

  // A reader sees the last commit while a writer holds its transaction.
  {
    Tx tx(a);
    EXPECT(tx.ok());
    EXPECT(exec(a, "INSERT INTO n VALUES(3);"));
    EXPECT_EQ(count(b, "n"), 2);
    // A second writer waits for the busy timeout, then gives up.
    OpenOptions quick;
    quick.busy_ms = 50;
    Db c(open(t.str(), quick));
    Tx tc(c);
    EXPECT(!tc.ok());
    EXPECT(tx.commit());
  }
  EXPECT_EQ(count(b, "n"), 3);
  // Uncommitted transactions roll back.
  {
    Tx tx(a);
    exec(a, "INSERT INTO n VALUES(4);");
  }
  EXPECT_EQ(count(b, "n"), 3);
}

void test_concurrent_writers() {
  TempDb t("writers");
  const Migrations steps = {"CREATE TABLE IF NOT EXISTS w (t INTEGER, i INTEGER);"};
  constexpr int kThreads = 4;
  constexpr int kRows = 50;
  std::vector<int> ok(kThreads, 0);
  std::vector<std::thread> threads;
  // Every thread opens (and migrates) on its own, like separate processes.
  for (int ti = 0; ti < kThreads; ++ti) {
    threads.emplace_back([&, ti] {
      Db db(open_store(t.str(), steps));
      if (!db) return;
      for (int i = 0; i < kRows; ++i) {
        Tx tx(db);
        Stmt s(db, "INSERT INTO w VALUES(?,?);");
        if (tx.ok() && s.bind(1, ti).bind(2, i).run() && tx.commit()) ++ok[ti];
      }
    });
  }
  for (auto& th : threads) th.join();
  for (int ti = 0; ti < kThreads; ++ti) EXPECT_EQ(ok[ti], kRows);
  Db db(open(t.str()));
  EXPECT_EQ(count(db, "w"), kThreads * kRows);
  EXPECT_EQ(user_version(db), 1);
}

std::string slurp(const fs::path& p) {
  std::ifstream in(p);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

void test_statedb() {
  using xanatoo::Bookmark;
  TempDb t("state");
  const fs::path dir = t.path.string() + ".d";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  // Old text files, imported once on first open.
  {
    std::ofstream(dir / "bookmarks")
        << "# comment\nhttps://a.example/\ttitle=A\n"
           "https://b.example/x//p:2\ttitle=B\tquote=Bee\tat=2026-01-02\n";
    std::ofstream(dir / "minibuf-url.history") << "https://one/\nhttps://two/\n";
  }
  xanatoo::statedb_use_path(t.str(), dir.string());
  auto marks = xanatoo::stored_bookmarks();
  EXPECT_EQ(marks.size(), 2u);
  if (marks.size() == 2) {
    EXPECT_EQ(marks[0].title, "A");
    EXPECT_EQ(marks[1].quote, "Bee");
    EXPECT_EQ(marks[1].created, "2026-01-02");
  }
  auto url = xanatoo::stored_input_history("url");
  EXPECT(url == (std::vector<std::string>{"https://one/", "https://two/"}));
  EXPECT(xanatoo::stored_input_history("path").empty());

  // Single-row changes; the bookmarks file is rewritten as an export.
  Bookmark c{"https://c.example/", "C", "", "2026-11-18"};
  c.snapshot = std::string(32, 'c');  // the page as bookmarked
  marks.push_back(c);
  EXPECT(xanatoo::store_bookmark_add(c, marks));
  EXPECT(xanatoo::store_bookmark_remove("https://a.example/", marks));
  marks = xanatoo::stored_bookmarks();
  EXPECT_EQ(marks.size(), 2u);
  if (marks.size() == 2) {
    EXPECT_EQ(marks[0].title, "B");
    EXPECT_EQ(marks[1].title, "C");
    EXPECT_EQ(marks[1].snapshot, c.snapshot);
    EXPECT(marks[0].snapshot.empty());
  }
  const std::string exported = slurp(dir / "bookmarks");
  EXPECT(exported.find("https://c.example/\ttitle=C") != std::string::npos);
  EXPECT(exported.find("a.example") == std::string::npos);

  // Folders: a bookmark's folder, empty folders, one-transaction changes.
  marks = xanatoo::stored_bookmarks();
  Bookmark e{"https://e.example/", "E", "", "", "News/Tech"};
  marks.push_back(e);
  EXPECT(xanatoo::store_bookmark_add(e, marks));
  EXPECT(xanatoo::stored_bookmark_folders() ==
         (std::vector<std::string>{"News", "News/Tech"}));
  std::vector<std::string> folders = {"News", "News/Tech", "Empty"};
  marks = xanatoo::stored_bookmarks();
  marks[0].folder = "Empty";
  marks[0].title = "B renamed";
  EXPECT(xanatoo::store_bookmark_changes({marks[0]}, folders, marks));
  marks = xanatoo::stored_bookmarks();
  EXPECT_EQ(marks[0].folder, "Empty");
  EXPECT_EQ(marks[0].title, "B renamed");
  EXPECT(xanatoo::stored_bookmark_folders() ==
         (std::vector<std::string>{"Empty", "News", "News/Tech"}));
  const std::string with_folders = slurp(dir / "bookmarks");
  EXPECT(with_folders.find("#folder\tEmpty\n") != std::string::npos);
  EXPECT(with_folders.find("\tfolder=News/Tech") != std::string::npos);
  EXPECT(xanatoo::store_bookmark_remove("https://e.example/", marks));
  // The folders stay when their bookmarks go.
  EXPECT(xanatoo::stored_bookmark_folders() ==
         (std::vector<std::string>{"Empty", "News", "News/Tech"}));
  // Only the folders of bookmarks remain without explicit ones.
  EXPECT(xanatoo::store_bookmark_changes({}, {}, xanatoo::stored_bookmarks()));
  EXPECT(xanatoo::stored_bookmark_folders() == (std::vector<std::string>{"Empty"}));

  // Ring: re-recording moves to the end; capped.
  std::vector<std::string> hist = url;
  xanatoo::record_input_history("url", hist, "https://one/", 3);
  xanatoo::record_input_history("url", hist, "https://three/", 3);
  xanatoo::record_input_history("url", hist, "https://four/", 3);
  const std::vector<std::string> want = {"https://one/", "https://three/",
                                         "https://four/"};
  EXPECT(hist == want);
  EXPECT(xanatoo::stored_input_history("url", 3) == want);

  // Another process writes: noticed once, and its rows are visible.
  EXPECT(!xanatoo::statedb_changed_elsewhere());
  {
    Db other(open(t.str()));
    EXPECT(exec(other, "INSERT INTO bookmarks(address, title, time) "
                       "VALUES('https://d.example/', 'D', 0);"));
  }
  EXPECT(xanatoo::statedb_changed_elsewhere());
  EXPECT(!xanatoo::statedb_changed_elsewhere());
  EXPECT_EQ(xanatoo::stored_bookmarks().size(), 3u);

  // Reopening does not import the text files again.
  xanatoo::statedb_use_path(t.str(), dir.string());
  EXPECT_EQ(xanatoo::stored_bookmarks().size(), 3u);

  // --no-save: reads, never writes.
  xanatoo::set_no_save(true);
  xanatoo::statedb_use_path(t.str(), dir.string());
  EXPECT(xanatoo::store_bookmark_remove("https://d.example/", {}));
  EXPECT_EQ(xanatoo::stored_bookmarks().size(), 3u);
  xanatoo::set_no_save(false);

  xanatoo::statedb_use_path({});
  fs::remove_all(dir, ec);
}

void test_visits() {
  using namespace xanatoo;
  // Text hash: whitespace and markup do not matter, words do.
  const std::string a = document_text_hash(parse_document("# Title\n\nSome *text*  here.\n", "a.md"));
  const std::string b = document_text_hash(parse_document("# Title\n\nSome text\nhere.\n", "b.md"));
  const std::string c = document_text_hash(parse_document("# Title\n\nOther text here.\n", "c.md"));
  EXPECT_EQ(a.substr(0, 7), "sha256:");
  EXPECT_EQ(a, b);
  EXPECT(a != c);
  EXPECT(document_text_hash(Document{}).empty());
  for (VisitKind k : {VisitKind::Nav, VisitKind::Link, VisitKind::NewTab,
                      VisitKind::Reload, VisitKind::External})
    EXPECT(parse_visit_kind(visit_kind_name(k)) == k);
  EXPECT(!parse_visit_kind("bogus"));

  TempDb t("visits");
  statedb_use_path(t.str(), t.path.string() + ".d");
  PageObservation o;
  o.url = "https://a.example/x";
  o.time = 1000;
  o.status = 200;
  o.etag = "\"v1\"";
  o.text_hash = a;
  const int64_t o1 = store_observation(o);
  EXPECT(o1 > 0);
  o.time = 2000;  // same content again: the same row
  EXPECT_EQ(store_observation(o), o1);
  o.text_hash = c;  // changed
  const int64_t o2 = store_observation(o);
  EXPECT(o2 > 0 && o2 != o1);
  auto back = load_observation(o1);
  EXPECT(back && back->etag == "\"v1\"" && back->text_hash == a);
  EXPECT(!store_observation(PageObservation{}));
  // A snapshot shown again is the observation it was seen as, even after
  // the page changed since.
  PageObservation snap = o;
  snap.text_hash = a;
  snap.snapshot = std::string(32, '1');
  snap.time = 3000;
  const int64_t o3 = store_observation(snap);
  EXPECT(o3 > 0 && o3 != o1 && o3 != o2);
  EXPECT(load_observation(o3)->snapshot == snap.snapshot);
  EXPECT(load_observation(o1)->snapshot.empty());
  o.time = 4000;  // the live page again
  EXPECT(store_observation(o) != o3);
  snap.time = 5000;
  EXPECT_EQ(store_observation(snap), o3);

  VisitRecord v;
  v.address = "https://a.example/x";
  v.time = 10;
  v.kind = VisitKind::Address;
  v.observation = o1;
  v.title = "X";
  const int64_t v1 = store_visit(v);
  v.address = "https://a.example/y//id:intro";
  v.time = 20;
  v.kind = VisitKind::Link;
  v.source = v1;
  v.href = "y";
  v.observation = 0;
  v.title = "";
  const int64_t v2 = store_visit(v);
  EXPECT(v1 > 0 && v2 > v1);
  store_visit_leave(v1, 20);
  EXPECT_EQ(latest_visit_of("https://a.example/x"), v1);
  EXPECT_EQ(latest_visit_of("https://nowhere/"), 0);
  // Visited links ask per page (base): the passage visit counts for /y.
  EXPECT(page_visited("https://a.example/x"));
  EXPECT(page_visited("https://a.example/y"));
  EXPECT(!page_visited("https://a.example/y//id:intro"));
  EXPECT(!page_visited("https://nowhere/"));
  EXPECT(!page_visited(""));
  auto rv = recent_visits(10);
  EXPECT_EQ(rv.size(), 2u);
  if (rv.size() == 2) {
    EXPECT_EQ(rv[0].id, v2);
    EXPECT(rv[0].kind == VisitKind::Link);
    EXPECT_EQ(rv[0].source, v1);
    EXPECT_EQ(rv[0].href, "y");
    EXPECT_EQ(rv[1].leave_time, 20);
    EXPECT_EQ(rv[1].observation, o1);
  }
  EXPECT_EQ(recent_visits(10, 20).size(), 1u);
  {
    // Pages are per base address: the passage visit counts for /y.
    Db raw(open(t.str()));
    Stmt s(raw, "SELECT address, visit_count, title FROM pages ORDER BY id;");
    EXPECT(s.row() && s.text(0) == "https://a.example/x" && s.text(2) == "X");
    EXPECT(s.row() && s.text(0) == "https://a.example/y" && s.int64(1) == 1);
  }
  // --no-save records nothing.
  set_no_save(true);
  EXPECT_EQ(store_visit(v), 0);
  set_no_save(false);
  EXPECT_EQ(recent_visits(10).size(), 2u);
  {
    // History panel pages: ties in time are paged by id; filter by title
    // or address, case-insensitive, with LIKE wildcards taken literally.
    VisitRecord w;
    w.time = 20;  // same time as v2
    w.address = "https://b.example/100%_off";
    w.title = "Closures";
    w.kind = VisitKind::Address;
    const int64_t v3 = store_visit(w);
    auto p1 = visit_page(2, nullptr);
    EXPECT_EQ(p1.size(), 2u);
    if (p1.size() == 2) {
      EXPECT_EQ(p1[0].id, v3);
      EXPECT_EQ(p1[1].id, v2);
      auto p2 = visit_page(2, &p1[1]);
      EXPECT_EQ(p2.size(), 1u);
      EXPECT(!p2.empty() && p2[0].id == v1);
    }
    EXPECT_EQ(visit_page(10, nullptr, "CLOSURE").size(), 1u);
    EXPECT_EQ(visit_page(10, nullptr, "a.example").size(), 2u);
    EXPECT_EQ(visit_page(10, nullptr, "%_").size(), 1u);
    EXPECT_EQ(visit_page(10, nullptr, "0%_o").size(), 1u);
    EXPECT_EQ(visit_page(10, nullptr, "x_y").size(), 0u);
  }
  {
    // Removing a visit: the visits reached from it take its source, and
    // a later visit naming the removed one as source is stored unlinked.
    VisitRecord w;
    w.address = "https://c.example/1";
    w.time = 100;
    w.kind = VisitKind::Address;
    const int64_t c1 = store_visit(w);
    w.address = "https://c.example/2";
    w.time = 110;
    w.kind = VisitKind::Link;
    w.source = c1;
    const int64_t c2 = store_visit(w);
    w.address = "https://c.example/3";
    w.time = 120;
    w.source = c2;
    const int64_t c3 = store_visit(w);
    EXPECT_EQ(remove_visits({c2}), 1u);
    EXPECT_EQ(remove_visits({c2}), 0u);
    auto rv2 = recent_visits(1);
    EXPECT(!rv2.empty() && rv2[0].id == c3 && rv2[0].source == c1);
    w.address = "https://c.example/4";
    w.time = 130;
    w.source = c2;  // gone
    const int64_t c4 = store_visit(w);
    EXPECT(c4 > 0);
    rv2 = recent_visits(1);
    EXPECT(!rv2.empty() && rv2[0].id == c4 && rv2[0].source == 0);
    Db raw(open(t.str()));
    Stmt s(raw, "SELECT visit_count FROM pages WHERE address = ?;",
           {std::string_view("https://c.example/2")});
    EXPECT(s.row() && s.int64(0) == 0);
    set_no_save(true);
    EXPECT_EQ(remove_visits({c1}), 0u);
    set_no_save(false);
  }
  statedb_use_path({});
  std::error_code ec;
  fs::remove_all(t.path.string() + ".d", ec);
}

}  // namespace
#endif

void test_sqlite_db(int& passed, int& failed) {
#if XANATOO_SQLITE
  test_migrations();
  test_statements();
  test_connections();
  test_concurrent_writers();
  test_statedb();
  test_visits();
#endif
  passed += g_passed;
  failed += g_failed;
}
