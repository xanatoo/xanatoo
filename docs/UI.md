<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com> -->

# UI structure and drawing order

The desktop UI takes the classic GNOME 2 HIG as a useful baseline: predictable
window controls, visible interactive targets, restrained chrome, and consistent
keyboard/mouse behaviour. The dock/tile model is intentionally more flexible.

## Navigation and layout menus

- **View → View Source** switches the current pane between the rendered
  document, raw source, S-expression and XML. It reuses the loaded document
  (including database-native and unsaved documents) instead of opening a
  second page or fetching the remote URL again. Source representations are
  read-only pane-local projections, not edits to the underlying buffer.
- **Layout** holds page columns, collection layout, split/tile arrangement,
  margins, text width, line spacing, justification and style sheets. **View**
  holds reading modes, document/source representations and viewing overlays;
  edit-only **Format** remains reserved for document formatting actions.
- **View → Headings on the scrollbar** toggles the scrollbar ticks for the
  page's headings (`ViewSettings::heading_ticks`); bookmark, annotation and
  search ticks stay.
- Right-click a document **tab** to select it, move it into a document window
  or close it. Right-click a document **window title** to move that *specific*
  window back to a tab, even when another window has focus. Moving a view
  preserves its own navigation history, source projection, scroll and selection.
  Utility panels (Bookmarks, Styles, etc.) cannot be converted into tabs.
- The centre tile-divider badge/right-click menu offers **Swap Panes** and
  **Join Left/Right** (or **Top/Bottom** in stacked mode). Join collapses the
  split while retaining the selected side's view and history. A Trail hover
  preview is reverted first; pending fetches for the discarded side stop.
- A link's right-click menu has **Open Link in Trail View**: it switches the
  centre to Trail (resuming a matching suspended trail) and opens the link in
  the second tile, the page holding the link staying in the first. From the
  second Trail tile it acts like a click; from the other independent tile or a
  document window that page moves to the first tile first.

## Editable documents

There is no global edit mode. A document is editable or not by what it is:
local files, `db:` documents and untitled buffers are; foreign content (web
pages, HTML, pandoc imports, cached pages), source views, invalid XML and
binary files are not (`pane_read_only`).

- Every view showing an editable document has an **edit strip** on top of
  its page, inside the tile or document window (`Pane::head_h`, reserved by
  `place_centre_rows` / the document-window layout from `edit_bar_h`). The
  main toolbar and tab strip never change with the page, so nothing above
  the page jumps when an editable document opens; the strip itself marks
  the page as editable.
- Strip groups (`edit_bar.hpp`): lock · save · undo · redo | cut · copy ·
  paste | bold · italic · link · more formatting | structure | lists |
  insert. A group wraps to the next row whole; the app and the ImGui strip
  compute the rows the same way (`edit_bar_rows`).
- **Lock** (the padlock, F2, View → Lock Document) makes an editable
  document read-only without taking its strip away: the tools grey out, the
  page does not move. The lock belongs to the buffer and its current path
  (`DocBuffer::lock_set` / `lock_path` / `lock_choice`); following a link
  to another page falls back to the default. Installed data (`data_dir()`:
  welcome, help, examples) starts locked, everything else unlocked.
- On the tab strip, the tab of a document that takes edits now (editable
  and unlocked, `buffer_takes_edits`) is green, with a green overline when
  selected; read-only and locked documents keep the grey tab and yellow
  overline (`Palette::doc_tab_edit*`, `UiState::tab_editable`).
- Centre tiles' strips are the `UiPanelLayer::EditBar` layer (under every
  floating window). A document window shows its strip's tools only while it
  is focused (they draw in its `FindBar` layer as `##find_bar/edit_strip`);
  unfocused it paints a plain band of the same height. A tool on another
  view's strip focuses that view first (`UiFrameResult::edit_bar_view`).
- Service pages have a **service strip** in the same place instead (see
  "Service strips"); a page never has both, since service pages are
  read-only.
- **New document** (Ctrl+N, File → New Document, the page button beside
  "+" on the tab strip) opens a blank document in a **new tab**, already
  stored in the local database (`db:`, listed under Local documents) with
  its title selected to type over; it never replaces the page in view.
  Right-clicking "+" offers New Tab, New Document, Open File… and Local
  Documents.


## Service strips

Pages of a native service (Hacker News, Lobsters, Discourse, 4chan, Stack
Exchange, Bluesky, Flarum, LessWrong, Digg, Usenet) have a **service strip**
on top of the page, where an editable document has its edit strip
(`Pane::head_h` with `Pane::head_service`, from `pane_head_h`). It replaces
the "Hacker News: Top · New · Best · …" lines the generated pages used to
start with (`NodeKind::Chrome` navigation regions): those pages carry no
navigation rows any more.

- `service_bar(address)` (`service_bar.hpp`, pure) derives the strip from
  the address alone, so it is there while the page loads and stays the same
  across all pages of the service. Each service describes its own:
  `hn_service_bar`, `forum_service_bar` (with the per-site
  `forum::*_service_rows`), `nntp_service_bar`. A forum's own web pages
  (`https://lobste.rs/…`) get its strip as well.
- Contents: the favicon and name (opens the front page, `home`), a
  separator, the sections with the page's own highlighted (`current`);
  further rows start with their label (Digg's "Sort" and "From"). Links
  that do not fit go into a "»" menu, so the reserved height
  (`service_bar_height`: one row per `ServiceRow`) always fits.
- Search: the engine that claims the address (`engine_for_address` over the
  enabled engines) gets a field on the right of the first row; Enter opens
  `search:<engine>:<query>`. A search's result page keeps its service's
  strip (`service_address`: the search's target, or for JSON engines the
  first service address among the engine's `sites`), with no section
  marked.
- Colour: `ServiceBar::color` (a service's own accent, else
  `service_color_for(id)`, a hue from the id) tints the strip, draws its
  bottom line and tints the service's tabs (`UiState::tab_service_color`;
  the green editable tab wins).
- Layers as for edit strips: centre tiles draw `##edit_bar/svc<view>`
  (EditBar), a focused document window `##find_bar/service_strip`; an
  unfocused window paints a tinted band. A link or search on another view's
  strip focuses that view first (`UiFrameResult::edit_bar_view`).
- Old snapshots taken before the strip still show their in-page rows.

## Toolbar states

- Ordinary toolbar actions have no button frame until hover; they rise on
  hover and sink only while pressed. Toggles that retain the same graphic
  stay recessed while on (native view, outline, history, downloads, the
  edit strip's lock and the contextual reader/outline/navigation/site-map
  buttons).
- Location-field buttons: **Heading outline** (`outline` icon) whenever the
  page has headings, **Page Navigation** (`navigation` icon) for `<nav>` /
  sidebar regions or page links. Both may show side by side: pages differ in
  how much the two overlap, so neither hides the other yet.
- The document tab's right-click menu restores normal menu item spacing and
  window padding instead of inheriting the tightly packed tab-strip style.
- **Split Panes** becomes **Join Panes** with a different icon while the
  independent two-pane arrangement is active, without remaining recessed.
  That toolbar action keeps the left/top pane when joining. For an explicit
  choice use the divider's **Join Left/Right** (or Top/Bottom) context menu.
- The bookmark button uses an outlined ribbon when the current address is
  unbookmarked, and a filled ribbon when bookmarked. Both states use the
  same flat/hover face. A bookmark on *another passage* of the current page
  does not change the button. Reload uses an opposing-arrows icon, distinct
  from Stop.

## URL bar suggestions

- Focusing the location field (including **Ctrl+L**) opens a list of up to
  eight matches from open tabs, bookmarks and recently visited addresses.
  Selecting all the location text presents recent suggestions; typing filters
  addresses and bookmark/tab titles case-insensitively. Matching and ranking
  are local; typing does not contact a search engine.
- **Up/Down** chooses a suggestion, **Enter** opens it (or switches to its
  existing tab), and clicking a result does the same. Without a selected
  suggestion, Enter navigates to the text as entered, completed: **Escape** or **Tab**
  dismisses the list without navigating. A new edit or focus reopens it.
- Typed text without a scheme is a web address: `example.com/x` and
  `localhost:8080` open as `https://…`. A path starting with `/`, `./`,
  `../` or `~/` (also `.` / `..`, Windows `C:\`) opens as a `file://` URL,
  relative ones against the working directory; a `#` after it starts the
  selectors as usual. A bare name that names an existing file or a bundled
  example (`notes.md`, `welcome.md`) still opens that file. Keyword
  searches (`w xanadu`, `? query`), `#…` and other text with blanks stay as
  typed (`complete_location`, pure part `complete_typed_address` in
  `file_url.hpp`).
- Visited URLs are added to the existing `minibuf-url.history` ring in the
  state directory. It is bounded to 200 distinct recent addresses and obeys
  `--no-save`; the per-pane Back/Forward history remains separate.

## External URL drops

- Dropping a URL from another application navigates the view directly under
  the pointer. With two centre panes, either side can receive the drop;
  linked Trail navigation does not redirect it to the other pane. A document
  floating window also accepts drops, while utility panels block drops onto
  pages behind them.
- SDL text drops accept plain URLs and the first URL in a `text/uri-list`
  (ignoring blank/comment lines). A `file://` URL to a local path is decoded
  and opened as a document. Regular file drops still open documents in
  windows, and dropping images onto a document still inserts at its caret.
- SDL3 supplies exact coordinates for the drop. SDL2's drop event lacks them,
  so the compatibility path uses the mouse position at the time of dropping.

## Bookmarks

- Bookmarks live in folders: `Bookmark::folder` is a `/`-separated path
  ("Video/YouTube", "" = top level). Folders are listed explicitly too, so
  empty ones survive (`bookmark_folders` table, `#folder<TAB>path` lines
  in the exported text file); a bookmark's folder and its ancestors are
  always in that list (`bookmark_folder_list`, util.hpp). Folders sort
  part by part, case-insensitively.
- Bookmarks menu: folders as submenus, then the loose bookmarks (newest
  40 per level).
- Bookmarks panel: Bookmark Page / New Folder / filter on top, then a
  folder tree with counts. A click on a bookmark opens it (middle-click:
  new tab); a click on a folder makes it the target of Bookmark Page and
  New Folder (click again, or the empty space below the rows, for the top
  level). Drag bookmarks and folders onto a folder (or the empty space)
  to move them. Right-click: Open in New Tab, Rename… (inline field; a
  folder name with `/` nests), Move To ▸, Copy Address, Remove; on
  folders Bookmark Page Here, New Subfolder…, Remove Folder (its contents
  move up one level, nothing is deleted). The filter lists matches flat,
  each with its folder. No rows appear or vanish while dragging, so the
  list never jumps under the pointer.
- The margin-icon menu of a passage bookmark has Move To ▸ as well.
- Commands: `MoveBookmark`, `RetitleBookmark`, `NewBookmarkFolder`,
  `MoveBookmarkFolder`, `RemoveBookmarkFolder` (two-part args joined by
  `\n`); systems `move_bookmark`, `retitle_bookmark`,
  `create_bookmark_folder`, `move_bookmark_folder`, `remove_bookmark_folder`
  (app_nav.cpp), stored in one transaction by `store_bookmark_changes`.
- Next (docs/BRAINSTORM.md): a site button in the URL bar with that
  site's bookmarks and subscriptions, and an "On this site" group in the
  menus.

## Native panels and ImGui

- A panel that can be docked, detached, resized, or maximized is a `FloatWindow`.
  `float_stack` drives both painting and hit-testing; no separate ImGui title
  bar or independent ImGui placement is allowed for that panel.
- Rich forms (Reply, Bookmarks, Annotations, Nostr, Lemmy) use borderless ImGui
  content in the native panel client rect. All title bars, close/maximize
  buttons, docking and resizing remain native.
- Preferences is an application settings **dialog**, not a dockable work panel.
  Main menus, toolbars, context menus, tooltips, and the virtual keyboard are
  also ImGui chrome rather than `FloatWindow`s.
- Native mouse/touch routing consults `Ui::captures_point` with the frontmost
  `FloatWindow` at the pointer. A hidden embedded widget cannot intercept a
  frame/title click on a native-only panel or on another embedded panel.
  Popups and dialogs remain above all native frames.

## Help panel

**Shift+F1** (Panels / Help → Help Panel) opens a dockable panel
(`WindowRole::HoverHelp`, docked right by default) that explains the
element under the pointer; F1 stays the keyboard reference. Inside its own
client area the text holds still so it can be read and scrolled.

- Texts: `src/ui_help.cpp`, one table of topics (Markdown body, `|`
  separated lower-case keys). Pure and unit-tested
  (`tests/ui_help_tests.cpp`, which also lists the keys the app derives
  itself — add new `@` keys there).
- ImGui widgets: built with `IMGUI_ENABLE_TEST_ENGINE`; `ui.cpp` defines
  ImGui's item hooks (no test engine is linked). While the panel is open
  (`Ui::set_hover_tracking`) `ImGuiTestEngineHook_ItemInfo` keeps the
  hovered widget's label; `ui_help_keys` turns it into keys: the `##id`
  (trailing digits dropped), the visible text, the text without a trailing
  `(…)` remark. Icon buttons (`tool_button`, `tool_menu_begin`) and tabs
  (their hook fires before `ItemAdd`) note their id with
  `note_hover_help`. A new menu item needs only a topic whose key is its
  label. Without a topic the panel shows the label and the tooltip.
- Canvas chrome: `hover_help_key_at` (`app_hoverhelp.cpp`) gives `@` keys
  from the same hit-tests as the clicks / cursor (title zones, resize
  edges, dock separators, status line, tile divider, scrollbar, gallery and
  page chrome via `gallery_help_key` / `page_help_key`, `App::hover` for
  page content); panel bodies are `@panel:<session name>`.

## Paint order

1. Document panes, including overlays, footnotes, scrollbars and page chrome.
2. Centre tile split chrome, then the find bar of a focused centre view (of a
   document window: right after that window's page).
3. All native floating/docked frames and client content, back to front. Each
   embedded ImGui form is submitted immediately after its owning native frame;
   nested ImGui child windows remain part of that form. Resize grips between
   docked neighbours are drawn after the panels.
4. Fixed status line (on top of a floating frame that extends too far down),
   and transient docking preview / native hover tooltip.
5. ImGui menu bar, toolbars, popups, settings dialog, and other non-panel
   draw lists, rendered in `Ui::end_frame` above the SDL chrome.

`Ui::prepare_render` finalizes ImGui draw lists after `run_chrome`, before
`draw_floats`. The `ui_panel_layer` classifier recognizes the embedded
panel roots (and the find bar) and their child windows. Tooltips and popups remain foreground
ImGui windows. Each modern SDL2/SDL3 render pass receives only its selected
draw lists, preserving their native FloatWindow z-order. The old SDL2
per-triangle fallback, which caches a whole UI frame as one texture, retains
single-pass rendering for compatibility; its embedded controls can still
overlap foreground native frames.

## Pointer ownership and input stacking

- Every UI frame synchronizes the embedded ImGui root windows **and their
  children** with native `float_stack` back-to-front order. Non-panel ImGui
  chrome retains the foreground layer.
- The frontmost native role under the pointer is compared with the ImGui
  window from the previous frame's hit-test. When they disagree, an obscured
  embedded editor cannot receive mouse-down, motion, or wheel events. The
  next ImGui frame masks that mouse position as unavailable to suppress
  hover/tooltips over the hidden form. Mouse-up still reaches ImGui so an
  interrupted widget drag cannot leave its button stuck.
- Touch and native mouse cursor selection use the same ownership check as
  mouse presses. A gamepad-driven pointer is likewise masked while its native
  foreground window hides a form.
- The gamepad's software cursor hides after `kStickCursorIdleMs` (3 s)
  without gamepad input (any button, stick past the deadzone, trigger) and
  reappears on the next input (`note_stick_use`, `stick_cursor_visible`).

The vendored-ImGui regression exercises two overlapping native panel bodies,
each with a child, and swaps their stacking order without focusing them. It
also verifies that standalone chrome windows stay out of the panel layers.
Desktop checks should still cover actual mouse down/up, wheel and cursor
transitions while dragging floating windows over Reply/Bookmarks/Nostr/Lemmy, plus
the SDL2 triangle-renderer fallback (which still uses one draw pass).

## Resize affordances

- Tile split: horizontal resize pointer for side-by-side panes, vertical for
  stacked panes. The Compare overview remains a scroll handle, not a resize
  handle; the divider badge opens the tile menu.
- Dock edge: horizontal or vertical pointer at the border facing the document.
- Two panels sharing a dock: a small dotted grip at the internal separator.
  Dragging changes the two neighbours' relative lengths; other panels and
  the outer dock width/height do not change. The shares are saved in the
  session and scale with the main window.
- Shaded (rolled-up) docked panel: no resize edges and no separator grips
  next to it; a side column gives its height to the open panels, a band
  gives its width (`dock_slices_shaded`). A band of only shaded panels is a
  single title strip high and cannot be resized until one is rolled out.
- Undocked window: its right/bottom edges and lower-right corner show the
  matching resize pointer. Resizing keeps the pointer shape until release.
- ImGui text inputs and other widgets retain their own I-beam/hand pointers;
  native pointers are never forced on top of an ImGui-owned hit area.

The pure `dock_slices_tests` exercise distribution, resizing, minimum lengths
and invalid persisted weight fallback; `ui_panel_layers_tests` verifies exact
embedded-window/child classification and excludes popups and chrome. A second
test, `ui_panel_imgui_tests`, produces actual vendored ImGui draw lists and
checks parent, child and dialog classifications along with overlapping window
input order. Interactive SDL/ImGui overlap testing still needs a desktop build.
