# Content model, layout and formats

Developer notes split out of `AGENTS.md`: document structure, layout,
HTML mapping, tables, images, collections, imports and style sheet details.

## Style sheets (details)

- Sheets may name faces with poor coverage (Verdana has no arrows):
  `Font::glyph` draws codepoints the face lacks from fallback faces
  (`fallback_font_faces`: FontConfig coverage sort, fixed list on Android);
  line metrics stay the primary face's.
- Font lookups return a `FontFace` (file + collection index); `Font::load`
  opens that member, so a `.ttc` / `.otc` match is the requested family,
  not face 0. FontConfig supplies `FC_INDEX`; without FontConfig
  (Android) `scanned_faces` lists every member of each file once at first
  lookup (cached), with style from flags, then style name, then the file
  name (single-face files only). Pass `face()`, not `path()`, when
  reopening a loaded font.
- `ViewSettings::style` (view.conf) is the global *fallback* sheet; the toolbar
  and View menu issue `UiCommand::SelectStyle` / `ReloadStyles`. The dockable
  Styles panel uses an embedded ImGui editor and `style.cpp` helpers to create,
  validate, atomically save and delete user-owned `.xss` sources. Shipped
  presets remain read-only; users duplicate them to customize.
- Domain/service pins persist separately in `style-pins.conf`, with exact
  domain keys checked before service keys. `sync_pinned_style` changes only
  the resolved active look, never the saved global fallback. Do not write the
  active pinned sheet into `view.conf` on navigation.

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
  node (remote ones load like any remote image; lazy `data-src` / `srcset`
  via `html_img_src`); icons (declared edge ≤ 48) and images inside
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
- Page chrome (nav / aside / footer / form / dialog, landmark roles, navbox /
  share / cookie classes, what lies around `<main>`) is kept as
  `NodeKind::Chrome` nodes (`summary` = what it is); hidden / sr-only text is
  always dropped. Reader mode is view state (`Pane::reader`, `pane_reader` /
  `set_reader`, default `ViewSettings::reader_mode`): layout leaves Chrome out
  (`set_hide_chrome`) and draws the "… hidden · Show all" notice
  (`LayoutResult::chrome_notice`). Never put reader mode into the address or
  parse a page twice for it (docs/PASSAGES.md "Reader mode").
- `web_fetch` dispatches on Content-Type; bodies go through
  `detect_charset` / `to_utf8` (util.hpp) first.
- `external:<url>` hrefs force the system browser (`open_external_url`).
- Browsing is the default: `App::edit_mode` starts false and F2 / the Edit
  toolbar toggle opts into editing. Foreign content is read-only:
  `is_foreign_path` (remote URL, `.html`, any `is_pandoc_path` file,
  anything under `cache_dir()`) makes `browsing(app)` true even with
  `app.edit_mode` set. Gate document edits on `browsing(app)`, never on
  `app.edit_mode` alone. Edit toggle is disabled for foreign content.

## Tables

- `TableCell::col_span` / `row_span` are occupied column and row counts;
  `build_table_grid` (`table_grid.hpp`) derives slots and placements without
  duplicating cells. Spanning tables are laid out table-wide; borders stop at
  merged cells and a complete unspanned row supplies column weights.
- HTML `rowspan=0` resolves at its row-group boundary. Markdown uses
  `:::cell` / `:::th` with `colspan=` / `rowspan=`; S-expressions use
  `:colspan` / `:rowspan`. Enter inside a cell extends spans crossing the
  inserted row. Details: `docs/TABLES.md`.

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
  and calls `remote_done`.
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
- Layout draws remote images from their `kRemoteThumbEdge` thumbnail
  (`layout_remote_image`); a click shows it in the page's `//gallery`.
- Links to images (`is_image_url`: a `has_image_fetch_scheme` URL or local
  path by extension)
  show that image in the page's `//gallery` (`open_link_image`, checked
  before split / external handling), never the system browser; one not
  among the page's images opens as `URL//gallery//item:1`
  (`is_image_address` / `image_url_document`, `gallery.hpp`).
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
- Without it (unit tests) stb_image decodes (PNG / JPEG / GIF / BMP) and
  thumbnails are BMPs under `cache_dir()/thumbs/`.
