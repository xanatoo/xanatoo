// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Web page reference marks → xanatoo footnotes. Wikipedia (Parsoid and the
// regular site), Grokipedia and many wikis write a citation as a bracketed
// same-page link ("[1]", "[a]", "[note 3]" → "#cite_note-1") into an
// ordered list of notes. After html_min those are plain Link spans and
// ListItems; this pass turns them into FootnoteRef spans and Footnote nodes
// so the footnote overlay, "#fn:key" jumps and the Markdown "[^key]" form
// work on them.

#include "xanatoo/types.hpp"

namespace xanatoo {

// A Link span whose text is "[mark]" (short, no nested brackets) and whose
// href "#id" names a ListItem of a List becomes FootnoteRef `key` (the mark
// with blanks as "-", made unique per target). Every item of a list that is
// such a target becomes a Footnote (label = key; unreferenced items take
// their position) in the list's place; its leading back links ("↑", "a b",
// "1↑") are dropped. Returns the number of footnotes made.
int html_link_footnotes(Document& doc);

}  // namespace xanatoo
