// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/gemini_identity.hpp"

#include "xanatoo/gemini.hpp"
#include "xanatoo/sexpr.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <utility>

namespace xanatoo {
namespace {

namespace fs = std::filesystem;

constexpr const char* kStoreName = "gemini-identities";

bool fail(std::string* err, std::string msg) {
  if (err) *err = std::move(msg);
  return false;
}

bool is_sha256(std::string_view s) {
  return s.size() == 64 &&
         std::all_of(s.begin(), s.end(), [](unsigned char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

// "gemini://host[:port]" (host lower-case, IPv6 bracketed) and the path
// without query or fragment.
struct ScopeParts {
  std::string authority;
  std::string path;
};

std::optional<ScopeParts> scope_parts(std::string_view url) {
  const auto u = parse_gemini_url(url);
  if (!u) return std::nullopt;
  ScopeParts out;
  std::string host;
  for (char c : u->host)
    host += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  out.authority = "gemini://";
  out.authority += host.find(':') != std::string::npos ? "[" + host + "]" : host;
  if (u->port != 1965) out.authority += ":" + std::to_string(u->port);
  out.path = u->path_query;
  if (const size_t q = out.path.find('?'); q != std::string::npos)
    out.path.resize(q);
  if (out.path.empty()) out.path = "/";
  return out;
}

std::mutex& store_mutex() {
  static std::mutex mutex;
  return mutex;
}

// --no-save (or no state directory): the identities of this process, per
// directory. Once a directory has an entry, the file is not read again.
std::map<std::string, std::vector<GeminiIdentity>>& session() {
  static std::map<std::string, std::vector<GeminiIdentity>> ids;
  return ids;
}

fs::path store_path(std::string_view directory) {
  return directory.empty() ? fs::path{} : fs::path(directory) / kStoreName;
}

bool load(std::string_view directory, std::vector<GeminiIdentity>& out,
          std::string* err) {
  out.clear();
  if (auto it = session().find(std::string(directory)); it != session().end()) {
    out = it->second;
    return true;
  }
  const fs::path path = store_path(directory);
  if (path.empty()) return true;
  std::error_code ec;
  if (!fs::exists(path, ec)) {
    if (ec) return fail(err, "cannot inspect the identity store: " + ec.message());
    return true;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) return fail(err, "cannot read the identity store " + path.string());
  std::ostringstream text;
  text << file.rdbuf();
  auto parsed = gemini_identities_parse(text.str(), err);
  if (!parsed) {
    if (err) *err = path.string() + ": " + *err;
    return false;
  }
  out = std::move(*parsed);
  for (auto& id : out) id.saved = true;
  return true;
}

bool save(std::string_view directory, const std::vector<GeminiIdentity>& ids,
          std::string* err) {
  const fs::path path = store_path(directory);
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) return fail(err, "cannot create the state directory: " + ec.message());
  static std::atomic<unsigned long> counter{0};
  fs::path temporary = path;
  temporary += ".tmp-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()) +
      "-" + std::to_string(++counter);
  {
    // Owner-only before the private keys go in.
    std::ofstream create(temporary, std::ios::binary | std::ios::trunc);
    if (!create) return fail(err, "cannot create a temporary identity store");
  }
#ifndef _WIN32
  fs::permissions(temporary, fs::perms::owner_read | fs::perms::owner_write,
                  fs::perm_options::replace, ec);
  if (ec) {
    std::error_code ignored;
    fs::remove(temporary, ignored);
    return fail(err, "cannot protect the identity store: " + ec.message());
  }
#endif
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out << gemini_identities_serialize(ids);
    out.close();
    if (!out) {
      fs::remove(temporary, ec);
      return fail(err, "cannot write the identity store");
    }
  }
  fs::rename(temporary, path, ec);
  if (ec) {
    std::error_code ignored;
    fs::remove(temporary, ignored);
    return fail(err, "cannot update the identity store: " + ec.message());
  }
  return true;
}

// Load, change, write back (or keep in the session), under the lock.
// `change` returns false (with `err`) to leave everything as it was.
bool mutate(std::string_view directory, bool persist, std::string* err,
            const std::function<bool(std::vector<GeminiIdentity>&)>& change) {
  const std::lock_guard guard(store_mutex());
  std::vector<GeminiIdentity> ids;
  if (!load(directory, ids, err)) return false;
  if (!change(ids)) return false;
  const std::string key(directory);
  if (persist && !directory.empty() && !session().count(key)) {
    if (!save(directory, ids, err)) return false;
    return true;
  }
  session()[key] = std::move(ids);
  return true;
}

GeminiIdentity* find(std::vector<GeminiIdentity>& ids, std::string_view sha) {
  for (auto& id : ids)
    if (id.sha256 == sha) return &id;
  return nullptr;
}

void quote(std::string& out, std::string_view s) {
  out += '"';
  for (char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      default: out += c; break;
    }
  }
  out += '"';
}

}  // namespace

std::string gemini_scope_of_page(std::string_view url) {
  const auto p = scope_parts(url);
  return p ? p->authority + p->path : std::string{};
}

std::string gemini_scope_of_site(std::string_view url) {
  const auto p = scope_parts(url);
  return p ? p->authority + "/" : std::string{};
}

bool gemini_scope_covers(std::string_view scope, std::string_view url) {
  const auto s = scope_parts(scope);
  const auto u = scope_parts(url);
  if (!s || !u || s->authority != u->authority) return false;
  const std::string& sp = s->path;
  const std::string& up = u->path;
  if (up.compare(0, sp.size(), sp) != 0) return false;
  return sp.back() == '/' || up.size() == sp.size() || up[sp.size()] == '/';
}

std::string gemini_identity_scope_for(const GeminiIdentity& identity,
                                      std::string_view url) {
  std::string best;
  for (const auto& scope : identity.scopes)
    if (scope.size() > best.size() && gemini_scope_covers(scope, url))
      best = scope;
  return best;
}

const GeminiIdentity* gemini_identity_for(
    const std::vector<GeminiIdentity>& identities, std::string_view url) {
  const GeminiIdentity* best = nullptr;
  size_t best_len = 0;
  for (const auto& id : identities) {
    const std::string scope = gemini_identity_scope_for(id, url);
    if (!scope.empty() && scope.size() > best_len) {
      best = &id;
      best_len = scope.size();
    }
  }
  return best;
}

std::vector<GeminiIdentity> gemini_identities_list(std::string_view directory,
                                                   std::string* err) {
  const std::lock_guard guard(store_mutex());
  std::vector<GeminiIdentity> ids;
  if (!load(directory, ids, err)) return {};
  return ids;
}

std::optional<GeminiIdentity> gemini_identity_for_url(
    std::string_view directory, std::string_view url) {
  const std::lock_guard guard(store_mutex());
  std::vector<GeminiIdentity> ids;
  if (!load(directory, ids, nullptr)) return std::nullopt;
  const GeminiIdentity* id = gemini_identity_for(ids, url);
  if (!id) return std::nullopt;
  return *id;
}

bool gemini_identity_add(std::string_view directory, bool persist,
                         GeminiIdentity identity, std::string* err) {
  if (!is_sha256(identity.sha256) || identity.cert_pem.empty() ||
      identity.key_pem.empty())
    return fail(err, "incomplete identity");
  return mutate(directory, persist, err, [&](auto& ids) {
    if (find(ids, identity.sha256))
      return fail(err, "this certificate is already an identity");
    identity.saved = persist && !directory.empty() &&
                     !session().count(std::string(directory));
    ids.push_back(std::move(identity));
    return true;
  });
}

bool gemini_identity_remove(std::string_view directory, bool persist,
                            std::string_view sha256, std::string* err) {
  return mutate(directory, persist, err, [&](auto& ids) {
    const auto it = std::find_if(ids.begin(), ids.end(), [&](const auto& id) {
      return id.sha256 == sha256;
    });
    if (it == ids.end()) return fail(err, "no such identity");
    ids.erase(it);
    return true;
  });
}

bool gemini_identity_rename(std::string_view directory, bool persist,
                            std::string_view sha256, std::string_view name,
                            std::string* err) {
  return mutate(directory, persist, err, [&](auto& ids) {
    GeminiIdentity* id = find(ids, sha256);
    if (!id) return fail(err, "no such identity");
    id->name = std::string(name);
    return true;
  });
}

bool gemini_identity_use(std::string_view directory, bool persist,
                         std::string_view sha256, std::string_view scope,
                         std::string* err) {
  const std::string canonical = gemini_scope_of_page(scope);
  // A site scope keeps its trailing "/"; a page scope has none to lose.
  if (canonical.empty()) return fail(err, "not a gemini:// address");
  return mutate(directory, persist, err, [&](auto& ids) {
    GeminiIdentity* target = find(ids, sha256);
    if (!target) return fail(err, "no such identity");
    for (auto& id : ids)
      std::erase(id.scopes, canonical);
    target->scopes.push_back(canonical);
    std::sort(target->scopes.begin(), target->scopes.end());
    return true;
  });
}

bool gemini_identity_drop_scope(std::string_view directory, bool persist,
                                std::string_view sha256, std::string_view scope,
                                std::string* err) {
  return mutate(directory, persist, err, [&](auto& ids) {
    GeminiIdentity* id = find(ids, sha256);
    if (!id) return fail(err, "no such identity");
    std::erase(id->scopes, std::string(scope));
    return true;
  });
}

bool gemini_identity_stop_using(std::string_view directory, bool persist,
                                std::string_view url, std::string* err) {
  return mutate(directory, persist, err, [&](auto& ids) {
    for (auto& id : ids)
      std::erase_if(id.scopes, [&](const std::string& scope) {
        return gemini_scope_covers(scope, url);
      });
    return true;
  });
}

std::string gemini_identities_serialize(const std::vector<GeminiIdentity>& ids) {
  std::string out =
      "; xanatoo Gemini identities: client certificates and their PRIVATE "
      "KEYS.\n; Whoever has this file can log in as you. Keep it private.\n";
  for (const auto& id : ids) {
    out += "(identity\n  (sha256 ";
    quote(out, id.sha256);
    out += ")\n  (name ";
    quote(out, id.name);
    out += ")\n  (created ";
    quote(out, id.created);
    out += ")\n";
    for (const auto& scope : id.scopes) {
      out += "  (use ";
      quote(out, scope);
      out += ")\n";
    }
    out += "  (certificate ";
    quote(out, id.cert_pem);
    out += ")\n  (key ";
    quote(out, id.key_pem);
    out += "))\n";
  }
  return out;
}

std::optional<std::vector<GeminiIdentity>> gemini_identities_parse(
    std::string_view text, std::string* err) {
  std::vector<GeminiIdentity> out;
  for (const SExpr& form : SExprParser(text).parse_all()) {
    if (!is_form(form, "identity")) continue;  // other/future forms
    GeminiIdentity id;
    for (size_t i = 1; i < form.kids.size(); ++i) {
      const SExpr& f = form.kids[i];
      if (f.kind != SExpr::Kind::List || f.kids.size() != 2) continue;
      const std::string value = sexpr_text(f.kids[1]);
      if (is_form(f, "sha256")) id.sha256 = value;
      else if (is_form(f, "name")) id.name = value;
      else if (is_form(f, "created")) id.created = value;
      else if (is_form(f, "use")) id.scopes.push_back(value);
      else if (is_form(f, "certificate")) id.cert_pem = value;
      else if (is_form(f, "key")) id.key_pem = value;
    }
    if (!is_sha256(id.sha256) || id.cert_pem.empty() || id.key_pem.empty()) {
      fail(err, "malformed identity entry");
      return std::nullopt;
    }
    if (std::any_of(out.begin(), out.end(), [&](const auto& other) {
          return other.sha256 == id.sha256;
        })) {
      fail(err, "identity " + id.sha256.substr(0, 16) + "… listed twice");
      return std::nullopt;
    }
    out.push_back(std::move(id));
  }
  return out;
}

}  // namespace xanatoo
