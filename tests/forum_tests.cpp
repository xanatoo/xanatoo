// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
//
// Offline tests for the forum clients (forums.hpp): refs and their web
// forms, and the document mappers fed with cut-down recorded API replies.
// Part of xanatoo_tests (test_forums is called from unit_tests.cpp).

#include "xanatoo/forums.hpp"
#include "xanatoo/gallery.hpp"
#include "xanatoo/json_dom.hpp"
#include "xanatoo/layout.hpp"

#include <cstdio>
#include <string>
#include <string_view>

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

using namespace xanatoo;

namespace {

Json parse(std::string_view text) {
  auto j = json_parse(text);
  EXPECT(j.has_value());
  return j ? *j : Json{};
}

ForumRef ref(std::string_view s) {
  auto r = parse_forum_ref(s);
  EXPECT(r.has_value());
  return r ? *r : ForumRef{};
}

std::string all_text(const Document& d) {
  std::string out;
  for (const Node& n : d.nodes) {
    for (const InlineSpan& s : n.spans) out += s.text + "|";
    out += n.src + "|";
  }
  return out;
}

bool has_href(const Document& d, std::string_view href) {
  for (const Node& n : d.nodes)
    for (const InlineSpan& s : n.spans)
      if (s.href == href) return true;
  return false;
}

const Node* message(const Document& d, std::string_view label) {
  for (const Node& n : d.nodes)
    if (n.kind == NodeKind::Message && n.label == label) return &n;
  return nullptr;
}

size_t count_kind(const Document& d, NodeKind k) {
  size_t n = 0;
  for (const Node& node : d.nodes) n += node.kind == k;
  return n;
}

void test_refs() {
  EXPECT(is_forum_ref("lobsters:"));
  EXPECT(is_forum_ref("fourchan:"));
  EXPECT(!is_forum_ref("discourse:"));  // needs a host
  EXPECT(!is_forum_ref("bsky:"));       // needs an actor
  EXPECT(!is_forum_ref("discourse:bad host/x"));
  EXPECT(!is_forum_ref("hn:"));

  ForumRef d = ref("discourse:Discourse.NixOS.org/t/zhf/81027/?page=2#id:5");
  EXPECT(d.site == ForumSite::Discourse);
  EXPECT_EQ(d.host, "discourse.nixos.org");
  EXPECT_EQ(d.path, "t/zhf/81027");
  EXPECT_EQ(d.page, 2);
  EXPECT_EQ(d.fragment, "id:5");
  EXPECT_EQ(forum_ref_string(d), "discourse:discourse.nixos.org/t/zhf/81027?page=2#id:5");
  EXPECT_EQ(forum_web_url(d), "https://discourse.nixos.org/t/zhf/81027?page=2");
  EXPECT_EQ(forum_up_ref(d), "discourse:discourse.nixos.org");
  EXPECT_EQ(forum_up_ref(ref("discourse:discourse.nixos.org")), "");

  EXPECT_EQ(forum_ref_string(ref("fourchan:g")), "fourchan:g/");
  EXPECT_EQ(forum_web_url(ref("fourchan:g/thread/1")), "https://boards.4chan.org/g/thread/1");
  EXPECT_EQ(forum_web_url(ref("fourchan:g/")), "https://boards.4chan.org/g/");
  EXPECT_EQ(forum_up_ref(ref("fourchan:g/thread/1#id:2")), "fourchan:g/");
  EXPECT_EQ(forum_up_ref(ref("fourchan:g/")), "fourchan:");
  EXPECT_EQ(forum_up_ref(ref("fourchan:")), "");

  EXPECT_EQ(forum_web_url(ref("lobsters:newest?page=3")), "https://lobste.rs/newest/page/3");
  EXPECT_EQ(forum_web_url(ref("lobsters:?page=2")), "https://lobste.rs/page/2");
  EXPECT_EQ(forum_up_ref(ref("lobsters:s/abc")), "lobsters:");

  ForumRef b = ref("bsky:alice.bsky.social?cursor=2026-11-03T16%3A06%3A41.753Z");
  EXPECT_EQ(b.cursor, "2026-11-03T16%3A06%3A41.753Z");
  EXPECT_EQ(forum_ref_string(b), "bsky:alice.bsky.social?cursor=2026-11-03T16%3A06%3A41.753Z");
  EXPECT(ref("bsky:alice?cursor=a&b").cursor == "a");
  EXPECT(ref("bsky:alice?cursor=<x>").cursor.empty());
  EXPECT_EQ(forum_web_url(ref("bsky:alice.bsky.social/post/3k")),
            "https://bsky.app/profile/alice.bsky.social/post/3k");
  EXPECT_EQ(forum_up_ref(ref("bsky:alice.bsky.social/post/3k")), "bsky:alice.bsky.social");
  EXPECT_EQ(forum_web_url(ref("bsky:hashtag/caf%C3%A9")), "https://bsky.app/hashtag/caf%C3%A9");
  EXPECT_EQ(forum_web_url(ref("bsky:bsky.app/feed/whats-hot")),
            "https://bsky.app/profile/bsky.app/feed/whats-hot");
  EXPECT_EQ(forum_up_ref(ref("bsky:bsky.app/feed/whats-hot")), "bsky:bsky.app");
  EXPECT_EQ(forum_up_ref(ref("bsky:hashtag/art")), "");

  // Web pages of the fixed-host sites → refs.
  EXPECT_EQ(forum_ref_from_web_url("https://lobste.rs/"), "lobsters:");
  EXPECT_EQ(forum_ref_from_web_url("https://lobste.rs/hottest"), "lobsters:");
  EXPECT_EQ(forum_ref_from_web_url("https://lobste.rs/s/spmgsd/codeberg_s_domain"),
            "lobsters:s/spmgsd");
  EXPECT_EQ(forum_ref_from_web_url("https://lobste.rs/t/rust/page/2"), "lobsters:t/rust?page=2");
  EXPECT_EQ(forum_ref_from_web_url("https://lobste.rs/~jcs"), "lobsters:~jcs/stories");
  EXPECT_EQ(forum_ref_from_web_url("https://lobste.rs/about"), "");
  EXPECT_EQ(forum_ref_from_web_url("https://boards.4chan.org/g/thread/123/some-slug#p456"),
            "fourchan:g/thread/123#id:456");
  EXPECT_EQ(forum_ref_from_web_url("https://boards.4chan.org/aco/catalog#s=csdg"), "fourchan:aco/");
  EXPECT_EQ(forum_ref_from_web_url("https://stackoverflow.com/questions/11227809/why-is"),
            "stackexchange:stackoverflow.com/questions/11227809");
  EXPECT_EQ(forum_ref_from_web_url("https://unix.stackexchange.com/questions/1/x/2#2"),
            "stackexchange:unix.stackexchange.com/questions/1#id:2");
  EXPECT_EQ(forum_ref_from_web_url("https://superuser.com/q/42"),
            "stackexchange:superuser.com/questions/42");
  EXPECT_EQ(forum_ref_from_web_url("https://askubuntu.com/questions/tagged/apt"),
            "stackexchange:askubuntu.com/questions/tagged/apt");
  EXPECT_EQ(forum_ref_from_web_url("https://api.stackexchange.com/2.3/questions"), "");
  EXPECT_EQ(forum_ref_from_web_url("https://stackoverflow.com/users/1"), "");
  EXPECT_EQ(forum_ref_from_web_url("https://bsky.app/profile/bsky.app/post/3n2"),
            "bsky:bsky.app/post/3n2");
  EXPECT_EQ(forum_ref_from_web_url("https://bsky.app/search"), "");
  EXPECT_EQ(forum_ref_from_web_url("https://bsky.app/search?q=open+web"),
            "bsky:search?query=open%20web");
  EXPECT_EQ(forum_web_url(ref("bsky:search?query=open+web")),
            "https://bsky.app/search?q=open%20web");
  EXPECT_EQ(forum_up_ref(ref("bsky:search?query=x")), "");
  EXPECT_EQ(forum_ref_from_web_url("https://bsky.app/profile/bsky.app/feed/whats-hot"),
            "bsky:bsky.app/feed/whats-hot");
  EXPECT_EQ(forum_ref_from_web_url("https://bsky.app/hashtag/café"), "bsky:hashtag/caf%C3%A9");
  EXPECT_EQ(forum_ref_from_web_url("https://bsky.app/hashtag/caf%C3%A9"), "bsky:hashtag/caf%C3%A9");
  EXPECT_EQ(forum_ref_from_web_url("https://bsky.app/hashtag/"), "");
  // Discourse / Flarum run anywhere: never guessed from a URL.
  EXPECT_EQ(forum_ref_from_web_url("https://discourse.nixos.org/t/x/1"), "");
}

void test_discourse() {
  const ForumRef list_ref = ref("discourse:discourse.nixos.org");
  Json list = parse(R"({"users":[{"id":7,"username":"alice"}],
    "topic_list":{"more_topics_url":"/latest?page=1","topics":[
      {"id":81027,"title":"Zero Hydra Failures &amp; more","slug":"zhf","posts_count":4,
       "views":1163,"like_count":54,"pinned":true,"category_id":8,
       "excerpt":"Hi everyone,\ntoday&hellip;","last_posted_at":"2026-11-12T21:29:00.513Z",
       "posters":[{"user_id":7}]}]}})");
  Document l = discourse_list_document(list, list_ref);
  EXPECT(has_href(l, "discourse:discourse.nixos.org/t/zhf/81027"));
  EXPECT(has_href(l, "discourse:discourse.nixos.org/c/8"));
  EXPECT(has_href(l, "discourse:discourse.nixos.org?page=2"));  // more_topics_url
  EXPECT(has_href(l, "https://discourse.nixos.org/u/alice"));
  const std::string lt = all_text(l);
  EXPECT(lt.find("Zero Hydra Failures & more") != std::string::npos);
  EXPECT(lt.find("3 replies") != std::string::npos);
  EXPECT(lt.find("Hi everyone, today…") != std::string::npos);

  // Page 2 of a topic whose post numbers have a gap (post 3 deleted).
  Json topic = parse(R"({"id":81027,"title":"ZHF","slug":"zhf","posts_count":4,"chunk_size":2,
    "post_stream":{"stream":[11,12,14,15],"posts":[
      {"id":14,"post_number":4,"username":"bob","created_at":"2026-11-08T03:10:00Z",
       "cooked":"<p>See <a href=\"https://discourse.nixos.org/t/other/5/2\">this</a></p>",
       "reply_to_post_number":2,"actions_summary":[{"id":2,"count":3}]},
      {"id":15,"post_number":5,"username":"carol","created_at":"2026-11-09T00:00:00Z",
       "cooked":"","post_type":3}]}})");
  Document t = discourse_topic_document(topic, ref("discourse:discourse.nixos.org/t/zhf/81027/4"));
  const Node* m = message(t, "4");
  EXPECT(m != nullptr);
  EXPECT(message(t, "5") == nullptr);  // moderator action without text
  if (m) {
    EXPECT_EQ(m->src, "bob");
    EXPECT_EQ(m->spans[1].href, "discourse:discourse.nixos.org/t/zhf/81027?page=2#id:4");
    EXPECT_EQ(m->spans[2].href, "#id:2");  // reply-to, not indented
    EXPECT(m->alt.empty());
    EXPECT_EQ(m->spans[3].text, "3 likes");
  }
  EXPECT(has_href(t, "discourse:discourse.nixos.org/t/other/5/2"));
  EXPECT(has_href(t, "discourse:discourse.nixos.org/t/zhf/81027"));  // ← Previous (page 1)
  EXPECT(!has_href(t, "discourse:discourse.nixos.org/t/zhf/81027?page=3"));
}

void test_lobsters() {
  Json story = parse(R"({"short_id":"spmgsd","title":"Codeberg's domain","url":"https://example.org/a",
    "score":80,"comment_count":2,"submitter_user":"mort","created_at":"2026-11-18T02:51:09.576-06:00",
    "description":"","tags":["vcs"],"comments":[
      {"short_id":"c1","parent_comment":null,"commenting_user":"krig","score":28,"is_deleted":false,
       "created_at":"2026-11-18T03:03:22.958-06:00","comment":"<p>See <a href=\"https://lobste.rs/c/c2\">below</a> and <a href=\"https://lobste.rs/s/other1/x\">this</a></p>"},
      {"short_id":"c2","parent_comment":"c1","commenting_user":"","score":1,"is_deleted":true,
       "created_at":"2026-11-18T03:10:00-06:00","comment":""}]})");
  Document d = lobsters_story_document(story, ref("lobsters:s/spmgsd"));
  const Node* top = message(d, "spmgsd");
  const Node* c1 = message(d, "c1");
  const Node* c2 = message(d, "c2");
  EXPECT(top && c1 && c2);
  if (top) EXPECT_EQ(top->summary, "2026-11-18 08:51 UTC");  // offset → UTC
  if (c1) EXPECT_EQ(c1->alt, "spmgsd");
  if (c2) {
    EXPECT_EQ(c2->alt, "c1");
    EXPECT_EQ(c2->src, "[deleted]");
  }
  EXPECT(has_href(d, "#id:c2"));
  EXPECT(has_href(d, "lobsters:s/other1"));
  EXPECT(has_href(d, "lobsters:t/vcs"));
  EXPECT(has_href(d, "https://example.org/a"));

  Json list = parse(R"([{"short_id":"a1","title":"T","url":"","score":1,"comment_count":0,
    "submitter_user":"u","created_at":"2026-11-18T00:00:00Z","tags":[]}])");
  Document l = lobsters_list_document(list, ref("lobsters:newest"));
  EXPECT(has_href(l, "lobsters:s/a1"));
  EXPECT(has_href(l, "lobsters:~u/stories"));
  EXPECT(!has_href(l, "lobsters:newest?page=2"));  // fewer than 25 stories
}

void test_fourchan() {
  Json boards = parse(R"({"boards":[{"board":"g","title":"Technology","ws_board":1,
      "meta_description":"&quot;/g/&quot; is for tech"},{"board":"b","title":"Random","ws_board":0}]})");
  Document b = fourchan_boards_document(boards);
  EXPECT(has_href(b, "fourchan:g/"));
  EXPECT(has_href(b, "fourchan:b/"));
  EXPECT(all_text(b).find("Not work safe") != std::string::npos);
  // List items are leaf text blocks: the board link sits on the item itself.
  int items = 0;
  for (const Node& n : b.nodes)
    if (n.kind == NodeKind::ListItem) {
      ++items;
      EXPECT(!n.spans.empty());
      EXPECT(n.children.empty());
    }
  EXPECT_EQ(items, 2);

  Json thread = parse(R"({"posts":[
    {"no":100,"resto":0,"sub":"/ldg/ &amp; co","name":"Anonymous","time":1794963393,
     "com":"Previous: <a href=\"/g/thread/90#p90\" class=\"quotelink\">&gt;&gt;90</a><br>https://rentry.org/ldg-lazy<wbr>-guide",
     "tim":1794963393356146,"ext":".jpg","filename":"finished","fsize":575611,"w":1741,"h":1603},
    {"no":101,"resto":100,"name":"Anonymous","trip":"!!abc","time":1794963638,
     "com":"<a href=\"#p100\" class=\"quotelink\">&gt;&gt;100</a><br>thanks"},
    {"no":102,"resto":100,"name":"Anonymous","time":1794963700,
     "com":"<a href=\"#p99\" class=\"quotelink\">&gt;&gt;99</a><br>gone"},
    {"no":103,"resto":100,"name":"Anonymous","time":1794963800,
     "com":"<a href=\"#p101\" class=\"quotelink\">&gt;&gt;101</a> <a href=\"#p102\" class=\"quotelink\">&gt;&gt;102</a>"}]})");
  Document t = fourchan_thread_document(thread, ref("fourchan:g/thread/100"));
  EXPECT_EQ(t.meta.title, "/ldg/ & co");
  const Node* op = message(t, "100");
  const Node* reply = message(t, "101");
  EXPECT(op && reply);
  if (reply) {
    EXPECT_EQ(reply->src, "Anonymous !!abc");
    EXPECT_EQ(reply->spans[2].href, "#id:100");
    EXPECT_EQ(reply->summary, "2026-11-18 01:00 UTC");
  }
  // Threaded by the first quotelink to an earlier post, depth-first.
  if (op) EXPECT_EQ(op->alt, "");
  if (reply) EXPECT_EQ(reply->alt, "100");
  if (const Node* gone = message(t, "102")) EXPECT_EQ(gone->alt, "");  // >>99 not here
  if (const Node* both = message(t, "103")) EXPECT_EQ(both->alt, "101");
  std::string order;
  for (const Node& n : t.nodes)
    if (n.kind == NodeKind::Message) order += n.label + " ";
  EXPECT_EQ(order, "100 101 103 102 ");
  EXPECT(has_href(t, "#id:100"));
  EXPECT(has_href(t, "fourchan:g/thread/90#id:90"));
  EXPECT(has_href(t, "https://i.4cdn.org/g/1794963393356146.jpg"));
  EXPECT(all_text(t).find("https://rentry.org/ldg-lazy-guide") != std::string::npos);
  bool thumb = false;
  for (const Node& n : t.nodes)
    thumb |= n.kind == NodeKind::Image && n.src == "https://i.4cdn.org/g/1794963393356146s.jpg";
  EXPECT(thumb);
  // A click on the thumbnail (and the gallery) shows the full file.
  bool full = false;
  for (const auto& it : gallery_items(t, INVALID_NODE))
    full |= it.thumb == "https://i.4cdn.org/g/1794963393356146s.jpg" &&
            it.src == "https://i.4cdn.org/g/1794963393356146.jpg";
  EXPECT(full);

  Json catalog = parse(R"([{"page":1,"threads":[{"no":5,"com":"no subject here","replies":2,
      "images":1,"time":1794963393}]}])");
  Document c = fourchan_catalog_document(catalog, ref("fourchan:g/"));
  EXPECT(has_href(c, "fourchan:g/thread/5"));
  EXPECT(all_text(c).find("no subject here") != std::string::npos);
}

void test_stackexchange() {
  const ForumRef r = ref("stackexchange:stackoverflow.com/questions/1");
  Json q = parse(R"({"items":[{"question_id":1,"title":"Why &quot;x&quot;?","score":5,
      "answer_count":1,"view_count":10,"tags":["c++"],"creation_date":1340805096,
      "owner":{"display_name":"G&amp;G","link":"https://stackoverflow.com/users/1/g"},
      "body":"<p>Q <a href=\"https://stackoverflow.com/questions/2/other\">other</a></p>"}]})");
  Json a = parse(R"({"has_more":true,"items":[{"answer_id":3,"question_id":1,"score":9,
      "is_accepted":true,"creation_date":1340805402,"owner":{"display_name":"M"},
      "body":"<p>A, see <a href=\"https://stackoverflow.com/questions/1/why/3#3\">here</a></p>"}]})");
  Json c = parse(R"({"items":[{"comment_id":7,"post_id":3,"score":2,"creation_date":1340806000,
      "owner":{"display_name":"P"},"body":"nice"}]})");
  Document d = se_question_document(q, a, &c, r);
  EXPECT_EQ(d.meta.title, "Why \"x\"?");
  const Node* qm = message(d, "1");
  const Node* am = message(d, "3");
  const Node* cm = message(d, "c7");
  EXPECT(qm && am && cm);
  if (qm) EXPECT_EQ(qm->src, "G&G");
  if (am) EXPECT_EQ(am->spans[2].text, "9 votes · accepted");
  if (cm) {
    EXPECT_EQ(cm->alt, "3");
    EXPECT_EQ(cm->spans[3].text, "2 votes");
  }
  EXPECT(has_href(d, "stackexchange:stackoverflow.com/questions/2"));
  EXPECT(has_href(d, "#id:3"));
  EXPECT(has_href(d, "stackexchange:stackoverflow.com/questions/tagged/c++"));
  EXPECT(has_href(d, "stackexchange:stackoverflow.com/questions/1?page=2"));
  // Without the comments reply the page still builds.
  Document nc = se_question_document(q, a, nullptr, r);
  EXPECT(message(nc, "c7") == nullptr);
  EXPECT(message(nc, "3") != nullptr);
}

void test_bluesky() {
  // "héllo @bob x.org": é is two bytes, so the facets' byte offsets differ
  // from character offsets.
  Json thread = parse(R"({"thread":{"$type":"app.bsky.feed.defs#threadViewPost",
    "parent":{"$type":"app.bsky.feed.defs#threadViewPost","post":{
      "uri":"at://did:plc:p/app.bsky.feed.post/root","author":{"did":"did:plc:p","handle":"pat.example"},
      "record":{"text":"root post","createdAt":"2026-11-13T21:00:00Z"},"likeCount":1}},
    "post":{"uri":"at://did:plc:a/app.bsky.feed.post/3k","author":{"did":"did:plc:a",
        "handle":"alice.example","displayName":"Alice"},
      "record":{"text":"héllo @bob x.org","createdAt":"2026-11-13T22:03:00Z",
        "facets":[{"index":{"byteStart":7,"byteEnd":11},"features":[{"$type":"app.bsky.richtext.facet#mention","did":"did:plc:b"}]},
                  {"index":{"byteStart":12,"byteEnd":17},"features":[{"$type":"app.bsky.richtext.facet#link","uri":"https://x.org/"}]}]},
      "embed":{"$type":"app.bsky.embed.images#view","images":[{"thumb":"https://cdn.example/t.jpg","fullsize":"https://cdn.example/f@jpeg","alt":"a cat"}]},
      "likeCount":5,"replyCount":1},
    "replies":[{"$type":"app.bsky.feed.defs#threadViewPost","post":{
        "uri":"at://did:plc:c/app.bsky.feed.post/r1","author":{"did":"did:plc:c","handle":"carl.example"},
        "record":{"text":"see https://bsky.app/profile/alice.example/post/3k","createdAt":"2026-11-13T22:10:00Z",
          "facets":[{"index":{"byteStart":4,"byteEnd":50},"features":[{"$type":"app.bsky.richtext.facet#link","uri":"https://bsky.app/profile/alice.example/post/3k"}]}]}}},
      {"$type":"app.bsky.feed.defs#notFoundPost","uri":"at://x/y/z"}]}})");
  Document d = bsky_thread_document(thread, ref("bsky:alice.example/post/3k"));
  const Node* root = message(d, "root");
  const Node* post = message(d, "3k");
  const Node* reply = message(d, "r1");
  EXPECT(root && post && reply);
  if (post) {
    EXPECT_EQ(post->src, "Alice (@alice.example)");
    EXPECT_EQ(post->spans[2].href, "#id:root");  // answers the parent shown above
    EXPECT(post->alt.empty());
    EXPECT_EQ(post->spans[3].text, "5 likes · 1 reply");
  }
  if (reply) EXPECT_EQ(reply->alt, "3k");
  const auto pics = gallery_items(d, INVALID_NODE);
  EXPECT(pics.size() == 1 && pics[0].thumb == "https://cdn.example/t.jpg" &&
         pics[0].src == "https://cdn.example/f@jpeg");
  // Parent first, then the post, then replies.
  if (root && post && reply) EXPECT(root->id < post->id && post->id < reply->id);
  std::string mention, link;
  for (const Node& n : d.nodes)
    for (const InlineSpan& s : n.spans) {
      if (s.href == "bsky:did:plc:b") mention = s.text;
      if (s.href == "https://x.org/") link = s.text;
    }
  EXPECT_EQ(mention, "@bob");
  EXPECT_EQ(link, "x.org");
  EXPECT(has_href(d, "bsky:alice.example/post/3k"));  // bsky.app link made native
  bool image = false;
  for (const Node& n : d.nodes)
    image |= n.kind == NodeKind::Image && n.src == "https://cdn.example/t.jpg" && n.alt == "a cat";
  EXPECT(image);

  Json missing = parse(R"({"thread":{"$type":"app.bsky.feed.defs#notFoundPost"}})");
  Document m = bsky_thread_document(missing, ref("bsky:alice.example/post/gone"));
  EXPECT(all_text(m).find("not found") != std::string::npos);

  Json feed = parse(R"({"cursor":"2026-11-03T16:06:41.753Z","feed":[{"post":{
      "uri":"at://did:plc:a/app.bsky.feed.post/p1","author":{"did":"did:plc:a","handle":"alice.example"},
      "record":{"text":"one\n\ntwo","createdAt":"2026-11-13T22:03:00Z"},
      "embed":{"$type":"app.bsky.embed.record#view","record":{"$type":"app.bsky.embed.record#viewRecord",
        "uri":"at://did:plc:q/app.bsky.feed.post/q1","author":{"did":"did:plc:q","handle":"quinn.example"},
        "value":{"text":"quoted words"}}}},
      "reason":{"$type":"app.bsky.feed.defs#reasonRepost","by":{"handle":"bob.example"}}}]})");
  Document f = bsky_feed_document(feed, ref("bsky:bob.example"));
  EXPECT(has_href(f, "bsky:bob.example?cursor=2026-11-03T16%3A06%3A41.753Z"));
  EXPECT(has_href(f, "bsky:bob.example"));  // reposted by
  // Replies are behind a classic comments link at the bottom of each post.
  bool discuss = false;
  for (const Node& n : f.nodes)
    for (const InlineSpan& s : n.spans)
      discuss |= s.text == "discuss" && s.href == "bsky:alice.example/post/p1";
  EXPECT(discuss);
  const Node* p1 = message(f, "p1");
  size_t paras = 0;
  if (p1)
    for (NodeId c : p1->children) paras += f.get(c).kind == NodeKind::Paragraph;
  EXPECT_EQ(paras, 2u);
  // Quotes are leaf blocks: the quoted text sits in a BlockQuote's spans.
  bool quoted = false, by = false;
  for (const Node& n : f.nodes)
    if (n.kind == NodeKind::BlockQuote)
      for (const InlineSpan& s : n.spans) {
        quoted |= s.text == "quoted words";
        by |= s.href == "bsky:quinn.example/post/q1";
      }
  EXPECT(quoted);
  EXPECT(by);

  // The profile heads an author feed.
  Json profile = parse(R"({"did":"did:plc:b","handle":"bob.example","displayName":"Bob",
      "description":"Writes things.","followersCount":12,"followsCount":1,"postsCount":3})");
  Document fp = bsky_feed_document(feed, ref("bsky:bob.example"), &profile);
  EXPECT_EQ(fp.meta.title, "Bluesky — Bob (@bob.example)");
  EXPECT(all_text(fp).find("Writes things.") != std::string::npos);
  EXPECT(all_text(fp).find("12 followers") != std::string::npos);

  // A custom feed under its generator's name; searchPosts (a hashtag) lists
  // bare posts with tag facets kept native.
  Json gen = parse(R"({"displayName":"Discover","description":"Trending","likeCount":5,
      "creator":{"did":"did:plc:z","handle":"bsky.app","displayName":"Bluesky"}})");
  Document g = bsky_feed_document(feed, ref("bsky:bsky.app/feed/whats-hot"), &gen);
  EXPECT_EQ(g.meta.title, "Bluesky — Discover (feed)");
  EXPECT(has_href(g, "bsky:bsky.app"));
  EXPECT(has_href(g, "bsky:bsky.app/feed/whats-hot?cursor=2026-11-03T16%3A06%3A41.753Z"));
  Json found = parse(R"({"cursor":"30","posts":[{"replyCount":4,
      "uri":"at://did:plc:a/app.bsky.feed.post/t1","author":{"did":"did:plc:a","handle":"alice.example"},
      "record":{"text":"hi #café","createdAt":"2026-11-13T22:03:00Z",
        "facets":[{"index":{"byteStart":3,"byteEnd":9},"features":[{"$type":"app.bsky.richtext.facet#tag","tag":"café"}]}]}}]})");
  Document h = bsky_feed_document(found, ref("bsky:hashtag/caf%C3%A9"));
  EXPECT_EQ(h.meta.title, "Bluesky — #café");
  EXPECT(message(h, "t1") != nullptr);
  EXPECT(has_href(h, "bsky:hashtag/caf%C3%A9"));
  EXPECT(has_href(h, "bsky:hashtag/caf%C3%A9?cursor=30"));
  EXPECT(all_text(h).find("No posts.") == std::string::npos);
  EXPECT(all_text(h).find("4 replies") == std::string::npos);  // not in the header
  bool comments = false;
  for (const Node& n : h.nodes)
    for (const InlineSpan& s : n.spans)
      comments |= s.text == "4 comments" && s.href == "bsky:alice.example/post/t1";
  EXPECT(comments);
  EXPECT_EQ(bsky_feed_document(found, ref("bsky:search?query=caf%C3%A9")).meta.title,
            "Bluesky — search: café");
  EXPECT(all_text(bsky_feed_document(parse(R"({"feed":[]})"), ref("bsky:bob.example")))
             .find("No posts.") != std::string::npos);
}

void test_flarum() {
  const ForumRef r = ref("flarum:discuss.flarum.org/d/38654-awards?page=2");
  Json head = parse(R"({"data":{"type":"discussions","id":"38654","attributes":{"title":"Awards"}}})");
  Json posts = parse(R"({"links":{"next":"x"},"data":[
    {"type":"posts","id":"1","attributes":{"number":21,"createdAt":"2026-01-19T09:16:06+00:00",
      "contentType":"comment","likesCount":2,
      "contentHtml":"<p><a href=\"https://discuss.flarum.org/d/38654/3\" class=\"PostMention\">bob</a> and <a href=\"https://discuss.flarum.org/d/12-other\">x</a></p>"},
     "relationships":{"user":{"data":{"type":"users","id":"9"}}}},
    {"type":"posts","id":"2","attributes":{"number":22,"contentType":"discussionRenamed"}}],
    "included":[{"type":"users","id":"9","attributes":{"username":"ann","displayName":"Ann"}}]})");
  Document d = flarum_discussion_document(head, posts, r);
  EXPECT_EQ(d.meta.title, "Awards");
  const Node* m = message(d, "21");
  EXPECT(m != nullptr);
  EXPECT(message(d, "22") == nullptr);
  if (m) {
    EXPECT_EQ(m->src, "Ann");
    EXPECT_EQ(m->spans[0].href, "https://discuss.flarum.org/u/ann");
    EXPECT_EQ(m->spans[1].href, "flarum:discuss.flarum.org/d/38654?page=2#id:21");
  }
  EXPECT(has_href(d, "#id:3"));
  EXPECT(has_href(d, "flarum:discuss.flarum.org/d/12"));
  EXPECT(has_href(d, "flarum:discuss.flarum.org/d/38654-awards"));  // ← Previous
  EXPECT(has_href(d, "flarum:discuss.flarum.org/d/38654-awards?page=3"));

  Json list = parse(R"({"links":{},"data":[{"type":"discussions","id":"5","attributes":{
      "title":"Hi","commentCount":3,"lastPostedAt":"2026-11-18T11:00:20+00:00"},
      "relationships":{"user":{"data":{"type":"users","id":"9"}},"tags":{"data":[{"type":"tags","id":"1"}]}}}],
    "included":[{"type":"users","id":"9","attributes":{"username":"ann"}},
                {"type":"tags","id":"1","attributes":{"name":"Extensions"}}]})");
  Document l = flarum_list_document(list, ref("flarum:discuss.flarum.org"));
  EXPECT(has_href(l, "flarum:discuss.flarum.org/d/5"));
  const std::string lt = all_text(l);
  EXPECT(lt.find("2 replies") != std::string::npos);
  EXPECT(lt.find("Extensions") != std::string::npos);
  EXPECT(count_kind(l, NodeKind::Article) == 1);
}


void test_lesswrong() {
  // Refs and their web forms.
  EXPECT(is_forum_ref("lesswrong:"));
  EXPECT_EQ(forum_ref_string(ref("lesswrong:search?query=rss+feed&page=2")),
            "lesswrong:search?query=rss%20feed&page=2");
  EXPECT_EQ(ref("lesswrong:search?query=a%26b").query, "a&b");
  EXPECT_EQ(forum_web_url(ref("lesswrong:posts/abc/slug#id:c1")),
            "https://www.lesswrong.com/posts/abc/slug?commentId=c1");
  EXPECT_EQ(forum_web_url(ref("lesswrong:new")), "https://www.lesswrong.com/allPosts?sortedBy=new");
  EXPECT_EQ(forum_web_url(ref("lesswrong:search?query=a%20b")),
            "https://www.lesswrong.com/search?query=a%20b");
  EXPECT_EQ(forum_up_ref(ref("lesswrong:posts/abc")), "lesswrong:");
  EXPECT_EQ(forum_ref_from_web_url("https://www.lesswrong.com/"), "lesswrong:");
  EXPECT_EQ(forum_ref_from_web_url(
                "https://www.lesswrong.com/posts/dzF8vSdDtmWjCBBDr/secrets?commentId=8iGJ9"),
            "lesswrong:posts/dzF8vSdDtmWjCBBDr/secrets#id:8iGJ9");
  EXPECT_EQ(forum_ref_from_web_url("https://lesswrong.com/users/brendan-long"),
            "lesswrong:users/brendan-long");
  EXPECT_EQ(forum_ref_from_web_url("https://www.lesswrong.com/tag/ai"), "lesswrong:w/ai");
  EXPECT_EQ(forum_ref_from_web_url("https://www.lesswrong.com/search?query=x+y"),
            "lesswrong:search?query=x%20y");
  EXPECT_EQ(forum_ref_from_web_url("https://www.lesswrong.com/library"), "");

  // A post with a reply tree (flat, best first) and links back to the site.
  Json post = parse(R"({"data":{"post":{"result":{"_id":"P1","slug":"s","title":"Secrets",
      "postedAt":"2026-02-23T20:12:06.950Z","baseScore":36,"commentCount":3,"url":null,
      "question":false,"user":{"slug":"brendan-long","displayName":"Brendan Long"},"coauthors":[],
      "tags":[{"name":"Practical","slug":"practical"}],
      "contents":{"html":"<p>See <a href=\"https://www.lesswrong.com/posts/P1/s?commentId=C2\">this</a> and <a href=\"/posts/Q9/other\">that</a>.</p>"}}},
    "comments":{"results":[
      {"_id":"C1","parentCommentId":null,"postedAt":"2026-02-24T08:10:50Z","baseScore":8,"deleted":false,
       "user":{"slug":"jim","displayName":"jim"},"contents":{"html":"<p>top</p>"}},
      {"_id":"C3","parentCommentId":null,"postedAt":"2026-02-24T09:00:00Z","baseScore":2,"deleted":true,
       "user":null,"contents":{"html":""}},
      {"_id":"C2","parentCommentId":"C1","postedAt":"2026-02-24T10:00:00Z","baseScore":1,"deleted":false,
       "user":{"slug":"ann","displayName":"Ann"},"contents":{"html":"<p>reply</p>"}}]}}})");
  Document d = lesswrong_post_document(post, ref("lesswrong:posts/P1/s"));
  const Node* top = message(d, "P1");
  const Node* c1 = message(d, "C1");
  const Node* c2 = message(d, "C2");
  const Node* c3 = message(d, "C3");
  EXPECT(top && c1 && c2 && c3);
  if (c1) EXPECT_EQ(c1->alt, "P1");
  if (c2) EXPECT_EQ(c2->alt, "C1");
  if (c3) EXPECT_EQ(c3->src, "[deleted]");
  // Depth first: C2 right after its parent C1, before C3.
  std::string order;
  for (const Node& n : d.nodes)
    if (n.kind == NodeKind::Message) order += n.label + ",";
  EXPECT_EQ(order, "P1,C1,C2,C3,");
  EXPECT(has_href(d, "#id:C2"));
  EXPECT(has_href(d, "lesswrong:posts/Q9/other"));
  EXPECT(has_href(d, "lesswrong:users/brendan-long"));
  EXPECT(has_href(d, "lesswrong:w/practical"));
  EXPECT(has_href(d, "lesswrong:posts/P1/s#id:C1"));  // comment date
  EXPECT(all_text(d).find("36 karma") != std::string::npos);

  // Listing: one entry per post, linkpost host, no pager below a full page.
  Json list = parse(R"({"data":{"posts":{"results":[{"_id":"A","slug":"a","title":"T",
      "postedAt":"2026-11-19T14:33:52Z","baseScore":171,"commentCount":7,
      "url":"https://www.example.org/p","question":true,
      "user":{"slug":"so8res","displayName":"So8res"},"coauthors":[{"slug":"x","displayName":"X"}],
      "tags":[{"name":"AI","slug":"ai"}]}]}}})");
  Document l = lesswrong_list_document(list, ref("lesswrong:"));
  EXPECT(count_kind(l, NodeKind::Article) == 1);
  EXPECT(has_href(l, "lesswrong:posts/A/a"));
  EXPECT(has_href(l, "lesswrong:users/x"));
  EXPECT(!has_href(l, "lesswrong:?page=2"));
  const std::string lt = all_text(l);
  EXPECT(lt.find("(example.org)") != std::string::npos);
  EXPECT(lt.find("[Question] T") != std::string::npos);
  // The "LessWrong: Frontpage · New · …" bar is a <nav> region (Page panel).
  {
    const Node& bar = l.get(l.get(l.root).children.at(0));
    EXPECT(bar.kind == NodeKind::Chrome);
    EXPECT_EQ(bar.summary, "navigation");
    EXPECT(margin_chrome(l, bar));
  }

  // Search: posts, tags and users of one multi-query reply.
  Json found = parse(R"([{"hits":[{"objectID":"P1","slug":"s","title":"Secrets","url":null,
      "baseScore":36,"commentCount":6,"postedAt":"2026-02-23T20:12:06.950Z",
      "authorSlug":"brendan-long","authorDisplayName":"Brendan Long","tags":[]}],
      "nbHits":40,"nbPages":2,"page":0},
    {"hits":[{"name":"RSS","slug":"rss"}]},{"hits":[{"displayName":"Ann","slug":"ann"}]}])");
  Document f = lesswrong_search_document(found, ref("lesswrong:search?query=rss"));
  EXPECT(has_href(f, "lesswrong:posts/P1/s"));
  EXPECT(has_href(f, "lesswrong:w/rss"));
  EXPECT(has_href(f, "lesswrong:users/ann"));
  EXPECT(has_href(f, "lesswrong:search?query=rss&page=2"));
  EXPECT(all_text(f).find("40 posts found") != std::string::npos);

  // Unknown user: a note, no crash.
  Document u = lesswrong_user_document(parse(R"({"data":{"user":{"result":null}}})"), Json{},
                                       ref("lesswrong:users/nobody"));
  EXPECT(all_text(u).find("No such user.") != std::string::npos);
}

// Digg: records as the public MCP tools send them (2026-11), cut down.
void test_digg() {
  constexpr const char* kPost = "13cbc94a-13c4-4177-ad08-bdf3146cf788";
  // Refs, their web forms, and the levels up.
  EXPECT(is_forum_ref("digg:"));
  EXPECT(is_forum_ref("digg:ai/newest"));
  EXPECT(is_forum_ref("digg:ai/top/7d"));
  EXPECT(is_forum_ref("digg:ai/7xbz1s0w"));
  EXPECT(is_forum_ref("digg:search/newest?query=gemini"));
  EXPECT(is_forum_ref("digg:post/13cbc94a-13c4-4177-ad08-bdf3146cf788?page=3#id:x"));
  EXPECT(is_forum_ref("digg:post/13cbc94a-13c4-4177-ad08-bdf3146cf788/comments?cursor=abc"));
  EXPECT(!is_forum_ref("digg:nope"));
  EXPECT(!is_forum_ref("digg:ai/newest/7d"));     // ranges are for Top only
  EXPECT(!is_forum_ref("digg:all/7xbz1s0w"));     // stories live under a topic
  EXPECT(!is_forum_ref("digg:search"));           // needs a query
  EXPECT(!is_forum_ref("digg:ai?query=x"));
  EXPECT(!is_forum_ref("digg:ai?page=2"));        // feeds go by cursor
  EXPECT(!is_forum_ref("digg:post/13cbc94a?page=2"));
  EXPECT(!is_forum_ref("digg:post/13cbc94a-13c4-4177-ad08-bdf3146cf788?page=26"));
  EXPECT_EQ(forum_web_url(ref("digg:")), "https://digg.com/");
  EXPECT_EQ(forum_web_url(ref("digg:ai/top/7d")), "https://digg.com/ai?filter=top");
  EXPECT_EQ(forum_web_url(ref("digg:all/newest")), "https://digg.com/?filter=newest");
  EXPECT_EQ(forum_web_url(ref("digg:ai/7xbz1s0w")), "https://digg.com/ai/7xbz1s0w");
  EXPECT_EQ(forum_web_url(ref("digg:search?query=abc")), "");
  EXPECT_EQ(forum_ref_from_web_url("https://digg.com/"), "digg:");
  EXPECT_EQ(forum_ref_from_web_url("https://digg.com/?filter=hot"), "digg:all/hot");
  EXPECT_EQ(forum_ref_from_web_url("https://digg.com/tech?filter=top"), "digg:tech/top");
  EXPECT_EQ(forum_ref_from_web_url("https://digg.com/ai/7xbz1s0w"), "digg:ai/7xbz1s0w");
  EXPECT_EQ(forum_ref_from_web_url("https://digg.com/u/someone"), "");
  EXPECT_EQ(forum_up_ref(ref("digg:ai/top/7d")), "digg:ai/top");
  EXPECT_EQ(forum_up_ref(ref("digg:ai/7xbz1s0w")), "digg:ai");
  EXPECT_EQ(forum_up_ref(ref("digg:ai")), "digg:");
  EXPECT_EQ(forum_up_ref(ref("digg:post/13cbc94a-13c4-4177-ad08-bdf3146cf788/comments")),
            std::string("digg:post/") + kPost);

  // Tool arguments: Top has a range; long cursors survive the ref.
  const std::string cursor(2900, 'Q');
  ForumRef top = ref("digg:ai/top?cursor=a%2Bb");
  EXPECT_EQ(digg_feed_arguments(top),
            R"({"topic":"ai","view":"top","range":"today","limit":20,"cursor":"a+b"})");
  EXPECT_EQ(::ref("digg:ai?cursor=" + cursor).cursor, cursor);
  EXPECT_EQ(digg_feed_arguments(ref("digg:")),
            R"({"topic":"all","view":"trending","limit":20})");
  EXPECT_EQ(digg_search_arguments(ref("digg:search/score?query=a%20b")),
            R"({"query":"a b","sort":"score","limit":30})");
  EXPECT_EQ(digg_post_arguments(kPost, 3),
            std::string(R"({"id":")") + kPost + R"(","sourceOffset":40,"sourceLimit":20})");

  // The MCP envelope: event stream, structured content or JSON text, errors.
  auto j = digg_mcp_decode(
      "event: message\r\ndata: {\"result\":{\"content\":[{\"type\":\"text\","
      "\"text\":\"{\\\"posts\\\":[]}\"}]},\"jsonrpc\":\"2.0\",\"id\":1}\r\n\r\n");
  EXPECT(j && json_get(*j, "posts"));
  j = digg_mcp_decode(R"({"result":{"content":[],"structuredContent":{"nextCursor":null}}})");
  EXPECT(j && json_get(*j, "nextCursor"));
  std::string err;
  EXPECT(!digg_mcp_decode(R"({"result":{"isError":true,"content":[{"type":"text","text":"bad id"}]}})", &err));
  EXPECT_EQ(err, "Digg: bad id");

  // A feed: native story links, previews, pictures from the web page, More.
  const std::string feed_html =
      "<img src=\"https://cdn.digg.com/cdn-cgi/image/width=640,quality=80,format=auto/"
      "editorial-story-images/13cbc94a-13c4-4177-ad08-bdf3146cf788/0638fdc8.jpg\">"
      "{\\\"x\\\":\\\"editorial-story-images/not-a-uuid/a.jpg\\\"}";
  const DiggImages images = digg_web_images(feed_html);
  EXPECT_EQ(images.size(), 1u);
  Json feed = parse(R"({"posts":[{"id":"13cbc94a-13c4-4177-ad08-bdf3146cf788",
      "url":"https://digg.com/tech/7xbz1s0w","topic":"tech","title":"Delta lifts ground stop",
      "description":"Delta says its Atlanta ground stop has been lifted.","tldr":null,
      "feedEntryAt":"2026-11-22T18:00:00Z","publishedAt":"2026-11-22T17:51:00.123Z",
      "updatedAt":"2026-11-22T18:10:00Z"}],
      "nextCursor":"c1+/=","asOf":"2026-11-22T18:20:00Z","view":"trending","range":null,
      "unavailableCount":0})");
  Document f = digg_list_document(feed, ref("digg:"), images);
  EXPECT(has_href(f, std::string("digg:post/") + kPost));
  EXPECT(has_href(f, "digg:tech"));
  EXPECT(has_href(f, "https://digg.com/tech/7xbz1s0w"));
  EXPECT(has_href(f, "digg:?cursor=c1%2B%2F%3D"));
  EXPECT(has_href(f, "digg:all/top"));
  EXPECT_EQ(count_kind(f, NodeKind::Header), 1u);  // the page title; entries are bold links
  EXPECT_EQ(count_kind(f, NodeKind::Image), 1u);
  EXPECT(all_text(f).find("cdn.digg.com/cdn-cgi/image/width=240,height=160,fit=cover,quality=85,format=jpeg/"
                          "editorial-story-images/13cbc94a") != std::string::npos);
  EXPECT(all_text(f).find("Atlanta ground stop has been lifted") != std::string::npos);
  // The Top feed offers its time windows.
  Document t = digg_list_document(Json{}, ref("digg:ai/top/7d"));
  EXPECT(has_href(t, "digg:ai/top/30d"));
  EXPECT(all_text(t).find("No stories.") != std::string::npos);
  // The static JSON feed: relative addresses, no topic.
  Json clusters = parse(R"({"clusters":[{"id":"13cbc94a-13c4-4177-ad08-bdf3146cf788",
      "short_id":"7xbz1s0w","url":"/ai/7xbz1s0w","title":"T","tldr":"Brief.",
      "story_count":83,"first_post_at":"2026-11-20T10:00:00Z"}]})");
  Document c = digg_list_document(clusters, ref("digg:ai"));
  EXPECT(has_href(c, "https://digg.com/ai/7xbz1s0w"));
  EXPECT(all_text(c).find("83 sources") != std::string::npos);
  // Search results count their matches.
  Json found = parse(R"({"posts":[{"id":"13cbc94a-13c4-4177-ad08-bdf3146cf788","title":"T",
      "topic":"ai","diggScore":71,"sourceCount":12,"uniqueAuthors":9,"firstSourceAt":null}],
      "nextCursor":null,"totalMatches":41})");
  Document s = digg_list_document(found, ref("digg:search?query=gemini"));
  EXPECT(all_text(s).find("41 stories found") != std::string::npos);
  EXPECT(all_text(s).find("9 authors") != std::string::npos);
  EXPECT(has_href(s, "digg:search/newest?query=gemini"));
  EXPECT(!has_href(s, "digg:search?query=gemini&cursor="));

  // A story page: the web page's id, picture and article.
  const std::string page =
      R"(<article data-cluster-id="aaaaaaaa-0000-0000-0000-000000000000"></article>)"
      R"(<div data-cluster-detail="" data-cluster-detail-url-cluster-id="7xbz1s0w" )"
      R"(data-cluster-id="13cbc94a-13c4-4177-ad08-bdf3146cf788" class="min-h-screen">)"
      R"(<img src="https://cdn.digg.com/cdn-cgi/image/width=1080/editorial-story-images/)"
      R"(13cbc94a-13c4-4177-ad08-bdf3146cf788/0638fdc8.jpg"><div>Image: @cnnbrk on X</div>)"
      R"(<section aria-label="Body" data-cluster-article-body="true" class="typeset">)"
      R"(<p>Delta says it is fine. <a href="https://example.org/a" class="text-link-always">Source</a></p>)"
      R"(<div data-article-featured-source="true"><section><iframe src="x"></iframe><p>Embed</p></section></div>)"
      R"(<h2>Why it matters</h2><p>Flights resumed.</p></section>)";
  const DiggWebStory web = digg_web_story(page);
  EXPECT_EQ(web.id, kPost);
  EXPECT_EQ(web.image, std::string(kPost) + "/0638fdc8.jpg");
  EXPECT_EQ(web.image_credit, "@cnnbrk on X");
  EXPECT(web.article_html.find("Flights resumed") != std::string::npos);
  EXPECT(web.article_html.find("Embed") == std::string::npos);
  EXPECT(digg_web_story("<p>404</p>").id.empty());

  Json post = parse(R"({"story":{"id":"13cbc94a-13c4-4177-ad08-bdf3146cf788",
      "url":"https://digg.com/tech/7xbz1s0w","topic":"tech","title":"Delta lifts ground stop",
      "description":"Delta says its ground stop was lifted.","tldr":"Flights resume.",
      "feedEntryAt":"2026-11-22T18:00:00Z","publishedAt":"2026-11-22T17:51:00Z",
      "updatedAt":"2026-11-22T19:30:00Z"},
    "follows":{"id":"6d35f561-a7db-40fb-b2d9-8256c53b0abb","url":"https://digg.com/tech/aaaaaaaa",
      "title":"Delta outage halts flights","publishedAt":"2026-11-22T12:00:00Z"},"newer":null,
    "sources":[
      {"id":"s1","kind":"x","url":"https://x.com/cnnbrk/status/1","author":"CNN Breaking News",
       "title":null,"text":"Delta lifts ground stop https://cnn.com/x","publishedAt":"2026-11-22T17:40:00Z"},
      {"id":"s2","kind":"webpage","url":"https://www.bloomberg.com/news/a","author":"Mary Schlangenstein",
       "title":"Delta Tech Outage","text":"Delta Air Lines said…","publishedAt":null}],
    "nextSourceOffset":20,"sourceLimitReached":false})");
  DiggPost extra;
  extra.web = web;
  extra.comments = parse(R"({"comments":[
      {"id":"6bf1d543-05d6-4ae1-9a23-ff391fb84322","postId":"13cbc94a-13c4-4177-ad08-bdf3146cf788",
       "parentId":null,"repliedToCommentId":null,"state":"published","body":"Finally.","gif":null,
       "author":{"username":"ann","name":"Ann","avatarUrl":null,"url":"https://digg.com/u/ann"},
       "diggCount":3,"replyCount":1,"createdAt":"2026-11-16T20:18:36.996177+00:00","editedAt":null},
      {"id":"3fbd1527-9875-4308-b73b-ac1dfd67fae5","postId":"13cbc94a-13c4-4177-ad08-bdf3146cf788",
       "parentId":null,"repliedToCommentId":null,"state":"unavailable","body":null,"gif":null,
       "author":null,"diggCount":0,"replyCount":2,"createdAt":"2026-11-16T21:00:00+00:00","editedAt":null}],
      "commentCount":4,"locked":false,"nextCursor":"n1"})");
  extra.replies.emplace("6bf1d543-05d6-4ae1-9a23-ff391fb84322", parse(R"({"replies":[
      {"id":"77777777-0000-0000-0000-000000000001","postId":"13cbc94a-13c4-4177-ad08-bdf3146cf788",
       "parentId":"6bf1d543-05d6-4ae1-9a23-ff391fb84322","repliedToCommentId":"6bf1d543-05d6-4ae1-9a23-ff391fb84322",
       "state":"published","body":"@ann Indeed.","gif":null,
       "author":{"username":"bob","name":"","avatarUrl":null,"url":"https://digg.com/u/bob"},
       "diggCount":0,"replyCount":0,"createdAt":"2026-11-16T22:00:00+00:00","editedAt":"2026-11-16T22:05:00+00:00"}],
      "replyCount":1,"nextCursor":null})"));
  Document d = digg_post_document(post, ref("digg:tech/7xbz1s0w"), extra);
  EXPECT_EQ(d.meta.title, "Delta lifts ground stop");
  const std::string text = all_text(d);
  EXPECT(has_href(d, "https://digg.com/tech/7xbz1s0w"));
  EXPECT(has_href(d, "digg:post/6d35f561-a7db-40fb-b2d9-8256c53b0abb"));  // earlier story
  EXPECT(text.find("updated 2026-11-22 19:30 UTC") != std::string::npos);
  EXPECT(text.find("Image: @cnnbrk on X") != std::string::npos);
  EXPECT(text.find("Flights resume.") != std::string::npos);
  EXPECT(text.find("Why it matters") != std::string::npos);
  EXPECT(has_href(d, "https://example.org/a"));
  // Comments with replies under their parent.
  EXPECT(text.find("Comments (4)") != std::string::npos);
  const Node* reply = message(d, "77777777-0000-0000-0000-000000000001");
  EXPECT(reply && reply->alt == "6bf1d543-05d6-4ae1-9a23-ff391fb84322");
  EXPECT(reply && reply->src == "bob");
  EXPECT(has_href(d, "https://digg.com/u/ann"));
  EXPECT(has_href(d, std::string("digg:post/") + kPost + "#id:6bf1d543-05d6-4ae1-9a23-ff391fb84322"));
  EXPECT(text.find("Comment not available.") != std::string::npos);
  EXPECT(text.find("edited 2026-11-16 22:05 UTC") != std::string::npos);
  EXPECT(has_href(d, std::string("digg:post/") + kPost +
                         "/comments/3fbd1527-9875-4308-b73b-ac1dfd67fae5"));
  EXPECT(has_href(d, std::string("digg:post/") + kPost + "/comments?cursor=n1"));
  // Sources: X author and handle; web page title, author and host.
  EXPECT(has_href(d, "https://x.com/cnnbrk/status/1"));
  EXPECT(text.find("CNN Breaking News|") != std::string::npos);
  EXPECT(text.find("@cnnbrk") != std::string::npos);
  EXPECT(text.find("Mary Schlangenstein") != std::string::npos);
  EXPECT(text.find("bloomberg.com") != std::string::npos);
  EXPECT(has_href(d, std::string("digg:post/") + kPost + "?page=2"));
  // Later pages: only sources.
  Document p2 = digg_post_document(post, ::ref(std::string("digg:post/") + kPost + "?page=2"));
  EXPECT(all_text(p2).find("Sources, page 2") != std::string::npos);
  EXPECT(all_text(p2).find("Delta says its ground stop") == std::string::npos);
  EXPECT(has_href(p2, std::string("digg:post/") + kPost + "?page=3"));

  // The replies to one comment.
  Document r = digg_comments_document(
      parse(R"({"replies":[],"replyCount":0,"nextCursor":null})"),
      ::ref(std::string("digg:post/") + kPost + "/comments/6bf1d543-05d6-4ae1-9a23-ff391fb84322"));
  EXPECT(all_text(r).find("No replies.") != std::string::npos);
  EXPECT(has_href(r, std::string("digg:post/") + kPost));
}
}  // namespace

void test_forums(int& passed, int& failed) {
  test_refs();
  test_discourse();
  test_lobsters();
  test_fourchan();
  test_stackexchange();
  test_bluesky();
  test_flarum();
  test_lesswrong();
  test_digg();
  passed += g_passed;
  failed += g_failed;
}
