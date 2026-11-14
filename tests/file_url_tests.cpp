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

}
