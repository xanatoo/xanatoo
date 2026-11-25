# Content model, layout and formats

Developer notes split out of `AGENTS.md`: document structure, layout,
HTML mapping, tables, images, collections, imports and style sheet details.

## Style sheets (details)

- Paint order: `draw_backdrop` (paper), then `Renderer::draw` with the
  pane's `SelectionWash`: role fills (cards, asides, tables, code), bars and
  rules, inline-code runs, *then the selection*, then glyphs. Sheets may use
  opaque fills; the selection is never passed to `draw_selection` before
  `draw`. Marks / bookmarks / quote washes are overlays after the text.
- Block quote bars span the laid-out text lines. The role's `space-after`
  still advances the next block below the bar.
- Shipped sheets keep text ≥ 4.5:1 on its background (metadata, meta,
  footnotes included — dark sheets must override the default's dark
  footnote / strike colours), a selection that differs from `mark`, every
  fill and the thread bar, and distinct author / thread-starter / meta,
  link / cite colours (sheets that underline links or show code only by
  its font are the deliberate exceptions: Hacker News, Typewriter).

- Sheets may name faces with poor coverage (Verdana has no arrows):
  `Font::glyph` draws codepoints the face lacks from fallback faces
  (`fallback_font_faces`: FontConfig coverage sort, fixed list on Android);
  line metrics stay the primary face's.
- Colour emoji: fallback faces may be colour fonts. Fixed-strike CBDT /
  sbix faces (Noto Color Emoji) select the nearest strike (`size_face`) and
  are scaled to the pixel size (`resample_premultiplied`); COLR outlines
  render to BGRA through FreeType. Such glyphs carry `Glyph::color` and the
  renderer draws them untinted (only alpha). OpenType-SVG-only faces
  (Twemoji SVGinOT) have no renderer here: their monochrome outlines are
  used, and only when no other face covers the codepoint.
- No face has it: `missing_glyph_bitmap` draws a hex box (4 digits, 6
  past U+FFFF) tinted like text; default-ignorables (`is_default_ignorable`:
  variation selectors, joiners, bidi marks, tags) take no space. No
  shaping yet, so ZWJ sequences and skin-tone modifiers show as their
  parts, and VS16 does not force emoji presentation (❤️ stays the body
  face's ❤ when it has one).
- Font lookups return a `FontFace` (file + collection index); `Font::load`
  opens that member, so a `.ttc` / `.otc` match is the requested family,
  not face 0. FontConfig supplies `FC_INDEX`; without FontConfig
  (Android) `scanned_faces` lists every member of each file once at first
  lookup (cached), with style from flags, then style name, then the file
  name (single-face files only). Pass `face()`, not `path()`, when
  reopening a loaded font.
- Glyph rasterisation (`Font::load_glyph`): light hinting (vertical only,
  no whole-pixel stroke snapping), unhinted fractional advances
  (`Glyph::advance_px`; layout keeps float pens), bitmaps at
  `kSubpixelSteps` horizontal phases, and coverage curves for dark vs
  light text (`kTextGamma`, paired with FreeType stem darkening set on
  the library in `Font::load`). Metrics come from `Font::glyph`; drawing goes
  through `Font::raster(cp, pen_x, light_text, &x)`, which takes the pen
  (bitmap left minus `bearing_x`), not the bitmap edge.
- `ViewSettings::style` (view.conf) is the global *fallback* sheet; the toolbar
  and View menu issue `UiCommand::SelectStyle` / `ReloadStyles`. The dockable
  Styles panel uses an embedded ImGui editor and `style.cpp` helpers to create,
  validate, atomically save and delete user-owned `.xss` sources. Shipped
  presets remain read-only; users duplicate them to customize.
- Domain/service pins persist separately in `style-pins.conf`, with exact
  domain keys checked before service keys. `sync_pinned_style` changes only
  the resolved active look, never the saved global fallback. Do not write the
  active pinned sheet into `view.conf` on navigation.
- Colour / font mixing: `StyleChoice{sheet, colors, fonts}` and
  `load_style(choice, …)`. Each lending sheet is resolved fully (built-in +
  inherit chain) and `copy_style_group` copies its `StyleGroup` fields
  (`kStyleColors` / `kStyleFonts` / `kStyleLayout`) — every `DocStyle`,
  `BlockStyle` and `InlineStyle` field belongs to exactly one group there;
  a `static_assert` in `test_style_choice_mix` counts the role structs'
  members, so a new field must be added to `copy_style_group`. Variants
  are lists of `StyleVariant` parts with a `groups` mask: the sheet's own
  parts lose the lent groups, the lender's parts are appended with only
  those. `DocStyle::colors_from` / `fonts_from` record the request (also
  when that sheet is missing, so `sync_pinned_style` does not retry each
  frame). `ViewSettings::style_colors` / `style_fonts` are global and sit
  on top of pinned sheets; `apply_style_layer` / `UiCommand::
  SelectStyleColors` / `SelectStyleFonts` set them.
- Visited links are coloured at draw time, not by layout:
  `update_visited_links` fills `Pane::visited_links.glyphs` (one flag per
  layout glyph; keyed on layout generation, buffer and `App::visited_gen`,
  answers cached per href). A link is visited when its target page
  (`resolve_link` → `page_address` of the base) is in `App::visited` or
  `page_visited` (visit log). `Renderer::draw` takes the flags and uses
  `DocStyle::visited_link_color` (`visited-link` role; without a colour,
  or for links not drawn in the link colour, the colour fades towards the
  paper). `PaneRaster::visited_version` redraws the cached pane image.
  `history_changed` / `note_visited_address` / removing a visit bump
  `visited_gen`.

## Layout virtualization

- `LayoutEngine::layout` accepts optional `LayoutViewport` + height-hint map.
- Sequential containers (Document, Section, Article, Thread, List, Column,
  DefList, Biblio) skip off-screen children when a prior box height is known.
- Main keeps per-pane `height_cache`
- `LayoutResult::glyphs_by_y` (built by `index_glyphs` at the end of
  `layout` and `paginate`) orders glyphs by baseline; pointer hit tests
  (`hit_test`, `hit_test_link_ex`, `hit_test_timestamp`) ask
  `glyphs_in_band` for the lines near the pointer instead of scanning every
  glyph. Code that moves glyphs afterwards must call `index_glyphs` again
  (a size mismatch falls back to a full scan).
- Per-frame chrome must not walk the whole document: scrollbar section /
  block ticks (`Pane::scroll_marks`) and the status-bar word counts
  (`Pane::stats`) are cached per `layout.generation`; the page-navigation
  button reads `Pane::nav_parts` (`page_nav_regions`, counted in
  `layout_one`). `Renderer::draw` draws only glyphs in the clip band.
- Line-level virtualization (`layout_paragraph`): with a viewport, lines of
  a block outside the materialised band are broken and measured but emit
  no glyphs; `LayoutResult::skipped_lines` keeps each such line's start
  and baseline, and `caret_screen_pos` falls back to it, so caret / search
  / scroll targets inside a partly materialised block stay exact.
- `LayoutEngine` line tables: a plain-text block (no math, inline images,
  footnote marks, nor floats beside it) that had skipped lines keeps its
  line starts (+ byte offsets, auto-link ranges), keyed by document and
  node and checked by a fingerprint of text, fonts, width and spacing. The
  next layout decodes and breaks only from the last line above the band to
  the first below it (10 MB block: ~3 ms instead of ~1 s). Few tables
  (`kMaxLineTables`, LRU).
- Preferences (`ViewSettings` / `view.conf`): line spacing, margins, measure, plus behaviour switches (footnote overlay, justify, hover preview) shown in Edit → Preferences (ImGui window, `UiCommand::TogglePreferences`); never document format
- Section Focus mode: `LayoutEngine::layout(..., focus_section)` filters body to one Section/Article (outline still full tree); scroll outside the materialised band
  triggers re-layout. Width changes clear the cache.
- History still snapshots whole documents (depth capped for large trees).

## Section tree (structure)

- Org stars and Markdown ATX/setext always normalize into nested `Section`
  nodes with a `Header` title child. Body blocks are children of the section.
- `Article` is never synthesised automatically.
- Soft interop: `Section.level` / `Header.level` may still hold depth for export.
- View fold state will live outside the document (future SQLite); do not rely
  on dirtying `.md`/`.org` for collapse alone.
- Structure highlight shows `headerN` for header depth.
- Folding (`fold.hpp`, pure): list items and stretchtext. Every ListItem
  hosting a non-empty List folds. State is `DocBuffer::folds` (node →
  folded) over the
  markup default `Node::expanded` (`- [+] text` above a nested list,
  `(li :expanded #f …)`, XML `expanded="false"`); it is cleared when the
  buffer gets a new document (`refresh_views_of_buf` with `top`), never
  dirties it and is not saved. Pane layouts set it on the engine
  (`FoldScope`); help / context / hover layouts use the markup only.
  Bullets become ▾ / ▸, numbered and task items get the triangle before
  the text (`ListSlot::fold_column`); a folded item ends in "… N items".
  Click areas: `LayoutResult::fold_marks` / `hit_test_fold`. Jumps,
  search and Tab-indent call `reveal_folds`.
- Stretchtext (`NodeKind::Stretch`) uses the same map. Classic: the label
  (`Node::summary`) stays above the children. Two-body: a `Summary` child
  (`:::summary`, `(summary …)`, `<summary>`; `stretch_summary`) is shown
  while folded and the other children replace it. Cues: "▸ Full Text" at
  the end of the summary's last paragraph (`FoldCue`), "▴ Summary" above
  and below the full text; they are decoration (no node, `fold_marks` only).
  While reading, toggling is view state; in edit mode it changes
  `expanded` like any edit (`apply_folds`).
- Hidden halves: `fold_hiding` / `reveal_folds` open whatever hides a
  target; find skips only the summary of a text already shown
  (`find_skips`), copying and the selection count skip whatever is
  hidden (`copied_text`); the page's word count has both bodies. Page-wide: `set_stretch_folds` (View → Summaries).
- View-local fold: `folded_sections` set passed into layout; Section/
  Article show header only when folded. Outline twisties / ←→ / Ctrl+Shift+F.

## Inline marks (nested emphasis)

- Spans stay flat: one `InlineKind` each. Faces from enclosing markup are
  `InlineSpan::marks` (`kMarkStrong` / `kMarkEmphasis` / `kMarkStrike`,
  `types.hpp`): `**a *[b](u)***` is Strong "a " + Link "b" marked
  strong|emphasis. `span_marks()` adds the kind's own face, `add_mark()`
  wraps a span (plain text takes the kind, anything else the mark).
- Markdown: `parse_inlines` recurses into `**` / `*` / `_` / `~~` content
  and styled link text (`find_star_closer` pairs `***` runs like
  CommonMark; a nested run needs flanking non-space, so `2 * 3` is text).
  `[*T* x](u)` becomes one Link span per styled run, same href.
- Writers group runs of shared marks (`emit_marked_spans`): Markdown
  delimiters, S-expression `(strong "a " (em (a "u" "b")))`, XML a
  `marks="strong em"` attribute. `html_min` sets marks from open
  `<strong>` / `<em>` / `<s>`.
- Layout: `style_role(base, kind, marks)` picks the face; `PositionedGlyph`
  carries `marks` so render strikes through a marked link.

## HTML fragments (`html_min.hpp`)

- `html_append_blocks`: tag soup → Paragraph / Heading / List / ListItem /
  BlockQuote / CodeBlock / Table / HorizontalRule with Link / Emphasis /
  Strong / Code / Strike spans; script / style / iframe dropped, unknown
  wrappers unwrapped. BlockQuote, ListItem and TableCell are leaves, so
  nested blocks inside them flatten to line breaks / "• " lines.
- `HtmlOptions::images` (web pages, feeds): block-level `<img>` → Image
  node (remote ones load like any remote image; `html_img_src` prefers the
  `srcset` / `data-srcset` pick of `html_srcset_pick` — largest width up to
  1600w, else up to 2x — over `src`, then lazy `data-src`); icons (declared edge ≤ 48) and images inside
  quotes / list items / cells → inline image spans (`data:` ones stay
  `[image: alt]` links).
- `HtmlOptions::asides` (the main content of web pages): `<aside>` → Aside
  box; elsewhere, or with chrome classes, it is page chrome.
- `chrome_nodes` (with `drop_chrome`): chrome blocks become Chrome nodes
  (`html_append_chrome`); inside running text, list items or cells they are
  dropped. EPUB chapters pass `keep_chrome = false` to `parse_html_page`.
- Used by feeds and Hacker News (`gt_quotes` for HN's "> " convention)
  and web pages (page-mode options below); also `html_to_plain`,
  `html_attr`, `resolve_url`, `html_element_end`.

## Web pages (`html_page.hpp`)

- `src/html_page.cpp`, gated by `XANATOO_WEB` (default ON). Any http(s)
  URL that no other service claims and that `is_web_url` accepts (not an
  obvious download) opens in-app; it is checked last in dispatch.
- `parse_html_page`: main content (`<main>`, lone `<article>`, `<body>`)
  through `html_min` page mode (`sections`, `anchors`, `drop_chrome`,
  `chrome_nodes`); also used for local `.html` files (`parse_document`
  dispatch). HTML is read-only: Save asks for a `.md` name.
  HTML subscription checks use the same conversion with `keep_chrome = false`
  and fingerprint the canonical XML.
- Article / section boxes (page mode, `sections`): `<article>` is always an
  Article node and `<section>` a Section when it has an id or starts with a
  heading; other sections are transparent. The box's first heading becomes
  its Header (the box takes the heading's level instead of opening a nested
  Section); headings never close an element's box, its end tag does
  (`Builder::open_box_tag` / `close_box_tag`, `Wrap` records the open
  elements). Unlabelled boxes that stay empty are dropped (`end_box`), so
  `<body><article>` split across chunks leaves nothing behind. The context
  menu's Open Article / Section / Block verbs cut a view to these boxes
  (docs/PASSAGES.md "Opening a block").
- Page chrome (nav / aside / footer / form / dialog, landmark roles, navbox /
  share / cookie classes, what lies around `<main>`) is kept as
  `NodeKind::Chrome` nodes (`summary` = what it is); hidden / sr-only /
  `opacity:0.00…` text is always dropped (`is_hidden`), except React
  streaming-SSR segments `<div hidden id="S:n">` (Next.js sites such as
  lesswrong.com), which hold the whole page. Reader mode is view state (`Pane::reader`, `pane_reader` /
  `set_reader`, default `ViewSettings::reader_mode`): layout leaves Chrome out
  (`set_hide_chrome`) and draws the "… hidden · Show all" notice
  (`LayoutResult::chrome_notice`). Never put reader mode into the address or
  parse a page twice for it (docs/PASSAGES.md "Reader mode").
- Site hints (`site_hints.hpp`, `sites/*.xsite`, docs/SITEHINTS.md): for
  remote pages `parse_html_page` looks up `site_hints_for_host(host)`
  (data_dir() sheets, then user_data_dir(); reread on mtime change). With
  hints, `bind_site_hints` builds a tolerant element tree (`html_elements`,
  implied `</li>` / `</p>`) and matches the selectors once; `(main …)`
  elements become the content parts (page order, everything between them
  Chrome "page"), and `attach_page_hints` sets `HtmlOptions::hint` /
  `hint_replace`, which `html_append_blocks` asks at each start tag (Drop,
  Chrome with role, Content = no chrome heuristics, Replace). `(thread …)`
  rebuilds a comment tree as Thread / flat Message nodes (`alt` = parent
  label; empty messages are skipped, their replies move up). Hints are a
  page-structure layer, not styling: they never touch `.xss` sheets.
- Related content: `is_related_block` (html_min.hpp) gives a div / section
  / aside / nav / list the chrome role "related" when its class / id
  (`kRelatedParts`, not WordPress `tag-*` / `category-*`) or its first
  heading (`kRelatedHeadings`, "You may also like", "More from …") say so
  and `html_link_share` of its text is at least 0.5; it also overrides
  "navigation" / "sidebar". `margin_chrome` takes it, so the Page panel
  lists it as "Related". Chrome is a native block in Markdown
  (`:::chrome role="…"`), `.sexp` (`(chrome :role "…")`) and XML; Org and
  gemtext flatten it.
- Native site bars: the forum clients' "Site: a · b" line
  (`forum::start_document`) and Hacker News' "Top · New · …" line
  (`append_nav`) are Chrome "navigation" regions, so they move into the Page
  panel like a web page's `<nav>`. The panel's `collect` keeps the bold
  spans of a link line: "Site:" becomes a group row, the bold current entry
  a plain row.
- Reference marks → footnotes (`html_footnotes.hpp`, last step of
  `parse_html_page`): a link whose text is `[mark]` and whose `#id` names a
  ListItem of a List becomes `FootnoteRef` (key = mark, blanks → `-`); every
  item of such a list becomes a `Footnote` in the list's place, leading back
  links (`↑`, `a b`, `1↑`) dropped, other `#id` links rewritten to `#fn:key`.
  Covers Wikipedia (Parsoid `cite_note-…` and the regular site), Grokipedia
  (`#ref-N`, host appended after the title link gets a ` · `) and wikis
  alike; unbracketed links to list items stay links.
- `web_fetch` dispatches on Content-Type; bodies go through
  `detect_charset` / `to_utf8` (util.hpp) first.
- `external:<url>` hrefs force the system browser (`open_external_url`).
- There is no edit mode: non-foreign documents are editable, each with an
  edit strip on its page and a per-buffer lock (docs/UI.md "Editable
  documents"). Foreign content is read-only: `is_foreign_path` (remote URL,
  `.html`, any `is_pandoc_path` file, anything under `cache_dir()`) makes
  `pane_read_only` and so `browsing(app)` true; a locked document
  (`pane_locked`) is browsing too. Gate document edits on `browsing(app)`.
  The lock is disabled for read-only content.
- Local files: `load_doc` (app_buffers.cpp) is the one reader. It sniffs
  the first 8 KiB with `looks_like_binary` (util.hpp: a NUL byte, or over
  10 % control bytes) and shows a short "binary data" page instead of the
  bytes; such paths go into `App::binary_paths` and are read-only like
  `invalid_xml_paths`. A 35 MB MP4 read as text took 2+ GB, kept growing,
  and starved the event loop (SIGTERM went unanswered).
- Plain text: `parse_document` sends `.txt` (`path_uses_plain_text`,
  checked before the S-expression sniff) to `plain_text_document` (the
  web `text/plain` reader: blank-line chunks → CodeBlocks), and
  `serialize_document` writes it back with `serialize_plain_text` (each
  block's text, blank-line separated), so drafts and exports of a `.txt`
  stay plain text. Gopher / Spartan / Nex text items ("remote.txt") get
  the same. Runs of blank lines and whitespace-only lines collapse to one
  blank line on the round trip.

## Tables

- `TableCell::col_span` / `row_span` are occupied column and row counts;
  `build_table_grid` (`table_grid.hpp`) derives slots and placements without
  duplicating cells. Spanning tables are laid out table-wide; borders stop at
  merged cells and a complete unspanned row supplies column weights.
- HTML `rowspan=0` resolves at its row-group boundary. Markdown uses
  `:::cell` / `:::th` with `colspan=` / `rowspan=`; S-expressions use
  `:colspan` / `:rowspan`. Enter inside a cell extends spans crossing the
  inserted row. Details: `docs/TABLES.md`.
- Layout: `plan_table` measures every cell (`measure_cell`: max-content at
  the available width, min-content at 1px, text-only cells trimmed to their
  ink) into a `TablePlan`; `distribute_table_widths` picks column widths
  (content-fit unless `Node::width` is a percent); `place_table` positions
  cells with their resolved `CellAlign` / `CellVAlign`
  (`table_cell_align` / `table_cell_valign`: cell → row → table `cols` /
  `valign`). Decimal columns keep per-column widths before / after the
  point (`TablePlan::dec_b`). Head rows (`TableRow::level` 1) and foot rows
  (2) get a 2× rule (`table_head_rows`).
- Column-drag weights live in the weight row's cells' `display_scale`;
  `seed_table_weights` turns the shown widths into weights at drag start.
  Idle table column handles are not drawn (the table rules show them).
- HTML (`html_min.cpp`): `html_cell_align` / `html_cell_valign` read CSS
  first (`css_decl`, last declaration wins), then attributes; row groups
  pass theirs on to their rows; `<col>` fills `cols`.

## Inline images and asides

- `InlineKind::Image` spans (`inline_image()`): `text` is U+FFFC so the
  caret sees one character; `src` / `alt` / `width` / `height` on the span,
  `href` = optional link. Layout places them as atoms on the baseline
  (`PositionedImage::in_text`; never resizable, a click follows the link or
  opens the viewer). Markdown `![a](s){width=… height=…}` inside text.
- Image addresses in text (`ViewSettings::auto_images`, View → "Show
  images of image addresses", on by default): `text_image_urls` finds bare
  http(s) URLs of image files in plain text and links whose text is a web
  address (`<https://…/x.jpg>`, Lemmy / HN style); `layout_text_images`
  puts their remote thumbnails in a row under the paragraph, display-time
  only (the document is unchanged; such a block takes no line table).
  `PositionedImage::in_text` + `auto_link` (target in
  `LayoutResult::auto_links`, or the link span); a failed load drops the
  picture, the address stays a link.
- `NodeKind::Aside`: a container of blocks, a side box (style role
  `aside`: `width`, `side`, `placement`, `indent` = padding, small `font`,
  muted `color`). `placement margin` (default) uses existing page margin
  beside its flow position (`LayoutEngine::margins_`, set up per layout()
  by `setup_margins`, which never narrows the text column;
  `layout_margin_box`); without margin room it floats inside the column
  if readable, otherwise it uses a full-width flow block.
  `placement float` is the HTML-compatible float: floats live in
  `LayoutEngine::floats_` for one layout() call, text lines take the band
  `float_band` leaves free, boxed blocks (`clears_floats`) start below.
  Message cards and Articles hold their floats (`contain_floats`, like
  CSS `display: flow-root`): they grow to the picture's bottom and the
  float ends with them; Sections do not (an infobox floats on past them).
  Never let a block that draws a full-width fill run under a float.
- Side box text uses `LayoutEngine::small_` (`FontRole::Small*`,
  `small_role`) and `ctx_role_` (the box role's text colour); never
  hard-code a smaller size or colour for margin content.
- No general box model: new floating things (figures, `align=right`
  tables) go through the same `FloatBox` list, not per-kind geometry.

## Block images and figures

- A block Image outside a figure sits centred in a column of ~60% of the
  page (at least 16 em). Images are never scaled up: a small one keeps its
  own size (and its interactive `display_scale`). The alt text is not laid
  out under the image; the loading placeholder shows it, and hovering a
  block image shows it as the tooltip (`image_alt_tooltip`, app_frame.cpp).
- A non-floating Figure with an image child is a column as wide as its
  image (at least 14 em for the caption, at most the 60% column), centred.
  The figure role's `align` (default `center`) places the image and the
  caption lines in that column (`figure_centered`, `center_lines_` in
  `layout_paragraph`); `(align start)` puts both flush left. Floating
  figures and other captions are not affected.
- Consecutive such sibling figures — and plain block images, which follow
  the same rules without the caption measure — share a row
  (`figure_row` / `layout_figure_row`) when their
  columns fit at ≥ 75% of their own width; a shared row gives all images
  one height (never above any image's own). Stateless: the row's first
  figure lays out the whole row, the others emit nothing, and
  `layout_children_sequential` never skips them on their own (every
  figure's box spans the row; the first one carries the height hint).

## Collections

- `NodeKind::Collection` (`:::collection view=grid|list|full` or pandoc
  `::: collection`; `.sexp` `(collection :view "grid" …)`): ordered Image /
  Figure items rendered as cards. `summary` = default view hint only.
- Layout / sort / card size are view state (`App::collection_views`,
  `CollectionView`, passed via `LayoutEngine::set_collection_views`), never
  written to the document. `collection_items` gives items in view order.
- Card images are `PositionedImage` with `thumb > 0` (+ `group`): drawn
  from `ImageCache::thumb` (box-filtered, cached as BMP under
  `cache_dir()/thumbs/`, keyed by path + size + mtime); no resize handle.
- `app_collection.cpp`: Collection menus (`UiCommand::CollectionView` +
  arg, context menu and View menu), click on a card → the collection's
  `//gallery//item:N` in the same view (`open_gallery_item`, app_gallery.cpp).

## Site maps (`sitemap.hpp`)

- `src/sitemap.cpp`: XML sitemap scanner (`parse_sitemap_xml`, urlset +
  sitemapindex), `robots_sitemaps`, `build_site_tree` (flat URLs → path
  tree in pre-order, page-less one-child chains folded to "a/b") and
  `site_tree_document`; parsing / tree are always built and unit-tested,
  the fetch (`sitemap_fetch_tree`: robots.txt, else /sitemap.xml,
  /sitemap_index.xml, /wp-sitemap.xml; ≤ 25 files, ≤ 50k URLs; `.gz`
  through `inflate.hpp`) needs `XANATOO_WEB`.
- `sitemap:host` is a read-only page (one Section per directory, so the
  outline is the site structure). The panel is a tool view of the focused
  pane, not a document; its keys run before the browse-mode key filter.
- Shown pages land in `SitemapPanel::seen` (`note_page_seen`, called by
  `show_doc_in_pane`): titles and per-node coverage (`SiteCache::
  seen_count`, recomputed on `seen_gen`). `sitemap_step` = Alt+PgUp/PgDn.
- URL-bar Site map button (`sitemap_button_shown`): only for hosts known
  to have a sitemap, or while the panel is open. `update_sitemap` starts a
  detached `sitemap_probe(host)` once per host per session (robots.txt
  `Sitemap:` lines, else the well-known paths; only the first 4 KiB of a
  candidate are read, `looks_like_sitemap`). Results go to
  `SitemapPanel::probes` and `cache_dir()/sitemap-probes` ("host yes|no
  unix-seconds"; yes kept 30 days, no 7 days, network failures not stored);
  a finished full read updates the record too. Menu / Ctrl+Alt+M still
  open the panel on any site.

## Page links (`page_links.hpp`)

- `page_links(html, url)`: what a web page declares in `<head>` (`<base
  href>` respected): icons (`rel=icon`, `apple-touch-icon`), relations
  (up / first / prev / next / last / contents·toc / index / glossary /
  help / search / author / license·copyright, normalised, first wins,
  `page_relation_order`), OpenSearch descriptions, `canonical`,
  `hreflang` alternates and `<html lang>`. From the body only
  `<a rel=next|prev|first|last|up|license>` (pagination), not when the
  head already has that relation. Pure, unit-tested.
- `focused_page_info(App&)` (app_sitemap.cpp) reads it once per buffer /
  path / raw-source size into `App::page_info` (`gen` bumps on change),
  together with the feeds (`feed_links`); only http(s) pages with a raw
  source that is not itself a feed.
- Icons: `icon_candidates` ranks PNG / raster near 32 px, SVG, touch
  icons, ICO last; stored per host in `App::site_icons`, which
  `address_icon` (app_frame.cpp) tries before `/favicon.ico` (a candidate
  still loading waits, a failed one is skipped). Session only.
- Relations: a "Page links" group at the top of the Page panel
  (app_page.cpp), plus "Canonical address" when it differs from the URL;
  the panel button shows for pages that only have relations, too.
- Canonical: `safe_canonical` accepts it only for the very same page
  (host modulo www. / m. / amp., same path modulo trailing slash and
  `/amp`, may drop the query, never adds one). `focused_canonical` is
  used by `bookmark_for_view` / `BookmarkPage` (stored address) and for
  "is this page bookmarked" / Remove Bookmark. Visits keep the real URL.
- Languages: `hreflang` links give the URL-bar flag menu
  (`UiState::page_languages`, label = the link's `title`, else
  `language_name`; `language_english_name` when the menu font lacks the
  script, `ui_can_draw`). The same menu and Preferences edit
  `ViewSettings::languages` (view.conf `languages=de,en`), which
  `apply_preferred_languages` turns into the Accept-Language header
  (`set_accept_language`, https.cpp; default `en-US,en;q=0.9`; the wasm
  build leaves it to the browser).
- OpenSearch: see docs/SEARCH.md.

## Pandoc import (`epub_import.hpp`)

- `kPandocFormats` maps extensions to pandoc readers (epub, docx, odt, rst,
  tex, typ, wiki, ipynb, csv, …); `is_pandoc_path` / `import_via_pandoc`
  convert to cached GFM Markdown with `--extract-media`. The original file
  is never touched and the import is read-only (foreign).
- Location: a converted page's address is always its origin
  (`file:///…/book.epub`, the book URL), never the cache file, so Reload,
  history, bookmarks and sessions go back to the source. The cached Markdown
  is `DocBuffer::content_path` (`derived_content_path` / `import_markdown_path`
  fill it for local pandoc paths); `content_file` / `base_of` resolve images
  and media against it. Never show or store `content_path`.
- Without pandoc (Android), `.epub` goes through `epub_to_markdown`
  (`src/epub_native.cpp`: zip + OPF spine → `parse_html_page` → Markdown).
- Pandoc misbehaviour and our workarounds (Lua filter, post-passes) are
  collected in `docs/PANDOC.md`; add new findings there.

## Remote images

- An Image whose `src` has `has_image_fetch_scheme` (http(s), gemini,
  spartan, gopher, nex) is remote: `ImageCache` never fetches,
  it queues the URL (`take_requests`) and returns a `pending` placeholder;
  `poll_images` (app_images.cpp) downloads it on 4 workers
  (`https_get_follow_pooled`: kept-alive connections, or the small-net
  client in `fetch_smallnet_image`), stores it with `store_remote_image`
  and calls `remote_done` (with the failure's reason).
- `Image::error` says why a picture is not ok: "file not found", the
  decoder's reason (`decode_error`), or the download's (HTTP status,
  connect error, "not an image (text/html)"). The `//gallery` viewer and
  the Context panel show it centred under "Cannot load image" and the
  address (`draw_image_wait`, app_gallery.cpp).
- Queue order: document order (layout asks for every image of a page), but
  waiting images that are drawn (`draw_image_loading`, the `//gallery`
  viewer) call `want_image_now`, and the next `poll_images` moves them to
  the front, so a view far down a long thread fills first. Arrivals share
  one relayout at most every 150 ms (`kImageRelayoutMs`).
- Small-net pages that are pictures: `show_fetched_image` (app_fetch.cpp)
  sniffs the bytes with `looks_like_image` (Nex answers, Gopher binary items
  such as `I` / `g`, Gemini / Spartan answers with a non-text MIME type),
  stores them in the remote image cache and shows `image_url_document`, so
  extension-less image URLs work too. An explicit Gopher download still
  saves the file.
- Loading feedback: each image worker runs under its own `TransferMeter`
  (`ImageFetchShared::active`, read with `image_load_state`).
  `draw_image_loading` paints label + progress bar (sweep when the size is
  unknown, dim while queued; "Preparing" for thumtoo thumbnails, "Cannot
  load" on failure) over visible placeholders every frame, on top of the
  pane raster; tiny inline images get a 2 px bar only. The `//gallery`
  viewer shows the same bar and byte count (`draw_image_progress_bar`).
- Layout draws block remote images from their `kRemoteThumbEdge` (1024)
  thumbnail (`layout_remote_image`), images in running text from
  `kInlineThumbEdge` (512); a click shows it in the page's `//gallery`.
- Links to images (`is_image_url`: a `has_image_fetch_scheme` URL or local
  path by extension)
  show that image in the page's `//gallery` (`open_link_image`, checked
  before split / external handling), never the system browser; one not
  among the page's images opens as `URL//gallery//item:1`
  (`is_image_address` / `image_url_document`, `gallery.hpp`).
- An http(s) name is only a guess ("/wiki/File:X.jpg" is a web page): the
  answer's Content-Type decides. **URL cache** (`url_cache.hpp`): every
  `web_fetch` answer and remote image download is kept under
  `cache_dir()` — bytes in `urls/<FNV hash>` (also `remote_image_path`),
  metadata (final URL, status, Content-Type, ETag, Last-Modified, time,
  size) in `urls.sqlite`, keyed by requested and final URL.
  `url_cache_kind` → Image / Other / Unknown (non-200 answers: Unknown).
  Each body is also hard-linked into the object store
  (`object_store.hpp`, `cache_dir()/objects/<sha256>`, `UrlCacheEntry::body`).
- **Page snapshots** (`snapshot.hpp`, `doc_codec.hpp`, docs/HISTORY.md
  "E"): every page shown is kept as its document (binary codec), raw source
  and pictures; `snap:<id>/<address>` shows that copy. Pictures of a
  snapshot are `snap:<id>/<picture URL>`: `is_remote_image` /
  `remote_image_path` / `download_image` unwrap them (`live_address`), the
  path is the kept copy or else the URL cache file. Back / Forward and
  History open the snapshot they showed (`App::open_snapshot`); Reload
  loads live.
  - Navigation (`opens_as_image`, app_buffers.cpp): an image-named http(s)
    address skips the fetch only when the cache says image; otherwise the
    web fetcher loads it (`is_web_url` no longer excludes image names) and
    an `image/*` answer (`content_type_is_image`) becomes
    `image_url_document` read from the cache, shown as
    `//gallery//item:1` (`finish_navigate`).
  - Page galleries stay optimistic (`may_be_image`: anything not known as
    Other): the image worker downloads; a text/html answer is recorded and
    `gallery_image_was_page` sends views showing it to the page, replacing
    the image's history entry. Next time the cache routes the link straight
    to the page.
  - Pruned at start: `urls/` by size (512 MiB), rows after ~6 months; the
    old `images/` directory is emptied. Small-net images still store bytes
    only (their fetchers sniff them).
- No posting or secret-key management in v1. Publication transport options
  (WebDAV, static export, Nostr, IPFS) are sketched in `docs/PUBLISHING.md`.

## Images through thumtoo (`image_backend.hpp`)

- `external/thumtoo` is a git subtree of `~/projects/thumtoo/thumtoo.git`
  (`git subtree pull --prefix external/thumtoo … --squash`); change thumtoo
  upstream first, then pull.
- `XANATOO_THUMTOO` (default ON on desktop): `ImageCache` decodes through
  libvips (`image_backend_decode`, every format vips has) and thumbnails
  asynchronously: `thumb()` returns a pending image and queues the request,
  `ImageCache::poll()` (from `poll_images`) uploads finished ones. Thumbnails
  are durable thumtoo tiles (JPEG, WebP with alpha) of the coarse scales
  only, composed by `get_pixels_from_tiles`; the cache is
  `$XDG_CACHE_HOME/thumtoo`, shared with galapix / biltoo.
- The thumtoo boundary treats decoder exceptions (including non-`std::exception`
  exceptions from DjVuLibre) as failed page reads / thumbnails. Its callbacks
  publish an empty `DecodedImage` so failed thumbnails cannot remain pending.
- Without it (unit tests) stb_image decodes (PNG / JPEG / GIF / BMP) and
  thumbnails are BMPs under `cache_dir()/thumbs/`.

## Audio

- `audio.hpp` (pure, unit-tested on `tests/data/audio`): `sniff_audio` by
  magic (RIFF/WAVE, `fLaC`, Ogg + `\x01vorbis` / `OpusHead`, ID3v2 or MP3
  frame sync), `open_audio` from bytes in memory → interleaved float
  frames (`audio_read`, `audio_seek`), length and title (ID3v2 TIT2,
  Vorbis / FLAC comments). Decoders are vendored single files in
  `external/` (dr_mp3, dr_flac, dr_wav, stb_vorbis), compiled in
  `src/audio_codecs.cpp` without our warnings. MP3 opens with a seek table
  (one header scan), so seeking does not decode from the start.
- `is_audio_url` (.mp3 .ogg .oga .flac .wav .opus; local, `file:`, http(s))
  is checked at the top of `navigate_pane`: an audio address calls
  `play_audio` and never becomes a page or a download. Link clicks reach
  it through the usual paths (relative links resolve beside the page first;
  remote ones go to `navigate_to_url` even without the web client).
  Played files are visits (`record_media_visit`, `docs/HISTORY.md` "B").
- Player (`app_audio.cpp`, `App::audio`): `play_audio` loads on a detached
  worker (`AudioLoad`: file read or `https_get_follow_stream` into memory,
  512 MiB cap, progress, cancel), then `update_audio` moves the decoder into
  `AudioShared`, which owns the `sdl::AudioOut` (sdl.hpp bridge: SDL2
  callback device / SDL3 device stream, F32 at the file's rate and
  channels, SDL converts). The callback reads the decoder under
  `AudioShared::m`; seeks take the same lock. End of file pauses the device
  (`AudioState::Ended`); Play replays from the start.
- Controls: the Media panel (`WindowRole::Media`, `App::media_win`,
  session key `media`, Panels → Media). `play_audio` opens it; closing it
  stops the sound (`close_media_panel`). Native-drawn in `app_audio.cpp`:
  title, format / progress / error line, seek bar, times, back 10 s /
  play-pause / forward 30 s / stop, volume. `media_layout` places the
  parts; `media_part_at` is the one hit-test for `draw_media_panel`,
  `click_media_panel` and `media_panel_hover` (status text). Seek bar and
  volume drag: `AudioPlayer::drag`, followed in `update_audio` while
  `sdl::left_button_down()`. Volume is `AudioShared::gain` (slider²),
  applied in the callback, kept across files. Go → Sound has the same
  transport commands (`UiCommand::Audio*`).
- The link whose file is in the player is washed in the lit title-tab
  yellow while the player holds it (`update_playing_links` →
  `Pane::playing_links`, keyed on layout generation and `AudioPlayer::url`;
  the href resolves like a click, `file:` as its path; Stop clears it).
- Bookmarks: right-click in the panel (`right_click_media_panel`) opens
  the bookmark menu (`App::media_menu`, `UiState::media_menu`,
  `UiCommand::MediaAction` → `media_action`) for the seek bar position
  under the pointer (elsewhere the current one): bookmark / remove that
  position, go there, copy its address, bookmark / remove the whole file.
  A position is a Media Fragments address, `file:///a.mp3#t=12.5`
  (`media_time_address`, tenths; `parse_media_time` /
  `split_media_address` read `t=`, `t=npt:1:02`, `t=10,20`, percent-
  encoded too). `update_media_marks` finds the file's bookmarks
  (`AudioPlayer::marks` / `file_mark`, keyed on `bookmarks_gen` and the
  url): green lines (`DocStyle::bookmark`) across the seek bar under the
  knob, a ribbon right of the title for the file. A press near a line
  goes to exactly its position; `mark_near` is the hit-test for click,
  hover and menu.
- A–B section and loop: the A-B button (`audio_toggle_ab`, Go → Sound,
  `UiCommand::AudioAB`) turns on two markers (`AudioPlayer::ab` / `ab_a` /
  `ab_b`; 10 s from the current position), drawn as a shaded span with a
  bracket under the seek bar and tabs in their own row (`MediaLayout::
  mark_a` / `mark_b`, `MediaPart::MarkA` / `MarkB`, dragged through
  `Drag::MarkA` / `MarkB`; at least `kMinSection` apart). `follow_section`
  (each frame) watches for the position crossing B — `last_pos`, which
  `audio_seek` resets, so a seek past B plays on — and then pauses
  (`at_b`: Play starts at A again) or, with Loop (`audio_toggle_loop`,
  `AudioPlayer::loop`, kept across files), seeks to A. Loop without markers
  restarts the file at its end instead of stopping. A `#t=a,b` address
  (`split_media_address` reads the end, `media_range_address` writes it)
  opens with the markers on that section. The right-click menu also sets A
  / B at the pointer and, for the section (a bookmarked one under the
  pointer, else the markers: `MediaMenu::sec_a` / `sec_b`), bookmarks /
  removes it, copies its address or makes it the A–B section. Section
  bookmarks are a green strip along the bar; a press on its start line
  also selects the section.
- `#t=` start: `navigate_pane` / startup split the time off before the
  selector test; `play_audio` keeps it in `AudioPlayer::start_at` and seeks
  once open (`start_playing` / `start_media`). For the file already in the
  player it only seeks (and plays). Visits store `canonical_media_address`
  (the locator form would percent-encode `t=`).
- Streaming (remote files of the built-in formats, threaded builds): a
  detached thread downloads into a `ByteFeed` (byte_feed.hpp, see "Media
  cache" below; `got` / `total`, `done` + error, `cancel`) while the media
  pipe plays it through `open_audio_stream` (`audio_stream.cpp`, a
  `MediaSource` over the built-in decoders). `open_audio_feed` sniffs the
  first bytes (`feed_sniff`, past an ID3v2 tag), then opens MP3 / FLAC / WAV
  on the dr_libs callback APIs (`feed_read` waits for bytes on the worker,
  never in the sound callback; seeks only move the cursor; dr_mp3 gets no
  tell callback, else it reads the end-of-file tags first). A streaming MP3
  has no seek table and, without a Xing / Info header, a length estimated
  from the first frame's bitrate. When the download completes the source
  re-opens the feed in memory (seek table, exact length; the pipe's
  `info.duration` follows) and seeks it to the same frame. Waiting reads
  give up on the pipe's cancel or a newer seek (`MediaOptions::seeks`,
  counted in `media_seek`; `audio_rearm` / `audio_interrupted`).
- Ranges: `feed_download` (byte_feed.cpp, offline-testable over a
  `FeedFetch`; `download_feed` sends `Range: bytes=N-` and reads
  `Content-Range` via `parse_content_range` into a `FeedHead`) asks for
  ranges from the first request. A 206 answer makes the feed `sparse` (it
  knows the length; `have` the arrived extents, `feed_reach`). A read waiting for a byte behind `at` or
  more than 256 KiB ahead of it stores it in `want`; the download breaks
  off and fetches from there, and when a request ends (or runs into bytes
  already there) goes on with the gap behind the reader (`reading`), then
  the first one; a broken connection resumes where it broke (3 tries
  without progress). A 200 answer stays one plain download. FLAC / WAV
  seek through their callbacks; a streaming MP3 jumps by the average frame
  size (`mp3_jump`: dr_mp3 restarted at the byte, `streamStartOffset`
  moved for the moment), so the place is exact only for constant bitrate.
  The jump is noticed on the download's next piece: a stalled connection
  delays it. Files over `fill_whole` (128 MiB) are fetched at most `ahead`
  (64 MiB) in front of the reader (on again when it comes within half of
  that) and their gaps are not filled; smaller ones download whole. Ogg
  (Vorbis / Opus) cannot stream built-in: with a backend the address goes
  to it instead, else the open waits for the whole file.
- Media cache (byte_feed.hpp / .cpp): every remote file the player opens
  in a threaded build gets a feed (`start_feed` in app_audio.cpp) — the
  built-in streaming path and, through `MediaOptions::feed`, FFmpeg. With a
  cache directory its bytes go to `cache_dir()/media/<FNV-1a of the
  address>` (written in place, holes where nothing arrived) and a text map
  `<name>.map` (address, length, type, ETag, Last-Modified, `have` lines;
  rewritten every 8 MiB, at the end and when the feed goes). The next feed
  of the address (`feed_open_cached`) starts with what is there: a
  complete file plays at once without waiting for the network, while
  `revalidate` asks once more and stops at the head (from byte 0: a byte
  past the end of a file that shrank would be refused); a changed length,
  ETag or Last-Modified makes the feed `stale`, the cache file goes with
  it and the next play fetches the new file (no answer keeps it). A
  partial one asks from its first gap and starts over when they changed;
  `open_audio_feed` waits for that answer before it reads a cached feed. One live feed per file (a registry; a second open
  waits up to 2 s for the first to go, else takes a memory feed); files
  are pruned least recently used past 2 GiB (`kMediaCacheBytes`, one file
  at most half). Without a cache directory the built-in path keeps a
  memory feed (512 MiB cap) and FFmpeg fetches by itself. Readers:
  `feed_wait` (wants far bytes; while waiting `reading` is the missing
  byte, so the download follows), `feed_copy`, `feed_head` (format checks
  and tags), `feed_whole` (a complete file for the in-memory decoders,
  read from the cache file).
- The panel: "Downloading N%" while opening, "Buffering… N% loaded" while
  the pipe has nothing to play (`media_buffering`), the part read ahead
  shaded on the seek bar (`draw_bar` `loaded` from `MediaSource::buffered`,
  copied to `MediaPlayback::buffered` by the worker; the feed source gives
  how far the bytes from the reader's position reach). Sound alone decodes 5 s ahead
  (`kMediaAudioAheadSoundOnly`). Local files and single-threaded WASM keep
  the load-then-play path (`AudioLoad`).
- By Content-Type: a Navigate / Tab response that `is_playable_type`
  (built-in audio types; with a backend every `audio/*` / `video/*`), not
  an attachment, sets `DownloadProgress::playable` in `web_fetch` instead of
  the download offer; `finish_navigate` / `finish_tab` hand the address and
  type to `play_audio(app, url, mime)` (the type picks backend or built-in
  as the extension does) and record a media visit. A Content-Disposition
  file name comes along (`DownloadProgress::name`) as the player's title
  until the file's own tags name one. A pane still showing its stand-in
  (`DocBuffer::placeholder`, also the startup address's "Loading…") gets
  a small page about what plays (`media_page`). Such addresses have no
  playing-link wash or bookmark marks yet (those test `is_playable_url`).
- Not yet: built-in Opus (without a backend); range requests (a seek far
  ahead of a built-in download waits for it).

## Video

Video and the audio formats the vendored decoders lack (`.opus`, `.m4a`,
`.aac`, …) play through a platform backend behind `media.hpp`; MP3 / FLAC /
Ogg Vorbis / WAV keep the built-in path above (also where a backend exists).

- Addresses: `is_video_url` / `needs_media_backend` by name;
  `is_playable_url` (built-in audio, or backend formats when
  `media_backend_available()`) is what links, `navigate_pane` and startup
  arguments test. Without a backend video links go to the system as before.
- HLS / DASH playlists (`.m3u8` / `.mpd`, `is_stream_playlist_url` /
  `is_stream_playlist_type`) count as playable when
  `media_backend_plays_playlists()` (FFmpeg yes, Android no).
- Service pages link streams directly so they open in the Media panel:
  PeerTube / Odysee "▶ Play" (docs/VIDEO_SITES.md), Internet Archive
  "▶ Play", feed enclosures the build plays ("▶ Play: type (size)"),
  Reddit posts with a `reddit_video` ("▶ Play video", the HLS address, or
  the sound-less `fallback_url` without playlist support). YouTube and
  Dailymotion keep mpv / the browser.
- Backends (`MediaSource`: `info()`, `next()` gives one piece — float
  sound or an I420 `VideoFrame` — `seek()` to the key frame before):
  - `media_ffmpeg.cpp` (`XANATOO_FFMPEG`, desktop; pkg-config
    libavformat / libavcodec / libswscale / libswresample / libavutil,
    `ffmpeg-headless` in the flake): local files and streamed http(s)
    (FFmpeg's own TLS; `user_agent("video")`, interrupt callback on
    cancel), more than two channels mixed to stereo, cover-art streams
    ignored. Streams (`MediaOptions::read_ahead`: -1 = http(s), 1 in tests)
    get a reader thread (`read_ahead`) that demuxes up to 30 s / 96 MiB of
    packets ahead into a queue; `next()` decodes from it (`read_packet`,
    waits give up on cancel or a newer seek), seeks are requests the reader
    carries out (`seek_reader`; the interrupt callback abandons a read in
    progress). `buffered()` is the end of the latest packet read. With
    `MediaOptions::feed` (`media_backend_reads_feeds`) an http(s) address
    waits for the feed's answer (`feed_wait_answer`); a ranged answer that is
    no playlist (`playlist`: HLS / DASH by type or extension) is read
    through an AVIOContext of ours (`read_from_feed`, `io_read` / `io_seek`:
    `feed_wait` + `feed_copy`, AVSEEK_SIZE = the length), else the feed is
    cancelled and FFmpeg fetches the address itself (servers without
    ranges, live streams, playlists).
  - `media_android.cpp` (`__ANDROID__`, `-lmediandk`, API 21):
    AMediaExtractor (file descriptor, or the URL with the system's
    networking) + AMediaCodec in ByteBuffer mode. Pictures are copied from
    the decoder's layout (I420 = colour format 19, else read as NV12; stride,
    slice height, crop from the output format, logged to logcat). Opening
    pre-rolls until the sound decoder's own output format (HE-AAC doubles
    the rate) is known.
  - Neither (Windows, WASM): `open_media` fails, nothing routes there.
- Pipe (`MediaPlayback`, unit-tested with fake sources): a detached worker
  opens the source (`open_media`, or `MediaOptions::open`) and decodes ahead into `audio` (deque of floats) and
  `frames` (≤ `kMediaFramesMax`); it waits on a condition variable that
  the sound callback (`media_pull_audio`) and the main thread
  (`media_take_frame`) signal. A full picture queue stops decoding — files
  decode pictures up to ~0.5 s behind their sound, and dropping old
  pictures to read sound further ahead dropped exactly the due ones; only
  when the sound runs dry (< 0.25 s) are the oldest dropped. Seeks are
  requests (`media_seek` clears the queues; the worker seeks the source and
  trims sound / drops pictures before the target).
- Clock: sound frames handed to the device since the last seek + its
  target; without sound (or once the sound has ended before the pictures)
  the wall clock, paused with the player (`media_set_paused`). The first
  picture after a start or seek shows at once (`want_first`), so a paused
  seek shows where it went.
- Player (`app_audio.cpp`): `play_audio` starts `media_start` for backend
  formats; `update_audio` calls `start_media` once `opened` (sound device
  for `info.rate` / `channels` with the same `fill` callback pulling from
  the pipe; `fit_panel_to_video` sizes a floating panel), then each frame
  `show_frame` uploads the due picture to an `SDL_PIXELFORMAT_IYUV`
  streaming texture (`AudioPlayer::video_tex`, recreated on size change,
  destroyed in `close_player`). The panel letterboxes it above the controls
  (`MediaLayout::video`, `MediaPart::Video`: click = play / pause).
- Fullscreen (`toggle_media_fullscreen`, `App::media_full` — a
  `MediaFullscreen` record outside `AudioPlayer`, which `audio_stop`
  resets): the panel's button (`MediaPart::Fullscreen`, right end of the
  button row while there is a picture), a double click on the picture,
  Go → Sound → Fullscreen Video, the right-click menu
  (`UiCommand::AudioFullscreen`). The window goes fullscreen unless it was
  already (`window_was_fullscreen`, restored on leaving). `media_layout`
  then places the picture over the whole window and the controls in a strip
  over its bottom (`MediaLayout::controls`), so draw, click, hover and drag
  share one hit-test as in the panel. `draw_media_fullscreen` paints last in
  `run_frame` (panes, gutters and split chrome are skipped; ImGui menus stay
  on top, the menu bar is hidden through `UiState::chrome_hidden`); the
  strip shows while paused, while dragging, with the pointer on it, or for
  `kControlsLinger` after the pointer moved, and the pointer hides with it.
  Input is modal (`media_fullscreen_event`, first in `handle_event`): the
  pointer and wheel (volume) go to the controls, plain keys to
  `fullscreen_key` (all taken) and through it to `player_key` — Esc / F /
  F11 leave, Space / K play-pause, Left / Right 10 s (Shift 60 s), ',' /
  '.' a picture back / on, Home / End, Up / Down / M volume; keys with Ctrl /
  Alt go on as usual.
- Hidden controls (`toggle_media_controls`, `App::media_controls_hidden`,
  `UiCommand::AudioControls`): the right-click menu's Hide Controls, Go →
  Sound → Hide Player Controls or H in the focused panel. While a video plays
  outside fullscreen (`media_bare`), `media_layout` gives the picture the
  whole client area and no control rects, so every point is
  `MediaPart::Video` (click play / pause, double click fullscreen,
  right-click the menu to show them again). Sound-only files and fullscreen
  show the controls as usual; the flag stays for the next video.
- Keys in the panel: with `KeyFocus::Panel` on `WindowRole::Media`,
  `media_panel_key` (in `on_keydown`, before `panel_key_unused`) gives the
  same `player_key`s (F enters fullscreen, Esc does not) and eats their
  text input; other keys go on to the document, which takes the focus.
- Frame steps (`audio_step_frame`): pause, then seek a quarter picture
  before the next / previous picture (the worker drops pictures before a
  seek's target; `want_first` shows the first after it). The spacing is
  `MediaInfo::frame_rate` (FFmpeg `av_guess_frame_rate`, Android
  `KEY_FRAME_RATE`), else the smallest pts step `show_frame` saw, else
  1/25 s. `AudioPlayer::step_to` holds a step whose picture is not shown
  yet, so quick presses go on from it. While an ImGui popup is open (the right-click menu)
  events go to ImGui only. `update_audio` leaves fullscreen when the player
  stops, the panel closes or a file without pictures plays.
- `examples/video.md` (synthesised H.264 + AAC MP4 and VP9 + Opus WebM);
  fixtures `tests/data/video` (0.5 s, 64×48, red / blue).
- Next: playing the stream URLs of YouTube /
  PeerTube pages here instead of mpv, `<video>` in web pages, subtitles.
