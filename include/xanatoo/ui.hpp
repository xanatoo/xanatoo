// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include "xanatoo/catalog_credentials.hpp"

#include "xanatoo/sdl.hpp"
#include "xanatoo/history_menu.hpp"
#include "xanatoo/http_cookies.hpp"
#include "xanatoo/urlbar_suggestions.hpp"
#include "xanatoo/nostr.hpp"
#include "xanatoo/nostr_store.hpp"
#include "xanatoo/lemmy_directory.hpp"
#include "xanatoo/matrix.hpp"
#include "xanatoo/style.hpp"
#include "xanatoo/subscriptions.hpp"
#include "xanatoo/service_bar.hpp"
#include "xanatoo/ui_panel_layers.hpp"
#include <string>
#include <optional>
#include <vector>
#include <unordered_map>

#include "xanatoo/util.hpp"

namespace xanatoo {

// Commands requested by the ImGui menu / toolbar / context menu.
// Applied by the main loop.
// Tabs of the Identities panel, one per protocol; each keeps its own store.
enum class IdentitySection { Gemini, Nostr };

enum class UiCommand {
  None = 0,
  // File
  NewDocument,
  OpenDocument,
  OpenUrlBar,  // Ctrl+L — focus URL bar / status fallback
  NavigateUrl,  // toolbar/menu URL field committed (arg = URL or path)
  NavigateHistoryUrl,  // History menu recent address (arg = URL); a `history` visit
  NavBack,     // Alt+Left session history
  NavForward,  // Alt+Right
  NavJump,     // arg = signed number of history steps (Back/Forward context menus)
  GoHome,      // Alt+Home: home page (welcome.md for now)
  GoUp,        // one level up the page hierarchy (UiState::up_url)
  StopLoading, // cancel running page fetches (toolbar Stop, Esc)
  ToggleTabBar,
  ToggleVerticalTabs,
  ToggleHistory,   // View → History panel
  ToggleDownloads, // Panels → Downloads
  ToggleMedia,     // Panels → Media (the sound player)
  ToggleStylesPanel, // Panels → Style sheets (pick applied theme)
  ToggleStyleManagerPanel, // Panels → Manage style sheets (edit / create)
  AddBookmark,     // bookmark the selection, else the current page
  BookmarkPage,    // always bookmark the page, ignoring text selection
  BookmarkPosition,  // bookmark the caret position in the page
  RemoveBookmark,  // remove current (or arg) from bookmarks
  ToggleBookmarks, // Bookmarks manager panel
  ToggleContextPanel, // Panels → Context (quote / transclusion source)
  ToggleSubscriptions, // watched feeds / communities / accounts
  Subscribe, // add a source by address (empty = current page)
  Unsubscribe, // remove subscription by address
  SubscriptionOpen, // open and mark read
  SubscriptionOpenInTab,
  SubscriptionMarkRead, // mark read without opening
  SubscriptionRefresh, // force a fetch now (empty = all)
  SubscriptionInterval, // address + newline + integer minutes
  ToggleAnnotationsPanel, // annotation layer visibility / selection
  ToggleRevisionsPanel,  // native database document revision browser
  ToggleNostrExplorer, // local relay event / pubkey catalogue
  ToggleLemmyExplorer, // collected Lemmy instances, communities, users, posts
  LemmySetTab,         // arg = 0..3
  LemmySetSearch,      // arg = local filter text
  LemmySetInstance,    // arg = known instance host
  LemmyAddInstance,    // arg = https://host or host
  LemmySetSort,        // arg = Hot, New, Active, TopDay, TopWeek
  LemmyToggleInstanceOnly,
  ToggleMatrixExplorer, // observed Matrix servers, rooms, spaces, people
  MatrixSetTab,         // arg = 0..3
  MatrixSetSearch,      // arg = local filter text
  MatrixSetServer,      // arg = server name (selected for directory actions)
  MatrixAddServer,      // arg = server name or https://server
  ToggleNostrAccounts, // Identities panel on its Nostr tab (or close it there)
  ToggleCookies, CookieSelect, CookieSiteToggle, CookieToggle, CookieDelete, CookieReveal,
  ToggleCertificates,  // Certificates panel; arg = host:port opens it on that row
  CertAccept,          // arg = host:port: trust the changed certificate
  CertReject,          // arg = host:port: keep the old pin
  CertForget,          // arg = host:port: remove the pin
  CertSelect,          // arg = host:port: show that row in detail
  ToggleIdentities,    // Identity panel (Gemini certificates, Nostr accounts)
  IdentitySection,     // arg = "gemini" / "nostr": the tab the user picked
  IdentitySelect,      // arg = sha256 (empty: none)
  IdentityCreate,      // arg = name: a new certificate
  IdentityCreateHere,  // arg = name: new, used on the focused page's site
  IdentityUseSite,     // arg = sha256: use on the focused page's server
  IdentityUsePage,     // arg = sha256: use on the focused page (and below)
  IdentityStopHere,    // no identity for the focused page any more
  IdentityDropScope,   // arg = sha256 "\n" scope
  IdentityRename,      // arg = sha256 "\n" new name
  IdentityDelete,      // arg = sha256 (the panel asked first)
  ToggleRelatedSites, // similar websites of the focused page's site
  RelatedOpen,       // arg = https://domain/: open in the focused view
  RelatedOpenTab,    // arg = https://domain/: open in a new tab
  RelatedRefresh,    // ask the services again for the shown site
  RelatedFollow,     // toggle: follow the focused page / hold the list
  NostrSearchLocal,    // arg = search text
  NostrShowPeople,     // people vs event result tab
  NostrShowEvents,
  NostrShowGroups,
  NostrRefreshLocal,   // reload DB observations
  NostrPrevPage,       // previous 100 locally observed records
  NostrNextPage,
  NostrAddWatchIdentity, // arg = label + newline + npub/hex key (also renames)
  NostrSelectIdentity, // arg = saved public key: make it the active account
  NostrIdentityShow,   // arg = saved public key shown in detail (empty: none)
  NostrIdentityRemove, // arg = saved public key
  NostrCreateKey,      // arg = label + newline + password (empty: none)
  NostrImportKey,      // arg = label + newline + nsec/ncryptsec + newline + password
  NostrSetKeyPassword, // arg = public key (unlocked) + newline + new password
                       // (empty: keep the key without a password)
  NostrUnlockKey,      // arg = public key + newline + password
  NostrLockKey,        // arg = public key: forget the unlocked secret
  NostrDeleteKey,      // arg = public key: delete the signing key for good
  NostrCopyNsec,       // arg = public key (unlocked): plain nsec to the clipboard
  NostrPublishNote,    // arg = note text, signed by the active account
  NewAnnotationLayer,     // new local, independently versioned layer
  SelectAnnotationLayer,  // arg = layer id (local owner only)
  ToggleAnnotationLayer,  // arg = layer id; view-only visibility
  AddAnnotationHighlight,// highlight selected passage in active local layer
  AddAnnotationNote,    // arg = note text; attach to selected passage
  SelectAnnotation,     // arg = annotation id in visible current page
  UpdateAnnotationNote, // arg = replacement body for selected local note
  RemoveAnnotation,     // arg = annotation id in local layer
  RestoreAnnotationLayerVersion, // arg = layer id + newline + revision id
  SelectBookmark,  // arg = address: select its passage in the focused view
  MoveBookmark,          // arg = address + "\n" + folder ("" = top level)
  RetitleBookmark,       // arg = address + "\n" + new title
  NewBookmarkFolder,     // arg = folder path
  MoveBookmarkFolder,    // arg = from + "\n" + to (rename / re-parent)
  RemoveBookmarkFolder,  // arg = folder path; contents move up one level
  CopyImage,
  OpenImageInTab,
  CopyText,        // arg = text for the clipboard (an address)
  OpenLinkInTab,   // arg = href of a link in the focused view: new tab
  OpenLinkInTrail, // arg = href of a link in the focused view: Trail tiles
  OpenBlock,        // arg = index into UiState::context_blocks: cut the view to it
  OpenBlockInWindow, // … in a new document window (tabs keep no cut)
  CopyBlockAddress, // … copy its address (page//id:…)
  DownloadUrl,     // arg = http(s) URL: open the download confirmation dialog
  SavePdf,
  ConfirmDownload, // user accepted the download dialog (default folder)
  DownloadToFolder,// user chose "Download to…" (native folder picker)
  CancelDownload,  // user dismissed the download dialog
  DownloadAction,  // arg = action on App::download_menu (right-clicked
                   // Downloads row): open / open-tab / system / folder /
                   // copy-path / copy-url / cancel / forget
  HistoryAction,   // arg = action on the right-clicked History row
                   // (HistoryPanel::menu_id): open / open-tab / open-other /
                   // copy / remove
  MediaAction,     // arg = action of the Media panel's seek bar menu
                   // (App::media_menu): bookmark-time / unbookmark-time /
                   // bookmark-file / unbookmark-file / seek / copy-time
  OpenReply,       // right-clicked Message: start a reply draft
  JumpQuote,       // context menu quote link; arg = index into the items
  ContextJump,     // Context panel: jump to hovered quote source
  EditReply,       // arg = Markdown currently in the reply editor
  ShowReplyTarget, // scroll a view to the message being answered
  SendReply,       // posts to Nostr (unlocked account); a mockup elsewhere
  // Tabs / MDI
  NewTab,          // new empty buffer + tab
  CloseOtherTabs, CloseTabsRight, CloseTabsLeft, SortTabsByTitle,
  RestoreClosedTab,
  CloseTab,        // close focused buffer (Ctrl+W)
  NextTab,
  PrevTab,
  SelectTab,       // arg = tab index in tab_order
  ReorderTabs,     // arg = buffer ids "3,1,2", the new tab_order (dragged)
  TearTab,         // arg = "id,x,y": move a specific tab to a document window
  PopOut,          // focused buffer → floating window
  PopIn,           // arg = optional buffer id: document window → centre tab
  Save,
  Publish,       // snapshot current database-native working revision locally
  EditLocalCopy, // preserve source; switch to editable local database document
  ViewOriginal,  // show immutable imported source snapshot

  SaveAs,
  ExportXml,
  ExportMarkdown,
  Reload,
  ForceReload,
  RevertToSaved,  // drop the buffer's draft and reload the file
  OpenDrafts,     // "drafts:" page: every stored unsaved draft
  OpenStoredDocuments,  // "documents:" page: database-native working documents
  Quit,
  // Edit
  Undo,
  Redo,
  Cut,
  Copy,
  CopyPageUrl,  // current gemini:// or file path
  CopyPageWebUrl,  // http(s) address of a page with another address (hn:, …)
  OpenInBrowser,   // the focused page in the system browser (browser_url)
  Paste,
  SelectAll,
  SelectBlock,
  ExpandSelection,
  DuplicateBlock,
  MoveBlockUp,
  MoveBlockDown,
  Find,
  Replace,
  ReplaceAll,
  // Find bar; arg "find text\nreplacement" ("" = unchanged).
  FindText,
  FindNext,
  FindPrev,
  FindClose,
  ReplaceOne,
  // Format / inline
  Bold,
  Italic,
  Strike,
  Code,
  Link,
  Cite,
  // Insert blocks
  InsertParagraph,
  InsertHr,
  InsertImage,
  InsertNote,
  InsertCodeBlock,
  InsertTable,
  InsertDefList,
  InsertBiblioEntry,
  InsertColumns,
  InsertTransclude,
  InsertFigure,
  // Structure / lists
  ToggleUnorderedList,
  ToggleOrderedList,
  ToggleTask,
  ToggleBlockQuote,
  IndentList,
  OutdentList,
  WrapStretch,
  EditStretchSummary,
  UnwrapStretch,
  Heading1,  // legacy → Header with depth hint
  Heading2,
  Heading3,
  Heading4,
  Heading5,
  Heading6,
  MakeHeader,
  WrapSection,
  WrapArticle,
  UnwrapSectioning,
  PromoteSection,
  DemoteSection,
  MoveSectionUp,
  MoveSectionDown,
  ParagraphLevel,
  // View
  ToggleSplit,
  ToggleTrail,
  TilesIndependent,
  TilesTrail,
  TilesCompare,
  TilesColumns,
  TilesRows,
  SwapPanes,       // exchange two centre views, including navigation state
  JoinLeftPane,    // collapse split, retaining the left/top view
  JoinRightPane,   // collapse split, retaining the right/bottom view
  ToggleOutline,
  ToggleBacklinks,
  ToggleSitemap,  // site structure panel of the focused web page
  AudioToggle,        // sound player: play / pause
  AudioStop,          // sound player: stop and hide
  AudioSkip,          // sound player: arg = seconds forward (negative: back)
  AudioAB,            // sound player: A–B markers on / off
  AudioLoop,          // sound player: loop on / off (the A–B section or the file)
  AudioFullscreen,    // sound player: the video fills the screen / back
  AudioControls,      // sound player: the panel's controls hidden / shown
  ToggleSearchBar,    // show / hide the search bar (Ctrl+K focuses it)
  FocusSearchBar,     // show the search bar and focus its field
  Search,             // search bar committed (arg = what was typed)
  SelectSearchEngine, // arg = engine id (search.hpp)
  ToggleSearchEngine, // arg = engine id: switch it on / off in the menu
  AddSearchEngine,    // arg = OpenSearch description URL the page offers
  SetLanguages,       // arg = preferred languages, "de, en" (Accept-Language)
  SearchSite,         // location bar: open search bar on the site engine (not a toggle)
  TogglePage,     // navigation / side bars of the focused page, as a panel
  SitemapPrev,    // previous / next page in the site map's order
  SitemapNext,
  OpenTranscludeOther,  // open include target in the other pane
  ToggleDiff,
  ToggleStructure,
  ToggleLock,  // lock / unlock the focused editable document (F2)
  ToggleChrome,  // content only: hide menu bar, toolbars, tabs, status line
  ToggleNative,  // web page ↔ its native xanatoo form (wikipedia:, hn:, …)
  ToggleArchive,  // web page ↔ its nearest Wayback Machine snapshot
  OpenPageFeed,   // location bar feed button (arg = feed URL of the page)
  ViewSourceRendered, // leave a source projection and show the document
  ViewSourceRaw,      // focused document's raw source (if available)
  ViewSourceXml,      // focused document's XML representation
  ToggleGallery,      // focused view: `//gallery` of the page, or back out
  ToggleReader,  // focused view: hide / show the page chrome (nav, aside, …)
  TogglePaged,        // focused view: scrolling ↔ ebook style pages (F8)
  TogglePagedDefault, // Preferences: paged reading for new views
  PageColumns,        // arg: "0" (fit the width) … "3" columns per spread
  CollectionView,  // arg: layout / size / sort / "view" for the collection
  ListFold,  // arg: "toggle" / "fold-all" / "expand-all" (fold_target)
  StretchFold,  // arg: "toggle" (fold_target) / "summaries" / "full"
  ToggleHelp,
  ToggleHoverHelp,  // Help panel: explains the element under the pointer
  ToggleVirtualKeyboard,  // F3 / View — on-screen keyboard
  VirtualKey,             // arg = UTF-8 key ("a", "\n", "\b", " ")
  OpenInWindow,
  OpenFormatDoc,
  OpenAbout,  // generated about: page (build features, services)
  OpenSemanticDoc,
  OpenExample,       // arg = filename under examples/ (or repo root)
  LoadDiffDemo,      // dual-pane diff-v1 | diff-v2 + diff marks on
  ZoomIn,
  ZoomOut,
  // View display (rendering only — not document format)
  ViewLineSpacingTight,
  ViewLineSpacingNormal,
  ViewLineSpacingRelaxed,
  ViewLineSpacingLoose,
  ViewMargins,              // arg = margin percent per side
  ViewMeasure,              // arg = column width in characters, "0" = full
  ToggleSectionFocus,       // outline: body shows one section only
  ToggleJustify,            // view: justify wrapped lines
  ToggleFootnoteOverlay,    // view: notes of on-screen footnote marks
  ToggleBookmarkMarks,      // view: wash over bookmarks inside the page
  ToggleHeadingTicks,       // view: heading ticks on the scrollbar
  ToggleAutoUpdates,         // Android update checks
  ToggleAutoLinks,          // view: plain-text http(s):// URLs become links
  ToggleAutoImages,         // view: image URLs in text show the picture
  ToggleHoverPreview,       // split view: preview hovered links on the right
  ToggleHistoryRewinds,     // History panel: a click steps Back / Forward
  ToggleReaderDefault,      // Preferences: reader mode for new views
  TogglePreferences,        // Edit → Preferences window
  ToggleCredentials,        // Edit → Credentials dialog
  SaveCatalogCredentials,   // arg = six newline-separated private values
  ClearCatalogCredentials,  // remove all saved catalog keys
  SaveRedditCredentials,    // arg = client_id + newline + client_secret
  ClearRedditCredentials,   // remove locally saved Reddit credentials
  SetUiScale,               // Preferences: scale chrome and default text
  SelectStyle,              // arg = style sheet name (styles/<name>.xss)
  SelectStyleColors,        // arg = sheet lending the colours; "" = own
  SelectStyleFonts,         // arg = sheet lending the fonts; "" = own
  ReloadStyles,             // rescan styles/ and re-read the active sheet
  StyleCreate,              // arg = new stem + '\n' + parent stem
  StyleSave,                // arg = stem + '\n' + source
  StyleDelete,              // arg = user-owned stem
  StylePin,                 // arg = domain:host/service:name + '\n' + stem
  StyleUnpin,               // arg = domain:host/service:name
};



// One entry of the Services menu: a client's front page, or for protocols
// with many hosts a directory page of them under examples/services/.
struct UiService {
  std::string label;
  std::string icon;      // name in the shared menu icon set
  std::string target;    // URL / ref, or example file name when `example`
  bool example = false;  // local page from examples/ (OpenExample)
  bool enabled = true;
  std::string tip;       // URL, or why the client is unavailable
  std::string group;     // non-empty: a group heading starts before this entry
};

// One engine of the search bar's menu.
struct UiSearchEngine {
  std::string id, name, keyword, badge;
  uint32_t color = 0;  // tile background, 0xRRGGBB (engine_color)
  bool on = true;      // in the menu (engine_on)
  std::string note;    // why it is off by default (SearchEngine::note)
};

// A search engine the focused page offers (OpenSearch) and that is not in
// the list yet.
struct UiOfferedEngine {
  std::string url, title;
};

// The edit strip on top of one editable view (docs/UI.md "Editable
// documents"): its rect in window pixels and what its buttons need.
struct UiEditBar {
  int view = 0;
  int x = 0, y = 0, w = 0, h = 0;
  bool focused = false;  // the focused view: its rows reflect the caret
  bool floating = false;  // in a document window (drawn with its frame)
  bool locked = false;
  bool dirty = false;
  bool selection = false;
  bool on_list_item = false;
  bool in_stretch = false;
};

// The service strip of a view showing a native service's page (docs/UI.md
// "Service strips"): name, favicon, sections and the service's search.
struct UiServiceBar {
  int view = 0;
  int x = 0, y = 0, w = 0, h = 0;
  bool floating = false;  // in a document window (drawn with its frame)
  std::string id;         // ServiceBar::id (keeps the search text apart)
  std::string name;
  std::string home;
  uint32_t color = 0;  // accent 0xRRGGBB
  SDL_Texture* icon = nullptr;
  std::vector<ServiceRow> rows;
  std::string search_engine;  // engine id; "" = no search field
  std::string search_name;
};

struct UiState {
  // Native FloatWindows determine both the painted and interactive stacking
  // of embedded ImGui forms. Chrome remains on top of all native windows.
  std::vector<UiPanelLayer> native_panel_order;
  std::optional<UiPanelLayer> pointer_native_top;
  float pointer_x = 0.f, pointer_y = 0.f;
  // No pointer over the window (a touch ended): ImGui hovers nothing, even
  // though SDL still reports the mouse where the finger was.
  bool pointer_away = false;
  bool dirty = false;
  bool can_publish = false;
  bool can_make_local_copy = false;
  bool has_path = false;
  bool split = false;
  bool trail = false;
  bool tiles_stacked = false;
  bool outline = false;
  bool section_focus = false;
  bool justify = false;
  bool footnote_overlay = true;
  bool bookmark_marks = true;
  bool heading_ticks = true;
  bool annotations_panel_open = false;
  int annotation_x = 0, annotation_y = 0, annotation_w = 0, annotation_h = 0;
  bool revisions_panel_open = false;
  bool can_show_revisions = false;
  bool nostr_explorer_open = false;
  bool lemmy_explorer_open = false;
  bool lemmy_supported = false;
  int lemmy_tab = 0;
  std::string lemmy_search, lemmy_instance, lemmy_sort;
  bool lemmy_instance_only = false;
  int lemmy_explorer_x = 0, lemmy_explorer_y = 0;
  int lemmy_explorer_w = 0, lemmy_explorer_h = 0;
  std::vector<LemmyDirectoryEntry> lemmy_entries;
  bool matrix_explorer_open = false;
  bool matrix_supported = false;
  bool matrix_has_token = false;
  int matrix_tab = 1;
  std::string matrix_search, matrix_server;
  int matrix_explorer_x = 0, matrix_explorer_y = 0;
  int matrix_explorer_w = 0, matrix_explorer_h = 0;
  std::vector<MatrixDirectoryEntry> matrix_entries;
  int nostr_tab = 0;
  std::string nostr_search;
  int nostr_page = 0;
  bool nostr_people_more = false;
  bool nostr_events_more = false;
  bool nostr_groups_more = false;
  int nostr_explorer_x = 0, nostr_explorer_y = 0;
  int nostr_explorer_w = 0, nostr_explorer_h = 0;
  // Site identity in the address bar (the favicon's place): a Gemini page
  // shows the Gemini icon, a refused changed certificate a warning.
  enum class SiteIdentity { None, Gemini, GeminiChanged };
  SiteIdentity site_identity = SiteIdentity::None;
  std::string site_identity_key;  // host:port, the panel row it opens
  std::string site_identity_tip;
  bool cookies_open = false, cookies_read_only = false, cookie_reveal = false;
  int cookies_x = 0, cookies_y = 0, cookies_w = 0, cookies_h = 0;
  const CookieSnapshot* cookie_snapshot = nullptr;
  std::string cookie_current, cookie_selected, cookie_preview, cookie_error;
  // Certificates panel: one row per known server (pins + seen this session).
  struct CertRow {
    std::string key, host;
    int port = 1965;
    std::string sha256;           // trusted (pinned) fingerprint, may be empty
    std::string presented;        // what the server showed this session
    std::string previous;         // pending change: the pin it differs from
    bool saved = false;           // in the pin store (else session only)
    bool seen = false;            // connected this session
    bool first_contact = false;   // pinned on this session's first visit
    bool changed = false;         // pending change, nothing sent
    bool current = false;         // the focused page's server
    std::string subject, issuer, not_before, not_after, self_signed;
  };
  bool certs_open = false;
  int certs_x = 0, certs_y = 0, certs_w = 0, certs_h = 0;
  std::vector<CertRow> cert_rows;
  std::string cert_selected;
  std::string cert_error;
  bool cert_no_save = false;
  // Identity panel: Gemini client certificates and Nostr accounts.
  IdentitySection identity_section = IdentitySection::Gemini;
  struct IdentityRow {
    std::string sha256, name, created;
    std::vector<std::string> scopes;
    std::string scope_here;  // its scope covering the focused page
    bool active_here = false;  // the focused page goes out with it
    bool saved = false;
  };
  bool identities_open = false;
  int identities_x = 0, identities_y = 0, identities_w = 0, identities_h = 0;
  std::vector<IdentityRow> identity_rows;
  std::string identity_selected;
  std::string identity_error;
  bool identity_no_save = false;
  // The focused Gemini page (empty otherwise) and its two scopes.
  std::string identity_page, identity_page_site, identity_page_scope;
  // That page asked for a certificate (60/61/62) with this text.
  int identity_request_status = 0;
  std::string identity_request_meta;
  std::string identity_active_name;  // identity the page goes out with
  // Related Sites panel: similar websites of the focused page's site.
  struct RelatedRow {
    std::string domain, title, description, category, source;
    float score = 0.f;  // 0..1, 0 = unknown
  };
  bool related_open = false;
  int related_x = 0, related_y = 0, related_w = 0, related_h = 0;
  std::string related_host;    // the page's site, "" = no web page
  std::string related_domain;  // domain the rows belong to
  bool related_loading = false;
  bool related_follow = true;   // list follows the focused page
  bool related_behind = false;  // held, and the focused page is elsewhere
  bool related_similars_net = false;  // rows from similars.net (credit)
  std::vector<RelatedRow> related_rows;
  std::vector<std::string> related_notes;
  NostrCacheStats nostr_stats;
  std::vector<NostrCachedPerson> nostr_people;
  std::vector<NostrCachedEvent> nostr_events;
  std::vector<NostrCachedGroup> nostr_groups;
  std::vector<NostrLocalIdentity> nostr_identities;
  std::string nostr_selected_identity;  // the active account
  std::string nostr_shown_identity;     // the row shown in detail
  std::string nostr_page_pubkey;  // author of the focused nostr: profile page
  bool nostr_can_sign = false;    // this build signs (OpenSSL)
  std::vector<std::string> nostr_key_pubkeys;       // accounts with a key
  std::vector<std::string> nostr_plain_pubkeys;     // … kept without password
  std::vector<std::string> nostr_unlocked_pubkeys;  // … unlocked this session
  std::string nostr_shown_ncryptsec;  // backup of the shown account's key
  bool nostr_posting = false;         // a note is on its way to relays
  std::string nostr_post_status;
  std::string nostr_post_note;        // nostr:note1… of the last note
  std::vector<NostrPublishResult> nostr_post_results;
  uint64_t nostr_post_sent = 0;       // bumped per posted note
  struct AnnotationLayerItem {
    std::string id, name, owner;
    bool visible = true;
    bool active = false;
    size_t count = 0;
  };
  std::vector<AnnotationLayerItem> annotation_layers;
  struct AnnotationItem {
    std::string id, layer_id, owner, kind, body;
    bool resolved = false;
    bool selected = false;
  };
  std::vector<AnnotationItem> annotations;
  struct AnnotationHistoryItem {
    std::string id;
    int64_t time = 0;
    size_t count = 0;
  };
  std::string annotation_history_layer;
  std::vector<AnnotationHistoryItem> annotation_history;
  uint64_t annotation_note_created_gen = 0;
  bool hover_preview = false;
  bool history_rewinds = true;  // ViewSettings::history_rewinds
  bool auto_links = true;  // ViewSettings::auto_links
  bool auto_images = true;  // ViewSettings::auto_images
  bool auto_updates = false;
  bool reader_default = false;  // ViewSettings::reader_mode
  bool reader = false;          // focused view hides the page chrome
  bool paged = false;           // focused view shows pages
  bool paged_default = false;   // ViewSettings::paged
  int page_columns = 0;         // ViewSettings::page_columns
  bool has_chrome = false;      // focused page has page chrome to hide
  bool preferences_open = false;
  bool download_confirm_open = false;
  std::string download_confirm_url;

  bool credentials_open = false;
  bool backlinks = false;
  bool sitemap = false;
  bool page_panel = false;
  bool searchbar = false;  // the search bar row is shown
  bool audio = false;          // the sound player is shown
  bool audio_playing = false;  // … and playing (not paused / ended)
  bool audio_ab = false;       // … with its A–B markers on
  bool audio_loop = false;     // … looping
  bool audio_video = false;       // … showing pictures
  bool audio_fullscreen = false;  // … fullscreen
  bool audio_controls_hidden = false;  // … the panel shows the picture alone
  std::vector<UiSearchEngine> search_engines;
  std::string search_engine;  // id of the engine in use
  std::string site_search;  // engine that searches the focused page's service
  std::vector<UiOfferedEngine> offered_engines;
  bool adding_engine = false;  // a description is being read
  bool has_nav = false;  // focused page has navigation for the panel
  bool has_headings = false;  // focused page has headings for the outline
  bool has_site = false; // focused site is known to have a sitemap (or panel open)
  bool diff = false;
  bool can_diff = false;  // two independent documents loaded
  bool structure = false;
  // Display prefs (for menu checks)
  float view_line_spacing = 1.30f;
  float view_margin_pct = 5.f;
  int view_measure_chars = 0;
  float ui_scale = 1.f;  // preference multiplier on the platform default
  // Style sheets: (name, title) pairs and the active name.
  std::vector<std::pair<std::string, std::string>> styles;
  std::string style;
  bool browse = true;  // read-only navigation (default, or foreign content)
  bool can_edit = false;  // false for foreign/read-only content
  bool chrome_hidden = false;  // content only: no menu bar / toolbars / tabs
  bool document_fullscreen = false;
  bool virtual_keyboard = false;
  // Gamepad right-stick software pointer (extra; real mouse stays separate)
  bool stick_cursor = false;
  bool stick_active = false;  // axis moved or A/B held this frame
  float stick_cx = 0.f, stick_cy = 0.f;
  bool stick_lmb = false, stick_rmb = false;
  bool help = false;
  bool hover_help = false;  // Help panel open
  bool selection = false;
  std::string context_comment_native;  // passage address for a right-clicked HN comment
  std::string context_comment_web;     // external HN per-comment permalink
  bool context_can_reply = false;
  std::vector<std::string> context_quote_items;  // "Go to Quoted Message (…)"
  std::string context_image_address;
  bool context_save_pdf = false;
  std::string context_link;       // right-clicked link target, if any
  std::string context_link_href;  // … as written in the page
  bool context_link_web = false;  // … and it is http(s): can be downloaded
  std::string context_link_rel;   // "mention" / "tag" (social links), or ""
  std::string context_link_text;  // … its text: "@alice", "#tag"
  // Blocks around the right-click, innermost first ("Article · Title"), and
  // the innermost Article / Section among them (-1 = none).
  std::vector<std::string> context_blocks;
  int context_article = -1;
  int context_section = -1;
  // Edit strips of the editable views shown.
  std::vector<UiEditBar> edit_bars;
  // Service strips of the service pages shown.
  std::vector<UiServiceBar> service_bars;
  // Find bar of the focused view (App::prompt).
  bool find_open = false, find_replace = false;
  int find_x = 0, find_y = 0, find_w = 0, find_h = 0;
  std::string find_text, find_replacement;
  uint64_t find_focus_seq = 0;   // a new value focuses the field
  bool find_select_all = false;  // … with its text selected
  size_t find_count = 0, find_index = 0;  // "index of count", 0 = none
  bool find_capped = false;
  bool find_can_replace = false;
  bool reply_open = false;
  // Embedded panels whose native window is rolled up (`ui_panel_bit`): their
  // ImGui body is not built while the frame shows only its title.
  unsigned shaded_panels = 0;
  unsigned left_docked_panels = 0;
  uint64_t reply_generation = 0;
  std::string reply_author, reply_page, reply_label, reply_text;
  std::string reply_excerpt;     // start of the message being answered
  bool reply_nostr = false;      // the target is a Nostr note: Send posts
  std::string reply_account;     // unlocked active Nostr account, or empty
  std::string reply_blocker;     // why Send cannot post now
  bool reply_sending = false;
  std::string reply_status;      // the last send's outcome
  uint64_t reply_sent = 0;       // bumped per posted reply
  int reply_x = 0, reply_y = 0, reply_w = 0, reply_h = 0;
  // True when caret is on a ListItem (enables indent/outdent menu items).
  bool on_list_item = false;
  // True when caret is inside a Stretch (enables edit summary).
  bool in_stretch = false;
  bool can_nav_back = false;
  bool can_nav_forward = false;
  // snap:ID/URL of the focused page as shown (snapshot.hpp); '' none.
  std::string snapshot_address;
  std::vector<HistoryMenuItem> back_history;
  std::vector<HistoryMenuItem> forward_history;
  // Address bar suggestions: open tabs, bookmarks, recent visits, newest first.
  std::vector<UrlBarSuggestion> urlbar_entries;
  // Other view of the focused page ("" = none): the native ref of a web
  // page (wikipedia:Title, hn:item?id=1) or the web URL of a native view.
  std::string alternate_url;
  // http(s) address of a page shown under another one (hn:, feed:, search:,
  // …); "" for web pages themselves and pages not on the web.
  std::string web_page_url;
  std::string browser_url;  // what Open in Browser opens; "" = nothing
  // Wayback Machine: where ToggleArchive goes ("" = not available), and
  // whether the focused page already is a snapshot (target = the live page).
  std::string archive_url;
  bool archive_view = false;
  std::string up_url;  // parent page of the focused one ("" at the top)
  // RSS / Atom feeds the focused web page announces (URL-bar feed button).
  struct PageFeed {
    std::string url, label;
  };
  std::vector<PageFeed> page_feeds;
  // Versions of the focused page in other languages (hreflang), and the
  // preferred languages as typed ("de, en").
  struct PageLanguage {
    std::string url, label;
    std::string fallback_label;  // English name, when the font lacks the script
    bool current = false;
  };
  std::vector<PageLanguage> page_languages;
  std::string languages;
  bool native_view = false;  // focused page is a native ref (alternate = web)
  std::string source_mode;  // empty = rendered, otherwise source-raw/sexpr/xml
  bool gallery = false;      // the focused view shows `//gallery`
  // Caret / right-click inside a Collection: its current view for the
  // Collection menus (layout 0 grid, 1 list, 2 full; sort 0 doc, 1 name,
  // 2 name reversed; card size in px).
  std::vector<UiService> services;
  // Right-click in a list that folds: 0 no, 1 on an item without a list of
  // its own, 2 on an expanded item, 3 on a folded one.
  int list_fold = 0;
  // Right-click in two-body stretchtext: 0 no, 1 full text, 2 summary shown.
  int stretch_fold = 0;
  bool has_summaries = false;  // the page has two-body stretchtext
  bool collection = false;
  int collection_layout = 0;
  int collection_sort = 0;
  float collection_thumb = 160.f;
  bool loading = false;  // a page fetch is running (Reload becomes Stop)
  bool history_open = false;
  bool vertical_tabs_open = false;
  bool tab_bar_visible = true;
  std::vector<std::string> recent_visited;  // newest first, for the History menu
  bool downloads_open = false;
  bool media_open = false;
  // The Downloads row of an open right-click menu (empty name: none).
  struct DownloadMenu {
    std::string name, path, url;
    bool active = false;    // still transferring: only Cancel applies
    bool complete = false;  // finished and the file still exists
  } download_menu;
  // The History row of an open right-click menu (empty address: none).
  struct HistoryMenu {
    std::string label, address;
    bool from_db = false;  // a logged visit (else a session address)
    bool rewinds = true;   // ViewSettings::history_rewinds (Shift flips)
  } history_menu;
  // The Media panel's open right-click menu (empty title: none).
  struct MediaMenu {
    std::string title, time;     // the file, and the position as m:ss
    bool time_bookmarked = false;  // a bookmark at that position
    bool file_bookmarked = false;
    bool seekable = false;
    bool ab = false, loop = false;  // markers on, looping
    bool video = false, fullscreen = false;  // pictures; filling the screen
    bool controls_hidden = false;            // the panel shows the picture alone
    std::string section;            // "0:10–0:20" ("" = no section)
    bool section_bookmarked = false;
    bool section_selected = false;  // it is the A–B markers' section
  } media_menu;
  bool styles_open = false;
  bool styles_manager_open = false;
  int styles_manager_x = 0, styles_manager_y = 0, styles_manager_w = 0,
      styles_manager_h = 0;
  int styles_x = 0, styles_y = 0, styles_w = 0, styles_h = 0;
  std::vector<StyleEntry> style_entries;
  StylePins style_pins;
  uint64_t style_save_generation = 0;
  std::vector<std::string> style_pin_candidates;
  std::string default_style;
  // Sheets lending colours / fonts to the applied one ("" = its own).
  std::string style_colors;
  std::string style_fonts;
  bool bookmarks_open = false;
  int bookmarks_x = 0, bookmarks_y = 0;
  int bookmarks_w = 0, bookmarks_h = 0;
  bool context_open = false;
  bool context_has_doc = false;  // real Document+layout; skip ImGui body
  int context_x = 0, context_y = 0, context_w = 0, context_h = 0;
  std::string context_kind;
  std::string context_title;
  std::string context_body;
  std::string context_meta;
  bool context_can_jump = false;
  int context_jump_pane = -1;
  long context_jump_link = -1;
  bool context_jump_to_source = true;
  bool current_bookmarked = false;
  bool page_bookmarked = false;
  int page_bookmarks = 0;  // bookmarks of passages inside the focused page
  // The bookmark a margin-icon menu is for (empty address: none).
  struct BookmarkMenu {
    std::string address, label, cut;  // cut: the passage as a cut address
    std::string folder;
  } bookmark_menu;
  // Persistent bookmarks for the Bookmarks menu / panel.
  std::vector<Bookmark> bookmarks;
  std::vector<std::string> bookmark_folders;  // all, sorted
  bool subscriptions_open = false;
  int subscriptions_x = 0, subscriptions_y = 0;
  int subscriptions_w = 0, subscriptions_h = 0;
  bool current_subscribable = false;
  bool current_subscribed = false;
  std::string subscription_address;
  // Excludes the potentially large per-source recent-ID snapshots; the UI
  // needs only status data, not the change-detection state.
  struct SubscriptionItem {
    std::string address, title, error;
    int interval_minutes = 15;
    int64_t checked_at = 0;
    uint32_t unread = 0;
    bool initialized = false, pending = false;
  };
  std::vector<SubscriptionItem> subscriptions;
  SDL_Texture* location_icon = nullptr;
  // Without a real favicon: an icon name from pseudo_favicon.hpp, or for a
  // search: address the id of the engine whose tile stands in.
  std::string location_pseudo_icon;
  std::string location_engine;
  std::vector<SDL_Texture*> bookmark_icons;
  // Tab strip (labels left→right); active_tab is index into tab_labels.
  std::vector<std::string> tab_labels;
  std::vector<uint32_t> tab_roots;  // root opener of each tab tree
  std::vector<uint32_t> tab_ids;  // buffer id of each tab (stable ImGui ids)
  std::vector<SDL_Texture*> tab_icons;
  std::vector<uint8_t> tab_editable;  // 1: editable, unlocked (green tab)
  std::vector<uint32_t> tab_service_color;  // service accent; 0 = none
  int active_tab = 0;
  bool has_floating = false;
  std::string status_left;
  std::string doc_label;
};

// The ImGui widget under the pointer, for the Help panel (empty label:
// none): its ImGui label (or the explicit help key of a toolbar button),
// its tooltip if the widget has a fixed one, and the root ImGui window
// under the pointer (also set without a widget).
struct UiHoverItem {
  std::string label;
  std::string tip;
  std::string window;
};

struct UiFrameResult {
  UiCommand cmd = UiCommand::None;
  // Pixels reserved at the top of the window for menu + toolbar.
  float top_reserved = 0.f;
  float bottom_reserved = 0.f;  // virtual keyboard, etc.
  // Optional payload (e.g. example filename for OpenExample).
  std::string arg;
  // A tab is being dragged away from the strip (the app shows where a drop
  // would dock it).
  bool tab_drag_out = false;
  // Index into UiState::context_blocks whose menu item is under the
  // pointer (the block is washed in the page), -1 = none.
  int context_block_hot = -1;
  // View whose edit strip `cmd` came from (-1 = elsewhere): it is focused
  // before the command runs.
  int edit_bar_view = -1;
  std::optional<std::pair<UiCommand, std::string>> style_hover;
};

class Ui {
public:
  Ui() = default;
  ~Ui();

  Ui(const Ui&) = delete;
  Ui& operator=(const Ui&) = delete;

  // ui_scale: chrome factor (font, padding, icons, toolbar rows); 1 = 14 px
  // menu font and 32 px toolbar icons before effective display/user scaling.
  bool init(SDL_Window* window, SDL_Renderer* renderer, float ui_scale = 1.f);
  void shutdown();

  void process_event(const SDL_Event& ev);

  bool want_capture_keyboard() const;
  bool want_capture_mouse() const;
  // An ImGui popup (menu) is open.
  bool popup_open() const;
  // True when ImGui chrome (any window from the last frame, or an open popup)
  // owns window point (x, y). Unlike want_capture_mouse() this does not
  // depend on where the mouse was last frame, so touches can be routed.
  // Touch can test the gutter beneath its own popup before dismissing it.
  bool captures_point(float x, float y,
                      std::optional<UiPanelLayer> native_top = std::nullopt,
                      bool dismiss_popup = false) const;
  // The embedded panel body under (x, y), nullopt for chrome, popups (an open
  // one covers everything) or no ImGui window.
  std::optional<UiPanelLayer> panel_at(float x, float y) const;
  // Used by event routing so a hidden embedded editor does not receive a
  // queued mouse press/wheel before ImGui's next NewFrame().
  bool pointer_obscured(float x, float y,
                        std::optional<UiPanelLayer> native_top) const;

  // After a menu/toolbar command, drop ImGui's active widget so the next
  // keystroke goes to the document instead of the chrome.
  void release_input_capture();
  // Help panel: while on, every frame notes the widget under the pointer
  // (ImGui's item hooks), read with hover_item() after the frame.
  void set_hover_tracking(bool on) { hover_tracking_ = on; }
  const UiHoverItem& hover_item() const { return hover_item_; }

  // The canvas (not ImGui) chose the pointer for this frame. The ImGui SDL
  // backends apply ImGui's cursor in the next new_frame; telling ImGui the
  // same shape keeps them from flipping it back to an arrow every frame.
  void claim_cursor(xanatoo::sdl::SystemCursor kind);

  // Open a top-level menu by name ("File", "Edit", …) for Alt+letter mnemonics.
  void request_menu(const char* name);

  // Focus the toolbar location / URL field (Ctrl+L).
  void focus_url_bar();
  // Focus the search bar field (once the bar is shown).
  void focus_search();

  // Request a context menu at window coordinates (call from main on right-click).
  void open_context_menu(float x, float y);
  // Right-click on the chrome of a document FloatWindow (exact buffer id).
  void open_window_menu(float x, float y, uint32_t buffer_id);
  // Divider badge / right-click: relationship and orientation menu.
  void open_tile_menu(float x, float y);
  // The menu of a bookmark's margin icon (UiState::bookmark_menu).
  void open_bookmark_menu(float x, float y);
  // The menu of a Downloads panel row (UiState::download_menu).
  void open_download_menu(float x, float y);
  // The menu of a History panel row (UiState::history_menu).
  void open_history_menu(float x, float y);
  // The Media panel's seek bar menu (UiState::media_menu).
  void open_media_menu(float x, float y);
  // A touch long press fired while the finger is still down. ImGui's left
  // button (SDL's touch-synthesised mouse) is let go now, and the widget it
  // holds is dropped without a click, so the lifting finger presses nothing
  // (not a button, nor a menu item that opened under it). With
  // `right_click`, a right click follows where ImGui's pointer is: the
  // context menu of the URL bar, the search bar, a tab, a toolbar button…
  void touch_long_press(bool right_click);
  // Close every open popup (a touch menu the finger then dragged away from).
  void close_popups();

  UiFrameResult begin_frame(const UiState& state);
  // A text field of the find bar has the keys.
  bool find_field_active() const { return find_field_active_; }
  // Finalize ImGui's command lists before native panels are painted. Each
  // embedded form is then drawn immediately after its owning native frame;
  // application chrome and popups are painted last in end_frame().
  void prepare_render();
  void render_embedded_panel(SDL_Renderer* renderer, UiPanelLayer panel);
  void end_frame(SDL_Renderer* renderer);

  bool ready() const { return ready_; }

private:
  void publish_text_field_selection();
  void resolve_middle_click();
  struct Icon {
    SDL_Texture* texture = nullptr;
    int w = 0;
    int h = 0;
  };

  void apply_classic_theme();
  void load_icons(SDL_Renderer* renderer);
  const Icon* icon(const char* name) const;
  bool tool_button(const char* id, const char* icon_name, const char* tip,
                   UiCommand c, UiCommand& out, bool enabled = true,
                   bool* right_clicked = nullptr, bool latched = false);
  void find_bar(const UiState& state, UiCommand& cmd, std::string& arg);
  void edit_bars(const UiState& state, UiFrameResult& result, UiCommand& cmd);
  void service_bars(const UiState& state, UiFrameResult& result,
                    UiCommand& cmd);
  bool tool_menu_begin(const char* id, const char* icon_name, const char* tip);
  void toolbars(const UiState& state, UiFrameResult& result, UiCommand& cmd);
  void draw_url_suggestions(const UiState& state, UiFrameResult& result,
                            UiCommand& cmd);
  void tool_menu_end();

  bool ready_ = false;
  bool hover_tracking_ = false;
  UiHoverItem hover_item_;
  bool render_prepared_ = false;
  bool layered_panels_ = false;
  float scale_ = 1.f;
  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  std::unordered_map<std::string, Icon> icons_;

  bool context_open_ = false;
  // touch_long_press: drop ImGui's active widget each frame until its left
  // button is up (the release may trickle in a frame late).
  bool touch_cancel_press_ = false;
  bool close_popups_ = false;
  bool window_menu_open_ = false;
  uint32_t window_menu_buffer_id_ = 0;
  float window_menu_x_ = 0.f;
  float window_menu_y_ = 0.f;
  bool tile_menu_open_ = false;
  bool bookmark_menu_open_ = false;
  bool download_menu_open_ = false;
  bool history_menu_open_ = false;
  bool media_menu_open_ = false;
  float context_x_ = 0.f;
  float context_y_ = 0.f;
  float tile_menu_x_ = 0.f;
  float tile_menu_y_ = 0.f;

  // Pending Alt+letter menu to open on the next begin_frame().
  std::string menu_request_;

  // Interactive location / URL field (toolbar).
  char url_input_[2048] = {};
  std::string url_synced_from_;  // last doc_label pushed into url_input_
  bool focus_url_bar_ = false;   // Ctrl+L / Open URL menu
  bool url_suggestions_open_ = false;
  bool url_suggestions_dismissed_ = false;  // Esc until next focus/edit
  bool url_field_active_ = false;
  int url_suggestion_selected_ = -1;  // no suggestion: Enter uses typed URL
  std::string url_suggestion_query_;
  float url_suggestion_x_ = 0.f, url_suggestion_y_ = 0.f;
  float url_suggestion_w_ = 0.f;
  bool url_context_was_active_ = false;
  bool url_context_was_open_ = false;
  bool url_context_resume_ = false;
  bool url_context_select_all_ = false;
  struct UrlContextEdit {
    int begin, end;
    std::string replacement;
  };
  std::optional<UrlContextEdit> url_context_edit_;
  // X11 / Wayland primary selection in ImGui text fields
  // (publish_text_field_selection, process_event).
  struct FieldSelection {
    unsigned int id = 0;
    std::string text;
  } field_primary_;
  // A middle press waits for the frame that lays out the widgets under it:
  // over a text field it becomes a left click + typed text (its release is
  // then swallowed, and the URL bar does not select all on that
  // activation), elsewhere it is passed on late (resolve_middle_click).
  struct MiddleClick {
    bool pending = false;
    bool released = false;
  } middle_click_;
  bool middle_paste_release_ = false;
  int middle_paste_frames_ = 0;
  char search_input_[512] = {};  // search bar text (kept between searches)
  // Preferred languages field (URL-bar language menu, Preferences).
  char languages_input_[256] = {};
  std::string languages_synced_;
  bool languages_editing_ = false;
  void languages_field(const UiState& state, UiCommand& cmd, std::string& arg,
                       const char* id);
  char subscription_input_[1024] = {};
  // Bookmarks panel: filter, the row being named inline ("b:" address,
  // "f:" folder, "n:" parent of a new folder; empty = none) and the folder
  // its Bookmark / New Folder buttons work in.
  char bm_filter_[256] = {};
  char bm_edit_[512] = {};
  std::string bm_edit_key_;
  bool bm_edit_focus_ = false;
  std::string bm_target_folder_;
  void bookmarks_panel(const UiState& state, UiCommand& cmd, std::string& arg);
  std::string subscription_remove_confirm_;
  char nostr_local_search_[512] = {};
  char nostr_group_relay_[512] = {};
  char lemmy_search_input_[256] = {};
  char lemmy_instance_input_[256] = {};
  char matrix_search_input_[256] = {};
  char matrix_open_input_[512] = {};
  std::string style_editor_sheet_;
  int style_panel_tab_ = 0;  // Style Sheets panel: 0 sheet, 1 colours, 2 fonts
  std::string style_editor_loaded_;
  char style_editor_[65536] = {};
  char style_new_name_[81] = {};
  char identity_new_name_[65] = {};
  char identity_rename_[65] = {};
  std::string identity_rename_for_;  // sha256 the rename field holds
  std::string identity_delete_armed_;  // sha256 whose Delete asks again
  char style_body_font_[128] = {};
  char style_heading_font_[128] = {};
  char style_code_font_[128] = {};
  char style_font_filter_[128] = {};
  char style_pin_input_[256] = {};
  bool style_editor_dirty_ = false;
  uint64_t style_save_seen_ = 0;
  bool style_delete_confirm_ = false;
  char nostr_identity_label_[160] = {};
  char nostr_identity_pubkey_[256] = {};
  char nostr_identity_rename_[160] = {};
  std::string nostr_identity_rename_for_;  // pubkey the rename field holds
  // Signing keys: password / import fields (wiped after use), the armed
  // reveal and delete buttons, the note being written.
  char nostr_key_password_[256] = {};
  char nostr_key_password2_[256] = {};
  char nostr_key_import_[512] = {};
  char nostr_unlock_password_[256] = {};
  bool nostr_no_password_ = false;  // create / import without a password
  // Set / change / remove the shown key's password (armed per public key).
  std::string nostr_change_pw_for_;
  char nostr_new_password_[256] = {};
  char nostr_new_password2_[256] = {};
  int nostr_new_mode_ = 0;  // 0 create, 1 import, 2 watch-only
  std::string nostr_delete_key_armed_;
  std::string nostr_reveal_armed_;
  char nostr_note_[4096] = {};
  uint64_t nostr_note_sent_seen_ = 0;
  // Tab the panel was last told to show (SetSelected once per change).
  IdentitySection identity_section_shown_ = IdentitySection::Gemini;
  bool identity_section_init_ = false;
  int identity_section_settle_ = 0;  // frames to ignore the shown tab
  char find_input_[1024] = {};
  // Search fields of the service strips, by ServiceBar::id.
  std::unordered_map<std::string, std::string> service_search_;
  char find_replace_input_[1024] = {};
  // Texts last taken from the app; anything else is typing it has not seen.
  std::string find_text_seen_, find_replacement_seen_;
  uint64_t find_seq_seen_ = 0;
  bool find_field_active_ = false;
  bool enter_shift_ = false, enter_ctrl_ = false;  // of the last Enter press
  bool find_refocus_ = false;  // a button was pressed: back to the field
  // Frames left in which the activated field gets its caret / selection.
  int find_caret_frames_ = 0;
  bool find_caret_select_ = false;
  char reply_input_[16384] = {}; // Markdown draft, not sent to any server
  uint64_t reply_generation_seen_ = 0;
  uint64_t reply_sent_seen_ = 0;
  char annotation_new_note_[16384] = {};
  char annotation_edit_note_[16384] = {};
  std::string annotation_edit_synced_id_;
  std::string annotation_edit_synced_body_;
  double annotation_note_last_edit_ = 0.0;
  uint64_t annotation_note_created_seen_ = 0;
  struct ReplySelection { int begin = 0, end = 0; } reply_selection_;
  bool reply_focus_ = false;
  bool reply_restore_selection_ = false;
  char catalog_credential_input_[catalog_key_count][512] = {};
  bool catalog_credential_shown_[catalog_key_count] = {};
  bool catalog_credentials_loaded_ = false;
  char reddit_client_id_[256] = {};
  char reddit_client_secret_[256] = {};
  bool reddit_credentials_loaded_ = false;
  bool reddit_secret_visible_ = false;
  bool focus_search_ = false;    // Ctrl+K / View → Search Bar
  // The search field's context menu: the range it acts on (byte offsets;
  // the caret when nothing was selected) and a pending Select All.
  int search_context_begin_ = 0;
  int search_context_end_ = 0;
  bool search_select_all_ = false;
  bool url_bar_editing_ = false;
  bool wanted_text_input_ = false;  // io.WantTextInput last frame

  // Tab bar: one-shot SetSelected so keyboard Next/Prev syncs ImGui without
  // fighting click selection every frame (which caused flicker).
  bool tab_force_select_ = false;
  std::vector<uint32_t> tab_submitted_ids_;
  uint32_t tab_synced_active_ = 0;
  // Tab being dragged off the strip (buffer id, 0 = none) until release.
  uint32_t tab_tear_id_ = 0;
};

}  // namespace xanatoo
