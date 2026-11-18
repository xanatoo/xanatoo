// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Shared SQLite helpers for the local stores (revstore, sidestore, nostr,
// history). Only included where XANATOO_SQLITE is on. docs/HISTORY.md "A".
//
// Several xanatoo processes may use the same database at once:
//   - every connection runs in WAL mode (readers never block the writer),
//     with a busy timeout so a second writer waits instead of failing;
//   - schemas are versioned by PRAGMA user_version and migrated inside
//     BEGIN IMMEDIATE, so two processes starting together migrate once;
//   - data_version() tells whether *another* connection committed since
//     the last look, so panels can reload only then.
// Keep write transactions short: never across a frame or a network wait.

#include <sqlite3.h>

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xanatoo::sqlite {

enum class OpenMode {
  ReadOnly,   // existing file only, never written (no pragmas that write)
  ReadWrite,  // existing file only
  Create,     // create the file if missing (the caller ensured the dir)
};

struct OpenOptions {
  OpenMode mode = OpenMode::Create;
  int busy_ms = 2000;
};

// Opens `path` with WAL (writable modes), synchronous=NORMAL,
// foreign_keys=ON and the busy timeout. nullptr on failure (or a missing
// file in the existing-only modes).
sqlite3* open(const std::string& path, OpenOptions opt = {});

// Schema steps: steps[i] brings user_version i to i + 1. Step 0 should be
// idempotent (CREATE … IF NOT EXISTS) so databases created before
// versioning (user_version 0, tables present) pass through it unharmed.
using Migrations = std::vector<const char*>;

enum class MigrateResult { Ok, Newer, Failed };

// Applies the missing steps in one BEGIN IMMEDIATE transaction (re-reading
// user_version inside it, so concurrent starters migrate once). Newer:
// the database was written by a later xanatoo with more steps; nothing is
// changed.
MigrateResult migrate(sqlite3* db, const Migrations& steps);

// open() + migrate(). A database newer than `steps` is reopened read-only
// with a warning on stderr (writes then fail instead of corrupting a
// schema this binary does not know). nullptr on failure.
sqlite3* open_store(const std::string& path, const Migrations& steps,
                    OpenOptions opt = {});

int user_version(sqlite3* db);
bool exec(sqlite3* db, const char* sql);
// PRAGMA data_version: changes when another connection has committed.
int64_t data_version(sqlite3* db);
bool table_has_column(sqlite3* db, const char* table, const char* column);

// Owns a connection; closes it on scope exit (after every Stmt is gone).
struct Db {
  sqlite3* db = nullptr;
  Db() = default;
  explicit Db(sqlite3* d) : db(d) {}
  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;
  Db(Db&& o) noexcept : db(o.db) { o.db = nullptr; }
  Db& operator=(Db&& o) noexcept;
  ~Db();
  explicit operator bool() const { return db != nullptr; }
  operator sqlite3*() const { return db; }
};

// Prepared statement, finalized on scope exit. Binds are 1-based.
class Stmt {
public:
  Stmt(sqlite3* db, const char* sql);
  // Binds `args` as text, in order.
  Stmt(sqlite3* db, const char* sql,
       std::initializer_list<std::string_view> args);
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;
  ~Stmt();

  bool valid() const { return st_ != nullptr; }
  sqlite3_stmt* handle() const { return st_; }

  Stmt& bind(int i, std::string_view v);  // empty → '' (never NULL)
  Stmt& bind(int i, int64_t v);
  Stmt& bind(int i, int v) { return bind(i, static_cast<int64_t>(v)); }
  Stmt& bind_blob(int i, std::string_view v);
  Stmt& bind_null(int i);
  // nullopt → NULL
  Stmt& bind_opt(int i, const std::optional<std::string>& v);
  Stmt& bind_opt(int i, std::optional<int64_t> v);

  bool row();  // step; true while there is a row
  bool run();  // step to completion; true on SQLITE_DONE
  void reset();  // for reuse: reset + clear bindings

  std::string text(int col) const;  // NULL → ""
  std::string blob(int col) const;
  int64_t int64(int col) const;
  bool is_null(int col) const;

private:
  sqlite3_stmt* st_ = nullptr;
};

// BEGIN IMMEDIATE … COMMIT; rolled back unless commit() succeeded.
class Tx {
public:
  explicit Tx(sqlite3* db);
  Tx(const Tx&) = delete;
  Tx& operator=(const Tx&) = delete;
  ~Tx();
  bool ok() const { return started_; }
  bool commit();

private:
  sqlite3* db_;
  bool started_ = false;
  bool done_ = false;
};

}  // namespace xanatoo::sqlite
