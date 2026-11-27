# Native XML format (canonical)

`.xml` is xanatoo's canonical serialization of its semantic tree: the only
writer that keeps everything a document holds, and the format of the local
document store (revstore). Markdown and Org stay as convenient, not
feature-complete, formats. `.sexp` is **read only** since 2026-11: old files
and old store revisions still open (`parse_stored_document` tells them apart
by their first character), but nothing writes S-expressions any more —
saving an opened `.sexp` file asks for another name, and its drafts are XML.

Example:

```xml
<doc title="Little experiment" lang="en">
  <section label="intro">
    <h>Introduction</h>
    <p>Rock &amp; roll: <em>text</em>, <a href="#intro">links</a>,
      <math src="x^2"/> and <img src="diagram.png" alt="a diagram"/>.</p>
    <ol>
      <li level="1">One</li>
      <li level="2">Two</li>
    </ol>
  </section>
</doc>
```

The parser does **not** implement general XML. It accepts exactly:

- One `<doc>` root and nested tags with matching end tags or `/>`.
- ASCII tag/attribute names (`A-Z`, `a-z`, digits, hyphen, underscore; a
  letter must come first), case-sensitive. No namespace prefixes or `xmlns`.
- Quoted attributes (`"value"` or `'value'`), including whitespace around `=`.
- Ordinary UTF-8 text and the five predefined entities `&amp;`, `&lt;`,
  `&gt;`, `&quot;`, `&apos;`, plus decimal/hex numeric character references
  (`&#10;`, `&#xA;`). Invalid XML characters and UTF-8 are rejected.
- Whitespace between elements in block containers is insignificant. Whitespace
  inside text-bearing tags (`p`, `h`, `li`, etc.) is content and preserved,
  except for XML's usual raw CR/LF normalization (use `&#13;` to preserve CR).
  Raw tab/CR/LF in attributes normalize to spaces (use numeric references to
  preserve them).

It deliberately **rejects** XML declarations (`<?xml…?>`), other processing
instructions, DTDs, `<!DOCTYPE>`, external/internal entities, namespace tags,
comments, CDATA, unquoted/duplicate attributes, unknown tags/attributes,
unbalanced elements, and nested inline markup. There is no network access,
entity substitution, or script execution. Malformed files produce a read-only
error view in the app instead of an editable replacement for the source.

## Semantic tag vocabulary

| Role | Block tags |
| --- | --- |
| Document and grouping | `doc`, `article`, `section`, `h`, `h1`…`h6`, `chrome`, `aside` |
| Publication and activity details | `metadata` |
| Named facts | `infobox`, `properties`, `property`, `name`, `value` |
| Paragraphs and lists | `p`, `ul`, `ol`, `li`, `quote`, `hr`, `codeblock`, `note` |
| Media and layout | `image`, `figure`, `caption`, `stretch`, `summary` (in `stretch`: shown while collapsed, instead of the rest), `columns`, `column`, `include`, `collection` |
| Definitions and references | `deflist`, `term`, `def`, `biblio`, `ref`, `footnote` |
| Tables | `table`, `tr`, `th`, `td` |
| Discussion | `thread`, `message` |

Inline content within text-bearing blocks: `em`, `strong`, `strike`, `code`,
`a`, `cite`, `fnref`, `img`, `math`, and `text` (when a text span has metadata).
An `<image>` is a block node; `<img>` is an inline object. As in XHTML 2, `<h>`
is the unranked semantic title (its rank follows from the enclosing
`<section>`s), while `<h1>`…`<h6>` are legacy ranked headings; a ranked
heading outside 1–6 is written as `<h1 level="N">`. Older Xanatoo documents
using `<heading>` or `<header>` as a title are still accepted on import and
exported as `<h>`. Before this change `<h level="2">` meant a ranked
heading; such files now read it as a title (with a depth hint). The `<header>` name is reserved for future
page-layout semantics, like `<footer>`; it is not a new layout node yet.

Use `<metadata>` for an article's byline, source, date, score, or discussion
links. It contains inline content and belongs to the enclosing composition.
Place it immediately after the title for a compact title and byline, followed
by any excerpt or body. For example:

```xml
<article>
  <h><a href="https://example.org/story">Story title</a></h>
  <metadata>By Ada · <a href="hn:item?id=42">3 comments</a></metadata>
</article>
```

The equivalent forms are `(metadata ...)` in S-expressions and
`:::metadata` in Markdown. Use `<caption>` to describe or identify media
or a table.

Use `<properties>` for a list of named facts. Each `<property>` has a
`<name>` and a `<value>` in that order, both with inline content.
An optional `<infobox>` groups the list with blocks such as a cover image
or heading. For example:

```xml
<infobox>
  <properties>
    <property><name>Creator</name><value><a href="ia:ocean">Ocean</a></value></property>
  </properties>
</infobox>
```

Markdown uses nested `:::infobox`, `:::properties`, and `:::property`
fences, each closed with `:::`. Write `:::name Creator` and
`:::value [Ocean](ia:ocean)` on separate lines inside the property fence.
S-expressions use the same names as XML.

Every block may carry `nid` (the source document's numeric node identity),
`label`, `style`, `tags`, `level`, `src`, `alt`, `summary`, `expanded` (`true`
or `false`), `float`, `scale`, `colspan`, and `rowspan`, as appropriate to its
node role. This is intentionally a direct serialization of the existing
`Node` fields, rather than an independent XML document model. Defaults can be
omitted; the exporter omits most default attributes. An `<image>` may also
carry `href`, its link target (an HTML `<a>` around a block `<img>`).
`<doc>` additionally
has `title`, `lang`, `author`, and may contain one `<front-matter>` text element
to preserve Markdown YAML metadata without losing line breaks.

Inline tags may carry `role="metadata"`, `marks`, `href`, `rel`, `datetime`, `target`,
`src`, `alt`, `width`, `height`, and `display="true"` as appropriate. `nid`
on blocks and `target` on inline spans preserve internal references even when
node IDs are reassigned during import. These IDs are not stable cross-file
anchors; `label` / `href` remain the human-facing references.

Text is only allowed in text-bearing blocks. An importer that leaves inline
text on a container node (a section, figure, …) produces a tree this format
cannot hold: the exporter leaves that text out and reports the nodes
(`serialize_xml`'s `unwritten`), and `//source-xml` shows a
warning naming them above the source, rather than failing or dropping it
silently.

Flat inline spans deliberately cannot nest (for example, `<em>one
<strong>two</strong></em>` is invalid). This mirrors the present document
model; silently flattening nested markup would lose semantics. The faces a
span inherits from enclosing emphasis are the `marks` attribute instead
(space-separated `strong`, `em`, `strike`): `<em>one </em><strong
marks="em">two</strong>`, or a link inside bold text as
`<a href="…" marks="strong">`. XML import also
doesn't attempt to read arbitrary SVG, XHTML or generic XML vocabularies.

### Social markup

Mentions, hashtags and scores are ordinary spans with a `rel` token
(`types.hpp` `kRelMention` / `kRelTag` / `kRelScore`, tested with
`rel_has`; `rel` is a space-separated list):

```xml
<p><a href="bsky:did:plc:b" rel="mention">@bob</a> likes
<a href="bsky:hashtag/xanadu" rel="tag">#xanadu</a></p>
<span role="metadata" rel="score">42 points</span>
```

- `mention`: the href is the account on its service (`bsky:DID`,
  `nostr:npub…`, `https://www.reddit.com/user/NAME`, a fediverse profile).
- `tag`: the href is that service's feed for the tag.
- `score`: a snapshot of the vote count when the page was fetched, not a
  live value; there are no voting actions.
- Readers mark only what the source marks (Bluesky facets, Nostr `nostr:`
  references, fediverse `class="mention"` / `rel="tag"` HTML, Reddit's
  `u/NAME` Markdown). Plain `@word` / `#word` text is never guessed.
- Markdown keeps the token as `[@bob](bsky:did:plc:b){rel=mention}`. The
  style sheet roles `mention` and `hashtag` colour them
  (`docs/STYLESHEETS.md`). The link context menu adds Copy Handle /
  Copy Hashtag.

## Editing and round trips

Open a `.xml` file, or use **File → Export as XML…**. Saving an
open `.xml` document writes this format. The direct `parse_xml` and
`serialize_xml` functions are also exposed in `parse.hpp`; both preserve
the semantic tree and metadata (not cosmetic whitespace). Invalid input
throws `std::invalid_argument` with a byte offset. Output is UTF-8 without an
XML declaration, to match the restricted grammar.

The writer indents block children; the reader drops whitespace-only text that
sits beside block children (that indentation), so a round trip is exact.
Text XML cannot hold (invalid UTF-8, control characters) makes
`serialize_xml` throw; the store writes with `XmlInvalid::Replace`, which
puts U+FFFD there instead, so an autosave never fails. A unit test round-trips
every file in `examples/` through XML and compares the lossless binary
encoding (`doc_codec`).

No claim of full compatibility with generic XML tools is made.
