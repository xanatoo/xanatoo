// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/pseudo_favicon.hpp"

#include <cassert>

int main() {
  using xanatoo::pseudo_favicon;
  assert(pseudo_favicon("") == "site-file");
  assert(pseudo_favicon("untitled") == "site-file");
  assert(pseudo_favicon("/home/u/notes.md") == "site-file");
  assert(pseudo_favicon("/home/u/docs", true) == "site-folder");
  assert(pseudo_favicon("examples/a:b.md") == "site-file");
  assert(pseudo_favicon("C:\\Users\\u\\a.md") == "site-file");
  assert(pseudo_favicon("file:///tmp/x.md") == "site-file");
  assert(pseudo_favicon("about:") == "site-about");
  assert(pseudo_favicon("About:") == "site-about");
  assert(pseudo_favicon("drafts:") == "site-store");
  assert(pseudo_favicon("db:abc") == "site-store");
  assert(pseudo_favicon("search:hn:rust") == "search");
  assert(pseudo_favicon("sitemap:gnu.org") == "sitemap");
  assert(pseudo_favicon("feed:https://a.org/x") == "site-feed");
  assert(pseudo_favicon("gemini://geminiprotocol.net/") == "gemini");
  assert(pseudo_favicon("nostr:npub1xyz") == "site-nostr");
  assert(pseudo_favicon("note1abc") == "site-nostr");
  assert(pseudo_favicon("https://example.org/") == "site-web");
  assert(pseudo_favicon("hn:item?id=1") == "site-web");
  assert(pseudo_favicon("wayback:https://a.org/") == "site-web");
}
