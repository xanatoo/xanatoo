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

}  // namespace
#endif

void test_sqlite_db(int& passed, int& failed) {
#if XANATOO_SQLITE
  test_migrations();
  test_statements();
  test_connections();
  test_concurrent_writers();
#endif
  passed += g_passed;
  failed += g_failed;
}
