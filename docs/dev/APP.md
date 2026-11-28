# App structure and view rules

Developer notes split out of `AGENTS.md` (which keeps the short list of
rules). Read this before touching views, panes, tabs, windows, docks,
navigation, buffers, prompts or drafts.

## Source layout (app = world + systems)

`main()` only creates the SDL window/renderer and loops `run_frame`.
All state is the plain-data `xanatoo::app::App` (`include/xanatoo/app.hpp`);
behaviour is free functions ("systems") taking `App&`:

| File | System |
|------|--------|
| `app_init.cpp` | build initial world, shutdown |
| `app_events.cpp` | `handle_event` → one `on_<sdl_event>` per event type |
| `app_frame.cpp` | frame stages: sync size, relayout, `run_chrome` (UiState + `execute_command`), draw panes, overlays |
| `app_passage.cpp` | the passage a view shows (`docs/PASSAGES.md`): selectors of the address are view state on `Pane`, `pane_address` / `pane_cut` / `apply_locator`; pure address parsing in `locator.hpp` |
| `app_chrome.cpp` | status line (`compute_status` is pure data → `draw_status_bar`), floating window frame, client scrollbar |
| `app_windows.cpp` | floating windows (`FloatWindow`, role = Help/Outline/Backlinks/Sitemap/Page/History/Styles/Document): one z-stack, shared hit-test / drag / resize / scroll / draw |
| `app_softwin.cpp` / `dock_slices.hpp` | docked panel geometry: adjacent panels in a side column or top/bottom band have persisted relative `dock_share` sizes; drag the separator between them to resize, independently of the dock's outer width/height handle |
| `service_bar.cpp` | the service strip of an address (`service_bar`, pure; dispatches to `hn_service_bar` / `forum_service_bar` / `nntp_service_bar`); the app keeps it per view in `Pane::service` (`pane_service_bar`, `pane_head_h`), `Ui::service_bars` centers the controls above the accent rule and aligns the search field with the navigation buttons — docs/UI.md "Service strips" |
| `search.cpp` | search engines (built-in table + user `search.sexp`), the `search:<engine>:<query>` address, query encoding, Ctrl+L keywords — pure, `search.hpp`, `docs/SEARCH.md` |
| `opensearch.cpp` | a site's OpenSearch description → engine (`parse_opensearch`, `engine_sexp`, `save_user_engine` to `search.sexp`, worker-thread `fetch_opensearch`), `opensearch.hpp` |
| `page_links.cpp` | what a web page declares in `<link rel>`: icons, next / prev / contents …, OpenSearch, canonical (`safe_canonical`), hreflang, language names, Accept-Language value — pure, `page_links.hpp`, `docs/dev/CONTENT.md` "Page links" |
| `search_results.cpp` | JSON search engines → Markdown result documents (`search_results_markdown`, pure; `search_fetch` needs HTTPS): wikipedia / hn / archive / gutendex / lemmy / bluesky mappers; link text is literal in our parser, only brackets are changed |
| `app_search.cpp` | search bar state (`App::search`, engine = `ViewSettings::search_engine`): show / toggle / select engine / `run_search`; the ImGui row is in `ui.cpp`; "Add …" OpenSearch engines (`add_search_engine_from` / `poll_search_engine_adds`); preferred languages → Accept-Language (`set_preferred_languages`) |
| `app_softwin.cpp` | window placement: `Dock` (None / Left / Right / Max) for every role — `dock_window` / `undock_window` / `toggle_maximize`, `place_windows` derives docked geometry, `dock_column_w` is what `layout_panes` leaves out |
| `app_touch.cpp` | touchscreen gestures: tap → click, drag/fling scroll, long press (on the 500 ms timer, finger still down) selects the word and opens the context menu, dragging on closes it and extends the selection; on ImGui chrome (`TouchMode::UiHold`) a held touch becomes an ImGui right click via `Ui::touch_long_press`, which also releases ImGui's touch-synthesised left button so the lift clicks nothing; pinch: pane text zoom; on a single gallery image pan, swipe to step, pinch zoom; two fingers either side of a tile divider, dock split or docked panel edge grab that gutter and drag it (search from the fingers' midpoint outward: `segment_hit_near_middle` in `touch_gutter.hpp`; the gesture starts the gutter drag directly, including over the tile badge or diff track, then shares the mouse motion and release path; a first finger on an embedded ImGui control can also start the gesture, which cancels the synthesized click; `cancel_press` drops a native drag the first finger started), one finger on any of those gutters drags it directly; a finger holding a free window's title plus a second finger anywhere span the window (`TouchMode::FloatSpan` → `float_span_begin` / `_motion` / `_end` in `app_windows.cpp`; each edge follows the finger on its side, `span_axis` in `touch_span.hpp`), lifting the second finger resumes the title drag; lifting the last finger calls `pointer_leave` (no hover until the next motion, ImGui included via `UiState::pointer_away`) |
| `app_buffers.cpp` / `app_view.cpp` / `app_nav.cpp` / `app_softwin.cpp` / `app_prompts.cpp` / `app_pointer.cpp` | document buffers, scroll/fold/outline, URL history + bookmarks, floating windows, minibuffer, cursor/gamepad |
| `app_fetch.cpp` | background page fetches (`FetchJob` on a worker thread → `poll_fetches`), page cache, loading indicator; a Navigate job remembers its view's buffer (`FetchJob::buf`), and `retarget_fetches` hands it to that tab when the view switched tabs meanwhile |
| `snapshot.cpp`, `object_store.cpp`, `doc_codec.cpp` | page snapshots (docs/HISTORY.md "E"): content-addressed objects, the lossless document codec, `snapshots.sqlite`, `snap:ID/URL` refs; captured in `finish_fetch`, shown by `fetch_page` when asked through `App::open_snapshot` (set by `nav_go` / History `open_line`, taken by `start_fetch`); `NavHistoryEntry::snapshot` via `note_entry_snapshot`; bookmarks pin theirs (`sync_bookmark_pins`) |
| `app_images.cpp` | remote (http/https) images: worker downloads of what `ImageCache` queued → the URL cache (`url_cache.hpp`, `cache_dir()/urls/`); an answer that is a web page switches its gallery view to that page (`gallery_image_was_page`), relayout on completion and on finished thumbnails (`poll_images`); cache pruning at startup |
| `images_thumtoo.cpp` | `image_backend.hpp` over vendored thumtoo (`XANATOO_THUMTOO`): libvips decode, thumbnails composed from the shared tile cache |
| `app_audio.cpp` / `audio.cpp` | sound player (`App::audio`): links to MP3 / FLAC / Ogg Vorbis / WAV play from `navigate_pane`; worker load, SDL audio callback through `sdl::AudioOut`, controls in the Media panel (`WindowRole::Media`) and Go → Sound; see `docs/dev/CONTENT.md` "Audio" |
| `media.cpp` / `media_ffmpeg.cpp` / `media_android.cpp` | video and backend audio formats (`media.hpp`): address tests, the decode-ahead pipe (`MediaPlayback`), FFmpeg (desktop) and MediaCodec (Android) sources; shown by `app_audio.cpp` in the Media panel; see `docs/dev/CONTENT.md` "Video" |
| `app_collection.cpp` | collection view state (layout / size / sort), card and image-link clicks → the page's `//gallery//item:N` |
| `app_gallery.cpp` / `gallery.cpp` | gallery views (`docs/PASSAGES.md` "Gallery"): `//gallery` grid is a derived Collection in `Pane::source_doc` (built by `apply_gallery_locator` in `app_passage.cpp`, items by pure `gallery_items`); `//gallery//item:N` draws one image into the pane rect with view-local zoom / pan (`Pane::gallery`, `pane_image_view`). Stepping replaces the history entry. No window-wide overlay |
| `app_drafts.cpp` | unsaved edits → drafts in the revision store (`revstore.hpp`): autosave, restore on load (`attach_draft`), save / revert, `drafts:` page |
| `app_about.cpp` | `about:` page (Help → About): services, document formats, images (thumtoo / libvips loaders, MuPDF / DjVu) and libraries of this build, gathered from the feature switches and runtime versions; `about_markdown` (`about.hpp`) formats it. New services and user-visible optional deps get an entry there |
| `app_report.cpp` | startup report on stderr (gamepkgstd `LOGGING.md`): version / build / compiler, features, system, SDL / loader env, each library's runtime vs built-against version and the mapped file, video, renderer + GL strings, controllers with the mapping SDL applies, directories. New libraries get a line there too; choices made while loading (fonts, icons, style, ImGui render path) are logged where they are made, as `topic: details` on stderr |
| `app_find.cpp` | find and replace in the focused view: `find_retype` (typing searches from `Prompt::origin`, so a match that still fits stays), `find_step`, `find_replace_one` / `_all`, `find_hits` (cached per view: count and scrollbar ticks), the bar's geometry (`find_bar_h` / `find_bar_rect`); the search itself is `find_next` / `find_prev` / `find_all` in `editor.cpp` (case folded, matches may cross spans) |
| `app_footnotes.cpp` | footnote overlay: notes of on-screen `FootnoteRef` marks laid out in a strip at the pane bottom (`FootnoteOverlay` per pane), in as many columns as the text column fits (min 18em each, whole notes balanced by height via `balance_columns`), leaving out the last notes when their marks would sit under the strip (`fit_footnote_strip`: marks top to bottom, the most leading notes whose height keeps shown marks above and the rest under the strip, from the scroll position alone so there is no resize feedback), on a sheet a shade off the paper under a thick rule; links in notes hover, follow, middle-click into a tab and take the right-click link menu like page links; nothing of the page under the strip is hovered or targeted (`footnote_overlay_link` hit-tests `FootnoteOverlay::doc`; `Hover::link_in_footnotes` marks the hovered link as the strip's; `follow_link_hit` in app_events.cpp is shared with page clicks), a click elsewhere jumps to the note; `hovered_footnote` (mark under the pointer) is washed in the style's `mark` colour here and in the Context panel (`App::context_footnote`) |
| `app_quotes.cpp` | quote links in threads (`docs/QUOTES.md`): `quote_links_of` caches `find_quote_links` per buffer (`DocBuffer::quotes`, keyed on `DocBuffer::doc_gen`), margin arrows on linked quotes (`quote_icons` / `quote_icon_at`), dotted underline under quoted originals, context menu jumps both ways (`App::context_quotes`, `UiCommand::JumpQuote`); `refresh_context_panel` fills the Context panel from the hover (quote, link, unlinked image, transclusion). Pictures (an image link's target, or an image that is no link) set `App::context_image_src` / `context_image_base` and are drawn fitted by `draw_context_image` (app_gallery.cpp, the gallery's single-image look), not laid out. A laid-out page takes clicks (`press_context` in app_windows.cpp): links open in the focused view via `follow_context_link` / `context_link_target`, resolved against `App::context_address` (the page `context_doc` came from; set wherever it is loaded, also in app_fetch.cpp); other presses select text (`App::context_sel`, Ctrl+C / Ctrl+A in `handle_context_key`). An in-page link to a thread message (HN's "reply to …", `#id:N`) sets `App::context_message`: the panel washes and outlines that card (up to its first nested reply) and `draw_context_window` ties it to the hovered link with the quote connector curve (`draw_connector` in app_frame.cpp) |
| `app_page.cpp` | Page panel (`WindowRole::Page`, `PagePanel`, Ctrl+Alt+P): navigation / sidebar Chrome (`margin_chrome`, found at any depth) of the focused page and service links from `focused_page_info` as link rows in a dockable tool window; while it is open the panes leave those regions out (`LayoutEngine::set_hide_panel_chrome`, set by `update_page_panel`) |
| `app_events.cpp` / `app_windows.cpp` / `ui.cpp` | Reply panel (`WindowRole::Reply`): right-click a semantic Message card → Reply…; `App::reply` holds the target and Markdown draft, the ImGui multiline editor overlays the custom dockable frame, and `app_frame.cpp` paints an amber card wash/scrollbar mark. Mock only; never posts or handles accounts (`docs/REPLIES.md`) |
| `app_revisions.cpp` / `revstore.cpp` | Dockable Document Revisions tree (`WindowRole::Revisions`) follows the focused document. Mouse and keyboard selection flush pending edits, move the persisted working pointer, and refresh views around their existing scroll anchors. The next autosave appends to the selected node. Local-day dividers are visual rows. Snapshot sizes come from SQLite metadata. Visible revisions get bounded parent comparisons cached by revision ID. The row menu opens two read-only `db-revision:` tabs in Compare mode. The two panes’ revisions are marked in the tree. Selecting a revision changes the focused pane’s snapshot address (`docs/REVISIONS.md`) |
| `app_windows.cpp` / `ui.cpp` | Annotations panel (`WindowRole::Annotations`): **native** dockable `FloatWindow`, not a standalone ImGui window. ImGui only supplies the rich editing widgets clipped to its client area, as with Reply; annotation revisions remain independent of document history |
| `app_windows.cpp` / `ui.cpp` | Bookmarks panel (`WindowRole::Bookmarks`): native dockable `FloatWindow`, not a standalone ImGui window; the bookmark list and actions are ImGui widgets inside its client. Preferences remains a separate settings dialog |
| `app_subscriptions.cpp` / `subscriptions.cpp` | Subscriptions panel (`WindowRole::Subscriptions`): a dockable persistent read-only watchlist for HTML pages and changing Lemmy, Nostr, RSS/Atom, HN, Reddit and native forum collections. Reuses `FetchPurpose::Subscription` workers with two concurrent checks and staggered automatic requests; compares stable item IDs or the chrome-free HTML conversion’s XML fingerprint for unread counts. `state_dir()/subscriptions.txt` stores the rolling snapshot, counts, intervals and check times. See `docs/SUBSCRIPTIONS.md` |
| `ui.cpp` | Download confirmation, Preferences and Credentials share `begin_panel_dialog` for panel-style title strips and left close controls. ImGui owns dialog placement, focus, and stacking. |
| `ui_panel_layers.hpp` / `src/ui_panel_imgui_order.hpp` / `ui.cpp` | Embedded ImGui panels are classified by exact root/child names, drawn after their native FloatWindow, and reordered for ImGui hit-testing in `float_stack` order. Native foreground frames mask obscured editor mouse events; popups/menus/dialogs remain foreground. Old SDL2 fallback renderer keeps one full UI pass. See `docs/UI.md` |
| `app_certs.cpp` | Gemini certificates: `GeminiSiteCert` per server met this session (`App::gemini_certs`), address-bar site icon (`cert_ui_state`), dockable Certificates panel (`WindowRole::Certificates`; ImGui body in `ui.cpp`) to view, accept / keep a changed certificate, or forget a pin (`gemini_tofu_list` / `gemini_tofu_forget`) |
| `app_frame.cpp` / `ui.cpp` | Cookies panel (`WindowRole::Cookies`): follows the focused HTTPS page by default, browses other origins from the shared jar, and displays a header preview and the latest cookie names sent. Values start hidden. Site and per-cookie controls run through `execute_command`; the open panel refreshes its snapshot twice a second so CLI imports appear while the app is running. |
| `app_identity.cpp` | Identities panel (`WindowRole::Identities`, `IdentityPanel`; ImGui body in `ui.cpp`), one tab per protocol, the data stays separate. The tab follows the focused page (`IdentityPanel::section`, set once per page-kind change; a tab click reports `UiCommand::IdentitySection`). Gemini tab: client certificates to create, use on a page / site, switch, rename, delete; `note_gemini_identity` opens it on status 60/61/62; store and scopes in `gemini_identity.cpp`. Nostr tab: watch-only accounts in `nostr.sqlite` (`nostr_command`, `refresh_nostr_identities`); `show_identities(app, section)` |
| `app_hoverhelp.cpp` / `ui_help.cpp` | Help panel (`WindowRole::HoverHelp`, `HoverHelp`, Shift+F1): explains the GUI element under the pointer. ImGui widgets via ImGui's item hooks (`Ui::hover_item`), canvas chrome via `hover_help_key_at` with the action's own hit-tests; texts in the pure `ui_help.cpp` table. See `docs/UI.md` "Help panel" |
| `app_sitemap.cpp` | Sitemap panel (`WindowRole::Sitemap`, `SitemapPanel`): site tree of the focused page's host, one `SiteCache` per host loaded on a worker thread; "Page" opens `sitemap:host`; also `focused_page_info` (the focused page's `<link rel>` facts and feeds, `App::page_info`) and `focused_canonical` |
| `app_related.cpp` / `related_sites.cpp` | Related Sites panel (`WindowRole::RelatedSites`, `RelatedPanel`; Panels / Go menus): similar websites of the focused page's site, asked once per host on a worker thread while the panel is open (`update_related_sites`), kept for the session (64 hosts). ImGui list in `ui.cpp`; click opens in the focused view, middle-click / context menu a new tab. "Follow page" off (`RelatedPanel::follow`, session `related-sites.follow`) holds the list on the shown site while its rows are explored. Formerly "Recommendations" (session key `recommendations.open` still read). Sources in `docs/dev/SERVICES.md` |
| `app_downloads.cpp` / `download.cpp` | Downloads panel (`WindowRole::Downloads`): MIME/attachment routing, streaming files, live progress/cancel, and persisted completed-file history; `https_get_follow_stream` decodes HTTP transfer chunks without buffering binaries |
| `http_url.cpp` / `https.cpp` | Scheme-preserving URL parsing/redirect resolution plus HTTP/1.1 over plain TCP (`http://`) or TLS (`https://`); historical `https_*` entry points accept both; parser is tested without network dependencies |
| `catalog_credentials.cpp` | `catalog-credentials.conf` in `config_dir()` holds the optional IGDB/RAWG/TMDB/Discogs keys; private 0600 Unix writes, environment overrides, loaded per fetch; Edit → Credentials is the unified editor for catalog keys and Reddit OAuth (see `docs/SEARCH.md`) |
| `reddit_credentials.cpp` | `reddit.conf` in `config_dir()` holds optional app-only OAuth credentials with private Unix permissions; `XANATOO_REDDIT_CLIENT_ID` and `XANATOO_REDDIT_CLIENT_SECRET` override only as a complete pair, read on each fetch; Edit → Credentials provides masked editing, save, clear and official signup links (see `docs/REDDIT.md`) |
| `app_pages.cpp` | paged reading (`docs/PAGED.md`): `Pane::paged` / `PageView`, `paginate_pane` (called by `layout_panes`, cuts the layout with `paginate.hpp`), `page_turn`, page bar + edge zones + column rules (`draw_page_chrome`, `page_pointer_*`); while `pages.on`, `scroll_y` is a multiple of `stride`, `Pane::h` leaves out the bar, `sb_reserve` is 0 — use `pane_max_scroll`, `spread_at`, `page_goto_spread`, never pixel scrolling |
| `fixed_layout.cpp` / `fixed_layout_thumtoo.cpp` / `app_pagetiles.cpp` | PDF / DjVu / CBZ / CBR (`docs/FIXED_LAYOUT.md`): `read_fixed_layout` (thumtoo page list, sizes, outline; worker) → `fixed_layout_document` (pure: `NodeKind::Page` nodes in outline Sections). Page sizes come only from thumtoo, never guessed. `app_pagetiles.cpp` draws thumtoo LOD tiles over the raster each frame (`PositionedImage::page_w > 0`); paginate breaks before every page |
| `math_parse.cpp` / `math_layout.cpp` / `math_font.cpp` | TeX math (`docs/MATH.md`): `parse_math` (TeX subset → `MathNode`), `layout_math` (TeXbook App. G on a `MathFace`, em units, y up) and the FreeType `MathFont` (MATH table, glyph rasters by glyph id, `draw_math`); `FontSet::math_font()` finds the font on first use. A formula is an `InlineKind::Math` span (`src` = TeX, `display`) laid out as an atom by `layout_paragraph` (`PositionedMath`); never draw math through `Font`. Pure parts are unit-tested with a fake `MathFace` |
| `app_split.cpp` | Trail-mode linked navigation: first-tile links open the second, second-tile links advance the trail; hover previews (`SplitPreview`, only with `ViewSettings::hover_preview`, off by default) |
| `theme.cpp` / `ui.cpp` | GTK2 Clearlooks-inspired grey and shared native/ImGui palette; BeOS-style title bars tinted per panel family (`theme::Tint` / `title_tint`, picked from the window role in `window_tint`, app_chrome.cpp): documents keep the yellow tab, navigation is blue, links teal, sites green, annotations lime, feeds / downloads orange, history / revisions amber, media red, bookmarks rose, social plum, style sheets violet, help mint, certificates steel; saturated while active, a faint hint on the grey strip when idle. Consistently sized window controls. **Regular panel buttons stay raised; toolbar actions are flat until hover.** The edit strip's lock and toggles without alternate graphics stay sunken while active (`tool_button(..., latched)` supplies the state *inside* the style stack); Split/Join and empty/filled Bookmark use alternate SVGs under `icons/svg/` and remain flat, including when active. Reload uses opposed green arrows. Native scrollbar thumbs use `theme::draw_scroll_thumb` (separate from button faces) for strong contrast; ImGui uses matching grab colours. `scrollbar_geometry.hpp` shares full-height, arrowless scrollbar thumb mathematics between paint and mouse paths |
| `style.cpp` | document style sheets (`styles/*.xss`, S-expressions): `DocStyle` roles → layout spacing/fonts/glyph colours + renderer decoration colours; `apply_style` in `app_view.cpp` |

`fetch_page_guarded` is the final exception boundary for detached page workers (also used in single-threaded WASM): exceptions from a parser, decoder or service complete `FetchShared` as an error, never leave a pending job or escape the thread. Remote image workers independently complete failed image requests on exception. The application state is still only changed by the frame-loop polling systems.

## Views, navigation, buffers, prompts, windows

View → Fullscreen (F11) hides the app chrome. The document context menu
offers Exit Fullscreen while this mode is active. Open it with a right
click or a touch long press.

Middle-clicking a source in the Subscriptions menu uses the same tab-opening
path as History and Bookmarks, and marks the subscription read.
New subscriptions check every six hours by default. Existing subscriptions use
their saved intervals.

Document views: `App::panes[kMaxViews]` — slots 0 / 1 are the centre
(split), document windows own a slot from `kCentreViews` up
(`FloatWindow::view`, `alloc_view` / `release_view` / `close_doc_window`).
Every view has its own rect (`Pane::x/y/w/h`, centre rows from
`place_centre_rows`, windows from `place_windows`), so the whole pane path
(layout, raster, caret, editing, overlays, footnotes) serves windows too.
Centre tiling currently has two slots: `App::tile_axis` chooses columns or
rows; `App::tile_mode` chooses independent or linked Trail. Compare is a
diff-marked independent pair (`App::diff_mode`), with a wider mode-coloured
divider and clickable badge. The divider's right-click menu and badge are
handled by `open_tile_menu`; `tile_divider_rect` and `tile_mode_button_rect`
are shared by hit-testing and drawing. This is the two-leaf foundation for
the planned arbitrary tiling tree (`docs/TILING.md`). Session save/restore
persists axis and relation, with old split sessions mapped to Trail.
The pure recursive geometry/split/close model is `tile_tree.hpp`: it is
unit-tested without SDL, and `place_centre_rows` uses its two-leaf adapter.
Do not equate its capacity for N leaves with N-view application support:
navigation, hit-testing, windows and sessions still assume centre slots 0/1.
Compare dividers draw the two-lane overview from `App::diff_overview`;
`diff_overview_positions` (`diff_overview.hpp`, pure C++20) derives markers
from document-tree order rather than the virtualized visible layout.
`tile_diff_track_rect` is the common hit/draw geometry; dragging the track
scrolls both panes to a normalized position, not an exact aligned hunk.
`app.focus` may be any live slot; tabs act on `app.centre_focus`. Use
`focus_view`, `live_views` (never `pi < 2`), `other_pane` (never
`1 - app.focus`), `pane_at(app, x, y)` and `content_y(app, y, pi)`;
after replacing a buffer's document call `refresh_views_of_buf`.
The tab right-click menu allows targeted tear-off, closing other tabs or tabs
to either side (relative to the clicked tab), and stable case-insensitive sorting
by the displayed title (ignoring the unsaved marker). Bulk closes use
`stash_or_confirm` for each buffer and stop if confirmation is needed.
Application order changes are copied into ImGui before tab layout;
the selected buffer stays selected. Tab menu items use the shared icon renderer.
New Tab uses an addressless, read-only buffer with a blank canvas and URL
bar. Navigation replaces its contents in the same tab. Empty tabs are
transient and omitted from session files.
Opening a link in a new tab records the source buffer as its parent and inserts
the tab after the opener's contiguous descendants. Blank tabs and documents
opened independently start their own trees. The horizontal strip adds space
between adjacent tabs with different roots. Dragging and title sorting change
the strip order while preserving parent links. Removing a buffer reparents its
children to its parent. Tear-off keeps the buffer and its parent links.
Ctrl+Shift+T restores the most recently closed tab at its former position.
Startup restores visible panes first. Remote tabs outside those panes
keep their saved positions and history as placeholders, then load one at
a time when page fetches are idle. Bringing a queued tab into view starts
its request immediately.
A ten-entry in-memory stack keeps closed buffers and their Back / Forward
trails for the current session.
The session stores parent links by saved tab position so they survive new buffer
IDs. Older sessions restore their saved tabs as roots.
**View → Tab Bar** toggles the horizontal strip. **Panels → Vertical Tabs**
opens a native tool window that can dock using the usual title drag.
It groups the same tabs by opener, indents children beneath their parent,
and keeps siblings in tab order. An opener in a floating document is skipped
in favor of its nearest ancestor that is still a tab. Click a row to switch,
its × to close, or the title's + to open a new tab. The session saves the
horizontal strip's visibility and the panel's open state and placement.
The Services menu also uses that renderer, with icon names assigned alongside
service targets in `app_frame.cpp`.
The native document-window
title context menu moves a specific window back (only `WindowRole::Document`).
View source projections (`//source-raw`, `//source-xml`; the old `//source-sexpr` is an alias of `//source-xml`) are
pane-local and switch in View → View Source; see `docs/UI.md`.
SDL `DropText` and URL-shaped `DropFile` events navigate the specific view at
the drop location via `drop_view_at` / `navigate_pane` (`app_events.cpp`), not
the linked Trail click handler. `drop_url.hpp` parses plain text/URI lists;
test URL parsing without SDL in `tests/drop_url_tests.cpp`.
To change a page without moving the view, call `keep_views(app)` before the
change (it needs the old layout): the top line of each view stays on its
spot through the next `layout_panes`. To hold another spot, set
`Pane::view_anchor` from `view_anchor_at` first (pinch: the text between the
fingers). Pane width changes do this by themselves.
An address is `base//selector#selector` (`locator.hpp`, `docs/PASSAGES.md`):
pipes cut the view to a passage, `#` marks one in the whole page. Only the base
reaches buffers, caches and fetches. Use `pane_address` for what a view shows,
never `path_of`, where the address is shown or stored.
Back / Forward follow the tab (`DocBuffer::tab_nav`) or floating document
window (`Pane::nav`). Use `view_nav` to get the trail for a view;
`select_view_tab` restores its current entry when switching tabs.
`open_link_in_tab` copies the opener’s trail through its displayed page
and appends the link destination. The opener retains its Forward entries.
`NavHistoryEntry` keeps the address,
caret, scroll offset, and source link. Record a shown page with
`push_nav_history(app, pi, url)`, step with `nav_step`; `sync_nav_history`
captures the current position and completes deferred restores after fetching.
`forward_nav_link` feeds the non-document link wash in `draw_pane_overlays`;
link clicks pass their `NavLink` through `navigate_to_url`; same-document
anchors use `navigate_anchor_link` so Back can return to the source. `App::visited`
is the session log (History menu, and the History panel's fallback without
a visit log). The History panel (`app_history.cpp`, state `App::history`)
draws the visit log as a trail tree: `visit_page` loads 200 visits at a time
(more on scroll, text filter as SQL `LIKE`). The **This Tab** title button
shows visits owned by the active tab and visits it inherited at opening. Without a visit log,
it shows the active tab’s Back / Forward entries. Session files store the
identity and trail for each tab, including background tabs. Older session
files have view-level trails with no tab ownership, so tabs open with fresh
trails and existing visits remain in the full History panel.
`history_trail.hpp` collapses
in-page runs and assigns lanes, `history_changed` marks it stale (after
`record_visit`, and when `poll_shared_state` sees another process write).
A click steps a view's trail to a visit it still holds (`nav_rewind_to_visit`
matches `NavHistoryEntry::visit`, via `nav_go` like `nav_step`) unless
`ViewSettings::history_rewinds` is off / Shift is held.
The persistent visit
graph is `visits.hpp` (`record_visit` in `push_nav_history`, kinds via
`App::visit_hint` / `NavHistory::pending_kind`, `docs/HISTORY.md` "Done
(step 3)"). `push_nav_history` additionally
adds normal visits to the persistent `minibuf_hist_url` ring
(`record_input_history`, stored with the bookmarks in
`state_dir()/xanatoo.sqlite` by `statedb.hpp`; `poll_shared_state` reloads
both when another process changed them), shared with the
location bar's local suggestions (`urlbar_suggestions.hpp`, also matches open
tabs and bookmarks). The URL bar closes the suggestion list and clears its
selection when the input becomes empty. Only in Trail mode do the
centre panes share one trail instead (`App::split_history`, levels = left
page + pages opened from it on the right): `push_nav_history` forwards to
it and `nav_step` / `can_nav_step` walk it (`app_split.cpp`). The split trail
already tracks scroll/selection; `split_history_forward_href` identifies its
next link for the same wash. Selecting a different tab reseeds the paired
trail from the current tiles.

Buffers are addressed by stable `BufferId` (`DocBuffer::id`, never reused,
`kNoBuffer` = 0): use `buf(app, id)` / `find_buf` / `add_buffer` /
`remove_buffer` / `close_tab` (`app_buffers.cpp`); never index
`app.buffers` directly and never hold a `DocBuffer&` across add/remove.

Status-bar prompts are one `Prompt` record (`App::prompt`: `PromptKind`,
`text`, `text2` for Replace, `node`/`span` for Edit*): open with
`open_prompt` / `open_search`, close with `close_prompt`, Enter dispatches
through `commit_prompt` (`app_prompts.cpp`). At most one prompt is active;
path/URL kinds edit through `App::minibuf` (`prompt_uses_minibuf`), and
`last_search`/`last_replace` keep a closed or displaced find for F3 /
Replace All.

Open, Open in Window, Export, Save PDF, and Insert Image / Figure use
SDL3 file dialogs on desktop. The SDL callback captures the result and
the frame loop applies it through the existing path handlers. SDL2 and
non-desktop ports keep the path prompts. Include / Transclude still
accepts a document address in the status bar.

The kinds Search and Replace are not in the status bar but in the find bar,
an ImGui strip (`Ui::find_bar`) below the text of `app.focus`, which takes its
height from the view (`pane_foot_h` = page bar + find bar, wherever a view's
height is set). It stays open while its page is used: `leave_prompt` (document
click, URL bar, F2) closes other prompts only, and code asking "does the
prompt take keys" uses `prompt_has_keys`, not `prompt_active` — true for a
search only while `Prompt::pending`, until the field is active. The bar's
commands carry "find text\nreplacement" (`find_set_texts`) and keep the keys in
the field. In a document window the bar is drawn with that window
(`float_ui_layer`, `UiPanelLayer::FindBar`).

Unsaved edits are never discarded behind a confirmation: before a buffer
is replaced or dropped call `stash_draft` / `stash_or_confirm` (the latter
only asks when no store is available). After putting a local file into a
buffer call `attach_draft` (`add_buffer` / `show_doc_in_pane` do); after
writing it, `note_saved`. Mark edits with `mark_dirty` / `note_edit`, not
`dirty = true`. Design and roadmap: `docs/REVISIONS.md`.

Keyboard focus: `App::key_focus` (`KeyFocus::Document/Outline/Backlinks/Sitemap/Page/Styles/Panel`).
Panels handle plain keys only while `panel_has_keys(app, …)`; opening or
clicking a panel focuses it, a pane click / jump / unused plain key returns
focus to the document. In Outline, clicking a heading or moving with
Up, Down, Home or End jumps the document while keeping panel focus.
Enter jumps and returns focus to the document. Page Navigation follows
link rows on clicks and when those movement keys select them. Enter
activates the selected row. Never let a panel consume keys just because it is open.
Panels without native keys (embedded ImGui panels, Help, Revisions,
Downloads) share `KeyFocus::Panel` + `App::panel_focus` (`focus_panel`,
`focus_float`). The lit (yellow) title bar is `float_active(app, w)` for
every role — never pass a constant `active` to `draw_floating_frame`.

Context menus: `ui.cpp` keeps page-content actions separate from View/Panels
controls. The location and search fields use the shared icon menu renderer
for their clipboard and field-specific actions. Back/Forward buttons show
nearest-first targets derived from the focused pane's `NavHistory`, or
`SplitHistory` when split browsing. `NavJump` uses a signed offset and must
preserve the destination's caret/scroll state.

Right-clicking a document image or a single gallery image adds actions to
copy its full-resolution pixels, copy its source address, and open the
source in a new gallery tab. Linked images also keep their link actions.
Copy image uses SDL3's bitmap clipboard. The address and tab actions
are available in SDL2 builds as well.

The context menu of a downloaded PDF offers **Save PDF…**. The file
dialog captures the cached file from the clicked view and suggests the
server's filename in Downloads.

Ctrl+A expands the selection in document views and laid-out Context pages.
It starts with the current text block, then uses the smallest enclosing
subtree that adds to the range. Ctrl+Shift+A selects the whole document in
one step.

Primary selection (X11 / Wayland, `sdl::kMiddleClickPastesPrimary`: Linux
desktop, SDL3 or SDL2 >= 2.26):
- Document views: `publish_primary_selection` (in `run_frame` after the
  chrome commands) sends the focused view's selection through
  `sdl::set_primary_selection` when it changed in a frame with user input
  (`App::primary_sel_input`, set in `handle_event`) in the same view and
  buffer. Program-set selections (history restore, passage addresses, undo,
  find) and switching to a view that has one are only recorded in
  `App::primary_sel`; find matches are set from user input, so they call
  `note_program_selection`. Read-only pages publish too.
- Middle-click paste in edit mode reads `sdl::primary_selection()` (the
  clipboard where the platform has no primary selection).
- ImGui text fields (`Ui::publish_text_field_selection`, `prepare_render`):
  the active field's selection from `ImGuiContext::InputTextState`, once
  the left button is up; not password or read-only fields.
- Middle press on an ImGui text field: `Ui::process_event` holds the press
  (and a quick release) until `Ui::resolve_middle_click`, after the next
  frame hovered the widgets; with ImGui's text cursor showing it becomes a
  left click (caret at the click) plus the text via
  `AddInputCharactersUTF8`, which the trickling input queue applies after the
  click. Otherwise the middle press is passed on. The URL bar drops
  `AutoSelectAll` for that activation (`middle_paste_frames_`).

The URL context menu takes keyboard focus while the field draws its inactive
selection with ImGui's normal clipping and horizontal scroll. Escape resumes
the active field with its existing caret and anchor. Clipboard edits and Select
All activate the field and apply their change through its input callback.

On a touchscreen, swipe outward from a left or right dock's panel body to
hide its column. Swipe inward from the corresponding window edge to show it
again. Each column keeps its panels, order and size while hidden. Vertical
drags and two-finger gestures still use the ordinary touch handling. The
visibility change lasts for the current session.

Windows place one way regardless of role: Left / Right docks are side
columns that take width from the panes (stacked by `dock_seq`), Top /
Bottom docks are bands between the columns that take height (side by side
by `dock_seq`; `dock_band_h`, used by `place_centre_rows`). Dock neighbours
are sized by `FloatWindow::dock_share`, not forced equal; shared separator
dragging updates only the adjacent pair. The size per
side is `App::dock_w[4]` (L, R, T, B), set by gutter drags through
`dock_resize_side`. There is no share-of-window cap: a column or band grows
until the panes keep a `64 * ui_scale` px strip (`pane_room`); when both
opposite sides together do not fit, `column_w` / `band_h` shrink both in
proportion (a drag first pins the opposite side at its shown size). The
tile divider likewise keeps each pane at least that strip, not a fixed
15–85 % ratio. A `dock_gap` gutter (scaled,
painted by `draw_dock_gutters` with `theme::draw_grip_dots`; docked panels draw no drop shadow) separates neighbouring docked panels and a
dock from the panes; `dock_column_w` / `dock_band_h` include it, and the
separator / dock-edge grab zones lie in it (plus two pixels of frame), so
they never overlap a panel's or a pane's scrollbar. Side docks put panel
scrollbars at the outer window edge for Fitts' law. Native panels share
`client_scrollbar_rect` between paint, mouse and touch. Embedded ImGui
bodies use `begin_panel` and the vendored `ImGuiWindowFlags_LeftScrollbar`
flag. Full-width child lists also reach the left edge when the body has no
vertical scrollbar. Native panel thumbs come from `theme::vscroll_thumb`
for both paint and drag: a press grabs the thumb where it was hit
(`FloatWindow::scroll_grab`); off the thumb it first centres it there. Max overlays the client area; every
frame has the close box at the title's left edge and the maximize /
restore box at its right edge, the shade box just left of it
(`title_close_zone` / `title_max_zone` / `title_shade_zone`). Shade
(`toggle_shade`, `FloatWindow::shaded`) rolls a *docked* window up to its
title strip so its neighbours get the room (a band whose panels are all
shaded shrinks to one strip and the panes get the height); it is greyed out
for free / maximized windows and cleared when the window leaves its dock.
Test `float_shaded(w)`, never the raw flag; `draw_floating_frame` returns
false for a shaded window — draw no client content then.
Title height is per window: `float_title_h(w)` is `help_title_h` for a
free window (room to grab its top edge) and the slimmer `dock_title_h`
while docked or maximized, where the controls (`float_control_w`) and
title buttons (`title_button_rect`) sit flush in the strip and the title
uses `title_font` (`ui_font_small*`). Never test against `help_title_h`
directly for a window that may be docked; `window_client_rect(w)` follows.
Tabs ↔ windows: dragging a tab past the strip (ImGui, `UiCommand::TearTab`
→ `tear_tab`) opens its buffer in a window (docked at a zone), dragging a
window title onto the strip (`over_tab_strip`) runs `pop_in_soft`; tab
reorder is ImGui's own (ids = buffer ids, `ReorderTabs` copies the order
into `tab_order`). Never add a per-panel
"docked" flag; set `FloatWindow::dock` through the helpers above.
New Document sits at the left of the horizontal strip. The New Tab plus
is a trailing tab-shaped button that stays visible when the tabs overflow.
As space runs out, longer tabs shrink first, down to a scaled 72-pixel
minimum. Scroll arrows appear only if the resulting row still overflows.
Document tabs use dark label ink and share a bottom edge. The active tab
extends one scaled pixel above the baseline, and inactive tabs start one
scaled pixel below it. The active tab has a rounded top and left highlight
below its accent overline and a right shadow. Selected service tabs use
a lighter fill. The bevel and favicon share the scrolling tab area's clip.
Stacking: `float_stack` sorts by layer (docked < free < maximized), then
`FloatWindow::z`, then its fixed role order. A title click raises the
window (`raise_float`; document windows raise on any click via
`raise_soft`), a title middle-click sends it to the back (`lower_float`),
right-click on the title is reserved for a context menu. A window shown in
`draw_floats` that was not shown in the pass before (just opened) is raised.
See `docs/BRAINSTORM.md` "Views, arrangement, documents" for where tabs /
split / windows are heading.

Rules: no behaviour on data types; new modal state goes in `App` (group
related fields into small records like `FloatPanel` / `Hover`); menus,
toolbar and shortcuts should route through `execute_command`.
While browsing (read-only or locked, `browsing(app)`) `execute_command`
refuses only the commands
`changes_document` (`app_frame.cpp`) lists; add a new command there when it
edits the document or replaces its buffer, and nowhere for views / toggles.
Mouse cursor feedback must use the same hit-test as the action it advertises:
`float_resize_at` for native panel edges, `dock_split_at` for neighbours,
and `tile_divider_rect` for centre tiles. Free windows resize from all four
edges and corners (`FloatWindow::resize` bits 1/2/4/8 = right/bottom/left/
top); corners use the per-corner cursors `cursor_size_nw/ne/sw/se`, since
some cursor themes draw SDL's double-arrow diagonal as a single corner.
ImGui handles its own controls;
the SDL2 backend must detect external cursor changes to restore its I-beam.
`set_cursor` also hands the canvas cursor to ImGui (`Ui::claim_cursor`):
both backends apply ImGui's cursor in their next `new_frame`, and without
the claim they reset a canvas hand to an arrow every frame, which flickers
while frames redraw continuously (e.g. hovering a quote).
Gutters and scrollbars also light under the pointer (same hit-tests,
gated by `canvas_hover`: no ImGui window, popup or other float in front,
pointer not away) and turn accent blue while dragged — `theme::Hot`,
`draw_gutter`, `draw_scroll_thumb(…, pressed, hover)`. The pressed look
comes from the drag state (`dragging_split`, `dock_split_drag`, a docked
window's `resize`, `dragging_scroll`, `scroll_dragging`), so a touch shows
which strip it grabbed even though touches leave no hover behind.
Panel titles darken and take a sunken bevel while held for dragging.
Pending touches on content and ImGui controls show a progress bar offset
from the contact. It follows the context-menu timer and disappears on
release, when motion cancels the hold, or when the menu gesture fires.
