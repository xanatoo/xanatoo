# Quote links: automatic transclusion in threads

Forum replies quote by copying ("> …", `<blockquote>`), never by
reference. Quote links recover the reference after the fact: search the
thread for the quoted text and link the quote to its original. The links
are derived data, computed from the document like layout; the document is
never changed.

Status: the matcher (`quote_links.hpp`, `tests/quote_links_tests.cpp`) and
the first UI slice (`app_quotes.cpp`: jump both ways, margin arrows,
underlined originals, linked highlight) are implemented. Offline demo:
`examples/quotes-demo.xml`.

## Matcher (`find_quote_links`)

- **Quotes**: `BlockQuote` nodes inside a `Message`, and lines starting with
  `>` in other text nodes (Reddit and Lemmy bodies are raw Markdown in one
  `Paragraph`). `>>` / `> >` give depth 2. A quote is a run of lines of the
  same depth in one node without a blank line between them.
- **Normalization** (`quote_normalize`): words lower-cased (ASCII,
  Latin-1, Greek, Cyrillic), everything between words (punctuation, smart
  quotes, dashes, line and paragraph breaks) one space. Matches start and
  end at word edges.
- **Pieces**: `...`, `…` and `[...]` split a quote into pieces that must
  match in order inside one message; the source range spans the gap.
- **Candidates**, in order: the reply chain (`alt` = parent label, else the
  enclosing `Message`), the thread's first message (story text, selftext),
  then every other message by distance, earlier first. Ancestors and the head
  are *near*: a quote of 3 words / 12 characters is enough there; elsewhere
  it takes 5 words / 24 characters.
- **Own text only**: candidates are searched without their own quotes, so a
  quote of a quote links to the origin, not to the copy. Only when no
  message has it as own text does a near candidate's quote count
  (`via_quote`: the origin is outside the thread).
- **Fuzzy**: when no exact match exists and the quote has 6+ words, share of
  its word trigrams found in the best window of a candidate (window = 1.5×
  the quote's length); accepted from 0.6, reported below 1 (`score`).

## In the app (`app_quotes.cpp`)

- `quote_links_of(DocBuffer&)`: the links of a buffer, cached in
  `DocBuffer::quotes` and rebuilt when `DocBuffer::doc_gen` (bumped by
  `note_edit` and `refresh_views_of_buf`) or the node count changes. Only
  documents with `Thread` / `Message` nodes are searched. A thread that
  grows (HN / Reddit "more") is a replaced document, so late origins resolve
  then.
- A linked quote has an up arrow at the right edge of its first line
  (`quote_icons`, hit-tested by `quote_icon_at` for click, hover tooltip and
  hand cursor): solid for exact matches, an outline for approximate ones.
  Click jumps to the original.
- Quoted text in the original has a dotted underline in the `mark` colour.
- Context menu: inside a quote "Go to Quoted Message (author)", inside
  quoted text "Go to Reply Quoting This (author)" (up to 8).
- A jump is `navigate_pane` to the page address with a `#text:` point
  (`text_selector`, unique in the document), so Back returns and the target
  gets the usual `#` mark; the other end of the link is washed as well
  (`App::quote_focus`) while the mark stays.

## Planned

1. Hover on the arrow could show the original sentence; "quoted by N" in
   the Backlinks panel.
2. Split view: the source in the other pane, a curve from quote to range
   across the divider, only for the hovered / selected link; clamped with an
   arrow when one end is off screen. Exact links solid, fuzzy dashed.
3. Stretchtext: collapse a quote to one line, expand it to the quote or to
   the whole sentence / paragraph around it in the original.
4. Later: misquote diff for fuzzy links; reply structure for flat forums and
   mail archives from quote links; native references (Nostr `q` tags,
   Discourse `[quote="user, post:N"]`) feed the same table without search;
   the Reply composer's Quote button records the Locator it quoted.

## Risks

- False positives on short common phrases: the thresholds, near-first
  order, and a visible score.
- Cost: an inverted word / trigram index (sorted flat postings) picks the
  candidate messages; only the reply chain and head are scanned in full.
  Positions are rebuilt only for matched messages. 3000 messages: ~70 ms
  (-O2), ~110 ms (-Og), ~33 MB peak beyond the document. Runs once per
  document generation (cached in `DocBuffer::quotes`).
- Virtualized layout: geometry of an off-band end comes from the last known
  Y, as the Reply panel's scrollbar tick does.
