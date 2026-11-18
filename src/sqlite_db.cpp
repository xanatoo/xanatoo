// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#ifndef XANATOO_SQLITE
#define XANATOO_SQLITE 1
#endif
#if XANATOO_SQLITE

#include "xanatoo/sqlite_db.hpp"

#include <cstdio>
#include <filesystem>

namespace xanatoo::sqlite {

sqlite3* open(const std::string& path, OpenOptions opt) {
  if (path.empty()) return nullptr;
  if (opt.mode != OpenMode::Create) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return nullptr;
  }
  int flags = SQLITE_OPEN_FULLMUTEX;
  switch (opt.mode) {
    case OpenMode::ReadOnly: flags |= SQLITE_OPEN_READONLY; break;
    case OpenMode::ReadWrite: flags |= SQLITE_OPEN_READWRITE; break;
    case OpenMode::Create:
      flags |= SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
      break;
  }
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(path.c_str(), &db, flags, nullptr) != SQLITE_OK) {
    if (db) sqlite3_close(db);
    return nullptr;
  }
  sqlite3_busy_timeout(db, opt.busy_ms);
  if (opt.mode != OpenMode::ReadOnly) {
#ifndef __EMSCRIPTEN__
    // Persistent in the file; a no-op once set. Fails harmlessly where WAL
    // is unsupported (the old rollback journal stays).
    exec(db, "PRAGMA journal_mode=WAL;");
#endif
    exec(db, "PRAGMA synchronous=NORMAL;");
  }
  exec(db, "PRAGMA foreign_keys=ON;");
  return db;
}

int user_version(sqlite3* db) {
  Stmt s(db, "PRAGMA user_version;");
  return s.row() ? static_cast<int>(s.int64(0)) : -1;
}

MigrateResult migrate(sqlite3* db, const Migrations& steps) {
  if (!db) return MigrateResult::Failed;
  const int want = static_cast<int>(steps.size());
  // Cheap check without a write lock: nothing to do in the common case.
  const int have = user_version(db);
  if (have < 0) return MigrateResult::Failed;
  if (have > want) return MigrateResult::Newer;
  if (have == want) return MigrateResult::Ok;

  Tx tx(db);
  if (!tx.ok()) return MigrateResult::Failed;
  const int now = user_version(db);  // another process may have migrated
  if (now > want) return MigrateResult::Newer;
  for (int i = now; i < want; ++i) {
    if (!exec(db, steps[static_cast<size_t>(i)])) return MigrateResult::Failed;
  }
  if (now < want &&
      !exec(db, ("PRAGMA user_version=" + std::to_string(want) + ";").c_str()))
    return MigrateResult::Failed;
  return tx.commit() ? MigrateResult::Ok : MigrateResult::Failed;
}

sqlite3* open_store(const std::string& path, const Migrations& steps,
                    OpenOptions opt) {
  sqlite3* db = open(path, opt);
  if (!db) return nullptr;
  if (opt.mode == OpenMode::ReadOnly) {
    if (user_version(db) < 0) {
      sqlite3_close(db);
      return nullptr;
    }
    return db;
  }
  switch (migrate(db, steps)) {
    case MigrateResult::Ok:
      return db;
    case MigrateResult::Newer: {
      std::fprintf(stderr,
                   "xanatoo: %s was written by a newer xanatoo (schema %d > "
                   "%zu); opening it read-only\n",
                   path.c_str(), user_version(db), steps.size());
      sqlite3_close(db);
      opt.mode = OpenMode::ReadOnly;
      return open(path, opt);
    }
    case MigrateResult::Failed:
      break;
  }
  std::fprintf(stderr, "xanatoo: cannot set up %s: %s\n", path.c_str(),
               sqlite3_errmsg(db));
  sqlite3_close(db);
  return nullptr;
}

bool exec(sqlite3* db, const char* sql) {
  return db && sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

int64_t data_version(sqlite3* db) {
  Stmt s(db, "PRAGMA data_version;");
  return s.row() ? s.int64(0) : -1;
}

bool table_has_column(sqlite3* db, const char* table, const char* column) {
  const std::string sql = std::string("PRAGMA table_info(") + table + ");";
  Stmt s(db, sql.c_str());
  while (s.row())
    if (s.text(1) == column) return true;
  return false;
}

Db& Db::operator=(Db&& o) noexcept {
  if (this != &o) {
    if (db) sqlite3_close(db);
    db = o.db;
    o.db = nullptr;
  }
  return *this;
}

Db::~Db() {
  if (db) sqlite3_close(db);
}

Stmt::Stmt(sqlite3* db, const char* sql) {
  if (!db || sqlite3_prepare_v2(db, sql, -1, &st_, nullptr) != SQLITE_OK)
    st_ = nullptr;
}

Stmt::Stmt(sqlite3* db, const char* sql,
           std::initializer_list<std::string_view> args)
    : Stmt(db, sql) {
  int i = 1;
  for (std::string_view a : args) bind(i++, a);
}

Stmt::~Stmt() {
  if (st_) sqlite3_finalize(st_);
}

Stmt& Stmt::bind(int i, std::string_view v) {
  // A null data() would bind SQL NULL instead of ''.
  if (st_)
    sqlite3_bind_text(st_, i, v.empty() ? "" : v.data(),
                      static_cast<int>(v.size()), SQLITE_TRANSIENT);
  return *this;
}

Stmt& Stmt::bind(int i, int64_t v) {
  if (st_) sqlite3_bind_int64(st_, i, v);
  return *this;
}

Stmt& Stmt::bind_blob(int i, std::string_view v) {
  if (st_)
    sqlite3_bind_blob(st_, i, v.empty() ? "" : v.data(),
                      static_cast<int>(v.size()), SQLITE_TRANSIENT);
  return *this;
}

Stmt& Stmt::bind_null(int i) {
  if (st_) sqlite3_bind_null(st_, i);
  return *this;
}

Stmt& Stmt::bind_opt(int i, const std::optional<std::string>& v) {
  return v ? bind(i, std::string_view(*v)) : bind_null(i);
}

Stmt& Stmt::bind_opt(int i, std::optional<int64_t> v) {
  return v ? bind(i, *v) : bind_null(i);
}

bool Stmt::row() { return st_ && sqlite3_step(st_) == SQLITE_ROW; }
bool Stmt::run() { return st_ && sqlite3_step(st_) == SQLITE_DONE; }

void Stmt::reset() {
  if (!st_) return;
  sqlite3_reset(st_);
  sqlite3_clear_bindings(st_);
}

std::string Stmt::text(int col) const {
  const auto* t = st_ ? sqlite3_column_text(st_, col) : nullptr;
  return t ? std::string(reinterpret_cast<const char*>(t),
                         static_cast<size_t>(sqlite3_column_bytes(st_, col)))
           : std::string();
}

std::string Stmt::blob(int col) const {
  const void* p = st_ ? sqlite3_column_blob(st_, col) : nullptr;
  const int n = p ? sqlite3_column_bytes(st_, col) : 0;
  return p ? std::string(static_cast<const char*>(p), static_cast<size_t>(n))
           : std::string();
}

int64_t Stmt::int64(int col) const {
  return st_ ? sqlite3_column_int64(st_, col) : 0;
}

bool Stmt::is_null(int col) const {
  return !st_ || sqlite3_column_type(st_, col) == SQLITE_NULL;
}

Tx::Tx(sqlite3* db) : db_(db) { started_ = exec(db_, "BEGIN IMMEDIATE;"); }

bool Tx::commit() {
  if (!started_ || done_) return false;
  done_ = exec(db_, "COMMIT;");
  return done_;
}

Tx::~Tx() {
  if (started_ && !done_) exec(db_, "ROLLBACK;");
}

}  // namespace xanatoo::sqlite

#endif  // XANATOO_SQLITE
