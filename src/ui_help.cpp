// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// Help panel texts: what each GUI element is for (ui_help.hpp).
//
// Keys: ImGui labels / ids as `ui_help_keys` makes them, "@…" for canvas
// chrome (app_hoverhelp.cpp). One topic may answer to several keys, e.g.
// the same action in a menu and on the toolbar.

#include "xanatoo/ui_help.hpp"

#include <algorithm>
#include <unordered_map>

#include "xanatoo/util.hpp"

namespace xanatoo {

namespace {

const std::vector<UiHelpTopic> kTopics = {
    // ---- The Help panel itself ----
    {"@panel:hover-help|help panel", "Help panel",
     "Explains the element under the mouse pointer: menu entries, toolbar "
     "buttons, fields, window controls, panels and the parts of a page.\n\n"
     "Move the pointer over anything to read about it. While the pointer is "
     "inside this panel the text stays, so it can be scrolled.\n\n"
     "Open or close it with **Shift+F1** or *Panels → Help Panel*. The full "
     "keyboard reference is **F1**."},

    // ---- Menu bar ----
    {"file", "File menu",
     "Documents and tabs: new, open (file or address), tabs and windows, "
     "save and export, reload, drafts, local documents and revisions, quit. "
     "**Alt+F** opens it from the keyboard."},
    {"edit", "Edit menu",
     "Undo / redo, clipboard, selection, moving blocks, find and replace, "
     "and the Preferences and Credentials dialogs. **Alt+E**."},
    {"format", "Format menu",
     "Inline formatting of the selection (bold, italic, code, links, "
     "citations) and the block structure (Structure submenu). Only in edit "
     "mode. **Alt+O**."},
    {"insert", "Insert menu",
     "Insert blocks at the caret: headings, lists, quotes, code, tables, "
     "images, figures, notes, includes. Only in edit mode. **Alt+I**."},
    {"structure", "Structure",
     "Change the shape of the document tree: wrap blocks in a section, "
     "article or stretchtext, unwrap them, promote / demote and move "
     "sections."},
    {"view", "View menu",
     "How the focused page is shown: native or web view, reader mode, view "
     "source, image gallery, section focus, structure highlight, edit mode, "
     "zoom and display options. **Alt+V**."},
    {"layout", "Layout menu",
     "Paged reading, collection layouts, tiles (side by side or stacked, "
     "independent / trail / compare), line spacing, margins, text width, "
     "justification and the style sheet."},
    {"panels", "Panels menu",
     "Open and close the dockable panels: outline, backlinks, site map, "
     "history, downloads, bookmarks, context, this Help panel and more. "
     "Panels can float or be docked to any window edge."},
    {"go", "Go menu",
     "Move around: back / forward, up a level, home, previous / next page "
     "of the site, and the navigation panels. **Alt+G**."},
    {"bookmarks", "Bookmarks menu",
     "Bookmark the page, the selection or the reading position, open the "
     "bookmark manager and jump to recent bookmarks. **Alt+B**."},
    {"history", "History",
     "Back / forward and the pages visited in this session; *Show "
     "History…* opens the History panel (**Ctrl+Shift+H**)."},
    {"subscriptions", "Subscriptions",
     "Feeds, communities and accounts you watch. Subscribe to the current "
     "page, refresh all, or open the Subscriptions panel with the unread "
     "counts."},
    {"services", "Services menu",
     "Start pages of the services xanatoo reads natively: Wikipedia, Hacker "
     "News, Reddit, Lemmy, Nostr, Matrix, Gemini, Usenet, feeds, the "
     "Internet Archive, ebooks and more. Greyed-out entries need a feature "
     "this build lacks."},
    {"help", "Help menu",
     "The keyboard reference (**F1**), the Help panel (**Shift+F1**), the "
     "format reference, example documents and About. **Alt+H**."},
    {"examples", "Examples",
     "Sample documents shipped with xanatoo; each opens in the focused "
     "view. Good for trying stretchtext, transclusion, tables and the other "
     "document features."},
    {"mainmenubar", "Menu bar",
     "All commands, grouped by topic. Hold the pointer over an entry to read "
     "about it here. **F11** hides the menus and toolbars (content only)."},

    // ---- File ----
    {"new", "New document",
     "Opens an empty document in a new tab, ready for editing. **Ctrl+N**."},
    {"open", "Open",
     "Opens a local file: type its path in the status-bar prompt (Tab "
     "completes). **Ctrl+O**."},
    {"open url|openurl", "Open URL",
     "Puts the keyboard into the address field to type an address, a path "
     "or a search. **Ctrl+L**."},
    {"new tab", "New tab",
     "Opens a new tab in the centre view. **Ctrl+T**."},
    {"close tab", "Close tab",
     "Closes the current tab. Unsaved edits are kept as a draft. "
     "**Ctrl+W**."},
    {"open in window", "Open in window",
     "Opens a path or address in a floating document window: a full view "
     "that can be moved, docked or turned back into a tab."},
    {"pop out to window", "Pop out to window",
     "Moves the focused tab into a floating window. **Ctrl+Shift+P**."},
    {"move window to tab", "Move window to tab",
     "Turns the focused document window back into a tab. **Ctrl+Shift+P** "
     "in a window, or drag its title onto the tab strip."},
    {"move tab to window", "Move tab to window",
     "Tears this tab off into a floating document window."},
    {"select tab", "Select tab", "Shows this tab in the centre view."},
    {"save", "Save",
     "Writes the document to its file. Remote pages are saved through *Edit "
     "local copy* first. **Ctrl+S**."},
    {"publish locally", "Publish locally",
     "Records the current working content as a new published revision in "
     "the local revision store. Nothing is uploaded."},
    {"edit local copy", "Edit local copy",
     "Imports this read-only page into the local document store and "
     "switches to edit mode; the original stays untouched."},
    {"view original", "View original",
     "Shows the originally imported snapshot of this document; Back returns "
     "to the working copy."},
    {"export a copy", "Export a copy",
     "Saves the document under a new name, in the format of the file "
     "extension. **Ctrl+Shift+S**."},
    {"export as s-expression", "Export as S-expression",
     "Writes the document tree in xanatoo's S-expression format (`.sexp`)."},
    {"export as xml (experimental)", "Export as XML",
     "Writes the document as XHTML 2-style XML. Experimental."},
    {"export as markdown", "Export as Markdown",
     "Writes the document as Markdown with xanatoo's `:::` block "
     "extensions."},
    {"reload", "Reload",
     "Loads the page again. **Ctrl+R** / **F5**."},
    {"force reload", "Force reload",
     "Reloads bypassing caches: converted files are converted again, "
     "downloads fetched again. **Ctrl+Shift+R** / **Shift+F5**."},
    {"revert to saved", "Revert to saved",
     "Drops unsaved edits and loads the file as last saved. The edits stay "
     "available as a draft."},
    {"drafts", "Drafts",
     "Lists unsaved edits kept in the revision store (autosaved and stashed "
     "drafts) to restore or discard them."},
    {"local documents", "Local documents",
     "Index of the documents in the local document store, with their "
     "working and published revisions."},
    {"document revisions", "Document revisions",
     "Opens the Revisions panel: working and published history of the "
     "focused stored document, with preview, diff and restore."},
    {"quit", "Quit",
     "Closes xanatoo. Unsaved edits are kept as drafts. **Ctrl+Q**."},

    // ---- Edit ----
    {"undo", "Undo", "Takes back the last edit. **Ctrl+Z**."},
    {"redo", "Redo", "Repeats an undone edit. **Ctrl+Y**."},
    {"cut", "Cut", "Moves the selection to the clipboard. **Ctrl+X**."},
    {"copy", "Copy", "Copies the selection to the clipboard. **Ctrl+C**."},
    {"paste", "Paste",
     "Inserts the clipboard at the caret (edit mode) or into the field. "
     "**Ctrl+V**."},
    {"paste and go", "Paste and go",
     "Replaces the address with the clipboard and opens it."},
    {"copy page url|copy address", "Copy address",
     "Copies the address of the page (including a passage selector) to the "
     "clipboard."},
    {"copy link address", "Copy link address",
     "Copies the target address of the link under the pointer."},
    {"copy web permalink", "Copy web permalink",
     "Copies the address of this item on its web site, for sharing with "
     "people who use a browser."},
    {"copy link to comment", "Copy link to comment",
     "Copies an address that opens this comment of the thread."},
    {"select block", "Select block",
     "Selects the block around the caret; again to grow to the parent. "
     "**Ctrl+A**."},
    {"select all", "Select all",
     "Selects the whole document (or field). **Ctrl+Shift+A**."},
    {"select passage", "Select passage",
     "Selects the passage this item addresses."},
    {"duplicate block", "Duplicate block",
     "Inserts a copy of the current block after it. **Ctrl+D**."},
    {"move block up", "Move block up",
     "Swaps the current block with the one before it. **Alt+Up**."},
    {"move block down", "Move block down",
     "Swaps the current block with the one after it. **Alt+Down**."},
    {"find", "Find",
     "Leap search: type in the status-bar prompt, the view jumps to each "
     "match as you type. **F3** finds the next one. **Ctrl+F**."},
    {"replace", "Replace",
     "Find and replace in the document, one match at a time. **Ctrl+H**."},
    {"replace all", "Replace all",
     "Replaces every match of the last search."},
    {"preferences", "Preferences",
     "Settings of the program: interface scale, hover previews, updates and "
     "other options. Saved in the configuration directory."},
    {"credentials", "Credentials",
     "Optional API keys for catalog services (IGDB, RAWG, TMDB, Discogs) and "
     "Reddit app credentials. Stored privately in the configuration "
     "directory."},

    // ---- Format / Insert ----
    {"bold", "Bold", "Strong emphasis on the selection. **Ctrl+B**."},
    {"italic", "Italic", "Emphasis on the selection. **Ctrl+I**."},
    {"strikethrough", "Strikethrough", "Strikes the selected text through."},
    {"code", "Code", "Marks the selection as inline code. **Ctrl+`**."},
    {"link", "Link",
     "Turns the selection into a link; type the target in the prompt. "
     "**Ctrl+Shift+K**."},
    {"citation", "Citation",
     "Marks the selection as a citation of a source in the bibliography."},
    {"heading", "Heading",
     "Makes the block the title of its section. **Ctrl+1**; **Ctrl+2** … "
     "**Ctrl+6** add a soft depth hint."},
    {"heading (depth 2)|heading (depth 3)", "Heading with depth hint",
     "A heading with a soft depth hint for documents without sections."},
    {"paragraph|paragraph (demote)", "Paragraph",
     "Turns the block back into a plain paragraph. **Ctrl+0**."},
    {"block quote", "Block quote", "Toggles a quotation block. **Ctrl+Shift+Q**."},
    {"code block", "Code block", "A preformatted block of code."},
    {"unordered list", "Unordered list", "A bullet list. **Ctrl+Shift+8**."},
    {"ordered list", "Ordered list", "A numbered list. **Ctrl+Shift+7**."},
    {"task item", "Task item", "A list item with a check box."},
    {"definition list", "Definition list", "Terms with their descriptions."},
    {"indent|indent list item", "Indent", "Nests the list item one level deeper. **Tab**."},
    {"outdent|outdent list item", "Outdent", "Moves the list item one level out. **Shift+Tab**."},
    {"horizontal rule", "Horizontal rule", "A separating line. **Ctrl+Shift+-**."},
    {"table", "Table", "Inserts a table at the caret."},
    {"image", "Image",
     "Inserts an image; type a path or address in the prompt. "
     "**Ctrl+Shift+I**, or drop an image file on the page."},
    {"figure", "Figure", "An image with a caption."},
    {"note", "Note", "A footnote or margin note at the caret."},
    {"bibliography entry", "Bibliography entry", "A source for citations."},
    {"include / transclude|transclude", "Include (transclusion)",
     "Embeds another passage live: `#anchor`, a path or an address. "
     "Edits of the source show up here."},
    {"wrap in section|wrap section", "Wrap in section",
     "Puts the selected blocks into a new section. **Ctrl+Shift+1**."},
    {"wrap in article|wrap article", "Wrap in article",
     "Puts the selected blocks into a new article. **Ctrl+Shift+2**."},
    {"wrap in stretch|wrap stretch", "Wrap in stretchtext",
     "Collapses the blocks behind a one-line summary that expands on click. "
     "**Ctrl+E**."},
    {"unwrap stretch", "Unwrap stretchtext",
     "Removes the stretchtext around the blocks. **Ctrl+Shift+U**."},
    {"edit stretch summary", "Edit stretch summary",
     "Changes the one-line summary of the stretchtext. **Ctrl+Shift+E**."},
    {"unwrap section/article", "Unwrap section / article",
     "Removes the section or article container; its blocks stay."},
    {"promote section", "Promote section", "Moves the section one level up the tree."},
    {"demote section", "Demote section",
     "Moves the section into the preceding one."},
    {"move section up|move section down", "Move section",
     "Moves the whole section past its neighbour."},
    {"highlight selection", "Highlight", "Marks the selection as highlighted."},
    {"annotate selection", "Annotate selection",
     "Attaches a note to the selected passage in the separate annotation "
     "layer; the document itself is not changed."},
    {"reply", "Reply",
     "Opens the Reply panel for this message (a mockup: nothing is "
     "posted)."},

    // ---- View ----
    {"native view|web page view", "Native / web page view",
     "Switches between the service's native reading view (threads, "
     "articles as structure) and the plain web page."},
    {"old version (archive.org)|live page", "Old version",
     "Shows the page as archived by the Wayback Machine, or back the live "
     "page."},
    {"reader mode", "Reader mode",
     "Shows only the main text of a web page, without navigation and side "
     "bars. **F9**."},
    {"view source", "View source",
     "Shows the page as the rendered document, its raw source, or the "
     "document tree as S-expression or XML."},
    {"rendered document", "Rendered document", "The page as a document (normal view)."},
    {"raw source", "Raw source", "The bytes the page was loaded from."},
    {"s-expression", "S-expression source", "The document tree in `.sexp` form."},
    {"xml", "XML source", "The document tree as XML."},
    {"image gallery", "Image gallery",
     "All images of the page as a grid; click one to view it alone. "
     "**Ctrl+Alt+I**."},
    {"focus current section", "Focus current section",
     "Shows only the section around the caret. **Ctrl+Alt+F**."},
    {"structure highlight", "Structure highlight",
     "Colour bars and kind tags beside the blocks show the document tree. "
     "**Ctrl+Shift+L**."},
    {"edit mode|tb_edit", "Edit mode",
     "Switches the focused view between browsing (links follow on click) "
     "and editing (caret, typing). Foreign pages are read-only; use *Edit "
     "local copy*. **F2**."},
    {"content only|content only (hide menus, toolbars)", "Content only",
     "Hides menus, toolbars and the status line for reading. **F11** "
     "brings them back."},
    {"zoom in", "Zoom in", "Larger text. **Ctrl++**, or Ctrl+wheel."},
    {"zoom out", "Zoom out", "Smaller text. **Ctrl+-**, or Ctrl+wheel."},
    {"footnote overlay", "Footnote overlay",
     "Shows the notes of the visible footnote marks in a strip at the "
     "bottom of the view."},
    {"bookmarks in the page", "Bookmarks in the page",
     "Marks bookmarked passages with an icon in the page margin."},
    {"make web addresses clickable", "Clickable web addresses",
     "Turns plain-text addresses in pages into links."},
    {"show images of image addresses", "Images of image addresses",
     "Shows a link to an image file as the image itself."},
    {"virtual keyboard", "Virtual keyboard",
     "An on-screen keyboard for gamepads and touch screens. **F3** on a "
     "gamepad setup."},

    // ---- Layout ----
    {"paged reading", "Paged reading",
     "Shows the page as book pages, turned with the page bar, the page "
     "edges or the keys instead of scrolling. **F8**."},
    {"page columns", "Page columns", "How many pages side by side in paged reading."},
    {"collection", "Collection layout",
     "How a collection (images, cards) is laid out: list, grid or card "
     "sizes, and its sort order."},
    {"independent tiles", "Independent tiles",
     "Two views side by side, each with its own page and history. "
     "**Ctrl+\\**."},
    {"trail (linked navigation)|tb_trail", "Trail",
     "Linked tiles: a link in the first tile opens in the second, a link in "
     "the second moves the trail on. Both tiles share Back / Forward."},
    {"side by side", "Side by side", "The two tiles as columns."},
    {"stacked", "Stacked", "The two tiles as rows."},
    {"swap panes", "Swap panes", "Exchanges the pages of the two tiles."},
    {"compare tiles", "Compare tiles",
     "Marks the differences between the two tiles' documents; the divider "
     "shows an overview of the changes. **Ctrl+Shift+D**."},
    {"line spacing|tb_linespacing", "Line spacing",
     "Space between lines for this view (tight to loose)."},
    {"margins", "Margins", "Space left and right of the text."},
    {"text width|tb_measure", "Text width",
     "The longest line length, in characters, and the margins."},
    {"justify lines", "Justify lines", "Aligns both edges of paragraphs."},
    {"style sheet|tb_style", "Style sheet",
     "The look of documents: fonts, colours and spacing per role. Choose a "
     "sheet, or manage and edit them."},
    {"manage / edit style sheets|manage style sheets", "Manage style sheets",
     "Opens the style sheet manager: create, edit and pin sheets to sites "
     "or services."},
    {"reload style sheets", "Reload style sheets",
     "Reads the style sheet files again after editing them outside."},
    {"tight|normal|relaxed|loose", "Line spacing",
     "Space between lines for this view."},

    // ---- Panels ----
    {"keyboard reference", "Keyboard reference",
     "The help document with every shortcut and concept, in a dockable "
     "window. **F1**."},
    {"heading outline|tb_outline", "Heading outline",
     "The headings of the page as a tree: click to jump, fold with the "
     "triangles. **Ctrl+Shift+O**."},
    {"backlinks", "Backlinks",
     "Places in open documents that link here. **Ctrl+Alt+B**."},
    {"site map", "Site map",
     "Tree of the pages of this site from its sitemap.xml. **Ctrl+Alt+M**."},
    {"related sites", "Related sites",
     "Web sites similar to the focused page's site."},
    {"page navigation", "Page navigation",
     "The navigation bars and side bars of the web page, moved into a "
     "panel. **Ctrl+Alt+P**."},
    {"downloads|tb_downloads", "Downloads",
     "Files being downloaded and the ones finished, with progress and "
     "cancel."},
    {"certificates", "Certificates",
     "Gemini server certificates seen in this session: view, accept a "
     "changed one, or forget a pin."},
    {"style sheets", "Style sheets panel", "Pick the style sheet of the focused page."},
    {"annotations", "Annotations",
     "Notes attached to passages of the document, kept apart from it."},
    {"context", "Context panel",
     "Shows what the pointer is over in a page: the quoted message, the "
     "source of an include, a link's target or an image."},
    {"nostr explorer", "Nostr explorer", "Browse Nostr relays, groups and events."},
    {"lemmy explorer", "Lemmy explorer", "Browse Lemmy servers, communities, people and posts."},
    {"matrix explorer", "Matrix explorer", "Browse public Matrix rooms and servers."},
    {"nostr accounts", "Nostr accounts", "Public Nostr profiles seen so far."},

    // ---- Go ----
    {"back|tb_back", "Back",
     "The previous page of this view. **Alt+Left**. Right-click lists the "
     "earlier pages to jump to."},
    {"forward|tb_fwd", "Forward",
     "The next page of this view, after going back. **Alt+Right**. "
     "Right-click lists them."},
    {"up|tb_up", "Up",
     "One level up the page hierarchy: thread → forum, file → folder, path "
     "→ parent. **Alt+Up** while browsing."},
    {"home|tb_home", "Home", "The start page. **Alt+Home**."},
    {"previous page in site|next page in site", "Previous / next page in site",
     "Steps through the site's pages in site-map order. **Alt+PgUp** / "
     "**Alt+PgDn**."},
    {"search bar", "Search bar",
     "Shows the search field next to the address. **Ctrl+K**."},
    {"show history", "History panel",
     "Every page visited, newest first, as a trail tree: each visit is "
     "linked to the page it was reached from. **Ctrl+Shift+H**."},

    // ---- Bookmarks ----
    {"bookmark this page", "Bookmark this page",
     "Adds the page to the bookmarks. **Ctrl+Shift+B**."},
    {"bookmark selection", "Bookmark selection",
     "Bookmarks the selected passage: the bookmark opens the page cut to "
     "it."},
    {"bookmark reading position", "Bookmark reading position",
     "Bookmarks the place of the caret, to continue reading there."},
    {"show bookmarks", "Bookmarks panel",
     "The bookmark manager: search, open, remove bookmarks."},
    {"remove bookmark|remove page bookmark", "Remove bookmark",
     "Deletes this bookmark."},
    {"go to bookmark", "Go to bookmark", "Opens the bookmarked page or passage."},
    {"subscribe", "Subscribe",
     "Watches this feed, community or account for new posts (Subscriptions "
     "panel)."},
    {"show subscriptions", "Subscriptions panel",
     "Watched feeds and communities with unread counts."},
    {"refresh all", "Refresh all", "Checks every subscription for new posts now."},

    // ---- Help ----
    {"format reference (format.md)", "Format reference",
     "The document format: blocks, inline markup, xanatoo's extensions."},
    {"about xanatoo", "About xanatoo",
     "Version, services, document formats and libraries of this build."},
    {"diff demo", "Diff demo",
     "Opens two versions of a document side by side with difference marks."},

    // ---- Toolbar ----
    {"toolbar_browse", "Navigation toolbar",
     "Back, forward, up, home, reload, bookmarks, view tools and the address "
     "field. Hold the pointer over a button to read about it."},
    {"toolbar_edit", "Edit toolbar",
     "Formatting and insert tools for edit mode."},
    {"tb_stop", "Stop loading",
     "Cancels loading the page. **Esc**."},
    {"tb_reload", "Reload",
     "Loads the page again. **Ctrl+R** / **F5**. Shift+click forces a "
     "reload past caches; right-click offers both."},
    {"tb_bookmark", "Bookmark",
     "Bookmarks the page, or the selection if there is one; a filled "
     "ribbon means the page is bookmarked, a click removes it then. "
     "**Ctrl+Shift+B**."},
    {"tb_find", "Find", "Leap search in the page. **Ctrl+F**."},
    {"tb_split", "Split / join",
     "Splits the centre into two independent tiles, or joins them again "
     "(keeping the left). **Ctrl+\\**."},
    {"tb_viewmenu", "View tools",
     "Display settings of the view: line spacing, text width, margins, "
     "style sheet, and view options."},
    {"tb_history", "History panel",
     "Every page visited, newest first, as a trail tree. "
     "**Ctrl+Shift+H**."},
    {"urlbar", "Address field",
     "The address of the focused view. Type an address, a file path or a "
     "search and press Enter; suggestions come from history, tabs and "
     "bookmarks. Search keywords like `w xanadu` work too. **Ctrl+L**."},
    {"searchfield", "Search field",
     "Searches with the engine shown at its left end; click the engine to "
     "change it. Enter searches, Esc closes. **Ctrl+K**."},
    {"doctabs", "Tab strip",
     "Open documents of the centre view. Drag to reorder, drag down out of "
     "the strip to make a window, middle-click or × to close, right-click "
     "for more."},
    {"tab", "Tab",
     "A document in the centre view. Click to show it, drag to reorder or "
     "out of the strip to turn it into a window; right-click for more."},
    {"+|new-tab", "New tab", "Opens a new tab. **Ctrl+T**."},

    // ---- Native window frames ----
    {"@float-title", "Window title bar",
     "Drag to move the window; drop it on a window edge to dock it as a "
     "side column or band, drag it off again to float. Double-click "
     "maximizes / restores."},
    {"@doc-window-title", "Document window title",
     "A page in its own window. Drag to move or dock; drop it on the tab "
     "strip to make it a tab again. Right-click to move it back."},
    {"@float-close", "Close box", "Closes this window."},
    {"@float-shade", "Roll-up box",
     "Rolls a docked window up to its title bar so its neighbours get the "
     "room; again to roll it down. Only for docked windows."},
    {"@float-max", "Maximize box",
     "Fills the whole client area with this window, or restores it."},
    {"@float-resize", "Window edge", "Drag to resize the window."},
    {"@dock-edge", "Dock edge",
     "Drag to change the width (or height) of this dock column or band."},
    {"@dock-split", "Panel separator",
     "Drag to share the dock between the two neighbouring panels."},
    {"@sitemap-page", "Page button",
     "Shows the site map as a `sitemap:` document in the view."},
    {"@outline-focus", "Focus button",
     "Shows only the current section in the view (section focus)."},

    // ---- Panels (client areas) ----
    {"@panel:help", "Keyboard reference",
     "The help document: shortcuts and concepts. Click links to jump; "
     "**F1** or **Esc** closes it."},
    {"@panel:outline", "Outline panel",
     "Headings of the page by depth. Click a heading to jump there, click "
     "a triangle to fold the section; ↑/↓ and Enter while focused."},
    {"@panel:backlinks", "Backlinks panel",
     "Passages of open documents that link to this one. Click one to go "
     "there."},
    {"@panel:sitemap", "Site map panel",
     "The site's pages as a tree. ↑/↓ move, ←/→ fold, Enter opens; "
     "middle-click opens in a new tab. Seen pages carry a dot."},
    {"@panel:page", "Page navigation panel",
     "Navigation and side bars of the web page as links; click to follow, "
     "middle-click for a new tab."},
    {"@panel:history", "History panel",
     "Visits newest first, each linked to the visit it came from, like "
     "`git log --graph`: a lane continues where you followed links, a fork "
     "is a tab opened from a page, a hollow dot starts a trail (typed, "
     "bookmark). Clicking a visit goes back to it (no new entry), so "
     "browsing on grows that branch. Anchor jumps and reloads in one page share a row (*×n*). "
     "Click to open, middle-click for a new tab, hover for details. With "
     "the panel focused: **/** or **Ctrl+F** filters, arrows / Home / End "
     "move, **Enter** opens, **Delete** removes the visit from the history, "
     "**Esc** clears the filter or closes. Right-click a row for its menu."},
    {"@panel:styles", "Style sheets panel", "Pick the style sheet of the focused page."},
    {"@panel:style-manager", "Style sheet manager",
     "Create, edit and pin style sheets. Changes apply as you type."},
    {"@panel:bookmarks", "Bookmarks panel",
     "All bookmarks: filter, open, remove. Right-click a bookmark for more."},
    {"@panel:context", "Context panel",
     "Shows what the pointer is over in a page: quoted message, include "
     "source, link target or a picture."},
    {"@panel:subscriptions", "Subscriptions panel",
     "Watched sources with unread counts; click to open and mark read."},
    {"@panel:downloads", "Downloads panel",
     "Current and finished downloads. Click to open, right-click for the "
     "folder, copy or cancel."},
    {"@panel:reply", "Reply panel",
     "Draft a reply to a message (a mockup: nothing is posted)."},
    {"@panel:annotations", "Annotations panel",
     "Notes on passages of this document, kept in a separate layer."},
    {"@panel:revisions", "Revisions panel",
     "Working and published history of the document: preview, diff and "
     "restore (restoring adds a new revision)."},
    {"@panel:nostr-explorer", "Nostr explorer",
     "Relays, groups and events seen; search and open them."},
    {"@panel:nostr-accounts", "Nostr accounts", "Public profiles seen so far."},
    {"@panel:lemmy-explorer", "Lemmy explorer",
     "Lemmy servers, communities, people and posts; pick a server and "
     "search."},
    {"@panel:matrix-explorer", "Matrix explorer",
     "Public Matrix rooms of a server; select one to open it."},
    {"@panel:certificates", "Certificates panel",
     "Gemini server certificates: accept a changed one or forget a pin."},
    {"@panel:related-sites", "Related sites panel",
     "Sites similar to the focused page's site. Click to open, middle-click "
     "for a new tab. *Follow page* off keeps the list while you explore."},

    // ---- Centre area ----
    {"@status-bar", "Status line",
     "The position of the caret, the page's address and state, and "
     "messages. Prompts (open, find, link) are typed here."},
    {"@tile-divider", "Tile divider",
     "Drag to share the room between the two tiles. Right-click to choose "
     "independent, trail or compare."},
    {"@tile-mode", "Tile mode badge",
     "How the two tiles relate (independent, trail, compare). Click to "
     "change."},
    {"@tile-diff-track", "Difference overview",
     "Where the two compared documents differ. Click or drag to scroll both "
     "tiles there."},
    {"@scrollbar", "Scrollbar",
     "Drag the thumb to scroll; click the track to page or jump."},
    {"@document", "Page",
     "The document of this view. Click links to follow them, middle-click "
     "to open them in a new tab, right-click for actions. **F2** switches "
     "to editing (when the page is editable)."},
    {"@document-edit", "Page (edit mode)",
     "Type to edit at the caret. Drag the left margin of a block to move "
     "it; right-click for actions. **F2** returns to browsing."},
    {"@link", "Link",
     "Click to follow, middle-click to open in a new tab, right-click to "
     "copy the address or open it elsewhere. The status line shows the "
     "target."},
    {"@image", "Image",
     "Right-click for actions; with the Context panel open its full picture "
     "shows there. **Ctrl+Alt+I** shows all images of the page."},
    {"@image-card", "Collection card",
     "Click to view the picture in the gallery viewer."},
    {"@image-handle", "Image resize handle",
     "Drag the corner to resize the image (edit mode)."},
    {"@stretch", "Stretchtext",
     "Click the marker to expand or collapse the hidden passage."},
    {"@task", "Task check box", "Click to tick or untick the task."},
    {"@grip", "Block grip",
     "Drag the block to move it among its siblings or into another "
     "container (edit mode)."},
    {"@col-gap", "Column gutter", "Drag to change the column widths."},
    {"@transclude", "Include",
     "A passage embedded live from elsewhere. Click to open the source; "
     "the Context panel shows it."},
    {"@bookmark-mark", "Bookmark mark",
     "A bookmarked passage of this page. Click to select it."},
    {"@quote-arrow", "Quote link",
     "This quote comes from (or is quoted in) another message of the "
     "thread. Click to jump there."},
    {"@annotation-tab", "Annotation",
     "A note on this passage. Click to show it in the Annotations panel."},
    {"@reader-notice", "Reader mode notice",
     "Parts of the page are hidden by reader mode; click *Show all* to see "
     "the whole page."},
    {"@gallery", "Gallery",
     "The page's images as a grid. Click one to view it alone; the strip "
     "at the top goes back to the page."},
    {"@gallery-image", "Gallery image",
     "Wheel or pinch zooms, drag pans; ←/→ step, Esc goes back to the "
     "grid."},
    {"@gallery-prev", "Previous image", "Click to show the previous image (←); on the first one ↺ wraps around to the last."},
    {"@gallery-next", "Next image", "Click to show the next image (→); on the last one ↻ wraps around to the first."},
    {"@gallery-up", "Up strip",
     "Click to go back: from an image to the grid, from the grid to the "
     "page."},
    {"@gallery-grid", "Grid button", "Shows all images of the page as a grid."},
    {"@page-bar", "Page bar",
     "Paged reading: ◀ / ▶ turn the page, drag the slider to go anywhere."},
    {"@page-edge", "Page edge", "Click to turn the page (paged reading)."},
};

std::string_view trim(std::string_view s) {
  trim_ascii(s);
  return s;
}

const std::unordered_map<std::string, const UiHelpTopic*>& index() {
  static const auto map = [] {
    std::unordered_map<std::string, const UiHelpTopic*> m;
    for (const UiHelpTopic& t : kTopics) {
      std::string_view keys = t.keys;
      while (!keys.empty()) {
        const size_t bar = keys.find('|');
        m.emplace(std::string(keys.substr(0, bar)), &t);
        if (bar == std::string_view::npos) break;
        keys.remove_prefix(bar + 1);
      }
    }
    return m;
  }();
  return map;
}

}  // namespace

const std::vector<UiHelpTopic>& ui_help_topics() { return kTopics; }

std::string ui_help_normalize(std::string_view s) {
  s = trim(s);
  for (bool again = true; again;) {
    again = false;
    for (std::string_view tail : {"\xE2\x80\xA6", "...", ":"})
      if (ends_with(s, tail)) {
        s.remove_suffix(tail.size());
        s = trim(s);
        again = true;
      }
  }
  return ascii_lower(s);
}

std::string ui_help_visible_label(std::string_view label) {
  const size_t hash = label.find("##");
  return std::string(trim(label.substr(0, hash)));
}

std::vector<std::string> ui_help_keys(std::string_view label) {
  std::vector<std::string> keys;
  const size_t last = label.rfind("##");
  if (last != std::string_view::npos) {
    std::string_view id = label.substr(last + 2);
    while (!id.empty() && id.front() == '#') id.remove_prefix(1);
    while (!id.empty() && id.back() >= '0' && id.back() <= '9')
      id.remove_suffix(1);
    std::string k = ui_help_normalize(id);
    if (!k.empty()) keys.push_back(std::move(k));
  }
  auto add = [&](std::string k) {
    if (!k.empty() && std::find(keys.begin(), keys.end(), k) == keys.end())
      keys.push_back(std::move(k));
  };
  const std::string visible = ui_help_normalize(ui_help_visible_label(label));
  add(visible);
  // "Trail (linked navigation)" → "trail": the name without its remark.
  const size_t paren = visible.find(" (");
  if (paren != std::string::npos && paren > 0)
    add(ui_help_normalize(std::string_view(visible).substr(0, paren)));
  return keys;
}

const UiHelpTopic* find_ui_help(std::string_view key) {
  const auto& m = index();
  const auto it = m.find(std::string(key));
  return it == m.end() ? nullptr : it->second;
}

std::string ui_help_markdown(const UiHelpTopic* topic, std::string_view label,
                             std::string_view tip) {
  std::string title = topic ? topic->title : ui_help_visible_label(label);
  if (title.empty()) title = std::string(tip.substr(0, tip.find('\n')));
  if (title.empty()) title = "Help";
  std::string md = "# " + title + "\n\n";
  // Without a topic the element's own tooltip is all there is to say.
  tip = trim(tip);
  if (!topic && !tip.empty() &&
      ui_help_normalize(tip) != ui_help_normalize(title)) {
    std::string_view rest = tip;
    while (!rest.empty()) {
      const size_t nl = rest.find('\n');
      md += "> ";
      md += trim(rest.substr(0, nl));
      md += "\n";
      if (nl == std::string_view::npos) break;
      rest.remove_prefix(nl + 1);
      if (!rest.empty()) md += ">\n";
    }
    md += "\n";
  }
  if (topic)
    md += topic->body;
  else
    md += "*No description for this element yet.*";
  md += "\n";
  return md;
}

}  // namespace xanatoo
