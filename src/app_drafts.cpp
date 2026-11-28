// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
//
// Drafts: unsaved edits go to the local revision store (revstore.hpp)
// instead of being guarded by "discard?" confirmations.

#include "xanatoo/app.hpp"
#include "xanatoo/parse.hpp"
#include "xanatoo/revstore.hpp"
#include "xanatoo/util.hpp"

#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace fs = std::filesystem;

namespace xanatoo::app {

namespace {

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

bool regular_file(const std::string& path) {
  std::error_code ec;
  return fs::is_regular_file(path, ec);
}

// Label for the drafts page: file name, else the first line of the text.
std::string draft_title(const std::string& path, const std::string& text) {
  if (!path.empty() && !is_stored_document_ref(path))
    return path_title(path);
  std::string_view rest = text;
  while (!rest.empty()) {
    const size_t nl = rest.find('\n');
    std::string_view line = rest.substr(0, nl);
    rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
    while (!line.empty() && (line.front() == '#' || line.front() == '*' ||
                             line.front() == ' '))
      line.remove_prefix(1);
    trim_ascii(line);
    if (line.empty() || line == "---") continue;
    std::string t(line.substr(0, 60));
    if (line.size() > 60) t += "…";
    return t;
  }
  return "untitled";
}

// Pathless documents are database-native. Their working revisions are
// committed on the same debounce as legacy file drafts, but never written to
// a file. Publishing advances a different head (see revstore.cpp).
bool write_stored_working(App& app, DocBuffer& b) {
  if (b.blank_tab || b.placeholder ||
      (!b.path.empty() && !is_stored_document_ref(b.path)))
    return false;
  if (b.doc_key.empty()) b.doc_key = new_doc_key();
  const std::string text = serialize_stored_document(b.doc);
  const std::string title = draft_title(b.path, serialize_document(b.doc, {}));
  const auto head = save_stored_document(b.doc_key, text, b.stored_working, title);
  if (!head) {
    std::cerr << "Document store unavailable or working revision changed\n";
    return false;
  }
  if (!b.draft_id.empty()) drop_draft(b.doc_key);
  if (b.doc_key == app.revision_document_key && b.stored_working != head->working)
    app.revision_working.clear();
  b.stored_working = head->working;
  b.stored_title = head->title;
  b.path = "db:" + b.doc_key;
  b.base_id = content_id(serialize_document(b.doc, {}));
  b.draft_id.clear();
  b.draft_due = 0;
  b.dirty = false;
  update_title(app);
  return true;
}

// Write `b`'s current text as its draft (no-op when unchanged). Undoing
// every edit drops the draft and makes the buffer clean again.
bool write_draft(App& app, DocBuffer& b) {
  b.draft_due = 0;
  if (b.path.empty() || is_stored_document_ref(b.path))
    return write_stored_working(app, b);
  const std::string text = serialize_document(b.doc, b.path);
  const std::string cid = content_id(text);
  if (cid == b.draft_id) return true;
  if (!b.base_id.empty() && cid == b.base_id) {
    if (!b.draft_id.empty()) drop_draft(b.doc_key);
    b.draft_id.clear();
    b.dirty = false;
    update_title(app);
    return true;
  }
  if (b.doc_key.empty())
    b.doc_key = b.path.empty() ? new_doc_key() : file_doc_key(b.path);
  // The first draft of a file version also stores what is on disk.
  const std::string base_text =
      b.draft_id.empty() && !b.path.empty() ? read_file(b.path) : std::string();
  const std::string id = put_draft(b.doc_key, text, base_text,
                                   draft_title(b.path, text));
  if (id.empty()) {
    std::cerr << "Draft store unavailable — unsaved edits are only in memory\n";
    return false;
  }
  b.draft_id = id;
  return true;
}

std::string format_time(int64_t t) {
  const std::time_t tt = static_cast<std::time_t>(t);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &tt);
#else
  localtime_r(&tt, &tm);
#endif
  char s[32];
  std::strftime(s, sizeof s, "%Y-%m-%d %H:%M", &tm);
  return s;
}

}  // namespace

bool attach_draft(App& app, BufferId id) {
  DocBuffer* b = find_buf(app, id);
  if (!b) return false;
  b->doc_key.clear();
  b->base_id.clear();
  b->draft_id.clear();
  b->stored_working.clear();
  b->stored_title.clear();
  b->draft_due = 0;
  if (b->path.empty() || b->placeholder || is_foreign_path(b->path) ||
      !regular_file(b->path))
    return false;
  b->doc_key = file_doc_key(b->path);
  b->base_id = content_id(serialize_document(b->doc, b->path));
  const auto d = find_draft(b->doc_key);
  if (!d) return false;
  const auto body = load_object(d->content);
  if (!body) return false;
  b->doc = parse_document(*body, b->path);
  b->draft_id = d->content;
  b->dirty = true;
  update_title(app);
  if (d->base != content_id(read_file(b->path)))
    flash_status(app, "Restored unsaved changes — the file has changed on "
                      "disk since (File → Revert to Saved shows it)");
  else
    flash_status(app, "Restored unsaved changes — Ctrl+S saves, "
                      "File → Revert to Saved drops them");
  return true;
}

bool stash_draft(App& app, BufferId id) {
  DocBuffer* b = find_buf(app, id);
  if (!b || !b->dirty || b->placeholder) return true;
  // Foreign pages are read-only; only view toggles can dirty them.
  if (!b->path.empty() && is_foreign_path(b->path)) return true;
  return write_draft(app, *b);
}

bool stash_or_confirm(App& app, BufferId id) {
  if (stash_draft(app, id) || app.discard_armed) {
    app.discard_armed = false;
    return true;
  }
  app.discard_armed = true;
  flash_status(app, "Could not autosave changes. Export a copy or repeat to discard.");
  return false;
}

bool stash_all_or_confirm(App& app) {
  bool ok = true;
  for (const DocBuffer& b : app.buffers)
    if (!stash_draft(app, b.id)) ok = false;
  if (ok || app.discard_armed) {
    app.discard_armed = false;
    return true;
  }
  app.discard_armed = true;
  flash_status(app, "Could not autosave changes. Export a copy or repeat to discard.");
  return false;
}

void autosave_drafts(App& app) {
  const Uint32 now = xanatoo::sdl::ticks();
  for (DocBuffer& b : app.buffers)
    if (b.draft_due && xanatoo::sdl::ticks_passed(now, b.draft_due)) {
      if (b.dirty && !b.placeholder &&
          (b.path.empty() || !is_foreign_path(b.path)))
        write_draft(app, b);
      b.draft_due = 0;
    }
}

void note_saved(App& app, BufferId id, const std::string& text) {
  DocBuffer* b = find_buf(app, id);
  if (!b || b->path.empty()) return;
  const std::string key = file_doc_key(b->path);
  if (!b->doc_key.empty() && b->doc_key != key && !b->draft_id.empty())
    drop_draft(b->doc_key);  // Save As: the draft lived under the old key
  b->doc_key = key;
  b->stored_working.clear();
  b->stored_title.clear();
  record_save(key, text);
  b->base_id = content_id(text);
  b->draft_id.clear();
  b->draft_due = 0;
}

bool save_stored_buffer(App& app, BufferId id) {
  DocBuffer* b = find_buf(app, id);
  return b && write_stored_working(app, *b);
}

bool make_stored_copy(App& app, BufferId id) {
  DocBuffer* b = find_buf(app, id);
  if (!b || b->placeholder) return false;
  if (is_stored_document_ref(b->path)) return true;
  if (is_stored_original_ref(b->path)) {
    const bool ok = open_stored_document(
        app, app.focus, "db:" + b->path.substr(12));
    return ok;
  }
  if (b->path.empty()) return write_stored_working(app, *b);

  const bool remote = is_in_app_remote_url(b->path);
  const bool local = !remote && regular_file(b->path);
  if (!remote && !local) return false;
  const std::string source = remote ? b->path : file_doc_key(b->path);
  if (const auto existing = find_imported_document(source)) {
    if (b->dirty) {
      flash_status(app, "Local copy already exists; unsaved file edits remain in drafts");
      return false;
    }
    const bool ok = open_stored_document(app, app.focus, "db:" + existing->key);
    return ok;
  }
  if (remote && !b->raw_source) {
    flash_status(app, "Cannot preserve original source: response bytes unavailable");
    return false;
  }
  const std::string raw = remote ? *b->raw_source : read_file(b->path);
  std::string initial, working;
  try {
    initial = serialize_stored_document(remote ? b->doc : parse_document(raw, b->path));
    working = serialize_stored_document(b->doc);
  } catch (const std::invalid_argument&) {
    flash_status(app, "Cannot import an invalid source document");
    return false;
  }
  const auto head = import_stored_document(
      source, raw, initial, working,
      draft_title(b->path, serialize_document(b->doc, {})));
  if (!head) return false;
  if (!b->draft_id.empty()) drop_draft(b->doc_key);
  b->doc_key = head->key;
  b->path = "db:" + head->key;
  b->stored_working = head->working;
  b->stored_title = head->title;
  b->base_id = content_id(serialize_document(b->doc, {}));
  b->draft_id.clear();
  b->draft_due = 0;
  b->dirty = false;
  b->raw_source.reset();
  update_title(app);
  return true;
}

bool select_buffer_revision(App& app, BufferId id, const std::string& revision) {
  const DocBuffer* selected = find_buf(app, id);
  if (!selected || !is_stored_document_ref(selected->path)) return false;
  const std::string key = selected->doc_key;
  for (const DocBuffer& b : app.buffers) {
    if (b.id != id && b.doc_key == key && b.dirty) {
      flash_status(app, "Another view has pending edits to this document");
      return false;
    }
  }
  const auto body = load_stored_document_revision(key, revision);
  if (!body || !stash_draft(app, id)) {
    flash_status(app, "Could not switch revision");
    return false;
  }
  Document doc = parse_stored_document(*body);
  const std::string text = serialize_document(doc, {});
  DocBuffer& b = buf(app, id);
  const auto head = select_stored_document_revision(
      key, revision, b.stored_working, draft_title(b.path, text));
  if (!head) {
    flash_status(app, "Document changed elsewhere. Reload to see its current revision.");
    return false;
  }
  // Selection alone leaves the graph intact. Autosave clears the cache
  // marker if stashing the previous selection added a child.
  if (key == app.revision_document_key &&
      app.revision_working == b.stored_working)
    app.revision_working = head->working;
  const std::string base = content_id(text);
  clear_interaction_hover(app);
  keep_views(app);
  for (DocBuffer& copy : app.buffers) {
    if (copy.doc_key != key || !is_stored_document_ref(copy.path)) continue;
    for (int pi : live_views(app)) {
      Pane& p = app.panes[pi];
      if (p.buf != copy.id) continue;
      p.height_cache.clear();
      if (!p.view_anchor) continue;
      const NodeId node = p.view_anchor->pos.node;
      const Node& previous = copy.doc.get(node);
      // Stored XML can renumber nodes. Find the same text block, preferring
      // the nearest occurrence by node order if the text appears twice.
      const std::string anchor_text = p.source_buf == p.buf || previous.id != node
          ? std::string{} : plain_text(copy.doc, node);
      NodeId match = INVALID_NODE;
      if (!anchor_text.empty() && is_text_node_kind(previous.kind)) {
        if (doc.get(node).id == node && doc.get(node).kind == previous.kind &&
            plain_text(doc, node) == anchor_text) {
          match = node;
        } else {
          auto distance = [node](NodeId other) {
            return node > other ? node - other : other - node;
          };
          for (const Node& candidate : doc.nodes) {
            if (candidate.id == INVALID_NODE || candidate.kind != previous.kind ||
                (match != INVALID_NODE && distance(candidate.id) >= distance(match)))
              continue;
            if (plain_text(doc, candidate.id) == anchor_text) match = candidate.id;
          }
        }
      }
      if (match == INVALID_NODE) {
        p.view_anchor.reset();
      } else {
        p.view_anchor->pos.node = match;
        ensure_valid_caret(doc, p.view_anchor->pos);
      }
    }
    copy.doc = doc;
    copy.folds.clear();
    copy.stored_working = head->working;
    copy.stored_title = head->title;
    copy.base_id = base;
    Selection caret;
    ensure_valid_caret(copy.doc, caret.focus);
    caret.anchor = caret.focus;
    copy.history.reset(copy.doc, caret);
    for (int pi : live_views(app)) {
      Pane& p = app.panes[pi];
      if (p.buf != copy.id) continue;
      p.sel = caret;
    }
    refresh_views_of_buf(app, copy.id, -1, false);
  }
  app.needs_layout = true;
  update_title(app);
  return true;
}

void revert_buffer(App& app, BufferId id) {
  DocBuffer* b = find_buf(app, id);
  if (!b) return;
  if (!b->doc_key.empty()) drop_draft(b->doc_key);
  b->draft_id.clear();
  b->draft_due = 0;
  b->dirty = false;
}

void open_drafts_page(App& app) {
  std::string md = "# Unsaved drafts\n\n";
  md += "Edits are kept here until they are saved (Ctrl+S) or dropped "
        "(File → Revert to Saved). Older versions stay in the revision "
        "store.\n\n";
  const auto drafts = list_drafts();
  if (drafts.empty()) md += "No unsaved drafts.\n";
  for (const DraftHead& d : drafts) {
    const bool file = starts_with(d.key, "file:");
    const std::string target = file ? d.key.substr(5) : "draft:" + d.key;
    md += "- [" + (d.title.empty() ? std::string("untitled") : d.title) +
          "](" + target + ") — ";
    md += file ? "`" + target + "`" : std::string("no file");
    md += ", " + format_time(d.time) + "\n";
  }
  show_doc_in_pane(app, app.focus, parse_document(md, {}), kDraftsPage);
}

bool open_draft(App& app, int pi, const std::string& ref) {
  const std::string key = ref.substr(6);  // after "draft:"
  if (starts_with(key, "file:")) return navigate_pane(app, pi, key.substr(5));
  for (const DocBuffer& b : app.buffers)
    if (b.doc_key == key) {
      app.panes[pi].buf = b.id;
      app.panes[pi].layout = {};
      app.needs_layout = true;
      update_title(app);
      return true;
    }
  const auto d = find_draft(key);
  const auto body = d ? load_object(d->content) : std::nullopt;
  if (!body) {
    flash_status(app, "Draft not found");
    return false;
  }
  show_doc_in_pane(app, pi, parse_document(*body, {}), {});
  DocBuffer& b = buf(app, app.panes[pi].buf);
  b.doc_key = key;
  b.draft_id = d->content;
  b.dirty = true;
  update_title(app);
  flash_status(app, "Draft restored — Save As gives it a file");
  return true;
}

void open_stored_documents_page(App& app) {
  std::string md = "# Local documents\n\n"
                   "Edits are saved automatically. Open Document Revisions "
                   "to move through a document’s history.\n\n";
  const auto documents = list_stored_documents();
  if (documents.empty()) md += "No local documents. Use File → New Document.\n";
  for (const StoredDocument& d : documents) {
    std::string title = d.title.empty() ? "untitled" : d.title;
    // The index is Markdown; escape user-controlled link markup in titles.
    for (char& c : title)
      if (c == '[' || c == ']' || c == '(' || c == ')' || c == '\n' ||
          c == '\r')
        c = ' ';
    md += "- [" + title + "](db:" + d.key + ")";
    if (find_document_origin(d.key))
      md += " · [Original](db-original:" + d.key + ")";
    md += " — " +
          format_time(d.time) + "\n";
  }
  show_doc_in_pane(app, app.focus, parse_document(md, {}), kDocumentsPage);
}

bool open_stored_document(App& app, int pi, const std::string& ref) {
  if (!is_stored_document_ref(ref)) return false;
  if (!stash_or_confirm(app, app.panes[pi].buf)) return false;
  const std::string key = ref.substr(3);
  const auto head = find_stored_document(key);
  const auto body = head ? load_stored_document_revision(key, head->working)
                         : std::nullopt;
  if (!body) {
    flash_status(app, "Local document not found");
    return false;
  }
  show_doc_in_pane(app, pi, parse_stored_document(*body), ref);
  DocBuffer& b = buf(app, app.panes[pi].buf);
  b.doc_key = key;
  b.stored_working = head->working;
  b.stored_title = head->title;
  b.base_id = content_id(serialize_document(b.doc, {}));
  b.dirty = false;
  return true;
}

std::optional<Document> load_stored_revision_ref(std::string_view path) {
  if (!is_stored_revision_ref(path)) return std::nullopt;
  const auto body = load_stored_document_revision(path.substr(12, 68), path.substr(81));
  if (!body) return std::nullopt;
  try {
    return parse_stored_document(*body);
  } catch (const std::invalid_argument&) {
    return std::nullopt;
  }
}

bool open_stored_original(App& app, int pi, const std::string& ref) {
  if (!is_stored_original_ref(ref)) return false;
  const std::string key = ref.substr(12);
  const auto origin = find_document_origin(key);
  const auto body = origin ? load_stored_document_revision(key, origin->revision)
                           : std::nullopt;
  if (!body) {
    flash_status(app, "Original source snapshot not found");
    return false;
  }
  if (!stash_or_confirm(app, app.panes[pi].buf)) return false;
  show_doc_in_pane(app, pi, parse_stored_document(*body), ref);
  return true;
}

}  // namespace xanatoo::app
