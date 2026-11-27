// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/http_cookies.hpp"
#include "xanatoo/paths.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <sys/stat.h>

void test_http_cookies(int& passed, int& failed) {
  using namespace xanatoo;
  auto expect = [&](bool ok, const char* description) {
    if (ok) ++passed;
    else {
      ++failed;
      std::fprintf(stderr, "FAIL HTTP cookies: %s\n", description);
    }
  };
  std::string dir = (std::filesystem::temp_directory_path() /
                     "xanatoo-cookies-XXXXXX").string();
  if (!::mkdtemp(dir.data())) {
    expect(false, "create isolated state directory");
    return;
  }
  const auto previous = dir_info(DirKind::State);
  set_dir_override(DirKind::State, dir);

  auto header = [](std::string_view url) {
    return http_cookie_header(*parse_http_url(url));
  };
  const auto url = *parse_http_url("https://old.example.test/account/profile?q=1");
  expect(import_http_cookie("https://OLD.example.test:443/", "sid=root=="),
         "import one cookie and normalize origin");
  expect(import_http_cookie("https://old.example.test/account", "sid=narrow"),
         "import same name with a different path");
  expect(!import_http_cookie("https://old.example.test/", "x=1; y=2"),
         "reject multiple cookies");
  expect(!import_http_cookie("https://old.example.test/", "x=a\r\nb=c"),
         "reject header injection");
  expect(!import_http_cookie("http://old.example.test/", "x=unsafe"),
         "reject plaintext scope");
  expect(header("https://old.example.test/account/profile?q=1") ==
         "sid=narrow; sid=root==", "send narrower path first");
  expect(header("https://old.example.test/accounts") == "sid=root==",
         "require a slash at a path prefix boundary");
  for (const auto unrelated : {"http://old.example.test/account",
                               "https://old.example.test:8443/account",
                               "https://sub.old.example.test/account",
                               "https://example.test/account"})
    expect(header(unrelated).empty(), "isolate scheme, port and host");

  struct stat st {};
  expect(::stat((dir + "/cookies.sqlite").c_str(), &st) == 0 &&
         (st.st_mode & 0777) == 0600, "database is private");

  receive_http_cookies(url, "HTTP/1.1 200 OK\r\n"
      "Set-Cookie: short=one; Secure\r\n"
      "Set-Cookie: persisted=two; Path=/; Max-Age=60; Domain=.example.test\r\n"
      "Set-Cookie: foreign=unwanted; Domain=unrelated.test\r\n"
      "Set-Cookie: expired=gone; Max-Age=0\r\n"
      "Set-Cookie: too-old=gone; Expires=Thu, 01 Jan 1970 00:00:00 GMT\r\n"
      "Set-Cookie: partition=no; Secure; Partitioned");
  const auto received = http_cookie_header(url);
  expect(received.find("short=one") != received.npos, "accept a session cookie");
  expect(received.find("persisted=two") != received.npos, "accept a persistent cookie");
  expect(received.find("foreign=") == received.npos && received.find("expired=") == received.npos &&
         received.find("too-old=") == received.npos && received.find("partition=") == received.npos,
         "exclude invalid, partitioned and expired cookies");
  expect(header("https://old.example.test/").find("short=") == std::string::npos,
         "use response URL's directory as the default path");
  expect(header("https://example.test/").empty(), "never widen a server cookie's origin");

  expect(!http_cookies_allowed(), "background HTTP requests are outside the cookie scope");
  bool sent = false;
  {
    HttpCookieScope scope(sent);
    expect(http_cookies_allowed(), "foreground cookie scope enabled");
    record_http_cookies(url, received);
  }
  expect(sent && !http_cookies_allowed(), "successful send marks the fetch");
  auto snapshot = http_cookie_snapshot();
  expect(snapshot.sites.size() == 1 && snapshot.sites[0].last_path == "/account/profile" &&
         snapshot.sites[0].last_names.find("persisted") != std::string::npos &&
         snapshot.sites[0].last_names.find("two") == std::string::npos,
         "audit keeps path and names with values and query omitted");

  const auto narrow = std::ranges::find(snapshot.cookies, "/account", &HttpCookie::path);
  if (narrow == snapshot.cookies.end()) {
    expect(false, "inspect path-specific cookie");
  } else {
    expect(edit_http_cookie(*narrow, false, false), "disable one cookie");
    receive_http_cookies(url, "HTTP/1.1 200 OK\r\nSet-Cookie: sid=renewed; Path=/account");
    expect(http_cookie_header(url).find("sid=narrow") == received.npos &&
           http_cookie_header(url).find("sid=renewed") == received.npos,
           "server response cannot re-enable a disabled cookie");
    expect(edit_http_cookie(*narrow, false, true) &&
           http_cookie_header(url).find("sid=narrow") != received.npos, "re-enable cookie");
    expect(edit_http_cookie(*narrow, true, false) &&
           http_cookie_header(url).find("sid=narrow") == received.npos, "delete cookie");
  }
  receive_http_cookies(url, "HTTP/1.1 200 OK\r\nSet-Cookie: short=; Max-Age=0");
  expect(http_cookie_header(url).find("short=") == received.npos, "expire an existing session cookie");
  expect(set_cookie_site(cookie_origin(url), false) && http_cookie_header(url).empty(),
         "turn off all cookies for a site");
  receive_http_cookies(url, "HTTP/1.1 200 OK\r\nSet-Cookie: blocked=one");
  expect(set_cookie_site(cookie_origin(url), true) &&
         http_cookie_header(url).find("blocked=") == received.npos,
         "decline cookies received while site is disabled");

  const bool previous_no_save = no_save();
  set_no_save(true);
  expect(http_cookie_header(url).empty() &&
         !import_http_cookie("https://old.example.test/", "sid=blocked"),
         "do not use or mutate cookie state under --no-save");
  set_no_save(previous_no_save);
  set_dir_override(DirKind::State, previous.source == "option" ? previous.path : "");
  std::filesystem::remove_all(dir);
}
