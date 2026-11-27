// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/http_cookies.hpp"
#include "xanatoo/paths.hpp"
#include "xanatoo/sqlite_db.hpp"
#include "xanatoo/util.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <locale>
#include <map>
#include <mutex>
#include <sstream>
#include <tuple>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace xanatoo {
namespace {

using Key = std::tuple<std::string, std::string, std::string>;
thread_local bool* sent_cookies = nullptr;
std::mutex jar_mutex;
std::map<Key, HttpCookie> session;
std::map<std::string, CookieSite> last_requests;

constexpr size_t kMaxPair = 4096;
constexpr size_t kMaxHeader = 8192;
constexpr size_t kMaxSession = 512;
constexpr int64_t kMaxAge = 400 * 24 * 60 * 60;

int64_t now_seconds() {
  using namespace std::chrono;
  return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

sqlite::Db open_jar(bool write = false, bool create = false) {
  if (no_save()) return {};
  const std::string dir = state_dir();
  if (dir.empty()) return {};
  const auto path = (std::filesystem::path(dir) / "cookies.sqlite").string();
  if (create && !ensure_dir(dir)) return {};
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK |
                        O_CLOEXEC | (create ? O_CREAT : 0), 0600);
  if (fd < 0) return {};
  struct stat st {};
  const bool private_file = ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
                            st.st_uid == ::geteuid() && (st.st_mode & 077) == 0;
  ::close(fd);
  if (!private_file) return {};
  const auto mode = create ? sqlite::OpenMode::Create :
                    write ? sqlite::OpenMode::ReadWrite : sqlite::OpenMode::ReadOnly;
  sqlite::Db db(sqlite::open_store(path, {
      "CREATE TABLE IF NOT EXISTS sites(origin TEXT PRIMARY KEY, enabled INTEGER NOT NULL);"
      "CREATE TABLE IF NOT EXISTS cookies(origin TEXT NOT NULL, path TEXT NOT NULL,"
      " name TEXT NOT NULL, value TEXT NOT NULL, enabled INTEGER NOT NULL,"
      " expires INTEGER NOT NULL, PRIMARY KEY(origin,path,name),"
      " FOREIGN KEY(origin) REFERENCES sites(origin));"
  }, {mode}));
  if (db && write) sqlite::exec(db, "PRAGMA secure_delete=ON");
  return db;
}

std::string_view path_of(const HttpUrl& url) {
  return std::string_view(url.target).substr(0, url.target.find('?'));
}

bool valid_path(std::string_view path) {
  return !path.empty() && path.size() <= kMaxPair && path.front() == '/' &&
         std::ranges::all_of(path, [](unsigned char c) {
           return c > 32 && c < 127 && c != ';' && c != '?' && c != '#';
         });
}

bool read_pair(std::string_view pair, HttpCookie& cookie) {
  const auto eq = pair.find('=');
  if (eq == 0 || eq == pair.npos || pair.size() > kMaxPair) return false;
  const auto name = pair.substr(0, eq);
  const auto value = pair.substr(eq + 1);
  constexpr std::string_view separators = "()<>@,;:\\\"/[]?={}";
  if (!std::ranges::all_of(name, [&](unsigned char c) {
        return c > 32 && c < 127 && separators.find(c) == separators.npos;
      })) return false;
  auto octets = value;
  if (octets.size() >= 2 && octets.front() == '"' && octets.back() == '"')
    octets = octets.substr(1, octets.size() - 2);
  if (!std::ranges::all_of(octets, [](unsigned char c) {
        return c >= 0x21 && c <= 0x7e && c != '"' && c != ',' &&
               c != ';' && c != '\\';
      })) return false;
  cookie.name = name;
  cookie.value = value;
  return true;
}

Key key_of(const HttpCookie& cookie) {
  return {cookie.origin, cookie.path, cookie.name};
}

bool site_enabled(sqlite3* db, std::string_view origin) {
  sqlite::Stmt q(db, "SELECT enabled FROM sites WHERE origin=?", {origin});
  return q.row() && q.int64(0);
}

bool write_site(sqlite3* db, std::string_view origin, bool enabled) {
  sqlite::Stmt q(db, "INSERT INTO sites VALUES(?,?) ON CONFLICT(origin)"
                      " DO UPDATE SET enabled=excluded.enabled");
  return q.bind(1, origin).bind(2, int(enabled)).run();
}

bool save_cookie(sqlite3* db, const HttpCookie& cookie) {
  sqlite::Stmt q(db, "INSERT INTO cookies VALUES(?,?,?,?,?,?)"
                     " ON CONFLICT(origin,path,name) DO UPDATE SET"
                     " value=excluded.value,enabled=excluded.enabled,expires=excluded.expires");
  return q.bind(1, cookie.origin).bind(2, cookie.path).bind(3, cookie.name)
      .bind(4, cookie.value).bind(5, int(cookie.enabled)).bind(6, cookie.expires).run();
}

bool save_imported_cookies(std::string_view origin,
                           const std::vector<HttpCookie>& cookies) {
  const std::lock_guard lock(jar_mutex);
  auto db = open_jar(true, true);
  if (!db) return false;
  sqlite::Tx tx(db);
  if (!tx.ok() || !write_site(db, origin, true)) return false;
  for (const auto& cookie : cookies)
    if (!save_cookie(db, cookie)) return false;
  if (!tx.commit()) return false;
  for (const auto& cookie : cookies) session.erase(key_of(cookie));
  return true;
}

bool erase_cookie(sqlite3* db, const HttpCookie& cookie) {
  sqlite::Stmt q(db, "DELETE FROM cookies WHERE origin=? AND path=? AND name=?",
                 {cookie.origin, cookie.path, cookie.name});
  return q.run();
}

bool cookie_disabled(sqlite3* db, const HttpCookie& cookie) {
  if (const auto it = session.find(key_of(cookie)); it != session.end())
    return !it->second.enabled;
  sqlite::Stmt q(db, "SELECT enabled FROM cookies WHERE origin=? AND path=? AND name=?",
                 {cookie.origin, cookie.path, cookie.name});
  return q.row() && !q.int64(0);
}

std::vector<HttpCookie> load_cookies(sqlite3* db, std::string_view origin,
                                     int64_t now) {
  sqlite::Stmt q(db, "SELECT origin,path,name,value,enabled,expires FROM cookies"
                     " WHERE (?1='' OR origin=?1) AND (enabled=0 OR expires=0 OR expires>?2)"
                     " ORDER BY rowid");
  q.bind(1, origin).bind(2, now);
  std::vector<HttpCookie> cookies;
  while (q.row())
    cookies.push_back({q.text(0), q.text(1), q.text(2), q.text(3),
                       q.int64(4) != 0, q.int64(5)});
  for (const auto& [key, cookie] : session) {
    if (!origin.empty() && cookie.origin != origin) continue;
    const bool has_persisted = std::ranges::any_of(cookies, [&](const auto& c) {
      return key_of(c) == key;
    });
    if (!has_persisted) cookies.push_back(cookie);
  }
  return cookies;
}

int64_t expires_at(std::string_view value) {
  using namespace std::chrono;
  std::tm tm{};
  std::istringstream input{std::string(value)};
  input.imbue(std::locale::classic());
  input >> std::get_time(&tm, "%a, %d %b %Y %H:%M:%S GMT");
  if (input.fail()) return 0;
  const year_month_day date{year(tm.tm_year + 1900), month(tm.tm_mon + 1),
                            day(tm.tm_mday)};
  if (!date.ok()) return 0;
  const auto point = sys_days(date) + hours(tm.tm_hour) + minutes(tm.tm_min) +
                     seconds(tm.tm_sec);
  const auto timestamp = point.time_since_epoch().count();
  return timestamp == 0 ? -1 : timestamp;
}

bool has_host_prefix(const HttpCookie& cookie) {
  return starts_with_ci(cookie.name, "__Host-") ||
         starts_with_ci(cookie.name, "__Host-Http-");
}

bool parse_set_cookie(const HttpUrl& url, std::string_view value,
                      HttpCookie& cookie, int64_t now) {
  auto pair = value.substr(0, value.find(';'));
  trim_ascii(pair);
  if (!read_pair(pair, cookie)) return false;
  const auto slash = path_of(url).rfind('/');
  cookie.path = slash == 0 ? "/" : std::string(path_of(url).substr(0, slash));
  bool domain = false, root_path = false, secure = false, http_only = false;
  std::optional<int64_t> max_age;
  while (value.find(';') != value.npos) {
    value.remove_prefix(value.find(';') + 1);
    auto attr = value.substr(0, value.find(';'));
    trim_ascii(attr);
    const auto eq = attr.find('=');
    const auto name = ascii_lower(attr.substr(0, eq));
    auto val = eq == attr.npos ? std::string_view{} : attr.substr(eq + 1);
    trim_ascii(val);
    if (name == "domain") {
      domain = true;
      if (val.starts_with('.')) val.remove_prefix(1);
      const auto host = ascii_lower(url.host);
      const auto parent = ascii_lower(val);
      if (parent.empty() || (host != parent && !ends_with(host, "." + parent)))
        return false;
    } else if (name == "path" && valid_path(val)) {
      cookie.path = val;
      root_path = val == "/";
    } else if (name == "max-age") {
      int64_t age;
      const auto [end, ec] = std::from_chars(val.data(), val.data() + val.size(), age);
      if (end == val.data() + val.size()) {
        if (ec == std::errc{}) max_age = age;
        if (ec == std::errc::result_out_of_range)
          max_age = val.starts_with('-') ? 0 : kMaxAge;
      }
    } else if (name == "expires") {
      cookie.expires = expires_at(val);
    } else if (name == "secure") {
      secure = true;
    } else if (name == "httponly") {
      http_only = true;
    } else if (name == "partitioned") {
      return false;
    }
  }
  if (max_age) cookie.expires = *max_age <= 0 ? now : now + std::min(*max_age, kMaxAge);
  if (cookie.expires) cookie.expires = std::min(cookie.expires, now + kMaxAge);
  if (has_host_prefix(cookie) && (domain || !root_path || !secure)) return false;
  if (starts_with_ci(cookie.name, "__Secure-") && !secure) return false;
  if ((starts_with_ci(cookie.name, "__Http-") ||
       starts_with_ci(cookie.name, "__Host-Http-")) && (!http_only || !secure))
    return false;
  return true;
}

}  // namespace

HttpCookieScope::HttpCookieScope(bool& sent) : previous_(sent_cookies) {
  sent_cookies = &sent;
}

HttpCookieScope::~HttpCookieScope() {
  sent_cookies = previous_;
}

bool http_cookies_allowed() {
  return sent_cookies != nullptr;
}

bool http_cookies_sent() {
  return sent_cookies && *sent_cookies;
}

bool http_cookie_site_enabled(const HttpUrl& url) {
  const auto origin = cookie_origin(url);
  if (origin.empty()) return false;
  const std::lock_guard lock(jar_mutex);
  auto db = open_jar();
  return db && site_enabled(db, origin);
}

std::string cookie_origin(const HttpUrl& url) {
  if (!url.secure) return {};
  const auto host = ascii_lower(url.host);
  auto origin = url.authority.starts_with('[') ? "https://[" + host + "]"
                                               : "https://" + host;
  if (url.port != 443) origin += ":" + std::to_string(url.port);
  return origin;
}

bool import_http_cookie(std::string_view scope, std::string_view pair) {
  const auto url = parse_http_url(scope);
  if (!url || !url->secure || scope.find_first_of("?#") != scope.npos ||
      !valid_path(url->target)) return false;
  HttpCookie cookie;
  cookie.origin = cookie_origin(*url);
  cookie.path = url->target;
  if (!read_pair(pair, cookie) || (has_host_prefix(cookie) && cookie.path != "/"))
    return false;
  return save_imported_cookies(cookie.origin, {cookie});
}

size_t import_netscape_cookies(std::string_view scope, std::istream& input) {
  const auto url = parse_http_url(scope);
  if (!url || !url->secure || url->target != "/" ||
      scope.find_first_of("?#") != scope.npos) return 0;

  const auto origin = cookie_origin(*url);
  const auto host = ascii_lower(url->host);
  const auto now = now_seconds();
  std::vector<HttpCookie> cookies;
  char buffer[2 * kMaxPair + 1024];
  while (input.getline(buffer, sizeof(buffer))) {
    std::string_view line(buffer, static_cast<size_t>(input.gcount()) - !input.eof());
    if (line.ends_with('\r')) line.remove_suffix(1);
    if (line.starts_with("#HttpOnly_")) line.remove_prefix(10);
    else if (line.empty() || line.starts_with('#')) continue;

    std::array<std::string_view, 7> fields;
    for (auto& field : fields) {
      const auto tab = line.find('\t');
      const bool last = &field == &fields.back();
      if (last != (tab == line.npos)) return 0;
      field = line.substr(0, tab);
      if (!last) line.remove_prefix(tab + 1);
    }
    const auto& [domain, subdomains, path, secure, expiry, name, value] = fields;
    auto cookie_host = ascii_lower(domain);
    if (cookie_host.starts_with('.')) cookie_host.erase(0, 1);
    if (cookie_host != host &&
        !(subdomains == "TRUE" && host.ends_with("." + cookie_host))) continue;
    if ((subdomains != "TRUE" && subdomains != "FALSE") ||
        (secure != "TRUE" && secure != "FALSE") ||
        !valid_path(path) || name.find('=') != name.npos ||
        name.size() + value.size() + 1 > kMaxPair) return 0;

    HttpCookie cookie{origin, std::string(path), {}, {}};
    if (!expiry.empty()) {
      const auto [end, error] = std::from_chars(expiry.data(),
          expiry.data() + expiry.size(), cookie.expires);
      if (error != std::errc{} || end != expiry.data() + expiry.size()) return 0;
    }
    if (cookie.expires != 0 && cookie.expires <= now) continue;
    if (!read_pair(std::string(name) + "=" + std::string(value), cookie) ||
        (has_host_prefix(cookie) &&
         (path != "/" || subdomains != "FALSE" || secure != "TRUE"))) return 0;
    if (cookies.size() == 4096) return 0;
    cookies.push_back(std::move(cookie));
  }
  if (!input.eof() || input.bad() || cookies.empty()) return 0;
  return save_imported_cookies(origin, cookies) ? cookies.size() : 0;
}

bool set_cookie_site(std::string_view origin, bool enabled) {
  const auto url = parse_http_url(origin);
  if (!url || origin != cookie_origin(*url)) return false;
  const std::lock_guard lock(jar_mutex);
  auto db = open_jar(true, enabled);
  return db && write_site(db, origin, enabled);
}

bool edit_http_cookie(const HttpCookie& key, bool remove, bool enabled) {
  const std::lock_guard lock(jar_mutex);
  auto db = open_jar(true);
  if (!db) return false;
  if (remove) {
    if (!erase_cookie(db, key)) return false;
    session.erase(key_of(key));
    return true;
  }
  sqlite::Stmt q(db, "UPDATE cookies SET enabled=? WHERE origin=? AND path=? AND name=?");
  if (!q.bind(1, int(enabled)).bind(2, key.origin).bind(3, key.path)
        .bind(4, key.name).run()) return false;
  if (auto it = session.find(key_of(key)); it != session.end()) it->second.enabled = enabled;
  return true;
}

CookieSnapshot http_cookie_snapshot() {
  const std::lock_guard lock(jar_mutex);
  auto db = open_jar();
  if (!db) return {};
  CookieSnapshot snapshot;
  sqlite::Stmt q(db, "SELECT origin,enabled FROM sites ORDER BY origin");
  while (q.row()) {
    CookieSite site{q.text(0), q.int64(1) != 0, {}, {}};
    if (auto it = last_requests.find(site.origin); it != last_requests.end()) {
      site.last_path = it->second.last_path;
      site.last_names = it->second.last_names;
    }
    snapshot.sites.push_back(std::move(site));
  }
  snapshot.cookies = load_cookies(db, {}, now_seconds());
  return snapshot;
}

std::string http_cookie_header(const HttpUrl& url) {
  const auto origin = cookie_origin(url);
  if (origin.empty()) return {};
  const std::lock_guard lock(jar_mutex);
  auto db = open_jar();
  if (!db || !site_enabled(db, origin)) return {};
  const auto path = path_of(url);
  auto cookies = load_cookies(db, origin, now_seconds());
  std::stable_sort(cookies.begin(), cookies.end(), [](const auto& a, const auto& b) {
    return a.path.size() > b.path.size();
  });
  std::string header;
  for (const auto& cookie : cookies) {
    if (!cookie.enabled || !path.starts_with(cookie.path)) continue;
    if (path.size() != cookie.path.size() && !cookie.path.ends_with('/') &&
        path[cookie.path.size()] != '/') continue;
    if (header.size() + cookie.name.size() + cookie.value.size() + 3 > kMaxHeader)
      continue;
    if (!header.empty()) header += "; ";
    header += cookie.name + "=" + cookie.value;
  }
  return header;
}

void record_http_cookies(const HttpUrl& url, std::string_view header) {
  if (header.empty()) return;
  if (sent_cookies) *sent_cookies = true;
  std::string names;
  while (!header.empty()) {
    const auto end = header.find("; ");
    if (!names.empty()) names += ", ";
    names += header.substr(0, header.find('='));
    if (end == header.npos) break;
    header.remove_prefix(end + 2);
  }
  const auto origin = cookie_origin(url);
  const std::lock_guard lock(jar_mutex);
  last_requests[origin] = {origin, true, std::string(path_of(url)), std::move(names)};
}

void receive_http_cookies(const HttpUrl& url, std::string_view headers) {
  const auto origin = cookie_origin(url);
  if (origin.empty() || ascii_lower(headers).find("\nset-cookie:") == std::string::npos) return;
  const std::lock_guard lock(jar_mutex);
  auto db = open_jar(true);
  if (!db) return;
  sqlite::Tx tx(db);
  if (!tx.ok() || !site_enabled(db, origin)) return;
  std::map<Key, std::optional<HttpCookie>> changes;
  const auto now = now_seconds();
  sqlite::Stmt prune(db, "DELETE FROM cookies WHERE enabled=1 AND expires<>0 AND expires<=?");
  if (!prune.bind(1, now).run()) return;
  while (!headers.empty()) {
    const auto eol = headers.find('\n');
    auto line = headers.substr(0, eol);
    headers.remove_prefix(eol == headers.npos ? headers.size() : eol + 1);
    if (!starts_with_ci(line, "set-cookie:")) continue;
    line.remove_prefix(11);
    if (line.ends_with('\r')) line.remove_suffix(1);
    trim_ascii(line);
    HttpCookie cookie;
    cookie.origin = origin;
    if (!parse_set_cookie(url, line, cookie, now) || cookie_disabled(db, cookie))
      continue;
    const auto key = key_of(cookie);
    if (cookie.expires > now) {
      if (!save_cookie(db, cookie)) return;
      changes[key] = std::nullopt;
    } else {
      if (!erase_cookie(db, cookie)) return;
      changes[key] = cookie.expires == 0 ? std::optional(std::move(cookie)) : std::nullopt;
    }
  }
  if (!tx.commit()) return;
  for (auto& [key, cookie] : changes) {
    if (cookie) {
      if (session.size() < kMaxSession || session.contains(key))
        session.insert_or_assign(key, std::move(*cookie));
    } else {
      session.erase(key);
    }
  }
}

}  // namespace xanatoo
