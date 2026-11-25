# Glossary

Phrases and concepts used in xanatoo, its documentation and its code. Each
entry is short; follow the pointer in parentheses for the full story.
Identifiers in `code` font are names in the source.

Sections:

1. [Ideas and inspirations](#1-ideas-and-inspirations)
2. [Document model](#2-document-model)
3. [Addresses and passages](#3-addresses-and-passages)
4. [Storage, identity and history](#4-storage-identity-and-history)
5. [Layout, reading and style](#5-layout-reading-and-style)
6. [User interface](#6-user-interface)
7. [Networking and protocols](#7-networking-and-protocols)
8. [Services](#8-services)
9. [File formats and media](#9-file-formats-and-media)
10. [Platforms, build and directories](#10-platforms-build-and-directories)
11. [Code architecture](#11-code-architecture)

---

## 1. Ideas and inspirations

**Addressable content** — The principle that any part of a document (a
section, a sentence, a region of an image) can be named and linked, not only
the whole file. The basis of passages and locators (§3).

**Backlinks** — The reverse direction of a link: which places point *here*.
Shown in the Backlinks panel; a Xanadu idea of bidirectional links.

**Canon Cat** — Jef Raskin's 1987 text appliance. It never asked the user to
save: the text simply was. xanatoo follows it with autosaved, versioned
database documents and export as an explicit action
(`docs/BRAINSTORM.md` §8).

**Composition engine** — What xanatoo calls itself: not a web browser or
word processor, but a realtime system where documents are laid out, read,
edited and rearranged in one place (`PROPOSAL.md`).

**HyperCard** — Apple's 1987 hypermedia environment, where reading and
authoring lived in the same tool. An inspiration for "reading and editing
coexist".

**Hypertext** — Text with links that can be explored non-linearly. The term
was coined by Ted Nelson in 1965.

**Parallel documents** — Two related documents or versions shown side by
side, with their relation visible. In xanatoo: split tiles, Trail and Compare
(§6).

**Stretchtext** — Ted Nelson's idea of text that expands and contracts in
place: a short summary unfolds into a longer explanation while the rest of the
page reflows. A `Stretch` node in the document model (§2).

**Ted Nelson** — Hypertext pioneer, author of Project Xanadu, coiner of
"hypertext", "transclusion" and "stretchtext".

**Transclusion** — Including a passage of another document *by reference*,
so the original stays the source and the inclusion follows it. In xanatoo:
the `Transclude` node (`:::include`), cuts and embeds (§3), and quote links.

**Typed link** — A link that names its relationship (`rel`: `glossary`,
`source`, `contra`, `annotates`, …) and not only its target
(`FORMAT.md`, `docs/BRAINSTORM.md` §6).

**Web Annotation / text fragments** — W3C standards for pointing into a web
page by quoted text (`#:~:text=…`). xanatoo's `text:` selector uses the same
grammar so that addresses survive edits and re-fetches.

**Xanadu** — Ted Nelson's Project Xanadu (from 1960): a design for a
universal hypertext system with permanent addresses, transclusion, versioning
and two-way links. xanatoo borrows these ideas locally and is explicitly
*not* a full Xanadu implementation.

---

## 2. Document model

See `FORMAT.md` and `docs/XML.md`.

**Article** — A self-contained composition inside a document (a blog post, a
story). Never created automatically; the author opts in (`:::article`).

**Aside** — A box of text beside the main flow (sidebar, pull quote, HTML
`<aside>`).

**Biblio / bibliography entry** — A bibliography block and its references;
a `Cite` span points at an entry by key.

**Block** — A unit of the document flow: paragraph, list, table, code block,
figure, quote, … As opposed to an *inline span* inside a block.

**Caption** — The text under a figure, part of the `Figure` node.

**Chrome (page chrome)** — The non-content parts of a web page: navigation,
sidebars, banners, footers, cookie and share boxes. Kept in the document as
`NodeKind::Chrome` with a role (`navigation`, `sidebar`, `related`, …) so
reader mode can hide it and the Page Navigation panel can list it. Not to be
confused with *UI chrome* (§6).

**Collection** — An ordered set of items (images, figures, documents) shown
as cards. A "gallery" is a collection of images in a grid. Layout, card size
and sort are view state, not content (`docs/BRAINSTORM.md` §4).

**Columns / Column** — Optional semantic grouping of parallel passages
("these belong side by side"), not a print layout.

**Definition list (DefList, Term, Definition)** — Glossary-style list of
terms and their explanations.

**Document** — The root node of a semantic tree, with metadata such as title,
language and author.

**Figure** — Media and its caption as one unit.

**Folding** — Hiding the body of a section, a list item with a nested list,
or the full text of a stretchtext. Fold state is view state (§5) unless
changed in edit mode.

**Footnote / FootnoteRef** — A note body and the mark in the text that refers
to it by key (`text[^1]` in Markdown).

**Header** — The unranked title of a section or article. Users see it spelled
`heading` (`(heading …)`, `:::heading`, XML `<h>`). Internally it is still
`NodeKind::Header`.

**Heading (ranked, legacy)** — An old-style heading with a level 1–6
(`<h1>`…`<h6>`, `NodeKind::Heading`). Imported headings are normalized into
nested sections with a `Header` child; the level is kept only as a soft hint.

**Inline span** — A run of text with one kind: plain text, emphasis, strong,
strikethrough, link, code, citation, footnote mark, inline image or math.
Nested emphasis is expressed as `marks` on a span, not as nested spans.

**Inline object** — An inline span that is one caret position (U+FFFC): an
inline image or a TeX formula.

**Label** — The human-stable name of a node (`Node.label`, Org `CUSTOM_ID`,
`id=`). `#label` and `id:label` resolve to it. The preferred durable address.

**Message / Thread** — A post or comment, and the discussion container
holding messages. Forums, feeds, Hacker News, Reddit, Lemmy, Nostr, Usenet
and Matrix are all shown as threads of messages.

**Node** — One element of the semantic tree. It has a kind (`NodeKind`), a
session-local `NodeId`, an optional label, children and inline spans.

**NodeId** — The runtime handle of a node (an arena index). It lasts for one
session only and is never used in durable addresses.

**Note** — An authorial aside or callout, semantic rather than a layout box.

**Page (node)** — One page of a fixed-layout document (PDF, DjVu, comic),
drawn from image tiles.

**Section** — A thematic group of blocks that may nest. The hierarchy of a
document lives in its section tree, not in heading levels.

**Section tree** — The nested `Section` / `Article` structure built from
Markdown `#` headings or Org stars. The outline is a projection of it.

**Semantic tree** — The common document model: a tree of *meaning*, not a
page description. Every format (Markdown, Org, XML, HTML, services) is turned
into it; layout and rendering are derived from it.

**Stretch / Summary** — A stretchtext block. In the classic form a label sits
above the expandable children; in the two-body form a `Summary` child is
shown while folded and is replaced by the full text when opened.

**Table / TableRow / TableCell** — A semantic grid; header cells have level 1
(`docs/TABLES.md`).

**Transclude (node)** — `:::include path.md#Heading`: a live embedding of a
labelled passage, optionally narrowed to an excerpt.

---

## 3. Addresses and passages

See `docs/PASSAGES.md`.

**Address** — What the location field shows and what history and bookmarks
store: `base//selector#selector`. A plain string, usable wherever a URL is.

**Base** — The part of an address before any selector. Only the base reaches
buffers, caches and network fetches; selectors are view state.

**Cut** — A view showing a passage *as* the document (`book.md//id:intro`).
Edits in a cut are ordinary edits of the whole document.

**Embed** — A cut placed inside another document: a `Transclude` node or an
image whose `src` is a locator.

**Extent** — A selector resolved against one loaded document (node, carets,
rectangle). Session data, rebuilt when the document changes.

**Fragment (`#`)** — The part after `#`. It *points*: the whole document is
shown, scrolled to the passage, with the passage marked. At most one, always
last. A bare `#name` is a label.

**Gallery view (`//gallery`)** — A view pipe that shows the images of a page
(or a passage) as a grid of cards; `//gallery//item:N` shows one image on its
own with zoom, pan and stepping.

**Locator** — The parsed form of an address: base, pipes and an optional
point (`locator.hpp`).

**Mark** — A passage highlighted over the whole document, e.g. the target of
a `#` address. Drawn in the style's `mark` colour.

**Native ref / service ref** — A short address that opens a service in its
native document view: `hn:item?id=1`, `reddit:r/linux/`,
`lemmy:lemmy.world/c/technology`, `wikipedia:Ted_Nelson`, `nostr:npub1…`.
The part after the prefix mirrors the web path.

**Passage** — A part of a document: a point, a text range, a node, a section,
or a rectangle of an image.

**Pipe (`//`)** — A selector segment that *cuts*: the result is the passage as
a document of its own. Pipes apply left to right and are never sent to a
server. Only known selector names count, so `https://web.archive.org/…//…`
stays a URL.

**Resolve: Exact / Moved / Lost** — The outcome of finding a stored passage
again after its document changed. A lost bookmark is never deleted; its
stored quote is shown.

**Selector** — How a passage is named, durably: `id:LABEL`, `path:0.2.1`
(child index path), `text:prefix-,start,end,-suffix`, `rect:x,y,w,h`
(fractions of an image), `at:x,y`, `item:N`, later `rev:ID`.

**Source view (`//source-raw`, `//source-xml`)** — View transformations
showing a document's original bytes, or its current semantic tree as XML.
Read-only and pane-local.

**Up** — Moving one level out: the last pipe turns into a `#` mark, then the
mark is dropped, then the containing directory (`page_up_url`).

**Special address schemes** used inside xanatoo:

| Scheme | Meaning |
|--------|---------|
| `file:///abs/path` | A local file (percent-encoded so a name never reads as a selector) |
| `db:doc:<hex>` | A database-native document in the local revision store |
| `db-original:doc:<hex>` | The read-only imported original of such a document |
| `draft:doc:<hex>`, `drafts:` | An untitled draft; the list of all drafts |
| `documents:` | The list of local database documents |
| `snap:ID/URL` | A stored snapshot of a page as it was seen |
| `search:<engine>:<query>` | A search through a configured engine |
| `sitemap:host` | A site's map as a page |
| `feed:` | A feed shown as a thread |
| `wayback:` | A Wayback Machine capture |
| `about:` | The About page (build features, libraries) |
| `external:https://…` | A link that opens in the system browser |

---

## 4. Storage, identity and history

See `docs/REVISIONS.md` and `docs/HISTORY.md`.

**Annotation layer** — An independently versioned set of highlights and
notes kept beside a document, never inside it.

**Autosave** — Edits are written as drafts after a short delay
(`kDraftDelayMs`) and at once when leaving a buffer. There is never a
"discard unsaved changes?" question.

**Bookmark** — A stored locator plus a record (title, quote, folder,
revision, date). Can point at a page, a passage or a reading position, and
lives in folders. Shown in the page as a wash and a margin ribbon.

**Clone / Edit local copy** — Importing foreign content into the database so
it becomes an editable document that remembers its origin.

**Content id (`sha256:<hex>`)** — The hash of exact bytes. Self-verifying;
the key of the content-addressed object store.

**Content-addressed store** — Storage where an object's name is the hash of
its bytes (`objects` table), so identical content is stored once and cannot
change under its name.

**Database-native document** — A document whose home is the local SQLite
store (`db:doc:…`), not a file. Files are for export.

**Doc key** — The stable identity of a logical document: a random 256-bit
`doc:<hex>`, unchanged across title and content changes.

**Document origin** — The immutable provenance of an import: the source
alias, the first parsed revision and the raw source bytes.

**Draft** — An autosaved, not yet saved state of a document in the revision
store. `attach_draft` puts it back when the file is loaded again.

**Export** — Writing a copy to a file (`.md`, `.xml`, …). It does not change
the document's identity or dirty state, and never overwrites an existing
file silently.

**Foreign content** — Anything xanatoo did not author: web pages, feeds,
service threads, imports, cached pages. Always read-only ("browsed").

**Head** — The mutable pointer to a document's latest revision (and its
draft, saved base, working and published revisions).

**Observation** — The record of *which* content a visit saw (ETag, content
hashes), so it can later be compared with a snapshot or the Wayback Machine.

**Publish locally** — Recording the working content in a separate
publication history. It uploads nothing (`docs/PUBLISHING.md`).

**Revision** — One immutable version record: doc key, parent, content, kind
(`file`, `draft`, `save`, `working`, `published`) and time. Its id is the
content id of the record.

**Revision store (revstore)** — `revisions.sqlite` in the state directory:
objects, revisions, heads, document heads and origins.

**Session** — The live state that is restored on the next start: open tabs,
views, windows, docks, scroll positions. `--new-session` skips it.

**Side store (sidestore)** — SQLite storage for view state kept outside the
documents, such as folds.

**Snapshot** — A permanent copy of a page as the user saw it, so history and
bookmarks show what was there even after the live page changed or vanished.

**Visit / visit graph** — Every followed link stored as an edge between two
visits, with its kind. The History panel draws it as a trail tree.

**Working / published** — Two separate revision chains of a database
document: *working* advances with autosaves, *published* only on Publish.

**Workspace** — A planned named snapshot of buffers, tiling, view state and
focus, distinct from the automatic session.

---

## 5. Layout, reading and style

**Builtin style** — The style sheet compiled into the program
(`builtin_style()`); `styles/default.xss` mirrors it, and every sheet starts
from it.

**DocStyle / role** — The resolved style of a document, organised by
semantic *roles* (`heading`, `quote`, `mark`, `bookmark`, `gallery`, …)
rather than CSS selectors. Document colours and spacing come only from here.

**Fixed-layout document** — PDF, DjVu, CBZ / CBR: pages with fixed sizes,
drawn from image tiles rather than reflowed (`docs/FIXED_LAYOUT.md`).

**Footnote overlay** — A strip at the bottom of a view showing the notes of
the footnote marks currently on screen.

**Height hints / height cache** — Remembered box heights that let layout
skip off-screen blocks without measuring them.

**Layout engine** — Turns the semantic tree plus a style into positioned
glyphs, images and boxes (`LayoutEngine`). Rendering only draws its result.

**Layout virtualization** — Laying out only the part of a long document near
the viewport (the *materialised band*); lines and blocks outside are measured
or estimated but emit no glyphs.

**Line table** — A cached list of line starts of a long plain-text block, so
re-layout breaks only the lines near the viewport.

**Measure** — The width of the text column.

**Outline (Heading Outline)** — The tree of section headings of the focused
page, with twisties to fold; a projection of the section tree.

**Paged reading** — Reading a long text as ebook-style pages instead of
scrolling, one *spread* (1–3 columns side by side) at a time, with a page bar
(F8, `docs/PAGED.md`). View state, never in the address.

**Reader mode** — Hiding the page chrome of a web page to show only its
content (F9). View state, not a selector.

**Section Focus** — Showing only one section or article in a view while the
outline stays complete; today expressed as a cut.

**Site hint sheet (`.xsite`)** — An S-expression file telling the page
reader which parts of a particular site are content, comments or chrome
(`docs/SITEHINTS.md`). It changes structure, never looks.

**Site look** — A style sheet assigned to a domain or service.

**Spread** — The set of 1–3 page columns visible at once in paged reading.

**Structure highlight** — Colour bars and kind tags beside each block,
showing the document structure (Ctrl+Shift+L).

**Style sheet (`.xss`)** — An S-expression file that styles semantic roles:
fonts, colours, spacing, margins, side boxes, paper. Supports `(inherit "x")`
and late-bound `(define …)` (`docs/STYLESHEETS.md`).

**View state** — Anything that changes how a document is shown but not the
document: scroll, folds, reader mode, paged mode, zoom, gallery zoom, cuts.
Kept per view, never written into the document.

**ViewSettings / `view.conf`** — Reader preferences (line spacing, margins,
measure, defaults for reader and paged mode, …) in the config directory.

---

## 6. User interface

See `docs/UI.md`, `docs/TILING.md` and `docs/dev/APP.md`.

### Windows, views and arrangement

**Buffer** — An open document in memory (`DocBuffer`), addressed by a stable
`BufferId`. A tab is a buffer; several views can show the same buffer.

**Centre** — The main area of the window, holding one or two tiles; the
area left over by docked panels.

**Document window** — A floating, dockable window that shows a document
view of its own, with its own history and scroll. Tabs can be torn off into
document windows and dropped back onto the tab strip.

**Dock** — A place a floating window can be attached: a Left or Right side
*column* (takes width from the centre) or a Top or Bottom *band* (takes
height). Panels sharing a dock are sized by relative *shares* and separated
by a draggable gutter.

**Floating window (FloatWindow)** — The one window kind used for every
panel and document window. It can be free, docked, maximized or shaded and
has native title bar controls: close (left), shade and maximize (right).

**Gutter** — The thin draggable strip between docked panels, between a dock
and the centre, or between tiles. Lights up under the pointer.

**Maximize** — A floating window covering the whole client area.

**Pane / view** — A document view: its own rectangle, scroll position,
caret, selection, passage and Back/Forward history (`Pane`). Centre tiles
and document windows are views.

**Panel** — A floating window with a tool role rather than a document. All
are in the **Panels** menu; see "Panels in brief" below.

**Shade** — Rolling a docked window up to its title strip so its neighbours
get the room.

**Split** — Dividing the centre into two tiles, side by side (columns) or
stacked (rows). Ctrl+\\ toggles it; **Join** collapses it again.

**Tab / tab strip** — The row of open buffers above the centre. A green tab
is an editable, unlocked document; grey with a yellow overline is read-only
or locked; service tabs take the service's colour.

**Tear off** — Dragging a tab out of the strip into a document window.

**Tile** — A document view in the centre with independent selection, scroll
and navigation. Tiles are arranged in a *tile tree*; today the centre holds
at most two.

**Tile divider and badge** — The bar between two tiles. Its labelled badge
and right-click menu choose the relation (below), the axis, Swap Panes and
Join.

**Tile relations:**

| Mode | Meaning |
|------|---------|
| Independent (`TILES`) | Two unrelated views; links stay in their tile |
| Trail (`TRAIL`, blue) | A link in tile one opens in tile two; both share one browsing trail and Back/Forward |
| Compare (`DIFF`, amber) | Two independent views with structural diff marks and a diff overview in the wider divider |

**Diff overview** — The two-lane strip in a Compare divider: changed blocks
of both documents and their viewports; click or drag to seek in both.

**Title bar tint** — Each panel family has its own title colour (documents
yellow, navigation blue, history amber, media red, …), saturated while the
window is active.

### Bars, fields and strips

**Edit strip** — The tool row on top of every editable document view: lock,
save, undo/redo, clipboard, formatting, structure, lists, insert. Its
presence marks the page as editable; there is no global edit mode.

**Find bar** — The strip under the focused view for find and replace
(Ctrl+F, F3).

**Location field / URL bar** — The address field in the toolbar (Ctrl+L).
It completes typed text (`example.com` → `https://example.com`, paths →
`file://`), offers keyword searches (`w xanadu`), and shows up to eight
*suggestions* from open tabs, bookmarks and recent addresses. Its buttons
open the Heading Outline and Page Navigation.

**Lock** — The padlock (F2) that makes an editable document read-only
without removing its edit strip. Installed data such as the welcome page
starts locked.

**Menu bar / toolbar** — The ImGui menus (File, Edit, View, Layout, Go,
Bookmarks, Panels, Services, Help) and the icon row below them. Toolbar
buttons are flat until hovered; toggles stay sunk while on.

**Minibuffer** — The line editor behind path and URL prompts, with its own
input history rings (Emacs-style naming).

**Page bar** — The bar at the bottom of a view in paged reading, showing
where you are.

**Prompt** — A one-line question or input in the status line (open file,
go to, …). Only one is active at a time.

**Search bar** — The row for searching the web and services with a chosen
engine (Ctrl+K / Ctrl+Alt+K).

**Service strip** — The row on top of a service page (Hacker News, a
forum, Wikipedia, a Gemini host, …) with the service's icon, name and
sections, and a search field. Derived from the address, so it is there
while the page loads.

**Status line** — The bar at the bottom of the window: messages, word
counts, loading progress and prompts.

### On the page

**Bookmark ribbon** — The icon in the left margin beside a bookmarked
passage; it takes clicks for the bookmark's menu.

**Caret** — The text cursor.

**Context menu** — The right-click (or long-press) menu. Page-content
actions (Open Section, Copy Block Address, Reply…, Open Link in Trail View)
are kept apart from view controls.

**Drag grip** — The handle beside a block for dragging it to another place.

**Edge zones** — The left and right edges of a gallery image or a paged
spread; hovering lights them, clicking steps to the previous / next.

**Link wash** — The tint on the link that leads forward in the view's
history, so you see where you went from here.

**Margin arrows / quote links** — Arrows beside quoted text in a thread that
jump to the original message it quotes; the original gets a dotted
underline (`docs/QUOTES.md`).

**Scrollbar ticks** — Marks on the scrollbar for headings, bookmarks,
annotations and search hits.

**Twisty** — The ▸ / ▾ triangle that folds or unfolds a section or list.

### Panels in brief

| Panel | Purpose |
|-------|---------|
| Help panel (Shift+F1) | Explains the GUI element under the pointer |
| Keyboard Reference (F1) | All shortcuts |
| Heading Outline (Ctrl+Shift+O) | Section tree of the page |
| Backlinks (Ctrl+Alt+B) | Where links to this place come from |
| Site Map (Ctrl+Alt+M) | Tree of the current site's pages |
| Page Navigation (Ctrl+Alt+P) | The page's navigation and sidebar chrome as link rows |
| Related Sites | Similar websites to the current one |
| History (Ctrl+Shift+H) | Visit log as a trail tree |
| Downloads | Running and finished downloads |
| Media | Audio and video player controls |
| Subscriptions | Watched feeds, communities and profiles with unread counts |
| Certificates | Pinned Gemini server certificates |
| Identities | Gemini client certificates and Nostr accounts |
| Style Sheets / Manage Style Sheets | Choose and edit `.xss` sheets |
| Bookmarks | Bookmark folders and passages |
| Annotations | Highlights and notes |
| Context | Preview of what is hovered: quoted original, link target, image |
| Document Revisions | Working and published history; preview, diff, restore |
| Reply | Compose a reply to a message (mock; never posts) |
| Nostr / Lemmy / Matrix Explorer | Browse relays, communities, rooms |

**Preferences** — The settings dialog (Edit → Preferences). It is a dialog,
not a dockable panel.

### UI terms in the code

**Canvas chrome** — Interface drawn by xanatoo itself on the SDL canvas
(status line, window titles, outline, tooltips) rather than by ImGui. It uses
the UI font, never the document fonts.

**Chrome (UI)** — Everything around the documents: menus, toolbars, title
bars, strips. Kept apart from document styling (`theme.hpp`).

**Embedded ImGui panel** — A native `FloatWindow` whose client area is
filled with ImGui widgets (Bookmarks, Reply, Styles, …), while the frame,
docking and title bar stay native.

**Key focus** — Which part takes plain keys: the document, the outline, or a
panel. Panels take keys only while they have focus (`panel_has_keys`).

**Paint order / layers** — The fixed drawing order: documents, tile chrome,
floating windows back to front, status line, ImGui menus and popups.

**Primary selection** — The X11 / Wayland selection pasted with the middle
mouse button.

**Software cursor / on-screen keyboard** — The pointer driven by a gamepad
stick and the virtual keyboard on handhelds such as the R36S.

**Touch gestures** — Tap = click, drag / fling = scroll, long press =
context menu, pinch = zoom, two fingers on a gutter = drag it.

---

## 7. Networking and protocols

See `docs/TLS.md` and `docs/dev/SERVICES.md`. All network clients are
read-only: they never post or log in.

**Accept-Language** — The HTTP header naming preferred languages, set from
the user's preferred language list.

**CA / trust store** — Certificate authorities whose root certificates the
system trusts. HTTPS certificates must chain to one; xanatoo uses the
system store (OpenSSL) or CA bundles (Mbed TLS on Android).

**Canonical URL** — The address a page declares as its preferred form
(`<link rel="canonical">`).

**Chunked transfer / Content-Encoding** — HTTP ways of framing and
compressing a response body (chunks; gzip / deflate). xanatoo decodes both.

**Client certificate** — A TLS certificate the *client* presents to identify
itself. Gemini uses them as identities, managed in the Identities panel.

**Connection pool / keep-alive** — Reusing open HTTPS connections for several
requests to the same host (used by the image fetcher).

**Fail closed** — If a certificate cannot be verified, the connection is
refused; there is no "ignore errors" switch.

**Favicon** — A site's small icon, shown in tabs and service strips. A
generated *pseudo favicon* stands in when there is none.

**Feed (RSS / Atom)** — An XML list of a site's latest entries; opened as a
thread of messages.

**Fetch / fetch job** — Loading a page in the background on a worker thread
(`FetchJob`), polled by the frame loop. Stop / Esc cancels it.

**Finger** — An old protocol (port 79) to ask a host about a user; shown as
a page.

**Gemini / capsule** — A small, TLS-only alternative to the web with its own
simple markup (*gemtext*). A Gemini site is called a capsule.

**Gopher** — A menu-based pre-web protocol (1991), still used in the small
net.

**HTTP / HTTPS** — The web's transfer protocol, plain or over TLS. xanatoo
speaks HTTP/1.1 itself.

**HTTPS upgrade** — A redirect from HTTPS to `http://` is followed as
`https://`; there is never a downgrade to plaintext.

**Link `rel`** — What a page declares in `<link rel>`: icons, next / prev /
contents, feeds, OpenSearch, canonical, alternate languages.

**Nex** — A minimal small-net protocol: plain text and directory listings
over TCP.

**NNTP / Usenet** — The Network News Transfer Protocol and the newsgroup
system it serves; `news://` and `nntp://` addresses (`docs/NNTP.md`).

**OpenSearch** — A small XML description a site publishes so browsers can
add its search engine; xanatoo can add it to `search.sexp`.

**Site map** — The tree of a site's pages, read from its `sitemap.xml` or
links.

**Small net** — The collective name for minimal protocols outside the web:
Gemini, Gopher, Spartan, Nex, Finger.

**SNI** — Server Name Indication: the host name sent in the TLS handshake so
one server can host many certificates.

**Spartan** — A Gemini-like small-net protocol without TLS.

**TLS** — Transport Layer Security, the encryption under HTTPS, Gemini and
`wss://`. Wrapped by `TlsStream` (OpenSSL on desktop, Mbed TLS on Android).

**TOFU (trust on first use)** — Gemini's usual trust model: the first
certificate a host shows is pinned (its SHA-256 fingerprint stored in
`gemini-known-hosts`), and later connections must show the same one.

**User-Agent** — The client name sent with HTTP requests, built by
`user_agent("purpose")`.

**WebSocket (`wss://`)** — A two-way connection over HTTPS, used for Nostr
relays.

---

## 8. Services

Native, read-only views of online services, each shown as threads and
messages with its own service strip (`docs/dev/SERVICES.md`).

**Bluesky** — A decentralized social network (AT Protocol); posts as threads.

**Discourse, Flarum, Lobsters, LessWrong, Stack Exchange, 4chan, Digg** —
Forums and discussion sites with native thread views (`docs/FORUMS.md`).

**Directory** — The curated link list in `examples/directory.md`.

**Ebooks (Standard Ebooks, Project Gutenberg / Gutendex)** — Free ebook
catalogs browsable from xanatoo; books open as EPUB.

**Fediverse / Mastodon** — Federated social networks; their mentions and
hashtags in feeds and pages become typed links.

**Hacker News (`hn:`)** — Y Combinator's news and discussion site.

**Internet Archive (`ia:`)** — archive.org: items, collections, media
(`docs/ARCHIVE.md`).

**Lemmy (`lemmy:`)** — A federated Reddit-like link aggregator; content is
organized in *communities* on *instances*.

**Matrix (`matrix:`)** — A federated chat network. Accounts live on a
*homeserver*; conversations are *rooms*, listed in a room directory.

**Mention / hashtag** — Social markup: a link to a person (`rel="mention"`)
or a topic (`rel="tag"`).

**Nostr (`nostr:`)** — A decentralized protocol of signed *events* (notes)
distributed through *relays*. People are public keys, written in bech32 as
`npub1…`. Its specifications are *NIPs* (Nostr Implementation Possibilities),
e.g. NIP-11 relay information, NIP-29 groups. xanatoo keeps watch-only
accounts and a local cache (`docs/NOSTR.md`).

**PeerTube, Odysee, Dailymotion, media.ccc.de, YouTube** — Video sites
shown as native pages; videos play in the Media panel (`docs/VIDEO_SITES.md`,
`docs/YOUTUBE.md`).

**Reddit (`reddit:`)** — Link aggregator organized in subreddits. Optional
app-only OAuth credentials (`docs/REDDIT.md`).

**Related Sites** — Similar websites to the current site, from public
"similar sites" sources.

**Search engine** — An entry in the built-in table or the user's
`search.sexp`; HTML engines open the result page, JSON engines are mapped to
a result document (`docs/SEARCH.md`). Catalog engines (IGDB, RAWG, TMDB,
Discogs) may need API keys, set in Edit → Credentials.

**Subscription** — A watched, changing collection (feed, community,
profile) with an unread count; unlike a bookmark, which marks one page
(`docs/SUBSCRIPTIONS.md`).

**Wayback Machine (`wayback:`)** — The Internet Archive's archive of old
versions of web pages; a *capture* is one stored copy.

**Wikipedia (`wikipedia:`)** — Articles fetched through the Wikipedia API.

---

## 9. File formats and media

**Audio** — MP3, FLAC, Ogg Vorbis and WAV links play in the Media panel.

**CBZ / CBR** — Comic book archives (ZIP / RAR of page images).

**DjVu** — A scanned-document format; like PDF, a fixed-layout format.

**EPUB** — The ebook format; imported natively or through Pandoc.

**Fenced div (`:::name` … `:::`)** — Markdown block syntax (as in Pandoc and
Djot) for xanatoo's semantic blocks: `:::stretch`, `:::note`,
`:::collection`, `:::include`, `:::heading`, `:::chrome`, …

**Front matter** — A YAML block at the top of a Markdown file (`---`) with
title, author and language.

**Gemtext** — Gemini's line-based markup (`.gmi`).

**Markdown** — The convenience import / export format, extended with fenced
divs.

**Org** — Emacs Org mode markup (`.org`); stars become sections.

**Pandoc** — An external converter used to import DOCX, ODT, reStructuredText,
LaTeX and more (read-only; `docs/PANDOC.md`).

**S-expression (`.sexp`)** — Lisp-style parenthesised notation. The older
document encoding (read only now) and the format of style sheets, site hints
and `search.sexp`.

**TeX math** — Formulas written in TeX (`$…$`, `$$…$$`), laid out by
xanatoo's own math engine with an OpenType MATH font (`docs/MATH.md`).

**thumtoo** — The companion image library vendored under `external/thumtoo`:
decodes images with libvips, keeps a tile and thumbnail cache shared with
galapix / biltoo, and renders PDF / DjVu pages. Addresses like
`book.pdf//page:12` use its location syntax.

**Video** — Decoded with FFmpeg on desktop and MediaCodec on Android.

**XML (canonical)** — The canonical, lossless disk encoding of the semantic
tree: fixed tags, quoted attributes (`docs/XML.md`). Headings use XHTML 2
style `<h>`.

---

## 10. Platforms, build and directories

**Dear ImGui** — An immediate-mode GUI library, used for menus, toolbars,
dialogs and panel widgets (vendored under `external/imgui`).

**Emscripten / WASM** — The compiler and WebAssembly target for the offline
browser preview.

**FontConfig / FreeType / HarfBuzz** — Font discovery, glyph rasterizing and
(optional) text shaping libraries.

**Minimal port** — `XANATOO_MINIMAL_PORT`: Windows and WebAssembly builds
without network services, SQLite, FontConfig and thumtoo (`docs/PORTS.md`).

**MinGW-w64** — The toolchain for the Windows builds.

**Nix / flake / `nix develop`** — The reproducible build system. Always build
inside `nix develop` (`xanatoo-build`, `xanatoo-test`, `xanatoo-run`).

**R36S / PortMaster / ArkOS** — A handheld Linux gaming console, the ports
package manager used on it, and its firmware; xanatoo runs there with a
gamepad.

**SDL2 / SDL3** — The cross-platform window, input, audio and rendering
library; the move from SDL2 to SDL3 is in progress.

**SQLite store** — Every database (revisions, history, bookmarks, Nostr,
snapshots) opens through `sqlite::open_store`: WAL mode, busy timeout,
schema migrations, safe with several running processes.

**User directories (XDG)** — Where xanatoo keeps its files:

| Function | Default | Contents |
|----------|---------|----------|
| `config_dir()` | `$XDG_CONFIG_HOME/xanatoo/` | `view.conf`, credentials |
| `state_dir()` | `$XDG_STATE_HOME/xanatoo/` | revisions, history, bookmarks, sessions, known hosts |
| `user_data_dir()` | `$XDG_DATA_HOME/xanatoo/` | your styles, site hints, `search.sexp` |
| `cache_dir()` | `$XDG_CACHE_HOME/xanatoo/` | images, thumbnails, import cache |
| `data_dir()` | `<exe>/../share/xanatoo` | installed examples, styles, fonts |

`--portable` keeps them beside the executable; `--no-save` writes no state.

---

## 11. Code architecture

See `AGENTS.md` and `docs/dev/APP.md`.

**App** — The single plain-data struct holding all application state
(`xanatoo::app::App`, the "world").

**`browsing(app)`** — True when the focused document is read-only or locked;
document edits are gated on it.

**Command (`execute_command`, `UiCommand`)** — Every menu item, toolbar
button and shortcut goes through one command dispatcher.

**Data-oriented / ECS-friendly** — Data in plain records with stable IDs;
behaviour in free functions that transform it. Layout geometry is kept
separate from document identity.

**`DocBuffer`, `BufferId`** — An open document and its never-reused id.

**Frame loop (`run_frame`)** — One pass per frame: handle events, relayout,
run the chrome, draw views, floating windows and overlays.

**`Pane`, `pane_address`** — The view record, and the function that gives its
full address (never `path_of`).

**Pure** — Code without SDL or global state, unit-tested in
`xanatoo_tests`.

**System** — A free function taking `App&` that implements one behaviour
(`app_*.cpp`).

**`WindowRole`** — What a `FloatWindow` shows: a document or one of the
panels.

**Worker thread** — Background thread for fetches, image decoding and
network checks; results are applied by polling systems in the frame loop.
