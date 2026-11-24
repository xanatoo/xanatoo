// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/nostr_keys.hpp"

#include "xanatoo/sexpr.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

namespace xanatoo {
namespace {

namespace fs = std::filesystem;

constexpr const char* kStoreName = "nostr-keys";

bool fail(std::string* err, std::string msg) {
  if (err) *err = std::move(msg);
  return false;
}

// Exactly one of the two forms of the secret.
bool well_formed(const NostrKey& k) {
  const bool enc = k.ncryptsec.rfind("ncryptsec1", 0) == 0 && k.nsec.empty();
  const bool plain = k.ncryptsec.empty() && k.nsec.rfind("nsec1", 0) == 0;
  return enc || plain;
}

bool is_hex64(std::string_view s) {
  return s.size() == 64 &&
         std::all_of(s.begin(), s.end(), [](unsigned char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

std::mutex& store_mutex() {
  static std::mutex mutex;
  return mutex;
}

fs::path store_path(std::string_view directory) {
  return fs::path(directory) / kStoreName;
}

bool load(std::string_view directory, std::vector<NostrKey>& out,
          std::string* err) {
  out.clear();
  const fs::path path = store_path(directory);
  std::error_code ec;
  if (!fs::exists(path, ec)) return true;
  std::ifstream in(path, std::ios::binary);
  if (!in) return fail(err, "cannot read " + path.string());
  std::ostringstream text;
  text << in.rdbuf();
  auto parsed = nostr_keys_parse(text.str(), err);
  if (!parsed) {
    if (err) *err = path.string() + ": " + *err;
    return false;
  }
  out = std::move(*parsed);
  return true;
}

// Owner-only temporary file, then renamed over the store.
bool save(std::string_view directory, const std::vector<NostrKey>& keys,
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
    std::ofstream create(temporary, std::ios::binary | std::ios::trunc);
    if (!create) return fail(err, "cannot create a temporary key store");
  }
#ifndef _WIN32
  fs::permissions(temporary, fs::perms::owner_read | fs::perms::owner_write,
                  fs::perm_options::replace, ec);
  if (ec) {
    std::error_code ignored;
    fs::remove(temporary, ignored);
    return fail(err, "cannot protect the key store: " + ec.message());
  }
#endif
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out << nostr_keys_serialize(keys);
    out.close();
    if (!out) {
      fs::remove(temporary, ec);
      return fail(err, "cannot write the key store");
    }
  }
  fs::rename(temporary, path, ec);
  if (ec) {
    std::error_code ignored;
    fs::remove(temporary, ignored);
    return fail(err, "cannot update the key store: " + ec.message());
  }
  return true;
}

void quote(std::string& out, std::string_view s) {
  out += '"';
  for (char c : s) {
    if (c == '\\' || c == '"') out += '\\';
    if (c == '\n') { out += "\\n"; continue; }
    out += c;
  }
  out += '"';
}

}  // namespace

std::string nostr_keys_serialize(const std::vector<NostrKey>& keys) {
  std::string out =
      "; xanatoo Nostr signing keys: NIP-49 encrypted (ncryptsec), each\n"
      "; opened by its password, or a plain nsec for a key kept without one\n"
      "; (anyone who reads an nsec can post as that account). Without this\n"
      "; file the accounts are lost. Keep a backup somewhere safe.\n";
  for (const auto& k : keys) {
    out += "(key\n  (pubkey ";
    quote(out, k.pubkey);
    out += ")\n  (created ";
    quote(out, k.created);
    if (nostr_key_plain(k)) {
      out += ")\n  (nsec ";
      quote(out, k.nsec);
    } else {
      out += ")\n  (ncryptsec ";
      quote(out, k.ncryptsec);
    }
    out += "))\n";
  }
  return out;
}

std::optional<std::vector<NostrKey>> nostr_keys_parse(std::string_view text,
                                                      std::string* err) {
  std::vector<NostrKey> out;
  for (const SExpr& form : SExprParser(text).parse_all()) {
    if (!is_form(form, "key")) continue;  // other/future forms
    NostrKey k;
    for (size_t i = 1; i < form.kids.size(); ++i) {
      const SExpr& f = form.kids[i];
      if (f.kind != SExpr::Kind::List || f.kids.size() != 2) continue;
      const std::string value = sexpr_text(f.kids[1]);
      if (is_form(f, "pubkey")) k.pubkey = value;
      else if (is_form(f, "created")) k.created = value;
      else if (is_form(f, "ncryptsec")) k.ncryptsec = value;
      else if (is_form(f, "nsec")) k.nsec = value;
    }
    if (!is_hex64(k.pubkey) || !well_formed(k)) {
      fail(err, "malformed key entry");
      return std::nullopt;
    }
    if (std::any_of(out.begin(), out.end(),
                    [&](const auto& o) { return o.pubkey == k.pubkey; })) {
      fail(err, "key " + k.pubkey.substr(0, 16) + "… listed twice");
      return std::nullopt;
    }
    out.push_back(std::move(k));
  }
  return out;
}

std::vector<NostrKey> nostr_keys_list(std::string_view directory,
                                      std::string* err) {
  std::vector<NostrKey> keys;
  if (directory.empty()) return keys;
  const std::lock_guard guard(store_mutex());
  load(directory, keys, err);
  return keys;
}

bool nostr_keys_put(std::string_view directory, NostrKey key, std::string* err) {
  if (directory.empty()) return fail(err, "no state directory to keep the key in");
  if (!is_hex64(key.pubkey) || !well_formed(key))
    return fail(err, "malformed key");
  const std::lock_guard guard(store_mutex());
  std::vector<NostrKey> keys;
  if (!load(directory, keys, err)) return false;
  auto it = std::find_if(keys.begin(), keys.end(),
                         [&](const auto& k) { return k.pubkey == key.pubkey; });
  if (it != keys.end()) *it = std::move(key);
  else keys.push_back(std::move(key));
  return save(directory, keys, err);
}

bool nostr_keys_remove(std::string_view directory, std::string_view pubkey,
                       std::string* err) {
  if (directory.empty()) return fail(err, "no state directory");
  const std::lock_guard guard(store_mutex());
  std::vector<NostrKey> keys;
  if (!load(directory, keys, err)) return false;
  const auto n = keys.size();
  keys.erase(std::remove_if(keys.begin(), keys.end(),
                            [&](const auto& k) { return k.pubkey == pubkey; }),
             keys.end());
  if (keys.size() == n) return fail(err, "no such key");
  return save(directory, keys, err);
}

}  // namespace xanatoo
