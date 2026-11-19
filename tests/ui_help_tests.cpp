// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
//
// Offline tests for the Help panel texts (ui_help.hpp): label keys, the
// topic table's integrity and coverage of the elements the app names.
// Part of xanatoo_tests (test_ui_help is called from unit_tests.cpp).

#include "xanatoo/ui_help.hpp"

#include <cstdio>
#include <set>
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

void test_ui_help(int& passed, int& failed) {
  using namespace xanatoo;
  using V = std::vector<std::string>;

  // Label → keys: id part first (digits trimmed), then the visible text.
  EXPECT((ui_help_keys("Open\xE2\x80\xA6") == V{"open"}));
  EXPECT(ui_help_keys("Manage / edit style sheets...") ==
         V{"manage / edit style sheets"});
  EXPECT((ui_help_keys("##urlbar") == V{"urlbar"}));
  EXPECT((ui_help_keys("welcome.md###tab12") == V{"tab", "welcome.md"}));
  EXPECT((ui_help_keys("+##new-tab") == V{"new-tab", "+"}));
  EXPECT((ui_help_keys("Show##reddit") == V{"reddit", "show"}));
  EXPECT((ui_help_keys("tb_back") == V{"tb_back"}));
  EXPECT((ui_help_keys("Diff demo (v1 | v2, marks on)") ==
          V{"diff demo (v1 | v2, marks on)", "diff demo"}));
  EXPECT(find_ui_help(ui_help_keys("Diff demo (v1 | v2, marks on)")[1]));
  EXPECT(ui_help_keys("").empty());
  EXPECT_EQ(ui_help_visible_label("Client ID##reddit"), "Client ID");
  EXPECT_EQ(ui_help_normalize("  Credentials\xE2\x80\xA6 "), "credentials");

  // Table integrity: unique, normalized keys; every topic has text.
  std::set<std::string> seen;
  bool keys_ok = true, texts_ok = true, unique = true;
  for (const UiHelpTopic& t : ui_help_topics()) {
    if (!t.title || !*t.title || !t.body || !*t.body) texts_ok = false;
    std::string_view keys = t.keys;
    while (true) {
      const size_t bar = keys.find('|');
      const std::string k(keys.substr(0, bar));
      if (k.empty() || ui_help_normalize(k) != k) {
        keys_ok = false;
        std::fprintf(stderr, "ui_help: bad key '%s'\n", k.c_str());
      }
      if (!seen.insert(k).second) {
        unique = false;
        std::fprintf(stderr, "ui_help: duplicate key '%s'\n", k.c_str());
      }
      if (bar == std::string_view::npos) break;
      keys.remove_prefix(bar + 1);
    }
  }
  EXPECT(keys_ok);
  EXPECT(texts_ok);
  EXPECT(unique);

  // Every key the app derives itself must have a topic.
  for (const char* k :
       {"@panel:hover-help", "@panel:help", "@panel:outline",
        "@panel:backlinks", "@panel:sitemap", "@panel:page", "@panel:history",
        "@panel:styles", "@panel:style-manager", "@panel:bookmarks",
        "@panel:context", "@panel:subscriptions", "@panel:downloads", "@panel:media",
        "@panel:reply", "@panel:annotations", "@panel:revisions",
        "@panel:nostr-explorer", "@panel:nostr-accounts",
        "@panel:lemmy-explorer", "@panel:matrix-explorer",
        "@panel:certificates", "@panel:related-sites", "@float-title",
        "@doc-window-title", "@float-close", "@float-shade", "@float-max",
        "@float-resize", "@dock-edge", "@dock-split", "@sitemap-page",
        "@outline-focus", "@status-bar", "@tile-divider", "@tile-mode",
        "@tile-diff-track", "@scrollbar", "@document", "@document-edit",
        "@link", "@image", "@image-card", "@image-handle", "@stretch",
        "@task", "@grip", "@col-gap", "@transclude", "@bookmark-mark",
        "@quote-arrow", "@annotation-tab", "@reader-notice", "@gallery",
        "@gallery-image", "@gallery-prev", "@gallery-next", "@gallery-up",
        "@gallery-grid", "@page-bar", "@page-edge",
        // toolbar ids and menu headers
        "tb_back", "tb_fwd", "tb_up", "tb_home", "tb_stop", "tb_reload",
        "tb_bookmark", "tb_find", "tb_split", "tb_trail", "tb_outline",
        "tb_viewmenu", "tb_linespacing", "tb_measure", "tb_style",
        "tb_history", "tb_downloads", "tb_edit", "urlbar", "searchfield",
        "tab", "new-tab", "file", "edit", "format", "insert", "view",
        "layout", "panels", "go", "bookmarks", "history", "services", "help",
        "help panel"}) {
    if (!find_ui_help(k)) std::fprintf(stderr, "ui_help: no topic '%s'\n", k);
    EXPECT(find_ui_help(k) != nullptr);
  }
  EXPECT(find_ui_help("no such element") == nullptr);

  // Panel text: the topic's title and body (its text covers the tooltip).
  const UiHelpTopic* back = find_ui_help("tb_back");
  const std::string md = ui_help_markdown(back, "tb_back", "Back (Alt+Left)");
  EXPECT(md.rfind("# Back\n\n", 0) == 0);
  EXPECT(md.find(back->body) != std::string::npos);
  EXPECT(md.find("> ") == std::string::npos);
  // Without a topic: the visible label, the tooltip as a quote, a note.
  const std::string none =
      ui_help_markdown(nullptr, "Frobnicate##x", "Frobnicates the page");
  EXPECT(none.rfind("# Frobnicate\n\n> Frobnicates the page\n", 0) == 0);
  EXPECT(none.find("No description") != std::string::npos);
  // A tooltip equal to the title is not repeated.
  EXPECT(ui_help_markdown(nullptr, "Frob", "Frob").find("> ") ==
         std::string::npos);
  // Multi-line tooltips stay one quote.
  EXPECT(ui_help_markdown(nullptr, "", "a\nb").find("> a\n>\n> b\n") !=
         std::string::npos);

  passed += g_passed;
  failed += g_failed;
}
