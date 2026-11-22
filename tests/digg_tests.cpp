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
  CHECK(says(doc, "3 sources"));
  CHECK(says(doc, "45 points"));
  CHECK(link(doc, "https://digg.com/tech/efgh5678"));

  Document search = digg_list_document(payload, address("digg:search?query=flight"));
  CHECK(search.meta.title.find("flight") != std::string::npos);
  CHECK(!link(search, "digg:search?query=flight&cursor=abc%2F123%3D"));
  // Published static JSON feeds use "clusters", not "posts".
  Document clusters = digg_list_document(parse(R"({"clusters":[{"title":"Short story",
     "clusterUrlId":"abcdef12","topic":"ai"}]})"), address("digg:ai"));
  CHECK(says(clusters, "Short story"));
  CHECK(link(clusters, "https://digg.com/ai/abcdef12"));

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
  mcp_transport();
  mcp_owned_result_regression();
  lists_and_posts();
}
