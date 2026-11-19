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
- Browsing is the default: `App::edit_mode` starts false and F2 / the Edit
  toolbar toggle opts into editing. Foreign content is read-only:
  `is_foreign_path` (remote URL, `.html`, any `is_pandoc_path` file,
  anything under `cache_dir()`) makes `browsing(app)` true even with
  `app.edit_mode` set. Gate document edits on `browsing(app)`, never on
  `app.edit_mode` alone. Edit toggle is disabled for foreign content.
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
- An http(s) name is only a guess ("/wiki/File:X.jpg" is a web page): the
  answer's Content-Type decides. **URL cache** (`url_cache.hpp`): every
  `web_fetch` answer and remote image download is kept under
  `cache_dir()` — bytes in `urls/<FNV hash>` (also `remote_image_path`),
  metadata (final URL, status, Content-Type, ETag, Last-Modified, time,
  size) in `urls.sqlite`, keyed by requested and final URL.
  `url_cache_kind` → Image / Other / Unknown (non-200 answers: Unknown).
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
- `#t=` start: `navigate_pane` / startup split the time off before the
  selector test; `play_audio` keeps it in `AudioPlayer::start_at` and seeks
  once open (`start_playing` / `start_media`). For the file already in the
  player it only seeks (and plays). Visits store `canonical_media_address`
  (the locator form would percent-encode `t=`).
- Not yet: built-in Opus (without a backend), streaming the built-in
  formats before the download ends.

## Video

Video and the audio formats the vendored decoders lack (`.opus`, `.m4a`,
`.aac`, …) play through a platform backend behind `media.hpp`; MP3 / FLAC /
Ogg Vorbis / WAV keep the built-in path above (also where a backend exists).

- Addresses: `is_video_url` / `needs_media_backend` by name;
  `is_playable_url` (built-in audio, or backend formats when
  `media_backend_available()`) is what links, `navigate_pane` and startup
  arguments test. Without a backend video links go to the system as before.
- Backends (`MediaSource`: `info()`, `next()` gives one piece — float
  sound or an I420 `VideoFrame` — `seek()` to the key frame before):
  - `media_ffmpeg.cpp` (`XANATOO_FFMPEG`, desktop; pkg-config
    libavformat / libavcodec / libswscale / libswresample / libavutil,
    `ffmpeg-headless` in the flake): local files and streamed http(s)
    (FFmpeg's own TLS; `user_agent("video")`, interrupt callback on
    cancel), more than two channels mixed to stereo, cover-art streams
    ignored.
  - `media_android.cpp` (`__ANDROID__`, `-lmediandk`, API 21):
    AMediaExtractor (file descriptor, or the URL with the system's
    networking) + AMediaCodec in ByteBuffer mode. Pictures are copied from
    the decoder's layout (I420 = colour format 19, else read as NV12; stride,
    slice height, crop from the output format, logged to logcat). Opening
    pre-rolls until the sound decoder's own output format (HE-AAC doubles
    the rate) is known.
  - Neither (Windows, WASM): `open_media` fails, nothing routes there.
- Pipe (`MediaPlayback`, unit-tested with fake sources): a detached worker
  opens the source and decodes ahead into `audio` (deque of floats) and
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
- `examples/video.md` (synthesised H.264 + AAC MP4 and VP9 + Opus WebM);
  fixtures `tests/data/video` (0.5 s, 64×48, red / blue).
- Next: fullscreen, keyboard keys, playing the stream URLs of YouTube /
  PeerTube pages here instead of mpv, `<video>` in web pages, subtitles.
