// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include "xanatoo/types.hpp"

#include <string_view>

namespace xanatoo {

// Markdown written by someone else (Reddit / Lemmy comment and post bodies)
// as blocks under `parent`. Parsed with parse_markup, then reduced to plain
// Markdown: xanatoo-only `:::` blocks are unwrapped (their text stays),
// `:::include` becomes its literal line, fixed-layout pages are dropped,
// `$…$` math is text again, node labels are cleared (they would collide
// with message ids), relative links and images resolve against `base_url`
// and `file:` targets are not followed. No front matter.
void append_foreign_markdown(Document& doc, NodeId parent, std::string_view md,
                             std::string_view base_url);

}  // namespace xanatoo
