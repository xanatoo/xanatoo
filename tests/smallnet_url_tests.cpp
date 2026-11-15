// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
// Unit tests for Gopher / Spartan / Nex / Finger URL parsers and menu conversion.

#include "xanatoo/gopher.hpp"
#include "xanatoo/spartan.hpp"
#include "xanatoo/nex.hpp"
#include "xanatoo/finger.hpp"

#include <cassert>
#include <iostream>
#include <string>

using namespace xanatoo;

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << " " #cond "\n"; ++fails; } } while (0)

int main() {
  // Gopher
  CHECK(is_gopher_url("gopher://example.com/1/"));
  CHECK(is_gopher_url("Gopher://HOST/0file.txt"));
  CHECK(!is_gopher_url("gemini://x"));
  {
    auto u = parse_gopher_url("gopher://floodgap.com/1/");
    CHECK(u.has_value());
    CHECK(u->host == "floodgap.com");
    CHECK(u->port == 70);
    CHECK(u->type == '1');
    CHECK(u->selector.empty() || u->selector == "/");
  }
  {
    auto u = parse_gopher_url("gopher://example.com:7070/0readme");
    CHECK(u.has_value());
    CHECK(u->port == 7070);
    CHECK(u->type == '0');
    CHECK(u->selector == "readme");
  }
  {
    auto items = parse_gopher_menu(
        "iWelcome\t\texample.com\t70\r\n"
        "0About\tabout.txt\texample.com\t70\r\n"
        "1Docs\tdocs\texample.com\t70\r\n"
        ".\r\n");
    CHECK(items.size() == 3);
    CHECK(items[0].type == 'i');
    CHECK(items[1].type == '0');
    CHECK(items[1].label == "About");
    CHECK(items[1].selector == "about.txt");
    auto md = gopher_menu_to_markdown(items, "gopher://example.com/1/");
    CHECK(md.find("[About") != std::string::npos);
    CHECK(md.find("gopher://example.com") != std::string::npos);
  }

  // Spartan
  CHECK(is_spartan_url("spartan://example.com/"));
  CHECK(!is_spartan_url("gemini://x"));
  {
    auto u = parse_spartan_url("spartan://mozz.us/files/about.txt");
    CHECK(u.has_value());
    CHECK(u->host == "mozz.us");
    CHECK(u->port == 300);
    CHECK(u->path == "/files/about.txt");
  }

  // Nex
  CHECK(is_nex_url("nex://nightfall.city/"));
  CHECK(is_nex_url("nex://host/path"));
  {
    auto u = parse_nex_url("nex://nightfall.city/nex/info/");
    CHECK(u.has_value());
    CHECK(u->host == "nightfall.city");
    CHECK(u->port == 1900);
    CHECK(u->path == "/nex/info/");
  }
  {
    auto md = nex_directory_to_markdown(
        "Welcome\n=> about.txt About\n=> nex://other/\n",
        "nex://example.com/");
    CHECK(md.find("[About]") != std::string::npos);
  }

  // Finger
  CHECK(is_finger_url("finger://example.com"));
  CHECK(is_finger_url("finger://user@example.com"));
  {
    auto u = parse_finger_url("finger://alice@example.com");
    CHECK(u.has_value());
    CHECK(u->host == "example.com");
    CHECK(u->user == "alice");
    CHECK(u->port == 79);
  }
  {
    auto u = parse_finger_url("finger://example.com/bob");
    CHECK(u.has_value());
    CHECK(u->user == "bob");
  }


  // Gopher type-7 query
  {
    auto u = parse_gopher_url("gopher://example.com/7/search?widgets");
    CHECK(u.has_value());
    CHECK(u->type == '7');
    CHECK(u->selector == "search" || u->selector == "/search");
    CHECK(u->query == "widgets");
  }
  CHECK(gopher_type_is_binary('9'));
  CHECK(gopher_type_is_binary('g'));
  CHECK(!gopher_type_is_binary('0'));
  CHECK(!gopher_type_is_binary('1'));

  if (fails) {
    std::cerr << fails << " failure(s)\n";
    return 1;
  }
  std::cout << "smallnet_url_tests: ok\n";
  return 0;
}
