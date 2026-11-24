// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
//
// Offline tests for page snapshots: the lossless document codec
// (doc_codec.hpp), the object store (object_store.hpp), snap: addresses and
// the snapshot store (snapshot.hpp). Part of xanatoo_tests (test_snapshots
// is called from unit_tests.cpp).

#include "xanatoo/doc_codec.hpp"
#include "xanatoo/images.hpp"
#include "xanatoo/object_store.hpp"
#include "xanatoo/paths.hpp"
#include "xanatoo/snapshot.hpp"
#include "xanatoo/url_cache.hpp"

#include <cstdio>
#include <filesystem>
#include <random>
#include <string>

#ifndef XANATOO_SQLITE
#define XANATOO_SQLITE 1
#endif

namespace {
int g_passed = 0;
int g_failed = 0;
void expect(bool cond, const char* expr, const char* file, int line) {
  if (cond) {
    ++g_passed;
    return;
  }
  ++g_failed;
  std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
}
}  // namespace

#define EXPECT(c) expect(!!(c), #c, __FILE__, __LINE__)
#define EXPECT_EQ(a, b) expect((a) == (b), #a " == " #b, __FILE__, __LINE__)

namespace {
using namespace xanatoo;
namespace fs = std::filesystem;

// A document using every field the codec writes, with non-default values.
Document sample_document() {
  Document doc;
  doc.meta.title = "Snapshot \xc3\xa4";
  doc.meta.lang = "de";
  doc.meta.author = "A. Uthor";
  doc.meta.front_matter = "title: x\n";
  Node root;
  root.id = 1;
  root.kind = NodeKind::Document;
  doc.nodes.push_back(root);
  doc.root = 1;

  Node p;
  p.id = 2;
  p.kind = NodeKind::Paragraph;
  p.parent = 1;
  InlineSpan link;
  link.kind = InlineKind::Link;
  link.role = static_cast<InlineRole>(1);
  link.marks = 0x5;
  link.text = "a link";
  link.href = "https://example.com/b";
  link.rel = "nofollow";
  link.datetime = "2026-11-24";
  link.target = 7;
  InlineSpan pic;
  pic.kind = InlineKind::Image;
  pic.text = "\xef\xbf\xbc";
  pic.src = "https://img.example.com/i.png";
  pic.alt = "inline";
  pic.width = 640;
  pic.height = 65535;
  pic.display = true;
  p.spans = {link, pic};
  p.level = -3;
  p.col_span = 2;
  p.row_span = 3;
  p.alt = "alt";
  p.expanded = true;
  p.summary = "sum";
  p.float_side = "right";
  p.align = "center";
  p.valign = "top";
  p.cols = "1fr 2fr";
  p.display_scale = 0.75f;
  p.width = -1;
  p.height = 1 << 20;
  p.label = "lbl";
  p.tags = "a b";
  p.style = "note";
  doc.nodes.push_back(p);

  Node img;
  img.id = 3;
  img.kind = NodeKind::Image;
  img.parent = 1;
  img.src = "https://img.example.com/big.jpg";
  doc.nodes.push_back(img);
  doc.nodes[0].children = {2, 3};
  doc.next_id = 9;  // ids handed out before need not be dense
  return doc;
}

bool same_span(const InlineSpan& a, const InlineSpan& b) {
  return a.kind == b.kind && a.role == b.role && a.marks == b.marks &&
         a.text == b.text && a.href == b.href && a.rel == b.rel &&
         a.datetime == b.datetime && a.target == b.target && a.src == b.src &&
         a.alt == b.alt && a.width == b.width && a.height == b.height &&
         a.display == b.display;
}

bool same_node(const Node& a, const Node& b) {
  if (a.spans.size() != b.spans.size()) return false;
  for (size_t i = 0; i < a.spans.size(); ++i)
    if (!same_span(a.spans[i], b.spans[i])) return false;
  return a.id == b.id && a.kind == b.kind && a.parent == b.parent &&
         a.children == b.children && a.level == b.level &&
         a.col_span == b.col_span && a.row_span == b.row_span &&
         a.alt == b.alt && a.src == b.src && a.expanded == b.expanded &&
         a.summary == b.summary && a.float_side == b.float_side &&
         a.align == b.align && a.valign == b.valign && a.cols == b.cols &&
         a.display_scale == b.display_scale && a.width == b.width &&
         a.height == b.height && a.label == b.label && a.tags == b.tags &&
         a.style == b.style;
}

bool same_document(const Document& a, const Document& b) {
  if (a.nodes.size() != b.nodes.size()) return false;
  for (size_t i = 0; i < a.nodes.size(); ++i)
    if (!same_node(a.nodes[i], b.nodes[i])) return false;
  return a.root == b.root && a.next_id == b.next_id &&
         a.meta.title == b.meta.title && a.meta.lang == b.meta.lang &&
         a.meta.author == b.meta.author &&
         a.meta.front_matter == b.meta.front_matter;
}

void test_doc_codec() {
  const Document doc = sample_document();
  const std::string bytes = encode_document(doc);
  const auto back = decode_document(bytes);
  EXPECT(back.has_value());
  EXPECT(back && same_document(doc, *back));
  EXPECT_EQ(encode_document(*back), bytes);  // deterministic
  // Every cut is refused, not misread.
  bool all_refused = true;
  for (size_t n = 0; n < bytes.size(); ++n)
    if (decode_document(std::string_view(bytes).substr(0, n))) all_refused = false;
  EXPECT(all_refused);
  EXPECT(!decode_document(bytes + "x"));
  std::string other = bytes;
  other[4] = static_cast<char>(kDocCodecVersion + 1);
  EXPECT(!decode_document(other));
  // Random damage never crashes (results may decode, ids stay in range).
  std::mt19937 rng(42);
  for (int k = 0; k < 2000; ++k) {
    std::string bad = bytes;
    bad[5 + rng() % (bad.size() - 5)] = static_cast<char>(rng());
    if (auto d = decode_document(bad)) {
      for (const Node& n : d->nodes)
        for (NodeId c : n.children) EXPECT(c <= d->nodes.size());
    }
  }
  EXPECT(decode_document(encode_document(Document{})).has_value());
}

void test_snapshot_refs() {
  const std::string id = snapshot_id_for("https://x.org/a", std::string(64, 'a'));
  EXPECT(is_snapshot_id(id));
  EXPECT_EQ(id, snapshot_id_for("https://x.org/a", std::string(64, 'a')));
  EXPECT(id != snapshot_id_for("https://x.org/b", std::string(64, 'a')));
  EXPECT(id != snapshot_id_for("https://x.org/a", std::string(64, 'b')));
  const std::string ref = snapshot_ref_string(id, "https://x.org/a?q=1#f");
  EXPECT_EQ(ref, "snap:" + id + "/https://x.org/a?q=1#f");
  const auto p = parse_snapshot_ref(ref);
  EXPECT(p && p->id == id && p->url == "https://x.org/a?q=1#f");
  EXPECT(is_snapshot_ref(ref));
  EXPECT_EQ(live_address(ref), "https://x.org/a?q=1#f");
  EXPECT_EQ(live_address(ref + "//id:x"), "https://x.org/a?q=1#f//id:x");
  EXPECT_EQ(live_address("https://x.org/"), "https://x.org/");
  // No nesting: wrapping a snapshot address wraps its live page.
  EXPECT_EQ(snapshot_ref_string(id, ref), ref);
  EXPECT_EQ(live_address("snap:" + id + "/" + ref), "https://x.org/a?q=1#f");
  EXPECT(!is_snapshot_ref("snap:" + id + "/"));
  EXPECT(!is_snapshot_ref("snap:" + id.substr(1) + "/https://x.org/"));
  EXPECT(!is_snapshot_ref("snap:" + std::string(32, 'G') + "/https://x.org/"));
  EXPECT(!is_snapshot_ref("wayback:20200101/https://x.org/"));

  Document doc = sample_document();
  const auto srcs = remote_image_srcs(doc);
  EXPECT(srcs.size() == 2 && srcs[0] == "https://img.example.com/i.png" &&
         srcs[1] == "https://img.example.com/big.jpg");
  rewrite_snapshot_images(doc, id);
  EXPECT_EQ(doc.nodes[2].src, "snap:" + id + "/https://img.example.com/big.jpg");
  EXPECT_EQ(doc.nodes[1].spans[1].src, "snap:" + id + "/https://img.example.com/i.png");
  EXPECT(doc.nodes[1].spans[0].src.empty());
  rewrite_snapshot_images(doc, id);  // idempotent
  EXPECT_EQ(doc.nodes[2].src, "snap:" + id + "/https://img.example.com/big.jpg");
  EXPECT_EQ(remote_image_srcs(doc), srcs);
  EXPECT(is_remote_image(doc.nodes[2].src));
}

void test_snapshot_store() {
  const fs::path cache = fs::temp_directory_path() /
      ("xanatoo-snapshots-" + std::to_string(std::random_device{}()));
  fs::remove_all(cache);
  set_dir_override(DirKind::Cache, cache.string());

  // Objects: content-addressed, idempotent, shared with cache files.
  const std::string h = object_put("hello");
  EXPECT_EQ(h, object_hash("hello"));
  EXPECT(is_object_hash(h));
  EXPECT_EQ(object_put("hello"), h);
  EXPECT(object_read(h) == std::optional<std::string>("hello"));
  EXPECT(!object_read(std::string(64, '0')));
  EXPECT(object_path("../x").empty());

  const std::string big = "https://img.example.com/big.jpg";
  const std::string small = "https://img.example.com/i.png";
  const std::string_view jpeg1 = "\xff\xd8\xff\xe0 first";
  UrlCacheEntry e;
  e.url = big;
  e.status = 200;
  e.content_type = "image/jpeg";
  EXPECT(url_cache_put(e, &jpeg1));
#if XANATOO_SQLITE
  const auto cached = url_cache_find(big);
  EXPECT(cached && cached->body == object_hash(jpeg1));
  EXPECT(object_exists(object_hash(jpeg1)));

  const Document doc = sample_document();
  const std::string raw = "<html>raw</html>";
  const std::string id =
      snapshot_store("https://x.org/page", doc, &raw, "text/html", "Web");
  EXPECT(is_snapshot_id(id));
  // Same page, same content: the same snapshot.
  EXPECT_EQ(snapshot_store("https://x.org/page", doc, &raw, "text/html", "Web"), id);
  const auto info = snapshot_find(id);
  EXPECT(info && info->url == "https://x.org/page" && info->title == doc.meta.title &&
         info->service == "Web" && info->resources == 2 &&
         info->resources_kept == 1 && info->last_seen >= info->first_seen);

  // The picture that was cached is the snapshot's copy; a newer answer
  // for the same URL does not change what the snapshot shows.
  const std::string snap_big = snapshot_ref_string(id, big);
  EXPECT_EQ(remote_image_path(snap_big), object_path(object_hash(jpeg1)));
  const std::string_view jpeg2 = "\xff\xd8\xff\xe0 second";
  EXPECT(url_cache_put(e, &jpeg2));
  EXPECT_EQ(remote_image_path(snap_big), object_path(object_hash(jpeg1)));
  EXPECT_EQ(remote_image_path(big), url_cache_body_path(big));
  // The one not loaded yet: the live file until it arrives, then kept.
  const std::string snap_small = snapshot_ref_string(id, small);
  EXPECT_EQ(remote_image_path(snap_small), url_cache_body_path(small));
  const std::string_view png = "\x89PNG small";
  EXPECT(store_remote_image(snap_small, png));
  EXPECT_EQ(remote_image_path(snap_small), object_path(object_hash(png)));
  EXPECT_EQ(snapshot_find(id)->resources_kept, 2);

  // Loaded: as shown, pictures pointing at their copies.
  const auto loaded = snapshot_load(id);
  EXPECT(loaded && loaded->raw == std::optional<std::string>(raw));
  EXPECT(loaded && loaded->doc.nodes.size() == doc.nodes.size() &&
         loaded->doc.nodes[2].src == snap_big &&
         loaded->doc.meta.title == doc.meta.title);

  // Another version of the page is another snapshot; newest first.
  Document doc2 = doc;
  doc2.nodes[1].spans[0].text = "changed";
  const std::string id2 =
      snapshot_store("https://x.org/page", doc2, nullptr, "text/html", "Web");
  EXPECT(is_snapshot_id(id2) && id2 != id);
  const auto all = snapshots_of("https://x.org/page");
  EXPECT(all.size() == 2 && all[0].id == id2);
  EXPECT(snapshots_of("snap:" + id + "/https://x.org/page").size() == 2);

  // A pruned cache file comes back from the object store, no download.
  fs::remove(url_cache_body_path(big));
  EXPECT(snapshot_restore_body(big, url_cache_body_path(big)));
  EXPECT_EQ(fs::file_size(url_cache_body_path(big)), jpeg2.size());

  // Evicted: the ref still names the live page, the load fails cleanly.
  fs::remove(object_path(info->doc));
  EXPECT(!snapshot_load(id));
  EXPECT_EQ(live_address(snapshot_ref_string(id, "https://x.org/page")),
            "https://x.org/page");
  snapshot_prune_missing();
  EXPECT(!snapshot_find(id));
  EXPECT(snapshot_find(id2).has_value());
#endif
  snapshot_close();
  url_cache_close();
  set_dir_override(DirKind::Cache, {});
  fs::remove_all(cache);
}

}  // namespace

void test_snapshots(int& passed, int& failed) {
  g_passed = g_failed = 0;
  test_doc_codec();
  test_snapshot_refs();
  test_snapshot_store();
  passed += g_passed;
  failed += g_failed;
}
