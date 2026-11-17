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
    CHECK(!u->prompt);
  }
  {
    // "=:" prompt links end in an empty '?'; the query is sent decoded.
    auto p = parse_spartan_url("spartan://example.com/search?");
    CHECK(p && p->prompt && p->path == "/search" && p->data.empty());
    CHECK(p->canonical == "spartan://example.com/search?");
    auto q = parse_spartan_url("spartan://example.com/search?hello%20world%2");
    CHECK(q && !q->prompt && q->query == "hello%20world%2");
    CHECK(q->data == "hello world%2");
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
  {
    // '?' is part of non-search selectors; selectors are percent-coded.
    auto u = parse_gopher_url("gopher://example.com/0/cgi?x=1%20y");
    CHECK(u && u->type == '0' && u->selector == "/cgi?x=1 y" && u->query.empty());
    CHECK(u->canonical == "gopher://example.com/0/cgi%3Fx=1%20y");
    auto again = parse_gopher_url(u->canonical);
    CHECK(again && again->selector == u->selector);
    // RFC 4266 search form: %09 between selector and search.
    auto s = parse_gopher_url("gopher://example.com/7/find%09two%20words");
    CHECK(s && s->selector == "/find" && s->query == "two words");
    // A query on a search keeps the selector's encoded '?'.
    auto q = parse_gopher_url("gopher://example.com/7/a%3Fb?c d");
    CHECK(q && q->selector == "/a?b" && q->query == "c d");
    CHECK(q->canonical == "gopher://example.com/7/a%3Fb?c%20d");
  }
  {
    // Relative references.
    const std::string base = "gopher://example.com/h/site/index.html";
    CHECK(resolve_gopher_url(base, "about.html") ==
          "gopher://example.com/h/site/about.html");
    CHECK(resolve_gopher_url(base, "/0readme.txt") ==
          "gopher://example.com/0readme.txt");
    CHECK(resolve_gopher_url("gopher://example.com/7/find", "?x") ==
          "gopher://example.com/7/find?x");
    CHECK(resolve_gopher_url(base, "https://example.org/") ==
          "https://example.org/");
  }
  {
    // Info lines stay preformatted (one block per run, backticks safe),
    // labels are escaped, selectors encoded, h "URL:" items are web links.
    auto items = parse_gopher_menu(
        "i  /\\_/\\   ```cat```\t\terror.host\t1\r\n"
        "i ( o.o )\t\terror.host\t1\r\n"
        "0My *notes* [draft](x)\t/notes/my file.txt\texample.com\t70\r\n"
        "hWeb site\tURL:https://example.org/a b\texample.com\t70\r\n"
        "1Local\t/sub\t\t\r\n");
    auto md = gopher_menu_to_markdown(items, "gopher://example.com:7070/1/");
    CHECK(md.find("````\n  /\\_/\\   ```cat```\n ( o.o )\n````\n") !=
          std::string::npos);
    CHECK(md.find("[My *notes* [draft] (x) [text]]"
                  "(gopher://example.com/0/notes/my%20file.txt)") !=
          std::string::npos);
    CHECK(md.find("[Web site [html]](https://example.org/a%20b)") !=
          std::string::npos);
    CHECK(md.find("(gopher://example.com:7070/1/sub)") != std::string::npos);
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
