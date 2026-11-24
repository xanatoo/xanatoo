// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
// Nostr key store (state_dir()/nostr-keys): format, add / replace / remove,
// owner-only permissions. No SDL, crypto or network.

#include "xanatoo/nostr_keys.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

xanatoo::NostrKey make(char fill) {
  return {std::string(64, fill), std::string("ncryptsec1") + fill + "qqqq",
          "2026-11-24"};
}

}  // namespace

int main() {
  using namespace xanatoo;
  namespace fs = std::filesystem;
  try {
    const auto text = nostr_keys_serialize({make('a'), make('b')});
    auto parsed = nostr_keys_parse(text);
    check(parsed && parsed->size() == 2, "round trip");
    check((*parsed)[1].pubkey == std::string(64, 'b') &&
              (*parsed)[1].ncryptsec == "ncryptsec1bqqqq" &&
              (*parsed)[1].created == "2026-11-24", "fields");
    check(!nostr_keys_parse("(key (pubkey \"xyz\") (ncryptsec \"ncryptsec1q\"))"),
          "bad pubkey rejected");
    check(!nostr_keys_parse("(key (pubkey \"" + std::string(64, 'a') +
                            "\") (ncryptsec \"nsec1abc\"))"),
          "nsec in the ncryptsec field rejected");
    // Password-less keys: a plain (nsec …), never both forms.
    NostrKey plain{std::string(64, 'c'), "", "2026-11-25", "nsec1cqqqq"};
    auto with_plain = nostr_keys_parse(nostr_keys_serialize({make('a'), plain}));
    check(with_plain && with_plain->size() == 2 &&
              nostr_key_plain((*with_plain)[1]) &&
              (*with_plain)[1].nsec == "nsec1cqqqq" &&
              !nostr_key_plain((*with_plain)[0]), "plain key round trip");
    check(!nostr_keys_parse("(key (pubkey \"" + std::string(64, 'a') +
                            "\") (ncryptsec \"ncryptsec1q\") (nsec \"nsec1q\"))"),
          "both forms rejected");
    check(!nostr_keys_parse("(key (pubkey \"" + std::string(64, 'a') +
                            "\") (nsec \"ncryptsec1q\"))"),
          "ncryptsec in the nsec field rejected");
    check(!nostr_keys_parse("(key (pubkey \"" + std::string(64, 'a') + "\"))"),
          "no secret rejected");
    check(!nostr_keys_parse(nostr_keys_serialize({make('a'), make('a')})),
          "duplicate rejected");

    const fs::path dir = fs::temp_directory_path() /
        ("xanatoo-nostr-keys-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::string d = dir.string();
    check(nostr_keys_list(d).empty(), "no file yet");
    check(nostr_keys_put(d, make('a')), "put a");
    check(nostr_keys_put(d, make('b')), "put b");
    auto replaced = make('a');
    replaced.ncryptsec = "ncryptsec1zzzz";
    check(nostr_keys_put(d, replaced), "replace a");
    auto keys = nostr_keys_list(d);
    check(keys.size() == 2 && keys[0].ncryptsec == "ncryptsec1zzzz", "replaced");
    // A key's password removed, then set again: replaced in place.
    auto unprotected = make('b');
    unprotected.ncryptsec.clear();
    unprotected.nsec = "nsec1bqqqq";
    check(nostr_keys_put(d, unprotected), "put b without password");
    keys = nostr_keys_list(d);
    check(keys.size() == 2 && nostr_key_plain(keys[1]) &&
              keys[1].nsec == "nsec1bqqqq", "b password-less");
    check(nostr_keys_put(d, make('b')), "b with password again");
    keys = nostr_keys_list(d);
    check(keys.size() == 2 && !nostr_key_plain(keys[1]) && keys[1].nsec.empty(),
          "b encrypted again");
    auto both = make('b');
    both.nsec = "nsec1bqqqq";
    check(!nostr_keys_put(d, both), "both forms refused");
#ifndef _WIN32
    const auto perms = fs::status(dir / "nostr-keys").permissions();
    check((perms & (fs::perms::group_all | fs::perms::others_all)) ==
              fs::perms::none, "owner only");
#endif
    check(nostr_keys_remove(d, std::string(64, 'a')), "remove");
    check(!nostr_keys_remove(d, std::string(64, 'a')), "remove twice");
    check(nostr_keys_list(d).size() == 1, "one left");
    check(!nostr_keys_put("", make('c')), "no directory");
    fs::remove_all(dir);
  } catch (const std::exception& e) {
    std::cerr << "nostr_keys_tests: FAIL: " << e.what() << "\n";
    return 1;
  }
  std::cout << "nostr_keys_tests: PASS\n";
  return 0;
}
