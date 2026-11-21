// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
// Offline tests for the Internet Archive refs and Markdown mapping; replies
// are cut-down copies of what archive.org sends.
#include "xanatoo/archive.hpp"

#include <cassert>
#include <string>

using namespace xanatoo;

static bool has(const std::string& s, const char* part) {
  return s.find(part) != std::string::npos;
}

int main() {
  // Refs.
  assert(parse_ia_ref("ia:prelinger")->id == "prelinger");
  assert(parse_ia_ref("ia:details/foo_bar-1.2")->id == "foo_bar-1.2");
  assert(parse_ia_ref("ia:foo?page=3")->page == 3);
  assert(!parse_ia_ref("ia:"));
  assert(!parse_ia_ref("ia:a b"));
  assert(!parse_ia_ref("https://archive.org/details/x"));
  assert(ia_ref_string({"x", 2}) == "ia:x?page=2");
  assert(ia_ref_from_web_url("https://archive.org/details/foo?q=1") == "ia:foo");
  assert(ia_ref_from_web_url("https://archive.org/download/foo/a.pdf") == "ia:foo");
  assert(ia_ref_from_web_url("https://archive.org/search?query=x").empty());
  assert(ia_ref_from_web_url("https://example.com/details/foo").empty());
  assert(ia_web_url({"foo", 1}) == "https://archive.org/details/foo");

  // An item.
  const char* item = R"({"files":[
    {"name":"book.pdf","format":"Text PDF","source":"original","size":"2048"},
    {"name":"book_meta.xml","format":"Metadata","source":"original"},
    {"name":"book.torrent","format":"Archive BitTorrent"},
    {"name":"my book.epub","format":"EPUB","source":"derivative","size":"3145728"}],
   "metadata":{"identifier":"book","title":"A *Book* [1]","mediatype":"texts",
     "creator":["Doe, Jane","Roe, R."],"date":"1901-02-03","collection":["gutenberg"],
     "subject":["Travel","Maps"],"description":"<p>First.</p><p>Second</p>",
     "licenseurl":"http://creativecommons.org/publicdomain/zero/1.0/"}})";
  assert(ia_mediatype(item) == "texts");
  const std::string md = ia_markdown(item, "", {"book", 1});
  assert(has(md, "# A \\*Book\\* \\[1\\]"));
  assert(has(md, "Creator: Doe, Jane; Roe, R."));
  assert(has(md, "[gutenberg](ia:gutenberg)"));
  assert(has(md, "First."));
  assert(has(md, "Second"));
  assert(has(md, "[book.pdf](https://archive.org/download/book/book.pdf)"));
  assert(has(md, "my%20book.epub"));
  assert(!has(md, "book_meta.xml") && !has(md, "torrent"));
  assert(has(md, "external:https://archive.org/details/book"));
  // The license is an ordinary page: it opens in-app like any other link.
  assert(has(md, "](http://creativecommons.org/publicdomain/zero/1.0/)"));
  assert(!has(md, "external:http://creativecommons.org"));
  assert(!has(md, "mpv"));  // texts are not played

  // Media gets a mpv link.
  const char* movie = R"({"files":[{"name":"a.mp4","format":"MPEG4","source":"original"}],
    "metadata":{"identifier":"m","title":"Film","mediatype":"movies"}})";
  assert(has(ia_markdown(movie, "", {"m", 1}), "external:mpv:https://archive.org/download/m/a.mp4"));
  // ... and a Media panel link where this build plays the file.
  assert(!has(ia_markdown(movie, "", {"m", 1}), "Play]("));
  auto mp3_only = [](std::string_view name) { return name.size() > 4 && name.substr(name.size() - 4) == ".mp3"; };
  const char* album = R"({"files":[{"name":"a.ogv","format":"Ogg Video","source":"original"},
    {"name":"t 1.mp3","format":"VBR MP3","source":"derivative"}],
    "metadata":{"identifier":"al","title":"Album","mediatype":"audio"}})";
  const std::string am = ia_markdown(album, "", {"al", 1}, +mp3_only);
  assert(has(am, "Play](https://archive.org/download/al/t%201.mp3)"));
  assert(has(am, "external:mpv:https://archive.org/download/al/a.ogv"));

  // A collection lists its members and pages.
  const char* coll = R"({"metadata":{"identifier":"c","title":"Coll","mediatype":"collection"}})";
  const char* members = R"({"response":{"numFound":120,"start":0,"docs":[
    {"identifier":"one","title":"First item","mediatype":"texts","creator":"Ann"},
    {"identifier":"bad id","title":"x"}]}})";
  const std::string cm = ia_markdown(coll, members, {"c", 1});
  assert(has(cm, "[First item](ia:one) — texts · Ann"));
  assert(!has(cm, "bad id"));
  assert(has(cm, "[Next page](ia:c?page=2)"));
  assert(!has(cm, "Previous page"));
  assert(has(ia_markdown(coll, members, {"c", 2}), "[Previous page](ia:c)"));
  assert(has(ia_members_url({"c", 2}), "q=collection%3Ac"));

  // Originals with their derivatives nested below; book details; reviews.
  const char* scan = R"({"files":[
    {"name":"b.pdf","format":"Text PDF","source":"original","size":"4096"},
    {"name":"b_jp2.zip","format":"Single Page Processed JP2 ZIP","source":"derivative","original":"b.pdf"},
    {"name":"b_djvu.txt","format":"DjVuTXT","source":"derivative","original":["b_jp2.zip","b.pdf"]},
    {"name":"x.ogg","format":"Ogg Vorbis","source":"derivative","original":"x.mp3","title":"Song"},
    {"name":"lost.gif","format":"Animated GIF","source":"derivative","original":"gone.avi"},
    {"name":"x.mp3","format":"VBR MP3","source":"original","length":"245.4","title":"Song","track":"3"},
    {"name":"loop.a","source":"derivative","original":"loop.b"},
    {"name":"loop.b","source":"derivative","original":"loop.a"},
    {"name":"cover.jpg","format":"JPEG","source":"metadata"}],
   "item_size":3145728,"files_count":9,"item_last_updated":1700000000,
   "reviews":[{"reviewtitle":"Great","reviewer":"ann","reviewdate":"2020-01-02 03:04:05",
               "stars":"4","reviewbody":"Worth *reading*.\nTwice.\n\n1. Get it\n| no table"}],
   "metadata":{"identifier":"b","title":"B","mediatype":"texts","imagecount":"312",
     "isbn":["0123456789"],"oclc-id":"12345","openlibrary_edition":"OL1M",
     "scanningcenter":"Hong Kong","ppi":"400","access-restricted-item":"true",
     "addeddate":"2015-06-07 08:09:10"}})";
  const std::string sm = ia_markdown(scan, "", {"b", 1});
  assert(has(sm, "## Original files"));
  assert(has(sm, "\n- [b.pdf](https://archive.org/download/b/b.pdf) — Text PDF, 4.0 KB\n"
                 "  - [b_jp2.zip]"));
  assert(has(sm, "\n    - [b_djvu.txt]"));  // derived from the derivative
  assert(has(sm, "VBR MP3, 4:05 · 3. “Song”"));
  assert(has(sm, "x.ogg) — Ogg Vorbis\n"));  // the original's title is not repeated
  assert(has(sm, "## Other derived files\n\n- [lost.gif]"));
  assert(has(sm, "(from gone.avi)"));
  assert(has(sm, "[loop.a]") && has(sm, "[loop.b]"));  // a cycle still lists both
  assert(has(sm, "## Other files\n\n- [cover.jpg]"));
  assert(has(sm, "Pages: 312"));
  assert(has(sm, "ISBN: 0123456789"));
  assert(has(sm, "[12345](https://search.worldcat.org/title/12345)"));
  assert(has(sm, "[OL1M](https://openlibrary.org/books/OL1M)"));
  assert(has(sm, "Scanned: Hong Kong, 400 ppi"));
  assert(has(sm, "Access: lending library"));
  assert(has(sm, "Added: 2015-06-07\n"));
  assert(has(sm, "Updated: 2023-11-14"));
  assert(has(sm, "Size: 3.0 MB in 9 files"));
  assert(has(sm, "## Reviews\n\n★★★★☆ **Great** — ann, 2020-01-02\n\n"
                 "Worth \\*reading\\*.  \nTwice.\n\n1\\. Get it  \n\\| no table\n\n"));
  // Replies without `source` keep one plain list.
  assert(has(md, "## Original files") && !has(ia_markdown(movie, "", {"m", 1}), "## Files"));
  const char* bare = R"({"files":[{"name":"a.txt"}],"metadata":{"identifier":"z","mediatype":"texts"}})";
  assert(has(ia_markdown(bare, "", {"z", 1}), "## Files\n\n- [a.txt]"));

  // Unknown / malformed replies.
  assert(has(ia_markdown("{}", "", {"nope", 1}), "# Not found"));
  assert(has(ia_markdown("garbage", "", {"nope", 1}), "# Not found"));
  assert(ia_mediatype("{}").empty());
  return 0;
}
