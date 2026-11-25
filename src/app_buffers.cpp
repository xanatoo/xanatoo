// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/forge.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>

#include "xanatoo/app.hpp"
#include "xanatoo/snapshot.hpp"
#include "xanatoo/gallery.hpp"
#include "xanatoo/fixed_layout.hpp"
#include "xanatoo/ebooks.hpp"
#include "xanatoo/epub_import.hpp"
#include "xanatoo/feed.hpp"
#include "xanatoo/file_url.hpp"
#include "xanatoo/hackernews.hpp"
#include "xanatoo/forums.hpp"
#include "xanatoo/nntp.hpp"
#include "xanatoo/html_page.hpp"
#include "xanatoo/sitemap.hpp"
#include "xanatoo/transfer_meter.hpp"
#include "xanatoo/wikipedia.hpp"
#include "xanatoo/gemini.hpp"
#include "xanatoo/gopher.hpp"
#include "xanatoo/spartan.hpp"
#include "xanatoo/nex.hpp"
#include "xanatoo/finger.hpp"
#include "xanatoo/lemmy.hpp"
#include "xanatoo/matrix.hpp"
#include "xanatoo/native.hpp"
#include "xanatoo/wayback.hpp"
#include "xanatoo/video_sites.hpp"
#include "xanatoo/archive.hpp"
#include "xanatoo/nostr.hpp"
#include "xanatoo/parse.hpp"
#include "xanatoo/paths.hpp"
#include "xanatoo/reddit.hpp"
#include "xanatoo/search_results.hpp"
#include "xanatoo/revstore.hpp"
#include "xanatoo/sidestore.hpp"
#include "xanatoo/util.hpp"
#include "xanatoo/url_cache.hpp"

namespace fs = std::filesystem;

namespace xanatoo::app {

std::string resolve_open_path(App& app, std::string path) {
  if (path.empty() || !xanatoo::is_pandoc_path(path)) return path;
  auto imp = xanatoo::import_via_pandoc(path);
  if (!imp.ok) {
    std::cerr << "Import failed: " << imp.error << "\n";
    return {};
  }
  return imp.markdown_path;
}

// Read-only Markdown index of directory `dir`: parent link, then
// sub-directories and files (dot files hidden), each linked by absolute path
// so the links resolve whatever the buffer path looks like.
static std::string directory_listing(const fs::path& dir) {
  std::vector<std::pair<std::string, bool>> entries;  // name, is directory
  std::error_code ec;
  for (auto it = fs::directory_iterator(dir, ec);
       !ec && it != fs::directory_iterator(); it.increment(ec)) {
    std::string name = it->path().filename().string();
    if (name.empty() || name[0] == '.') continue;
    std::error_code ec2;
    entries.emplace_back(std::move(name), it->is_directory(ec2));
  }
  std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
    if (a.second != b.second) return a.second;
    return xanatoo::ascii_lower(a.first) < xanatoo::ascii_lower(b.first);
  });
  std::string md = "# " + dir.string() + "\n\n";
  if (ec) md += "Cannot read directory: " + ec.message() + "\n\n";
  const fs::path parent = dir.parent_path();
  if (!parent.empty() && parent != dir)
    md += "- [../](" + parent.string() + "/)\n";
  for (const auto& [name, is_dir] : entries) {
    const std::string label = is_dir ? name + "/" : name;
    const std::string target = (dir / name).string() + (is_dir ? "/" : "");
    // The link syntax has no escapes: such names are listed unlinked.
    if (target.find_first_of("()[]") != std::string::npos)
      md += "- `" + label + "`\n";
    else
      md += "- [" + label + "](" + target + ")\n";
  }
  if (entries.empty() && !ec) md += "*(empty)*\n";
  return md;
}

std::string path_title(const std::string& path) {
  try {
    fs::path p(path);
    if (p.has_filename()) return p.filename().string();
    p = p.parent_path();
    if (p.has_filename()) return p.filename().string() + "/";
  } catch (...) {
  }
  return path;
}

xanatoo::Document load_doc(App& app, std::string path) {
  if (path.empty()) {
    return xanatoo::parse_document(xanatoo::fallback_document(), {});
  }
  if (opens_as_image(path)) return xanatoo::image_url_document(path);
  if (xanatoo::is_pandoc_path(path)) {
    std::string md = resolve_open_path(app, path);
    if (md.empty()) {
      return xanatoo::parse_document(
          "# Import failed\n\n`" + path +
              "` is read through **pandoc**: install it, or see the "
              "console for pandoc's error.\n",
          {});
    }
    path = std::move(md);
  }
  if (xanatoo::is_fixed_layout_path(path)) {
    // Synchronous path (new tab, session restore): waits for every page
    // size; navigation goes through the fetch worker instead.
    std::string why;
    if (auto src = xanatoo::read_fixed_layout(path, &why))
      return xanatoo::fixed_layout_document(*src);
    return xanatoo::parse_document(
        "# Cannot open " + path_title(path) + "\n\n" + why + "\n", {});
  }
  std::error_code ec;
  if (fs::is_directory(path, ec)) {
    fs::path dir = fs::weakly_canonical(path, ec);
    if (ec) dir = fs::absolute(path);
    std::cout << "Listed: " << dir.string() << "\n";
    return xanatoo::parse_document(directory_listing(dir), {});
  }
  std::ifstream in(path);
  if (!in) {
    std::cerr << "Failed to open document: " << path << "\n";
    return xanatoo::parse_document(xanatoo::fallback_document(), {});
  }
  // Look at the start first: a video, archive or other binary file read
  // and laid out as text takes gigabytes and minutes.
  std::string source(8192, '\0');
  in.read(source.data(), static_cast<std::streamsize>(source.size()));
  source.resize(static_cast<size_t>(in.gcount()));
  if (xanatoo::looks_like_binary(source)) {
    app.binary_paths.insert(path);
    std::cout << "Binary, not opened as text: " << path << "\n";
    const uint64_t size = fs::file_size(path, ec);
    std::string md = "# " + path_title(path) + "\n\n`" + path +
                     "` is binary data";
    if (!ec) md += " (" + xanatoo::transfer_label(size, -1, 0) + ")";
    md += ", not a document; it is not shown as text.\n";
    const fs::path dir = fs::absolute(path, ec).parent_path();
    if (!ec && dir.string().find_first_of("()[]") == std::string::npos)
      md += "\n[Its folder](" + dir.string() + "/)\n";
    return xanatoo::parse_document(md, {});
  }
  app.binary_paths.erase(path);
  source.append(std::istreambuf_iterator<char>(in),
                std::istreambuf_iterator<char>());
  std::cout << "Opened: " << path << "\n";
  if (xanatoo::path_uses_xml(path)) {
    try {
      auto doc = xanatoo::parse_xml(source);
      app.invalid_xml_paths.erase(path);
      return doc;
    } catch (const std::invalid_argument& e) {
      app.invalid_xml_paths.insert(path);
      std::cerr << "Cannot import " << path << ": " << e.what() << "\n";
      return xanatoo::parse_markup(
          "# XML import failed\n\nThe source file was not modified. "
          "This view is read-only; fix the input and reload it.\n\n`" +
          std::string(e.what()) + "`\n");
    }
  }
  return xanatoo::parse_document(source, path);
}

std::string parent_dir(const std::string& path) {
  if (path.empty()) return {};
  std::filesystem::path p(path);
  if (p.has_parent_path()) return p.parent_path().string();
  return {};
}

// Other http(s) pages open as reader views when built with XANATOO_WEB;
// otherwise they go to the system browser.
static bool web_in_app(std::string_view s) {
#if defined(XANATOO_WEB) && XANATOO_WEB
  return xanatoo::is_web_url(s);
#else
  (void)s;
  return false;
#endif
}

bool is_in_app_remote_url(std::string_view s) {
  return xanatoo::is_snapshot_ref(s) || xanatoo::is_gemini_url(s) || xanatoo::is_gopher_url(s) ||
         xanatoo::is_spartan_url(s) || xanatoo::is_nex_url(s) ||
         xanatoo::is_finger_url(s) || xanatoo::is_nntp_url(s) ||
         xanatoo::is_reddit_ref(s) ||
         xanatoo::is_lemmy_ref(s) || xanatoo::is_nostr_ref(s) ||
         xanatoo::is_matrix_ref(s) ||
         xanatoo::is_hn_ref(s) || xanatoo::is_forum_ref(s) ||
         xanatoo::is_youtube_ref(s) ||
         xanatoo::is_video_site_ref(s) || xanatoo::is_ia_ref(s) ||
         xanatoo::is_forge_ref(s) || xanatoo::is_git_ref(s) ||
         xanatoo::is_ebook_url(s) || xanatoo::is_wayback_ref(s) ||
         xanatoo::is_feed_url(s) || xanatoo::is_wikipedia_ref(s) ||
         xanatoo::is_sitemap_ref(s) || xanatoo::is_search_ref(s) ||
         xanatoo::is_media_ref(s) ||
         web_in_app(s);
}

bool may_be_image(std::string_view url) {
  return xanatoo::url_cache_kind(url) != xanatoo::UrlKind::Other;
}

bool opens_as_image(std::string_view url) {
  if (!xanatoo::is_image_address(url)) return false;
  const bool http = xanatoo::starts_with_ci(url, "http://") ||
                    xanatoo::starts_with_ci(url, "https://");
  if (!http) return true;  // small-net fetchers sniff the bytes themselves
  switch (xanatoo::url_cache_kind(url)) {
    case xanatoo::UrlKind::Image: return true;
    case xanatoo::UrlKind::Other: return false;
    case xanatoo::UrlKind::Unknown: break;
  }
  return !web_in_app(url);  // no web fetcher: the image viewer it is
}

bool is_foreign_path(std::string_view path) {
  if (path.empty()) return false;
  if (is_in_app_remote_url(path) || xanatoo::starts_with(path, "http://") ||
      xanatoo::starts_with(path, "https://"))
    return true;
  if (xanatoo::path_uses_html(path) || xanatoo::is_pandoc_path(path) ||
      xanatoo::is_fixed_layout_path(path))
    return true;
  if (path == kDraftsPage || path == kDocumentsPage || path == kAboutPage ||
      is_stored_original_ref(path))
    return true;
  // Directory listings (navigate_pane gives them a trailing '/').
  std::error_code ec;
  if (xanatoo::ends_with(path, "/") ||
      fs::is_directory(fs::path(std::string(path)), ec))
    return true;
  const std::string cache = xanatoo::cache_dir();
  return !cache.empty() && xanatoo::starts_with(path, cache + "/");
}

namespace {
bool buffer_read_only(const App& app, const DocBuffer& b) {
  return is_foreign_path(b.path) || app.invalid_xml_paths.contains(b.path) ||
         app.binary_paths.contains(b.path);
}
bool buffer_locked(const DocBuffer& b) {
  return b.lock_set && b.lock_path == b.path ? b.lock_choice
                                             : default_locked(b.path);
}
}  // namespace

bool pane_read_only(App& app, const Pane& p) {
  const DocBuffer* b = find_buf(app, p.buf);
  return b && ((p.source_buf == p.buf && p.source_doc.has_value()) ||
               buffer_read_only(app, *b));
}

bool buffer_takes_edits(App& app, BufferId id) {
  const DocBuffer* b = find_buf(app, id);
  return b && !b->placeholder && !buffer_read_only(app, *b) &&
         !buffer_locked(*b);
}

bool focused_read_only(App& app) {
  return pane_read_only(app, focused_pane(app));
}

bool default_locked(std::string_view path) {
  const std::string data = xanatoo::data_dir();
  return !data.empty() && xanatoo::starts_with(path, data + "/");
}

bool pane_locked(App& app, const Pane& p) {
  const DocBuffer* b = find_buf(app, p.buf);
  return b && buffer_locked(*b);
}

void set_pane_locked(App& app, const Pane& p, bool locked) {
  DocBuffer* b = find_buf(app, p.buf);
  if (!b) return;
  b->lock_set = true;
  b->lock_path = b->path;
  b->lock_choice = locked;
}

const char* no_edit_notice(App& app) {
  return focused_read_only(app) ? kReadOnlyNotice : kLockedNotice;
}

bool browsing(App& app) {
  const Pane& p = focused_pane(app);
  return pane_read_only(app, p) || pane_locked(app, p);
}

xanatoo::Document load_doc_or_placeholder(App& app, const std::string& path) {
  if (is_in_app_remote_url(path)) {
    // Real fetch happens after navigate_to_url is defined.
    return xanatoo::parse_document("# Loading…\n", {});
  }
  return load_doc(app, path);
}

DocBuffer* find_buf(App& app, BufferId id) {
  if (id == kNoBuffer) return nullptr;
  for (auto& b : app.buffers)
    if (b.id == id) return &b;
  return nullptr;
}

BufferId add_buffer(App& app, xanatoo::Document doc, std::string path) {
  DocBuffer b;
  b.id = app.next_buffer_id++;
  b.history_tab = xanatoo::visit_instance_id() + "/tab/" + std::to_string(b.id);
  b.doc = std::move(doc);
  b.path = std::move(path);
  b.content_path = derived_content_path(b.path);
  app.buffers.push_back(std::move(b));
  const BufferId id = app.buffers.back().id;
  attach_draft(app, id);
  return id;
}

DocBuffer& buf(App& app, BufferId id) {
  DocBuffer* b = find_buf(app, id);
  assert(b && "buffer id not found");
  if (!b) {
    // Release-build safety net: never crash on a stale id.
    if (app.buffers.empty()) {
      add_buffer(app, xanatoo::parse_document(xanatoo::fallback_document(), {}),
                 {});
    }
    return app.buffers.front();
  }
  return *b;
}

BufferId tab_root(App& app, BufferId id) {
  while (const DocBuffer* b = find_buf(app, id)) {
    if (b->tab_parent == kNoBuffer) break;
    id = b->tab_parent;
  }
  return id;
}

void insert_tab(App& app, BufferId id, BufferId opener) {
  auto& order = app.tab_order;
  if (std::find(order.begin(), order.end(), id) != order.end()) return;
  auto pos = std::find(order.begin(), order.end(), opener);
  // A returning floating document can already have descendants.
  BufferId ancestor = opener;
  while (ancestor != kNoBuffer && ancestor != id)
    ancestor = buf(app, ancestor).tab_parent;
  if (opener != kNoBuffer && ancestor != id) buf(app, id).tab_parent = opener;
  if (pos != order.end()) {
    ++pos;
    for (; pos != order.end(); ++pos) {
      BufferId parent = buf(app, *pos).tab_parent;
      while (parent != kNoBuffer && parent != opener)
        parent = buf(app, parent).tab_parent;
      if (parent != opener) break;
    }
  }
  order.insert(pos, id);
}

NavHistory& view_nav(App& app, int pi) {
  Pane& p = app.panes[pi];
  if (pi < kCentreViews)
    if (DocBuffer* b = find_buf(app, p.buf)) return b->tab_nav;
  return p.nav;
}

void select_view_tab(App& app, int pi, BufferId id) {
  Pane& p = app.panes[pi];
  revert_split_preview(app);
  if (p.buf == id) return;
  nav_history_capture(view_nav(app, pi), pane_address(app, p), p.sel, p.scroll_y);
  p.buf = id;
  reset_pane_locator(p);
  p.view_anchor.reset();
  p.sel = {};
  p.scroll_y = 0.f;
  p.layout = {};
  p.height_cache.clear();
  app.pending_scroll[pi] = {};
  app.split_history = {};
  app.split_trail = {};
  retarget_fetches(app);

  NavHistory& h = view_nav(app, pi);
  if (h.pos >= 0 && h.restoring < 0) {
    const NavHistoryEntry& e = h.entries[static_cast<size_t>(h.pos)];
    xanatoo::Locator loc = xanatoo::parse_locator(e.url);
    if (loc.has_selectors()) apply_locator(app, pi, std::move(loc));
    p.sel = e.sel;
    p.scroll_y = e.scroll;
  } else if (const DocBuffer* b = find_buf(app, id);
             h.pos < 0 && b && !b->path.empty()) {
    push_nav_history(app, pi, pane_address(app, p));
  }
  xanatoo::ensure_valid_caret(doc_of(app, p), p.sel.focus);
  xanatoo::ensure_valid_caret(doc_of(app, p), p.sel.anchor);
  app.needs_layout = true;
  update_title(app);
}

// Drop `id` from the tab strip (keeping at least one tab) and repoint panes
// that showed it at the neighbouring tab. The buffer itself is untouched.
static void leave_tab(App& app, BufferId id) {
  auto tab = std::find(app.tab_order.begin(), app.tab_order.end(), id);
  const size_t tab_i = tab == app.tab_order.end()
                           ? 0
                           : static_cast<size_t>(tab - app.tab_order.begin());
  if (tab != app.tab_order.end()) app.tab_order.erase(tab);
  if (app.tab_order.empty()) {
    BufferId nb = add_buffer(
        app, xanatoo::parse_document(xanatoo::fallback_document(), {}), {});
    buf(app, nb).history.reset(buf(app, nb).doc, xanatoo::Selection{});
    app.tab_order.push_back(nb);
  }
  const BufferId next =
      app.tab_order[std::min(tab_i, app.tab_order.size() - 1)];
  for (int pi = 0; pi < kCentreViews; ++pi)
    if (app.panes[pi].buf == id || !find_buf(app, app.panes[pi].buf))
      select_view_tab(app, pi, next);
  app.needs_layout = true;
  update_title(app);
}

void remove_buffer(App& app, BufferId id) {
  if (!find_buf(app, id)) return;
  stash_draft(app, id);
  for (size_t i = app.soft_wins.size(); i-- > 0;) {
    if (app.panes[app.soft_wins[i].view].buf != id) continue;
    release_view(app, app.soft_wins[i].view);
    app.soft_wins.erase(app.soft_wins.begin() +
                        static_cast<std::ptrdiff_t>(i));
  }
  const BufferId parent = buf(app, id).tab_parent;
  for (auto& b : app.buffers)
    if (b.tab_parent == id) b.tab_parent = parent;
  leave_tab(app, id);
  app.buffers.erase(
      std::remove_if(app.buffers.begin(), app.buffers.end(),
                     [&](const DocBuffer& b) { return b.id == id; }),
      app.buffers.end());
  update_title(app);
}

void close_tab(App& app, BufferId id) {
  const bool floated =
      std::any_of(app.soft_wins.begin(), app.soft_wins.end(),
                  [&](const FloatWindow& sw) {
                    return app.panes[sw.view].buf == id;
                  });
  if (floated)
    leave_tab(app, id);  // MDI: floating windows keep the buffer alive
  else
    remove_buffer(app, id);
}

std::string buffer_tab_label(App& app, BufferId bi) {
  const DocBuffer* b = find_buf(app, bi);
  if (!b) return "?";
  std::string s;
  if (b->dirty) s += "*";
  const std::string& path = b->path;
  if (path.empty()) {
    s += "untitled";
  } else if (is_stored_original_ref(path)) {
    s += "Original (read-only)";
  } else if (is_stored_document_ref(path)) {
    s += b->stored_title.empty() ? "untitled" : b->stored_title;
  } else if (is_in_app_remote_url(path)) {
    // URL: host + last path segment ("geminiprotocol.net/.../x.gmi"); ASCII
    // dots because the ImGui tab font has no "…".
    std::string u = path;
    if (size_t sch = u.find("://"); sch != std::string::npos)
      u.erase(0, sch + 3);
    while (u.size() > 1 && u.back() == '/') u.pop_back();
    const size_t slash = u.find('/');
    const size_t last = u.rfind('/');
    if (slash != std::string::npos && last != slash)
      u = u.substr(0, slash) + "/..." + u.substr(last);
    if (u.size() > 40) u = u.substr(0, 37) + "...";
    s += u;
  } else {
    s += path_title(path);
  }
  return s;
}

void update_title(App& app) {
  std::string title = "xanatoo";
  if (!app.buffers.empty()) {
    title += " — ";
    title += buffer_tab_label(app, app.buffers.front().id);
    if (app.buffers.size() > 1) {
      title += " (+";
      title += std::to_string(app.buffers.size() - 1);
      title += " more)";
    }
  }
  SDL_SetWindowTitle(app.window, title.c_str());
}

xanatoo::Document& doc_of(App& app, Pane& p) {
  if (p.source_buf == p.buf && p.source_doc) return *p.source_doc;
  return buf(app, p.buf).doc;
}

std::string& path_of(App& app, Pane& p) { return buf(app, p.buf).path; }

std::string derived_content_path(const std::string& path) {
  if (!xanatoo::looks_like_local_path(path) || !xanatoo::is_pandoc_path(path))
    return {};
  return xanatoo::import_markdown_path(path);
}

const std::string& content_file(const DocBuffer& b) {
  return b.content_path.empty() ? b.path : b.content_path;
}

std::string base_of(App& app, Pane& p) {
  return parent_dir(content_file(buf(app, p.buf)));
}

bool shared_doc(App& app) { return app.panes[0].buf == app.panes[1].buf; }

Pane& focused_pane(App& app) { return app.panes[app.focus]; }

bool view_live(const App& app, int pi) {
  if (pi == 0) return true;
  if (pi == 1) return app.split;
  if (pi < kCentreViews || pi >= kMaxViews) return false;
  return std::any_of(app.soft_wins.begin(), app.soft_wins.end(),
                     [&](const FloatWindow& w) { return w.view == pi; });
}

std::vector<int> live_views(const App& app) {
  std::vector<int> v{0};
  if (app.split) v.push_back(1);
  for (const FloatWindow& w : app.soft_wins)
    if (w.view >= kCentreViews) v.push_back(w.view);
  return v;
}

int other_pane(const App& app) {
  return app.focus < kCentreViews ? 1 - app.focus : app.centre_focus;
}

void focus_view(App& app, int pi) {
  if (pi < 0 || pi >= kMaxViews) return;
  app.focus = pi;
  if (pi < kCentreViews) app.centre_focus = pi;
}

int alloc_view(App& app) {
  for (int pi = kCentreViews; pi < kMaxViews; ++pi)
    if (!view_live(app, pi)) return pi;
  return -1;
}

void refresh_views_of_buf(App& app, BufferId bi, int except, bool top) {
  DocBuffer* b = find_buf(app, bi);
  if (!b) return;
  ++b->doc_gen;
  if (top) b->folds.clear();  // keyed by the old document's nodes
  for (int pi : live_views(app)) {
    Pane& v = app.panes[pi];
    if (pi == except || v.buf != bi) continue;
    xanatoo::ensure_valid_caret(b->doc, v.sel.focus);
    v.sel.anchor = v.sel.focus;
    v.layout = {};
    if (top) v.scroll_y = 0.f;
    refresh_pane_locator(app, v);
  }
}

void release_view(App& app, int pi) {
  if (pi < kCentreViews || pi >= kMaxViews) return;
  cancel_fetches(app, pi);
  clear_interaction_hover(app);  // may point into this view
  Pane& p = app.panes[pi];
  release_raster(p.raster);
  p = Pane{};
  if (app.focus == pi) app.focus = app.centre_focus;
  if (app.scroll_drag_pane == pi) app.dragging_scroll = false;
}

void checkpoint(App& app) {
  Pane& p = focused_pane(app);
  buf(app, p.buf).history.push(doc_of(app, p), p.sel);
}

void mark_dirty(App& app) {
  const BufferId di = focused_pane(app).buf;
  app.discard_armed = false;  // new edits require a fresh discard warning
  if (find_buf(app, di)) {
    // The first edit to an imported local file detaches the editor from
    // the filesystem. The file remains an immutable source, not a save target.
    const std::string path = buf(app, di).path;
    std::error_code ec;
    if (!path.empty() && !is_stored_document_ref(path) &&
        !is_foreign_path(path) && fs::is_regular_file(path, ec) &&
        !find_imported_document(file_doc_key(path)))
      make_stored_copy(app, di);
    note_edit(app, buf(app, di));
    // Virtualization: drop height hints for the edited node and ancestors
    // so the next layout remeasures only the affected spine (not the world).
    for (int pi : live_views(app)) {
      if (app.panes[pi].buf == di)
        invalidate_height_around(app, app.panes[pi],
                                 focused_pane(app).sel.focus.node);
    }
  }
}

void note_edit(App& app, DocBuffer& b) {
  ++b.doc_gen;
  if (!b.draft_due) b.draft_due = xanatoo::sdl::ticks() + kDraftDelayMs;
  if (!b.dirty) {
    b.dirty = true;
    update_title(app);
  }
}

void edit(App& app) {
  checkpoint(app);
  mark_dirty(app);
}

std::string find_help_path(App& app) {
  return xanatoo::data_file("examples/help.md");
}

std::string find_data_file(App& app, const char* name) {
  // Installed data only (gamepkgstd DIR-3), never the working directory:
  // <data dir>/<name>, then <data dir>/examples/<name>.
  for (const std::string& rel : {std::string(name), "examples/" + std::string(name)})
    if (std::string p = xanatoo::data_file(rel); !p.empty()) return p;
  return {};
}

std::string canon_path(App& app, const std::string& path) {
  try {
    return fs::weakly_canonical(path).string();
  } catch (...) {
    return path;
  }
}

BufferId find_buffer(App& app, const std::string& path) {
  if (path.empty()) return kNoBuffer;
  std::string c = canon_path(app, path);
  for (const auto& b : app.buffers) {
    if (b.path.empty()) continue;
    if (canon_path(app, b.path) == c) return b.id;
  }
  return kNoBuffer;
}

BufferId ensure_buffer(App& app, const std::string& path) {
  BufferId bi = find_buffer(app, path);
  if (bi != kNoBuffer) {
    if (std::find(app.tab_order.begin(), app.tab_order.end(), bi) ==
        app.tab_order.end())
      app.tab_order.push_back(bi);
    return bi;
  }
  bi = add_buffer(app, load_doc(app, path), path);
  app.tab_order.push_back(bi);
  update_title(app);
  return bi;
}

namespace {

// New document window showing `bi` in a fresh view (cascaded from the help
// window's spot); nullptr when every view slot is taken.
FloatWindow* new_doc_window(App& app, BufferId bi) {
  const int view = alloc_view(app);
  if (view < 0) {
    flash_status(app, "Too many windows");
    return nullptr;
  }
  Pane& p = app.panes[view];
  p = Pane{};
  p.buf = bi;
  push_nav_history(app, view, buf(app, bi).path);
  xanatoo::ensure_valid_caret(buf(app, bi).doc, p.sel.focus);
  p.sel.anchor = p.sel.focus;
  FloatWindow sw;
  sw.view = view;
  const int cascade = static_cast<int>(app.soft_wins.size()) * 28;
  const int top = static_cast<int>(app.ui_top);
  sw.x = app.help.x + 30 + cascade;
  sw.y = std::max(app.help.y, top) + 30 + cascade;
  if (sw.x + sw.w > app.win_w - 20) sw.x = 40 + (cascade % 60);
  if (sw.y < top) sw.y = top + 20 + (cascade % 40);
  if (sw.y + sw.h > app.win_h - 40) sw.y = top + 20 + (cascade % 40);
  snap_soft_geom(app, sw.x, sw.y, sw.w, sw.h);
  sw.restore_x = sw.x;
  sw.restore_y = sw.y;
  app.soft_wins.push_back(std::move(sw));
  app.needs_layout = true;
  place_windows(app);
  return &app.soft_wins.back();
}

// Raise the window already showing `bi`; false when there is none.
bool raise_window_of(App& app, BufferId bi) {
  for (size_t i = 0; i < app.soft_wins.size(); ++i) {
    if (app.panes[app.soft_wins[i].view].buf != bi) continue;
    const int view = app.soft_wins[i].view;
    raise_soft(app, i);
    focus_view(app, view);
    return true;
  }
  return false;
}

}  // namespace

void pop_out_focused(App& app) {
  if (app.focus >= kCentreViews) {
    flash_status(app, "Already in a window");
    return;
  }
  Pane& p = focused_pane(app);
  const BufferId bi = p.buf;
  if (raise_window_of(app, bi)) {
    flash_status(app, "Already in a window");
    return;
  }
  const xanatoo::Selection sel = p.sel;
  FloatWindow* w = new_doc_window(app, bi);
  if (!w) return;
  Pane& v = app.panes[w->view];
  v.nav = view_nav(app, app.focus);  // the window takes the tab's trail along
  v.sel = sel;
  v.scroll_y = p.scroll_y;
  v.locator = p.locator;
  v.locator_buf = p.locator_buf;
  v.source_doc = p.source_doc;
  v.source_buf = p.source_buf;
  v.gallery = p.gallery;
  v.cut = p.cut;
  v.marks = p.marks;
  v.section_focus = p.section_focus;
  v.reader = p.reader;
  v.paged = p.paged;
  focus_view(app, w->view);
  flash_status(app, "Popped out to window");
}

void open_context_block(App& app, size_t index, bool new_window) {
  const int pi = app.context_blocks_pane;
  if (index >= app.context_blocks.size() || pi < 0 ||
      app.panes[pi].buf != app.context_blocks_buf ||
      !find_buf(app, app.context_blocks_buf))
    return;
  const xanatoo::NodeId node = app.context_blocks[index];
  Pane& from = app.panes[pi];
  const xanatoo::Document& d = doc_of(app, from);
  if (node >= d.next_id || d.get(node).id != node) return;
  const std::string address = block_address(app, from, node);
  if (!new_window) {
    focus_view(app, pi);
    navigate_pane(app, pi, address);
    return;
  }
  FloatWindow* w = new_doc_window(app, app.context_blocks_buf);
  if (!w) return;
  const int view = w->view;
  Pane& v = app.panes[view];
  v.reader = from.reader;
  v.paged = from.paged;
  v.nav = {};
  push_nav_history(app, view, address);
  set_pane_cut(app, v, node);
  const xanatoo::Selection r = xanatoo::node_range(d, node);
  v.sel.anchor = v.sel.focus = r.anchor;
  xanatoo::ensure_valid_caret(buf(app, app.context_blocks_buf).doc, v.sel.focus);
  v.sel.anchor = v.sel.focus;
  focus_view(app, view);
}

void tear_tab(App& app, BufferId bi, int mx, int my) {
  if (!find_buf(app, bi)) return;
  if (raise_window_of(app, bi)) {
    leave_tab(app, bi);
    return;
  }
  // The pane showing the tab hands its place in the page to the window.
  const Pane* from = nullptr;
  for (int pi = 0; pi < kCentreViews; ++pi)
    if (app.panes[pi].buf == bi) from = &app.panes[pi];
  const xanatoo::Selection sel = from ? from->sel : xanatoo::Selection{};
  const float scroll = from ? from->scroll_y : 0.f;
  const NavHistory nav = buf(app, bi).tab_nav;
  const xanatoo::Locator locator = from ? from->locator : xanatoo::Locator{};
  const BufferId locator_buf = from ? from->locator_buf : kNoBuffer;
  const std::optional<xanatoo::Document> source_doc =
      from ? from->source_doc : std::nullopt;
  const BufferId source_buf = from ? from->source_buf : kNoBuffer;
  const GalleryView gallery = from ? from->gallery : GalleryView{};
  const xanatoo::NodeId cut = from ? from->cut : xanatoo::INVALID_NODE;
  const std::vector<xanatoo::Selection> marks =
      from ? from->marks : std::vector<xanatoo::Selection>{};
  const bool section_focus = from && from->section_focus;
  const std::optional<bool> reader = from ? from->reader : std::nullopt;
  const std::optional<bool> paged = from ? from->paged : std::nullopt;
  FloatWindow* w = new_doc_window(app, bi);
  if (!w) return;
  w->x = std::max(0, mx - w->w / 2);
  w->y = std::max(static_cast<int>(app.ui_top), my - help_title_h / 2);
  w->restore_x = w->x;
  w->restore_y = w->y;
  Pane& v = app.panes[w->view];
  v.nav = nav;
  v.sel = sel;
  v.scroll_y = scroll;
  v.locator = locator;
  v.locator_buf = locator_buf;
  v.source_doc = source_doc;
  v.source_buf = source_buf;
  v.gallery = gallery;
  v.cut = cut;
  v.marks = marks;
  v.section_focus = section_focus;
  v.reader = reader;
  v.paged = paged;
  const int view = w->view;
  leave_tab(app, bi);
  // new_doc_window may have moved soft_wins; look the window up again.
  if (FloatWindow* nw = window_of_view(app, view)) {
    const Dock zone = compute_dock_zone(app, mx, my);
    if (zone != Dock::None)
      dock_window(app, *nw, zone, mx, my);
    else
      snap_soft_geom(app, nw->x, nw->y, nw->w, nw->h);
  }
  focus_view(app, view);
  flash_status(app, "Tab moved to a window");
}

void pop_in_soft(App& app, size_t si) {
  if (si >= app.soft_wins.size()) return;
  if (app.soft_wins[si].role != WindowRole::Document) return;
  const int view = app.soft_wins[si].view;
  Pane& v = app.panes[view];
  const int to = app.centre_focus;
  if (find_buf(app, v.buf)) {
    Pane& c = app.panes[to];
    c.buf = v.buf;
    buf(app, v.buf).tab_nav = std::move(v.nav);
    app.split_history = {};
    app.split_trail = {};
    c.sel = v.sel;
    c.scroll_y = v.scroll_y;
    c.locator = std::move(v.locator);
    c.locator_buf = v.locator_buf;
    c.source_doc = std::move(v.source_doc);
    c.source_buf = v.source_buf;
    c.gallery = std::move(v.gallery);
    c.cut = v.cut;
    c.marks = std::move(v.marks);
    c.section_focus = v.section_focus;
    c.reader = v.reader;
    c.paged = v.paged;
    c.layout = {};
    c.layout_buf = kNoBuffer;
    c.pages = {};
    c.height_cache.clear();
    xanatoo::ensure_valid_caret(doc_of(app, c), c.sel.focus);
    if (std::find(app.tab_order.begin(), app.tab_order.end(), c.buf) ==
        app.tab_order.end())
      app.tab_order.push_back(c.buf);
  }
  release_view(app, view);
  app.soft_wins.erase(app.soft_wins.begin() + static_cast<std::ptrdiff_t>(si));
  focus_view(app, to);
  app.needs_layout = true;
  update_title(app);
  flash_status(app, "Moved window to tab");
}

void open_browse(App& app, const std::string& path, const std::string& title) {
  if (path.empty() || !fs::exists(path)) {
    std::cerr << "Cannot open in window: " << path << "\n";
    return;
  }
  const BufferId bi = ensure_buffer(app, path);
  if (raise_window_of(app, bi)) return;
  FloatWindow* w = new_doc_window(app, bi);
  if (!w) return;
  if (!title.empty()) w->title = title;
  std::cout << "Software window: " << path << "\n";
}

void ensure_help_doc(App& app) {
  if (app.help_loaded) return;
  std::string hp = find_help_path(app);
  if (!hp.empty()) {
    app.help_doc = load_doc(app, hp);
    std::cout << "Help document: " << hp << "\n";
  } else {
    // Minimal built-in so F1 always works offline
    app.help_doc = xanatoo::parse_document(
        "# xanatoo help\n\n"
        "The full reference is `examples/help.md` in the data directory "
        "(`xanatoo --show-paths`, `--data-dir`).\n\n"
        "Press **F1** or **Esc** to close.\n",
        {});
    std::cerr << "examples/help.md not found; using short built-in help.\n";
  }
  app.help_loaded = true;
  app.help_needs_layout = true;
  app.help.scroll = 0.f;
}

void layout_help(App& app) {
  if (!app.help_loaded) return;
  float mw = static_cast<float>(std::max(120, std::get<2>(help_client_rect(app))));
  app.help_layout = app.layout_engine.layout(app.help_doc, mw);
  app.help_needs_layout = false;
}

void layout_context(App& app) {
  if (!app.context_has_doc) return;
  auto [cx, cy, cw, ch] = float_client_rect(app, app.context_win);
  (void)cx; (void)cy; (void)ch;
  float mw = static_cast<float>(std::max(120, cw));
  app.context_layout = app.layout_engine.layout(app.context_doc, mw);
  app.context_needs_layout = false;
  // Scroll so the target node is in view once we have a layout.
  if (app.context_scroll_to != xanatoo::INVALID_NODE) {
    xanatoo::Caret caret{app.context_scroll_to, 0, 0};
    auto cpos = xanatoo::caret_screen_pos(
        app.context_layout, caret, 0.f,
        static_cast<float>(app.fonts.regular.line_height()));
    if (cpos.valid) {
      auto [cx2, cy2, cw2, ch2] = float_client_rect(app, app.context_win);
      (void)cx2; (void)cw2;
      const float view_h = static_cast<float>(std::max(1, ch2));
      app.context_win.scroll =
          std::max(0.f, cpos.y - view_h * 0.25f);
    }
    app.context_scroll_to = xanatoo::INVALID_NODE;
  }
}

std::tuple<int, int, int, int> help_client_rect(App& app) {
  return window_client_rect(app.help);
}

}  // namespace xanatoo::app
