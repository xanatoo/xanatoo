// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
//
// Standalone offline regression for Digg public MCP result handling.
// Does not need SDL, TLS or a live Digg account/server.

#include "xanatoo/forums.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

using namespace xanatoo;

// Always check, even for Release builds with NDEBUG.
#define CHECK(expr) do {                                                  \
  if (!(expr)) {                                                           \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr);     \
    std::abort();                                                           \
  }                                                                        \
} while (false)

namespace {

constexpr const char* kUuid = "01234567-89ab-cdef-0123-456789abcdef";

Json parse(std::string_view s) {
  auto v = json_parse(s);
  CHECK(v);
  return *v;
}

ForumRef address(std::string_view s) {
  auto r = parse_forum_ref(s);
  CHECK(r);
  return *r;
}

bool link(const Document& doc, const std::string& href) {
  for (const Node& node : doc.nodes)
    for (const InlineSpan& span : node.spans)
      if (span.href == href) return true;
  return false;
}

bool says(const Document& doc, std::string_view word) {
  for (const Node& node : doc.nodes)
    for (const InlineSpan& span : node.spans)
      if (span.text.find(word) != std::string::npos) return true;
  return false;
}

void refs() {
  CHECK(address("digg:").site == ForumSite::Digg);
  CHECK(address("digg:ai/top").path == "ai/top");
  CHECK(address("digg:all/newest").path == "all/newest");
  CHECK(address("digg:search?query=moon+landing").query == "moon landing");
  CHECK(forum_ref_string(address("digg:search?query=moon+landing")) ==
         "digg:search?query=moon%20landing");
  CHECK(address("digg:post/01234567-89ab-cdef-0123-456789abcdef").site == ForumSite::Digg);
  CHECK(!is_forum_ref("digg:unknown"));
  CHECK(!is_forum_ref("digg:ai/posts/foo"));
  CHECK(!is_forum_ref("digg:/top"));
  CHECK(!is_forum_ref("digg:post/abc"));
  CHECK(!is_forum_ref("digg:search"));
  CHECK(!is_forum_ref("digg:science?query=a"));
  CHECK(forum_web_url(address("digg:ai")) == "https://digg.com/ai");
  CHECK(forum_web_url(address("digg:all/newest")) == "https://digg.com/");
  CHECK(forum_web_url(address("digg:post/01234567-89ab-cdef-0123-456789abcdef")).empty());
  CHECK(forum_ref_from_web_url("https://digg.com/tech") == "digg:tech");
  CHECK(forum_ref_from_web_url("https://digg.com/") == "digg:");
  CHECK(forum_ref_from_web_url("https://digg.com/ai/abc1234z").empty());
  CHECK(forum_ref_from_web_url("https://evil.digg.com/tech").empty());
  CHECK(forum_up_ref(address("digg:science/top")) == "digg:science");
  CHECK(forum_up_ref(address("digg:science")) == "digg:");
  CHECK(forum_up_ref(address("digg:post/01234567-89ab-cdef-0123-456789abcdef")) == "digg:");
  CHECK(forum_up_ref(address("digg:")) == "");
  const auto cursor = address("digg:ai?cursor=a%2Fb%3Dc");
  CHECK(forum_ref_string(cursor) == "digg:ai?cursor=a%2Fb%3Dc");
}

void feed_arguments() {
  // Live API contract: digg_get_feed.limit must not exceed 20.
  const Json home = parse(digg_feed_arguments(address("digg:")));
  CHECK(json_string(json_path(home, "topic")) == "all");
  CHECK(json_number(json_path(home, "limit")) == 20);
  CHECK(!json_path(home, "view"));
  CHECK(!json_path(home, "cursor"));

  const Json more = parse(digg_feed_arguments(address("digg:science/newest?cursor=a%2Fb%3Dc")));
  CHECK(json_string(json_path(more, "topic")) == "science");
  CHECK(json_number(json_path(more, "limit")) == 20);
  CHECK(json_string(json_path(more, "view")) == "newest");
  CHECK(json_string(json_path(more, "cursor")) == "a/b=c");
}

void search_arguments() {
  CHECK(address("digg:search?query=moon&cursor=a%2Fb%3Dc").cursor == "a%2Fb%3Dc");
  const Json args = parse(digg_search_arguments(address(
      "digg:search?query=moon+landing&cursor=a%2Fb%3Dc")));
  CHECK(json_string(json_path(args, "query")) == "moon landing");
  CHECK(json_string(json_path(args, "cursor")) == "a/b=c");
  CHECK(json_number(json_path(args, "limit")) == 30);
  CHECK(!json_path(parse(digg_search_arguments(address("digg:search?query=test"))),
                   "cursor"));
}

void polished_layout() {
  Document home = digg_list_document(parse(R"({"posts":[]})"), address("digg:"));
  CHECK(link(home, "digg:all/top"));
  CHECK(!link(home, "digg:all/trending"));  // Home defaults to Trending
  Document topic = digg_list_document(parse(R"({"posts":[]})"), address("digg:science"));
  CHECK(link(topic, "digg:science/trending"));
  CHECK(!link(topic, "digg:science/top"));  // Topic defaults to Top
  CHECK(link(topic, "digg:science/hot"));
  CHECK(link(topic, "digg:science/best"));
  Document all_top = digg_list_document(parse(R"({"posts":[]})"),
      address("digg:all/top"));
  CHECK(!link(all_top, "digg:"));  // All is the selected topic
  CHECK(!link(all_top, "digg:all/top"));

  // Sorting belongs to navigation Chrome (including the Page panel), not
  // between the feed headline and its first article.
  int sort_rows_in_chrome = 0, sort_rows_in_thread = 0;
  for (const Node& n : topic.nodes) {
    for (const InlineSpan& sp : n.spans) {
      if (sp.text != "Sort: ") continue;
      if (topic.get(n.parent).kind == NodeKind::Chrome) ++sort_rows_in_chrome;
      if (topic.get(n.parent).kind == NodeKind::Thread) ++sort_rows_in_thread;
    }
  }
  CHECK(sort_rows_in_chrome == 1);
  CHECK(sort_rows_in_thread == 0);

  const ForumRef post = address("digg:post/01234567-89ab-cdef-0123-456789abcdef");
  Document doc = digg_post_document(parse(R"JSON({
    "post":{"title":"Grouped coverage", "id":"01234567-89ab-cdef-0123-456789abcdef",
            "url":"https://digg.com/science/a8bx21dc", "topic":"science",
            "diggScore":42, "sourceCount":5, "distinctSourceAuthorCount":2,
            "publishedAt":"2026-11-20T11:00:00Z",
            "updatedAt":"2026-11-22T13:00:00Z", "summary":"Digg editorial summary"},
    "sources":[
       {"title":"First publisher", "url":"https://first.example/one", "domain":"first.example",
        "author":"Reporter One", "type":"web", "publishedAt":"2026-11-20T12:00:00Z",
        "excerpt":"An excerpt unique to the first publisher"},
       {"author":{"displayName":"Researcher"}, "url":"https://x.com/researcher/status/123", "type":"x",
        "text":"First-hand commentary from X"},
       {"title":"Unsafe link", "url":"javascript:alert(1)", "excerpt":"Still plain text"}
    ], "nextSourceOffset":3, "totalSources":5
  })JSON"), post);
  CHECK(says(doc, "Score 42"));
  CHECK(says(doc, "2 source authors"));
  CHECK(says(doc, "Updated 2026-11-22"));
  CHECK(link(doc, "digg:science"));
  CHECK(says(doc, "Sources (5)"));
  CHECK(link(doc, "https://first.example/one"));
  CHECK(link(doc, "https://x.com/researcher/status/123"));
  CHECK(says(doc, "Researcher"));
  CHECK(!link(doc, "javascript:alert(1)"));
  CHECK(says(doc, "by Reporter One"));
  CHECK(says(doc, "X"));
  CHECK(says(doc, "See all sources on Digg"));
  CHECK(link(doc, "https://digg.com/science/a8bx21dc"));
  // Sources without publisher/date/author must not produce blank rows.
  Document sparse_source = digg_post_document(parse(R"JSON({
    "title":"Short post", "summary":"One line", "sources":[
      {"title":"Only a link", "url":"https://simple.example/article"}]
  })JSON"), post);
  int empty_source_captions = 0;
  for (const Node& n : sparse_source.nodes)
    if (n.kind == NodeKind::Caption && n.spans.empty())
      ++empty_source_captions;
  CHECK(empty_source_captions == 0);

  // The page title is shown by the Thread, not repeated in an Article.
  int title_lines = 0, source_articles = 0, excerpts_in_sources = 0;
  for (const Node& n : doc.nodes) {
    if (n.kind == NodeKind::Header)
      for (const InlineSpan& sp : n.spans)
        if (sp.text == "Grouped coverage") ++title_lines;
    if (n.kind == NodeKind::Article && n.label.find("source-") == 0)
      ++source_articles;
    if (n.kind == NodeKind::Paragraph &&
        doc.get(n.parent).kind == NodeKind::Article &&
        doc.get(n.parent).label.find("source-") == 0)
      for (const InlineSpan& sp : n.spans)
        if (sp.text.find("excerpt unique") != std::string::npos ||
            sp.text.find("First-hand commentary") != std::string::npos)
          ++excerpts_in_sources;
  }
  CHECK(title_lines == 1);
  CHECK(source_articles == 3);
  CHECK(excerpts_in_sources == 2);

  // Source headlines are readable body-size links, not twenty competing
  // major headings. Only the story title and Sources should be large.
  int major_headers = 0, source_links_on_paragraphs = 0;
  for (const Node& n : doc.nodes) {
    if (n.kind == NodeKind::Header) ++major_headers;
    if (n.kind == NodeKind::Paragraph &&
        doc.get(n.parent).kind == NodeKind::Article &&
        doc.get(n.parent).label.find("source-") == 0) {
      for (const InlineSpan& sp : n.spans)
        if (sp.kind == InlineKind::Link && (sp.marks & kMarkStrong))
          ++source_links_on_paragraphs;
    }
  }
  CHECK(major_headers == 3);  // post headline + Comments + Sources
  CHECK(source_links_on_paragraphs == 2);

  // A publisher address in `post.url` must not be mislabelled a Digg page.
  Document misleading = digg_post_document(parse(R"({"title":"External link",
    "url":"https://publisher.example/important", "summary":"Summary"})"), post);
  CHECK(!link(misleading, "https://publisher.example/important"));
  CHECK(!says(misleading, "View on Digg"));
}

// Images and editorial metadata must survive the JSON->Document boundary.
// These are synthetic field shapes, not a claim that all are guaranteed by MCP.
void media_and_metadata() {
  Document feed = digg_list_document(parse(R"JSON({"posts":[
    {"title":"Story with a picture","thumbnailUrl":"https://cdn.digg.com/cover.jpg",
     "diggScore":8, "sourceCount":4, "distinctSourceAuthorCount":3},
    {"title":"Nested video poster", "media":[{"type":"video",
     "url":"https://video.example/movie.mp4",
     "thumbnail":{"url":"https://cdn.digg.com/poster.jpg", "alt":"Video still"}}]},
    {"title":"Invalid image","imageUrl":"javascript:alert(1)"},
    {"title":"No image"},
    {"title":"Malformed nested image", "thumbnail": {"thumbnail": {
     "thumbnail":{"url":"https://cdn.digg.com/deep.jpg"}}}}
  ]})JSON"), address("digg:"));
  int pics = 0;
  for (const Node& n : feed.nodes) {
    if (n.kind != NodeKind::Image) continue;
    ++pics;
    CHECK(n.float_side == "right");
    CHECK(n.src == "https://cdn.digg.com/cover.jpg" ||
          n.src == "https://cdn.digg.com/poster.jpg");
    if (n.src.ends_with("poster.jpg")) CHECK(n.alt == "Video still");
  }
  CHECK(pics == 2);
  CHECK(says(feed, "3 source authors"));

  Document post = digg_post_document(parse(R"JSON({"post":{
    "title":"Full Digg story", "url":"https://digg.com/ai/aabbccdd",
    "heroImage":{"url":"https://cdn.digg.com/hero.jpg", "originalUrl":"https://cdn.digg.com/hero-full.jpg",
                 "altText":"Hero picture","credit":"Photographer Example"},
    "article":{"sections":[
      {"heading":"Key findings", "paragraphs":["First reported fact", "Second reported fact"]},
      {"type":"image","url":"https://cdn.digg.com/chart.jpg", "alt":"Chart"}
    ]}, "entities":[{"name":"Acme","url":"https://digg.com/entities/abcd"}, "Other"],
    "usefulLinks":[{"url":"https://example.org/report","title":"Primary report"}],
    "relatedPosts":[{"id":"01234567-89ab-cdef-0123-456789abcdef", "title":"Followup"}],
    "combinedViews":8300,"combinedLikes":42,"distinctSourceAuthorCount":7,
    "summary":"Editorial summary"
  },"sources":[{"url":"https://publisher.example/a", "title":"Original coverage"}]})JSON"),
    address("digg:post/01234567-89ab-cdef-0123-456789abcdef"));
  CHECK(says(post, "Photographer Example"));
  CHECK(says(post, "Key findings"));
  CHECK(says(post, "First reported fact"));
  CHECK(says(post, "Second reported fact"));
  CHECK(says(post, "8300 source views"));
  CHECK(says(post, "42 source likes"));
  CHECK(says(post, "7 source authors"));
  CHECK(says(post, "Useful links"));
  CHECK(says(post, "Related stories"));
  CHECK(says(post, "Tags:"));
  CHECK(link(post, "https://example.org/report"));
  CHECK(link(post, "digg:post/01234567-89ab-cdef-0123-456789abcdef"));
  CHECK(link(post, "https://digg.com/entities/abcd"));
  CHECK(!says(post, "no editorial summary"));
  int images = 0, hero_links = 0;
  for (const Node& n : post.nodes) {
    if (n.kind != NodeKind::Image) continue;
    ++images;
    CHECK(n.float_side.empty());
    if (n.src == "https://cdn.digg.com/hero.jpg") {
      CHECK(n.alt == "Hero picture");
      for (const InlineSpan& span : n.spans)
        if (span.href == "https://cdn.digg.com/hero-full.jpg") ++hero_links;
    } else CHECK(n.src == "https://cdn.digg.com/chart.jpg");
  }
  CHECK(images == 2);
  CHECK(hero_links == 1);

  // Missing metadata must not produce empty placeholder sections or images.
  Document sparse = digg_post_document(parse(R"({"title":"Empty", "summary":"Only text"})"),
      address("digg:post/01234567-89ab-cdef-0123-456789abcdef"));
  for (const Node& n : sparse.nodes) {
    CHECK(n.kind != NodeKind::Image);
    CHECK(n.label != "related" && n.label != "useful-links");
  }
}

void comments_document() {
  const ForumRef post = address("digg:post/01234567-89ab-cdef-0123-456789abcdef");
  // A post payload with native comments should produce separate Message
  // nodes, not re-label third-party Sources as Digg comments.
  Document doc = digg_post_document(parse(R"JSON({
    "post":{"title":"Discussion", "url":"https://digg.com/ai/abc12345",
            "commentCount":999, "summary":"Editorial text"},
    "discussion":{"comments":[
      {"id":"one","body":"A native comment", "author":{"displayName":"Reader"},
       "createdAt":"2026-11-22T12:00:00Z", "likeCount":3,
       "replies":[{"id":"two","text":"A nested reply", "user":{"username":"Reply user"}}]},
      {"id":"gone", "deleted":true, "replies":[]}
    ]},
    "sources":[{"title":"External post", "url":"https://x.com/test",
                "text":"External conversation is not a Digg comment"}]
  })JSON"), post);
  CHECK(says(doc, "A native comment"));
  CHECK(says(doc, "A nested reply"));
  CHECK(says(doc, "Reader"));
  CHECK(says(doc, "Reply user"));
  CHECK(says(doc, "[deleted]"));
  CHECK(link(doc, "https://digg.com/ai/abc12345"));
  int messages = 0, nested = 0;
  for (const Node& n : doc.nodes) {
    if (n.kind == NodeKind::Message) {
      ++messages;
      if (n.label == "two" && n.alt == "one") ++nested;
    }
  }
  CHECK(messages == 3);
  CHECK(nested == 1);

  // Source engagement is NOT a native Digg comment count. Without an
  // explicit comment list there must be a candid note and external link.
  Document no_comments = digg_post_document(parse(R"JSON({
    "title":"Digg article", "url":"https://digg.com/tech/abc12345",
    "commentCount":543, "sources":[{"text":"X replies"}]
  })JSON"), post);
  CHECK(says(no_comments, "did not include a comment thread"));
  CHECK(says(no_comments, "Read or join the discussion"));
  CHECK(!says(no_comments, "543 comments"));
  for (const Node& n : no_comments.nodes) CHECK(n.kind != NodeKind::Message);

  // The URL may be carried by the envelope rather than `post`.
  Document outer_url = digg_post_document(parse(R"JSON({
    "post":{"title":"Brief"}, "url":"https://digg.com/tech/abcdef12"
  })JSON"), post);
  CHECK(link(outer_url, "https://digg.com/tech/abcdef12"));

  Document empty = digg_post_document(parse(R"JSON({
    "title":"Empty discussion", "comments":[]
  })JSON"), post);
  CHECK(says(empty, "No Digg comments were included"));
  CHECK(!says(empty, "did not include a comment thread"));
}

void mcp_transport() {
  constexpr std::string_view sample = R"({"jsonrpc":"2.0","id":1,
    "result":{"structuredContent":{"posts":[{"title":"A sample"}]}}})";
  auto j = digg_mcp_decode(sample);
  CHECK(j && json_path(*j, "posts/0/title"));
  CHECK(json_string(json_path(*j, "posts/0/title")) == "A sample");
  auto content = digg_mcp_decode(R"({"result":{"content":[{"type":"text",
    "text":"{\"posts\":[{\"title\":\"Another\"}]}"}]}})");
  CHECK(content && json_string(json_path(*content, "posts/0/title")) == "Another");
  const std::string event = "event: message\r\ndata: " +
      std::string(R"({"result":{"structuredContent":{"posts":[{"title":"A sample"}]}}})") +
      "\r\n\r\n";
  j = digg_mcp_decode(event);
  CHECK(j && json_string(json_path(*j, "posts/0/title")) == "A sample");
  std::string error;
  CHECK(!digg_mcp_decode(R"({"error":{"code":-32600,"message":"Denied"}})", &error));
  CHECK(error == "Denied");
  CHECK(!digg_mcp_decode("not JSON", &error));
  CHECK(!digg_mcp_decode(R"({"result":{"content":[{"type":"text","text":"oops"}]}})", &error));
  CHECK(error.find("JSON") != std::string::npos);
}

// A normal MCP reply owns the `result` object in its top-level fields.
// Regression: assigning `reply = *json_get(reply, "result")` used to copy
// from an element of reply.fields into reply itself. Depending on the JSON
// shape this caused heap-use-after-free (confirmed with AddressSanitizer).
// Exercise different result member counts/order, including nested arrays.
void mcp_owned_result_regression() {
  const std::string posts = R"({"posts":[{"id":"01234567-89ab-cdef-0123-456789abcdef",
      "title":"First \u2014 story","summary":"Detail"}]})";
  const std::string content = "\"content\":[{\"type\":\"text\",\"text\":\"not structured JSON\"}]";
  const std::string structured = "\"structuredContent\":" + posts;
  const std::string flag = "\"isError\":false";
  const std::string extra = "\"extra\":{\"nested\":[{\"text\":\"" +
      std::string(512, 'x') + "\"}]}";
  const std::string properties[] = {structured, content, flag, extra};
  for (size_t width = 1; width <= 4; ++width) {
    for (size_t rotate = 0; rotate < 4; ++rotate) {
      std::string fields;
      for (size_t i = 0; i < width; ++i) {
        if (!fields.empty()) fields += ',';
        fields += properties[(i + rotate) % 4];
      }
      // Only replies including structuredContent are expected to unwrap as
      // feed data; other variants still must parse without throwing.
      const std::string response = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{" +
                                   fields + "}}";
      std::string err;
      auto result = digg_mcp_decode(response, &err);
      CHECK(result || !err.empty());
      if (result && fields.find("structuredContent") != std::string::npos) {
        CHECK(json_string(json_path(*result, "posts/0/title")) == "First — story");
        Document doc = digg_list_document(*result, address("digg:"));
        CHECK(says(doc, "First — story"));
      }
    }
  }
  // A full feed page, not just a one-post stub: make sure the decoded tree
  // survives long enough to produce all 20 Document entries.
  std::string page = R"({"jsonrpc":"2.0","id":1,"result":{"structuredContent":{"posts":[)";
  for (int i = 0; i < 20; ++i) {
    if (i) page += ',';
    page += "{\"title\":\"Digg story " + std::to_string(i) +
            "\",\"summary\":\"" + std::string(300, 'x') + "\"}";
  }
  page += R"(]}}})";
  auto feed = digg_mcp_decode(page);
  CHECK(feed);
  Document full = digg_list_document(*feed, address("digg:"));
  int articles = 0;
  for (const Node& n : full.nodes)
    if (n.kind == NodeKind::Article) ++articles;
  CHECK(articles == 20);

  const std::string fail = R"({"jsonrpc":"2.0","id":1,"result":{
    "isError":true,"content":[{"type":"text","text":"rate limited"}]}})";
  std::string err;
  CHECK(!digg_mcp_decode(fail, &err));
  CHECK(err == "rate limited");
}

// Digg publishes editorial copy alongside source links. The MCP detail
// payload may use flat strings, nested copy records, or a separate copy
// section in the result envelope. None of them are full source articles.
void editorial_copy() {
  const ForumRef post = address("digg:post/01234567-89ab-cdef-0123-456789abcdef");
  Document nested = digg_post_document(parse(R"JSON({
    "post":{"title":"Launch coverage", "copy":{
      "dek":"Short introduction", "tldr":{"text":"TLDR of the event"},
      "body":{"text":"Digg's additional editorial paragraph"}}},
    "sources":[{"title":"Source one","url":"https://example.com/article",
                "text":"External source excerpt"}]
  })JSON"), post);
  CHECK(says(nested, "Short introduction"));
  CHECK(says(nested, "TLDR of the event"));
  CHECK(says(nested, "Digg's additional editorial paragraph"));
  CHECK(says(nested, "External source excerpt"));
  CHECK(link(nested, "https://example.com/article"));
  CHECK(says(nested, "Sources"));
  CHECK(!says(nested, "Digg has no editorial summary"));

  Document separate = digg_post_document(parse(R"JSON({
    "post":{"title":"A story without inline copy"},
    "summaryText":"Summary from response envelope",
    "contentText":"Editorial body from response envelope",
    "sources":[{"title":"Example","url":"https://example.com/other"}]
  })JSON"), post);
  CHECK(says(separate, "Summary from response envelope"));
  CHECK(says(separate, "Editorial body from response envelope"));

  Document paragraphs = digg_post_document(parse(R"JSON({
    "post":{"title":"A story with blocks", "content":{
      "paragraphs":["First authored paragraph", {"text":"Second authored paragraph"}]}}
  })JSON"), post);
  CHECK(says(paragraphs, "First authored paragraph"));
  CHECK(says(paragraphs, "Second authored paragraph"));

  Document empty = digg_post_document(parse(R"JSON({
    "post":{"title":"No story copy"},
    "sources":[{"title":"Source summary","description":"Source-only text"}]
  })JSON"), post);
  CHECK(says(empty, "Digg has no editorial summary"));
  CHECK(says(empty, "Source-only text"));
  // A Source description is never promoted to the post's own summary.

  Document repeated = digg_post_document(parse(R"JSON({
    "post":{"title":"Same copy fields", "description":"Only once",
            "summary":"Only once", "body":"Only once"}})JSON"), post);
  int occurrences = 0;
  for (const Node& node : repeated.nodes)
    for (const InlineSpan& span : node.spans)
      if (span.text == "Only once") ++occurrences;
  CHECK(occurrences == 1);

  Document listing = digg_list_document(parse(R"JSON({
    "posts":[{"title":"Feed item", "copy":{"summary":"TLDR inside copy"}}]
  })JSON"), address("digg:"));
  CHECK(says(listing, "TLDR inside copy"));
}

void lists_and_posts() {
  auto payload = parse(R"({"posts":[
    {"id":"01234567-89ab-cdef-0123-456789abcdef", "title":"Reusable launch vehicle",
     "topic":"science", "url":"https://digg.com/science/abcd1234",
     "tldr":"Successful test flight", "diggScore":45,
     "sourceCount":3, "publishedAt":"2026-11-20T13:00:00Z"},
    {"shortId":"efgh5678", "title":"Second story", "url":"https://digg.com/tech/efgh5678"}
  ],"nextCursor":"abc/123="})");
  Document doc = digg_list_document(payload, address("digg:science"));
  CHECK(link(doc, std::string("digg:post/") + kUuid));
  CHECK(link(doc, "https://digg.com/science/abcd1234"));
  CHECK(link(doc, "digg:science?cursor=abc%2F123%3D"));
  CHECK(says(doc, "Successful test flight"));
  int feed_title_headers = 0, strong_link_titles = 0;
  for (const Node& node : doc.nodes) {
    if (node.kind == NodeKind::Header) ++feed_title_headers;
    if (node.kind != NodeKind::Paragraph ||
        doc.get(node.parent).kind != NodeKind::Article) continue;
    for (const InlineSpan& span : node.spans)
      if (span.text == "Reusable launch vehicle" &&
          span.kind == InlineKind::Link && (span.marks & kMarkStrong))
        ++strong_link_titles;
  }
  CHECK(feed_title_headers == 1);  // only the page's own title
  CHECK(strong_link_titles == 1);
  CHECK(says(doc, "3 sources"));
  CHECK(says(doc, "Score 45"));
  CHECK(link(doc, "https://digg.com/tech/efgh5678"));

  Document search = digg_list_document(payload, address("digg:search?query=flight"));
  CHECK(search.meta.title.find("flight") != std::string::npos);
  CHECK(link(search, "digg:search?query=flight&cursor=abc%2F123%3D"));
  // Published static JSON feeds use "clusters", not "posts".
  Document clusters = digg_list_document(parse(R"({"clusters":[{"title":"Short story",
     "clusterUrlId":"abcdef12","topic":"ai"}]})"), address("digg:ai"));
  CHECK(says(clusters, "Short story"));
  CHECK(link(clusters, "https://digg.com/ai/abcdef12"));

  // Sparse entries need no empty metadata spacer.
  Document sparse = digg_list_document(parse(R"({"posts":[{"title":"Minimal"}]})"),
                                        address("digg:"));
  for (const Node& n : sparse.nodes)
    CHECK(n.kind != NodeKind::Caption || !n.spans.empty());

  // Overlong snippets in feeds are previews and should not overwhelm the
  // page or split a multibyte character in the middle.
  const std::string long_copy = std::string(229, 'a') + "é" + std::string(180, 'b');
  Json long_story = parse(R"({"posts":[{"title":"Clipped story"}]})");
  Json summary;
  summary.type = Json::Type::String;
  summary.s = long_copy;
  long_story.fields[0].second.items[0].fields.emplace_back("summary", std::move(summary));
  const Document clipped = digg_list_document(long_story, address("digg:"));
  bool got_preview = false;
  for (const Node& n : clipped.nodes)
    for (const InlineSpan& sp : n.spans)
      if (sp.text.find("aaa") != std::string::npos) {
        CHECK(sp.text.size() <= 240);
        CHECK(sp.text.ends_with("…"));
        // U+00E9 begins exactly at the clipping boundary: it must not be
        // emitted as a dangling first UTF-8 byte.
        CHECK(sp.text.find("é") == std::string::npos);
        CHECK(sp.text.find("\xC3") == std::string::npos);
        got_preview = true;
      }
  CHECK(got_preview);

  Document detail = digg_post_document(parse(R"JSON({"post":{
    "id":"01234567-89ab-cdef-0123-456789abcdef",
    "title":"Reusable launch vehicle","summary":"Engine separation complete",
    "url":"https://digg.com/science/abcd1234","diggScore":12},
    "sources":[{"title":"Space News","url":"https://space.example/story",
                "excerpt":"Primary launch report", "publishedAt":"2026-11-20T12:00:00Z"},
               {"title":"Bad","url":"javascript:alert(1)"}]})JSON"),
                   address("digg:post/01234567-89ab-cdef-0123-456789abcdef"));
  CHECK(says(detail, "Engine separation complete"));
  CHECK(says(detail, "Primary launch report"));
  CHECK(link(detail, "https://space.example/story"));
  CHECK(link(detail, "https://digg.com/science/abcd1234"));
  CHECK(!link(detail, "javascript:alert(1)"));
}

} // namespace

int main() {
  refs();
  feed_arguments();
  search_arguments();
  polished_layout();
  media_and_metadata();
  comments_document();
  mcp_transport();
  mcp_owned_result_regression();
  lists_and_posts();
  editorial_copy();
}
