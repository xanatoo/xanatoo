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
#include <utility>

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

// A Digg post has a headline, a short editorial take and citations. Extra
// copy is presented as body text, never as another headline-sized section.
// The one short label identifies a summary without dominating the page.
void add_editorial_copy(Document& doc, NodeId parent,
                        const std::string& intro, const std::string& summary,
                        const std::string& body, bool has_blocks = false) {
  if (!intro.empty()) append_plain(doc, parent, intro);
  if (!summary.empty() && summary != intro) {
    NodeId line = doc.create(NodeKind::Paragraph, parent);
    add_text(doc, line, "In brief: ", InlineKind::Strong);
    add_text(doc, line, summary);
  }
  if (!body.empty() && body != intro && body != summary)
    append_plain(doc, parent, body);
  if (intro.empty() && summary.empty() && body.empty() && !has_blocks)
    add_note(doc, parent, "Digg has no editorial summary for this story. "
                          "The original reports are linked below.");
}

// Feed snippets are previews, not full articles. Normalize whitespace and
// clip on a word/UTF-8 boundary so long editorial copy does not bury the
// next headline; the post view retains the complete text.
std::string feed_preview(std::string_view text) {
  constexpr size_t kMaxBytes = 230;
  std::string out;
  out.reserve(std::min(text.size(), kMaxBytes + 32));
  bool space = false;
  for (unsigned char c : text) {
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      space = !out.empty();
      continue;
    }
    if (space) { out += ' '; space = false; }
    out += static_cast<char>(c);
    if (out.size() > kMaxBytes + 32) break;
  }
  if (out.size() <= kMaxBytes) return out;
  size_t stop = kMaxBytes;
  while (stop && (static_cast<unsigned char>(out[stop]) & 0xc0) == 0x80)
    --stop;
  const size_t last_space = out.rfind(' ', stop);
  if (last_space != std::string::npos && last_space >= kMaxBytes / 2)
    stop = last_space;
  while (stop && out[stop - 1] == ' ') --stop;
  out.resize(stop);
  out += "…";
  return out;
}

// Linked headlines should stay semantically clickable but use body-sized
// bold type rather than the large section-heading role.
void add_story_link(Document& doc, NodeId para, const std::string& text,
                    const std::string& href) {
  if (href.empty()) {
    add_text(doc, para, text, InlineKind::Strong);
  } else {
    add_link(doc, para, text, href);
    add_mark(doc.get(para).spans.back(), InlineKind::Strong);
  }
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

// The MCP schema does not promise an image, but public Digg feeds can
// carry media metadata. Only explicit, bounded image fields qualify: never
// reinterpret a source URL or a video URL as an image. Images use Xanatoo's
// existing async remote-image pipeline and remain optional.
struct StoryImage {
  std::string thumbnail, full, alt, credit;
};

std::string media_string(const Json& object,
                         std::initializer_list<std::string_view> names) {
  for (std::string_view name : names) {
    const Json* value = json_get(object, name);
    if (value && value->type == Json::Type::String && web_url(value->s))
      return value->s;
  }
  return {};
}

StoryImage image_record(const Json& value, bool video = false, bool nested = false) {
  StoryImage out;
  if (value.type == Json::Type::String && web_url(value.s)) {
    if (!video) out.thumbnail = value.s;
    return out;
  }
  if (value.type != Json::Type::Object) return out;
  const std::string media_type = text_field(value, {"type", "mediaType", "kind"});
  video = video || media_type == "video" || media_type == "audio";
  out.thumbnail = media_string(value, {"thumbnailUrl", "thumbnail_url",
      "previewUrl", "preview_url", "imageUrl", "image_url", "src"});
  if (out.thumbnail.empty() && !video)
    out.thumbnail = media_string(value, {"url", "secureUrl"});
  out.full = media_string(value, {"fullsize", "fullsizeUrl", "originalUrl",
                                  "original_url", "fullUrl"});
  out.alt = text_field(value, {"alt", "altText", "description", "caption"});
  out.credit = text_field(value, {"credit", "attribution", "sourceName"});
  // A nested image/thumbnail record is common for featured video cards.
  if (out.thumbnail.empty() && !nested) {
    for (std::string_view key : {"thumbnail", "preview", "image"}) {
      if (const Json* nested = object(value, key)) {
        StoryImage child = image_record(*nested, false, true);
        if (!child.thumbnail.empty()) {
          out.thumbnail = std::move(child.thumbnail);
          if (out.full.empty()) out.full = std::move(child.full);
          if (out.alt.empty()) out.alt = std::move(child.alt);
          if (out.credit.empty()) out.credit = std::move(child.credit);
          break;
        }
      }
    }
  }
  return out;
}

StoryImage story_image(const Json& post) {
  for (const Json* scope : {&post, object(post, "media"), object(post, "article"),
                            object(post, "story")}) {
    if (!scope || scope->type != Json::Type::Object) continue;
    // Explicit image-media objects may carry a direct URL; generic media
    // objects might be video or audio, so never make this fallback for them.
    if (scope != &post && text_field(*scope, {"type", "mediaType"}) == "image") {
      StoryImage image = image_record(*scope);
      if (!image.thumbnail.empty()) return image;
    }
    for (std::string_view key : {"thumbnailUrl", "thumbnail_url", "imageUrl",
                                 "image_url", "heroImageUrl", "coverImageUrl",
                                 "featuredImageUrl", "previewImageUrl",
                                 "thumbnail", "heroImage", "featuredImage",
                                 "coverImage", "previewImage", "image"}) {
      if (const Json* value = object(*scope, key)) {
        StoryImage img = image_record(*value);
        if (!img.thumbnail.empty()) {
          if (img.alt.empty()) img.alt = text_field(post, {"imageAlt", "imageAltText"});
          if (img.credit.empty()) img.credit = text_field(post, {"imageCredit", "imageAttribution"});
          return img;
        }
      }
    }
    // An explicit media array may hold video items. For videos, use only a
    // supplied poster/thumbnail, never the media item's playback URL.
    if (const Json* items = list_field(*scope, {"media", "mediaItems", "attachments"})) {
      for (const Json& item : items->items) {
        if (item.type != Json::Type::Object) continue;
        const std::string type = text_field(item, {"type", "mediaType", "kind"});
        if (type == "audio" || type == "file") continue;
        StoryImage img = image_record(item, type == "video");
        if (!img.thumbnail.empty()) return img;
      }
    }
  }
  return {};
}

void add_story_image(Document& doc, NodeId parent, StoryImage image,
                     std::string_view fallback_alt, bool feed) {
  if (!web_url(image.thumbnail)) return;
  NodeId id = doc.create(NodeKind::Image, parent);
  Node& node = doc.get(id);
  node.src = std::move(image.thumbnail);
  node.alt = image.alt.empty() ? std::string(fallback_alt) : std::move(image.alt);
  if (feed) node.float_side = "right";
  if (web_url(image.full) && image.full != node.src) {
    InlineSpan link;
    link.kind = InlineKind::Link;
    link.href = std::move(image.full);
    node.spans.push_back(std::move(link));
  }
  if (!feed && !image.credit.empty()) {
    NodeId caption = doc.create(NodeKind::Caption, parent);
    add_meta(doc, caption, "Image: " + image.credit);
  }
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
  if (starts_with(ref.path, "post/"))
    return {{"All stories", "digg:", false}};
  const std::string topic = ref.path.substr(0, ref.path.find('/'));
  std::vector<NavItem> nav;
  for (const LabelRef& item : kTopics) {
    const std::string target = item.slug == "all" ? "digg:" :
                               digg_ref(std::string(item.slug));
    nav.push_back({std::string(item.name), target,
                   item.slug == "all" ? (ref.path.empty() || topic == "all") :
                                        topic == item.slug});
  }
  return nav;
}

// Put both navigation rows in the page's Chrome region. Keeping sorting
// controls outside the story Thread prevents them from visually reading as
// part of the first story (and makes the Page panel treat them as navigation).
void add_view_nav(Document& doc, const ForumRef& ref) {
  if (ref.path == "search" || starts_with(ref.path, "post/")) return;
  const size_t slash = ref.path.find('/');
  const std::string topic = ref.path.empty() ? "all" : ref.path.substr(0, slash);
  const std::string view = slash == std::string::npos ?
      (topic == "all" ? "trending" : "top") : ref.path.substr(slash + 1);
  NodeId nav = INVALID_NODE;
  for (NodeId id : doc.get(doc.root).children) {
    if (doc.get(id).kind == NodeKind::Chrome &&
        doc.get(id).summary == "navigation") { nav = id; break; }
  }
  if (nav == INVALID_NODE) return;
  NodeId line = doc.create(NodeKind::Paragraph, nav);
  add_text(doc, line, "Sort: ", InlineKind::Strong);
  bool first_item = true;
  for (const LabelRef& item : kViews) {
    if (!first_item) add_meta(doc, line, " · ");
    first_item = false;
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
               bool detail = false, std::string_view current_topic = {}) {
  if (const Json* score = first(post, {"diggScore", "score"}))
    add_meta(doc, caption, "Score " + std::to_string(json_number(score)));
  // The post detail has a Sources heading with the count already. Do not
  // repeat the same count in both the dateline and that heading.
  if (!detail) {
    if (const Json* sources = first(post, {"sourceCount", "sourcesCount"})) {
      add_sep(doc, caption);
      add_meta(doc, caption, count(json_number(sources), "source", "sources"));
    }
  }
  if (const Json* authors = first(post, {"distinctSourceAuthorCount", "uniqueSourceAuthorCount"});
      authors && authors->type == Json::Type::Number && authors->n > 0) {
    add_sep(doc, caption);
    add_meta(doc, caption, count(json_number(authors), "source author", "source authors"));
  }
  // These are aggregated engagement signals on outside sources, not votes
  // or comments on Digg itself; label them explicitly when supplied.
  if (detail) {
    for (const auto& metric : {
        std::pair{"combinedViews", "source views"},
        std::pair{"combinedLikes", "source likes"},
        std::pair{"combinedReposts", "source reposts"},
        std::pair{"combinedSaves", "source saves"},
        std::pair{"combinedComments", "source replies"},
        std::pair{"combinedReplies", "source replies"}}) {
      if (std::string_view(metric.first) == "combinedReplies" &&
          object(post, "combinedComments")) continue;
      const Json* value = object(post, metric.first);
      if (value && value->type == Json::Type::Number && value->n >= 0) {
        add_sep(doc, caption);
        add_meta(doc, caption, std::to_string(json_number(value)) + " " + metric.second);
      }
    }
    if (!object(post, "combinedViews")) {
      if (const Json* views = object(post, "totalViews");
          views && views->type == Json::Type::Number && views->n >= 0) {
        add_sep(doc, caption);
        add_meta(doc, caption, std::to_string(json_number(views)) + " source views");
      }
    }
  }
  const std::string topic = post_topic(post);
  if (!topic.empty() && topic != current_topic) {
    add_sep(doc, caption);
    add_link(doc, caption, topic_name(topic), digg_ref(topic), InlineRole::Metadata);
  }
  add_date(doc, caption, post);
  if (detail) {
    if (const Json* rank = first(post, {"todayRank", "rank"});
        rank && rank->type == Json::Type::Number && rank->n > 0) {
      add_sep(doc, caption);
      add_meta(doc, caption, "Rank #" + std::to_string(json_number(rank)));
    }
    const std::string updated = text_field(post, {"updatedAt", "lastUpdatedAt"});
    const std::string published = text_field(post, {"publishedAt", "createdAt"});
    if (!updated.empty() && updated != published)
      add_timestamp(doc, caption, updated, "Updated ");
  }
}

// The feed is a compact reading list. Titles remain bold links, not large
// section headings; one brief excerpt and one quiet dateline per story.
void add_story(Document& doc, NodeId thread, const Json& story,
               std::string_view current_topic) {
  const std::string title = text_field(story, {"title", "headline", "label"});
  if (title.empty()) return;
  NodeId art = doc.create(NodeKind::Article, thread);
  doc.get(art).label = text_field(story, {"id", "shortId", "clusterUrlId"});
  add_story_image(doc, art, story_image(story), title, true);
  const std::string ref = native_post(story);
  const std::string web = post_web(story);
  NodeId heading = doc.create(NodeKind::Paragraph, art);
  add_story_link(doc, heading, title, !ref.empty() ? ref : web);
  const std::string summary = post_summary(story);
  const std::string intro = post_intro(story);
  if (!summary.empty() || !intro.empty()) {
    const std::string preview = feed_preview(!summary.empty() ? summary : intro);
    // Editorial snippets which simply repeat the headline add no information.
    if (preview != title) append_plain(doc, art, preview);
  }
  // Do not emit an empty Caption: even without text its layout reserves
  // vertical space, producing odd gaps in sparse API feed entries.
  const std::string topic = post_topic(story);
  const bool has_meta = first(story, {"diggScore", "score", "sourceCount",
      "sourcesCount", "distinctSourceAuthorCount", "uniqueSourceAuthorCount",
      "publishedAt", "createdAt", "firstSourceAt",
      "firstSourceTime", "earliestSourceAt", "earliestSourceTime"}) ||
      (!topic.empty() && topic != current_topic) || (!ref.empty() && !web.empty());
  if (has_meta) {
    NodeId cap = doc.create(NodeKind::Caption, art);
    add_stats(doc, cap, story, false, current_topic);
    if (!ref.empty() && !web.empty()) {
      add_sep(doc, cap);
      add_link(doc, cap, "Digg", web, InlineRole::Metadata);
    }
  }
}

// Source entries are citations, not a second news feed with giant headlines.
// Keep the attribution under the link and the publisher's excerpt separate
// from Digg's own editorial summary above.
void add_source(Document& doc, NodeId section, const Json& source, size_t index) {
  const std::string href = text_field(source, {"url", "sourceUrl", "xUrl"});
  const std::string title = text_field(source, {"title", "headline"});
  std::string author = text_field(source,
      {"authorName", "author", "displayName", "username", "handle"});
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
  const std::string name = !title.empty() ? title : !author.empty() ? author :
                           !outlet.empty() ? outlet : web_url(href) ? href : "Source";
  NodeId line = doc.create(NodeKind::Paragraph, art);
  add_story_link(doc, line, name, web_url(href) ? href : "");

  const std::string type = text_field(source, {"type", "sourceType"});
  const bool x_post = type == "x" || type == "tweet";
  const bool has_outlet = !outlet.empty() && outlet != name && outlet != author;
  const bool has_author = !author.empty() && author != name;
  if (x_post || has_outlet || has_author ||
      first(source, {"publishedAt", "createdAt", "firstSourceAt",
                     "firstSourceTime", "earliestSourceAt", "earliestSourceTime"})) {
    NodeId cap = doc.create(NodeKind::Caption, art);
    if (x_post) add_meta(doc, cap, "X");
    if (has_outlet) {
      add_sep(doc, cap);
      add_meta(doc, cap, outlet);
    }
    if (has_author) {
      add_sep(doc, cap);
      add_meta(doc, cap, "by " + author);
    }
    add_date(doc, cap, source);
  }
  if (!excerpt.empty() && excerpt != title && excerpt != name)
    append_plain(doc, art, excerpt);
}

// The public website has Useful links, related stories and entity tags.
// These may be absent in MCP results: emit sections only for explicit data.
void add_useful_links(Document& doc, NodeId article, const Json& data,
                      const Json& post) {
  const Json* links = list_field(post, {"usefulLinks", "keyLinks", "featuredLinks"});
  if (!links) links = list_field(data, {"usefulLinks", "keyLinks", "featuredLinks"});
  if (!links) return;
  NodeId section = INVALID_NODE;
  size_t count = 0;
  for (const Json& item : links->items) {
    if (count >= 24) break;
    const std::string href = item.type == Json::Type::String ? item.s :
        text_field(item, {"url", "href", "sourceUrl"});
    if (!web_url(href)) continue;
    const std::string title = item.type == Json::Type::Object ?
        text_field(item, {"title", "label", "headline", "name"}) : std::string();
    if (section == INVALID_NODE) {
      section = doc.create(NodeKind::Section, article);
      doc.get(section).label = "useful-links";
      add_text(doc, doc.create(NodeKind::Paragraph, section), "Useful links", InlineKind::Strong);
    }
    add_link(doc, doc.create(NodeKind::Paragraph, section),
             title.empty() ? href : title, href);
    ++count;
  }
}

void add_related_stories(Document& doc, NodeId article, const Json& data,
                         const Json& post) {
  const Json* related = list_field(post, {"relatedPosts", "relatedStories", "related"});
  if (!related) related = list_field(data, {"relatedPosts", "relatedStories", "related"});
  if (!related) return;
  NodeId section = INVALID_NODE;
  size_t count = 0;
  for (const Json& item : related->items) {
    if (count >= 12) break;
    if (item.type != Json::Type::Object) continue;
    const std::string title = text_field(item, {"title", "headline", "label"});
    std::string href = native_post(item);
    if (href.empty()) href = post_web(item);
    if (href.empty() || title.empty()) continue;
    if (section == INVALID_NODE) {
      section = doc.create(NodeKind::Section, article);
      doc.get(section).label = "related";
      add_text(doc, doc.create(NodeKind::Paragraph, section), "Related stories", InlineKind::Strong);
    }
    NodeId line = doc.create(NodeKind::Paragraph, section);
    add_story_link(doc, line, title, href);
    ++count;
  }
}

void add_tags(Document& doc, NodeId article, const Json& data, const Json& post) {
  const Json* tags = list_field(post, {"entities", "tags"});
  if (!tags) tags = list_field(data, {"entities", "tags"});
  if (!tags) return;
  NodeId line = INVALID_NODE;
  size_t count = 0;
  for (const Json& item : tags->items) {
    if (count >= 16) break;
    const std::string name = item.type == Json::Type::String ? item.s :
        text_field(item, {"name", "label", "title"});
    if (name.empty()) continue;
    const std::string url = item.type == Json::Type::Object ?
        text_field(item, {"url", "entityUrl", "permalink"}) : std::string();
    if (line == INVALID_NODE) {
      line = doc.create(NodeKind::Caption, article);
      add_meta(doc, line, "Tags: ");
    } else add_meta(doc, line, " · ");
    if (starts_with_ci(url, "https://digg.com/entities/"))
      add_link(doc, line, name, url, InlineRole::Metadata);
    else add_meta(doc, line, name);
    ++count;
  }
}

// Some editorial payloads contain ordered sections rather than a flat body.
// Preserve these as native semantic blocks, not as another flattened summary.
// This is intentionally bounded and does not recursively walk arbitrary JSON.
const Json* editorial_blocks(const Json& post) {
  for (const Json* scope : {object(post, "article"), object(post, "story"),
                            object(post, "copy"), &post}) {
    if (!scope || scope->type != Json::Type::Object) continue;
    if (const Json* blocks = list_field(*scope, {"sections", "contentBlocks", "bodyBlocks"}))
      return blocks;
  }
  return nullptr;
}

void add_editorial_blocks(Document& doc, NodeId article, const Json& blocks) {
  size_t added = 0;
  for (const Json& block : blocks.items) {
    if (added >= 100) break;
    if (block.type == Json::Type::String) {
      if (!block.s.empty()) { append_plain(doc, article, block.s); ++added; }
      continue;
    }
    if (block.type != Json::Type::Object) continue;
    const std::string heading = text_field(block, {"heading", "title"});
    if (!heading.empty()) {
      NodeId line = doc.create(NodeKind::Paragraph, article);
      add_text(doc, line, heading, InlineKind::Strong);
      ++added;
    }
    const std::string body = text_field(block, {"text", "body", "paragraph", "content"});
    if (!body.empty() && body != heading) {
      append_plain(doc, article, body);
      ++added;
    }
    if (const Json* paragraphs = list_field(block, {"paragraphs"})) {
      for (const Json& part : paragraphs->items) {
        if (added >= 100) break;
        const std::string text = part.type == Json::Type::String ? part.s :
            text_field(part, {"text", "plainText"});
        if (!text.empty()) { append_plain(doc, article, text); ++added; }
      }
    }
    const std::string kind = text_field(block, {"type", "kind"});
    if (kind == "image" || kind == "photo") {
      StoryImage im = image_record(block);
      if (!im.thumbnail.empty()) {
        add_story_image(doc, article, std::move(im), heading, false);
        ++added;
      }
    }
  }
}

// Digg's documented public MCP tools do not expose a separate comment
// endpoint. Some post payloads may include discussion records. Render only
// explicit comment arrays; never interpret aggregated X reply counts or
// attributed Sources as native Digg comments.
const Json* digg_comments(const Json& data, const Json& post) {
  const Json* discussion = object(data, "discussion");
  const Json* post_discussion = object(post, "discussion");
  for (const Json* scope : {&data, &post, discussion, post_discussion}) {
    if (!scope || scope->type != Json::Type::Object) continue;
    const Json* list = object(*scope, "comments");
    if (!list) continue;
    if (list->type == Json::Type::Array) return list;
    if (list->type == Json::Type::Object)
      if (const Json* items = list_field(*list, {"items", "nodes", "results"}))
        return items;
  }
  return nullptr;
}

std::string comment_author(const Json& comment) {
  if (std::string name = text_field(comment, {"authorName", "username", "displayName"});
      !name.empty()) return name;
  for (std::string_view name : {"author", "user", "account"}) {
    const Json* person = object(comment, name);
    if (!person) continue;
    if (person->type == Json::Type::String) return person->s;
    if (person->type == Json::Type::Object)
      if (std::string label = text_field(*person, {"displayName", "name", "username", "handle"});
          !label.empty()) return label;
  }
  return {};
}

// Bounded DFS keeps replies in source order, lets the existing Message
// layout indent children and avoids recursion on a malformed response.
void add_comments(Document& doc, NodeId article, const Json& data,
                  const Json& post, const std::string& post_url) {
  NodeId section = doc.create(NodeKind::Section, article);
  doc.get(section).label = "comments";
  NodeId heading = doc.create(NodeKind::Header, section);
  add_text(doc, heading, "Comments", InlineKind::Strong);

  const Json* comments = digg_comments(data, post);
  if (comments) {
    struct Entry { const Json* json; std::string parent; unsigned depth; };
    std::vector<Entry> stack;
    for (auto it = comments->items.rbegin(); it != comments->items.rend(); ++it)
      stack.push_back({&*it, {}, 0});
    size_t emitted = 0;
    constexpr size_t kMaxComments = 500;
    constexpr unsigned kMaxDepth = 32;
    while (!stack.empty() && emitted < kMaxComments) {
      Entry current = std::move(stack.back());
      stack.pop_back();
      const Json& c = *current.json;
      if (c.type != Json::Type::Object) continue;
      const std::string body = text_field(c, {"body", "text", "bodyText", "markdown"});
      std::string content = body.empty() ? copy_text(object(c, "content")) : body;
      const Json* deleted = first(c, {"deleted", "isDeleted"});
      const bool gone = deleted && deleted->type == Json::Type::Bool && deleted->b;
      if (content.empty() && !gone) continue;
      ++emitted;
      MsgHead head;
      head.label = text_field(c, {"id", "commentId", "_id"});
      if (head.label.empty()) head.label = "digg-comment-" + std::to_string(emitted);
      head.author = gone ? "[deleted]" : comment_author(c);
      if (head.author.empty()) head.author = "[unknown author]";
      head.iso = text_field(c, {"createdAt", "publishedAt", "postedAt"});
      head.reply_to = text_field(c, {"parentId", "parentCommentId"});
      if (head.reply_to.empty()) head.reply_to = current.parent;
      head.indent = !head.reply_to.empty();
      if (const Json* likes = first(c, {"score", "likeCount", "likes"}); likes &&
          likes->type == Json::Type::Number)
        head.score = count(json_number(likes), "point", "points");
      NodeId message = add_message(doc, section, head);
      if (gone) add_note(doc, message, "[deleted]");
      else append_plain(doc, message, content);
      if (current.depth >= kMaxDepth) continue;
      if (const Json* replies = list_field(c, {"replies", "children"})) {
        for (auto it = replies->items.rbegin(); it != replies->items.rend(); ++it)
          stack.push_back({&*it, head.label, current.depth + 1});
      }
    }
    if (emitted == 0)
      add_note(doc, section, "No Digg comments were included in this response.");
    if (!stack.empty())
      add_note(doc, section, "More comments were omitted from this view.");
  } else {
    add_note(doc, section, "Digg's public post API did not include a comment thread.");
  }
  if (!post_url.empty()) {
    NodeId line = doc.create(NodeKind::Paragraph, section);
    add_link(doc, line, "Read or join the discussion on Digg", post_url);
  }
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
  if (!search) add_view_nav(doc, ref);
  if (posts) {
    for (const Json& post : posts->items)
      add_story(doc, thread, post, search ? std::string_view{} : topic);
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
  std::string url = post_web(*post);
  if (url.empty() && post != &data) url = post_web(data);
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
  const bool has_dateline = !url.empty() ||
      !post_topic(*post).empty() || first(*post, {"diggScore", "score",
      "distinctSourceAuthorCount", "uniqueSourceAuthorCount", "combinedViews",
      "combinedLikes", "combinedReposts", "combinedSaves", "combinedComments",
      "combinedReplies", "todayRank", "rank", "totalViews",
      "publishedAt", "createdAt", "firstSourceAt", "firstSourceTime",
      "earliestSourceAt", "earliestSourceTime", "updatedAt", "lastUpdatedAt"});
  if (has_dateline) {
    NodeId cap = doc.create(NodeKind::Caption, art);
    add_stats(doc, cap, *post, true);
    if (!url.empty()) {
      add_sep(doc, cap);
      add_link(doc, cap, "View on Digg", url, InlineRole::Metadata);
    }
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
  // Featured images belong to the article, not its individual citations.
  // Prefer the post image, falling back to outer MCP metadata when needed.
  StoryImage featured = story_image(*post);
  if (featured.thumbnail.empty() && post != &data) featured = story_image(data);
  add_story_image(doc, art, std::move(featured), title, false);
  const Json* blocks = editorial_blocks(*post);
  if (!blocks && post != &data) blocks = editorial_blocks(data);
  // A structured article is more faithful than a flattened copy of the
  // same material. Never show both representations when both are present.
  if (blocks && !blocks->items.empty()) body.clear();
  add_editorial_copy(doc, art, intro, summary, body,
                     blocks && !blocks->items.empty());
  if (blocks) add_editorial_blocks(doc, art, *blocks);
  add_tags(doc, art, data, *post);
  add_useful_links(doc, art, data, *post);

  add_comments(doc, art, data, *post, url);

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
  add_related_stories(doc, art, data, *post);
  return doc;
}

}  // namespace xanatoo
