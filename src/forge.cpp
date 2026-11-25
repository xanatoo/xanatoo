// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/forge.hpp"

#include "forum_common.hpp"
#include "xanatoo/html_min.hpp"
#include "xanatoo/http_status.hpp"
#include "xanatoo/https.hpp"
#include "xanatoo/parse.hpp"
#include "xanatoo/text_escape.hpp"
#include "xanatoo/util.hpp"
#include "xanatoo/version.hpp"

#include <algorithm>
#include <charconv>
#include <limits>

namespace xanatoo {
namespace {

using View = ForgeView;

std::string take(std::string_view& path, char delimiter = '/') {
  const auto at = path.find(delimiter);
  std::string part(path.substr(0, at));
  path = at == path.npos ? std::string_view{} : path.substr(at + 1);
  return part;
}

bool safe_path(std::string_view path, bool project = false) {
  if (path.empty() || path.back() == '/') return false;
  while (!path.empty()) {
    const auto part = take(path);
    if (part.empty() || part == "." || part == "..") return false;
    for (unsigned char c : part) {
      if (c < 32 || c == 127 || c == '\\') return false;
      if (project && !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
        return false;
    }
  }
  return true;
}

std::string prefix(const ForgeRef& ref) { return ref.github ? "github:" : "gitlab:"; }

std::string origin(const ForgeRef& ref) {
  return ref.github ? "https://github.com/" : "https://gitlab.com/";
}

std::string project_ref(const ForgeRef& ref) {
  return prefix(ref) + ref.project;
}

const char* changes(const ForgeRef& ref) { return ref.github ? "pulls" : "merge_requests"; }

std::string route(const ForgeRef& ref) {
  switch (ref.view) {
    case View::Home: return {};
    case View::Search: return "search";
    case View::About: return "about";
    case View::Releases: return "releases";
    case View::Tags: return "tags";
    case View::Tree: return "tree";
    case View::Blob: return "blob";
    case View::Raw: return "raw";
    case View::Issues:
    case View::Issue: return "issues";
    case View::Changes: return changes(ref);
    case View::Change:
    case View::Reviews:
    case View::ReviewComments: return ref.github ? "pull" : changes(ref);
  }
  return {};
}

std::string route_separator(const ForgeRef& ref) { return ref.github ? "/" : "/-/"; }

bool file_view(View view) {
  return view == View::Tree || view == View::Blob || view == View::Raw;
}

std::string encoded_path(std::string_view path) {
  std::string result;
  while (!path.empty()) {
    if (!result.empty()) result += '/';
    result += forum::url_encode(take(path));
  }
  return result;
}

std::string api_base(const ForgeRef& ref) {
  return ref.github ? "https://api.github.com/repos/" + ref.project :
      "https://gitlab.com/api/v4/projects/" + forum::url_encode(ref.project);
}

}  // namespace

bool is_forge_ref(std::string_view url) {
  return starts_with_ci(url, "github:") || starts_with_ci(url, "gitlab:");
}

std::optional<ForgeRef> parse_forge_ref(std::string_view url) {
  if (!is_forge_ref(url)) return std::nullopt;
  ForgeRef ref;
  ref.github = starts_with_ci(url, "github:");
  url.remove_prefix(7);
  url = url.substr(0, url.find('#'));
  const auto query_at = url.find('?');
  auto query = query_at == url.npos ? std::string_view{} : url.substr(query_at + 1);
  auto path = url.substr(0, query_at);
  if (starts_with(path, "/")) path.remove_prefix(1);
  if (ends_with(path, "/")) path.remove_suffix(1);
  while (!query.empty()) {
    std::string field = take(query, '&');
    std::string_view value = field;
    const auto key = take(value, '=');
    if (key == "q" && path == "search") {
      ref.query = percent_decode(value);
    } else if (key == "ref") {
      ref.revision = percent_decode(value);
      if (!safe_path(ref.revision)) return std::nullopt;
    } else if (key == "after" || key == "before") {
      auto cursor = percent_decode(value);
      if (cursor.empty() || cursor.size() > 512 ||
          cursor.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-/+=") !=
              cursor.npos) return std::nullopt;
      (key == "after" ? ref.after : ref.before) = std::move(cursor);
    } else if (key == "page") {
      if (value.empty()) return std::nullopt;
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), ref.page);
      if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
          ref.page < 1 || ref.page == std::numeric_limits<int>::max()) return std::nullopt;
    }
  }
  const bool cursor = !ref.after.empty() || !ref.before.empty();
  if (path.empty()) {
    if (cursor) return std::nullopt;
    return ref;
  }
  if (path == "search") {
    std::string_view query_text = ref.query;
    trim_ascii(query_text);
    if (cursor || !ref.revision.empty() || query_text.empty() ||
        !is_valid_utf8(query_text) || std::any_of(query_text.begin(), query_text.end(),
            [](unsigned char c) { return c < 32 || c == 127; }))
      return std::nullopt;
    ref.query = query_text;
    ref.view = View::Search;
    return ref;
  }
  if (ref.github) {
    ref.project = take(path);
    const auto repo = take(path);
    if (ref.project.empty() || repo.empty()) return std::nullopt;
    ref.project += "/" + repo;
  } else {
    const auto separator = path.find("/-/");
    ref.project = path.substr(0, separator);
    path = separator == path.npos ? std::string_view{} : path.substr(separator + 3);
    if (ref.project.find('/') == std::string::npos) return std::nullopt;
  }
  if (!safe_path(ref.project, true)) return std::nullopt;
  const auto kind = take(path);
  if (kind == "issues" || kind == changes(ref) || (ref.github && kind == "pull")) {
    if (!path.empty()) {
      ref.number = take(path);
      if (ref.number.empty() || ref.number.front() == '0' ||
          ref.number.find_first_not_of("0123456789") != ref.number.npos)
        return std::nullopt;
      ref.view = kind == "issues" ? View::Issue : View::Change;
      if (!path.empty()) {
        if (!ref.github || kind != "pull") return std::nullopt;
        if (path == "reviews") ref.view = View::Reviews;
        else if (path == "files") ref.view = View::ReviewComments;
        else return std::nullopt;
      }
    } else {
      if (ref.github && kind == "pull") return std::nullopt;
      ref.view = kind == "issues" ? View::Issues : View::Changes;
    }
  } else if (kind == "tree" || kind == "blob" || kind == "raw") {
    ref.view = kind == "tree" ? View::Tree : kind == "blob" ? View::Blob : View::Raw;
    ref.path = percent_decode(path);
    if ((!ref.path.empty() || ref.view != View::Tree) && !safe_path(ref.path))
      return std::nullopt;
  } else if (path.empty() && kind == "about") {
    ref.view = View::About;
  } else if (path.empty() && kind == "releases") {
    ref.view = View::Releases;
  } else if (path.empty() && kind == "tags") {
    ref.view = View::Tags;
  } else if (!kind.empty() || !path.empty()) {
    return std::nullopt;
  }
  const bool cursor_view = !ref.github &&
      (ref.view == View::Issue || ref.view == View::Change || ref.view == View::Releases);
  if ((cursor_view || ref.view == View::About) && ref.page != 1) return std::nullopt;
  if (cursor && (!cursor_view || (!ref.after.empty() && !ref.before.empty())))
    return std::nullopt;
  return ref;
}

std::string forge_ref_string(const ForgeRef& ref) {
  if (ref.view == View::Search) {
    auto url = prefix(ref) + "search?q=" + forum::url_encode(ref.query);
    if (ref.page > 1) url += "&page=" + std::to_string(ref.page);
    return url;
  }
  std::string url = project_ref(ref);
  const auto section = route(ref);
  if (!section.empty()) url += route_separator(ref) + section;
  if (!ref.path.empty()) url += "/" + encoded_path(ref.path);
  if (!ref.number.empty()) url += "/" + ref.number;
  if (ref.view == View::Reviews) url += "/reviews";
  if (ref.view == View::ReviewComments) url += "/files";
  if (!ref.revision.empty()) url += "?ref=" + forum::url_encode(ref.revision);
  if (ref.page > 1) url += (ref.revision.empty() ? "?page=" : "&page=") + std::to_string(ref.page);
  if (!ref.after.empty()) url += (ref.revision.empty() ? "?after=" : "&after=") + forum::url_encode(ref.after);
  if (!ref.before.empty()) url += (ref.revision.empty() ? "?before=" : "&before=") + forum::url_encode(ref.before);
  return url;
}

std::string forge_web_url(const ForgeRef& ref) {
  if (ref.view == View::Search) {
    const auto query = forum::url_encode(ref.query);
    return ref.github ? "https://github.com/search?type=repositories&q=" + query :
                        "https://gitlab.com/explore/projects?name=" + query;
  }
  std::string url = origin(ref) + ref.project;
  const auto section = route(ref);
  if (section.empty() || ref.view == View::About) return url;
  url += route_separator(ref) + section;
  if (file_view(ref.view)) {
    url += "/" + (ref.revision.empty() ? "HEAD" : forum::url_encode(ref.revision));
    if (!ref.path.empty()) url += "/" + encoded_path(ref.path);
  }
  if (!ref.number.empty()) url += "/" + ref.number;
  if (ref.view == View::ReviewComments) url += "/files";
  return url;
}

std::string forge_ref_from_web(std::string_view url) {
  ForgeRef ref;
  ref.github = starts_with_ci(url, "https://github.com/");
  const auto host = origin(ref);
  if (!starts_with_ci(url, host)) return {};
  url.remove_prefix(host.size());
  url = url.substr(0, url.find_first_of("?#"));
  if (url.empty()) return {};
  // Web file URLs carry the revision as their first segment after the view.
  size_t separator = url.find("/-/");
  size_t section = separator == url.npos ? url.npos : separator + 3;
  if (ref.github) {
    auto rest = url;
    take(rest);
    take(rest);
    if (!rest.empty()) section = url.size() - rest.size();
  }
  std::string native = prefix(ref) + std::string(url);
  if (section != url.npos) {
    auto rest = url.substr(section);
    auto kind = take(rest);
    if (kind == "tree" || kind == "blob" || kind == "raw") {
      auto revision = take(rest);
      if (revision.empty()) return {};
      native = prefix(ref) + std::string(url.substr(0, section)) + kind;
      if (!rest.empty()) native += "/" + std::string(rest);
      if (revision != "HEAD") native += "?ref=" + revision;
    }
  }
  auto parsed = parse_forge_ref(native);
  if (!parsed) return {};
  return forge_ref_string(*parsed);
}

std::string forge_up_url(ForgeRef ref) {
  if (ref.view == View::Search && ref.page == 1) return prefix(ref);
  if (ref.page > 1 || !ref.after.empty() || !ref.before.empty()) {
    ref.page = 1;
    ref.after.clear();
    ref.before.clear();
  } else if (ref.view == View::Reviews || ref.view == View::ReviewComments) {
    ref.view = View::Change;
  } else if (ref.view == View::Issue || ref.view == View::Change) {
    ref.view = ref.view == View::Issue ? View::Issues : View::Changes;
    ref.number.clear();
  } else if (file_view(ref.view) && !ref.path.empty()) {
    const auto slash = ref.path.rfind('/');
    ref.path = slash == ref.path.npos ? "" : ref.path.substr(0, slash);
    ref.view = ref.path.empty() && ref.revision.empty() ? View::Home : View::Tree;
  } else if (ref.view != View::Home) {
    ref.view = View::Home;
    ref.revision.clear();
  } else if (!ref.project.empty()) {
    return prefix(ref);
  } else {
    return {};
  }
  return forge_ref_string(ref);
}

std::optional<ServiceBar> forge_service_bar(std::string_view url) {
  auto ref = parse_forge_ref(url);
  if (!ref) return std::nullopt;
  ServiceBar bar;
  bar.id = ref->github ? "github" : "gitlab";
  bar.name = ref->github ? "GitHub" : "GitLab";
  bar.home = prefix(*ref);
  bar.color = ref->github ? 0x57606a : 0xc55327;
  if (!ref->project.empty()) {
    const auto project = project_ref(*ref);
    const auto sep = route_separator(*ref);
    auto files = project;
    if (!ref->revision.empty()) files += sep + "tree?ref=" + forum::url_encode(ref->revision);
    bar.rows.push_back({{}, {
      {"Files", files, ref->view == View::Home || file_view(ref->view)},
      {"About", project + sep + "about", ref->view == View::About},
      {"Releases", project + sep + "releases", ref->view == View::Releases},
      {"Tags", project + sep + "tags", ref->view == View::Tags},
      {"Issues", project + sep + "issues", ref->view == View::Issues || ref->view == View::Issue},
      {ref->github ? "Pull requests" : "Merge requests", project + sep + changes(*ref),
       ref->view == View::Changes || ref->view == View::Change ||
       ref->view == View::Reviews || ref->view == View::ReviewComments},
    }});
    if (ref->github && !ref->number.empty() && ref->view != View::Issue) {
      const auto pull = project + "/pull/" + ref->number;
      bar.rows.push_back({{}, {
        {"Conversation", pull, ref->view == View::Change},
        {"Reviews", pull + "/reviews", ref->view == View::Reviews},
        {"Review comments", pull + "/files", ref->view == View::ReviewComments},
      }});
    }
  }
  return bar;
}

std::string forge_raw_url(const ForgeRef& ref) {
  const std::string rev = ref.revision.empty() ? "HEAD" : forum::url_encode(ref.revision);
  if (ref.github)
    return "https://raw.githubusercontent.com/" + ref.project + "/" + rev + "/" +
           encoded_path(ref.path);
  return api_base(ref) + "/repository/files/" + forum::url_encode(ref.path) + "/raw?ref=" + rev;
}

std::string forge_filename(const ForgeRef& ref) {
  return ref.path.substr(ref.path.rfind('/') + 1);
}

#if defined(XANATOO_HTTPS) && XANATOO_HTTPS
namespace {

constexpr int kPageSize = 30;

std::optional<Json> get_json(const std::string& url, std::string* err,
                             std::string_view headers = {}) {
  const auto response = https_request("GET", url, user_agent("repository browser"),
                                      headers, {}, err);
  if (!response) return std::nullopt;
  auto data = json_parse(response->body);
  if (response->status != 200) {
    *err = http_status_error(response->status);
    if (data) {
      const auto message = forum::str(*data, "message");
      if (!message.empty()) *err += " — " + message;
    }
    return std::nullopt;
  }
  if (!data) *err = "Invalid JSON from repository API";
  return data;
}

NodeId start_page(Document& doc, const std::string& title) {
  const auto root = doc.create(NodeKind::Document);
  doc.meta.title = title;
  doc.append_text(doc.create(NodeKind::Header, root), title);
  return root;
}

void link_line(Document& doc, NodeId parent, const std::string& title, const std::string& href) {
  forum::add_link(doc, doc.create(NodeKind::Paragraph, parent), title, href);
}

std::string paging(const ForgeRef& ref) {
  return "per_page=" + std::to_string(kPageSize) + "&page=" + std::to_string(ref.page);
}

void add_pager(Document& doc, ForgeRef ref, bool has_next) {
  if (ref.page == 1 && !has_next) return;
  const auto nav = doc.create(NodeKind::Paragraph, doc.root);
  if (ref.page > 1) {
    --ref.page;
    forum::add_link(doc, nav, "← Previous", forge_ref_string(ref));
    ++ref.page;
    forum::add_sep(doc, nav);
  }
  forum::add_text(doc, nav, "Page " + std::to_string(ref.page));
  if (has_next && ref.page < std::numeric_limits<int>::max() - 1) {
    ++ref.page;
    forum::add_sep(doc, nav);
    forum::add_link(doc, nav, "Next →", forge_ref_string(ref));
  }
}

std::optional<Document> project_search(const ForgeRef& ref, std::string* err) {
  const auto query = forum::url_encode(ref.query);
  const auto url = ref.github
      ? "https://api.github.com/search/repositories?q=" + query + "&" + paging(ref)
      : "https://gitlab.com/api/v4/projects?visibility=public&order_by=star_count&sort=desc&search=" +
            query + "&" + paging(ref);
  const auto data = get_json(url, err);
  if (!data) return std::nullopt;
  const auto* results = ref.github ? json_get(*data, "items") : &*data;
  if (!results || results->type != Json::Type::Array) {
    *err = "Expected search results from the repository API";
    return std::nullopt;
  }

  Document doc;
  const auto root = start_page(doc, (ref.github ? "GitHub repositories · " : "GitLab projects · ") +
                                  ref.query);
  bool found = false;
  for (const auto& item : results->items) {
    const auto project = forum::str(item, ref.github ? "full_name" : "path_with_namespace");
    const auto target = parse_forge_ref(prefix(ref) + project);
    if (!target || target->view != View::Home || target->project != project) continue;
    found = true;
    const auto article = doc.create(NodeKind::Article, root);
    forum::add_link(doc, doc.create(NodeKind::Header, article), project, forge_ref_string(*target));
    const auto description = forum::str(item, "description");
    if (!description.empty()) forum::add_note(doc, article, description);
    const auto stars = forum::num(item, ref.github ? "stargazers_count" : "star_count");
    const auto caption = doc.create(NodeKind::Caption, article);
    forum::add_meta(doc, caption, forum::count(stars, "star", "stars"));
  }
  if (!found) forum::add_note(doc, root, "No projects match this search.");

  const long long offset = static_cast<long long>(ref.page) * kPageSize;
  const bool has_next = results->items.size() >= kPageSize &&
      (ref.github ? offset < std::min(1000LL, forum::num(*data, "total_count")) : offset < 50000);
  add_pager(doc, ref, has_next);
  return doc;
}

std::optional<Document> tree_page(const ForgeRef& ref, std::string* err) {
  const auto rev = forum::url_encode(ref.revision);
  const auto url = ref.github
      ? api_base(ref) + "/contents" + (ref.path.empty() ? "" : "/" + encoded_path(ref.path)) +
            (rev.empty() ? "" : "?ref=" + rev)
      : api_base(ref) + "/repository/tree?" + paging(ref) +
            (ref.path.empty() ? "" : "&path=" + forum::url_encode(ref.path)) +
            (rev.empty() ? "" : "&ref=" + rev);
  auto data = get_json(url, err);
  if (!data) return std::nullopt;
  if (data->type != Json::Type::Array) {
    *err = "Expected a repository directory";
    return std::nullopt;
  }
  Document doc;
  const auto root = start_page(doc, ref.project + (ref.path.empty() ? "" : " / " + ref.path));
  if (!ref.revision.empty()) forum::add_note(doc, root, "Revision " + ref.revision);
  if (!ref.path.empty()) {
    auto directory = ref;
    directory.page = 1;
    link_line(doc, root, "↑ Parent directory", forge_up_url(directory));
  }
  if (data->items.empty()) forum::add_note(doc, root, "No files on this page.");
  const auto directory = [&](const Json& entry) {
    return forum::str(entry, "type") == (ref.github ? "dir" : "tree");
  };
  std::stable_sort(data->items.begin(), data->items.end(), [&](const Json& a, const Json& b) {
    if (directory(a) != directory(b)) return directory(a);
    return forum::str(a, "name") < forum::str(b, "name");
  });
  for (const auto& entry : data->items) {
    auto item = ref;
    item.path = forum::str(entry, "path");
    if (!safe_path(item.path)) continue;
    item.view = directory(entry) ? View::Tree : View::Blob;
    item.page = 1;
    const auto row = doc.create(NodeKind::Paragraph, root);
    const auto name = forge_filename(item);
    const auto type = forum::str(entry, "type");
    if (json_get(entry, "submodule_git_url") || type == "submodule" || type == "commit") {
      forum::add_text(doc, row, name + " (submodule)");
      continue;
    }
    forum::add_link(doc, row, directory(entry) ? name + "/" : name, forge_ref_string(item));
    if (!directory(entry)) {
      item.view = View::Raw;
      forum::add_sep(doc, row);
      forum::add_link(doc, row, "Download", forge_ref_string(item), InlineRole::Metadata);
    }
  }
  if (!ref.github) add_pager(doc, ref, data->items.size() >= kPageSize);
  if (ref.github && data->items.size() >= 1000)
    forum::add_note(doc, root, "GitHub's Contents API returns at most 1,000 entries per directory.");
  return doc;
}

std::optional<Document> file_page(const ForgeRef& ref, std::string* err) {
  Document doc;
  const auto root = start_page(doc, forge_filename(ref));
  auto raw = ref;
  raw.view = View::Raw;
  link_line(doc, root, "Download file", forge_ref_string(raw));
  link_line(doc, root, "↑ Parent directory", forge_up_url(ref));
  constexpr size_t kPreviewLimit = 256 * 1024;
  std::string body;
  int status = 0;
  bool too_large = false;
  const bool ok = https_get_follow_stream(forge_raw_url(ref), user_agent("repository file preview"),
      [&](const HttpStreamHeaders& headers) {
        status = headers.status;
        too_large = status == 200 && headers.content_length > static_cast<int64_t>(kPreviewLimit);
        return status == 200 && !too_large;
      },
      [&](std::string_view bytes) {
        too_large = bytes.size() > kPreviewLimit - body.size();
        if (!too_large) body.append(bytes);
        return !too_large;
      }, err);
  if (too_large) {
    forum::add_note(doc, root, "This file exceeds the 256 KiB preview limit.");
  } else if (!ok) {
    if (status && status != 200) *err = http_status_error(status);
    forum::add_note(doc, root, err->empty() ? "File preview unavailable." : *err);
  } else if (!is_valid_utf8(body) || body.find('\0') != body.npos) {
    forum::add_note(doc, root, "Binary file.");
  } else if (body.empty()) {
    forum::add_note(doc, root, "Empty file.");
  } else {
    doc.append_text(doc.create(NodeKind::CodeBlock, root), body);
  }
  err->clear();
  return doc;
}

std::string item_number(const Json& item, const ForgeRef& ref) {
  const auto number = forum::num(item, ref.github ? "number" : "iid");
  return number > 0 ? std::to_string(number) : std::string();
}

std::optional<Document> item_list(const ForgeRef& ref, std::string* err) {
  const auto url = api_base(ref) + "/" + route(ref) + "?" + paging(ref) +
      (ref.github ? "&state=all" : "&scope=all");
  const auto data = get_json(url, err);
  if (!data) return std::nullopt;
  if (data->type != Json::Type::Array) {
    *err = "Expected a list from the repository API";
    return std::nullopt;
  }
  Document doc;
  const bool issues = ref.view == View::Issues;
  const auto root = start_page(doc, ref.project + " · " +
      (issues ? "Issues" : ref.github ? "Pull requests" : "Merge requests"));
  auto item = ref;
  item.view = issues ? View::Issue : View::Change;
  item.page = 1;
  bool listed = false;
  for (const auto& entry : data->items) {
    if (issues && ref.github && json_get(entry, "pull_request")) continue;
    item.number = item_number(entry, ref);
    if (item.number.empty()) continue;
    listed = true;
    const auto article = doc.create(NodeKind::Article, root);
    forum::add_link(doc, doc.create(NodeKind::Header, article), "#" + item.number + " " +
                    forum::str(entry, "title"), forge_ref_string(item));
    const auto caption = doc.create(NodeKind::Caption, article);
    forum::add_meta(doc, caption, forum::str(entry, "state"));
    forum::add_sep(doc, caption);
    forum::add_meta(doc, caption, forum::short_time(forum::str(entry, "updated_at")));
  }
  if (!listed) forum::add_note(doc, root, "No entries on this page.");
  add_pager(doc, ref, data->items.size() >= kPageSize);
  return doc;
}

// GitLab returns Markdown. Copy ordinary Markdown blocks and web links from
// the parser, excluding xanatoo-specific directives and local destinations.
void copy_markdown_block(Document& doc, NodeId parent, const Document& source,
                         NodeId id, const std::string& base) {
  const auto& block = source.get(id);
  switch (block.kind) {
    case NodeKind::Paragraph: case NodeKind::BlockQuote: case NodeKind::CodeBlock:
    case NodeKind::Heading: case NodeKind::Header: case NodeKind::Section:
    case NodeKind::List: case NodeKind::ListItem: case NodeKind::HorizontalRule:
    case NodeKind::Table: case NodeKind::TableRow: case NodeKind::TableCell:
    case NodeKind::Image:
      break;
    default: return;
  }
  const auto image = block.kind == NodeKind::Image ? resolve_url(base, block.src) : "";
  if (block.kind == NodeKind::Image && !starts_with_ci(image, "https://")) return;
  const auto next = doc.create(block.kind, parent);
  auto& node = doc.get(next);
  node.level = block.level;
  node.col_span = block.col_span;
  node.row_span = block.row_span;
  node.align = block.align;
  if (block.kind == NodeKind::CodeBlock) node.src = block.src;
  if (block.kind == NodeKind::Image) {
    node.src = image;
    node.alt = block.alt;
  }
  for (auto span : block.spans) {
    if (span.kind == InlineKind::Image) {
      span.src = resolve_url(base, span.src);
      if (!starts_with_ci(span.src, "https://")) {
        span.kind = InlineKind::Text;
        span.text = span.alt;
        span.src.clear();
      }
    }
    if ((span.kind == InlineKind::Link || span.kind == InlineKind::Image) && !span.href.empty()) {
      if (!starts_with(span.href, "#")) span.href = resolve_url(base, span.href);
      if (!starts_with_ci(span.href, "https://") && !starts_with_ci(span.href, "http://") &&
          !starts_with(span.href, "#")) {
        if (span.kind == InlineKind::Link) span.kind = InlineKind::Text;
        span.href.clear();
      }
    }
    node.spans.push_back(std::move(span));
  }
  for (const auto child : block.children) copy_markdown_block(doc, next, source, child, base);
}

void append_markdown(Document& doc, NodeId parent, std::string_view body,
                     const std::string& base) {
  const auto markdown = parse_markup(body);
  for (const auto id : markdown.get(markdown.root).children)
    copy_markdown_block(doc, parent, markdown, id, base);
}

NodeId add_post(Document& doc, NodeId thread, const ForgeRef& ref, const Json& entry,
                const std::string& label, const std::string& permalink,
                const std::string& reply_to = {}) {
  const auto author_key = ref.github ? "user/login" : "author/username";
  const auto author = json_string(json_path(entry, author_key));
  auto timestamp = forum::str(entry, "created_at");
  if (timestamp.empty()) timestamp = forum::str(entry, ref.github ? "submitted_at" : "createdAt");
  const auto message = forum::add_message(doc, thread, {
      .label = label,
      .author = author,
      .author_href = {},
      .iso = timestamp,
      .permalink = permalink,
      .reply_to = reply_to,
      .indent = !reply_to.empty(),
      .score = {},
  });
  const auto html = forum::str(entry, "body_html");
  if (!html.empty()) {
    forum::append_html(doc, message, html, forge_web_url(ref));
  } else {
    const auto* description = json_get(entry, "description");
    append_markdown(doc, message, description ? json_string(description) : forum::str(entry, "body"),
                    forge_web_url(ref));
  }
  return message;
}

bool flag(const Json& value, std::string_view path) {
  const auto* member = json_path(value, path);
  return member && member->type == Json::Type::Bool && member->b;
}

std::string cursor_args(const ForgeRef& ref, int count) {
  const auto size = std::to_string(count);
  if (!ref.before.empty()) return "last:" + size + ",before:" + forum::json_quote(ref.before);
  if (!ref.after.empty()) return "first:" + size + ",after:" + forum::json_quote(ref.after);
  return "first:" + size;
}

void add_cursor_pager(Document& doc, const ForgeRef& ref, const Json& connection) {
  const auto start = json_string(json_path(connection, "pageInfo/startCursor"));
  const auto end = json_string(json_path(connection, "pageInfo/endCursor"));
  const bool previous = !start.empty() &&
      (!ref.after.empty() || flag(connection, "pageInfo/hasPreviousPage"));
  const bool next = !end.empty() &&
      (!ref.before.empty() || flag(connection, "pageInfo/hasNextPage"));
  if (previous || next) {
    const auto pager = doc.create(NodeKind::Paragraph, doc.root);
    auto target = ref;
    target.page = 1;
    if (previous) {
      target.after.clear();
      target.before = start;
      forum::add_link(doc, pager, "← Previous", forge_ref_string(target));
    }
    if (next) {
      target.before.clear();
      target.after = end;
      forum::add_sep(doc, pager);
      forum::add_link(doc, pager, "Next →", forge_ref_string(target));
    }
  }
}

void gitlab_discussion(Document& doc, NodeId thread, const ForgeRef& ref, std::string* err) {
  const auto field = ref.view == View::Issue ? "issue" : "mergeRequest";
  const auto page = cursor_args(ref, kPageSize);
  const auto query = "{project(fullPath:" + forum::json_quote(ref.project) + "){" +
      field + "(iid:" + forum::json_quote(ref.number) + "){discussions(" + page +
      "){pageInfo{startCursor endCursor hasNextPage hasPreviousPage}"
      " nodes{notes{nodes{id body createdAt system author{username}}}}}}}}";
  const auto data = get_json("https://gitlab.com/api/graphql?query=" + forum::url_encode(query), err);
  const auto* discussion = data ? json_path(*data, std::string("data/project/") +
                                             field + "/discussions") : nullptr;
  if (!discussion || discussion->type != Json::Type::Object) {
    if (data && err->empty()) *err = json_string(json_path(*data, "errors/0/message"));
    forum::add_note(doc, thread, err->empty() ? "Discussion unavailable." :
                                               "Discussion unavailable. " + *err);
    err->clear();
    return;
  }
  const auto web = forge_web_url(ref);
  bool shown = false;
  for (const auto& entry : forum::arr(*discussion, "nodes")) {
    const auto* notes = json_path(entry, "notes/nodes");
    if (!notes) continue;
    std::string reply_to;
    for (const auto& note : notes->items) {
      if (flag(note, "system")) continue;
      constexpr std::string_view prefix = "gid://gitlab/Note/";
      const auto gid = forum::str(note, "id");
      if (!starts_with(gid, prefix)) continue;
      const auto id = gid.substr(prefix.size());
      const auto label = "note-" + id;
      add_post(doc, thread, ref, note, label, web + "#note_" + id, reply_to);
      if (reply_to.empty()) reply_to = label;
      shown = true;
    }
  }
  if (!shown) forum::add_note(doc, thread, "No comments on this page.");
  add_cursor_pager(doc, ref, *discussion);
}

std::optional<Document> thread_page(const ForgeRef& ref, std::string* err) {
  constexpr auto kHtml = "Accept: application/vnd.github.html+json\r\n";
  constexpr auto kReviewHtml = "Accept: application/vnd.github-commitcomment.html+json\r\n";
  const bool reviews = ref.view == View::Reviews;
  const bool inline_comments = ref.view == View::ReviewComments;
  const auto base = api_base(ref);
  const auto endpoint = base + "/" + (ref.github && ref.view != View::Issue ? "pulls" : route(ref)) +
                        "/" + ref.number;
  const auto item = get_json(endpoint, err, ref.github ? kHtml : "");
  if (!item) return std::nullopt;
  if (item->type != Json::Type::Object) {
    *err = "Expected a discussion from the repository API";
    return std::nullopt;
  }
  Document doc;
  const auto thread = forum::start_document(doc, "#" + ref.number + " " +
      forum::str(*item, "title"), "discussion-" + ref.number);
  const auto native = forge_ref_string(ref);
  const auto web = forge_web_url(ref);
  const auto top = add_post(doc, thread, ref, *item, "description", web);
  const auto state = forum::str(*item, "state");
  if (!state.empty()) forum::add_note(doc, top, "State " + state);
  if (!ref.github) {
    gitlab_discussion(doc, thread, ref, err);
  } else {
    const auto comments_url = reviews ? endpoint + "/reviews" :
        inline_comments ? endpoint + "/comments" : base + "/issues/" + ref.number + "/comments";
    auto comments = get_json(comments_url + "?" + paging(ref), err,
                             reviews || inline_comments ? kReviewHtml : kHtml);
    if (!comments || comments->type != Json::Type::Array) {
      forum::add_note(doc, thread, err->empty() ? "Discussion unavailable." :
                                                 "Discussion unavailable. " + *err);
      err->clear();
      return doc;
    }
    if (comments->items.empty()) forum::add_note(doc, thread, "No comments on this page.");
    for (const auto& entry : comments->items) {
      const auto id = std::to_string(forum::num(entry, "id"));
      const auto parent = inline_comments ? forum::num(entry, "in_reply_to_id") : 0;
      auto permalink = forum::str(entry, "html_url");
      if (!starts_with(permalink, origin(ref))) permalink = web;
      const auto post = add_post(doc, thread, ref, entry, "comment-" + id, permalink,
                                 parent > 0 ? "comment-" + std::to_string(parent) : "");
      if (reviews) forum::add_note(doc, post, forum::str(entry, "state"));
      if (inline_comments) {
        auto path = forum::str(entry, "path");
        const auto line = forum::num(entry, "line");
        if (line > 0) path += ":" + std::to_string(line);
        if (!path.empty()) forum::add_note(doc, post, path);
      }
    }
    add_pager(doc, ref, comments->items.size() >= kPageSize);
  }
  forum::map_links(doc, [&](std::string& href) {
    if (starts_with(href, web + "#issuecomment-"))
      href = native + "#id:comment-" + href.substr(web.size() + 14);
    else if (starts_with(href, web + "#note_"))
      href = native + "#id:note-" + href.substr(web.size() + 6);
    else if (href != web && href.find('#') == href.npos) {
      auto target = forge_ref_from_web(href);
      if (!target.empty()) href = std::move(target);
    }
  });
  return doc;
}

std::optional<ForgeRef> project_file(const ForgeRef& ref, std::string_view url,
                                     std::string_view revision) {
  const auto base = origin(ref) + ref.project + route_separator(ref) + "blob/";
  if (!starts_with(url, base)) return std::nullopt;
  url.remove_prefix(base.size());
  if (revision.empty()) {
    return parse_forge_ref(forge_ref_from_web(base + std::string(url)));
  }
  const auto encoded = forum::url_encode(revision) + "/";
  const auto literal = std::string(revision) + "/";
  if (starts_with(url, encoded)) url.remove_prefix(encoded.size());
  else if (starts_with(url, literal)) url.remove_prefix(literal.size());
  else return std::nullopt;
  ForgeRef file;
  file.github = ref.github;
  file.project = ref.project;
  file.view = View::Blob;
  file.path = percent_decode(url.substr(0, url.find_first_of("?#")));
  file.revision = revision == "HEAD" ? "" : std::string(revision);
  if (!safe_path(file.path)) return std::nullopt;
  return file;
}

void readme_links(Document& doc, const ForgeRef& ref, const ForgeRef& file) {
  const auto revision = file.revision.empty() ? "HEAD" : file.revision;
  auto rewrite = [&](std::string& href, bool image) {
    const auto target = project_file(ref, href, revision);
    if (!target) return;
    if (image) {
      href = forge_raw_url(*target);
    } else {
      const auto fragment = href.find('#');
      href = forge_ref_string(*target) +
          (fragment == href.npos ? "" : href.substr(fragment));
    }
  };
  for (auto& node : doc.nodes) {
    if (node.kind == NodeKind::Image) rewrite(node.src, true);
    for (auto& span : node.spans) {
      if (span.kind == InlineKind::Image) rewrite(span.src, true);
      if ((span.kind == InlineKind::Link || span.kind == InlineKind::Image) && !span.href.empty())
        rewrite(span.href, false);
    }
  }
}

void about_license(Document& doc, NodeId root, const ForgeRef& ref, const Json& data) {
  const auto path = forum::str(data, "path");
  auto file = ref.github ? std::optional<ForgeRef>{} :
      project_file(ref, forum::str(data, "license_url"), forum::str(data, "default_branch"));
  if (ref.github && safe_path(path)) {
    ForgeRef target;
    target.github = true;
    target.project = ref.project;
    target.view = View::Blob;
    target.path = path;
    file = std::move(target);
  }
  auto name = json_string(json_path(data, "license/name"));
  const auto spdx = json_string(json_path(data, "license/spdx_id"));
  if (spdx == "NOASSERTION" || name == "Other") name.clear();
  if (name.empty() && !file) {
    forum::add_note(doc, root, "License information unavailable.");
    return;
  }
  const auto row = doc.create(NodeKind::Paragraph, root);
  forum::add_text(doc, row, "License ", InlineKind::Strong);
  if (file) forum::add_link(doc, row, name.empty() ? forge_filename(*file) : name,
                           forge_ref_string(*file));
  else forum::add_text(doc, row, name);
  if (!spdx.empty() && spdx != "NOASSERTION" && spdx != name)
    forum::add_meta(doc, row, " (" + spdx + ")");
}

std::optional<Document> about_page(const ForgeRef& ref, std::string* err) {
  const auto api = api_base(ref);
  Document doc;
  const auto root = start_page(doc, ref.project + " · About");
  std::optional<ForgeRef> file;
  std::optional<HttpResult> readme;
  std::string readme_error;
  if (ref.github) {
    std::string license_error;
    const auto license = get_json(api + "/license", &license_error);
    if (license) about_license(doc, root, ref, *license);
    else forum::add_note(doc, root, "License information unavailable.");
    constexpr auto accept = "Accept: application/vnd.github.html+json\r\n";
    readme = https_request("GET", api + "/readme", user_agent("repository README"), accept,
                           {}, &readme_error);
    if (readme && readme->status == 200 && starts_with(readme->body, "<div ")) {
      const std::string_view html = readme->body;
      auto path = html_attr(html.substr(5, html.find('>') - 5), "data-path");
      if (safe_path(path)) {
        ForgeRef target;
        target.github = true;
        target.project = ref.project;
        target.view = View::Blob;
        target.path = std::move(path);
        file = std::move(target);
      }
    }
  } else {
    const auto project = get_json(api + "?license=true", err);
    if (!project) return std::nullopt;
    about_license(doc, root, ref, *project);
    file = project_file(ref, forum::str(*project, "readme_url"), forum::str(*project, "default_branch"));
    if (file) readme = https_get_follow(forge_raw_url(*file), user_agent("repository README"),
                                      &readme_error);
  }

  if (!readme || readme->status != 200) {
    if (readme && readme->status != 404) readme_error = http_status_error(readme->status);
    forum::add_note(doc, root, readme_error.empty() ? "README unavailable." :
                                                     "README unavailable. " + readme_error);
    return doc;
  }
  const auto base = file ? forge_web_url(*file) : forge_web_url(ref) + "/";
  if (file) link_line(doc, root, "README source", forge_ref_string(*file));
  if (readme->body.empty()) forum::add_note(doc, root, "The README is empty.");
  else if (ref.github) {
    HtmlOptions options;
    options.base_url = base;
    options.sections = true;
    options.anchors = true;
    options.images = true;
    html_append_blocks(doc, root, readme->body, options);
  } else if (is_valid_utf8(readme->body) && readme->body.find('\0') == std::string::npos) {
    append_markdown(doc, root, readme->body, base);
  } else {
    forum::add_note(doc, root, "This README cannot be rendered as text.");
  }
  if (file) readme_links(doc, ref, *file);
  return doc;
}

void https_link(Document& doc, NodeId parent, const std::string& label, const std::string& url) {
  if (!label.empty() && starts_with_ci(url, "https://")) link_line(doc, parent, label, url);
}

NodeId version_entry(Document& doc, const ForgeRef& ref, const std::string& title,
                     const std::string& tag, const std::string& commit, const std::string& date) {
  const auto article = doc.create(NodeKind::Article, doc.root);
  const auto heading = doc.create(NodeKind::Header, article);
  auto tree = ref;
  tree.view = View::Tree;
  tree.page = 1;
  tree.before.clear();
  tree.after.clear();
  tree.revision = safe_path(commit) ? commit : ref.github ? "refs/tags/" + tag : tag;
  if (safe_path(tag) && safe_path(tree.revision))
    forum::add_link(doc, heading, title, forge_ref_string(tree));
  else
    doc.append_text(heading, title);
  if (tag != title || !date.empty()) {
    const auto caption = doc.create(NodeKind::Caption, article);
    if (tag != title) forum::add_meta(doc, caption, tag);
    if (!date.empty()) {
      forum::add_sep(doc, caption);
      forum::add_meta(doc, caption, forum::short_time(date));
    }
  }
  return article;
}

std::optional<Document> tags_page(const ForgeRef& ref, std::string* err) {
  const auto url = api_base(ref) + (ref.github ? "/tags?" : "/repository/tags?") + paging(ref);
  const auto data = get_json(url, err);
  if (!data) return std::nullopt;
  if (data->type != Json::Type::Array) {
    *err = "Expected a tag list from the repository API";
    return std::nullopt;
  }
  Document doc;
  const auto root = start_page(doc, ref.project + " · Tags");
  if (data->items.empty()) forum::add_note(doc, root, "No tags on this page.");
  for (const auto& item : data->items) {
    const auto name = forum::str(item, "name");
    const auto commit = json_string(json_path(item, ref.github ? "commit/sha" : "commit/id"));
    if (name.empty()) continue;
    const auto article = version_entry(doc, ref, name, name, commit, forum::str(item, "created_at"));
    const auto message = forum::str(item, "message");
    if (!message.empty()) forum::append_plain(doc, article, message);
    if (ref.github) {
      https_link(doc, article, "Source ZIP", forum::str(item, "zipball_url"));
      https_link(doc, article, "Source tar.gz", forum::str(item, "tarball_url"));
    } else if (safe_path(commit)) {
      const auto archive = api_base(ref) + "/repository/archive";
      const auto query = "?sha=" + forum::url_encode(commit);
      https_link(doc, article, "Source ZIP", archive + ".zip" + query);
      https_link(doc, article, "Source tar.gz", archive + ".tar.gz" + query);
    }
  }
  add_pager(doc, ref, data->items.size() >= kPageSize);
  return doc;
}

std::optional<Document> releases_page(const ForgeRef& ref, std::string* err) {
  constexpr int page_size = 10;
  std::optional<Json> data;
  const Json* connection = nullptr;
  const Json* entries = nullptr;
  if (ref.github) {
    const auto url = api_base(ref) + "/releases?per_page=" + std::to_string(page_size) +
        "&page=" + std::to_string(ref.page);
    data = get_json(url, err);
    if (data) entries = &*data;
  } else {
    const auto query = "{project(fullPath:" + forum::json_quote(ref.project) +
        "){releases(" + cursor_args(ref, page_size) +
        "){pageInfo{startCursor endCursor hasNextPage hasPreviousPage}"
        " nodes{tagName name descriptionHtml releasedAt commit{sha}"
        " assets{sources{nodes{format url}} links{nodes{name url directAssetUrl}}}}}}}";
    data = get_json("https://gitlab.com/api/graphql?query=" + forum::url_encode(query), err);
    if (data) {
      connection = json_path(*data, "data/project/releases");
      if (connection) entries = json_get(*connection, "nodes");
      if (!entries) *err = json_string(json_path(*data, "errors/0/message"));
    }
  }
  if (!entries || entries->type != Json::Type::Array) {
    if (err->empty()) *err = "Expected a release list from the repository API";
    return std::nullopt;
  }

  Document doc;
  const auto root = start_page(doc, ref.project + " · Releases");
  if (entries->items.empty()) forum::add_note(doc, root, "No releases on this page.");
  for (const auto& item : entries->items) {
    const auto tag = forum::str(item, ref.github ? "tag_name" : "tagName");
    auto title = forum::str(item, "name");
    if (title.empty()) title = tag;
    if (title.empty()) continue;
    const auto date = forum::str(item, ref.github ? "published_at" : "releasedAt");
    const auto article = version_entry(doc, ref, title, tag,
                                       json_string(json_path(item, "commit/sha")), date);
    if (ref.github) {
      if (flag(item, "prerelease")) forum::add_note(doc, article, "Pre-release");
      const auto base = forge_web_url(ref) + "/tag/" + forum::url_encode(tag);
      append_markdown(doc, article, forum::str(item, "body"), base);
      https_link(doc, article, "Source ZIP", forum::str(item, "zipball_url"));
      https_link(doc, article, "Source tar.gz", forum::str(item, "tarball_url"));
      for (const auto& asset : forum::arr(item, "assets"))
        https_link(doc, article, forum::str(asset, "name"), forum::str(asset, "browser_download_url"));
    } else {
      HtmlOptions options;
      options.base_url = forge_web_url(ref) + "/" + forum::url_encode(tag);
      options.sections = true;
      options.images = true;
      html_append_blocks(doc, article, forum::str(item, "descriptionHtml"), options);
      if (const auto* sources = json_path(item, "assets/sources"))
        for (const auto& asset : forum::arr(*sources, "nodes"))
          https_link(doc, article, "Source " + forum::str(asset, "format"), forum::str(asset, "url"));
      if (const auto* links = json_path(item, "assets/links")) {
        for (const auto& asset : forum::arr(*links, "nodes")) {
          const auto direct = forum::str(asset, "directAssetUrl");
          https_link(doc, article, forum::str(asset, "name"),
                     direct.empty() ? forum::str(asset, "url") : direct);
        }
      }
    }
  }
  if (connection) add_cursor_pager(doc, ref, *connection);
  else add_pager(doc, ref, entries->items.size() >= page_size);
  return doc;
}

}  // namespace

std::optional<Document> forge_fetch(const ForgeRef& ref, std::string* err) {
  std::string ignored;
  if (!err) err = &ignored;
  if (ref.view == View::Search) return project_search(ref, err);
  if (ref.project.empty()) {
    Document doc;
    const auto root = start_page(doc, ref.github ? "GitHub" : "GitLab");
    forum::add_note(doc, root, "Enter " + prefix(ref) + "owner/project in the address bar.");
    if (!ref.github)
      forum::add_note(doc, root, "Include every group in the project path on gitlab.com.");
    link_line(doc, root, ref.github ? "xanatoo/xanatoo" : "gitlab-org/gitlab",
              ref.github ? "github:xanatoo/xanatoo" : "gitlab:gitlab-org/gitlab");
    return doc;
  }
  switch (ref.view) {
    case View::Search: break;
    case View::About: return about_page(ref, err);
    case View::Releases: return releases_page(ref, err);
    case View::Tags: return tags_page(ref, err);
    case View::Home: case View::Tree: return tree_page(ref, err);
    case View::Blob: return file_page(ref, err);
    case View::Issues: case View::Changes: return item_list(ref, err);
    case View::Issue: case View::Change: case View::Reviews:
    case View::ReviewComments: return thread_page(ref, err);
    case View::Raw: *err = "Open the raw file as a download."; return std::nullopt;
  }
  return std::nullopt;
}
#endif

}  // namespace xanatoo
