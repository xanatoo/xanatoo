# Quote links: automatic transclusion in threads

Forum replies quote by copying ("> …", `<blockquote>`), never by
reference. Quote links recover the reference after the fact: search the
thread for the quoted text and link the quote to its original. The links
are derived data, computed from the document like layout; the document is
never changed.

Status: slice 1 (the matcher, `quote_links.hpp`) is implemented and tested
(`tests/quote_links_tests.cpp`). Nothing in the app uses it yet.

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

## Planned

1. Per-buffer `QuoteLink` table, rebuilt when a thread document is replaced
   or grows (HN / Reddit "more"); quotes whose origin is not loaded yet
   resolve when it arrives.
2. Jump: context menu "Go to quoted message" and a small `↑ author` label on
   the quote, jumping with a `#text:` mark on the original (PASSAGES).
   Hover shows author and the original sentence.
3. Reverse: quoted ranges underlined in the original, "quoted by N" in the
   backlinks; which sentence of a comment drew the replies.
4. Linked highlight: clicking either end marks both.
5. Split view: the source in the other pane, a curve from quote to range
   across the divider, only for the hovered / selected link; clamped with an
   arrow when one end is off screen. Exact links solid, fuzzy dashed.
6. Stretchtext: collapse a quote to one line, expand it to the quote or to
   the whole sentence / paragraph around it in the original.
7. Later: misquote diff for fuzzy links; reply structure for flat forums and
   mail archives from quote links; native references (Nostr `q` tags,
   Discourse `[quote="user, post:N"]`) feed the same table without search;
   the Reply composer's Quote button records the Locator it quoted.

## Risks

- False positives on short common phrases: the thresholds, near-first
  order, and a visible score.
- Cost: plain substring search over the thread, usually ending at the
  parent; trigram tables are built only for fuzzy fallback.
- Virtualized layout: geometry of an off-band end comes from the last known
  Y, as the Reply panel's scrollbar tick does.
