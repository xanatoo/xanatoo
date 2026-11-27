// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include <algorithm>
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
#include "xanatoo/html_page.hpp"
#include "xanatoo/epub_import.hpp"
#include "xanatoo/gemini.hpp"
#include "xanatoo/lemmy.hpp"
#include "xanatoo/nostr.hpp"
#include "xanatoo/paths.hpp"
#include "xanatoo/parse.hpp"
#include "xanatoo/reddit.hpp"
#include "xanatoo/revstore.hpp"
#include "xanatoo/sidestore.hpp"
#include "xanatoo/statedb.hpp"
#include "xanatoo/util.hpp"

namespace fs = std::filesystem;

namespace xanatoo::app {

bool prompt_uses_minibuf(PromptKind k) {
  switch (k) {
    case PromptKind::SaveAs:
    case PromptKind::SavePdf:
    case PromptKind::OpenFile:
    case PromptKind::OpenInWindow:
    case PromptKind::UrlBar:
    case PromptKind::InsertImage:
    case PromptKind::InsertFigure:
    case PromptKind::InsertInclude:
      return true;
    default:
      return false;
  }
}

bool minibuf_active(App& app) { return prompt_uses_minibuf(app.prompt.kind); }

const char* prompt_label(PromptKind k) {
  switch (k) {
    case PromptKind::OpenInWindow:
      return "Open in window: ";
    case PromptKind::OpenFile:
      return "Open file: ";
    case PromptKind::UrlBar:
      return "URL: ";
    case PromptKind::SaveAs:
      return "Export as: ";
    case PromptKind::SavePdf:
      return "Save PDF as: ";
    case PromptKind::InsertImage:
      return "Image path: ";
    case PromptKind::InsertFigure:
      return "Figure image path: ";
    case PromptKind::InsertInclude:
      return "Include: ";
    default:
      return "";
  }
}

void open_prompt(App& app, PromptKind kind, std::string initial) {
  close_prompt(app);
  app.prompt.kind = kind;
  app.prompt.text = std::move(initial);
  if (prompt_uses_minibuf(kind)) minibuf_begin(app, app.prompt.text);
}

void open_search(App& app, bool replace, bool focus) {
  if (!prompt_is_search(app)) {
    open_prompt(app, PromptKind::Search, app.last_search);
    app.prompt.text2 = app.last_replace;
  }
  Prompt& pr = app.prompt;
  pr.kind = replace ? PromptKind::Replace : PromptKind::Search;
  if (focus) {
    pr.pending = !app.ui.find_field_active();
    pr.fresh = true;
    ++app.find_focus_seq;
    app.key_focus = KeyFocus::Document;
  }
  reset_blink(app);
}

void leave_prompt(App& app) {
  if (prompt_is_search(app))
    app.prompt.pending = false;
  else
    close_prompt(app);
}

void close_prompt(App& app) {
  if (prompt_is_search(app)) {
    app.last_search = app.prompt.text;
    app.last_replace = app.prompt.text2;
  }
  if (prompt_uses_minibuf(app.prompt.kind)) app.minibuf.clear();
  app.prompt = Prompt{};
}

void minibuf_begin(App& app, const std::string& initial) {
  app.minibuf.clear();
  if (!initial.empty()) app.minibuf.set_text(initial);
}

void minibuf_commit_path_hist(App& app) {
  xanatoo::record_input_history("path", app.minibuf_hist_path, app.minibuf.text);
}

void minibuf_commit_url_hist(App& app) {
  xanatoo::record_input_history("url", app.minibuf_hist_url, app.minibuf.text);
}

void minibuf_sync_out(App& app) {
  if (minibuf_active(app)) app.prompt.text = app.minibuf.text;
}

bool commit_prompt(App& app, bool ctrl, bool shift) {
  if (!prompt_has_keys(app)) return false;
  Pane& p = focused_pane(app);
  xanatoo::Document& d = doc_of(app, p);
  Prompt& pr = app.prompt;
  switch (pr.kind) {
    case PromptKind::None:
      return false;

    case PromptKind::Search:
    case PromptKind::Replace:
      if (ctrl && shift && pr.kind == PromptKind::Replace)
        find_replace_all(app);
      else
        find_step(app, shift);
      return true;

    case PromptKind::EditLink:
    case PromptKind::EditCite: {
      const bool is_cite = pr.kind == PromptKind::EditCite;
      if (pr.node != xanatoo::INVALID_NODE) {
        auto& node = d.get(pr.node);
        // BiblioEntry: edit the entry's label (cite target key)
        if (node.kind == xanatoo::NodeKind::BiblioEntry && is_cite) {
          std::string key = pr.text;
          if (key.rfind("cite:", 0) == 0) key = key.substr(5);
          if (!key.empty() && key[0] == '#') key = key.substr(1);
          if (key.empty()) key = "key";
          node.label = key;
          mark_dirty(app);
          app.needs_layout = true;
          std::cout << "Bibliography key: " << key << "\n";
        } else if (pr.span < node.spans.size()) {
          auto& sp = node.spans[pr.span];
          if (is_cite) {
            // Strip optional cite: / # prefix from the typed key
            std::string key = pr.text;
            if (key.rfind("cite:", 0) == 0) key = key.substr(5);
            if (!key.empty() && key[0] == '#') key = key.substr(1);
            sp.href = key;
            sp.kind = xanatoo::InlineKind::Cite;
            std::cout << "Citation key: " << key << "\n";
          } else {
            // Accept "href rel=glossary" or "href {rel=glossary}"
            std::string raw = pr.text;
            std::string rel;
            auto strip_rel = [&](std::string& s) {
              auto rpos = s.find("rel=");
              if (rpos == std::string::npos) return;
              size_t after = rpos + 4;
              while (after < s.size() && (s[after] == ' ' || s[after] == '\t'))
                ++after;
              size_t end = after;
              while (end < s.size() && s[end] != ' ' && s[end] != '\t' &&
                     s[end] != '}')
                ++end;
              rel = s.substr(after, end - after);
              // Remove the rel clause (and optional braces) from href
              size_t cut = rpos;
              while (cut > 0 && (s[cut - 1] == ' ' || s[cut - 1] == '\t' ||
                                 s[cut - 1] == '{'))
                --cut;
              s = s.substr(0, cut);
              while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
                s.pop_back();
            };
            strip_rel(raw);
            sp.href = raw;
            if (!rel.empty()) sp.rel = rel;
            sp.kind = xanatoo::InlineKind::Link;
            std::cout << "Link href: " << sp.href;
            if (!sp.rel.empty()) std::cout << " rel=" << sp.rel;
            std::cout << "\n";
          }
          mark_dirty(app);
          app.needs_layout = true;
        }
      }
      close_prompt(app);
      return true;
    }

    case PromptKind::HistoryFilter:
      app.history.filter = pr.text;
      close_prompt(app);
      return true;

    case PromptKind::EditSummary:
      if (pr.node != xanatoo::INVALID_NODE) {
        d.get(pr.node).summary = pr.text;
        mark_dirty(app);
        app.needs_layout = true;
      }
      close_prompt(app);
      return true;

    case PromptKind::OpenInWindow: {
      minibuf_sync_out(app);
      if (!pr.text.empty()) {
        std::string path = xanatoo::expand_user_path(pr.text);
        if (!fs::exists(path)) {
          std::string found = find_data_file(app, path.c_str());
          if (!found.empty()) path = found;
        }
        open_browse(app, path, path);
        minibuf_commit_path_hist(app);
      }
      close_prompt(app);
      return true;
    }

    case PromptKind::UrlBar: {
      minibuf_sync_out(app);
      std::string target = pr.text;
      if (!target.empty()) minibuf_commit_url_hist(app);
      close_prompt(app);
      app.visit_hint = xanatoo::VisitKind::Address;
      navigate_to_url(app, target.empty() ? target : complete_location(app, target));
      app.visit_hint.reset();
      return true;
    }

    case PromptKind::OpenFile: {
      minibuf_sync_out(app);
      if (pr.text.empty()) {
        close_prompt(app);
        return true;
      }
      std::string path = xanatoo::expand_user_path(pr.text);
      if (!fs::exists(path)) {
        std::string found = find_data_file(app, path.c_str());
        if (!found.empty()) path = found;
      }
      if (!fs::exists(path)) {
        flash_status(app, "Open: file not found");
        std::cerr << "Open: file not found: " << path << "\n";
        return true;  // prompt stays open
      }
      if (const auto stored = xanatoo::find_imported_document(
              xanatoo::file_doc_key(path))) {
        if (navigate_pane(app, app.focus, "db:" + stored->key)) {
          minibuf_commit_path_hist(app);
          close_prompt(app);
        }
        return true;
      }
      const BufferId existing = find_buffer(app, path);
      if (existing != kNoBuffer && app.focus < kCentreViews) {
        select_view_tab(app, app.focus, existing);
        flash_status(app, "Switched to " + path);
        minibuf_commit_path_hist(app);
        close_prompt(app);
        return true;
      }
      NavHistory& nav = view_nav(app, app.focus);
      nav_history_capture(nav, pane_address(app, p), p.sel, p.scroll_y);
      nav.pending_link = {};
      nav.restoring = -1;
      if (!stash_or_confirm(app, p.buf)) return true;
      DocBuffer& b = buf(app, p.buf);
      b.doc = load_doc(app, path);
      b.path = path;
      b.content_path = derived_content_path(path);
      b.dirty = false;
      attach_draft(app, p.buf);
      xanatoo::ensure_valid_caret(b.doc, p.sel.focus);
      p.sel.anchor = p.sel.focus;
      p.scroll_y = 0.f;
      b.history.reset(b.doc, p.sel);
      refresh_views_of_buf(app, p.buf, app.focus, true);
      // Ensure path is on the tab strip
      if (std::find(app.tab_order.begin(), app.tab_order.end(), p.buf) ==
          app.tab_order.end())
        app.tab_order.push_back(p.buf);
      app.needs_layout = true;
      update_title(app);
      push_nav_history(app, app.focus, path);
      flash_status(app, "Opened " + path);
      std::cout << "Opened: " << path << "\n";
      minibuf_commit_path_hist(app);
      close_prompt(app);
      return true;
    }

    case PromptKind::SavePdf: {
      minibuf_sync_out(app);
      if (!pr.text.empty()) {
        const fs::path target = xanatoo::expand_user_path(pr.text);
        const std::string downloads = xanatoo::downloads_dir();
        if (!downloads.empty() && target.parent_path() == fs::path(downloads))
          xanatoo::ensure_dir(downloads);
        std::error_code ec;
        if (fs::copy_file(pr.source_path, target, fs::copy_options::none, ec)) {
          minibuf_commit_path_hist(app);
          flash_status(app, "Saved PDF to " + target.string());
        } else if (ec == std::errc::file_exists) {
          flash_status(app, "PDF destination already exists. Choose another filename.");
        } else {
          flash_status(app, "Could not save PDF: " + ec.message());
        }
      }
      close_prompt(app);
      return true;
    }

    case PromptKind::SaveAs: {
      minibuf_sync_out(app);
      if (!pr.text.empty()) {
        const std::string target = xanatoo::expand_user_path(pr.text);
        // Export never changes the document's source or identity, and never
        // truncates an existing file without an explicit overwrite flow.
        std::error_code exists_ec;
        if (fs::exists(target, exists_ec)) {
          flash_status(app, "Export target already exists; choose another filename");
          return true;
        }
        if (app.invalid_xml_paths.contains(target)) {
          flash_status(app, "Cannot overwrite an XML file that failed to import");
          return true;
        }
        if (xanatoo::path_uses_sexpr(target)) {
          flash_status(app, "S-expressions are read-only — use .xml, .md or .org");
          return true;
        }
        if (xanatoo::path_uses_html(target)) {
          flash_status(app, "Cannot write HTML — use .xml, .md or .org");
          return true;
        }
        std::string text;
        try {
          text = xanatoo::serialize_document(buf(app, p.buf).doc, target);
        } catch (const std::invalid_argument& e) {
          std::cerr << "Cannot save " << target << ": " << e.what() << "\n";
          flash_status(app, e.what());
          return true;
        }
        std::ofstream out(target);
        if (!out) {
          std::cerr << "Failed to save: " << target << "\n";
        } else {
          out << text;
          flash_status(app, "Exported " + target);
          std::cout << "Exported: " << target << "\n";
          minibuf_commit_path_hist(app);
        }
      }
      close_prompt(app);
      return true;
    }

    case PromptKind::InsertImage:
    case PromptKind::InsertFigure: {
      minibuf_sync_out(app);
      if (!pr.text.empty()) {
        const std::string path = xanatoo::expand_user_path(pr.text);
        edit(app);
        const bool figure = pr.kind == PromptKind::InsertFigure;
        const bool ok = figure ? static_cast<bool>(xanatoo::insert_figure(
                                     d, p.sel.focus, path))
                               : static_cast<bool>(xanatoo::insert_image(
                                     d, p.sel.focus, path));
        if (ok) {
          std::cout << (figure ? "Inserted figure: " : "Inserted image: ")
                    << path << "\n";
          p.sel.anchor = p.sel.focus;
          app.needs_layout = true;
          reset_blink(app);
          minibuf_commit_path_hist(app);
        }
      }
      close_prompt(app);
      return true;
    }

    case PromptKind::InsertInclude: {
      minibuf_sync_out(app);
      if (!pr.text.empty()) {
        edit(app);
        if (xanatoo::insert_transclude(d, p.sel.focus, pr.text)) {
          p.sel.anchor = p.sel.focus;
          app.needs_layout = true;
          reset_blink(app);
          std::cout << "Inserted include: " << pr.text << "\n";
          minibuf_commit_path_hist(app);
        }
      }
      close_prompt(app);
      return true;
    }
  }
  return false;
}

void flash_status(App& app, const std::string& msg) {
  app.status_notice = msg;
  app.status_notice_until = xanatoo::sdl::ticks() + 4000;
}

}  // namespace xanatoo::app
