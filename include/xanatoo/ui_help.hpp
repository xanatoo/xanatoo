// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// Help panel texts (docs/UI.md "Help panel"): one explanation per GUI
// element, found by a lookup key. Pure data + string handling, no SDL.
//
// Keys are lower case. ImGui widgets are found by their label
// (`ui_help_keys`: the "##id" part first, then the visible text); canvas
// chrome drawn natively uses "@" keys (`@float-close`, `@status-bar`, …,
// panels `@panel:<session name>`).

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace xanatoo {

struct UiHelpTopic {
  const char* keys;   // '|'-separated lookup keys, lower case
  const char* title;  // heading in the panel
  const char* body;   // Markdown
};

// Every topic, in table order.
const std::vector<UiHelpTopic>& ui_help_topics();

// Lower case, trimmed, without a trailing "…" / "..." / ":".
std::string ui_help_normalize(std::string_view s);

// Lookup keys of an ImGui label, best first: the id after the last "##"
// (trailing digits dropped: "name###tab12" → "tab"), then the visible text
// before the first "##". Empty parts are left out.
std::vector<std::string> ui_help_keys(std::string_view label);

// The topic of a normalized key, nullptr when none.
const UiHelpTopic* find_ui_help(std::string_view key);

// The visible text of an ImGui label ("Open…##x" → "Open…").
std::string ui_help_visible_label(std::string_view label);

// Panel text: the topic as a Markdown document; without one, the
// element's label and its tooltip `tip` (when it adds to the label).
std::string ui_help_markdown(const UiHelpTopic* topic, std::string_view label,
                             std::string_view tip);

}  // namespace xanatoo
