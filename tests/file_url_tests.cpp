// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/file_url.hpp"

#include <cassert>
#include <string>

int main() {
  using namespace xanatoo;
  assert(file_url_path("file:///tmp/a.md") == "/tmp/a.md");
  assert(file_url_path("file://localhost/tmp/a.md") == "/tmp/a.md");
  assert(file_url_path("file:/tmp/a.md") == "/tmp/a.md");
  assert(file_url_path("file:///tmp/a%20b%23c.md") == "/tmp/a b#c.md");
  assert(file_url_path("file:///dir/") == "/dir/");
  assert(file_url_path("file://remote/x").empty());
  assert(file_url_path("file:///%GG").empty());
  assert(file_url_path("file:///a%00b").empty());
  assert(file_url_path("https://x/").empty());

  assert(file_url("/tmp/a.md") == "file:///tmp/a.md");
  assert(file_url("/tmp/a b#c?.md") == "file:///tmp/a%20b%23c%3F.md");
  assert(file_url("/tmp/100%.md") == "file:///tmp/100%25.md");
  assert(file_url("/tmp/Grüße.md") == "file:///tmp/Grüße.md");
  assert(file_url("C:/x/y.md") == "file:///C:/x/y.md");
  for (const char* p : {"/tmp/a b#c?.md", "/tmp/100%.md", "/d/", "/"})
    assert(file_url_path(file_url(p)) == p);

  assert(looks_like_local_path("/a"));
  assert(looks_like_local_path("examples/a.md"));
  assert(looks_like_local_path("./a:b"));
  assert(looks_like_local_path("C:\\x"));
  for (const char* s : {"", "about:", "db:doc:1", "https://x/", "file:///a",
                        "hn:item?id=1", "drafts:"})
    assert(!looks_like_local_path(s));

  // URL bar: explicit paths → file://, a scheme stays, the rest is https.
  const char* cwd = "/home/u/docs";
  assert(complete_typed_address("./a.md", cwd) == "file:///home/u/docs/a.md");
  assert(complete_typed_address("../b/a.md", cwd) == "file:///home/u/b/a.md");
  assert(complete_typed_address("/tmp/a b.md", cwd) == "file:///tmp/a%20b.md");
  assert(complete_typed_address("./a.md#intro", cwd) ==
         "file:///home/u/docs/a.md#intro");
  assert(complete_typed_address("./sub/", cwd) == "file:///home/u/docs/sub/");
  assert(complete_typed_address(".", cwd) == "file:///home/u/docs/");
  assert(complete_typed_address("..", cwd) == "file:///home/u/");
  assert(complete_typed_address("/", cwd) == "file:///");
  assert(complete_typed_address("example.com", cwd) == "https://example.com");
  assert(complete_typed_address("example.com/a?b=1#c", cwd) ==
         "https://example.com/a?b=1#c");
  assert(complete_typed_address("localhost:8080", cwd) ==
         "https://localhost:8080");
  assert(complete_typed_address("example.com:443/x", cwd) ==
         "https://example.com:443/x");
  for (const char* s : {"https://x/", "http://x/", "gemini://x/", "file:///a",
                        "about:", "db:doc:1", "hn:item?id=1", "mailto:a@b",
                        "~/a.md", "#intro", "", "two words"})
    assert(complete_typed_address(s, cwd) == s);

}
