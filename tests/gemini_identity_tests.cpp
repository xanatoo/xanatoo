// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
// Gemini identities (client certificates): scopes, store format and the
// --no-save session overlay. No SDL, TLS or network.

#include "xanatoo/gemini_identity.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

xanatoo::GeminiIdentity make(char fill, std::string name) {
  xanatoo::GeminiIdentity id;
  id.sha256 = std::string(64, fill);
  id.name = std::move(name);
  id.created = "2026-11-21";
  id.cert_pem = "-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n";
  id.key_pem = "-----BEGIN PRIVATE KEY-----\n\"q\\\"\n-----END PRIVATE KEY-----\n";
  return id;
}

void scopes() {
  using namespace xanatoo;
  check(gemini_scope_of_site("gemini://Station.MartinRue.com/join?x") ==
            "gemini://station.martinrue.com/", "site scope");
  check(gemini_scope_of_page("gemini://station.martinrue.com:1965/join?x") ==
            "gemini://station.martinrue.com/join", "page scope drops query/port");
  check(gemini_scope_of_page("gemini://h:1966") == "gemini://h:1966/",
        "page scope keeps other port");
  check(gemini_scope_of_page("https://example.org/").empty(), "not gemini");
  check(gemini_scope_covers("gemini://h/a", "gemini://h/a"), "same page");
  check(gemini_scope_covers("gemini://h/a", "gemini://h/a/b"), "below page");
  check(gemini_scope_covers("gemini://h/a", "gemini://h/a?q"), "query");
  check(!gemini_scope_covers("gemini://h/a", "gemini://h/ab"), "segment edge");
  check(gemini_scope_covers("gemini://h/", "gemini://H/anything"), "site");
  check(!gemini_scope_covers("gemini://h/", "gemini://h:1966/"), "other port");
  check(!gemini_scope_covers("gemini://h/", "gemini://hh/"), "other host");

  std::vector<GeminiIdentity> ids{make('a', "site"), make('b', "page")};
  ids[0].scopes = {"gemini://h/"};
  ids[1].scopes = {"gemini://h/app"};
  check(gemini_identity_for(ids, "gemini://h/app/x")->name == "page",
        "longest scope wins");
  check(gemini_identity_for(ids, "gemini://h/other")->name == "site",
        "site scope otherwise");
  check(gemini_identity_for(ids, "gemini://elsewhere/") == nullptr, "none");
}

void format() {
  using namespace xanatoo;
  std::vector<GeminiIdentity> ids{make('a', "Alice \"A\"\n"), make('b', "Bob")};
  ids[0].scopes = {"gemini://h/", "gemini://h/x"};
  const std::string text = gemini_identities_serialize(ids);
  std::string err;
  const auto back = gemini_identities_parse(text, &err);
  check(back.has_value(), "parse back");
  check(back->size() == 2, "two identities");
  check((*back)[0].name == ids[0].name, "name escapes");
  check((*back)[0].key_pem == ids[0].key_pem, "key round trip");
  check((*back)[0].scopes == ids[0].scopes, "scopes round trip");
  check((*back)[1].cert_pem == ids[1].cert_pem, "cert round trip");
  check(!gemini_identities_parse("(identity (name \"x\"))", &err), "incomplete");
}

void store() {
  namespace fs = std::filesystem;
  using namespace xanatoo;
  const fs::path dir = fs::temp_directory_path() / ("xanatoo-gemini-id-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  check(fs::create_directory(dir), "temp dir");
  const std::string d = dir.string();
  std::string err;
  check(gemini_identity_add(d, true, make('a', "A"), &err), "add a");
  check(gemini_identity_add(d, true, make('b', "B"), &err), "add b");
  check(!gemini_identity_add(d, true, make('b', "B2"), &err), "dup refused");
#ifndef _WIN32
  const auto perms = fs::status(dir / "gemini-identities").permissions();
  check((perms & (fs::perms::group_all | fs::perms::others_all)) ==
            fs::perms::none, "store is owner-only");
#endif
  const std::string a(64, 'a'), b(64, 'b');
  check(gemini_identity_use(d, true, a, "gemini://h/", &err), "use a");
  check(gemini_identity_for_url(d, "gemini://h/x")->name == "A", "a here");
  // Switching: the same scope moves to b.
  check(gemini_identity_use(d, true, b, "gemini://h/", &err), "use b");
  check(gemini_identity_for_url(d, "gemini://h/x")->name == "B", "b here");
  check(gemini_identities_list(d)[0].scopes.empty(), "a lost the scope");
  check(gemini_identity_rename(d, true, b, "Bee", &err), "rename");
  check(gemini_identity_stop_using(d, true, "gemini://h/x", &err), "stop");
  check(!gemini_identity_for_url(d, "gemini://h/x"), "none after stop");
  check(gemini_identity_remove(d, true, a, &err), "remove");
  auto list = gemini_identities_list(d, &err);
  check(list.size() == 1 && list[0].name == "Bee" && list[0].saved, "saved");
  // --no-save: changes stay in this process, the file keeps its content.
  check(gemini_identity_add(d, false, make('c', "C"), &err), "session add");
  check(gemini_identities_list(d).size() == 2, "session sees both");
  check(!gemini_identities_list(d)[1].saved, "session-only");
  fs::remove_all(dir);
}

}  // namespace

int main() {
  try {
    scopes();
    format();
    store();
  } catch (const std::exception& e) {
    std::cerr << "gemini_identity_tests: " << e.what() << "\n";
    return 1;
  }
  std::cout << "gemini_identity_tests: ok\n";
  return 0;
}
