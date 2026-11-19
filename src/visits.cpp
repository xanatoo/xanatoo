// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/visits.hpp"
#include "xanatoo/locator.hpp"
#include "xanatoo/paths.hpp"
#include "xanatoo/statedb.hpp"
#include "xanatoo/types.hpp"
#include "xanatoo/util.hpp"

#ifndef XANATOO_SQLITE
#define XANATOO_SQLITE 1
#endif
#if XANATOO_SQLITE
#include "xanatoo/sqlite_db.hpp"
#endif

#include <array>
#include <chrono>
#include <random>

namespace xanatoo {

namespace {
constexpr std::array<std::pair<VisitKind, const char*>, 9> kKinds = {{
    {VisitKind::Nav, "nav"},
    {VisitKind::Link, "link"},
    {VisitKind::Address, "address"},
    {VisitKind::Bookmark, "bookmark"},
    {VisitKind::History, "history"},
    {VisitKind::NewTab, "new-tab"},
    {VisitKind::Restore, "restore"},
    {VisitKind::External, "external"},
    {VisitKind::Reload, "reload"},
}};
}  // namespace

const char* visit_kind_name(VisitKind k) {
  for (const auto& [kind, name] : kKinds)
    if (kind == k) return name;
  return "nav";
}

std::optional<VisitKind> parse_visit_kind(std::string_view s) {
  for (const auto& [kind, name] : kKinds)
    if (s == name) return kind;
  return std::nullopt;
}

bool visit_kind_starts_trail(VisitKind k) {
  return k == VisitKind::Address || k == VisitKind::Bookmark ||
         k == VisitKind::History || k == VisitKind::External;
}

std::string document_text_hash(const Document& doc) {
  if (doc.root == INVALID_NODE) return {};
  std::string text;
  bool space = false;  // a separator is pending
  auto add = [&](std::string_view s) {
    for (char c : s) {
      const bool ws = c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                      c == '\f' || c == '\v';
      if (ws) {
        space = true;
        continue;
      }
      if (space && !text.empty()) text.push_back(' ');
      space = false;
      text.push_back(c);
    }
    space = true;
  };
  // Reading order: depth first from the root.
  std::vector<NodeId> stack{doc.root};
  while (!stack.empty()) {
    const NodeId id = stack.back();
    stack.pop_back();
    const Node& n = doc.get(id);
    for (const auto& sp : n.spans) add(sp.text);
    if (!n.alt.empty()) add(n.alt);
    for (auto it = n.children.rbegin(); it != n.children.rend(); ++it)
      if (*it != INVALID_NODE) stack.push_back(*it);
  }
  if (text.empty()) return {};
  return "sha256:" + sha256_hex("xanatoo-text 1\n" + text);
}

int64_t now_epoch_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

const std::string& visit_instance_id() {
  static const std::string id = [] {
    std::random_device rd;
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 2; ++i) {
      const uint32_t v = rd();
      for (int s = 28; s >= 0; s -= 4) out.push_back(hex[(v >> s) & 0xf]);
    }
    return out;
  }();
  return id;
}

#if XANATOO_SQLITE

namespace {
sqlite3* writable() { return no_save() ? nullptr : statedb_connection(); }

VisitRecord visit_from(const sqlite::Stmt& s) {
  // id, address, time, leave_time, source, kind, href, source_node,
  // observation, window, title
  VisitRecord v;
  v.id = s.int64(0);
  v.address = s.text(1);
  v.time = s.int64(2);
  v.leave_time = s.int64(3);
  v.source = s.int64(4);
  v.kind = parse_visit_kind(s.text(5)).value_or(VisitKind::Nav);
  v.href = s.text(6);
  v.source_node = s.text(7);
  v.observation = s.int64(8);
  v.window = s.text(9);
  v.title = s.text(10);
  return v;
}

constexpr const char* kVisitColumns =
    "SELECT id, address, time, COALESCE(leave_time, 0), COALESCE(source, 0), "
    "kind, href, source_node, COALESCE(observation, 0), window, title "
    "FROM visits ";
}  // namespace

int64_t store_observation(const PageObservation& o) {
  sqlite3* d = writable();
  if (!d || o.empty() || o.url.empty()) return 0;
  sqlite::Tx tx(d);
  {
    sqlite::Stmt last(d, "SELECT id, status, content_type, etag, last_modified, "
                         "raw_hash, text_hash, size FROM observations "
                         "WHERE url = ? ORDER BY time DESC, id DESC LIMIT 1;",
                      {o.url});
    if (last.row() && last.int64(1) == o.status && last.text(2) == o.content_type &&
        last.text(3) == o.etag && last.text(4) == o.last_modified &&
        last.text(5) == o.raw_hash && last.text(6) == o.text_hash &&
        last.int64(7) == o.size)
      return last.int64(0);
  }
  sqlite::Stmt s(d, "INSERT INTO observations(url, time, status, content_type, "
                    "etag, last_modified, raw_hash, text_hash, size) "
                    "VALUES(?,?,?,?,?,?,?,?,?);");
  s.bind(1, o.url).bind(2, o.time).bind(3, o.status).bind(4, o.content_type)
      .bind(5, o.etag).bind(6, o.last_modified).bind(7, o.raw_hash)
      .bind(8, o.text_hash).bind(9, o.size);
  if (!s.run()) return 0;
  const int64_t id = sqlite3_last_insert_rowid(d);
  return tx.commit() ? id : 0;
}

std::optional<PageObservation> load_observation(int64_t id) {
  sqlite3* d = statedb_connection();
  if (!d || id <= 0) return std::nullopt;
  sqlite::Stmt s(d, "SELECT url, time, status, content_type, etag, "
                    "last_modified, raw_hash, text_hash, size "
                    "FROM observations WHERE id = ?;");
  s.bind(1, id);
  if (!s.row()) return std::nullopt;
  PageObservation o;
  o.url = s.text(0);
  o.time = s.int64(1);
  o.status = static_cast<int>(s.int64(2));
  o.content_type = s.text(3);
  o.etag = s.text(4);
  o.last_modified = s.text(5);
  o.raw_hash = s.text(6);
  o.text_hash = s.text(7);
  o.size = s.int64(8);
  return o;
}

int64_t store_visit(const VisitRecord& v) {
  sqlite3* d = writable();
  if (!d || v.address.empty()) return 0;
  const std::string page = parse_locator(v.address).base;
  const int64_t t = v.time ? v.time : now_epoch_ms();
  sqlite::Tx tx(d);
  {
    sqlite::Stmt p(d, "INSERT INTO pages(address, title, first_seen, last_seen, "
                      "visit_count) VALUES(?1, ?2, ?3, ?3, 1) "
                      "ON CONFLICT(address) DO UPDATE SET last_seen = ?3, "
                      "visit_count = visit_count + 1, "
                      "title = CASE WHEN ?2 = '' THEN title ELSE ?2 END;");
    if (!p.bind(1, page.empty() ? v.address : page).bind(2, v.title)
             .bind(3, t).run())
      return 0;
  }
  sqlite::Stmt s(d, "INSERT INTO visits(page, address, time, source, kind, "
                    "href, source_node, observation, window, title) "
                    "VALUES((SELECT id FROM pages WHERE address = ?1), ?2, ?3, "
                    // A source removed meanwhile (remove_visits, maybe in
                    // another process) becomes NULL, not a foreign-key error.
                    "(SELECT id FROM visits WHERE id = ?4), ?5, ?6, ?7, "
                    "NULLIF(?8, 0), ?9, ?10);");
  s.bind(1, page.empty() ? v.address : page).bind(2, v.address).bind(3, t)
      .bind(4, v.source).bind(5, visit_kind_name(v.kind)).bind(6, v.href)
      .bind(7, v.source_node).bind(8, v.observation).bind(9, v.window)
      .bind(10, v.title);
  if (!s.run()) return 0;
  const int64_t id = sqlite3_last_insert_rowid(d);
  return tx.commit() ? id : 0;
}

void store_visit_leave(int64_t id, int64_t time) {
  sqlite3* d = writable();
  if (!d || id <= 0) return;
  sqlite::Stmt s(d, "UPDATE visits SET leave_time = ? WHERE id = ? "
                    "AND leave_time IS NULL;");
  s.bind(1, time).bind(2, id).run();
}

int64_t latest_visit_of(std::string_view address) {
  sqlite3* d = statedb_connection();
  if (!d || address.empty()) return 0;
  sqlite::Stmt s(d, "SELECT id FROM visits WHERE address = ? "
                    "ORDER BY time DESC, id DESC LIMIT 1;", {address});
  return s.row() ? s.int64(0) : 0;
}

std::vector<VisitRecord> recent_visits(size_t limit, int64_t before) {
  std::vector<VisitRecord> out;
  sqlite3* d = statedb_connection();
  if (!d || limit == 0) return out;
  const std::string sql = std::string(kVisitColumns) +
      "WHERE time < ? ORDER BY time DESC, id DESC LIMIT ?;";
  sqlite::Stmt s(d, sql.c_str());
  s.bind(1, before > 0 ? before : INT64_MAX)
      .bind(2, static_cast<int64_t>(limit));
  while (s.row()) out.push_back(visit_from(s));
  return out;
}

bool visit_log_available() { return statedb_connection() != nullptr; }

size_t remove_visits(const std::vector<int64_t>& ids) {
  sqlite3* d = writable();
  if (!d || ids.empty()) return 0;
  size_t removed = 0;
  sqlite::Tx tx(d);
  for (int64_t id : ids) {
    if (id <= 0) continue;
    // Visits reached from it hang on its own source instead.
    sqlite::Stmt relink(d, "UPDATE visits SET source = "
                           "(SELECT source FROM visits WHERE id = ?1) "
                           "WHERE source = ?1;");
    if (!relink.bind(1, id).run()) return 0;
    sqlite::Stmt count(d, "UPDATE pages SET visit_count = "
                          "MAX(visit_count - 1, 0) WHERE id = "
                          "(SELECT page FROM visits WHERE id = ?1);");
    if (!count.bind(1, id).run()) return 0;
    sqlite::Stmt del(d, "DELETE FROM visits WHERE id = ?1;");
    if (!del.bind(1, id).run()) return 0;
    removed += static_cast<size_t>(sqlite3_changes(d));
  }
  return tx.commit() ? removed : 0;
}

std::vector<VisitRecord> visit_page(size_t limit, const VisitRecord* after,
                                    std::string_view filter) {
  std::vector<VisitRecord> out;
  sqlite3* d = statedb_connection();
  if (!d || limit == 0) return out;
  // LIKE pattern with %, _ and the escape character itself escaped.
  std::string pattern = "%";
  for (char c : filter) {
    if (c == '%' || c == '_' || c == '\\') pattern += '\\';
    pattern += c;
  }
  pattern += '%';
  const std::string sql = std::string(kVisitColumns) +
      "WHERE (?1 = 0 OR time < ?2 OR (time = ?2 AND id < ?1)) "
      "AND (?3 = '' OR title LIKE ?4 ESCAPE '\\' "
      "OR address LIKE ?4 ESCAPE '\\') "
      "ORDER BY time DESC, id DESC LIMIT ?5;";
  sqlite::Stmt s(d, sql.c_str());
  s.bind(1, after ? after->id : int64_t{0})
      .bind(2, after ? after->time : int64_t{0})
      .bind(3, filter)
      .bind(4, pattern)
      .bind(5, static_cast<int64_t>(limit));
  while (s.row()) out.push_back(visit_from(s));
  return out;
}

#else  // !XANATOO_SQLITE

int64_t store_observation(const PageObservation&) { return 0; }
std::optional<PageObservation> load_observation(int64_t) { return std::nullopt; }
int64_t store_visit(const VisitRecord&) { return 0; }
void store_visit_leave(int64_t, int64_t) {}
size_t remove_visits(const std::vector<int64_t>&) { return 0; }
int64_t latest_visit_of(std::string_view) { return 0; }
std::vector<VisitRecord> recent_visits(size_t, int64_t) { return {}; }
bool visit_log_available() { return false; }
std::vector<VisitRecord> visit_page(size_t, const VisitRecord*,
                                    std::string_view) {
  return {};
}

#endif

}  // namespace xanatoo
