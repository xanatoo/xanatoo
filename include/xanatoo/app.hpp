// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include "xanatoo/media.hpp"
#include "xanatoo/sdl.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "xanatoo/cli.hpp"
#include "xanatoo/download.hpp"
#include "xanatoo/editor.hpp"
#include "xanatoo/feed.hpp"
#include "xanatoo/page_links.hpp"
#include "xanatoo/font.hpp"
#include "xanatoo/gallery.hpp"
#include "xanatoo/gemini.hpp"
#include "xanatoo/gemini_tofu.hpp"
#include "xanatoo/gemini_identity.hpp"
#include "xanatoo/history_trail.hpp"
#include "xanatoo/images.hpp"
#include "xanatoo/layout.hpp"
#include "xanatoo/fold.hpp"
#include "xanatoo/locator.hpp"
#include "xanatoo/minibuffer.hpp"
#include "xanatoo/nav_history.hpp"
#include "xanatoo/nostr.hpp"
#include "xanatoo/nostr_keys.hpp"
#include "xanatoo/nostr_store.hpp"
#include "xanatoo/paginate.hpp"
#include "xanatoo/quote_links.hpp"
#include "xanatoo/render.hpp"
#include "xanatoo/revstore.hpp"
#include "xanatoo/sidestore.hpp"
#include "xanatoo/transfer_meter.hpp"
#include "xanatoo/annotations.hpp"
#include "xanatoo/search.hpp"
#include "xanatoo/service_bar.hpp"
#include "xanatoo/related_sites.hpp"
#include "xanatoo/sitemap.hpp"
#include "xanatoo/types.hpp"
#include "xanatoo/ui.hpp"
#include "xanatoo/tile_tree.hpp"
#include "xanatoo/tls.hpp"
#include "xanatoo/visits.hpp"
#include "xanatoo/subscriptions.hpp"
#include "xanatoo/view_settings.hpp"

// Application world: plain data (App + the records below) and free-function
// "systems" that read/transform it. No behaviour lives on the data types.
namespace xanatoo::app {

struct App;

// Chrome metrics in px at ui scale 1. Variables, not constants: app_init and
// apply_ui_scale multiply them by the effective display/user scale.
inline int kSbW = 14;
inline int kSbLayoutGutter = kSbW + 4;
constexpr float kBlockGripW = 18.f;
constexpr int soft_snap_px = 16;
constexpr int soft_dock_zone = 36;
// App::panes slots: two centre views (split), the rest for document windows.
constexpr int kCentreViews = 2;
constexpr int kMaxViews = 16;
// Geometry and relationships are independent: a pair of tiles can be
// unlinked, linked as a navigation trail, or compared via diff_mode.
enum class TileMode { Independent, Trail };
inline int status_bar_h = 28;       // 0 while content only hides it
inline int status_bar_full_h = 28;  // scaled height when shown
constexpr int kMaxNavHistory = 100;
inline int help_title_h = 28;   // title strip of a free (undocked) window
inline int float_close_w = 28;  // width of one title control hit zone (free)
// Docked / maximized windows: a slimmer title strip whose controls fill it
// flush (square, no padding) and a smaller title font. Free windows keep
// the taller strip so their edges stay easy to grab for resizing.
inline int dock_title_h = 20;
inline int kUiSmallFontPx = 12;
// Canvas chrome text (status line, window titles, outline / backlinks);
// matches the ImGui menu font and ignores document style + zoom.
inline int kUiFontPx = 14;
inline constexpr int kStructTagPx = 9;
inline int help_pad = 12;
inline int help_edge = 6;
// Gutter between docked panels and between a dock and the panes; the dock
// separators and resize edges are grabbed in it, clear of scrollbars.
inline int dock_gap = 8;
inline int help_grip_h = 14;  // strip under the client well: resize grip / h-scroll

// float_title_h / float_control_w / window_client_rect follow FloatWindow.

// Scale the chrome metrics above (call before layout and when the UI scale changes).
void set_chrome_scale(float scale);
void apply_ui_scale(App& app);

// Stable identity of a document buffer; never reused, 0 = none.
using BufferId = uint32_t;
constexpr BufferId kNoBuffer = 0;

// Quote links of a thread document (quote_links.hpp, docs/QUOTES.md):
// derived data, rebuilt by quote_links_of when the document changed.
struct QuoteLinks {
  bool valid = false;
  uint64_t doc_gen = 0;  // DocBuffer::doc_gen the links were found in
  size_t nodes = 0;
  xanatoo::NodeId next_id = 0;
  std::vector<xanatoo::QuoteLink> links;
};

// Identifies a buffer's document as of its last change.
struct SummaryScan {
  BufferId buf = kNoBuffer;
  const xanatoo::Document* doc = nullptr;
  uint64_t doc_gen = 0;
  size_t nodes = 0;
  bool operator==(const SummaryScan&) const = default;
};

struct DocBuffer {
  BufferId id = kNoBuffer;
  NavHistory tab_nav;
  std::vector<int64_t> inherited_visits;  // Visit rows copied from the opener
  std::string history_tab;  // Visit log identity, saved with the session
  BufferId tab_parent = kNoBuffer;  // Opener, independent of the current page
  xanatoo::Document doc;
  // Lists and stretches the reader folded / unfolded (fold.hpp); gone with `doc`.
  xanatoo::Folds folds;
  // Bumped when `doc` changes (note_edit, refresh_views_of_buf); derived
  // per-document data compares it.
  uint64_t doc_gen = 0;
  QuoteLinks quotes;
  // The page's address (Location): what the user opened — a local file, a
  // URL — never a derived file under cache_dir(). Reload goes through it.
  std::string path;
  // The local file the document was read from when that is not `path`:
  // generated Markdown or a cached fixed-layout download. Relative images
  // and links resolve against it (`content_file`). Empty = `path`.
  std::string content_path;
  std::optional<std::string> raw_source;  // HTTP/Gemini body before parsing
  // What the fetch returned (visits.hpp); empty for local / generated
  // pages. `observation_id`: its row once a visit stored it (0 = not yet).
  xanatoo::PageObservation observed;
  int64_t observation_id = 0;
  // A tab opened from a link: how its first visit was reached.
  std::optional<xanatoo::VisitKind> origin_kind;
  int64_t origin_visit = 0;
  bool dirty = false;
  xanatoo::History history;
  // Shows a "Loading …" stand-in until a background fetch replaces it.
  bool placeholder = false;
  bool blank_tab = false;  // An empty, addressless tab awaiting navigation.
  // Local revisions (revstore.hpp, app_drafts.cpp): doc key, content id of
  // the buffer's serialized text when clean, of the last draft written, and
  // when unsaved edits are next written (SDL ticks, 0 = nothing pending).
  std::string doc_key;
  std::string base_id;
  std::string draft_id;
  std::string stored_working;   // database-native head (no file path)
  std::string stored_title;     // friendly local tab label
  Uint32 draft_due = 0;
  // The lock (F2, the edit strip's lock button) the user set on the page at
  // `lock_path`; any other page this buffer shows takes default_locked.
  bool lock_set = false;
  std::string lock_path;
  bool lock_choice = false;
};

struct ClosedTab {
  BufferId id = kNoBuffer;
  size_t index = 0;
  std::optional<DocBuffer> buffer;  // A floating view may still own the buffer.
};

// Cached raster of a pane's document layer (background, selection, text) so
// scrolling only shifts the old pixels and draws the newly exposed strip.
// Two render-target textures ping-pong because SDL cannot copy a texture onto
// itself. Overlays (caret, hover, focus bar, …) are drawn on top every frame.
struct PaneRaster {
  SDL_Texture* tex[2] = {nullptr, nullptr};
  int cur = 0;  // index of the texture holding the current image
  int w = 0, h = 0;
  bool valid = false;
  // What the current image was drawn from:
  uint64_t layout_gen = 0;
  float scroll = 0.f;  // integer-valued texture-space scroll
  xanatoo::Selection sel{};
  bool search_tint = false;
  uint64_t visited_version = 0;  // Pane::visited_links.version
};

// Notes of the footnote marks visible in a pane, laid out for the strip at
// the pane's bottom (app_footnotes.cpp). Rebuilt when the marks or the
// pane's layout change; height 0 = not shown this frame.
struct FootnoteOverlay {
  uint64_t layout_gen = 0;  // pane layout the notes were laid out for
  int width = 0;
  std::vector<std::string> keys;
  std::vector<xanatoo::NodeId> notes;  // source Footnote per laid-out note
  std::vector<float> note_y;           // top of each note in `layout`
  std::vector<float> note_bottom;      // bottom of each note's last line
  // The strip shows the first `shown` notes (marks sorted top to bottom),
  // so it can stay clear of the marks it explains (fit_footnote_strip);
  // strip_h[n] = height of the columns of the first n notes.
  std::vector<float> note_h;  // each note incl. the space to the next
  std::vector<float> strip_h;
  size_t shown = 0;
  size_t max_columns = 1;
  float left = 0.f;  // text column's left edge, pane-local
  // Notes are laid out once at one column's width and shown in columns side
  // by side, each a run of whole notes (as many as the text width allows).
  struct Column {
    size_t first = 0;  // first note (index into notes / note_y)
    float x = 0.f;     // left edge of the column's text, pane-local
    float y0 = 0.f;    // layout y shown at the strip's top pad
    float h = 0.f;     // height incl. pads
  };
  std::vector<Column> columns;
  float column_w = 0.f;
  float gap = 0.f;
  float pad = 0.f;
  xanatoo::Document doc;  // the notes, as laid out in `layout`
  xanatoo::LayoutResult layout;
  float content_h = 0.f;  // tallest column of the shown notes
  float y = 0.f;  // strip top, viewport y (0 = top of the pane content)
  float h = 0.f;
  float w = 0.f;
};

// One document view: a buffer shown at some scroll position with its own
// caret, layout and raster. App::panes[0] / [1] are the centre slots (split
// view); document windows own one of the slots from kCentreViews up
// (FloatWindow::view). x / y / w / h is the content rect in window pixels
// (the scrollbar strip is the rightmost kSbW of it).
// A document position and where in its view it is to stay while the page
// changes around it (zoom, reflow, images arriving). `view_y` is the
// position's baseline relative to the pane content.
struct ViewAnchor {
  BufferId buf = kNoBuffer;
  xanatoo::Caret pos;
  float view_y = 0.f;
};

// Scrollbar ticks of a pane: section / heading positions and, on demand,
// the (estimated) top of every block (app_frame.cpp section_marks). Both
// walk the whole document, so they are kept until the layout changes.
struct SectionMark {
  float y = 0.f;  // document space
  int level = 1;
};
struct ScrollMarks {
  uint64_t layout_gen = 0;  // 0 = not built
  BufferId buf = kNoBuffer;
  std::vector<SectionMark> sections;
  bool has_block_y = false;
  std::unordered_map<xanatoo::NodeId, float> block_y;
};

// The hits of the find query, kept while the query and the document stay the
// same (find_hits): the bar's count and the first scrollbar ticks.
struct SearchTicks {
  std::string query;
  const xanatoo::Document* doc = nullptr;
  BufferId buf = kNoBuffer;
  uint64_t doc_gen = 0;
  size_t nodes = 0;
  std::vector<xanatoo::Caret> hits;
};

// Right-hand status panel counts of a pane (word / char totals or of the
// selection), kept while the layout and selection stay the same.
struct StatsCache {
  uint64_t layout_gen = 0;  // 0 = not built
  BufferId buf = kNoBuffer;
  xanatoo::Selection sel{};
  std::string text;
};

// A bookmark found in the page a view shows.
struct BookmarkMark {
  std::string address;      // the bookmark's, as stored
  xanatoo::Selection range;  // empty: a reading position
};
// Bookmarks inside the page a view shows, resolved for drawing over it
// (update_bookmark_marks). Keyed on what they were found in.
struct BookmarkMarks {
  BufferId buf = kNoBuffer;
  uint64_t bookmarks_gen = 0;
  uint64_t layout_gen = 0;
  Uint32 at = 0;  // SDL ticks of the last search (throttles while typing)
  std::vector<BookmarkMark> items;
};

// Links of the page a view shows whose target another tab already shows
// (update_open_tab_links), drawn as a dotted underline. Keyed on the layout
// and the set of open tab pages.
struct OpenTabLinks {
  uint64_t layout_gen = 0;
  std::string tabs_key;  // page addresses of the other tabs, '\n'-joined
  bool valid = false;
  std::vector<std::pair<xanatoo::NodeId, size_t>> spans;  // node, span index
};

// Links of the page a view shows whose target is the file in the Media
// panel (update_playing_links), washed while it is loaded there. Keyed on
// the layout and the player's address.
struct PlayingLinks {
  uint64_t layout_gen = 0;
  std::string url;  // AudioPlayer::url they were found for ("" = none)
  bool valid = false;
  std::vector<std::pair<xanatoo::NodeId, size_t>> spans;  // node, span index
};

// Links of the page a view shows whose target page was visited before
// (update_visited_links), drawn in the style's `visited-link` colour.
// Keyed on the layout and App::visited_gen; `version` changes with `glyphs`
// so the pane raster knows to redraw.
struct VisitedLinks {
  uint64_t layout_gen = 0;
  uint64_t visited_gen = 0;
  bool valid = false;
  uint64_t version = 0;
  // Per glyph of the layout: 1 = part of a visited link.
  std::vector<uint8_t> glyphs;
  // Answers per href as written, for this buffer and visited_gen (a new
  // layout band asks again for the same links).
  BufferId buf = kNoBuffer;
  std::unordered_map<std::string, bool> by_href;
};

struct AnnotationMark {
  std::string annotation_id;
  std::string layer_id;
  std::string kind;
  std::string body;
  xanatoo::Selection range;
};

struct AnnotationMarks {
  BufferId buf = kNoBuffer;
  uint64_t layers_gen = 0;
  uint64_t layout_gen = 0;
  Uint32 at = 0;
  std::vector<AnnotationMark> items;
  size_t unresolved = 0;
};
struct AnnotationIcon {
  size_t item = 0;  // into AnnotationMarks::items
  float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
};
// Margin arrow of a linked quote (quote_icons): click jumps to the source.
struct QuoteIcon {
  size_t link = 0;  // into DocBuffer::quotes.links
  float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
};
// Margin icon of a bookmark mark, in pane-local x / content-relative y
// (bookmark_icons).
struct BookmarkIcon {
  size_t item = 0;  // into BookmarkMarks::items
  float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
};

// Paged reading state of a view (app_pages.cpp, docs/PAGED.md), refreshed
// by every layout_panes. While `on`, Pane::layout is the paginated stack of
// spreads, scroll_y a multiple of `stride`, and the page bar sits below the
// view (Pane::h is the page area only).
struct PageView {
  bool on = false;
  int columns = 1;
  float pitch = 0.f;   // width of one text column
  float stride = 1.f;  // height of one spread = the scroll step
  xanatoo::PageMap map;
};

// The `//gallery` view of a pane (app_gallery.cpp, docs/PASSAGES.md
// "Gallery"): the images of the page, shown as a grid (Pane::source_doc, a
// Collection of cards) or, with `//item:N`, one of them filling the view.
// Valid while `buf` is the pane's buffer; use pane_gallery / pane_image_view.
struct GalleryView {
  BufferId buf = kNoBuffer;
  std::vector<xanatoo::GalleryItem> items;
  std::vector<xanatoo::NodeId> cards;  // grid Image node of each item
  int index = -1;                      // the single image; -1 = the grid
  // Zoom of the single image: scale = screen px per image px, 0 = fit the
  // view. pan = offset of the image centre from the view centre. View state,
  // reset when the image changes.
  float scale = 0.f;
  float pan_x = 0.f, pan_y = 0.f;
  bool drag = false;
  int drag_x = 0, drag_y = 0;  // last pointer position of a pan drag
};

struct Pane {
  BufferId buf = kNoBuffer;
  NavHistory nav;  // floating window trail (centre panes use the tab)
  xanatoo::Selection sel{};
  // Scrollbar strip sits left of the rect (left pane of a split: the middle
  // stays a bare divider). pane.x is then the content origin.
  bool sb_left = false;
  float scroll_y = 0.f;
  float preferred_x = 0.f;
  xanatoo::LayoutResult layout{};
  BufferId layout_buf = kNoBuffer;  // buffer `layout` was made from
  // layout_panes brings this back to its spot, then drops it.
  std::optional<ViewAnchor> view_anchor;
  // The passage this view shows (docs/PASSAGES.md): selectors of its address
  // (`base` unused, the buffer's path is the base), valid while `locator_buf`
  // is `buf`. Use pane_cut / pane_address.
  xanatoo::Locator locator;
  BufferId locator_buf = kNoBuffer;
  // Source pipes are pane-local projections; the shared buffer remains the
  // semantic document, so another pane can edit or render it independently.
  std::optional<xanatoo::Document> source_doc;
  BufferId source_buf = kNoBuffer;
  xanatoo::NodeId cut = xanatoo::INVALID_NODE;  // what the pipes selected
  std::vector<xanatoo::Selection> marks;        // the `#` passage
  GalleryView gallery;                          // `//gallery` (source_doc)
  BookmarkMarks bookmark_marks;
  AnnotationMarks annotation_marks;
  OpenTabLinks open_tab_links;
  PlayingLinks playing_links;
  VisitedLinks visited_links;
  bool section_focus = false;  // Section Focus: the outline picks the cut
  // Reader mode of this view: page chrome (Chrome nodes) hidden. Unset =
  // ViewSettings::reader_mode; kept across navigation. View state, never
  // part of the address (docs/PASSAGES.md "Reader mode"). Use pane_reader.
  std::optional<bool> reader;
  size_t chrome_parts = 0;  // top-level Chrome nodes at the last layout
  size_t nav_parts = 0;  // Page panel regions (navigation) at the last layout
  bool has_headings = false;  // headings for the Heading Outline, last layout
  // Paged reading of this view (ebook style pages instead of scrolling).
  // Unset = ViewSettings::paged; view state like `reader`. Use pane_paged.
  std::optional<bool> paged;
  PageView pages;
  int x = 0;
  int w = 0;
  int y = 0;
  int h = 0;
  // The strip above the text (pane_head_h; 0 = none): the view's tile
  // starts at y - head_h. An editable document's edit strip, else the
  // service strip of a native service's page (head_service).
  int head_h = 0;
  bool head_service = false;
  // pane_service_bar's last result, for the address service_addr.
  std::string service_addr;
  std::optional<xanatoo::ServiceBar> service;
  // Prior block heights for viewport virtualization (node → height).
  std::unordered_map<xanatoo::NodeId, float> height_cache;
  int last_layout_pane_w = 0;  // drop cache when this pane's width changes
  // Content-Y band last materialised (including overscan).
  float materialised_top = 0.f;
  float materialised_bottom = 0.f;
  PaneRaster raster;
  FootnoteOverlay footnotes;
  ScrollMarks scroll_marks;
  StatsCache stats;
  SearchTicks search_ticks;
};

// Placement of a floating window. Left / Right dock it into a side column
// that takes its width from the panes (windows docked on the same side
// stack top to bottom by `dock_seq`); Top / Bottom into a band between the
// columns that takes its height (windows side by side by `dock_seq`); Max
// covers the whole client area.
enum class Dock : uint8_t { None = 0, Left, Right, Top, Bottom, Max };

enum class WindowRole : uint8_t {
  Help, Outline, Backlinks, Sitemap, Page, History, VerticalTabs, Styles, StyleManager, Bookmarks, Downloads, Reply,
  Annotations, Context, Revisions, NostrExplorer, LemmyExplorer,
  Subscriptions, Certificates, Cookies, Identities, RelatedSites, MatrixExplorer, HoverHelp, Media,
  Document
};

inline UiPanelLayer embedded_panel_layer(WindowRole role) {
  switch (role) {
    case WindowRole::Styles: return UiPanelLayer::Styles;
    case WindowRole::StyleManager: return UiPanelLayer::StyleManager;
    case WindowRole::Reply: return UiPanelLayer::Reply;
    case WindowRole::Bookmarks: return UiPanelLayer::Bookmarks;
    case WindowRole::Subscriptions: return UiPanelLayer::Subscriptions;
    case WindowRole::Annotations: return UiPanelLayer::Annotations;
    case WindowRole::Context: return UiPanelLayer::Context;
    case WindowRole::NostrExplorer: return UiPanelLayer::NostrExplorer;
    case WindowRole::LemmyExplorer: return UiPanelLayer::LemmyExplorer;
    case WindowRole::Cookies: return UiPanelLayer::Cookies;
    case WindowRole::Certificates: return UiPanelLayer::Certificates;
    case WindowRole::Identities: return UiPanelLayer::Identities;
    case WindowRole::RelatedSites: return UiPanelLayer::RelatedSites;
    case WindowRole::MatrixExplorer: return UiPanelLayer::MatrixExplorer;
    default: return UiPanelLayer::Chrome;
  }
}

// One floating window: help / outline / backlinks panels and document
// windows share geometry, docking + interaction state; role picks the
// content.
struct FloatWindow {
  WindowRole role = WindowRole::Document;
  // Document role only: the view (App::panes slot) shown in the client area.
  int view = -1;
  std::string title;  // display override; empty → derive from path
  // Placement (all roles). x/y/w/h of a docked or maximized window are
  // derived each frame (`place_windows`); restore_* keep the free geometry.
  Dock dock = Dock::None;
  Dock restore_dock = Dock::None;  // placement to return to from Max
  int dock_seq = 0;                // order within a dock column
  float dock_share = 1.f;          // relative size within that column / band
  // Rolled up to the title strip; only takes effect while docked to a side or
  // band (`float_shaded`), cleared whenever the window leaves its dock.
  bool shaded = false;
  int restore_x = 120, restore_y = 70, restore_w = 540, restore_h = 440;
  Uint32 last_title_click = 0;
  // Stacking within its layer (free / docked / maximized): higher is in
  // front, ties keep the float_stack order. Set by raise_float / lower_float.
  int z = 0;
  // draw_floats pass that last showed it; a window that was not shown in the
  // pass before (just opened) comes to the front.
  uint64_t shown_pass = 0;
  // Geometry + interaction (all roles):
  int x = 120, y = 70, w = 540, h = 440;
  bool dragging = false;
  int drag_dx = 0, drag_dy = 0;
  int press_x = 0, press_y = 0;  // title press; a docked window tears off
                                 // once the pointer moved away from it
  int resize = 0;  // bit 1=right, 2=bottom, 4=left, 8=top, 8=top
  int resize_start_x = 0, resize_start_y = 0;
  int resize_start_w = 0, resize_start_h = 0;
  int resize_start_wx = 0, resize_start_wy = 0;  // window origin then
  float scroll = 0.f;  // panel roles; a document view scrolls its Pane
  bool scroll_dragging = false;
  bool span = false;  // touch: title finger + a second finger resize it
  float scroll_grab = 0.f;  // pointer y minus thumb top while dragging
};

// Docked to a side column or a band (not free, not maximized).
inline bool float_docked(const FloatWindow& w) {
  return w.dock != Dock::None && w.dock != Dock::Max;
}

// Shown as its title strip only: no client area, no resize edges.
inline bool float_shaded(const FloatWindow& w) {
  return w.shaded && float_docked(w);
}

// Title strip height of `w` (compact while docked or maximized).
inline int float_title_h(const FloatWindow& w) {
  return w.dock == Dock::None ? help_title_h : dock_title_h;
}

// Width of one title control hit zone of `w` (square + flush when docked).
inline int float_control_w(const FloatWindow& w) {
  return w.dock == Dock::None ? float_close_w : dock_title_h;
}

// Client rect of a floating window: the sunken well under the title minus
// its bevel. Left inset only; the right edge is flush so the scrollbar sits
// on the frame; the bottom strip is the grip row.
inline constexpr int client_scrollbar_width = 12;

inline std::tuple<int, int, int, int> window_client_rect(
    const FloatWindow& w, bool reserve_left_scrollbar = true) {
  const int lp = help_pad - 4;
  const int th = float_title_h(w);
  const int left_scroll = reserve_left_scrollbar && w.dock == Dock::Left &&
                                  w.role != WindowRole::Document &&
                                  w.role != WindowRole::Media
                              ? client_scrollbar_width : 0;
  return {w.x + 4 + lp + left_scroll, w.y + th + 5,
          w.w - 8 - lp - 1 - left_scroll,
          w.h - th - 4 - help_grip_h - 2};
}

inline FloatWindow make_float_panel(WindowRole role, int x, int y, int w,
                                    int h) {
  FloatWindow f;
  f.role = role;
  f.x = x;
  f.y = y;
  f.w = w;
  f.h = h;
  f.restore_x = x;
  f.restore_y = y;
  f.restore_w = w;
  f.restore_h = h;
  return f;
}

// Reply composition is local-only until service account/authentication support
// exists. BufferId + node/label keep the highlight tied to one comment, never
// to the current selection or the same numeric node in another document.
struct ReplyDraft {
  bool open = false;
  BufferId buffer = kNoBuffer;
  xanatoo::NodeId node = xanatoo::INVALID_NODE;
  std::string page, label, author, text;
  std::string excerpt;  // the start of the message's text, one line
  float anchor_y = -1.f;  // track position while the card is virtualised away
  uint64_t generation = 0;  // new target: reset ImGui's text/selection state
};

// What the pointer is currently over (rebuilt on every mouse move).
struct Hover {
  std::string href;
  std::string datetime;  // semantic instant under pointer, even without href
  std::string rel;
  bool is_cite = false;
  std::string preview;
  xanatoo::NodeId link_node = xanatoo::INVALID_NODE;
  size_t link_span = 0;
  uint32_t link_auto = 0;  // PositionedGlyph::auto_link of the hovered text
  int link_pane = -1;
  // link_node / link_span name a node of link_pane's footnote overlay
  // (FootnoteOverlay::doc), not of the pane's document.
  bool link_in_footnotes = false;
  xanatoo::NodeId stretch = xanatoo::INVALID_NODE;
  int stretch_pane = -1;
  xanatoo::NodeId list_fold = xanatoo::INVALID_NODE;  // fold marker's list item
  int list_fold_pane = -1;
  xanatoo::NodeId image = xanatoo::INVALID_NODE;
  int image_pane = -1;
  bool image_handle = false;
  bool image_card = false;  // collection card: click opens the viewer
  bool col_gap = false;
  int col_gap_pane = -1;
  xanatoo::NodeId task = xanatoo::INVALID_NODE;
  int task_pane = -1;
  xanatoo::NodeId grip = xanatoo::INVALID_NODE;
  int grip_pane = -1;
  xanatoo::NodeId transclude = xanatoo::INVALID_NODE;
  int transclude_pane = -1;
  std::string transclude_src;
  std::string bookmark;  // address of the bookmark whose margin icon is hovered
  int bookmark_pane = -1;
  long quote = -1;  // quote link whose margin arrow is hovered
  int quote_pane = -1;
  std::string annotation;  // note id whose passage tab is hovered
  int annotation_pane = -1;
  bool chrome_notice = false;  // reader mode notice ("Show all")
};

// Help panel (WindowRole::HoverHelp, app_hoverhelp.cpp): the explanation
// of the GUI element under the pointer, from the ui_help.hpp table. `key`
// names what is shown (an ImGui label or an "@" canvas key); the document
// is rebuilt only when it or the tooltip changes.
struct HoverHelp {
  bool open = false;
  std::string key;
  std::string tip;
  xanatoo::Document doc;
  xanatoo::LayoutResult layout;
  bool has_doc = false;
  bool needs_layout = true;
  int layout_w = 0;
};

// Touchscreen gestures (app_touch.cpp). Fingers on the document surface
// become taps (left click), pans with momentum, long-press selection and
// two-finger pinch zoom; touches on ImGui chrome are left to ImGui.
enum class TouchMode : uint8_t {
  Idle,     // no finger down
  Pending,  // one finger down: not yet a tap, pan or long press
  Pan,      // dragging scrolls the target
  Select,   // after a long press on text: the word is selected and the
            // context menu open; dragging closes it and extends the
            // selection, lifting then opens it again
  Hold,     // after a long press on no text: the context menu is open,
            // waiting for the lift
  UiHold,   // one finger on ImGui chrome, which gets the touch as a mouse:
            // held still, it becomes a right click (the chrome's menus)
  Direct,   // scrollbar / float frame / split gutter: plain pointer drag
  Pinch,    // two fingers: zoom
  Image,    // one finger on a single-image gallery view: pan / swipe / tap
  ImagePinch,  // two fingers there: zoom the image
  DockSwipe,  // a horizontal swipe across a side dock or window edge
  GutterDrag,  // two fingers either side of a gutter: they drag it along
  FloatSpan,   // a finger on a window title + a second one: each window
               // edge follows the finger on its side (resize / move)
  Ignored,  // ImGui's touch or a finished pinch; wait for all fingers up
};

struct TouchPoint {
  SDL_FingerID id = 0;
  float x = 0.f, y = 0.f;  // window pixels
};

struct Touch {
  TouchMode mode = TouchMode::Idle;
  TouchPoint fingers[2];  // tracked fingers; further ones are ignored
  int count = 0;
  // Gesture origin; its float window (if any) or pane is the scroll target.
  float start_x = 0.f, start_y = 0.f;
  Uint32 start_ticks = 0;
  bool on_float = false;
  bool on_image = false;  // `pane` shows a single gallery image
  bool menu_open = false;  // the first finger opened a menu
  Dock swipe_dock = Dock::None;
  bool ui_moved = false;   // motion on ImGui chrome cancels the long press
  int pane = 0;
  // Pan velocity (scroll px/s) and the fling that continues it.
  float velocity = 0.f;
  Uint32 last_move_ticks = 0;
  bool flinging = false;
  Uint32 fling_ticks = 0;
  // Pinch: finger distance, body size and the text between the fingers when
  // the second finger landed.
  float pinch_dist = 0.f;
  int pinch_body_px = 0;
  std::optional<ViewAnchor> pinch_anchor;
  // GutterDrag: grabbed gutter point minus the fingers' midpoint.
  float gutter_dx = 0.f, gutter_dy = 0.f;
  // FloatSpan: both fingers' positions when the second one landed.
  float span_ax = 0.f, span_ay = 0.f, span_bx = 0.f, span_by = 0.f;
};

// ---- background page fetches (app_fetch.cpp) ----
// Remote pages (Gemini / Nostr / Reddit / Lemmy) are fetched and parsed on a
// worker thread; the frame loop applies finished jobs (poll_fetches).
enum class FetchPurpose : uint8_t {
  Navigate,  // show in `pane` when done
  Preview,   // split-pane hover preview: goes to the page cache
  Tab,       // fill the stand-in buffer `buf` of a background tab
  Subscription, // check a watched source without opening/altering its view
  Download,  // "Download Link": every answer goes to Downloads, nothing shown
};

// Written by the worker, read by the main thread once `done` is set.
struct FetchShared {
  std::atomic<bool> done{false};
  // The job was cancelled (navigated elsewhere): long transfers that only
  // feed the view, such as a PDF shown as pages, stop early.
  std::atomic<bool> abandoned{false};
  bool force_download = false;  // set before the worker starts
  bool subscription_check = false;  // set before the worker starts
  std::string html_change_key;  // worker's XML fingerprint for an HTML watch
  // PDF / DjVu / comic answers may open as pages (set before the worker
  // starts): Navigate / Tab, and Context panel previews.
  bool allow_pages = false;
  // Page snapshots (snapshot.hpp; set before the worker starts):
  // `open_snapshot` shows that snapshot instead of loading the page (the
  // live page when it is gone); `keep_snapshot` keeps what was loaded.
  std::string open_snapshot;
  bool keep_snapshot = true;
  bool sent_http_cookies = false;
  // Set by the worker when `doc` is a snapshot (its time; 0: live).
  int64_t snapshot_time = 0;
  bool snapshot_missing = false;  // asked for one that was evicted: live
  std::optional<xanatoo::Document> doc;
  std::optional<std::string> raw_source;
  std::shared_ptr<xanatoo::DownloadProgress> download;
  // Bytes received so far, fed by the network readers on the worker thread.
  xanatoo::TransferMeter meter;
  std::string error;      // with `doc` set: the page is an HTTP error body
  int http_status = 0;    // non-200 answer whose body is in `doc`
  std::string final_url;  // page URL after redirects (buffer path)
  std::string content_path;  // local file of the document (DocBuffer::content_path)
  xanatoo::PageObservation observed;  // set with `doc` (finish_fetch)
  const char* service = "";
  // Gemini 10/11: the server asks for input (meta = prompt text).
  int input_status = 0;
  std::string input_meta;
  // Populated only for a structured Gemini TOFU pin mismatch; distinct
  // from generic TLS failures, which must never offer a trust bypass.
  std::optional<xanatoo::GeminiCertificateChange> gemini_change;
  // The certificate a Gemini server presented (pinned on first contact).
  std::optional<xanatoo::GeminiPeer> gemini_peer;
  // Gemini 60/61/62: the page needs a client certificate (identity), the
  // one sent is not authorised, or not valid; meta = the server's text.
  int identity_status = 0;
  std::string identity_meta;
  std::string identity_name;  // the identity the final request presented
};

// Remote image downloads (app_images.cpp): URLs ImageCache asked for,
// fetched by a few worker threads into the URL cache (cache_dir()/urls/).
struct PageTiles;  // app_pagetiles.cpp

// A finished remote image download; `error` says why it failed.
struct ImageFetchDone {
  std::string url;
  bool ok = false;
  std::string error;
};

struct ImageFetchShared {
  std::mutex mutex;
  std::deque<std::string> queue;
  std::vector<ImageFetchDone> done;
  // Of those, the ones that answered with something other than a picture
  // (a web page): views showing them switch to the page (poll_images).
  std::vector<std::string> pages;
  int workers = 0;
  // Downloads in progress (url → its worker's meter), for the placeholders'
  // progress bars; a worker adds its entry when it starts the URL.
  std::unordered_map<std::string, std::shared_ptr<xanatoo::TransferMeter>> active;
};

// Where remote image `url` stands: still queued, or downloading with the
// meter's figures. Neither: not fetching (done, failed, or a local image).
struct ImageLoadState {
  bool queued = false;
  bool active = false;
  uint64_t received = 0;
  int64_t expected = -1;  // -1: size unknown
};

struct FetchJob {
  uint64_t id = 0;
  FetchPurpose purpose = FetchPurpose::Navigate;
  int pane = 0;
  std::string url;
  xanatoo::Locator locator;  // Navigate: selectors to show once it landed
  bool record_history = true;
  // Tab: the "Loading …" buffer to fill. Navigate: the buffer `pane` showed
  // when the load began; a view that switched tabs since leaves the job
  // behind as a Tab job for that buffer (`left_view`, retarget_fetches).
  BufferId buf = kNoBuffer;
  bool left_view = false;  // Tab: replaces the page in `buf` (not a stand-in)
  Uint32 started = 0;
  std::shared_ptr<FetchShared> shared;
};

// Recently fetched pages, so a hover preview turns into a page instantly.
struct CachedPage {
  std::string url;
  std::string final_url;
  std::string content_path;
  xanatoo::Document doc;
  std::optional<std::string> raw_source;
  Uint32 ticks = 0;
  xanatoo::PageObservation observed;
};

// Sitemap panel (app_sitemap.cpp): the path tree of the focused page's
// site, read once per host on a worker thread.
struct SitemapLoad {
  std::atomic<bool> done{false};
  std::optional<xanatoo::SiteTree> tree;
  std::string error;
};

struct SiteCache {
  std::string host;
  std::shared_ptr<SitemapLoad> pending;  // set while the worker runs
  std::optional<xanatoo::SiteTree> tree;
  std::string error;
  std::vector<uint8_t> open;  // per tree node: expanded in the panel
  // Coverage, derived from SitemapPanel::seen when `seen_gen` is stale:
  // per node, pages of the subtree shown so far.
  std::vector<int> seen_count;
  uint64_t seen_gen = 0;
};

// Sound player (app_audio.cpp, docs/dev/CONTENT.md "Audio"): one file at a
// time, its controls in the Media panel (WindowRole::Media). Opening a link
// to an audio or video file starts it and shows the panel; the page stays.
// Video shows in the panel above the controls (docs/dev/CONTENT.md "Video").
struct AudioShared;  // decoder + sound device, shared with SDL's audio thread
struct AudioLoad;    // worker reading / downloading and opening the file
enum class AudioState : uint8_t { Idle, Loading, Playing, Paused, Ended, Failed };
struct AudioPlayer {
  AudioState state = AudioState::Idle;
  std::string url;    // address being played (base, as opened)
  std::string mime;   // response Content-Type, kept for retries
  std::string title;  // from the file's tags, else its name
  std::string error;  // Failed: why
  double duration = 0;  // seconds; 0 = unknown
  std::string format;   // "MP3 · 44100 Hz · stereo", once open
  float volume = 1.f;   // 0..1, applied in the callback
  // Dragging in the panel: the seek bar, the volume slider or an A / B
  // marker, while the left button that pressed it is down (update_audio
  // follows the pointer).
  enum class Drag : uint8_t { None, Seek, Volume, MarkA, MarkB } drag = Drag::None;
  int drag_x = -1;      // pointer x last applied
  std::shared_ptr<AudioShared> play;  // the sound device, while open
  std::shared_ptr<AudioLoad> load;    // built-in formats while loading (local)
  // A remote file of a built-in format: its download, which `media` plays
  // from while it arrives (open_audio_stream).
  std::shared_ptr<xanatoo::ByteFeed> feed;
  // Video and the formats only a backend decodes (media.hpp): the decoding
  // pipe, and with pictures their texture (I420, renderer-owned).
  std::shared_ptr<xanatoo::MediaPlayback> media;
  SDL_Texture* video_tex = nullptr;
  int tex_w = 0, tex_h = 0;      // size of video_tex
  int video_w = 0, video_h = 0;  // display size; 0 = sound only
  // Frame steps (audio_step_frame): the picture on screen, the time between
  // pictures (the file's rate, else as seen), and a step's picture not yet
  // shown (< 0: none), which the next step goes on from.
  double frame_pts = -1, frame_dt = 0, step_to = -1;
  int64_t visit = 0;  // its visit in the log (record_media_visit), 0 = none
  double start_at = -1;  // a `#t=` start, applied once it is open (< 0: none)
  // A / B markers (the panel's A–B button, a `#t=a,b` address): a section
  // of the file. Playing stops at B, or goes back to A with `loop`.
  bool ab = false;
  double ab_a = 0, ab_b = 0;  // seconds, ab_a < ab_b
  bool at_b = false;          // paused at B: Play starts at A again
  // Loop: the section while `ab`, else the whole file. Kept across files.
  bool loop = false;
  double last_pos = -1;  // position the last frame (B is crossed, not reached)
  // Bookmarks of this file (update_media_marks), keyed on bookmarks_gen and
  // url: the whole file, and positions (`#t=`) drawn on the seek bar.
  struct Mark {
    double t = 0;
    double end = -1;      // a section's end (`#t=a,b`), < 0: a position
    std::string address;  // as stored
  };
  std::vector<Mark> marks;
  std::string file_mark;  // the whole file's bookmark, as stored ("" = none)
  uint64_t marks_gen = 0;
  std::string marks_url;
  bool marks_valid = false;
};

// The Media panel's video filling the screen (toggle_media_fullscreen): the
// window goes fullscreen and the panel's controls lie over the bottom of the
// picture while the pointer moves, the video is paused or the pointer is on
// them. Kept apart from AudioPlayer, which audio_stop resets.
struct MediaFullscreen {
  bool on = false;
  bool window_was_fullscreen = false;  // left as it was on leaving
  double moved_at = 0;                 // last pointer motion (steady seconds)
  bool cursor_hidden = false;
  uint32_t last_video_click = 0;       // a press on the picture (double click)
};

// The Media panel's right-click menu (UiCommand::MediaAction).
struct MediaMenu {
  bool open = false;
  double t = 0;          // the position it is for (seconds)
  std::string mark;      // a position bookmark under the pointer, as stored
  // The section it is for: a bookmarked one under the pointer, else the
  // A–B markers (sec_b <= sec_a: none).
  double sec_a = 0, sec_b = 0;
  std::string sec_mark;  // that section's bookmark, as stored ("" = none)
};

// Search bar (app_search.cpp). The engine in use is
// ViewSettings::search_engine (saved in view.conf).
// A site's OpenSearch description being read (search menu "Add …").
struct OpenSearchLoad {
  std::atomic<bool> done{false};
  std::optional<xanatoo::SearchEngine> engine;
  std::string error;
};

struct SearchBar {
  bool open = true;  // shown by default: hidden it is hard to find
  std::vector<xanatoo::SearchEngine> engines;  // all, for search: addresses
  // Those switched on (engines_on): the menu, keywords, site search.
  std::vector<xanatoo::SearchEngine> active;
  std::shared_ptr<OpenSearchLoad> adding;  // one at a time
  std::string adding_url;
};

// Background "has a sitemap?" check of one host (URL-bar button).
struct SitemapProbeLoad {
  std::atomic<bool> done{false};
  xanatoo::SitemapProbe result = xanatoo::SitemapProbe::Unknown;
};

struct SitemapProbeState {
  xanatoo::SitemapProbeRecord rec;  // Unknown until checked (or net failure)
  std::shared_ptr<SitemapProbeLoad> pending;
  bool tried = false;  // asked this session (failures are not retried)
};

struct SitemapPanel {
  bool shown = false;
  // Per host, loaded from / saved to cache_dir()/sitemap-probes.
  std::unordered_map<std::string, SitemapProbeState> probes;
  bool probes_loaded = false;
  std::vector<SiteCache> sites;
  std::string host;       // site of the focused page; "" = not a web page
  std::string page;       // page URL the selection was last moved to
  std::vector<int> rows;  // visible tree nodes, top to bottom
  int sel = 0;            // selected row
  // Pages shown this session (site_url_key → title, may be ""), for every
  // host, whether or not the panel is open; `seen_gen` counts changes.
  std::unordered_map<std::string, std::string> seen;
  uint64_t seen_gen = 1;
};

// What the focused web page says about itself (<link rel=…>): feeds for the
// URL-bar feed button, icons, relations for the page panel, OpenSearch,
// canonical, language versions. Read from the buffer's raw source once per
// page (app_sitemap.cpp `focused_page_info`), alongside service navigation
// generated from its address. `gen` counts rebuilds.
struct PageInfoCache {
  BufferId buf = kNoBuffer;
  std::string path;
  size_t raw_size = 0;
  bool raw = false;
  uint64_t gen = 0;
  std::vector<xanatoo::FeedLink> feeds;
  xanatoo::PageLinks links;
  std::vector<xanatoo::ServiceRow> service_nav;
};

// Page panel (app_page.cpp): navigation / side bar chrome of the focused page.
struct PageRow {
  enum Kind : uint8_t { Label, Group, Link, Text };
  Kind kind = Text;
  int depth = 0;
  std::string text;
  std::string href;  // Link rows
};
struct PagePanel {
  bool shown = false;
  bool built = false;
  uint64_t key = 0;  // what `rows` were built from
  std::vector<PageRow> rows;
  int sel = 0;
};

// Split browsing (app_split.cpp): hovering a link in the left pane previews
// its target in the right pane; the right pane's own page is kept in
// `saved_*` and comes back when the pointer leaves the link.
struct SplitPreview {
  bool active = false;      // right pane currently shows a preview
  std::string shown;        // href previewed (or just committed)
  std::string hover;        // href under the pointer (left pane), "" = none
  Uint32 hover_since = 0;
  Uint32 off_since = 0;     // pointer left the link (grace period start)
  std::string pending;      // remote URL whose preview fetch is running
  BufferId scratch = kNoBuffer;  // preview doc not open elsewhere
  BufferId shown_buf = kNoBuffer;  // what the right pane shows for it
  BufferId saved_buf = kNoBuffer;
  xanatoo::Selection saved_sel{};
  float saved_scroll = 0.f;
};

// The link in the left pane whose target the right pane shows (marked in
// the left pane). Valid while both panes still show these pages; cleared by
// update_split_preview otherwise.
struct SplitTrail {
  std::string href;        // "" = no mark
  std::string left_page;   // path_of(left pane) the link lives in
  std::string right_page;  // what the right pane shows; "" = still loading
  xanatoo::NodeId target = xanatoo::INVALID_NODE;  // anchor in the right page
};

// Split browsing history (app_split.cpp): the two panes are a window on one
// trail. Each level is a page shown on the left with the pages opened from
// it on the right; a right-pane click starts the next level. Back steps
// through the top level's pages, then slides back a level (left page moves
// right); Forward retraces.
struct SplitHistoryEntry {
  std::string url;
  std::string href;  // left link that opened it ("" = none known)
  xanatoo::NodeId target = xanatoo::INVALID_NODE;  // its anchor, if any
  xanatoo::Selection sel{};  // last caret / scroll seen there
  float scroll = 0.f;
};
struct SplitHistoryLevel {
  SplitHistoryEntry page;                  // left pane
  std::vector<SplitHistoryEntry> opened;   // right pane, oldest first
  int cur = -1;                            // into `opened`, -1 = none
};
struct SplitHistory {
  std::vector<SplitHistoryLevel> levels;  // levels past `top` = Forward
  int top = -1;
  uint64_t gen = 0;          // bumped by every recorded change
  std::string pending_href;  // link being opened on the right
  bool restore[2] = {false, false};  // put caret / scroll back once loaded
};

// What the status bar says this frame (derived from world state each frame).
enum class StatusTone { Hint, Info, Browse, Warn, Prompt, Notice };
struct StatusLine {
  std::string text;   // left panel: hint, hover target, prompt, notice
  std::string stats;  // right panel: word / selection counts (may be empty)
  StatusTone tone = StatusTone::Hint;
};

// Who receives plain keys (arrows, Enter, digits). Panels only while open;
// see panel_has_keys(). Status prompts / minibuffer take precedence anyway.
// Panel: a panel without native keys of its own (embedded ImGui panels, Help,
// Downloads) is the active window; which one is App::panel_focus.
enum class KeyFocus : uint8_t {
  Document, Outline, Backlinks, Sitemap, Page, History, Revisions, Styles, Context, StyleManager, Panel
};

// The single active status-bar prompt (at most one at a time).
enum class PromptKind : uint8_t {
  None,
  Search,
  Replace,  // search + replacement fields
  SaveAs,
  SavePdf,
  OpenFile,
  OpenInWindow,
  UrlBar,
  InsertImage,
  InsertFigure,
  InsertInclude,
  EditLink,
  EditCite,
  EditSummary,
  HistoryFilter,  // History panel filter, applied while typing
};

struct Prompt {
  PromptKind kind = PromptKind::None;
  std::string text;           // find text / path / URL / href / summary …
  std::string text2;          // replacement text (Replace only)
  std::string source_path;    // cached original (SavePdf)
  // Search / Replace (app_find.cpp): the bar's field shall get the keys and
  // does not have them yet — typing goes to `text` meanwhile. `fresh`: the
  // next typing replaces the text (it is selected).
  bool pending = false;
  bool fresh = false;
  xanatoo::Caret origin;     // where typing searches from
  xanatoo::Selection found;  // the selection find made last, in `found_buf`
  BufferId found_buf = kNoBuffer;
  // Target of EditLink / EditCite / EditSummary.
  xanatoo::NodeId node = xanatoo::INVALID_NODE;
  size_t span = 0;
};

struct FileDialog;

// A Gemini server's certificate as last seen this session (app_certs.cpp),
// shown by the address bar's site icon and the Certificates panel.
struct GeminiSiteCert {
  xanatoo::GeminiPeer peer;  // host, port, presented SHA-256, details
  // The server presents another certificate than the pinned one: nothing
  // was sent; the user accepts or rejects it in the Certificates panel.
  std::optional<xanatoo::GeminiCertificateChange> change;
  std::string url;  // the page that met the change, reloaded on accept
  int pane = -1;
};

// A line of the History panel: a visit entry or a day header.
struct HistoryLine {
  int entry = -1;   // index into HistoryPanel::entries; -1: day header
  std::string day;  // header text
  int after = -1;   // day header: the entry above it (its lanes go on)
};

// History panel (WindowRole::History, app_history.cpp): the visit log,
// newest first, as a trail tree (history_trail.hpp, docs/HISTORY.md "C").
// Derived from the database; reloaded when `stale`.
struct HistoryPanel {
  std::vector<xanatoo::VisitRecord> visits;   // loaded, newest first
  std::vector<xanatoo::TrailEntry> entries;   // page runs of `visits`
  std::vector<xanatoo::LaneRow> lanes;        // one per entry
  std::vector<HistoryLine> lines;             // what the panel draws
  int graph_cols = 1;          // lane columns drawn (widest row, capped)
  std::string filter;          // kept filter (Enter in the filter prompt)
  std::string loaded_filter;   // the filter `visits` were loaded with
  bool stale = true;           // reload from the visit log
  bool more = false;           // older visits may exist (loaded on scroll)
  bool current_tab = false;
  std::string loaded_tab;
  bool from_db = false;        // false: this session's addresses only
  int sel = -1;                // keyboard selection (line index)
  int hover = -1;              // line under the pointer, -1 none
  // Row of the open right-click menu: its newest visit (0 without a
  // visit log) and address; empty address = no menu.
  int64_t menu_id = 0;
  std::string menu_address;
  // Observation of the hovered visit, for its tooltip.
  int64_t tip_observation = 0;
  std::optional<xanatoo::PageObservation> tip_observed;
};

// Certificates panel (WindowRole::Certificates).
struct CertPanel {
  bool open = false;
  std::string selected;  // gemini_cert_key of the row shown in detail
  std::vector<xanatoo::GeminiPin> pins;  // gemini_tofu_list, re-read on change
  std::string error;     // last store / action error
};

// Identity panel (WindowRole::Identities, app_identity.cpp), one tab per
// protocol: Gemini client certificates, which servers treat as accounts, and
// watch-only Nostr accounts (App::nostr_identities, nostr_store). Keys do not
// cross protocols; each tab keeps its own store. Server certificates stay in
// the Certificates panel.
struct IdentityPanel {
  bool open = false;
  xanatoo::IdentitySection section = xanatoo::IdentitySection::Gemini;
  // Protocol of the focused page last frame (0 none, 1 Gemini, 2 Nostr):
  // the tab follows the page when that changes.
  int page_kind = 0;
  std::string nostr_shown;  // hex pubkey shown in detail
  bool loaded = false;   // `ids` read from the store
  std::string selected;  // sha256 of the identity shown in detail
  std::vector<xanatoo::GeminiIdentity> ids;  // gemini_identities_list
  std::string error;     // last store / action error
  // The last page that asked for a certificate (Gemini 60/61/62).
  std::string request_url;
  int request_status = 0;
  std::string request_meta;
};

// Related Sites panel (app_related.cpp): similar websites of the
// focused page's site, asked once per domain on a worker thread.
// A note being sent to relays by a worker (app_identity.cpp).
struct NostrPostJob {
  std::atomic<bool> done{false};
  std::vector<xanatoo::NostrPublishResult> results;
  std::string event_id;  // set by a reply worker (it signs there)
  std::string error;     // why it never reached the relays
};

// The last note posted from the Nostr tab and what the relays said.
struct NostrPost {
  std::shared_ptr<NostrPostJob> pending;  // set while it is being sent
  std::string event_id;                   // hex id of the last note
  std::string status;                     // summary line
  std::vector<xanatoo::NostrPublishResult> results;
  uint64_t sent = 0;  // bumped when a note went out (the UI clears its draft)
};

struct RelatedLoad {
  std::atomic<bool> done{false};
  xanatoo::RelatedResult result;
};

struct RelatedSite {
  std::string host;  // as asked (the focused page's host)
  std::shared_ptr<RelatedLoad> pending;
  std::optional<xanatoo::RelatedResult> result;
};

struct RelatedPanel {
  bool open = false;
  // Follow the focused page; off holds the list on `host` while the user
  // explores its rows (otherwise every click reshuffles the list).
  bool follow = true;
  std::string host;  // site shown; "" = not a web page
  std::vector<RelatedSite> sites;  // this session's answers, newest last
};

// Scroll position from the last session, put back once the page is there
// (app_session.cpp).
struct PendingScroll {
  float y = -1.f;  // < 0: none
  std::string base;
};

struct RevisionPanelRow {
  int revision = -1; // Day dividers have no revision.
  std::string label;
};

struct App {
  App(SDL_Window* w, SDL_Renderer* r)
      : window(w),
        sdl(r),
        images(r),
        layout_engine(fonts, &images),
        renderer(r, fonts, &images) {}
  App(const App&) = delete;
  App& operator=(const App&) = delete;

  // --- platform / services ---
  CliOptions opt;
  SDL_Window* window = nullptr;
  SDL_Renderer* sdl = nullptr;
  int body_px = 17;
  int heading_px = 26;
  xanatoo::FontSet fonts;
  float ui_scale = 1.f;        // chrome / default zoom factor (set_chrome_scale)
  float ui_scale_base = 1.f;   // display density or --ui-scale, before preference
  bool ui_scale_pending = false;  // applied after the current ImGui frame renders
  xanatoo::Font ui_font;       // chrome text (see kUiFontPx)
  xanatoo::Font ui_font_bold;  // outline top level, emphasis in chrome
  xanatoo::Font ui_font_tiny;  // structure highlight tags (kStructTagPx)
  xanatoo::Font ui_font_small;       // docked window titles (kUiSmallFontPx)
  xanatoo::Font ui_font_small_bold;
  std::vector<DocBuffer> buffers;  // dense storage; address by BufferId
  // Documents whose strict XML import failed stay read-only until reloaded
  // successfully. An error preview must never overwrite the original file.
  std::unordered_set<std::string> invalid_xml_paths;
  // Files shown as a "binary data" notice instead of their bytes: read-only.
  std::unordered_set<std::string> binary_paths;
  BufferId next_buffer_id = 1;
  std::vector<BufferId> tab_order;
  std::vector<ClosedTab> closed_tabs;
  ImageCache images;
  LayoutEngine layout_engine;
  xanatoo::ViewSettings view_settings;
  // Active document style sheet (layout_engine / renderer point at it) and
  // the sheets found in style_dirs() for the toolbar / View menu.
  xanatoo::DocStyle style;
  std::optional<xanatoo::DocStyle> style_before_preview;
  std::optional<std::pair<xanatoo::UiCommand, std::string>> style_hover;
  std::vector<xanatoo::StyleEntry> styles;
  // White radial glows (alpha only) from the top / left / right edge,
  // stretched under gallery edge-zone icons.
  SDL_Texture* gallery_glow[3] = {};
  // Body font file: --font beats the sheet's body family, which beats
  // XANATOO_FONT (a packaging default, e.g. the Nix wrapper).
  std::string font_path;      // --font
  std::string env_font_path;  // XANATOO_FONT
  Renderer renderer;
  xanatoo::Ui ui;
  float ui_top = 0.f;
  float ui_bottom = 0.f;
  // Document views: [0] / [1] centre slots, then document windows.
  Pane panes[kMaxViews];
  int focus = 0;         // view with the caret (any slot)
  int centre_focus = 0;  // last focused centre slot (tabs, closing windows)
  KeyFocus key_focus = KeyFocus::Document;
  // The panel meant by KeyFocus::Panel (ignored for any other key_focus).
  WindowRole panel_focus = WindowRole::Document;
  // A panel consumed a printable key (e.g. history '/'): drop the TEXTINPUT
  // SDL sends right after it so the digit is not typed into the document.
  bool eat_text_input = false;
  // The window got shorter (on-screen keyboard opened): bring the caret back
  // into view once the panes are laid out for the new size.
  bool reveal_caret_after_layout = false;
  bool split = false;  // auto-enabled when two files are given
  float split_ratio = 0.5f;
  TileAxis tile_axis = TileAxis::Columns;
  TileMode tile_mode = TileMode::Independent;
  bool dragging_sel = false;
  // The selection last handed to the X11 / Wayland primary selection
  // (publish_primary_selection), or the last one seen when it was not sent.
  struct PrimarySel {
    int pane = -1;
    BufferId buf = kNoBuffer;
    xanatoo::Selection sel{};
  } primary_sel;
  // Keys, clicks or touches arrived this frame: a changed selection is the
  // user's own (publish_primary_selection).
  bool primary_sel_input = false;
  bool click_new_tab = false;  // middle-click in progress: panels open in a new tab
  bool dragging_split = false;
  int split_drag_grab = 0;
  bool dragging_diff_overview = false;
  bool dragging_scroll = false;
  int page_slider_pane = -1;  // paged view whose bar slider is being dragged
  Uint32 page_turn_at = 0;    // SDL ticks of the last wheel page turn
  int scroll_drag_pane = 0;
  float scroll_drag_grab = 0.f;
  bool dragging_image = false;
  xanatoo::NodeId resize_image = xanatoo::INVALID_NODE;
  float resize_start_x = 0.f;
  float resize_start_scale = 1.f;
  bool dragging_block = false;
  xanatoo::NodeId drag_block_id = xanatoo::INVALID_NODE;
  xanatoo::NodeId drop_target_id = xanatoo::INVALID_NODE;
  bool drop_before = true;
  float drop_line_y = -1.f;
  bool dragging_col_gap = false;
  xanatoo::NodeId col_gap_left = xanatoo::INVALID_NODE;
  xanatoo::NodeId col_gap_right = xanatoo::INVALID_NODE;
  float col_gap_start_x = 0.f;
  float col_gap_px = 1.f;      // pixel width of the two columns together
  float resize_start_w = 1.f;  // image width in px when the drag began
  float col_gap_left_w = 1.f;
  float col_gap_right_w = 1.f;
  Uint32 last_click_ticks = 0;
  float last_click_x = -999.f;
  float last_click_y = -999.f;
  int click_count = 0;
  int win_w = 1024;
  int win_h = 768;
  bool running = true;
  bool needs_layout = true;
  int last_layout_w = 0;
  int last_layout_h = 0;
  std::vector<FloatWindow> soft_wins;  // Document-role floats, back→front
  int float_z_top = 0, float_z_bottom = 0;  // FloatWindow::z handed out so far
  uint64_t float_pass = 0;                  // draw_floats calls
  Dock dock_preview = Dock::None;  // drop target while dragging a title
  struct DockSplitDrag {
    Dock side = Dock::None;
    size_t first = 0;    // ordered neighbour index in this dock
    int start = 0;       // pointer coordinate at press
    int first_px = 0, second_px = 0;
    float first_share = 1.f, second_share = 1.f;
  } dock_split_drag;
  bool tab_drag_preview = false;   // dock_preview comes from a dragged tab
  bool tab_drop_preview = false;   // a dragged window would become a tab
  bool left_dock_hidden = false;
  bool right_dock_hidden = false;
  int dock_w[4] = {280, 360, 200, 200};  // Left / Right width, Top / Bottom height
  Prompt prompt;
  std::shared_ptr<FileDialog> file_dialog;
  // Last find / replace text, kept when another prompt displaces a search
  // so F3 / Replace All keep working.
  std::string last_search, last_replace;
  uint64_t find_focus_seq = 0;  // requests to focus the find bar's field
  xanatoo::Minibuffer minibuf;
  std::vector<std::string> minibuf_hist_path;
  std::vector<std::string> minibuf_hist_url;
  std::string gemini_current_url;
  // Gemini certificates met this session, by gemini_cert_key (host:port).
  std::unordered_map<std::string, GeminiSiteCert> gemini_certs;
  CertPanel certs;
  IdentityPanel identities;
  // Pages opened this session in any view, oldest first (History panel);
  // Back / Forward use each view's own `Pane::nav`.
  std::vector<std::string> visited;
  // Bumped whenever a page is visited or visits are removed (visited links).
  uint64_t visited_gen = 1;
  HistoryPanel history;
  std::vector<xanatoo::Bookmark> bookmarks;
  // Every bookmark folder, empty ones too (xanatoo::bookmark_folder_list).
  std::vector<std::string> bookmark_folders;
  std::vector<xanatoo::Subscription> subscriptions;
  bool subscriptions_panel_open = false;
  int64_t next_subscription_auto_at = 0;
  std::vector<xanatoo::AnnotationLayer> annotation_layers;
  std::unordered_set<std::string> hidden_annotation_layers;
  std::string active_annotation_layer;
  std::string selected_annotation_id;
  uint64_t annotation_note_created_gen = 0;
  uint64_t annotation_layers_gen = 1;
  bool annotation_panel_open = false;
  bool revision_panel_open = false;
  bool nostr_explorer_open = false;
  bool lemmy_explorer_open = false;
  int lemmy_tab = 0;  // 0: instances, 1: communities, 2: people, 3: posts
  std::string lemmy_search;
  std::string lemmy_instance = "lemmy.world";
  std::string lemmy_sort = "Hot";
  bool lemmy_instance_only = false;
  bool matrix_explorer_open = false;
  int matrix_tab = 1;  // 0: servers, 1: rooms, 2: spaces, 3: people
  std::string matrix_search;
  std::string matrix_server = "matrix.org";
  bool matrix_has_token = false;      // cached; credentials are read on demand
  uint64_t matrix_token_checked = 0;  // ticks of the last check (0 = never)
  int nostr_tab = 0;  // 0: people, 1: events, 2: NIP-29 groups
  std::string nostr_search;
  int nostr_page = 0;
  bool nostr_people_more = false;
  bool nostr_events_more = false;
  bool nostr_groups_more = false;
  NostrCacheStats nostr_stats;
  std::vector<NostrCachedPerson> nostr_people;
  std::vector<NostrCachedEvent> nostr_events;
  std::vector<NostrCachedGroup> nostr_groups;
  std::vector<NostrLocalIdentity> nostr_identities;
  std::string nostr_active_identity;
  // Signing keys (nostr_keys.hpp; the nsec of a password-less key is wiped
  // once loaded) and, per public key, the secret unlocked with its password
  // for this session only (wiped on lock), or open from the start for a
  // password-less key.
  std::vector<xanatoo::NostrKey> nostr_keys;
  std::unordered_map<std::string, std::string> nostr_unlocked;
  NostrPost nostr_post;
  NostrPost nostr_reply;  // a Reply panel answer to a Nostr note
  std::string revision_document_key, revision_working;
  std::string revision_compare_base, revision_menu_target;
  std::vector<xanatoo::DocumentRevision> revision_history;
  // Visual rows include day dividers. Revision indices still drive edges
  // and keyboard navigation, and map back to visual rows through slots.
  std::vector<RevisionPanelRow> revision_rows;
  std::vector<int> revision_slots, revision_lanes, revision_parents;
  std::unordered_map<std::string, std::string_view> revision_labels;
  int revision_lane_count = 0;
  std::string annotation_history_cached_layer;
  std::string annotation_history_cached_head;
  std::vector<xanatoo::AnnotationLayerRevision> annotation_history_cache;
  uint64_t bookmarks_gen = 1;  // bumped by every change of `bookmarks`
  uint64_t shared_state_poll_ms = 0;  // last poll_shared_state check
  // How the next navigation was asked for (visits.hpp): set right before
  // navigate_to_url / open_address by the chrome; navigate_pane moves it
  // into the view's NavHistory::pending_kind.
  std::optional<xanatoo::VisitKind> visit_hint;
  // With a `history` hint: the visit picked in the History panel, which
  // the new visit continues (navigate_pane → NavHistory::pending_from).
  int64_t visit_from = 0;
  // With `visit_from`: the view returns to that visit (it becomes the
  // current entry's visit) instead of recording a new one.
  bool visit_resume = false;
  // Page snapshot (snapshot.hpp id) the next remote page load shows instead
  // of the live page: set right before navigate_to_url by Back / Forward
  // and the History panel, taken by start_fetch (find_cached_page only
  // answers with that snapshot meanwhile). '' = the live page.
  std::string open_snapshot;
  // False while start-up shows stand-in pages (the default document before
  // a session restore replaces it): no visits are recorded then.
  bool record_visits = true;
  std::string bookmark_menu;   // bookmark of the open margin-icon menu
  bool history_panel_open = false;
  bool vertical_tabs_open = false;
  bool tab_bar_visible = true;
  bool styles_panel_open = false;
  bool styles_manager_panel_open = false;
  xanatoo::StylePins style_pins;
  uint64_t style_save_generation = 0;
  bool downloads_panel_open = false;
  bool preferences_open = false;  // Edit → Preferences (ImGui window)
  bool credentials_open = false;  // Edit → Credentials (ImGui window)
  bool bookmarks_panel_open = false;
  bool context_panel_open = false;
  // Live body of the Context panel (refresh_context_panel).
  // When context_has_doc is set, the panel draws context_doc with the real
  // layout engine (same path as Help); otherwise it falls back to the
  // ImGui text body below.
  std::string context_kind;
  std::string context_title;
  std::string context_body;
  std::string context_meta;
  bool context_can_jump = false;
  int context_jump_pane = -1;
  long context_jump_link = -1;
  bool context_jump_to_source = true;
  bool context_has_doc = false;
  std::string context_source_key;  // identity of what is loaded; skip reload if same
  // A picture preview (context_has_doc with an "image:" key): the panel
  // draws this original fitted, like the gallery's single image, instead
  // of laying out context_doc. Relative `src` resolves against `base`.
  std::string context_image_src;
  std::string context_image_base;
  std::string context_pending_url;  // remote/local page fetch in flight for Context
  xanatoo::Document context_doc;
  xanatoo::LayoutResult context_layout;
  bool context_needs_layout = true;
  xanatoo::NodeId context_scroll_to = xanatoo::INVALID_NODE;
  // Footnote whose mark is hovered: washed in the Context panel.
  xanatoo::NodeId context_footnote = xanatoo::INVALID_NODE;
  // Thread message an in-page link under the pointer points at (HN's
  // "reply to …"): its card is washed in the Context panel and tied to the
  // link by a connector curve. Set by refresh_context_panel while hovered.
  xanatoo::NodeId context_message = xanatoo::INVALID_NODE;
  // Address context_doc was read from (its page, without fragment); the
  // panel's links resolve against it. Empty when unknown.
  std::string context_address;
  // Text selection in the Context panel (carets into context_doc), started
  // by a press in its client area and extended while context_selecting.
  xanatoo::Selection context_sel;
  bool context_selecting = false;
  std::string status_notice;
  Uint32 status_notice_until = 0;
  bool discard_armed = false;  // only without a revision store (no SQLite)
  bool help_mode = false;
  xanatoo::Document help_doc;
  xanatoo::LayoutResult help_layout;
  bool help_needs_layout = true;
  bool help_loaded = false;
  bool outline_mode = false;
  std::vector<xanatoo::OutlineEntry> outline;
  int outline_sel = 0;
  bool backlinks_mode = false;
  std::vector<xanatoo::BacklinkEntry> backlinks;
  int backlinks_sel = 0;
  std::string backlinks_label;
  std::unordered_set<xanatoo::NodeId> folded_sections;
  // View-local Collection layouts (grid / list / full, sort, card size);
  // missing entry = the node's default. Like folded_sections, keyed by node.
  std::unordered_map<xanatoo::NodeId, xanatoo::CollectionView> collection_views;
  // Collection the last right-click hit (context menu target).
  xanatoo::NodeId collection_target = xanatoo::INVALID_NODE;
  // Block the last right-click hit (context menu's Fold / Expand).
  xanatoo::NodeId fold_target = xanatoo::INVALID_NODE;
  // Document state `has_summaries` was found for.
  SummaryScan summary_scan;
  bool has_summaries = false;  // the focused page has two-body stretchtext
  std::string last_fold_doc_key;
  bool diff_mode = false;
  std::vector<xanatoo::NodeId> diff_marks[2];
  std::vector<xanatoo::DiffWordMark> diff_words[2];
  std::vector<float> diff_overview[2];
  bool structure_mode = false;
  // Content only (F11, gamepad X): no menu bar, toolbars, tab strip; the
  // status line only while a prompt or notice needs it.
  bool chrome_hidden = false;
  bool virtual_keyboard = false;
  xanatoo::sdl::Gamepad* gamepad = nullptr;
  bool stick_cursor_on = false;
  float stick_cx = 0.f;
  float stick_cy = 0.f;
  bool stick_cursor_init = false;
  // Last gamepad input (sdl::ticks); the stick cursor hides after
  // kStickCursorIdleMs without any (stick_cursor_visible).
  Uint32 stick_last_use = 0;
  bool stick_lmb = false;
  bool stick_rmb = false;
  float mouse_x = 0.f;
  float mouse_y = 0.f;
  // No pointer over the window: a touch ended (a lifted finger sends no
  // "moved away"). mouse_x/y sit at kPointerAway and nothing is hovered
  // until the next mouse motion or touch (pointer_away, app_events.cpp).
  bool pointer_away = false;
  SDL_Cursor* cursor_arrow = nullptr;
  SDL_Cursor* cursor_hand = nullptr;
  SDL_Cursor* cursor_sizewe = nullptr;
  SDL_Cursor* cursor_sizens = nullptr;
  // Corner resize cursors, by the corner being dragged.
  SDL_Cursor* cursor_size_nw = nullptr;
  SDL_Cursor* cursor_size_ne = nullptr;
  SDL_Cursor* cursor_size_sw = nullptr;
  SDL_Cursor* cursor_size_se = nullptr;
  SDL_Cursor* cursor_sizeall = nullptr;
  SDL_Cursor* cursor_current = nullptr;
  Uint32 caret_blink_start = 0;
  FloatWindow help = make_float_panel(WindowRole::Help, 80, 100, 560, 480);
  FloatWindow bl = make_float_panel(WindowRole::Backlinks, 80, 120, 360, 320);
  FloatWindow ol = make_float_panel(WindowRole::Outline, 40, 100, 320, 420);
  FloatWindow sm = make_float_panel(WindowRole::Sitemap, 140, 110, 340, 460);
  FloatWindow vertical_tabs_win = make_float_panel(WindowRole::VerticalTabs, 50, 130, 260, 440);
  FloatWindow history_win = make_float_panel(WindowRole::History, 100, 130, 440, 360);
  FloatWindow styles_win = make_float_panel(WindowRole::Styles, 140, 100, 320, 420);
  FloatWindow styles_manager_win =
      make_float_panel(WindowRole::StyleManager, 200, 120, 560, 560);
  FloatWindow bookmarks_win = make_float_panel(WindowRole::Bookmarks, 130, 145, 460, 400);
  FloatWindow context_win = make_float_panel(WindowRole::Context, 160, 180, 420, 360);
  FloatWindow subscriptions_win = make_float_panel(WindowRole::Subscriptions, 170, 155, 590, 480);
  FloatWindow downloads_win = make_float_panel(WindowRole::Downloads, 120, 160, 520, 380);
  FloatWindow reply_win = make_float_panel(WindowRole::Reply, 170, 170, 490, 350);
  FloatWindow annotation_win = make_float_panel(WindowRole::Annotations, 150, 110, 430, 540);
  FloatWindow revision_win = make_float_panel(WindowRole::Revisions, 120, 140, 550, 560);
  FloatWindow nostr_explorer_win = make_float_panel(WindowRole::NostrExplorer, 180, 120, 560, 520);
  FloatWindow lemmy_explorer_win = make_float_panel(WindowRole::LemmyExplorer, 190, 130, 560, 520);
  FloatWindow matrix_explorer_win = make_float_panel(WindowRole::MatrixExplorer, 200, 140, 560, 520);
  bool cookies_open = false, cookie_reveal = false;
  uint64_t cookie_refresh_at = 0;
  std::string cookie_site, cookie_page_url, cookie_preview, cookie_error;
  CookieSnapshot cookie_snapshot;
  FloatWindow cookies_win = make_float_panel(WindowRole::Cookies, 160, 120, 540, 460);
  FloatWindow certs_win = make_float_panel(WindowRole::Certificates, 160, 120, 520, 460);
  FloatWindow identities_win = make_float_panel(WindowRole::Identities, 180, 140, 520, 480);
  FloatWindow related_win = make_float_panel(WindowRole::RelatedSites, 150, 120, 380, 480);
  // Help panel (Shift+F1): explains the GUI element under the pointer.
  HoverHelp hover_help;
  FloatWindow hover_help_win = make_float_panel(WindowRole::HoverHelp, 200, 160, 360, 300);
  RelatedPanel related;
  ReplyDraft reply;
  SitemapPanel sitemap;
  PageInfoCache page_info;
  // Declared icon (<link rel=icon>) per web host, learnt from pages shown
  // this session: tabs and bookmarks of that site use it too.
  std::unordered_map<std::string, std::vector<std::string>> site_icons;
  SearchBar search;
  AudioPlayer audio;
  bool media_panel_open = false;
  FloatWindow media_win = make_float_panel(WindowRole::Media, 140, 140, 440, 200);
  FloatWindow pg = make_float_panel(WindowRole::Page, 60, 110, 260, 420);
  PagePanel page;
  Hover hover;
  // Right-clicked HN Message, not necessarily the current text selection.
  std::string context_comment_native;
  std::string context_comment_web;
  // Right-clicked link: its target as the address bar would show it, and
  // whether it is a web URL ("Download Link").
  std::string context_image_address;  // source of the right-clicked image
  std::string context_pdf_path;
  std::string context_pdf_name;
  std::string context_link;
  std::string context_link_href;  // as written in the page (new tab)
  bool context_link_web = false;
  // A mention / hashtag link (rel, types.hpp kRel*): its text as shown,
  // "@alice.example" / "#tag", for Copy Handle / Copy Hashtag.
  std::string context_link_rel;
  std::string context_link_text;
  // Blocks around the right-click (xanatoo::cut_blocks), innermost first,
  // in view `context_blocks_pane` showing `context_blocks_buf`; the one
  // whose menu item is hovered is washed in the page.
  std::vector<xanatoo::NodeId> context_blocks;
  BufferId context_blocks_buf = kNoBuffer;
  int context_blocks_pane = -1;
  xanatoo::NodeId context_block_hot = xanatoo::INVALID_NODE;
  BufferId context_reply_buffer = kNoBuffer;
  xanatoo::NodeId context_reply_node = xanatoo::INVALID_NODE;
  // Quote links under the right-click (context menu "Go to …" items).
  struct QuoteMenuItem {
    std::string label;
    size_t link = 0;
    bool to_source = true;  // quote → original, else original → reply
  };
  std::vector<QuoteMenuItem> context_quotes;
  BufferId context_quote_buf = kNoBuffer;
  // The quote link last jumped along: both ends washed while the view
  // that jumped keeps its `#` mark.
  BufferId quote_focus_buf = kNoBuffer;
  long quote_focus = -1;
  float context_reply_y = -1.f;
  Touch touch;
  // A press on linked quoted text: on release without a drag or selection
  // it opens the Context panel; otherwise the press placed the caret or
  // started a selection as usual (pointer_left_down / pointer_left_up).
  struct QuoteClick {
    long link = -1;
    int pane = -1;
    BufferId buf = kNoBuffer;
    int x = 0, y = 0;
  } quote_click;
  std::vector<FetchJob> fetches;
  std::vector<BufferId> deferred_tab_loads;  // background session restores
  std::vector<std::shared_ptr<xanatoo::DownloadProgress>> downloads;
  // Confirmation before an explicit download starts (Cancel | Download [to…]).
  struct DownloadConfirm {
    bool open = false;
    std::string url;
  } download_confirm;
  // Consumed by the next Download-purpose fetch (folder from "Download to…").
  std::string pending_download_dir;

  // The row of the open Downloads right-click menu.
  std::shared_ptr<xanatoo::DownloadProgress> download_menu;
  MediaMenu media_menu;
  MediaFullscreen media_full;
  // The Media panel shows only the picture, over its whole client area
  // (toggle_media_controls); the controls are back with a file without one.
  bool media_controls_hidden = false;
  uint64_t next_fetch_id = 1;
  std::shared_ptr<ImageFetchShared> image_fetch =
      std::make_shared<ImageFetchShared>();
  // Waiting remote images drawn this frame (want_image_now), in draw order.
  std::vector<std::string> images_on_screen;
  // Arrived images not yet relaid out, and when the last such relayout ran.
  bool image_relayout_due = false;
  Uint32 image_relayout_at = 0;
  // Fixed-layout page tiles (app_pagetiles.cpp): loaders, views, textures.
  std::shared_ptr<PageTiles> page_tiles;
  std::vector<CachedPage> page_cache;
  SplitPreview split_preview;
  SplitTrail split_trail;
  SplitHistory split_history;
  PendingScroll pending_scroll[2];
  // Session file as last written, when it was last compared, and whether
  // the start-up restore is over (only then does autosave_session write).
  std::string session_text;
  Uint32 session_checked = 0;
  bool session_live = false;
};

// ---- session (app_session.cpp) ----
// Nostr's local SQLite observation catalogue, refreshed only on open/search
// or the explicit Refresh action (not with a database query per frame).
void refresh_nostr_catalogue(App& app);
// Just the saved accounts and the active one (Identities panel).
void refresh_nostr_identities(App& app);
// Collect relay answers to a note or reply being posted (each frame).
void update_nostr_post(App& app);
// The Reply panel's target is a Nostr note (an event id on a nostr: page).
bool reply_is_nostr(const App& app);
// Reply panel Send: posts to Nostr as the unlocked active account, else
// says why it cannot (other services: the panel is still a mockup).
void send_reply(App& app);
void reply_ui_state(App& app, xanatoo::UiState& st);

// Write state_dir()/session.conf (at shutdown); restore it at startup when
// no document was named on the command line; put the saved scroll back once
// its page has loaded (per frame).
void save_session(App& app);
void restore_session(App& app);
void autosave_session(App& app);  // per frame: write it when it changed
void apply_pending_scroll(App& app);

// ---- drafts (app_drafts.cpp) ----
// Unsaved edits are written to the local revision store as drafts, so
// buffers can be closed or replaced without asking; reopening the file
// restores them. Only when the store is unavailable is a discard confirmed.
inline constexpr Uint32 kDraftDelayMs = 2000;  // max. edit age before a write
// After loading `b.path` into the buffer: set its doc key / base and put a
// stored draft in place of the file text. True when a draft was restored.
bool attach_draft(App& app, BufferId id);
// Write the buffer's unsaved edits now. True when nothing would be lost by
// dropping the buffer (clean, read-only, or draft written).
bool stash_draft(App& app, BufferId id);
// stash_draft, else the old "repeat to discard" confirmation.
bool stash_or_confirm(App& app, BufferId id);
bool stash_all_or_confirm(App& app);  // every buffer (quit, demos)
void autosave_drafts(App& app);       // per frame: write drafts that are due
// `text` was written to `b.path`: record the save, clear the draft (also
// the one under a previous key after Save As).
void note_saved(App& app, BufferId id, const std::string& text);
bool save_stored_buffer(App& app, BufferId id);
bool make_stored_copy(App& app, BufferId id);
// Drop the draft and reload the file (File → Revert to Saved).
void revert_buffer(App& app, BufferId id);
// "drafts:" page listing every stored draft; "draft:<key>" opens one.
inline constexpr const char* kDraftsPage = "drafts:";
inline constexpr const char* kDocumentsPage = "documents:";
inline bool is_stored_document_ref(std::string_view path) {
  return path.size() == 71 && path.starts_with("db:doc:");
}
inline bool is_stored_original_ref(std::string_view path) {
  return path.size() == 80 && path.starts_with("db-original:doc:");
}
inline bool is_stored_revision_ref(std::string_view path) {
  return path.size() == 152 && path.starts_with("db-revision:doc:") &&
         path.substr(80).starts_with("@sha256:");
}
std::optional<xanatoo::Document> load_stored_revision_ref(std::string_view path);
void open_drafts_page(App& app);
// "about:" page: services, formats and libraries of this build (app_about.cpp).
inline constexpr const char* kAboutPage = "about:";
void open_about_page(App& app);
// Startup report on stderr (app_report.cpp, gamepkgstd LOGGING.md): versions
// at runtime, video / renderer / OpenGL, controllers, directories.
void print_startup_report(App& app);
bool open_draft(App& app, int pi, const std::string& ref);
bool open_stored_document(App& app, int pi, const std::string& ref);
void toggle_revision_snapshot(App& app);
bool open_stored_original(App& app, int pi, const std::string& ref);
void open_stored_documents_page(App& app);

// ---- buffers ----
// The local file to read for `path`: the import's derived Markdown for
// pandoc formats (converted on demand), else `path`; empty = import failed.
// The buffer keeps `path` as its address and this as `content_path`.
std::string resolve_open_path(App& app, std::string path);
xanatoo::Document load_doc(App& app, std::string path);
std::string parent_dir(const std::string& path);
bool is_in_app_remote_url(std::string_view s);
// An image address (`is_image_address`, by its name) to open straight in
// the image viewer, without fetching it as a page first. For http(s) the
// name is only a guess: true only when the URL cache saw an image/* answer
// there (a web build fetches every other one and lets the Content-Type
// decide, so "/wiki/File:X.jpg" becomes the page it is).
bool opens_as_image(std::string_view url);
// Not known to serve something other than a picture (url_cache_kind): a
// link named like an image may still go to a page's gallery, which
// switches to the page should the download turn out to be one.
bool may_be_image(std::string_view url);
// Foreign content — fetched pages, imported HTML / EPUB, anything under
// cache_dir() — is read-only: browsed, never edited or saved in place.
bool is_foreign_path(std::string_view path);
// Editable documents are edited in place (no edit mode); foreign content
// stays read-only, and an editable document can be locked (docs/UI.md
// "Editable documents"). browsing = the focused view takes no edits now.
bool browsing(App& app);
bool focused_read_only(App& app);
bool pane_read_only(App& app, const Pane& p);
// Editable and locked: installed data (data_dir(): examples, help) starts
// locked, the user's own documents unlocked.
bool pane_locked(App& app, const Pane& p);
bool default_locked(std::string_view path);
void set_pane_locked(App& app, const Pane& p, bool locked);
// The buffer's document is editable and unlocked (its tab is tinted).
bool buffer_takes_edits(App& app, BufferId id);
// Height of the edit strip above an editable view (0 when it has none).
int edit_bar_h(App& app, const Pane& p);
// The address whose service strip a page shows: the page, or the target of
// a search: address (a Digg search is Digg's).
std::string service_address(App& app, const std::string& address);
// The service strip of the view's page (docs/UI.md "Service strips"), also
// while it loads; nullptr for pages of no native service.
const xanatoo::ServiceBar* pane_service_bar(App& app, Pane& p);
// Height of the strip on top of a view: its edit strip, else its service
// strip (sets Pane::head_service); 0 when chrome is hidden.
int pane_head_h(App& app, Pane& p);
inline constexpr const char* kReadOnlyNotice =
    "Read-only page (foreign content) — Ctrl+S saves an editable Markdown copy";
inline constexpr const char* kLockedNotice =
    "Locked — F2 or the lock on the edit strip allows editing";
// Why the focused view takes no edits: kReadOnlyNotice or kLockedNotice.
const char* no_edit_notice(App& app);
xanatoo::Document load_doc_or_placeholder(App& app, const std::string& path);
std::string buffer_tab_label(App& app, BufferId bi);
// Last component of a local path for tabs / titles; "dir/" for a directory
// listing (trailing '/').
std::string path_title(const std::string& path);
void update_title(App& app);
xanatoo::Document& doc_of(App& app, Pane& p);
std::string& path_of(App& app, Pane& p);
// The local file behind a buffer: `content_path` when set, else `path`.
// For resolving relative images / links, never for showing or storing.
const std::string& content_file(const DocBuffer& b);
// The derived file a local address is read from (pandoc import → cached
// content.md), empty when it is read itself. add_buffer / show_doc_in_pane
// fill DocBuffer::content_path from it unless told otherwise.
std::string derived_content_path(const std::string& path);
std::string base_of(App& app, Pane& p);  // directory of content_file
bool shared_doc(App& app);
// Some live view other than `except_pane` shows `b`.
bool shown_anywhere_else(App& app, BufferId b, int except_pane);
Pane& focused_pane(App& app);
// Views (App::panes slots): live = shown (centre 0, 1 while split, and every
// document window's view). The other centre slot of a split for links that
// open "in the other pane"; a window view's partner is the centre focus.
bool view_live(const App& app, int pi);
std::vector<int> live_views(const App& app);
int other_pane(const App& app);
void focus_view(App& app, int pi);
// Free slot for a new document window, -1 when all are taken; release_view
// clears a slot (layout, raster) and moves the focus off it.
int alloc_view(App& app);
void release_view(App& app, int pi);
// Views of `bi` other than `except` after its document was replaced or
// rewound: stale layout dropped, caret clamped to the tree; `top` also
// scrolls them home.
void refresh_views_of_buf(App& app, BufferId bi, int except, bool top);
void checkpoint(App& app);
void mark_dirty(App& app);
// Edits outside mark_dirty (e.g. floating windows): dirty + draft pending.
void note_edit(App& app, DocBuffer& b);
void edit(App& app);
std::string find_help_path(App& app);
std::string find_data_file(App& app, const char* name);
std::string canon_path(App& app, const std::string& path);
BufferId find_buffer(App& app, const std::string& path);
BufferId ensure_buffer(App& app, const std::string& path);
// Buffer accessors. Never hold a DocBuffer& across add_buffer/remove_buffer.
DocBuffer* find_buf(App& app, BufferId id);
DocBuffer& buf(App& app, BufferId id);  // asserts the buffer exists
BufferId add_buffer(App& app, xanatoo::Document doc, std::string path);
// Removes the buffer and every pane / float / tab reference to it
// (repoints panes at the next tab; creates an untitled buffer if none left).
void remove_buffer(App& app, BufferId id);
// Close a tab: like remove_buffer, but keeps the buffer while a floating
// window still shows it.
void close_tab(App& app, BufferId id);
bool restore_closed_tab(App& app);
void insert_tab(App& app, BufferId id, BufferId opener = kNoBuffer);
BufferId tab_root(App& app, BufferId id);
void pop_out_focused(App& app);
// A tab dragged off the strip: its buffer moves into a window at (mx, my),
// docked when that is a dock zone.
void tear_tab(App& app, BufferId bi, int mx, int my);
void pop_in_soft(App& app, size_t si);
void open_browse(App& app, const std::string& path, const std::string& title);
void ensure_help_doc(App& app);
void layout_help(App& app);
void layout_context(App& app);
void draw_context_window(App& app);
std::tuple<int, int, int, int> help_client_rect(App& app);

// ---- view ----
void apply_view_settings(App& app, bool persist = true);
void refresh_outline(App& app);
void refresh_backlinks(App& app);
void invalidate_height_around(App& app, Pane& pane, xanatoo::NodeId id);
void drop_height_hints_for_buf(App& app, BufferId bi);
void toggle_section_fold(App& app, xanatoo::NodeId section);
void toggle_section_focus_mode(App& app);
void reload_folds_for_focused(App& app);
// The section, or a nested list's item right at the caret.
void toggle_fold_at_caret(App& app);
// Fold / expand a list item or stretch of view `pi`'s page (fold.hpp), in
// every view of the buffer. View state; only in edit mode is a stretch's
// state part of the document.
void set_fold(App& app, int pi, xanatoo::NodeId id, bool folded);
void toggle_fold(App& app, int pi, xanatoo::NodeId id);
// All items of the outermost list around `node`.
void set_list_folds(App& app, int pi, xanatoo::NodeId node, bool folded);
// All two-body stretchtext of the page: summaries or full texts.
void set_stretch_folds(App& app, int pi, bool folded);
// Switch the folds hiding `node`; false when none did.
bool reveal_folds(App& app, Pane& p, xanatoo::NodeId node);
// The page chrome and folds hiding a search hit.
void reveal_hit(App& app, Pane& p, xanatoo::NodeId node);
// Text find leaves alone: summaries of texts shown in full.
std::vector<xanatoo::NodeId> find_skips(App& app, Pane& p);
// The selection without the hidden half of two-body stretchtext.
std::string copied_text(App& app, Pane& p);
// The context menu's list item in the focused view, if it still is one in
// a list that can be folded.
xanatoo::NodeId current_list_target(App& app);
// Its two-body stretch.
xanatoo::NodeId current_stretch_target(App& app);
void refresh_diff(App& app);
void expand_ancestors_for_node(App& app, xanatoo::NodeId target);
// Reader mode (Pane::reader): whether the view hides the page chrome, and
// switching it (relayout, outline, status message).
bool pane_reader(const App& app, const Pane& p);
void set_reader(App& app, Pane& p, bool on);
// A jump / find hit inside hidden page chrome: leave reader mode first.
void reveal_chrome(App& app, Pane& p, xanatoo::NodeId target);
void layout_panes(App& app);
int tile_gutter_size(const App& app);
SDL_Rect tile_divider_rect(const App& app);
SDL_Rect tile_mode_button_rect(const App& app);
SDL_Rect tile_diff_track_rect(const App& app);
bool scroll_diff_overview(App& app, int x, int y);

// ---- Paged reading (app_pages.cpp, docs/PAGED.md) ----
bool pane_paged(const App& app, const Pane& p);
void set_paged(App& app, Pane& p, bool on);
// Text columns per spread of the view (the setting, or what fits).
int pane_page_columns(const App& app, const Pane& p);
// Height of the page bar below a paged view (0 for a scrolling one).
int page_bar_h(const App& app);
inline int pane_bar_h(const App& app, const Pane& p) {
  return pane_paged(app, p) ? page_bar_h(app) : 0;
}
// Height of the find bar below the focused view (0 for the others, or while
// closed); it goes under the page bar.
int find_bar_h(const App& app, const Pane& p);
// All that a view's text area leaves free at the bottom.
inline int pane_foot_h(const App& app, const Pane& p) {
  return pane_bar_h(app, p) + find_bar_h(app, p);
}
// layout_panes: cut the freshly laid out single-column pane.layout into
// pages (replaces it) when the view is paged; else clears pane.pages.
void paginate_pane(App& app, Pane& p, int columns, float column_w);
// Scroll position of spread `s` / the spread a content y lies in.
float spread_scroll(const Pane& p, size_t s);
size_t spread_at(const Pane& p, float content_y);
// Turn the page: spreads forward (+) or back (-); clamps. True if moved.
bool page_turn(App& app, Pane& p, int spreads);
void page_goto_spread(App& app, Pane& p, size_t spread);
// Where the view stands: pages (1-based), locations, percent.
struct PageStatus {
  size_t first_page = 1, last_page = 1, pages = 1;
  size_t location = 1, locations = 1;
  int percent = 0;
};
PageStatus page_status(const Pane& p);
// Pointer on the page bar or an edge zone of paged view `pi`: press /
// drag / release. Return true when consumed.
bool page_pointer_down(App& app, int pi, int x, int y);
// Paged view whose area (page bar included) lies under a window point, or -1.
int page_view_at(App& app, int x, int y);
bool page_pointer_drag(App& app, int x, int y);
void page_pointer_up(App& app);
void draw_page_chrome(App& app, int pi);  // bar, edge arrows, dividers
void ensure_virtual_layout(App& app, Pane& p);
std::optional<float> caret_doc_y(App& app, Pane& p, const xanatoo::Caret& c);
// The text at (x, view_y) of the pane's current layout, as it shows now.
std::optional<ViewAnchor> view_anchor_at(App& app, Pane& p, float x,
                                         float view_y);
// Call before anything that moves the page under a view: the top line of
// every view (or the anchor one already holds) stays on its spot through the
// next layout_panes. To hold another spot set Pane::view_anchor first.
void keep_views(App& app);
void force_full_pane_layout(App& app, Pane& p);
void scroll_caret_into_view(App& app, Pane& p);
void scroll_caret_to_top(App& app, Pane& p);
float pane_max_scroll(App& app, const Pane& p);
// Body/heading font size ± `steps` points (View → Zoom, pinch).
void zoom_by(App& app, int steps);
// New window size (resize event / per-frame sync): relayout, re-dock floats,
// and reveal the caret when the window got shorter (on-screen keyboard).
void apply_window_size(App& app, int nw, int nh);
// Push body_px / heading_px (scaled by the style sheet) into the fonts.
void apply_font_sizes(App& app);
// Body font file for FontSet::load (see App::font_path), "" = families.
std::string body_font_path(const App& app);
// Load style sheet `name` (re-reading the file), reload fonts if its
// families changed, relayout everything and remember it in view.conf.
void preview_style(App& app,
                   const std::optional<std::pair<xanatoo::UiCommand,
                                                 std::string>>& hover);
void apply_style(App& app, const std::string& name);
// Take the colours (group kStyleColors) or fonts (kStyleFonts) from sheet
// `name` on top of the sheet in use; "" = the sheet's own. Saved in
// view.conf (style_colors / style_fonts), kept across pinned sheets.
void apply_style_layer(App& app, unsigned group, const std::string& name);
void sync_pinned_style(App& app);
// Rescan style_dirs() and re-read the active sheet (after editing a .xss).
void reload_styles(App& app);
void nav_page(App& app, bool up);
void nav_move_block(App& app, bool prev);
void nav_move_line(App& app, bool up);
void nav_header(App& app, bool prev);
bool toggle_stretch_at_caret(App& app);
void collapse_or_expand_all(App& app, bool expand);
void jump_outline_sel(App& app);
// View under a window point: a document window's content rect, else the
// centre slot by x.
int pane_at(App& app, float mx, float my);
// Centre views' y / h from the chrome (menu, toolbar, status line).
void place_centre_rows(App& app);
// Over a floating window's frame or a panel, i.e. not over document text.
bool over_float_chrome(App& app, int x, int y);
float local_x(App& app, float mx, int pi);
// Scrollbar strip of a view: x of the strip, width reserved at the right of
// the content, and the layout width taken off the pane width.
inline int sb_x(const Pane& p) { return p.sb_left ? p.x - kSbW : p.x + p.w - kSbW; }
inline int sb_reserve(const Pane& p) {
  return p.pages.on || p.sb_left ? 0 : kSbW;
}
inline int sb_gutter(const Pane& p) {
  if (p.pages.on) return 0;
  return p.sb_left ? kSbLayoutGutter - kSbW : kSbLayoutGutter;
}
// Window y → content y of view `pi` (0 = top of its viewport).
float content_y(App& app, float window_y, int pi);
std::tuple<int, int, int, int> outline_client_rect(App& app);
float outline_row_step(App& app);
// Backlinks rows: same height as outline rows (paint + click).
inline float backlinks_row_step(App& app) { return outline_row_step(app); }
// Horizontal layout of one outline row, relative to the client x:
// disclosure triangle box (twist_w wide), text. Scales with the ui scale.
struct OutlineRowGeom {
  int twist_x = 0;
  int twist_w = 14;
  int text_x = 0;
};
OutlineRowGeom outline_row_geom(App& app, int level);

// ---- prompts ----
// ---- prompts ----
// True when `panel` is open and has keyboard focus (a closed panel never
// does, so closing needs no focus bookkeeping).
inline bool panel_has_keys(const App& app, KeyFocus panel) {
  if (app.key_focus != panel) return false;
  switch (panel) {
    case KeyFocus::Outline:
      return app.outline_mode;
    case KeyFocus::Backlinks:
      return app.backlinks_mode;
    case KeyFocus::Sitemap:
      return app.sitemap.shown;
    case KeyFocus::Page:
      return app.page.shown;
    case KeyFocus::History:
      return app.history_panel_open;
    case KeyFocus::Revisions:
      return app.revision_panel_open && !float_shaded(app.revision_win);
    case KeyFocus::Styles:
      return app.styles_panel_open;
    case KeyFocus::StyleManager:
      return app.styles_manager_panel_open;
    case KeyFocus::Context:
      return app.context_panel_open;
    default:
      return true;
  }
}

// Give KeyFocus::Panel to `role` (a panel without native keys), e.g. on open.
inline void focus_panel(App& app, WindowRole role) {
  app.key_focus = KeyFocus::Panel;
  app.panel_focus = role;
}

inline bool prompt_active(const App& app) {
  return app.prompt.kind != PromptKind::None;
}
inline bool prompt_is_search(const App& app) {
  return app.prompt.kind == PromptKind::Search ||
         app.prompt.kind == PromptKind::Replace;
}
// The prompt takes plain keys: every status-bar prompt, and the find bar only
// until its field has them (afterwards ImGui or the document does).
inline bool prompt_has_keys(const App& app) {
  return prompt_active(app) && (!prompt_is_search(app) || app.prompt.pending);
}
// Path/URL kinds edited through the Minibuffer (Tab completion, history).
bool prompt_uses_minibuf(PromptKind k);
// Opens `kind` (closing any prompt that was open); `initial` seeds the text.
void open_prompt(App& app, PromptKind kind, std::string initial = {});
// Pick a local file using SDL, or use a path prompt on ports without dialogs.
void open_file_dialog(App& app, PromptKind kind, std::string initial = {},
                      std::string source_path = {});
void poll_file_dialog(App& app);
// Closes the prompt and clears its text. Search text survives in last_search.
void close_prompt(App& app);
// The keys go elsewhere (document click, URL bar): closes status-bar
// prompts; the find bar stays open.
void leave_prompt(App& app);
// Opens the Find (or Find+Replace) bar, keeping the current / last query;
// `focus` puts the keys into its field, the query selected.
void open_search(App& app, bool replace, bool focus = true);
const char* prompt_label(PromptKind k);
// Enter pressed: commit the prompt that has the keys. Returns false when the
// key should fall through to the document (no prompt, or an empty one).
bool commit_prompt(App& app, bool ctrl, bool shift);

// ---- Find and replace (app_find.cpp) ----
// The query changed: the match at or after the place the typing started.
void find_retype(App& app);
// The bar's fields: "find text\nreplacement"; anything else is ignored.
void find_set_texts(App& app, const std::string& arg);
// Next / previous match of the query from the selection, wrapping.
void find_step(App& app, bool backward);
// Replaces the selection if it is a match, then goes to the next one.
void find_replace_one(App& app);
void find_replace_all(App& app);
// Matches of the query in the view, kept in Pane::search_ticks.
const std::vector<xanatoo::Caret>& find_hits(App& app, Pane& p);
struct FindStatus {
  size_t count = 0;
  bool capped = false;  // there may be more
  size_t index = 0;     // 1-based match that is selected, 0 = none
};
FindStatus find_status(App& app);
bool find_bar_shown(App& app);
SDL_Rect find_bar_rect(const App& app);

bool minibuf_active(App& app);
void minibuf_begin(App& app, const std::string& initial = {});
void minibuf_commit_path_hist(App& app);
void minibuf_commit_url_hist(App& app);
void minibuf_sync_out(App& app);
void flash_status(App& app, const std::string& msg);
// Content only on / off (app_chrome.cpp).
void toggle_chrome(App& app);

// ---- pointer ----
void open_first_gamepad(App& app);
void clear_interaction_hover(App& app);
void draw_stick_cursor(App& app, SDL_Renderer* r, int cx, int cy, bool pressed);
// Gamepad idle time after which the stick cursor is no longer drawn.
inline constexpr Uint32 kStickCursorIdleMs = 3000;
// Note gamepad input: shows the stick cursor again.
void note_stick_use(App& app);
// Stick cursor on and the pad used within kStickCursorIdleMs.
bool stick_cursor_visible(const App& app);
void set_cursor(App& app, SDL_Cursor* c);
void reset_blink(App& app);
// The focused view's selection was set by the program (find match): keep
// it out of the X11 / Wayland primary selection.
void note_program_selection(App& app);

// Left-button press / drag / release at window point (x, y); the mouse
// handlers and touch taps share these (app_events.cpp).
void tile_split_press(App& app, int x, int y);
void pointer_left_down(App& app, int x, int y, bool imgui_mouse);
void pointer_motion(App& app, int x, int y, bool imgui_mouse);
void pointer_left_up(App& app, int x, int y);
// Right press: the context menu of what lies at (x, y); touch long press too.
void pointer_right_down(App& app, int x, int y, bool imgui_mouse);
// The pointer left the window (a touch ended): drop every hover effect, in
// the document and in ImGui, until the next motion. Off-window position:
inline constexpr float kPointerAway = -100000.f;
void pointer_leave(App& app);

// ---- touch (app_touch.cpp) ----
// Mouse events SDL synthesises from a touchscreen (which == SDL_TOUCH_MOUSEID).
bool is_touch_mouse_event(const SDL_Event& ev);
void touch_finger_down(App& app, const SDL_TouchFingerEvent& tf);
void touch_finger_motion(App& app, const SDL_TouchFingerEvent& tf);
void touch_finger_up(App& app, const SDL_TouchFingerEvent& tf);
// Per frame: long-press timer and fling momentum.
void update_touch(App& app);
void draw_touch_hold(const App& app);

// ---- nav ----
// Records `url` as the page view `pi` now shows (its trail + session log).
void push_nav_history(App& app, int pi, const std::string& url);
// The current Back / Forward entry of view `pi` remembers the snapshot its
// page is (NavHistoryEntry::snapshot), when the view shows that page.
void note_entry_snapshot(App& app, int pi);
// The snapshots bookmarks were made on are pinned against cache pruning
// (snapshot_set_pins "bookmark"): after app.bookmarks changed.
void sync_bookmark_pins(App& app);
// The page in view `pi` was loaded again (Reload): a Reload visit whose
// source is the visit it replaces, with the new observation.
void record_reload_visit(App& app, int pi);
// `url` went to the Media panel (play_audio) from view `pi` (-1: none,
// e.g. the command line): a visit of its own, reached from the page the
// view shows (which stays), so the History panel lists what was played.
// `resume` (App::visit_resume, with a `history` hint and `from`): played
// again from the History panel, the player takes the picked visit `from`
// instead of recording a new one.
void record_media_visit(App& app, int pi, const std::string& url,
                        std::optional<xanatoo::VisitKind> hint,
                        int64_t from = 0, const NavLink& via = {},
                        bool resume = false);
// Keep the current history stop's view position up to date; also finish
// restores deferred until an asynchronously fetched page arrives.
void sync_nav_history(App& app);
// The link to be highlighted when Back has returned to its source page.
const NavLink* forward_nav_link(App& app, int pi);
// The link just clicked in view `pi` while its target page is still being
// fetched for that view (the page shown is still the link's page).
const NavLink* pending_nav_link(App& app, int pi);
// Recompute Pane::open_tab_links of view `pi` when its layout or the open
// tabs changed.
void update_open_tab_links(App& app, int pi);
// Recompute Pane::playing_links of view `pi` when its layout or the file in
// the Media panel changed (none while the player is idle).
void update_playing_links(App& app, int pi);
// Recompute Pane::visited_links of view `pi` when its layout or the visit
// log changed.
void update_visited_links(App& app, int pi);
// Back (dir −1) / Forward (+1) in the focused view; false when there is
// no page that way.
bool can_nav_step(App& app, int dir);
bool nav_step(App& app, int dir);
// Steps a view's Back / Forward trail to the entry of one of `visits` (a
// History panel row), focusing that view; no new visit is recorded. False
// when no view's trail holds them.
bool nav_rewind_to_visit(App& app, const std::vector<int64_t>& visits);
bool is_bookmarked(App& app, const std::string& url);
void add_bookmark(App& app, xanatoo::Bookmark b);
void remove_bookmark(App& app, const std::string& url);
// Folder systems (docs/UI.md "Bookmarks"). Folder paths are "/"-separated;
// each call stores the change and bumps `bookmarks_gen`.
void move_bookmark(App& app, const std::string& address, const std::string& folder);
void retitle_bookmark(App& app, const std::string& address, const std::string& title);
void create_bookmark_folder(App& app, const std::string& path);
// Moves (renames or re-parents) a folder with its contents.
void move_bookmark_folder(App& app, const std::string& from, const std::string& to);
// Removes a folder; its bookmarks and subfolders move up one level.
void remove_bookmark_folder(App& app, const std::string& path);
// Picks up bookmarks / input histories another xanatoo process changed in
// the shared state database (statedb.hpp); checks about once a second.
void poll_shared_state(App& app);
bool is_inapp_doc_href(App& app, const std::string& href);
std::string resolve_doc_href(App& app, const std::string& href,
                             const std::string& base_path = {});
// Page the Home button opens (welcome.md); empty when not found.
std::string home_page(App& app);
// Parent of the focused page (directory, URL path level, subreddit, …) for
// the Up button; "" at the top.
std::string page_up_url(App& app);
// Fetch the remote page of view `pi` again; its passage stays.
void refetch_page(App& app, int pi);

// app_passage.cpp: the passage a view shows (docs/PASSAGES.md).
// Node the view is cut to, INVALID_NODE for the whole document.
xanatoo::NodeId pane_cut(App& app, Pane& p);
// Full address of what the view shows: page + selectors. Local pages are
// file:///abs/path URLs.
std::string pane_address(App& app, Pane& p);
// The http(s) address of a page shown under another one (page_web_url, and
// the result page of a search:); "" for web pages and pages not on the web.
std::string pane_web_url(App& app, Pane& p);
// What Open in Browser opens: pane_web_url, else the page's own http(s)
// address; "" when the page has none.
std::string pane_browser_url(App& app, Pane& p);
// The address spelling of a page: a local path (relative to the cwd) as its
// file: URL, everything else unchanged.
std::string page_address(std::string_view page);
// `address` with its base spelled by page_address (old plain-path bookmarks
// and history compare equal to file: URLs).
std::string canonical_address(std::string_view address);
// Local date, YYYY-MM-DD (Bookmark::created).
std::string today();
// Both name the same page (path vs file: URL).
bool same_page(std::string_view a, std::string_view b);
// Show `loc`'s selectors in the page the view already shows, no load. A
// passage that cannot be found drops out of the address with a status flash.
void apply_locator(App& app, int pi, xanatoo::Locator loc);
// Cut the view to `node` (INVALID_NODE: the whole document).
void set_pane_cut(App& app, Pane& p, xanatoo::NodeId node);
// Address of view `p`'s page cut to `node` (page//id:… or page//path:…).
std::string block_address(App& app, Pane& p, xanatoo::NodeId node);
// The context menu's "Open …" verbs on app.context_blocks[index]: cut the
// view it was opened in, or a new document window on the same buffer.
void open_context_block(App& app, size_t index, bool new_window);
// Bookmark of what the view shows: the selection when there is one, with
// `position` the caret, else the page.
xanatoo::Bookmark bookmark_for_view(App& app, Pane& p, bool position);
// Forget the passage, for a view that gets another document.
void reset_pane_locator(Pane& p);
// Drop the `#` passage: address and mark, the view stays.
void clear_pane_point(App& app, Pane& p);
// The document of `p` was replaced: find the passage in the new one.
void refresh_pane_locator(App& app, Pane& p);
// Find the bookmarks inside the page `p` shows again, when the page, its
// layout or the bookmarks changed (ViewSettings::bookmark_marks).
void update_bookmark_marks(App& app, Pane& p);
// Number of bookmarks with selectors inside the page `p` shows.
int bookmarks_in_page(App& app, Pane& p);
// True while the marks match the layout (not waiting for a typing pause).
bool bookmark_marks_fresh(const Pane& p);
void update_annotation_marks(App& app, Pane& p);
bool annotation_marks_fresh(const Pane& p);
void select_annotation(App& app, int pi, const std::string& annotation_id);
std::vector<AnnotationIcon> annotation_icons(App& app, Pane& p);
const AnnotationMark* annotation_icon_at(App& app, Pane& p,
                                         float lx, float my);
// Margin icons of the marks on screen (one per bookmark, those on one line
// side by side), pane-local x, y relative to the pane's content top.
std::vector<BookmarkIcon> bookmark_icons(App& app, Pane& p);
// The bookmark whose margin icon is at (lx, my) (pane-local / content y),
// or nullptr.
const BookmarkMark* bookmark_icon_at(App& app, Pane& p, float lx, float my);
// Quote links (app_quotes.cpp, docs/QUOTES.md).
const std::vector<xanatoo::QuoteLink>& quote_links_of(DocBuffer& b);
std::vector<QuoteIcon> quote_icons(App& app, Pane& p);
long quote_icon_at(App& app, Pane& p, float lx, float my);
// Margin arrow, or a hit inside a linked quote / source span.
long quote_at(App& app, Pane& p, float lx, float my);
// Only a hit inside a linked quote / source span (no margin arrow).
long quote_span_at(App& app, Pane& p, float lx, float my);
// Open the Context panel on quote link `link` of view `pi` (touch-friendly).
void show_quote_in_context(App& app, int pi, size_t link);
// Fills App::context_quotes for a right-click at `at` in pane `p`.
void collect_context_quotes(App& app, Pane& p, std::optional<xanatoo::Caret> at);
// Jump view `pi` along quote link `link` (to its original or to the quote),
// marking the target; Back returns.
void jump_quote(App& app, int pi, size_t link, bool to_source);
// Context panel: fill UiState from hover (quote / transclusion source).
void refresh_context_panel(App& app);
// Absolute address of link `href` in the Context panel's document
// (resolved against App::context_address); empty when it names nothing.
std::string context_link_target(App& app, const std::string& href);
// Click on a Context panel link: open it in the focused view (a new tab
// while App::click_new_tab is set); outside links go to the system browser.
void follow_context_link(App& app, const std::string& href);
// Link under window point (mx, my) in the Context panel's page (the hit-test
// of both its clicks and its hand cursor).
std::optional<xanatoo::LinkHit> context_link_at(App& app, int mx, int my);
std::string quote_tooltip(App& app, int pi, size_t link);
void draw_quote_marks(App& app, int pi, float ox, float py, float draw_scroll);
// Select the passage of bookmark `address` in view `pi` and scroll to it.
void select_bookmark(App& app, int pi, const std::string& address);
// `address` with its point turned into the last pipe: the passage as a cut.
std::string cut_address(const std::string& address);
// The stored bookmark with `address`, or nullptr.
const xanatoo::Bookmark* find_bookmark(App& app, const std::string& address);
// Open `target` (path, gemini://, Nostr, Reddit, Lemmy) in the focused pane /
// in pane `pi`. Remote pages load in the background; true = opened or started.
bool navigate_to_url(App& app, std::string target, bool record_history = true,
                     NavLink via = {});
bool navigate_pane(App& app, int pi, std::string target,
                   bool record_history = true, NavLink via = {});
// Same-document #anchor as a new history stop, retaining the source link.
bool navigate_anchor_link(App& app, int pi, const std::string& href,
                          NavLink via = {});
// What a link `href` in pane `pi` points at.
enum class LinkKind : uint8_t { None, Anchor, Doc, Remote, External };
struct LinkTarget {
  LinkKind kind = LinkKind::None;
  std::string url;       // Doc: path without fragment; Remote: absolute URL
  std::string fragment;  // Doc: "#heading" (may be empty)
  xanatoo::Caret dest{};  // Anchor: target in the pane's own document
};
LinkTarget resolve_link(App& app, int pi, const std::string& href);
// Middle click: open `href` (in pane `pi`) in a new tab right after the
// current one; the view stays. Anchors into the page itself just jump.
BufferId open_link_in_tab(App& app, int pi, const std::string& href,
                          bool inherit_history = true, bool defer_fetch = false);
void open_image_in_tab(App& app, int pi, const std::string& address);
NavHistory& view_nav(App& app, int pi);
void select_view_tab(App& app, int pi, BufferId id);
void finish_new_tab_history(App& app, BufferId id);
void note_background_navigation(App& app, BufferId id, const std::string& url,
                                bool record_history);
// Panel rows (History, Sitemap, Page…) open an address here: a new tab while
// `App::click_new_tab` is set (middle-click), else the focused view.
void open_address(App& app, const std::string& url);
// Middle-click on a panel row: same as a click, but pages open in a new tab.
// Returns true when a panel row consumed it.
bool middle_click_float(App& app, int mx, int my);
// Put a freshly loaded page into pane `pi`: reuses the pane's buffer unless
// the other visible pane shows it too or it has unsaved edits. `label` is
// the page's address, `content_path` the local file it was read from when
// that differs (DocBuffer::content_path).
void show_doc_in_pane(App& app, int pi, xanatoo::Document doc,
                      const std::string& label,
                      std::optional<std::string> raw_source = std::nullopt,
                      std::string content_path = {},
                      xanatoo::PageObservation observed = {});
// Links in pane `pi` resolve against that pane's page (gemini relative URLs).
std::string link_base_url(App& app, int pi);
void open_transclude_in_other_pane(App& app);
void activate_at_caret(App& app);

// ---- background fetches (app_fetch.cpp) ----
// True when this build can fetch `url` (otherwise *why explains).
bool remote_fetch_supported(std::string_view url, std::string* why);
// Start fetching `url`; a Navigate job replaces the pane's running one.
// Tab jobs have no pane (-1) and fill buffer `tab_buf`.
uint64_t start_fetch(App& app, FetchPurpose purpose, int pane,
                     const std::string& url, bool record_history = true,
                     BufferId tab_buf = kNoBuffer,
                     bool preview_pages = false);
// `preview_pages`: a Preview may open a PDF / DjVu / comic answer as pages
// (transfers the whole file). Only the Context panel asks for that; split
// previews and subscriptions refuse such answers.
// "Download Link": save the http(s) answer to Downloads whatever its type
// (a PDF that would otherwise open as pages, or even an HTML page).
// Open the download confirmation dialog for an http(s) URL.
void start_download(App& app, const std::string& url);
// Actually start the transfer (after the user confirms). `dest_dir` empty
// uses the default Downloads folder.
void begin_download(App& app, const std::string& url,
                    std::string dest_dir = {});
// Draw the pending-download modal (Cancel | Download [to…] | Download).
// Legacy no-op (dialog is drawn inside Ui::begin_frame).
void draw_download_confirm(App& app);
// True when SDL can show a native folder picker (enables "Download to…").
bool download_folder_dialog_available();
// Open the native folder picker; on success, begin_download into that dir.
void pick_download_folder(App& app, const std::string& url);
// Main loop: start the downloads whose folder the (possibly threaded) SDL
// folder dialog has answered.
void poll_download_folder_picks(App& app);

// Apply finished jobs (once per frame).
void poll_fetches(App& app);
// Subscriptions use the same read-only fetch workers, never a pane or tab.
bool subscribable_source(std::string_view address);
std::string subscription_source_address(std::string_view address);
void add_subscription(App& app, std::string address);
void remove_subscription(App& app, const std::string& address);
void open_subscription(App& app, const std::string& address);
void mark_subscription_read(App& app, const std::string& address);
void request_subscription_refresh(App& app, const std::string& address = {});
void set_subscription_interval(App& app, const std::string& address, int minutes);
void poll_subscriptions(App& app);
void finish_subscription_fetch(App& app, const FetchJob& job);
// Gemini certificates (app_certs.cpp): "host:port", the key of App::gemini_certs.
std::string gemini_cert_key(std::string_view host, int port);
// Record what a finished fetch met; a changed certificate opens the panel.
void note_gemini_peer(App& app, const FetchJob& job);
// The certificate record of the page view `pi` shows, or null.
GeminiSiteCert* pane_gemini_cert(App& app, int pi);
// Address-bar site icon and Certificates panel rows for this frame.
void cert_ui_state(App& app, xanatoo::UiState& st);
// Open the Certificates panel on `key` (empty: the focused page's server).
void show_certificates(App& app, const std::string& key = {});
void refresh_cert_pins(App& app);
// Trust the changed certificate of `key` and reload the page that met it.
void accept_gemini_cert(App& app, const std::string& key);
// Keep the old pin: the new certificate stays refused.
void reject_gemini_cert(App& app, const std::string& key);
// Remove the pin: the next visit trusts what the server presents then.
void forget_gemini_cert(App& app, const std::string& key);
// Identity panel (app_identity.cpp): Gemini client certificates.
// Opens it on `section`, or without one on the focused page's protocol.
void show_identities(App& app);
void show_identities(App& app, xanatoo::IdentitySection section);
void refresh_identities(App& app);
// Record a page's request for a certificate (60/61/62); opens the panel.
void note_gemini_identity(App& app, const FetchJob& job);
// The identity the focused page's address goes out with, or null.
const xanatoo::GeminiIdentity* pane_gemini_identity(App& app, int pi);
void identity_ui_state(App& app, xanatoo::UiState& st);
// UiCommand::Identity* (arg as documented in ui.hpp).
void identity_command(App& app, xanatoo::UiCommand cmd, const std::string& arg);
// A view that switched tabs leaves its running navigation to the tab it
// started in (a background Tab job); switching back takes it up again.
// Called by poll_fetches.
void retarget_fetches(App& app);
// Drop pane `pi`'s running navigation (-1: all navigations). True if any.
bool cancel_fetches(App& app, int pi = -1);
void cancel_preview_fetch(App& app);
// Running navigation for pane `pi`, else its preview fetch (pane 1), or null.
const FetchJob* pane_fetch(const App& app, int pi);
const CachedPage* find_cached_page(App& app, const std::string& url);
void forget_cached_page(App& app, const std::string& url);  // Reload
// Progress bar + "Loading host…" label on panes with a running fetch.
void draw_load_indicator(App& app, int pi);
// Browser-style downloads panel: active transfers and completed files.
void draw_downloads_panel(App& app);
void click_downloads_panel(App& app, int mx, int my);
// Right-click on a Downloads row: remember it, open the row menu.
bool right_click_downloads_panel(App& app, int mx, int my);
// UiCommand::DownloadAction on App::download_menu.
void download_action(App& app, const std::string& action);
void load_download_history(App& app);
void save_download_history(const App& app);

// ---- footnotes (app_footnotes.cpp) ----
// Notes of the footnote marks on screen, as a strip at the bottom of pane
// `pi` (View → Footnote overlay). Call after the pane's content is drawn.
void draw_footnote_overlay(App& app, int pi, const SDL_Rect& clip);
// Pane-local point (lx, viewport y) inside pane `pi`'s footnote strip?
bool in_footnote_overlay(const App& app, int pi, float lx, float my);
// Press on the strip: jump to the note under it. True = press consumed.
bool click_footnote_overlay(App& app, int pi, float lx, float my);
// Link in pane `pi`'s footnote strip at pane-local (lx, viewport y); its
// node / span are of FootnoteOverlay::doc.
std::optional<xanatoo::LinkHit> footnote_overlay_link(const App& app, int pi,
                                                      float lx, float my);
// Footnote node whose reference mark is under the pointer in pane `pi`
// (INVALID_NODE = none); highlighted in the strip and the Context panel.
xanatoo::NodeId hovered_footnote(App& app, int pi);

// ---- collections (app_collection.cpp) ----
// Collection under the caret (or the last right-click target), else INVALID.
xanatoo::NodeId current_collection(App& app);
// UiCommand::CollectionView: "grid" / "list" / "full", "small" / "medium" /
// "large", "sort-doc" / "sort-name" / "sort-name-desc", "view" (the
// collection's `//gallery`, at its first image).
void collection_command(App& app, const std::string& arg);
// Press on a collection card or image in pane `pi`: follow its link, or show
// it in the pane's `//gallery//item:N`. True = consumed.
bool click_collection_card(App& app, int pi, float lx, float my);
// Link to an image (http(s) or a local file, `is_image_url`) in pane `pi`:
// show it in the page's gallery. False (nothing done) when `href` is no
// image link.
bool open_link_image(App& app, int pi, const std::string& href);
// Right-click: remember the collection under the point for the menu.
void note_collection_target(App& app, int pi, float lx, float my);

// ---- gallery views (app_gallery.cpp, docs/PASSAGES.md "Gallery") ----
// Images of `scope` (INVALID = the whole page) in gallery order: document
// order, a Collection scope in its view sort.
std::vector<xanatoo::GalleryItem> page_gallery_items(App& app,
                                                     const xanatoo::Document& d,
                                                     xanatoo::NodeId scope);
// apply_locator / refresh_pane_locator for an address with `//gallery`:
// builds the grid document and resolves `//item:N` / `#item:N`. `keep`
// keeps scroll and zoom (refresh of the same view).
void apply_gallery_locator(App& app, int pi, xanatoo::Locator loc, bool keep);
// The pane's gallery view, when it shows one.
GalleryView* pane_gallery(Pane& p);
// The pane shows a single gallery image (not the grid).
bool pane_image_view(const Pane& p);
// Up from a single image: the page it is in, at the image when the gallery
// was of the whole page or one collection (the grid is the corner button /
// `g`). "" when the pane shows no single image.
std::string gallery_up_address(App& app, Pane& p);
// Navigate pane `pi` to `//gallery//item:N` of the page it shows, with the
// pipes `scope_pipes` before `//gallery` (a cut or a Collection).
void open_gallery_item(App& app, int pi,
                       std::vector<xanatoo::Selector> scope_pipes, int index);
// Single image: next / previous image (replaces the history entry).
void gallery_step(App& app, int pi, int delta, bool wrap = false);
// The image download of `url` answered with a web page (an address named
// like a picture, "/wiki/File:X.jpg"): every view showing it as its single
// gallery image opens that page instead, in place of the image's history
// entry.
void gallery_image_was_page(App& app, const std::string& url);
// Input for single-image views. Pointer events go to the image view under
// the pointer, keys to the focused one. True = consumed.
std::string gallery_image_at(App& app, Pane& p, int x, int y);
bool gallery_pointer_down(App& app, int pi, int x, int y, int button);
bool gallery_pointer_motion(App& app, int x, int y);
bool gallery_pointer_up(App& app);
bool gallery_wheel(App& app, int pi, int x, int y, int dy);
bool gallery_key(App& app, SDL_Keycode key, bool ctrl, bool alt, bool shift);
// Zoom the image of pane `pi` by `factor` around window point (x, y).
void gallery_zoom(App& app, int pi, float factor, float x, float y);
// Pan it by (dx, dy) window pixels; false when it is not zoomed in.
bool gallery_pan(App& app, int pi, float dx, float dy);
// Draw pane `pi`'s single image into its rect (instead of the document).
void draw_gallery_image(App& app, int pi);
// The Context panel's picture preview: the original of `src` (relative to
// `base`) fitted into `r`, never enlarged, on the gallery background with
// its name and size below (or the loading / failure note).
void draw_context_image(App& app, const SDL_Rect& r, const std::string& src,
                        const std::string& base);
// The grid's Up strip (top edge, hover highlight), over the drawn page.
void draw_gallery_grid_chrome(App& app, int pi);
void release_gallery_textures(App& app);
// While alive, the layout engine and the renderer use the `//gallery` grid
// look (xanatoo::gallery_view_style of app.style) if `p` shows a gallery
// grid; app.style again afterwards.
struct GalleryStyleScope {
  GalleryStyleScope(App& app, Pane& p);
  ~GalleryStyleScope();
  GalleryStyleScope(const GalleryStyleScope&) = delete;
  GalleryStyleScope& operator=(const GalleryStyleScope&) = delete;
  App& app;
  std::optional<xanatoo::DocStyle> style;
};

// ---- fixed-layout pages (app_pagetiles.cpp, docs/FIXED_LAYOUT.md) ----
// Once per frame before drawing: run thumtoo's tile scheduler when it asked
// for it, drop views / textures that went off screen.
void pump_page_tiles(App& app);
// Paint the visible Page images of view `pi` from tiles (screen y =
// content y - draw_scroll), inside `clip`; after the pane raster.
void draw_page_tiles(App& app, int pi, const SDL_Rect& clip, float draw_scroll);
// Shutdown: textures and outstanding requests.
void release_page_tiles(App& app);

// ---- remote images (app_images.cpp) ----
// Start downloads ImageCache asked for and apply finished ones (relayout,
// at most every 150 ms while images keep arriving). Once per
// frame, after layout. Images marked with want_image_now jump the queue.
void poll_images(App& app);
// Remote image `url` is on screen and still waiting: the next poll_images
// moves it to the front of the download queue (main thread, every frame).
void want_image_now(App& app, const std::string& url);
// Download state of remote image `url` (main thread).
ImageLoadState image_load_state(App& app, const std::string& url);
// Progress over the visible image placeholders of view `pi` (still
// downloading / thumbnail building / failed); painted every frame on top of
// the pane raster, which keeps the plain placeholder.
void draw_image_loading(App& app, int pi, const SDL_Rect& clip, float draw_scroll);
// A progress bar in `r`: filled by `st` when its size is known, else a
// sweeping segment. `track` is the bar's background.
void draw_image_progress_bar(App& app, const SDL_Rect& r,
                             const ImageLoadState& st, xanatoo::Rgba track);
// Trim the download and thumbnail caches (background, at startup).
void prune_image_caches();

// ---- linked tile navigation (app_split.cpp) ----
// Only Trail links the two centre tiles: first-tile links open in the
// second; second-tile navigation advances the shared history.
bool split_browsing(const App& app);
void enter_tile_trail(App& app);  // resume matching trail, or seed anew
// Follow `href` clicked in pane `pi` the split-browsing way. False = not
// handled (caller falls back to single-pane behaviour).
bool follow_link_split(App& app, int pi, const std::string& href);
// Context menu: show the page of pane `pi` in Trail with `href` opened
// beside it (enters Trail first; a non-centre view moves to the left tile).
void open_link_in_trail(App& app, int pi, const std::string& href);
// Per frame: dwell-timed hover previews for links in the left pane.
void update_split_preview(App& app);
// The previewed page stays (a click / scroll in the right pane).
void commit_split_preview(App& app);
void revert_split_preview(App& app);
// "Preview" frame + label over the right pane while a preview shows.
void draw_preview_badge(App& app, int pi);
// Href of the left-pane link the right pane shows, "" = none (or preview).
const std::string& split_trail_href(const App& app, int pi);
// Link on the visible page which Forward would follow in the split trail.
const std::string& split_history_forward_href(App& app, int pi);
// The node the trail link jumped to in the right pane, or INVALID_NODE.
xanatoo::NodeId split_trail_target(App& app, int pi);
// push_nav_history forwards centre-pane pages here while split browsing.
void split_history_note(App& app, int pi, const std::string& url);
// Back / Forward over the split trail (nav_step / can_nav_step use these
// while split browsing). `pos` / `total` for the status line "3/7".
bool split_history_can_step(const App& app, int dir);
bool split_history_step(App& app, int dir);
bool split_history_jump(App& app, int steps);
bool split_history_position(const App& app, int& pos, int& total);

// ---- softwin ----
std::tuple<int, int, int, int> soft_client_rect(App& app,
                                                const FloatWindow& sw);
void snap_soft_geom(App& app, int& x, int& y, int w, int h);
std::tuple<int, int, int> soft_client_bounds(App& app);
// Docking: one placement model for every FloatWindow role.
// Drop target for a title dragged to (mx, my); None = keep floating.
Dock compute_dock_zone(App& app, int mx, int my);
// Area a Dock covers (column / band for Left, Right, Top, Bottom; client
// area for Max).
std::tuple<int, int, int, int> dock_rect(App& app, Dock d);
// Width taken from the panes by visible windows docked on `side`; 0 = none.
int dock_column_w(App& app, Dock side);
// Height taken from the panes by visible windows docked at Top / Bottom.
int dock_band_h(App& app, Dock side);
// Resize the column / band on `side` to `px` (gutter drag): it may grow
// until the panes keep a small strip; the opposite side stays as shown.
void dock_resize_side(App& app, Dock side, int px);
// Paint the gutters around docked panels (before the panels draw).
void draw_dock_gutters(App& app);
// The mouse may light native canvas chrome under it: no ImGui window or
// popup and no floating window in front (other than `owner`), pointer not
// away. Gutters and scrollbars show hover feedback only then.
bool canvas_hover(App& app, const FloatWindow* owner = nullptr);
// Dock `w` at `d`; for a column or band, the pointer (`mx`, `my`) picks its
// slot among the others.
void dock_window(App& app, FloatWindow& w, Dock d, int mx, int my);
// Back to free placement at the remembered geometry.
void undock_window(App& app, FloatWindow& w);
// Max ↔ previous placement (title double-click).
void toggle_maximize(App& app, FloatWindow& w);
// Roll a docked window up to its title strip and back; its dock neighbours
// (or, for a band whose panels are all rolled up, the panes) take the room.
// No-op for free and maximized windows.
void toggle_shade(App& app, FloatWindow& w);
// The keyboard focus a click on `w` gives (Panel for roles without keys of
// their own, Document for document windows).
KeyFocus float_key_focus(const FloatWindow& w);
// Make `w` the active window (a rolled-up panel gives focus to the document).
void focus_float(App& app, const FloatWindow& w);
// `w` is the active window: its title bar is drawn lit. At most one is.
bool float_active(const App& app, const FloatWindow& w);
// Derive docked / maximized geometry, keep floating windows on screen.
void place_windows(App& app);
const std::string& soft_path(App& app, const FloatWindow& sw);
std::string soft_title(App& app, const FloatWindow& sw);
void raise_soft(App& app, size_t i);
// True when (x, y) is over the document tab strip.
bool over_tab_strip(App& app, int x, int y);
// Close soft_wins[i] and free its view.
void close_doc_window(App& app, size_t i);
// Document window showing view `pi`, nullptr for a centre view.
FloatWindow* window_of_view(App& app, int pi);

// ---- page panel (app_page.cpp) ----
// Regions the Page panel lists (navigation, side bars); walks the document.
size_t page_nav_regions(const xanatoo::Document& doc);
// Whether the focused view's page has any (from its last layout).
bool page_has_navigation(App& app);
void toggle_page_panel(App& app);
void update_page_panel(App& app);
void draw_page_panel(App& app);
std::tuple<int, int, int, int> page_client_rect(App& app);
float page_max_scroll(App& app);
void click_page(App& app, int mx, int my);
bool page_key(App& app, SDL_Keycode sym);

// ---- sound player (app_audio.cpp) ----
// Start playing `url` (local path, file: URL or http(s) address) in place
// of what plays now. A Media Fragments time (`#t=30`) is where it starts;
// for the file already in the player it only goes there. True = started
// loading (or went there). `mime`: the Content-Type the address answered
// with, when known (an address without a file name extension); it picks the
// decoder as the extension does otherwise. `name`: the file name the server
// gave (Content-Disposition), else the title comes from the address. Remote
// files play while they download.
bool play_audio(App& app, const std::string& url, std::string_view mime = {},
                std::string_view name = {});
void audio_toggle(App& app);        // play / pause (replay once ended)
void audio_stop(App& app);          // stop and hide the player
void audio_seek(App& app, double seconds);  // to this position
void audio_skip(App& app, double delta);    // forward (+) / back (-)
// Video: pause and show the next (dir > 0) / previous picture.
void audio_step_frame(App& app, int dir);
double audio_position(const App& app);      // seconds
// A / B markers on / off (on: around the current position, 10 s).
void audio_toggle_ab(App& app);
// Loop on / off: the A–B section while the markers are on, else the file.
void audio_toggle_loop(App& app);
// Per frame: finished loads start playing, the end of the file is noted.
void update_audio(App& app);
void shutdown_audio(App& app);
// Media panel (WindowRole::Media): the player's controls. Opening it does
// not play anything; closing it stops the sound.
void toggle_media_panel(App& app);
void close_media_panel(App& app);
void draw_media_panel(App& app);
// Press in the panel's client area (buttons, seek bar, volume).
void click_media_panel(App& app, int mx, int my);
// Status text for the part of the panel under the pointer ("" = none).
std::string media_panel_hover(const App& app, int mx, int my);
// Right press in the panel: the bookmark menu for the seek bar position
// under the pointer (elsewhere: the current position). True when the
// press was inside the client area (taken, menu or not).
bool right_click_media_panel(App& app, int mx, int my);
// The playing video fills the screen / back into the panel
// (UiCommand::AudioFullscreen, the panel's button, a double click on it).
void toggle_media_fullscreen(App& app);
inline bool media_fullscreen(const App& app) { return app.media_full.on; }
// The panel's controls hidden / shown (UiCommand::AudioControls, H): the
// video alone fills the panel.
void toggle_media_controls(App& app);
// The panel shows the picture alone (controls hidden, a video, not fullscreen).
bool media_bare(const App& app);
// While fullscreen: takes the pointer, wheel and keys (true = taken).
bool media_fullscreen_event(App& app, const SDL_Event& ev);
// The Media panel with key focus (not fullscreen): Space, arrows, F, ',' / '.'
// … control the player (true = taken); other keys go on to the document.
bool media_panel_key(App& app, const SDL_Event& ev);
// While fullscreen: the picture and its controls over the whole window.
void draw_media_fullscreen(App& app);
// UiCommand::MediaAction on App::media_menu: bookmark-time / unbookmark-time
// / bookmark-file / unbookmark-file / seek / copy-time / set-a / set-b /
// bookmark-section / unbookmark-section / copy-section / use-section.
void media_action(App& app, const std::string& action);
// The address a played file is stored under (visits, bookmarks): the base
// as canonical_address, a `#t=` start kept in Media Fragments form.
std::string canonical_media_address(std::string_view address);
void audio_set_volume(App& app, float volume);  // 0..1

// ---- search bar (app_search.cpp) ----
void init_search(App& app);
std::string search_engine_id(const App& app);  // the engine in use
void show_search_bar(App& app);                // open and focus
void toggle_search_bar(App& app);
void select_search_engine(App& app, const std::string& id);
// Switch an engine on / off in the menu (saved in view.conf).
void toggle_search_engine(App& app, const std::string& id);
// Location bar search button: open the search bar on the site engine.
// Not a toggle — never hides the bar.
void search_site(App& app, const std::string& id);
// Search menu "Add …": reads the OpenSearch description at `url` in the
// background, then saves the engine to search.sexp and selects it.
void add_search_engine_from(App& app, const std::string& url);
void poll_search_engine_adds(App& app);
// Preferred languages (ViewSettings::languages) → Accept-Language.
void apply_preferred_languages(const App& app);
void set_preferred_languages(App& app, std::vector<std::string> tags);
void run_search(App& app, const std::string& text);  // → search: address
// Ctrl+L text "w xanadu" → search address ("" when it is no keyword search).
std::string search_from_location(const App& app, const std::string& text);
// Typed location → address: a keyword search, "./a.md" / "/a" / "~/a" →
// file://, a scheme as is, a bare name of an existing (or bundled) file as
// is, anything else → https:// (app_nav.cpp).
std::string complete_location(App& app, const std::string& text);

// ---- related sites panel (app_related.cpp) ----
void toggle_related_sites(App& app);
// Follow the focused page's site, start / finish loads (every frame).
void update_related_sites(App& app);
// Ask again for the shown site (drops this session's answer).
void refresh_related_sites(App& app);
// Follow toggle: off holds the list on the shown site.
void toggle_related_follow(App& app);
void related_ui_state(App& app, xanatoo::UiState& st);

// ---- sitemap panel (app_sitemap.cpp) ----
void toggle_sitemap(App& app);
// Per frame while shown: follow the focused page, collect finished loads.
void update_sitemap(App& app);
void draw_sitemap_panel(App& app);
std::tuple<int, int, int, int> sitemap_client_rect(App& app);
float sitemap_max_scroll(App& app);
void click_sitemap(App& app, int mx, int my);
// Keys while the panel has focus (arrows, ←/→ fold, Enter); true = used.
bool sitemap_key(App& app, SDL_Keycode sym);
// `sitemap:host` of the panel's site in the focused pane.
void open_sitemap_page(App& app);
// URL-bar Site map button: the focused page's site is known to have a
// sitemap (probed in the background, cached per host), or the panel is open.
bool sitemap_button_shown(App& app);
// <link rel=…> facts of the focused web page (cached; empty for local
// pages, native views and feeds themselves).
const PageInfoCache& focused_page_info(App& app);
// rel=canonical of the focused page when it names that very page (only
// tracking parameters, www. / http differ: safe_canonical) and the view
// shows the whole page; else "". Bookmarks store and match it.
std::string focused_canonical(App& app);
// Feeds of the focused web page (empty without XANATOO_FEEDS).
const std::vector<xanatoo::FeedLink>& page_feeds(App& app);
// Opens a feed from page_feeds (as `feed:` when its URL does not say so).
void open_page_feed(App& app, const std::string& url);
// Records a shown web page (title for the panel, coverage counts).
void note_page_seen(App& app, const std::string& url,
                    const xanatoo::Document& doc);
// Previous / next page of the focused page's site map (pre-order);
// starts reading the site map when it is not there yet.
void sitemap_step(App& app, int dir);

// ---- floating windows (app_windows.cpp) ----
// z-order, back→front: docked columns, floating (outline, backlinks, help,
// document windows), maximized.
bool dock_hidden(const App& app, Dock side);
void set_dock_hidden(App& app, Dock side, bool hidden);
std::vector<FloatWindow*> float_stack(App& app, bool include_hidden = false);
// Bring `w` to the front of its layer (title click) / send it to the back
// (title middle-click). Document windows also move within soft_wins.
void raise_float(App& app, FloatWindow& w);
void lower_float(App& app, FloatWindow& w);
FloatWindow* float_at(App& app, int x, int y);  // topmost window under (x,y)
// nullopt = no native frame; Chrome = a native-only panel, which covers any
// embedded ImGui form belonging to a different window.
std::optional<UiPanelLayer> float_layer_at(App& app, int x, int y);
// The UI layer that goes with a window: the panel it embeds, or the find bar
// of the document it shows.
UiPanelLayer float_ui_layer(App& app, const FloatWindow& w);
// Same edge hit-test as float_press, including the resize slack outside a frame.
// bit 1=right, 2=bottom, 4=left, 8=top. Zero if nothing resizable is hit.
int float_resize_at(App& app, int x, int y);
// A docked panel's outer resize edge (the gutter beside it) is at (x, y).
bool dock_edge_at(App& app, int x, int y);
// Separator between two adjacent panels sharing a dock, not the dock's
// outer resize edge. Returned axis is the direction the divider moves.
enum class DockSplitAxis : uint8_t { None, Horizontal, Vertical };
DockSplitAxis dock_split_at(App& app, int x, int y);
bool dock_split_press(App& app, int x, int y);
bool dock_split_motion(App& app, int x, int y);
void dock_split_release(App& app);
std::tuple<int, int, int, int> float_client_rect(App& app,
                                                 const FloatWindow& w);
// A panel docked against the right edge of the window: its scrollbar runs
// out to that edge (Fitts' law) instead of stopping at the frame.
bool float_flush_right(const App& app, const FloatWindow& w);
// The native client scrollbar strip (draw and hit-test share it).
SDL_Rect client_scrollbar_rect(App& app, const FloatWindow& w);
// Client rect handed to an embedded ImGui panel body (drawn 3 px inside it);
// flush-right panels reach past the window edge so ImGui's scrollbar does too.
std::tuple<int, int, int, int> imgui_panel_rect(App& app, const FloatWindow& w);
float float_max_scroll(App& app, const FloatWindow& w);
bool float_press(App& app, int x, int y);   // true = click consumed
bool float_motion(App& app, int x, int y);  // true = drag/resize/scroll active
void float_release(App& app, int x, int y);
// Touch: while a free window is dragged by its title, a second finger turns
// the drag into a two-finger span of app.touch's fingers (false = no such
// drag). float_span_end with `keep_drag` resumes the title drag under the
// first finger (the second lifted); otherwise the window snaps into place.
bool float_span_begin(App& app);
void float_span_motion(App& app);
void float_span_end(App& app, bool keep_drag);
bool float_wheel(App& app, int x, int y, int wheel_y);
bool float_page_scroll(App& app, int x, int y, bool up);
// Closes the window that has key focus (float_active), if any.
bool close_focused_float(App& app);

// ---- keymap ----
// Ctrl+<key> chord → command (nullopt = not a table binding).
std::optional<xanatoo::UiCommand> keymap_lookup(SDL_Keycode sym, bool ctrl,
                                                bool shift);

// ---- chrome (theme-based canvas widgets) ----
StatusLine compute_status(App& app);
// Fill a document client area with the style sheet's paper colour.
void fill_doc_paper(App& app, const SDL_Rect& rect);
// `text` cut to `max_w` px in `font`, with a trailing "…" when shortened.
std::string fit_text(xanatoo::Font& font, const std::string& text, int max_w);
// Title-bar control hit zones of a floating frame (paint + hit-test): the
// close box sits at the title's left edge, maximize / restore at its right,
// shade (roll up, docked windows only) just left of maximize.
SDL_Rect title_close_zone(const FloatWindow& w);
SDL_Rect title_max_zone(const FloatWindow& w);
SDL_Rect title_shade_zone(const FloatWindow& w);
// Outline title-bar "Focus" toggle, left of the shade box (paint + hit-test).
SDL_Rect outline_focus_rect(App& app, const FloatWindow& w);
// Push button `label` in a window title, left of the shade box (flush with
// the strip while docked).
SDL_Rect title_button_rect(App& app, const FloatWindow& w, const char* label);
void draw_title_button(App& app, const FloatWindow& w, const SDL_Rect& r,
                       const char* label, bool latched);
// Chrome font for the title of `w` (smaller while docked).
xanatoo::Font& title_font(App& app, const FloatWindow& w, bool bold);
// Frame of `win` (title, controls, grip and the client well).
// `title_reserve`: px kept free left of the shade box (title buttons).
// Returns false when the window is shaded: draw no client content then.
bool draw_floating_frame(App& app, const FloatWindow& win,
                         const std::string& title, bool active,
                         int title_reserve = 0);
void draw_client_scrollbar(App& app, const FloatWindow& w, float max_scroll);

// ---- database-native document history (app_revisions.cpp) ----
std::string_view revision_panel_key_for(const DocBuffer& buffer);
void draw_revision_panel(App& app);
void click_revision_panel(App& app, int mx, int my);
bool right_click_revision_panel(App& app, int mx, int my);
void revision_compare_action(App& app, std::string_view action);
bool revision_panel_key(App& app, SDL_Keycode key);
bool select_buffer_revision(App& app, BufferId id, const std::string& revision);
float revision_panel_max_scroll(App& app, int client_h);

// ---- frame stages ----
void sync_window_size(App& app);
void relayout_if_needed(App& app);
void run_chrome(App& app);
void poll_gamepad_axes(App& app);
void draw_pane(App& app, int i);
void draw_panes(App& app);
// Drop cached pane rasters (render-target reset / shutdown).
void release_pane_rasters(App& app);
void release_raster(PaneRaster& r);
void draw_split_chrome(App& app);
void draw_status_bar(App& app);
void draw_outline_panel(App& app);
void draw_backlinks_panel(App& app);
// History panel (app_history.cpp).
void draw_history_panel(App& app);
void click_history_panel(App& app, FloatWindow& w, int my);
float history_panel_max_scroll(App& app, const FloatWindow& w);
// Keys while the panel has keyboard focus; true = consumed.
bool history_panel_key(App& app, SDL_Keycode k, bool ctrl, bool shift);
// The visit log changed (a visit was recorded, another process wrote).
void history_changed(App& app);
// Tooltip text for the hovered row ("" = none).
std::string history_tooltip(App& app);
// Right-click in the panel: menu of the row under the pointer (true =
// the click was in the list).
bool right_click_history_panel(App& app, int mx, int my);
// Title (or address) of the row of the open menu.
std::string history_menu_label(App& app);
// UiCommand::HistoryAction on HistoryPanel::menu_id.
void history_action(App& app, const std::string& action);
void draw_help_window(App& app);
// Help panel (app_hoverhelp.cpp): open / close, follow the pointer (each
// frame after the chrome), draw. `hover_help_key_at` is the canvas part:
// the "@" key of the native element at a window point, from the same
// hit-tests as its clicks and cursor ("" over nothing explained).
void toggle_hover_help(App& app);
void update_hover_help(App& app);
void draw_hover_help_window(App& app);
std::string hover_help_key_at(App& app, int x, int y);
// Help keys of the gallery / paged-reading chrome at a window point in view
// `pi` (nullptr: none), from their own hit-tests.
const char* gallery_help_key(App& app, int pi, int x, int y);
const char* page_help_key(App& app, int pi, int x, int y);
// The floating window whose resize edge is at (x, y), as float_resize_at.
const FloatWindow* float_resize_window_at(App& app, int x, int y);
void draw_floats(App& app);  // all floating windows, back→front
void draw_dock_preview(App& app);
void draw_hover_tooltip(App& app);
void present_frame(App& app);
void handle_event(App& app, SDL_Event& ev);
void run_frame(App& app);
// Apply a menu/toolbar/shortcut command (shared by chrome and keyboard).
void execute_command(App& app, xanatoo::UiCommand cmd,
                     const std::string& arg = {});
bool app_init(App& app);
void app_shutdown(App& app);

}  // namespace xanatoo::app
