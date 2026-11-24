// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Signing keys of the user's own Nostr accounts: state_dir()/nostr-keys, an
// owner-only S-expression file. Each key is either NIP-49 ncryptsec
// (password-encrypted) or, for a key the user chose to keep without a
// password, a plain nsec (protected only by the file's permissions); the
// label and the active account stay in nostr.sqlite (nostr_store.hpp). Kept
// apart from that cache so clearing the cache never loses a key.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xanatoo {

struct NostrKey {
  std::string pubkey;     // 64 lower-case hex
  std::string ncryptsec;  // ncryptsec1…, or empty for a password-less key
  std::string created;    // YYYY-MM-DD
  std::string nsec;       // nsec1… of a password-less key (else empty)
};

// Without a password: the store holds the plain nsec.
inline bool nostr_key_plain(const NostrKey& k) { return k.ncryptsec.empty(); }

std::string nostr_keys_serialize(const std::vector<NostrKey>& keys);
std::optional<std::vector<NostrKey>> nostr_keys_parse(std::string_view text,
                                                      std::string* err = nullptr);

// The keys in `directory` (empty list when there is no file yet).
std::vector<NostrKey> nostr_keys_list(std::string_view directory,
                                      std::string* err = nullptr);
// Adds `key`, or replaces the one with its public key.
bool nostr_keys_put(std::string_view directory, NostrKey key,
                    std::string* err = nullptr);
bool nostr_keys_remove(std::string_view directory, std::string_view pubkey,
                       std::string* err = nullptr);

}  // namespace xanatoo
