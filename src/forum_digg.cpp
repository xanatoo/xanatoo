// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
//
// Digg's public read-only MCP: Feed, Search and Post. The transport is kept
// separate (forum_fetch.cpp); these are pure JSON -> document mappers.
// An MCP tool result may have structuredContent or JSON text content.

#include "forum_common.hpp"
#include "xanatoo/text_escape.hpp"
#include "xanatoo/util.hpp"

#include <algorithm>
#include <string_view>

namespace xanatoo {

using namespace forum;

namespace {

const Json* object(const Json& obj, std::string_view key) {
  return json_get(obj, key);
}

const Json* first(const Json& obj,
                  std::initializer_list<std::string_view> names) {
  for (std::string_view name : names)
    if (const Json* value = object(obj, name)) return value;
  return nullptr;
}

std::string text_field(const Json& obj,
                       std::initializer_list<std::string_view> names) {
  return json_string(first(obj, names));
}

long long count_field(const Json& obj,
                      std::initializer_list<std::string_view> names) {
  return json_number(first(obj, names));
}

// Digg post copy can be plain text or a small text record. Only traverse
// known copy fields: never mistake a Source excerpt for Digg's own summary.
std::string copy_text(const Json* value) {
  if (!value) return {};
  if (value->type == Json::Type::String) return value->s;
  if (value->type == Json::Type::Object) {
    for (std::string_view name : {"text", "plainText", "markdown", "body", "content"}) {
      if (const Json* child = object(*value, name);
          child && child->type == Json::Type::String && !child->s.empty())
        return child->s;
    }
    // Some copy records contain a sequence of authored paragraphs. Keep
    // their order, without interpreting arbitrary structured metadata as text.
    for (std::string_view name : {"paragraphs", "blocks"}) {
      if (const Json* parts = object(*value, name)) {
        if (parts->type != Json::Type::Array) continue;
        std::string joined;
        for (const Json& part : parts->items) {
          std::string paragraph;
          if (part.type == Json::Type::String) paragraph = part.s;
          else if (part.type == Json::Type::Object)
            paragraph = text_field(part, {"text", "plainText"});
          if (paragraph.empty()) continue;
          if (!joined.empty()) joined += "\n\n";
          joined += paragraph;
        }
        if (!joined.empty()) return joined;
      }
    }
  }
  return {};
}

// `copy` is the editorial copy on Digg's post pages. Other supported shapes
// are the flat post fields and a bounded story/article record. This is a
// data-driven mapping rather than recursive field-name guessing.
std::string post_copy(const Json& post,
                      std::initializer_list<std::string_view> names) {
  for (const Json* obj : {&post, object(post, "copy"), object(post, "story"),
                          object(post, "article"), object(post, "storyCopy")}) {
    if (!obj || obj->type != Json::Type::Object) continue;
    for (std::string_view name : names) {
      std::string text = copy_text(object(*obj, name));
      if (!text.empty()) return text;
    }
  }
  return {};
}

std::string post_intro(const Json& post) {
  return post_copy(post, {"dek", "subtitle", "subheading", "lead", "lede",
                          "leadText", "teaser", "oneLiner", "oneLineSummary",
                          "description"});
}

std::string post_summary(const Json& post) {
  return post_copy(post, {"tldr", "tlDr", "tldrText", "tlDrText",
                          "tldrSummary", "summary", "summaryText", "shortSummary"});
}

std::string post_body(const Json& post) {
  std::string body = post_copy(post, {"body", "bodyText", "articleBody",
                                      "articleText", "content", "contentText",
                                      "storyText", "narrative"});
  if (body.empty()) body = copy_text(object(post, "copy"));
  return body;
}

void add_copy_section(Document& doc, NodeId parent, std::string_view heading,
                      const std::string& text, std::string_view prior1 = {},
                      std::string_view prior2 = {}) {
  if (text.empty() || text == prior1 || text == prior2) return;
  if (!heading.empty())
    add_text(doc, doc.create(NodeKind::Header, parent), std::string(heading),
             InlineKind::Strong);
  append_plain(doc, parent, text);
}

const Json* list_field(const Json& obj,
                       std::initializer_list<std::string_view> names) {
  const Json* a = first(obj, names);
  return a && a->type == Json::Type::Array ? a : nullptr;
}

// Some MCP tools put their data under `data`; static Digg exports use
// `clusters`. Never search recursively for a field (sources also have titles).
const Json& content(const Json& reply) {
  if (const Json* data = object(reply, "data"); data && data->type == Json::Type::Object)
    return *data;
  return reply;
}

bool web_url(std::string_view url) {
  return starts_with_ci(url, "https://") || starts_with_ci(url, "http://");
}

std::string native_post(const Json& post) {
  const std::string uuid = text_field(post, {"id", "postId", "uuid"});
  if (uuid.size() != 36) return {};
  for (size_t i = 0; i < uuid.size(); ++i) {
    const char c = uuid[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return {};
    } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                 (c >= 'A' && c <= 'F'))) return {};
  }
  return "digg:post/" + uuid;
}

std::string post_topic(const Json& post);

std::string post_web(const Json& post) {
  const std::string url = text_field(post, {"url", "postUrl", "permalink"});
  // A link labelled "on Digg" must really resolve to Digg, never a
  // publisher URL accidentally supplied in a feed's `url` field.
  if (starts_with_ci(url, "https://digg.com/") ||
      starts_with_ci(url, "https://www.digg.com/")) return url;
  // Published static JSON cluster feeds may carry the short URL id but
  // no absolute page URL. Only use documented topic slugs and safe ids.
  const std::string id = text_field(post, {"clusterUrlId", "shortId", "urlId"});
  const std::string topic = post_topic(post);
  const bool topic_ok = topic == "ai" || topic == "tech" || topic == "gaming" ||
      topic == "entertainment" || topic == "world-business" ||
      topic == "science" || topic == "sports";
  if (!topic_ok || id.size() < 4 || id.size() > 32) return {};
  for (char c : id)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9'))) return {};
  return "https://digg.com/" + topic + "/" + id;
}

std::string post_topic(const Json& post) {
  const Json* t = object(post, "topic");
  if (t && t->type == Json::Type::Object)
    return text_field(*t, {"slug", "name"});
  return json_string(t);
}

// Digg's public Topics and feed sorts are a small table, not UI-specific
// branches or a second state model. Keep navigation and labels in one place.
struct LabelRef { std::string_view name, slug; };
constexpr LabelRef kTopics[] = {
    {"All", "all"}, {"AI", "ai"}, {"Technology", "tech"},
    {"Gaming", "gaming"}, {"Entertainment", "entertainment"},
    {"World & Business", "world-business"}, {"Science", "science"},
    {"Sports", "sports"},
};
constexpr LabelRef kViews[] = {
    {"Trending", "trending"}, {"Top", "top"}, {"Best", "best"},
    {"Hot", "hot"}, {"Newest", "newest"},
};

std::string topic_name(std::string_view slug) {
  for (const LabelRef& topic : kTopics)
    if (topic.slug == slug) return std::string(topic.name);
  return "All topics";
}

std::string digg_ref(std::string path) { return "digg:" + path; }

std::vector<NavItem> nav_for(const ForumRef& ref) {
  const std::string topic = ref.path.substr(0, ref.path.find('/'));
  std::vector<NavItem> nav;
  for (const LabelRef& item : kTopics) {
    // The Home feed is `digg:`, not `digg:all`.
    const std::string target = item.slug == "all" ? "digg:" :
                               digg_ref(std::string(item.slug));
    nav.push_back({std::string(item.name), target,
                   item.slug == "all" ? (ref.path.empty() || topic == "all") :
                                        topic == item.slug});
  }
  return nav;
}

// A separate navigation row keeps eight topics and five view options from
// becoming one long, hard-to-scan line. The Topic page defaults to Top,
// whereas Home defaults to Trending (as specified by the MCP feed tool).
void add_view_nav(Document& doc, NodeId thread, const ForumRef& ref) {
  if (ref.path == "search" || starts_with(ref.path, "post/")) return;
  const size_t slash = ref.path.find('/');
  const std::string topic = ref.path.empty() ? "all" : ref.path.substr(0, slash);
  const std::string view = slash == std::string::npos ?
      (topic == "all" ? "trending" : "top") : ref.path.substr(slash + 1);
  NodeId nav = doc.create(NodeKind::Chrome, thread);
  doc.get(nav).summary = "navigation";
  NodeId line = doc.create(NodeKind::Paragraph, nav);
  add_text(doc, line, "View: ", InlineKind::Strong);
  for (const LabelRef& item : kViews) {
    if (item.slug != kViews[0].slug) add_meta(doc, line, " · ");
    if (item.slug == view) add_text(doc, line, std::string(item.name), InlineKind::Strong);
    else add_link(doc, line, std::string(item.name),
                  digg_ref(topic + "/" + std::string(item.slug)));
  }
}

void add_timestamp(Document& doc, NodeId caption, const std::string& instant,
                   std::string_view prefix = {}) {
  if (instant.empty()) return;
  add_sep(doc, caption);
  InlineSpan span;
  span.role = InlineRole::Metadata;
  span.text = std::string(prefix) + short_time(instant);
  span.datetime = instant;
  doc.get(caption).spans.push_back(std::move(span));
}

void add_date(Document& doc, NodeId caption, const Json& obj) {
  add_timestamp(doc, caption, text_field(obj,
      {"publishedAt", "createdAt", "firstSourceAt", "firstSourceTime",
       "earliestSourceAt", "earliestSourceTime"}));
}

void add_stats(Document& doc, NodeId caption, const Json& post,
               bool detail = false) {
  if (const Json* score = first(post, {"diggScore", "score"}))
    add_meta(doc, caption, "Digg Score " + std::to_string(json_number(score)));
  if (const Json* sources = first(post, {"sourceCount", "sourcesCount"})) {
    add_sep(doc, caption);
    add_meta(doc, caption, count(json_number(sources), "source", "sources"));
  }
  if (const Json* authors = first(post,
          {"distinctSourceAuthors", "distinctSourceAuthorCount", "sourceAuthorCount"})) {
    const bool numeric = authors->type == Json::Type::Number ||
                         authors->type == Json::Type::String;
    if (numeric || authors->type == Json::Type::Array) {
      const long long n = authors->type == Json::Type::Array ?
          static_cast<long long>(authors->items.size()) : json_number(authors);
      add_sep(doc, caption);
      add_meta(doc, caption, count(n, "source author", "source authors"));
    }
  }
  const std::string topic = post_topic(post);
  if (!topic.empty()) {
    add_sep(doc, caption);
    add_link(doc, caption, topic_name(topic), digg_ref(topic), InlineRole::Metadata);
  }
  add_date(doc, caption, post);
  if (detail) {
    const std::string updated = text_field(post, {"updatedAt", "lastUpdatedAt"});
    const std::string published = text_field(post, {"publishedAt", "createdAt"});
    if (!updated.empty() && updated != published)
      add_timestamp(doc, caption, updated, "Updated ");
  }
}

// The feed is a scan-friendly list of summaries, not a repetition of the
// complete post. Full Digg copy and attributed sources belong on post pages.
void add_story(Document& doc, NodeId thread, const Json& story) {
  const std::string title = text_field(story, {"title", "headline", "label"});
  if (title.empty()) return;
  NodeId art = doc.create(NodeKind::Article, thread);
  doc.get(art).label = text_field(story, {"id", "shortId", "clusterUrlId"});
  const std::string ref = native_post(story);
  const std::string web = post_web(story);
  NodeId heading = doc.create(NodeKind::Header, art);
  if (!ref.empty()) add_link(doc, heading, title, ref);
  else if (!web.empty()) add_link(doc, heading, title, web);
  else add_text(doc, heading, title, InlineKind::Strong);
  NodeId cap = doc.create(NodeKind::Caption, art);
  add_stats(doc, cap, story);
  if (!ref.empty() && !web.empty()) {
    add_sep(doc, cap);
    add_link(doc, cap, "Digg page", web, InlineRole::Metadata);
  }
  const std::string summary = post_summary(story);
  const std::string intro = post_intro(story);
  if (!summary.empty()) append_plain(doc, art, summary);
  else if (!intro.empty()) append_plain(doc, art, intro);
}

// Keep each attributed source's title, attribution, date and excerpt inside
// one Article. Previously excerpts were appended directly to the post, making
// them look like Digg's own text and separating them from their publishers.
void add_source(Document& doc, NodeId section, const Json& source, size_t index) {
  const std::string href = text_field(source, {"url", "sourceUrl", "xUrl"});
  const std::string title = text_field(source, {"title", "headline"});
  std::string author = text_field(source,
      {"authorName", "author", "displayName", "username", "handle"});
  // Some source records attribute an X post to a nested account record.
  if (author.empty()) {
    for (std::string_view key : {"author", "account"}) {
      const Json* person = object(source, key);
      if (person && person->type == Json::Type::Object) {
        author = text_field(*person, {"displayName", "name", "username", "handle"});
        if (!author.empty()) break;
      }
    }
  }
  const std::string outlet = text_field(source, {"domain", "publisher", "siteName"});
  const std::string excerpt = text_field(source, {"excerpt", "text", "description"});
  if (title.empty() && author.empty() && outlet.empty() && excerpt.empty() &&
      !web_url(href)) return;

  NodeId art = doc.create(NodeKind::Article, section);
  doc.get(art).label = "source-" + std::to_string(index + 1);
  NodeId heading = doc.create(NodeKind::Header, art);
  const std::string name = !title.empty() ? title : !author.empty() ? author :
                           !outlet.empty() ? outlet : web_url(href) ? href : "Source";
  if (web_url(href)) add_link(doc, heading, name, href);
  else add_text(doc, heading, name, InlineKind::Strong);

  NodeId cap = doc.create(NodeKind::Caption, art);
  const std::string type = text_field(source, {"type", "sourceType"});
  if (type == "x" || type == "tweet") add_meta(doc, cap, "X post");
  else if (type == "web" || type == "article") add_meta(doc, cap, "Article");
  if (!author.empty() && author != name) {
    add_sep(doc, cap);
    add_meta(doc, cap, "by " + author);
  }
  if (!outlet.empty() && outlet != name && outlet != author) {
    add_sep(doc, cap);
    add_meta(doc, cap, outlet);
  }
  add_date(doc, cap, source);
  // The source text is an excerpt, never the full third-party article.
  if (!excerpt.empty() && excerpt != title && excerpt != name)
    append_plain(doc, art, excerpt);
}

// May be an unwrapped JSON-RPC result, a complete MCP reply, or a content
// text block containing a serialized JSON object.
std::optional<Json> parse_object_json(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\r' ||
                           text.front() == '\n' || text.front() == '\t'))
    text.remove_prefix(1);
  if (text.empty() || (text.front() != '{' && text.front() != '['))
    return std::nullopt;
  return json_parse(text);
}

std::optional<Json> unwrap(Json reply, std::string* err) {
  if (const Json* error = object(reply, "error")) {
    if (err) *err = text_field(*error, {"message", "detail"});
    return std::nullopt;
  }
  // Do not assign a nested `result` back onto its owning JSON tree. The
  // source can be invalidated while vector/object members are being copied.
  // Keep both the envelope and selected result alive until extraction.
  const Json* result = object(reply, "result");
  if (!result) result = &reply;
  if (const Json* failed = object(*result, "isError");
      failed && failed->type == Json::Type::Bool && failed->b) {
    if (err) {
      const Json* c = list_field(*result, {"content"});
      *err = c && !c->items.empty() ? text_field(c->items.front(), {"text"})
                                     : "Digg MCP returned a tool error";
    }
    return std::nullopt;
  }
  if (const Json* data = object(*result, "structuredContent")) return *data;
  if (const Json* parts = list_field(*result, {"content"})) {
    for (const Json& part : parts->items) {
      const std::string payload = text_field(part, {"text"});
      if (auto parsed = parse_object_json(payload)) return parsed;
    }
    if (err) *err = "Digg MCP returned text instead of structured JSON";
    return std::nullopt;
  }
  return *result;
}

}  // namespace

std::optional<Json> digg_mcp_decode(std::string_view body, std::string* err) {
  if (auto j = parse_object_json(body)) return unwrap(std::move(*j), err);
  // Streamable HTTP also permits text/event-stream. Each blank line ends an
  // event; accept the last complete JSON-RPC message, not arbitrary SSE text.
  std::string payload;
  std::optional<Json> last;
  size_t at = 0;
  while (at < body.size()) {
    const size_t end = body.find('\n', at);
    std::string_view line = body.substr(at, end == std::string_view::npos
                                            ? std::string_view::npos : end - at);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (starts_with(line, "data:")) {
      if (!payload.empty()) payload += '\n';
      line.remove_prefix(5);
      if (!line.empty() && line.front() == ' ') line.remove_prefix(1);
      payload += line;
    } else if (line.empty() && !payload.empty()) {
      if (auto j = parse_object_json(payload)) last = std::move(*j);
      payload.clear();
    }
    if (end == std::string_view::npos) break;
    at = end + 1;
  }
  if (!payload.empty())
    if (auto j = parse_object_json(payload)) last = std::move(*j);
  if (last) return unwrap(std::move(*last), err);
  if (err) *err = "Digg MCP returned neither JSON nor a JSON SSE event";
  return std::nullopt;
}

std::string digg_feed_arguments(const ForumRef& ref) {
  // Digg's digg_get_feed tool rejects values above 20 (MCP schema).
  constexpr int kFeedLimit = 20;
  const size_t slash = ref.path.find('/');
  const std::string topic = ref.path.empty() ? "all" : ref.path.substr(0, slash);
  const std::string view = slash == std::string::npos ? "" : ref.path.substr(slash + 1);
  std::string args = "{\"topic\":" + json_quote(topic) +
                     ",\"limit\":" + std::to_string(kFeedLimit);
  if (!view.empty()) args += ",\"view\":" + json_quote(view);
  // Refs contain URL-escaped cursors; the MCP tool needs their raw values.
  if (!ref.cursor.empty()) args += ",\"cursor\":" + json_quote(percent_decode(ref.cursor));
  return args + "}";
}

// Search uses its own result window; forward opaque cursors only for
// another page of the same query. The MCP search tool caps limit at 50.
std::string digg_search_arguments(const ForumRef& ref) {
  std::string args = "{\"query\":" + json_quote(ref.query) + ",\"limit\":30";
  if (!ref.cursor.empty()) args += ",\"cursor\":" + json_quote(percent_decode(ref.cursor));
  return args + "}";
}

Document digg_list_document(const Json& reply, const ForumRef& ref) {
  const Json& data = content(reply);
  const Json* posts = data.type == Json::Type::Array ? &data :
      list_field(data, {"posts", "results", "items", "clusters"});
  const bool search = ref.path == "search";
  const std::string topic = ref.path.substr(0, ref.path.find('/'));
  const size_t slash = ref.path.find('/');
  const std::string view = slash == std::string::npos ? "" : ref.path.substr(slash + 1);
  std::string title = search ? "Search Digg: " + ref.query :
      "Digg — " + topic_name(topic);
  if (!search && !view.empty()) {
    for (const LabelRef& item : kViews)
      if (view == item.slug) { title += " · " + std::string(item.name); break; }
  }
  Document doc;
  NodeId thread = start_document(doc, title, "Digg", nav_for(ref), "");
  doc.get(thread).label.clear();
  if (!search) add_view_nav(doc, thread, ref);
  if (posts) {
    for (const Json& post : posts->items) add_story(doc, thread, post);
  }
  if (!posts || posts->items.empty())
    add_note(doc, thread, search ? "No recent Digg posts matched this search." :
                                      "No published posts were returned.");
  // Both the Feed and Search tools return opaque nextCursor values. Never
  // pretend a cursor is an integer page, nor construct an old-page link.
  const std::string cursor = text_field(data, {"nextCursor"});
  if (!cursor.empty() && cursor.size() < 512) {
    ForumRef next = ref;
    next.cursor = url_encode(cursor);
    NodeId nav = doc.create(NodeKind::Paragraph, doc.root);
    add_link(doc, nav, search ? "More search results →" : "More stories →",
             forum_ref_string(next));
  }
  add_web_link(doc, doc.root, ref);
  return doc;
}

Document digg_post_document(const Json& reply, const ForumRef& ref) {
  const Json& data = content(reply);
  const Json* post = object(data, "post");
  if (!post || post->type != Json::Type::Object) post = &data;
  const std::string title = text_field(*post, {"title", "headline", "label"});
  const std::string url = post_web(*post);
  Document doc;
  NodeId thread = start_document(doc, title.empty() ? "Digg post" : title,
                                 "Digg", nav_for(ref), "");
  doc.get(thread).label.clear();
  doc.meta.author = "Digg";
  // The thread supplies the only visible headline; do not repeat it as a
  // second Header in the article. Link it to Digg when a permalink exists.
  if (!url.empty()) {
    NodeId heading = thread_title(doc, thread);
    if (heading != INVALID_NODE) {
      doc.get(heading).spans.clear();
      add_link(doc, heading, title.empty() ? "Digg post" : title, url);
    }
  }
  NodeId art = doc.create(NodeKind::Article, thread);
  doc.get(art).label = "digg-post";
  NodeId cap = doc.create(NodeKind::Caption, art);
  add_stats(doc, cap, *post, true);
  if (!url.empty()) {
    add_sep(doc, cap);
    add_link(doc, cap, "View on Digg", url, InlineRole::Metadata);
  }

  // Digg publishes editorial copy and attributable outside sources, not
  // the full articles. Keep these distinct in the semantic document tree.
  std::string intro = post_intro(*post);
  std::string summary = post_summary(*post);
  std::string body = post_body(*post);
  if (post != &data) {
    if (intro.empty()) intro = post_intro(data);
    if (summary.empty()) summary = post_summary(data);
    if (body.empty()) body = post_body(data);
  }
  add_copy_section(doc, art, {}, intro);
  add_copy_section(doc, art, "TLDR", summary, intro);
  add_copy_section(doc, art, "Digg overview", body, intro, summary);
  if (intro.empty() && summary.empty() && body.empty())
    add_note(doc, art, "Digg did not include a summary for this post. "
                       "Read the attributed sources below for coverage.");

  const Json* sources = list_field(data, {"sources"});
  if (!sources) sources = list_field(*post, {"sources"});
  const Json* source_total = first(data, {"sourceCount", "totalSources"});
  if (!source_total) source_total = first(*post, {"sourceCount", "totalSources"});
  const long long total = source_total ? json_number(source_total) :
      (sources ? static_cast<long long>(sources->items.size()) : 0);
  NodeId section = doc.create(NodeKind::Section, art);
  doc.get(section).label = "sources";
  const std::string heading = total > 0 ? "Sources (" + std::to_string(total) + ")" :
                                "Sources";
  add_text(doc, doc.create(NodeKind::Header, section), heading, InlineKind::Strong);
  if (sources && !sources->items.empty()) {
    for (size_t i = 0; i < sources->items.size(); ++i)
      add_source(doc, section, sources->items[i], i);
  } else {
    add_note(doc, section, "No source excerpts were returned.");
  }
  const long long next = count_field(data, {"nextSourceOffset"});
  if (next > 0 && (total == 0 || next < total)) {
    NodeId para = doc.create(NodeKind::Paragraph, section);
    add_meta(doc, para, "Digg returned only the first source page. ");
    if (!url.empty()) add_link(doc, para, "See all sources on Digg", url);
    else add_meta(doc, para, "Additional sources are available on the Digg website.");
  }
  return doc;
}

}  // namespace xanatoo
