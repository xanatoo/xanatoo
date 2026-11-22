// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/layout.hpp"
#include "xanatoo/fold.hpp"
#include "xanatoo/autolink.hpp"
#include "xanatoo/math_font.hpp"
#include "xanatoo/images.hpp"
#include "xanatoo/parse.hpp"
#include "xanatoo/table_grid.hpp"
#include "xanatoo/util.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <cstring>
#include <limits>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>

namespace xanatoo {

namespace {

// True if `id` is `root` or nested under `root`.
bool is_section_kind(NodeKind kind) {
  return kind == NodeKind::Section || kind == NodeKind::Article;
}

bool node_is_descendant(const Document& doc, NodeId root, NodeId id) {
  if (root == INVALID_NODE || id == INVALID_NODE) return false;
  for (NodeId cur = id; cur != INVALID_NODE && cur < doc.next_id;
       cur = doc.get(cur).parent) {
    if (cur == root) return true;
  }
  return false;
}


// Cached external documents for :::include path.md[#anchor].
// Keyed by resolved absolute path. Cleared via clear_external_doc_cache().
std::map<std::string, Document>& external_doc_cache_map() {
  static std::map<std::string, Document> cache;
  return cache;
}

void clear_external_doc_cache_impl() {
  external_doc_cache_map().clear();
}

const Document* load_external_doc(const std::string& path) {
  if (path.empty()) return nullptr;
  std::string key = path;
  try {
    key = std::filesystem::absolute(path).lexically_normal().string();
  } catch (...) {
  }
  auto& cache = external_doc_cache_map();
  auto it = cache.find(key);
  if (it != cache.end()) return &it->second;

  std::ifstream in(path);
  if (!in) return nullptr;
  std::string source((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
  Document doc = parse_document(source, path);
  auto [ins, _] = cache.emplace(key, std::move(doc));
  return &ins->second;
}

// Split src into (path, #anchor). path empty ⇒ same-document.
// Forms: "#H", "file.md#H", "file.md", "dir/x.md#H"
void split_include_src(const std::string& src, std::string& path_out,
                       std::string& href_out) {
  path_out.clear();
  href_out.clear();
  if (src.empty()) return;
  if (src[0] == '#') {
    href_out = src;
    return;
  }
  size_t hash = src.find('#');
  if (hash == std::string::npos) {
    path_out = src;
    return;
  }
  path_out = src.substr(0, hash);
  href_out = src.substr(hash);  // includes '#'
}

}  // namespace

void clear_external_doc_cache() {
  clear_external_doc_cache_impl();
}

static const DocStyle& default_style() {
  static const DocStyle style = builtin_style();
  return style;
}

LayoutEngine::LayoutEngine(FontSet& fonts, ImageCache* images)
    : fonts_(fonts), images_(images), style_(&default_style()) {}

void LayoutEngine::set_style(const DocStyle* style) {
  style_ = style ? style : &default_style();
}

// Face for an inline span inside a block set in `base` (a bold / italic
// block face combines with the span's own emphasis and its marks).
static FontRole style_role(FontRole base, InlineKind kind, uint8_t marks) {
  switch (base) {  // small block faces: the body logic, then made small
    case FontRole::SmallCode: return FontRole::SmallCode;
    case FontRole::Small:
      return small_role(style_role(FontRole::Body, kind, marks));
    case FontRole::SmallBold:
      return small_role(style_role(FontRole::BodyBold, kind, marks));
    case FontRole::SmallItalic:
      return small_role(style_role(FontRole::BodyItalic, kind, marks));
    case FontRole::SmallBoldItalic:
      return small_role(style_role(FontRole::BodyBoldItalic, kind, marks));
    default: break;
  }
  if (kind == InlineKind::Code || base == FontRole::Code) return FontRole::Code;
  if (base == FontRole::Heading) return FontRole::Heading;
  marks |= kind_mark(kind);
  const bool strong = (marks & kMarkStrong) ||
                      base == FontRole::BodyBold ||
                      base == FontRole::BodyBoldItalic;
  const bool emph = (marks & kMarkEmphasis) ||
                    base == FontRole::BodyItalic ||
                    base == FontRole::BodyBoldItalic;
  if (strong && emph) return FontRole::BodyBoldItalic;
  if (strong) return FontRole::BodyBold;
  if (emph) return FontRole::BodyItalic;
  return FontRole::Body;
}

static void decode_utf8(const std::string& text, std::vector<uint32_t>& out);

// Unicode superscript for an ASCII digit, else 0.
static uint32_t superscript_digit(uint32_t cp) {
  switch (cp) {
    case '1': return 0x00B9;
    case '2': return 0x00B2;
    case '3': return 0x00B3;
    default:
      return (cp >= '0' && cp <= '9') ? 0x2070 + (cp - '0') : 0;
  }
}

// Message in-reply-to label → node id (rebuilt each layout).
thread_local std::unordered_map<std::string, NodeId> g_msg_label_map;
// What a list's items share (rebuilt each layout), so their texts line up:
// the widest ordered marker number (a list past "99." widens its slot), and
// whether fold triangles need room beside a number or task box.
struct ListSlot {
  int max_number = 0;
  bool fold_column = false;
};
thread_local std::unordered_map<NodeId, ListSlot> g_list_slots;

static ListSlot list_slot(const Document& doc, const Node& item) {
  if (item.parent == INVALID_NODE) return {item.level, false};
  auto it = g_list_slots.find(item.parent);
  if (it == g_list_slots.end()) {
    ListSlot s;
    bool marked = false, foldable = false;
    for (NodeId sib : doc.get(item.parent).children) {
      const Node& sn = doc.get(sib);
      if (sn.kind != NodeKind::ListItem) continue;
      s.max_number = std::max(s.max_number, sn.level);
      marked = marked || sn.level > 0 || !sn.label.empty();
      foldable = foldable || list_item_foldable(doc, sib);
    }
    s.fold_column = marked && foldable;
    it = g_list_slots.emplace(item.parent, s).first;
  }
  return it->second;
}
// Parent → widest footnote key ("97.") among its Footnote children, in
// pixels of the footnote font (rebuilt each layout), so long note runs
// hang their bodies past the widest key.
thread_local std::unordered_map<NodeId, float> g_footnote_key_w;

// The key a Footnote shows before its body: "12." for numbers, else as is.
static std::string footnote_marker(const std::string& label) {
  if (!label.empty() &&
      label.find_first_not_of("0123456789") == std::string::npos)
    return label + ".";
  return label;
}

bool LayoutEngine::try_skip_block(NodeId id, float x, float& y, float max_width,
                                  LayoutResult& out) {
  if (!viewport_.active() || !height_hints_ || id == INVALID_NODE)
    return false;
  auto it = height_hints_->find(id);
  if (it == height_hints_->end()) return false;
  const float h = it->second;
  if (h < 1.f) return false;
  const float y0 = y;
  const float y1 = y + h;
  const float band0 = viewport_.top - viewport_.overscan;
  const float band1 = viewport_.bottom + viewport_.overscan;
  if (y1 < band0 || y0 > band1) {
    out.boxes.push_back({id, x, y0, max_width, h});
    y = y1;
    skipped_any_ = true;
    return true;
  }
  return false;
}

static bool next_utf8(const std::string& text, size_t& i, uint32_t& cp);

std::vector<TextImageUrl> text_image_urls(const Node& node) {
  std::vector<TextImageUrl> out;
  auto add = [&](std::string url, size_t span, bool link) {
    if (!is_image_url(url)) return;
    for (const TextImageUrl& t : out)
      if (t.url == url) return;
    for (const InlineSpan& s : node.spans)
      if (s.kind == InlineKind::Image && (s.src == url || s.href == url))
        return;
    out.push_back({std::move(url), span, link});
  };
  std::vector<uint32_t> cps;
  for (size_t si = 0; si < node.spans.size(); ++si) {
    const InlineSpan& s = node.spans[si];
    if (s.kind == InlineKind::Link) {
      // A link written as its address ("https://…/x.jpg", or a shortened
      // "https://…/x…"); a named link stays a link.
      std::string_view text = s.text;
      trim_ascii(text);
      if (starts_with_ci(text, "http"))
        add(s.href, si, true);
      continue;
    }
    if (s.kind != InlineKind::Text && s.kind != InlineKind::Emphasis &&
        s.kind != InlineKind::Strong && s.kind != InlineKind::Strike)
      continue;
    if (s.text.find("http") == std::string::npos &&
        s.text.find("HTTP") == std::string::npos)
      continue;
    cps.clear();
    size_t byte = 0;
    for (uint32_t cp = 0; next_utf8(s.text, byte, cp);) cps.push_back(cp);
    for (const auto& [b, e] : find_urls(cps)) {
      std::string url;
      for (size_t i = b; i < e; ++i) autolink_append_utf8(url, cps[i]);
      add(std::move(url), si, false);
    }
  }
  return out;
}

// Everything `in` laid out (a column, a table cell) added to `out`.
static void append_result(LayoutResult& out, const LayoutResult& in) {
  auto add = [](auto& dst, const auto& src) {
    dst.insert(dst.end(), src.begin(), src.end());
  };
  // Auto-link ids of `in` index its own auto_links: shift them.
  const auto link_base = static_cast<uint32_t>(out.auto_links.size());
  add(out.auto_links, in.auto_links);
  const size_t first_glyph = out.glyphs.size();
  add(out.glyphs, in.glyphs);
  for (size_t i = first_glyph; i < out.glyphs.size(); ++i)
    if (out.glyphs[i].auto_link != 0) out.glyphs[i].auto_link += link_base;
  add(out.boxes, in.boxes);
  const size_t first_image = out.images.size();
  add(out.images, in.images);
  for (size_t i = first_image; i < out.images.size(); ++i)
    if (out.images[i].auto_link != 0) out.images[i].auto_link += link_base;
  add(out.maths, in.maths);
  add(out.fold_marks, in.fold_marks);
  add(out.quote_bars, in.quote_bars);
  add(out.note_bars, in.note_bars);
  add(out.message_bars, in.message_bars);
  add(out.message_cards, in.message_cards);
  add(out.code_blocks, in.code_blocks);
  add(out.table_lines, in.table_lines);
  add(out.table_header_fills, in.table_header_fills);
  add(out.hr_lines, in.hr_lines);
  add(out.role_fills, in.role_fills);
  add(out.role_rules, in.role_rules);
  add(out.role_frames, in.role_frames);
  add(out.column_gaps, in.column_gaps);
  add(out.page_gaps, in.page_gaps);
  add(out.skipped_lines, in.skipped_lines);
}

// Blocks with a box of their own (fills, grids, pictures) that cannot run
// past a floating aside: they start below it.
static bool clears_floats(NodeKind k) {
  switch (k) {
    case NodeKind::Table:
    case NodeKind::CodeBlock:
    case NodeKind::Image:
    case NodeKind::Figure:
    case NodeKind::Columns:
    case NodeKind::Collection:
    case NodeKind::Thread:
    case NodeKind::Transclude:
    case NodeKind::HorizontalRule:
      return true;
    default:
      return false;
  }
}

void LayoutEngine::float_band(float top, float bottom, float& x,
                              float& w) const {
  float l = x, r = x + w;
  for (const FloatBox& f : floats_) {
    if (f.bottom <= top || f.top >= bottom) continue;
    if (f.right <= l || f.left >= r) continue;  // not in this column
    if (f.side == FloatSide::Right) r = std::min(r, f.left);
    else l = std::max(l, f.right);
  }
  x = l;
  w = std::max(0.f, r - l);
}

float LayoutEngine::clear_floats(float x, float w, float y) const {
  float out = y;
  for (const FloatBox& f : floats_) {
    if (f.bottom <= y || f.top > y + 0.5f) continue;
    if (f.right <= x || f.left >= x + w) continue;
    out = std::max(out, f.bottom);
  }
  return out;
}

void LayoutEngine::contain_floats(size_t first, float& y) {
  if (first >= floats_.size()) return;
  for (size_t i = first; i < floats_.size(); ++i)
    y = std::max(y, floats_[i].bottom);
  floats_.resize(first);
}

void LayoutEngine::layout_children_sequential(
    const Document& doc, const std::vector<NodeId>& children, float x, float& y,
    float max_width, LayoutResult& out) {
  for (NodeId child : children) {
    if (child == INVALID_NODE) continue;
    const NodeKind kind = doc.get(child).kind;
    const bool floating = (kind == NodeKind::Image || kind == NodeKind::Figure ||
                           kind == NodeKind::Table) &&
                          !doc.get(child).float_side.empty();
    // An Article is self-contained: like <br clear=all>, no float of a
    // previous sibling runs into it. Sections are not cleared on purpose
    // (an infobox may float past a heading, see unit_tests "infobox").
    const bool sectioning = kind == NodeKind::Article;
    if (!floats_.empty() && (clears_floats(kind) || sectioning) && !floating)
      y = clear_floats(x, max_width, y);
    // Side boxes may not advance y: their box height is no hint.
    // A figure beside others in a row is laid out with the row's first
    // figure; only that one stands for the row's height.
    const bool row_follower =
        (kind == NodeKind::Figure || kind == NodeKind::Image) && !floating &&
        figure_in_row(doc, child) &&
        [&] {
          auto row = figure_row(doc, child, max_width);
          return !row.empty() && row.front().node != child;
        }();
    if (kind != NodeKind::Aside && !floating && !row_follower &&
        try_skip_block(child, x, y, max_width, out))
      continue;
    layout_node(doc, child, x, y, max_width, out);
  }
}

std::vector<NodeId> chrome_regions(const Document& doc) {
  std::vector<NodeId> out;
  if (doc.root == INVALID_NODE) return out;
  std::vector<NodeId> stack{doc.root};
  while (!stack.empty()) {
    NodeId id = stack.back();
    stack.pop_back();
    if (id == INVALID_NODE || id >= doc.next_id) continue;
    const Node& n = doc.get(id);
    if (n.id != id) continue;
    if (n.kind == NodeKind::Chrome) {
      out.push_back(id);
      continue;
    }
    for (auto it = n.children.rbegin(); it != n.children.rend(); ++it)
      stack.push_back(*it);
  }
  return out;
}

bool margin_chrome(const Document& doc, const Node& chrome) {
  if (chrome.kind != NodeKind::Chrome) return false;
  if (chrome.summary != "navigation" && chrome.summary != "sidebar" &&
      chrome.summary != "related")
    return false;
  std::vector<NodeId> stack(chrome.children.begin(), chrome.children.end());
  while (!stack.empty()) {
    const NodeId id = stack.back();
    stack.pop_back();
    if (id == INVALID_NODE || id >= doc.next_id) continue;
    const Node& n = doc.get(id);
    if (n.id != id) continue;
    switch (n.kind) {
      case NodeKind::Table:
      case NodeKind::CodeBlock:
      case NodeKind::Columns:
      case NodeKind::Collection:
        return false;  // needs the column's width
      default:
        break;
    }
    stack.insert(stack.end(), n.children.begin(), n.children.end());
  }
  return true;
}

std::string chrome_hidden_text(const Document& doc,
                               const std::vector<NodeId>& regions) {
  if (regions.empty()) return {};
  std::vector<std::string> kinds;
  for (NodeId id : regions) {
    const std::string& k = doc.get(id).summary;
    if (!k.empty() && std::find(kinds.begin(), kinds.end(), k) == kinds.end())
      kinds.push_back(k);
  }
  std::string t = regions.size() == 1
                      ? std::string("1 part of the page hidden")
                      : std::to_string(regions.size()) + " parts of the page hidden";
  if (!kinds.empty()) {
    t += " (";
    for (size_t i = 0; i < kinds.size() && i < 4; ++i) {
      if (i) t += ", ";
      t += kinds[i];
    }
    if (kinds.size() > 4) t += ", …";
    t += ")";
  }
  return t;
}

CollectionView default_collection_view(const Node& collection) {
  CollectionView v;
  const std::string hint = ascii_lower(collection.summary);
  if (hint == "list") v.layout = CollectionLayout::List;
  else if (hint == "full") v.layout = CollectionLayout::Full;
  return v;
}

std::vector<CollectionItem> collection_items(const Document& doc,
                                             NodeId collection,
                                             CollectionSort sort) {
  std::vector<CollectionItem> items;
  const Node& coll = doc.get(collection);
  if (coll.id != collection) return items;
  auto plain = [](const Node& n) {
    std::string t;
    for (const auto& sp : n.spans) t += sp.text;
    return t;
  };
  for (NodeId c : coll.children) {
    const Node& ch = doc.get(c);
    if (ch.id != c) continue;
    CollectionItem it;
    it.item = c;
    if (ch.kind == NodeKind::Image) {
      it.image = c;
      it.src = ch.src;
      it.caption = ch.alt;
    } else if (ch.kind == NodeKind::Figure) {
      for (NodeId g : ch.children) {
        const Node& gn = doc.get(g);
        if (gn.kind == NodeKind::Image && it.image == INVALID_NODE) {
          it.image = g;
          it.src = gn.src;
          if (it.caption.empty()) it.caption = gn.alt;
        } else if (gn.kind == NodeKind::Caption) {
          it.caption = plain(gn);
        }
      }
    } else {
      continue;
    }
    items.push_back(std::move(it));
  }
  if (sort != CollectionSort::Document) {
    auto name = [](const CollectionItem& it) {
      return ascii_lower(std::filesystem::path(it.src).filename().string());
    };
    std::stable_sort(items.begin(), items.end(),
                     [&](const CollectionItem& a, const CollectionItem& b) {
                       return sort == CollectionSort::Name ? name(a) < name(b)
                                                           : name(b) < name(a);
                     });
  }
  return items;
}

// Cards for a Collection: a grid of thumbnails with captions, a list
// (small thumbnail + caption), or the plain children (Full). Other
// children (intro paragraphs, …) come first at full width.
void LayoutEngine::layout_remote_image(const Document& doc, const Node& node,
                                       float x, float& y, float max_width,
                                       LayoutResult& out, bool center) {
  const float dp = options_.px_scale;
  const float max_h = 480.f * dp;
  float w = std::min(max_width, 320.f * dp);
  float h = 180.f * dp;
  if (images_) {
    const Image& img = images_->thumb(node.src, image_base_, kRemoteThumbEdge);
    if (!img.ok && !img.pending) {
      out.boxes.push_back({node.id, x, y, max_width, 0.f});
      return;
    }
    if (img.ok && img.width > 0 && img.height > 0) {
      // Thumbnail size at most: never blown up past what was downloaded.
      const float sc = std::min({dp, max_width / img.width, max_h / img.height});
      w = img.width * sc;
      h = img.height * sc;
    }
  }
  const float start_y = y;
  // Centred in its column (in a figure: flush with the caption); the alt
  // text is not laid out below the image (the loading placeholder shows
  // it instead).
  (void)doc;
  const float ix = center ? x + std::max(0.f, (max_width - w) * 0.5f) : x;
  PositionedImage pi{node.id, ix, y, w, h, node.src, image_base_};
  pi.thumb = kRemoteThumbEdge;
  out.images.push_back(std::move(pi));
  y += h + 14.f * dp;
  out.boxes.push_back({node.id, x, start_y, max_width, y - start_y});
}

bool LayoutEngine::figure_image_size(const Document& doc, const Node& fig,
                                     float limit, float& w, float& h) {
  const float dp = options_.px_scale;
  w = h = 0.f;
  std::vector<NodeId> cands = fig.children;
  if (fig.kind == NodeKind::Image) cands.assign(1, fig.id);
  for (NodeId child : cands) {
    const Node& image = doc.get(child);
    if (image.kind != NodeKind::Image || image.src.empty()) continue;
    if (!images_) return true;
    if (is_remote_image(image.src)) {
      // Mirrors layout_remote_image.
      const Image& img = images_->thumb(image.src, image_base_, kRemoteThumbEdge);
      if (img.ok && img.width > 0 && img.height > 0) {
        const float sc = std::min({dp, limit / img.width, 480.f * dp / img.height});
        w = img.width * sc;
        h = img.height * sc;
      } else if (img.pending) {
        w = std::min(limit, 320.f * dp);
        h = 180.f * dp;
      }
      return true;
    }
    // Mirrors the local Image case: never scaled up past its own size.
    const Image& img = images_->get(image.src, image_base_);
    if (!img.ok || img.width <= 0 || img.height <= 0) return true;
    float scale = std::min({1.f, limit / img.width, 480.f / img.height});
    scale *= image.display_scale > 0.05f ? image.display_scale : 1.f;
    scale = std::min(scale, limit / img.width);
    w = img.width * scale;
    h = img.height * scale;
    return true;
  }
  return false;
}

bool LayoutEngine::figure_in_row(const Document& doc, NodeId id) {
  const Node& n = doc.get(id);
  if (n.float_side == "left" || n.float_side == "right") return false;
  if (n.kind == NodeKind::Image) return !n.src.empty();
  if (n.kind != NodeKind::Figure) return false;
  for (NodeId c : n.children)
    if (doc.get(c).kind == NodeKind::Image && !doc.get(c).src.empty()) return true;
  return false;
}

std::vector<LayoutEngine::FigureSlot> LayoutEngine::figure_row(
    const Document& doc, NodeId id, float max_width) {
  const float em = static_cast<float>(fonts_.regular.pixel_size());
  const float gap = 1.5f * em;
  // One figure alone gets at most ~60% of the column; its caption keeps a
  // readable measure even under a small image.
  const float limit = std::min(max_width, std::max(max_width * 0.6f, 16.f * em));
  const float min_col = std::min(limit, 14.f * em);
  struct Natural { float w = 0.f, h = 0.f; };
  auto natural = [&](NodeId f) {
    Natural n;
    figure_image_size(doc, doc.get(f), limit, n.w, n.h);
    return n;
  };
  // Column of a figure on its own: its image, a caption measure at least
  // (just that when the image is missing or failed).
  auto column = [&](NodeId f, const Natural& n) {
    if (doc.get(f).kind == NodeKind::Image)  // no caption to keep readable
      return n.w > 0.f ? std::min(n.w, limit) : min_col;
    return std::clamp(n.w, min_col, limit);
  };
  // The run of consecutive row figures around `id`.
  std::vector<NodeId> run{id};
  const NodeId parent = doc.get(id).parent;
  if (parent != INVALID_NODE) {
    const std::vector<NodeId>& sib = doc.get(parent).children;
    auto it = std::find(sib.begin(), sib.end(), id);
    if (it != sib.end()) {
      auto b = it;
      while (b != sib.begin() && figure_in_row(doc, *(b - 1))) --b;
      auto e = it + 1;
      while (e != sib.end() && figure_in_row(doc, *e)) ++e;
      run.assign(b, e);
    }
  }
  // Greedy packing into rows of the full width; a figure may shrink to
  // kMinShare of its own width to share a row. A shared row gives every
  // image the same height (never above any image's own), so the images
  // line up; the columns are the image widths.
  constexpr float kMinShare = 0.75f;
  struct Item { NodeId node; Natural n; float col; };
  auto fit = [&](const std::vector<Item>& items) {
    std::vector<FigureSlot> row;
    if (items.size() == 1) {
      row.push_back({items[0].node, items[0].col});
      return row;
    }
    const float room = max_width - gap * static_cast<float>(items.size() - 1);
    bool sized = true;
    float aspect_sum = 0.f, h = 0.f;
    for (const Item& it : items) {
      if (it.n.w <= 0.f || it.n.h <= 0.f) sized = false;
      else {
        aspect_sum += it.n.w / it.n.h;
        h = h <= 0.f ? it.n.h : std::min(h, it.n.h);
      }
    }
    if (sized && aspect_sum > 0.f) {
      h = std::min(h, room / aspect_sum);
      for (const Item& it : items)
        row.push_back({it.node, it.n.w / it.n.h * h});
      return row;
    }
    float sum = 0.f;
    for (const Item& it : items) sum += it.col;
    const float k = sum > room ? room / sum : 1.f;
    for (const Item& it : items) row.push_back({it.node, it.col * k});
    return row;
  };
  std::vector<Item> items;
  float sum = 0.f;
  for (NodeId f : run) {
    const Natural n = natural(f);
    const float w = column(f, n);
    if (!items.empty() &&
        (sum + w) * kMinShare + gap * static_cast<float>(items.size()) > max_width) {
      for (const Item& it : items)
        if (it.node == id) return fit(items);
      items.clear();
      sum = 0.f;
    }
    sum += w;
    items.push_back({f, n, w});
  }
  return fit(items);
}

bool LayoutEngine::figure_centered() const {
  return style_ && (*style_)[BlockRole::Figure].align == TextAlign::Center;
}

bool LayoutEngine::layout_figure_row(const Document& doc, NodeId id, float x,
                                     float& y, float max_width,
                                     LayoutResult& out) {
  const std::vector<FigureSlot> row = figure_row(doc, id, max_width);
  // A plain image alone keeps the usual block image layout.
  if (row.size() == 1 && doc.get(id).kind == NodeKind::Image) return false;
  // Laid out with the row's first member.
  if (row.empty() || row.front().node != id) return true;
  const float em = static_cast<float>(fonts_.regular.pixel_size());
  const float dp = options_.px_scale;
  const BlockStyle& fs = (*style_)[BlockRole::Figure];
  const float gap = 1.5f * em;
  float total = gap * static_cast<float>(row.size() - 1);
  for (const FigureSlot& f : row) total += f.width;
  y += fs.space_before.px(em, dp);
  const float top = y;
  float bottom = top;
  float cx = x + std::max(0.f, (max_width - total) * 0.5f);
  std::vector<size_t> own_box;
  in_image_float_ = true;
  in_figure_column_ = true;
  for (const FigureSlot& f : row) {
    float fy = top;
    if (doc.get(f.node).kind == NodeKind::Image) {
      layout_node(doc, f.node, cx, fy, f.width, out);
    } else {
      for (NodeId c : doc.get(f.node).children)
        layout_node(doc, c, cx, fy, f.width, out);
    }
    bottom = std::max(bottom, fy);
    own_box.push_back(out.boxes.size());
    out.boxes.push_back({f.node, cx, top, f.width, 0.f});
    cx += f.width + gap;
  }
  in_image_float_ = false;
  in_figure_column_ = false;
  y = bottom + fs.space_after.px(em, dp);
  // Every member's box spans the row; height hints only use the first
  // one (the others are never skipped on their own).
  for (size_t b : own_box) out.boxes[b].height = y - top;
  return true;
}

void LayoutEngine::layout_collection(const Document& doc, NodeId id, float x,
                                     float& y, float max_width,
                                     LayoutResult& out) {
  const Node& node = doc.get(id);
  const DocStyle& st = *style_;
  const float em = static_cast<float>(fonts_.regular.pixel_size());
  const float dp = options_.px_scale;
  CollectionView view = default_collection_view(node);
  if (collection_views_) {
    auto it = collection_views_->find(id);
    if (it != collection_views_->end()) view = it->second;
  }
  y += st[BlockRole::Figure].space_before.px(em, dp);
  const float start_y = y;

  if (view.layout == CollectionLayout::Full) {
    for (NodeId c : node.children) layout_node(doc, c, x, y, max_width, out);
  } else {
    for (NodeId c : node.children) {
      const NodeKind k = doc.get(c).kind;
      if (k != NodeKind::Image && k != NodeKind::Figure)
        layout_node(doc, c, x, y, max_width, out);
    }
    const auto items = collection_items(doc, id, view.sort);
    const float gap = 12.f * dp;
    const bool list = view.layout == CollectionLayout::List;
    const float box = list ? std::max(48.f, view.thumb_px * 0.5f) * dp
                           : std::max(48.f, view.thumb_px) * dp;
    int cols = list ? 1 : std::max(1, static_cast<int>((max_width + gap) /
                                                       (box + gap)));
    cols = std::min<int>(cols, std::max<size_t>(1, items.size()));
    const float cell_w =
        list ? max_width : (max_width - gap * static_cast<float>(cols - 1)) /
                               static_cast<float>(cols);
    const float img_w = list ? box : cell_w;

    // Caption text for one card (Figure caption node, else image alt).
    auto caption = [&](const CollectionItem& it, float cx, float& cy,
                       float w) {
      const Node& item = doc.get(it.item);
      for (NodeId g : item.children) {
        const Node& gn = doc.get(g);
        if (gn.kind == NodeKind::Caption) {
          layout_role_text(doc, gn, BlockRole::Caption, cx, cy, w, out);
          return;
        }
      }
      if (it.image == INVALID_NODE || it.caption.empty()) return;
      Node fake = doc.get(it.image);
      fake.kind = NodeKind::Paragraph;
      fake.spans.clear();
      InlineSpan sp;
      sp.kind = InlineKind::Emphasis;
      sp.text = it.caption;
      fake.spans.push_back(std::move(sp));
      layout_role_text(doc, fake, BlockRole::Caption, cx, cy, w, out);
    };

    float row_y = y;
    float row_h = 0.f;
    for (size_t i = 0; i < items.size(); ++i) {
      const auto& it = items[i];
      const int col = static_cast<int>(i % static_cast<size_t>(cols));
      if (col == 0 && i > 0) {
        row_y += row_h + gap;
        row_h = 0.f;
      }
      const float cx = x + static_cast<float>(col) * (cell_w + gap);
      // Thumbnail fitted into img_w × box, centred.
      float tw = img_w, th = box;
      const int edge = static_cast<int>(std::max(img_w, box));
      if (images_ && !it.src.empty()) {
        const Image& img = images_->thumb(it.src, image_base_, edge);
        if (img.ok && img.width > 0 && img.height > 0) {
          const float sc = std::min(img_w / static_cast<float>(img.width),
                                    box / static_cast<float>(img.height));
          tw = static_cast<float>(img.width) * sc;
          th = static_cast<float>(img.height) * sc;
        }
      }
      const float ix = cx + (img_w - tw) * 0.5f;
      const float iy = row_y + (box - th) * 0.5f;
      if (it.image != INVALID_NODE) {
        PositionedImage pi;
        pi.node = it.image;
        pi.x = ix;
        pi.y = iy;
        pi.width = tw;
        pi.height = th;
        pi.src = it.src;
        pi.base = image_base_;
        pi.thumb = edge;
        pi.group = id;
        out.images.push_back(std::move(pi));
        out.boxes.push_back({it.image, ix, iy, tw, th});
      }
      float cy;
      float text_x;
      float text_w;
      if (list) {
        text_x = cx + box + gap;
        text_w = std::max(40.f, cell_w - box - gap);
        cy = row_y + 2.f;
      } else {
        text_x = cx;
        text_w = cell_w;
        cy = row_y + box + 4.f * dp;
      }
      const size_t first_glyph = out.glyphs.size();
      caption(it, text_x, cy, text_w);
      if (!list) {
        // Centre each caption line under the thumbnail.
        size_t a = first_glyph;
        while (a < out.glyphs.size()) {
          size_t b = a;
          float right = text_x;
          while (b < out.glyphs.size() && out.glyphs[b].y == out.glyphs[a].y) {
            const auto& g = out.glyphs[b];
            right = std::max(
                right,
                g.x + fonts_.pick(g.font).glyph(g.codepoint).advance_px());
            ++b;
          }
          const float shift = std::max(0.f, (text_w - (right - text_x)) * 0.5f);
          for (size_t k = a; k < b; ++k) out.glyphs[k].x += shift;
          a = b;
        }
      }
      const float h = std::max(box, cy - row_y);
      row_h = std::max(row_h, h);
      out.boxes.push_back({it.item, cx, row_y, cell_w, h});
    }
    if (!items.empty()) y = row_y + row_h;
  }
  y += st[BlockRole::Figure].space_after.px(em, dp);
  out.boxes.push_back({id, x, start_y, max_width, y - start_y});
}

LayoutResult LayoutEngine::layout(
    const Document& doc, float max_width, const std::string& image_base,
    const LayoutViewport& viewport,
    const std::unordered_map<NodeId, float>* height_hints,
    const std::unordered_set<NodeId>* folded_sections,
    NodeId focus_section) {
  static uint64_t next_generation = 0;
  LayoutResult result;
  result.generation = ++next_generation;
  active_style_index_ = 0;
  image_base_ = image_base;
  viewport_ = viewport;
  height_hints_ = height_hints;
  folded_sections_ = folded_sections;
  focus_section_ = focus_section;
  skipped_any_ = false;
  floats_.clear();
  if (viewport_.active()) {
    result.materialised_top = viewport_.top - viewport_.overscan;
    result.materialised_bottom = viewport_.bottom + viewport_.overscan;
  }
  if (doc.root == INVALID_NODE) return result;
  // Rough glyph budget: a few characters per node on average.
  // When virtualizing, most blocks are skipped so reserve less aggressively.
  const size_t glyph_budget =
      (viewport_.active() && height_hints_ && !height_hints_->empty())
          ? doc.nodes.size() * 2
          : doc.nodes.size() * 8;
  result.glyphs.reserve(glyph_budget);
  result.boxes.reserve(doc.nodes.size());
  g_msg_label_map.clear();
  g_list_slots.clear();
  g_footnote_key_w.clear();
  for (const auto& n : doc.nodes) {
    if (n.kind == NodeKind::Message && !n.label.empty())
      g_msg_label_map.emplace(n.label, n.id);
  }
  auto [mx, content_w] = text_column(max_width);
  float y = options_.margin_y;
  setup_margins(doc, mx, content_w, max_width);
  if (style_->has_margin()) {
    const float pad = 0.75f * static_cast<float>(fonts_.regular.pixel_size());
    result.page_x = mx - pad;
    result.page_w = content_w + 2.f * pad;
  }
  layout_node(doc, doc.root, mx, y, content_w, result);
  if (!result.page_gaps.empty()) {
    // A gap at the very top / bottom of the page is the margin itself.
    LayoutBox& first = result.page_gaps.front();
    if (first.y <= options_.margin_y + 0.5f) {
      first.height += first.y;
      first.y = 0.f;
    }
    LayoutBox& last = result.page_gaps.back();
    if (last.y + last.height >= y - 0.5f) last.height += 1.0e5f;
  }
  for (const FloatBox& f : floats_) y = std::max(y, f.bottom);
  floats_.clear();
  for (const MarginArea& m : margins_) y = std::max(y, m.bottom);
  result.content_height = y + options_.margin_y + 24.f;
  result.virtualized = skipped_any_;
  index_glyphs(result);
  height_hints_ = nullptr;
  folded_sections_ = nullptr;
  focus_section_ = INVALID_NODE;
  return result;
}

std::pair<float, float> LayoutEngine::text_column(float max_width) {
  float mx = std::max(options_.margin_x, options_.margin_pct * max_width);
  float content_w = std::max(40.f, max_width - 2.f * mx);
  if (options_.measure_chars > 0) {
    // Average lowercase advance of the body font: follows zoom and sheet.
    const float ch =
        fonts_.pick(FontRole::Body).measure("abcdefghijklmnopqrstuvwxyz") /
        26.f;
    const float cap = std::max(40.f, options_.measure_chars * ch);
    if (content_w > cap) {
      mx += (content_w - cap) * 0.5f;
      content_w = cap;
    }
  }
  return {mx, content_w};
}

// ---- Tables ----

// The measured cells and chosen column widths of one table (plan_table).
struct TablePlan {
  struct Cell {
    TablePlacement p;
    CellAlign align = CellAlign::Left;
    CellVAlign valign = CellVAlign::Top;
    float max_w = 0.f;     // content width unbroken (at most the column)
    float min_w = 0.f;     // longest word / picture
    float height = 0.f;    // content height at the planned width
    float baseline = 0.f;  // first baseline below the content top
    float dec_a = 0.f;     // Decimal: ink left of the decimal point …
    float dec_b = 0.f;     // … and from it to the right
  };
  TableGrid grid;
  std::vector<Cell> cells;
  std::vector<float> col_w;     // including the padding
  std::vector<float> dec_b;     // per column: widest part right of the point
  std::vector<bool> head;       // head rows
  std::vector<NodeId> weight_cells;  // a complete unspanned row (resizing)
  size_t weight_row = 0;
  float pad_x = 4.f, pad_y = 4.f;
  float width = 0.f;
};

namespace {

// How far right `r` put ink, from x0 (spaces do not count).
float content_extent(const LayoutResult& r, float x0) {
  float right = x0;
  for (const PositionedGlyph& g : r.glyphs)
    if (g.codepoint != ' ' && g.codepoint != '\t' && g.codepoint != '\n')
      right = std::max(right, g.x + g.advance);
  for (const PositionedImage& im : r.images) right = std::max(right, im.x + im.width);
  for (const PositionedMath& m : r.maths)
    if (m.box) right = std::max(right, m.x + m.box->width);
  for (const LayoutBox& b : r.table_lines) right = std::max(right, b.x + b.width);
  // Glyph positions are rounded; line breaking sums the exact advances.
  // Slack keeps a word that just fits from being broken at this width.
  return right > x0 ? std::ceil(right - x0) + 2.f : 0.f;
}

bool single_line(const LayoutResult& r) {
  if (r.glyphs.empty() || !r.images.empty()) return false;
  const float y0 = r.glyphs.front().y;
  for (const PositionedGlyph& g : r.glyphs)
    if (std::abs(g.y - y0) > 0.5f) return false;
  return true;
}

// The decimal point of a one-line cell: the first '.', else after the last
// digit, else the line's end. a = ink before it, b = from it on.
void decimal_parts(const LayoutResult& r, float x0, float& a, float& b) {
  float right = x0, dot = -1.f, digit = -1.f;
  for (const PositionedGlyph& g : r.glyphs) {
    if (g.codepoint == ' ') continue;
    right = std::max(right, g.x + g.advance);
    if (g.codepoint == '.' && dot < 0.f) dot = g.x;
    if (g.codepoint >= '0' && g.codepoint <= '9') digit = g.x + g.advance;
  }
  const float at = dot >= 0.f ? dot : digit >= 0.f ? digit : right;
  a = at - x0;
  b = right - at;
}

}  // namespace

LayoutResult LayoutEngine::measure_cell(const Document& doc, NodeId cell,
                                        float w, float& height) {
  LayoutResult r;
  const auto saved_floats = floats_;
  const TextAlign saved_align = line_align_;
  const bool saved_cell = in_cell_;
  floats_.clear();
  line_align_ = TextAlign::Start;
  in_cell_ = true;
  float bottom = 0.f;
  layout_node(doc, cell, 0.f, bottom, std::max(1.f, w), r);
  for (const FloatBox& f : floats_) bottom = std::max(bottom, f.bottom);
  // Text only: end at the last line's descent, not after the spacing that
  // paragraphs leave below themselves (the cell padding does that job).
  if (!r.glyphs.empty() && r.images.empty() && r.maths.empty() && floats_.empty()) {
    float ink = 0.f;
    for (const PositionedGlyph& g : r.glyphs)
      ink = std::max(ink, g.y - static_cast<float>(fonts_.pick(g.font).descender()));
    for (const LayoutBox& b : r.table_lines) ink = std::max(ink, b.y + b.height);
    if (ink > 0.f) bottom = std::min(bottom, std::ceil(ink) + 1.f);
  }
  floats_ = saved_floats;
  line_align_ = saved_align;
  in_cell_ = saved_cell;
  height = bottom;
  return r;
}

void LayoutEngine::plan_table(const Document& doc, NodeId table, float avail,
                              TablePlan& plan) {
  plan.grid = build_table_grid(doc, table);
  const TableGrid& grid = plan.grid;
  const size_t columns = grid.columns;
  plan.head = table_head_rows(doc, grid);
  const float em = static_cast<float>(fonts_.regular.pixel_size());
  plan.pad_x = std::max(4.f, std::round(0.4f * em));
  plan.pad_y = std::max(4.f, std::round(0.3f * em));
  if (columns == 0) return;

  // A complete row of unspanned cells supplies the column weights (dragged
  // column widths) and resize handles. A spanning heading is not one.
  for (size_t r = 0; r < grid.rows.size(); ++r) {
    std::vector<NodeId> candidate;
    for (NodeId id : grid.slots[r]) {
      if (id == INVALID_NODE || doc.get(id).parent != grid.rows[r] ||
          doc.get(id).col_span != 1 || doc.get(id).row_span != 1) {
        candidate.clear();
        break;
      }
      candidate.push_back(id);
    }
    if (candidate.size() == columns) {
      plan.weight_cells = std::move(candidate);
      plan.weight_row = r;
      break;
    }
  }

  const float pads = 2.f * plan.pad_x;
  const float inner = std::max(1.f, avail - pads);
  const LayoutViewport saved_viewport = viewport_;
  viewport_ = {};
  std::vector<float> dec_a(columns, 0.f);
  plan.dec_b.assign(columns, 0.f);
  for (const TablePlacement& p : grid.cells) {
    TablePlan::Cell c;
    c.p = p;
    c.align = table_cell_align(doc, table, p);
    c.valign = table_cell_valign(doc, table, p);
    plan.cells.push_back(c);
  }
  // A right-aligned column of numbers lines them up at the decimal point.
  std::vector<int> numeric(columns, 0);  // 1 numbers, -1 not, 0 empty
  auto body_cell = [&](const TablePlan::Cell& c) {
    return c.p.cols == 1 && doc.get(c.p.id).level == 0;
  };
  for (const TablePlan::Cell& c : plan.cells) {
    if (!body_cell(c)) continue;
    const std::string text = plain_text(doc, c.p.id);
    int& k = numeric[c.p.col];
    if (text.find_first_not_of(" \t") == std::string::npos) continue;
    if (c.align != CellAlign::Right) k = -1;
    else if (k == 0 && numeric_text(text)) k = 1;
    else if (!numeric_text(text)) k = -1;
  }
  for (TablePlan::Cell& c : plan.cells)
    if (body_cell(c) && numeric[c.p.col] == 1) c.align = CellAlign::Decimal;
  for (TablePlan::Cell& c : plan.cells) {
    const TablePlacement& p = c.p;
    LayoutResult r = measure_cell(doc, p.id, inner, c.height);
    c.max_w = content_extent(r, 0.f);
    c.baseline = r.glyphs.empty() ? 0.f : r.glyphs.front().y;
    if (c.align == CellAlign::Decimal) {
      if (single_line(r)) {
        decimal_parts(r, 0.f, c.dec_a, c.dec_b);
        dec_a[p.col] = std::max(dec_a[p.col], c.dec_a);
        plan.dec_b[p.col] = std::max(plan.dec_b[p.col], c.dec_b);
      } else {
        c.align = CellAlign::Right;  // prose in a number column
      }
    }
  }

  // Widest and narrowest content per column; spanning cells widen the
  // columns they cover evenly when those do not hold them already.
  auto per_column = [&](auto width_of, std::vector<float>& col) {
    col.assign(columns, pads + 0.75f * em);
    for (const TablePlan::Cell& c : plan.cells)
      if (c.p.cols == 1) col[c.p.col] = std::max(col[c.p.col], width_of(c) + pads);
    for (size_t k = 0; k < columns; ++k)
      if (dec_a[k] + plan.dec_b[k] > 0.f)
        col[k] = std::max(col[k], dec_a[k] + plan.dec_b[k] + pads);
    std::vector<const TablePlan::Cell*> spans;
    for (const TablePlan::Cell& c : plan.cells)
      if (c.p.cols > 1) spans.push_back(&c);
    std::stable_sort(spans.begin(), spans.end(),
                     [](auto* a, auto* b) { return a->p.cols < b->p.cols; });
    for (const TablePlan::Cell* c : spans) {
      float have = 0.f;
      for (size_t k = c->p.col; k < c->p.col + c->p.cols; ++k) have += col[k];
      const float need = width_of(*c) + pads;
      if (need > have)
        for (size_t k = c->p.col; k < c->p.col + c->p.cols; ++k)
          col[k] += (need - have) / static_cast<float>(c->p.cols);
    }
  };
  std::vector<float> col_max, col_min;
  per_column([](const TablePlan::Cell& c) { return c.max_w; }, col_max);
  const int pct = doc.get(table).width;
  const float target = pct > 0 ? avail * std::min(pct, 100) / 100.f : avail;
  float sum_max = 0.f;
  for (float w : col_max) sum_max += w;
  if (sum_max > target) {
    for (TablePlan::Cell& c : plan.cells) {
      float h = 0.f;
      c.min_w = content_extent(measure_cell(doc, c.p.id, 1.f, h), 0.f);
      if (c.align == CellAlign::Decimal) c.min_w = 0.f;  // numbers do not wrap
    }
    per_column([](const TablePlan::Cell& c) { return c.min_w; }, col_min);
  } else {
    col_min = col_max;
  }
  plan.col_w = distribute_table_widths(col_min, col_max, target, pct > 0);

  // Dragged column widths: weights share out the table's width.
  bool weighted = false;
  for (NodeId id : plan.weight_cells)
    weighted |= std::abs(doc.get(id).display_scale - 1.f) > 0.01f;
  if (weighted) {
    float total = 0.f, sum_w = 0.f;
    for (float w : plan.col_w) total += w;
    std::vector<float> weights(columns);
    for (size_t k = 0; k < columns; ++k) {
      weights[k] = std::clamp(doc.get(plan.weight_cells[k]).display_scale, 0.15f, 20.f);
      sum_w += weights[k];
    }
    for (size_t k = 0; k < columns; ++k) plan.col_w[k] = total * weights[k] / sum_w;
  }

  // Heights at the chosen widths (only cells that now wrap change).
  plan.width = 0.f;
  for (float w : plan.col_w) plan.width += w;
  for (TablePlan::Cell& c : plan.cells) {
    float w = -pads;
    for (size_t k = c.p.col; k < c.p.col + c.p.cols; ++k) w += plan.col_w[k];
    if (c.max_w > w + 0.5f) {
      LayoutResult r = measure_cell(doc, c.p.id, w, c.height);
      c.baseline = r.glyphs.empty() ? 0.f : r.glyphs.front().y;
    }
  }
  viewport_ = saved_viewport;
}

void LayoutEngine::place_table(const Document& doc, NodeId table,
                               const TablePlan& plan, float x, float& y,
                               LayoutResult& out) {
  const TableGrid& grid = plan.grid;
  const size_t columns = grid.columns, n_rows = grid.rows.size();
  if (columns == 0 || n_rows == 0) return;
  const float pad_x = plan.pad_x, pad_y = plan.pad_y;
  std::vector<float> col_x(columns + 1, x);
  for (size_t k = 0; k < columns; ++k) col_x[k + 1] = col_x[k] + plan.col_w[k];

  // Row heights: single-row cells first (baseline cells line their first
  // baselines up), then spans, shorter first; a deficit is shared out.
  std::vector<float> heights(n_rows, pad_y * 2.f + 2.f);
  std::vector<float> row_base(n_rows, 0.f);
  for (const TablePlan::Cell& c : plan.cells)
    if (c.p.rows == 1 && c.valign == CellVAlign::Baseline)
      row_base[c.p.row] = std::max(row_base[c.p.row], c.baseline);
  auto cell_need = [&](const TablePlan::Cell& c) {
    float h = c.height;
    if (c.p.rows == 1 && c.valign == CellVAlign::Baseline)
      h += row_base[c.p.row] - c.baseline;
    return h + pad_y * 2.f + 2.f;
  };
  for (const TablePlan::Cell& c : plan.cells)
    if (c.p.rows == 1) heights[c.p.row] = std::max(heights[c.p.row], cell_need(c));
  std::vector<const TablePlan::Cell*> spans;
  for (const TablePlan::Cell& c : plan.cells)
    if (c.p.rows > 1) spans.push_back(&c);
  std::stable_sort(spans.begin(), spans.end(),
                   [](auto* a, auto* b) { return a->p.rows < b->p.rows; });
  for (const TablePlan::Cell* c : spans) {
    float have = 0.f;
    for (size_t r = c->p.row; r < c->p.row + c->p.rows; ++r) have += heights[r];
    const float extra = std::max(0.f, cell_need(*c) - have) / c->p.rows;
    for (size_t r = c->p.row; r < c->p.row + c->p.rows; ++r) heights[r] += extra;
  }
  std::vector<float> row_y(n_rows + 1, y);
  for (size_t r = 0; r < n_rows; ++r) row_y[r + 1] = row_y[r] + heights[r];

  const auto saved_floats = floats_;
  const TextAlign saved_align = line_align_;
  const bool saved_cell = in_cell_;
  in_cell_ = true;
  for (const TablePlan::Cell& c : plan.cells) {
    const float left = col_x[c.p.col], right = col_x[c.p.col + c.p.cols];
    const float top = row_y[c.p.row], bottom = row_y[c.p.row + c.p.rows];
    const float w = std::max(1.f, right - left - 2.f * pad_x);
    const float room = std::max(0.f, bottom - top - 2.f * pad_y - 2.f - c.height);
    float dy = 0.f;
    switch (c.valign) {
      case CellVAlign::Top: break;
      case CellVAlign::Middle: dy = std::round(room * 0.5f); break;
      case CellVAlign::Bottom: dy = room; break;
      case CellVAlign::Baseline:
        if (c.p.rows == 1) dy = row_base[c.p.row] - c.baseline;
        break;
    }
    float dx = 0.f;
    line_align_ = TextAlign::Start;
    if (c.align == CellAlign::Center) line_align_ = TextAlign::Center;
    else if (c.align == CellAlign::Right) line_align_ = TextAlign::End;
    else if (c.align == CellAlign::Decimal)
      dx = std::clamp(w - plan.dec_b[c.p.col] - c.dec_a, 0.f,
                      std::max(0.f, w - c.dec_a - c.dec_b));
    float content_y = top + pad_y + dy;
    const size_t box_at = out.boxes.size();
    const size_t fill_at = out.table_header_fills.size();
    floats_.clear();
    layout_node(doc, c.p.id, left + pad_x + dx, content_y, w - dx, out);
    floats_ = saved_floats;
    for (size_t i = box_at; i < out.boxes.size(); ++i)
      if (out.boxes[i].node == c.p.id)
        out.boxes[i] = {c.p.id, left + pad_x, top + pad_y, w,
                        std::max(0.f, bottom - top - 2.f * pad_y)};
    for (size_t i = fill_at; i < out.table_header_fills.size(); ++i)
      if (out.table_header_fills[i].node == c.p.id)
        out.table_header_fills[i] = {c.p.id, left + 1.f, top + 1.f,
                                     std::max(0.f, right - left - 2.f),
                                     std::max(0.f, bottom - top - 2.f)};
  }
  line_align_ = saved_align;
  in_cell_ = saved_cell;
  const float width = col_x.back() - x;
  for (size_t r = 0; r < n_rows; ++r)
    out.boxes.push_back({grid.rows[r], x, row_y[r], width, heights[r]});

  // Frame, the lines between cells (not through merged ones) and a heavier
  // rule under the head rows and over the foot rows.
  const float stroke = std::max(1.f, std::round(options_.px_scale));
  const float top = row_y.front(), bottom = row_y.back();
  out.table_lines.push_back({table, x, top, width, stroke});
  out.table_lines.push_back({table, x, bottom - stroke, width, stroke});
  out.table_lines.push_back({table, x, top, stroke, bottom - top});
  out.table_lines.push_back({table, x + width - stroke, top, stroke, bottom - top});
  for (size_t r = 1; r < n_rows; ++r) {
    const bool heavy = (plan.head[r - 1] && !plan.head[r]) ||
                       (doc.get(grid.rows[r]).level == 2 &&
                        doc.get(grid.rows[r - 1]).level != 2);
    const float s = heavy ? 2.f * stroke : stroke;
    for (size_t k = 0; k < columns; ++k)
      if (grid.slots[r - 1][k] == INVALID_NODE || grid.slots[r - 1][k] != grid.slots[r][k])
        out.table_lines.push_back({table, col_x[k], row_y[r] - s * 0.5f,
                                   col_x[k + 1] - col_x[k], s});
  }
  for (size_t k = 1; k < columns; ++k) {
    for (size_t r = 0; r < n_rows; ++r)
      if (grid.slots[r][k - 1] == INVALID_NODE || grid.slots[r][k - 1] != grid.slots[r][k])
        out.table_lines.push_back({table, col_x[k] - stroke * 0.5f, row_y[r],
                                   stroke, heights[r]});
    if (!plan.weight_cells.empty()) {
      ColumnGap gap;
      gap.columns = table;
      gap.left = plan.weight_cells[k - 1];
      gap.right = plan.weight_cells[k];
      gap.x = col_x[k];
      gap.y = row_y[plan.weight_row];  // not over a spanning heading
      gap.height = bottom - gap.y;
      gap.left_w = col_x[k] - col_x[k - 1];
      gap.right_w = col_x[k + 1] - col_x[k];
      out.column_gaps.push_back(gap);
    }
  }
  y = bottom;
}

void seed_table_weights(Document& doc, const LayoutResult& layout, NodeId table) {
  if (table == INVALID_NODE || doc.get(table).kind != NodeKind::Table) return;
  std::vector<std::pair<NodeId, float>> widths;  // weight cells, left to right
  for (const ColumnGap& g : layout.column_gaps) {
    if (g.columns != table) continue;
    if (widths.empty()) widths.push_back({g.left, g.left_w});
    if (widths.back().first != g.left) return;  // not one chain of gaps
    widths.push_back({g.right, g.right_w});
  }
  if (widths.size() < 2) return;
  float sum = 0.f;
  for (const auto& [id, w] : widths) {
    if (std::abs(doc.get(id).display_scale - 1.f) > 0.01f) return;
    sum += w;
  }
  if (sum <= 0.f) return;
  const float mean = sum / static_cast<float>(widths.size());
  for (const auto& [id, w] : widths)
    doc.get(id).display_scale = std::clamp(w / mean, 0.2f, 19.f);
}

void LayoutEngine::layout_node(const Document& doc, NodeId id,
                               float x, float& y, float max_width,
                               LayoutResult& out) {
  if (id == INVALID_NODE) return;
  const Node& node = doc.get(id);
  if (node.id != id) return;  // stale child id → invalid-node sentinel

  // A named block variant overlays only its declared role properties. The
  // scope is lexical: the resolved style applies to this block and its
  // descendants, then the parent's style is restored for the next sibling.
  std::optional<DocStyle> scoped_style;
  const DocStyle* parent_style = style_;
  const size_t parent_index = active_style_index_;
  if (!node.style.empty()) {
    scoped_style.emplace(*style_);
    if (apply_style_variant(*scoped_style, node.style)) {
      style_ = &*scoped_style;
      out.scoped_styles.push_back(*scoped_style);
      active_style_index_ = out.scoped_styles.size();
    }
  }
  if (active_style_index_ != 0) out.node_style_index[id] = active_style_index_;
  struct RestoreStyle {
    LayoutEngine& engine;
    const DocStyle* parent;
    size_t& active;
    size_t parent_index;
    ~RestoreStyle() {
      engine.set_style(parent);
      active = parent_index;
    }
  } restore{*this, parent_style, active_style_index_, parent_index};

  // Role spacing from the style sheet; em follows the body size (zoom).
  const DocStyle& st = *style_;
  const float em = static_cast<float>(fonts_.regular.pixel_size());
  const float dp = options_.px_scale;
  auto before = [&](BlockRole r) { return st[r].space_before.px(em, dp); };
  auto after = [&](BlockRole r) { return st[r].space_after.px(em, dp); };
  auto indent = [&](BlockRole r) { return st[r].indent.px(em, dp); };

  switch (node.kind) {
    case NodeKind::Document:
      if (focus_section_ != INVALID_NODE) {
        // A cut that is no part of the section tree is laid out on its own.
        NodeId top = focus_section_;
        while (doc.get(top).parent != id &&
               is_section_kind(doc.get(top).kind))
          top = doc.get(top).parent;
        if (doc.get(top).parent != id || !is_section_kind(doc.get(top).kind)) {
          layout_node(doc, focus_section_, x, y, max_width, out);
          break;
        }
        // Only section tree on the path to the focus; skip preamble blocks.
        for (NodeId child : node.children) {
          const Node& ch = doc.get(child);
          if (ch.kind == NodeKind::Section || ch.kind == NodeKind::Article)
            layout_node(doc, child, x, y, max_width, out);
        }
      } else {
        if (hide_chrome_) layout_chrome_notice(doc, x, y, max_width, out);
        layout_children_sequential(doc, node.children, x, y, max_width, out);
      }
      break;

    case NodeKind::Chrome: {
      if (hide_chrome_) break;  // reader mode
      // Navigation / side bars shown in the Page panel instead.
      if (hide_panel_chrome_ && margin_chrome(doc, node)) break;
      float start_y = y;
      layout_children_sequential(doc, node.children, x, y, max_width, out);
      out.boxes.push_back({id, x, start_y, max_width, y - start_y});
      break;
    }

    case NodeKind::Aside:
      layout_aside(doc, node, x, y, max_width, out);
      break;

    case NodeKind::List:
      y += before(BlockRole::List);
      layout_children_sequential(doc, node.children, x, y, max_width, out);
      y += after(BlockRole::List);  // trailing gap after the list block
      break;

    case NodeKind::Column:
      // Column content is laid out by the parent Columns node.
      layout_children_sequential(doc, node.children, x, y, max_width, out);
      break;

    case NodeKind::Columns: {
      // Side-by-side columns. Widths from Column.display_scale (relative weight).
      std::vector<NodeId> cols;
      for (NodeId child : node.children) {
        if (doc.get(child).kind == NodeKind::Column) {
          cols.push_back(child);
        }
      }
      if (cols.empty()) {
        for (NodeId child : node.children) {
          layout_node(doc, child, x, y, max_width, out);
        }
        break;
      }

      const float gutter = 16.f;
      const float n = static_cast<float>(cols.size());
      float weight_sum = 0.f;
      std::vector<float> weights(cols.size(), 1.f);
      for (size_t i = 0; i < cols.size(); ++i) {
        float w = doc.get(cols[i]).display_scale;
        if (w < 0.1f) w = 1.f;
        weights[i] = w;
        weight_sum += w;
      }
      if (weight_sum < 0.1f) weight_sum = n;

      const float usable = max_width - gutter * (n - 1.f);
      if (usable / n < 40.f) {
        for (NodeId child : cols) {
          layout_node(doc, child, x, y, max_width, out);
        }
        break;
      }

      float start_y = y;
      float max_bottom = y;
      float cx = x;
      size_t gap_base = out.column_gaps.size();

      for (size_t i = 0; i < cols.size(); ++i) {
        float col_w = usable * (weights[i] / weight_sum);
        // Keep columns inside the usable width (never force min 40 past usable).
        if (col_w < 1.f) col_w = 1.f;
        if (col_w > usable) col_w = usable;
        float cy = start_y;

        LayoutResult col_out;
        layout_node(doc, cols[i], cx, cy, col_w, col_out);

        append_result(out, col_out);

        if (cy > max_bottom) max_bottom = cy;

        if (i + 1 < cols.size()) {
          ColumnGap gap;
          gap.columns = id;
          gap.left = cols[i];
          gap.right = cols[i + 1];
          gap.x = cx + col_w + gutter * 0.5f;
          gap.y = start_y;
          gap.height = 0.f;
          gap.left_w = col_w;
          gap.right_w = usable * (weights[i + 1] / weight_sum);
          out.column_gaps.push_back(gap);
        }
        cx += col_w + gutter;
      }

      for (size_t gi = gap_base; gi < out.column_gaps.size(); ++gi) {
        if (out.column_gaps[gi].columns == id)
          out.column_gaps[gi].height = max_bottom - start_y;
      }

      out.boxes.push_back({id, x, start_y, max_width, max_bottom - start_y});
      y = max_bottom + 12.f;
      break;
    }

    case NodeKind::Stretch:
      layout_stretch(doc, node, x, y, max_width, out);
      break;

    case NodeKind::Summary:
      layout_children_sequential(doc, node.children, x, y, max_width, out);
      break;

    case NodeKind::Heading: {
      FontRole role = (node.level <= 2) ? FontRole::Heading : FontRole::Body;
      float start_y = y;
      y += (node.level == 1) ? 12.f : 8.f;
      layout_paragraph(doc, node, x, y, max_width, out, role,
                       BlockRole::Header);
      y += (node.level == 1) ? 14.f : 10.f;
      out.boxes.push_back({id, x, start_y, max_width, y - start_y});
      break;
    }

    case NodeKind::Header: {
      float start_y = y;
      y += before(BlockRole::Header);
      const float text_y = y;
      // Org TODO / :tags: live in src + tags. Show them as decoration glyphs
      // that do not own a real span index (node = INVALID) so caret/edit stay
      // on the title text only.
      if (!node.src.empty() || !node.tags.empty()) {
        const size_t glyph0 = out.glyphs.size();
        size_t deco_before = node.src.empty() ? 0 : 1;
        Node vis = node;
        vis.spans.clear();
        if (!node.src.empty()) {
          InlineSpan todo;
          todo.kind = InlineKind::Strong;
          todo.text = node.src + " ";
          vis.spans.push_back(std::move(todo));
        }
        for (const auto& sp : node.spans) vis.spans.push_back(sp);
        size_t deco_tag_span = std::string::npos;
        if (!node.tags.empty()) {
          deco_tag_span = vis.spans.size();
          InlineSpan tg;
          tg.kind = InlineKind::Code;
          tg.text = "  :" + node.tags + ":";
          vis.spans.push_back(std::move(tg));
        }
        layout_role_text(doc, vis, BlockRole::Header, x, y, max_width, out);
        for (size_t gi = glyph0; gi < out.glyphs.size(); ++gi) {
          auto& g = out.glyphs[gi];
          g.node = id;
          if (g.span_index < deco_before ||
              (deco_tag_span != std::string::npos &&
               g.span_index == deco_tag_span)) {
            g.node = INVALID_NODE;  // decoration — not editable
          } else if (deco_before > 0) {
            g.span_index -= deco_before;
          }
        }
      } else {
        layout_role_text(doc, node, BlockRole::Header, x, y, max_width, out);
      }
      float band_x = x, band_w = max_width;
      float_band(text_y, y, band_x, band_w);
      if (band_w > 0.f)
        add_role_band(BlockRole::Header, id, band_x, text_y, band_w, y, out);
      y += after(BlockRole::Header);
      out.boxes.push_back({id, x, start_y, max_width, y - start_y});
      break;
    }

    case NodeKind::Article:
    case NodeKind::Section: {
      // Section-focus view: show only the focused section (full), its
      // ancestors (header only), and descendants of the focus (full).
      if (focus_section_ != INVALID_NODE) {
        const bool is_focus = (id == focus_section_);
        const bool under_focus = node_is_descendant(doc, focus_section_, id);
        const bool is_ancestor = node_is_descendant(doc, id, focus_section_);
        if (!is_focus && !under_focus && !is_ancestor)
          break;  // sibling / other branch — omit entirely
        if (is_ancestor && !is_focus && !under_focus) {
          float start_y = y;
          float inset =
              (node.kind == NodeKind::Section) ? indent(BlockRole::Section) : 0.f;
          y += before(BlockRole::Section);
          for (NodeId child : node.children) {
            const Node& ch = doc.get(child);
            if (ch.kind == NodeKind::Header || ch.kind == NodeKind::Heading)
              layout_node(doc, child, x + inset, y, max_width - inset, out);
          }
          // Recurse into section children so the path reaches the focus.
          for (NodeId child : node.children) {
            const Node& ch = doc.get(child);
            if (ch.kind == NodeKind::Section || ch.kind == NodeKind::Article)
              layout_node(doc, child, x + inset, y, max_width - inset, out);
          }
          y += before(BlockRole::Section);
          out.boxes.push_back({id, x, start_y, max_width, y - start_y});
          break;
        }
        // focus or descendant: fall through to full layout
      }
      // Articles on one page are separated by a gap where the margin shows.
      const bool article = node.kind == NodeKind::Article;
      auto gap = [&](float h) {
        if (h > 0.f) out.page_gaps.push_back({id, x, y, max_width, h});
        y += h;
      };
      if (article) gap(before(BlockRole::Article));
      float start_y = y;
      // Nested sections indent slightly; articles keep full width.
      float inset =
          (node.kind == NodeKind::Section) ? indent(BlockRole::Section) : 0.f;
      y += before(BlockRole::Section);
      const bool folded =
          folded_sections_ && folded_sections_->find(id) != folded_sections_->end();
      // Always materialise title headers so fold state stays visible.
      std::vector<NodeId> body;
      body.reserve(node.children.size());
      for (NodeId child : node.children) {
        const Node& ch = doc.get(child);
        if (ch.kind == NodeKind::Header || ch.kind == NodeKind::Heading)
          layout_node(doc, child, x + inset, y, max_width - inset, out);
        else
          body.push_back(child);
      }
      if (folded) {
        // Compact cue under the header (view-local fold; not Stretch).
        y += before(BlockRole::Section);
        out.boxes.push_back({id, x, start_y, max_width, y - start_y});
        if (article) gap(after(BlockRole::Article));
        break;
      }
      const size_t first_float = floats_.size();
      layout_children_sequential(doc, body, x + inset, y, max_width - inset, out);
      // An Article holds its floats (its page gap must not run under one);
      // sections let an infobox float on past their end.
      if (article) contain_floats(first_float, y);
      y += after(BlockRole::Section);
      out.boxes.push_back({id, x, start_y, max_width, y - start_y});
      if (article) gap(after(BlockRole::Article));
      break;
    }

    case NodeKind::Paragraph: {
      float start_y = y;
      y += before(BlockRole::Paragraph);
      if (cue_.block == id)
        layout_cued_text(doc, node, BlockRole::Paragraph, x, y, max_width,
                         (node.spans.empty() ? "" : " ") + cue_.text,
                         cue_.fold, out);
      else
        layout_role_text(doc, node, BlockRole::Paragraph, x, y, max_width,
                         out);
      y += after(BlockRole::Paragraph);
      out.boxes.push_back({id, x, start_y, max_width, y - start_y});
      break;
    }

    case NodeKind::BlockQuote: {
      y += before(BlockRole::BlockQuote);
      float start_y = y;
      const float bar_w = 3.f;
      const float inset = indent(BlockRole::BlockQuote);
      float text_x = x + bar_w + inset;
      float text_w = max_width - bar_w - inset;
      if (text_w < 40.f) {
        text_x = x + 8.f;
        text_w = max_width - 8.f;
      }
      layout_role_text(doc, node, BlockRole::BlockQuote, text_x, y, text_w, out);
      y += after(BlockRole::BlockQuote);
      out.quote_bars.push_back({id, x, start_y, bar_w, y - start_y});
      out.boxes.push_back({id, x, start_y, max_width, y - start_y});
      break;
    }

    case NodeKind::Note: {
      y += before(BlockRole::Note);
      float start_y = y;
      const float bar_w = 3.f;
      const float inset = indent(BlockRole::Note);
      float text_x = x + bar_w + inset;
      float text_w = max_width - bar_w - inset;
      if (text_w < 40.f) {
        text_x = x + 8.f;
        text_w = max_width - 8.f;
      }
      layout_role_text(doc, node, BlockRole::Note, text_x, y, text_w, out);
      y += after(BlockRole::Note);
      out.note_bars.push_back({id, x, start_y, bar_w, y - start_y});
      out.boxes.push_back({id, x, start_y, max_width, y - start_y});
      break;
    }

    case NodeKind::Thread: {
      float start_y = y;
      // Subject line: the title Header (editable, in the outline), else the
      // legacy `summary`; the thread's #label follows as decoration.
      const NodeId title = thread_title(doc, id);
      Node header = title != INVALID_NODE ? doc.get(title) : node;
      header.kind = NodeKind::Paragraph;
      if (title == INVALID_NODE) header.spans.clear();
      const size_t own_spans = header.spans.size();
      for (InlineSpan& sp : header.spans)
        if (sp.kind == InlineKind::Text) sp.kind = InlineKind::Strong;
      std::string tail = header.spans.empty()
                             ? (node.summary.empty() ? "Thread" : node.summary)
                             : std::string();
      if (!node.label.empty()) {
        tail += "  ·  #";
        tail += node.label;
      }
      if (!tail.empty()) {
        InlineSpan hs;
        hs.kind = InlineKind::Strong;
        hs.text = std::move(tail);
        header.spans.push_back(std::move(hs));
      }
      y += before(BlockRole::Thread);
      const float text_y = y;
      const size_t glyph0 = out.glyphs.size();
      layout_role_text(doc, header, BlockRole::Thread, x, y, max_width, out);
      if (title != INVALID_NODE) {
        for (size_t gi = glyph0; gi < out.glyphs.size(); ++gi) {
          auto& g = out.glyphs[gi];
          g.node = g.span_index < own_spans ? title : INVALID_NODE;
        }
        out.boxes.push_back({title, x, text_y, max_width, y - text_y});
      }
      add_role_band(BlockRole::Thread, id, x, text_y, max_width, y, out);
      y += 8.f;
      // Replies by the thread starter get the `message-op` author colour.
      const std::string outer_op = std::move(thread_op_);
      thread_op_.clear();
      for (NodeId child : node.children) {
        const Node& ch = doc.get(child);
        if (ch.kind == NodeKind::Message) {
          thread_op_ = ch.src;
          break;
        }
      }
      if (title != INVALID_NODE) {
        const std::vector<NodeId> rest(node.children.begin() + 1,
                                       node.children.end());
        layout_children_sequential(doc, rest, x, y, max_width, out);
      } else {
        layout_children_sequential(doc, node.children, x, y, max_width, out);
      }
      thread_op_ = outer_op;
      y += after(BlockRole::Thread);
      out.boxes.push_back({id, x, start_y, max_width, y - start_y});
      break;
    }

    case NodeKind::Message: {
      y += before(BlockRole::Message);
      float start_y = y;
      // Indent by reply depth (follow in-reply-to chain via labels).
      int depth = 0;
      NodeId reply_to = INVALID_NODE;
      if (!node.alt.empty() && node.alt.rfind("about:", 0) != 0) {
        std::string cur = node.alt;
        for (int guard = 0; guard < 24; ++guard) {
          if (cur.empty()) break;
          std::string key = cur[0] == '#' ? cur.substr(1) : cur;
          NodeId parent_msg = INVALID_NODE;
          auto it = g_msg_label_map.find(key);
          if (it != g_msg_label_map.end()) parent_msg = it->second;
          if (parent_msg == INVALID_NODE) break;
          if (depth == 0) reply_to = parent_msg;
          ++depth;
          cur = doc.get(parent_msg).alt;
          if (cur.rfind("about:", 0) == 0) break;
        }
      }
      const bool alt_card = (depth % 2) == 1;
      if (depth > 8) depth = 8;
      const float indent = 16.f * static_cast<float>(depth);
      const float bar_w = 3.f;
      const float pad = 6.f * dp;  // card inset above the header / at the right
      float body_x = x + indent + bar_w + 10.f;
      float body_w = max_width - indent - bar_w - 10.f - pad;
      if (body_w < 48.f) {
        body_x = x + 8.f;
        body_w = max_width - 8.f;
      }

      // Header: author · date · reply/about, coloured by the message-author /
      // message-op / message-meta roles so posts separate at a glance.
      // Optional semantic spans on Message (0 = author, 1 = date, 2 = reply)
      // annotate the reconstructed header without making layout HN-specific.
      // Spans with role=metadata and text (vote score, …) are extra fields
      // shown after the date; they never fill the positional slots.
      auto header_field = [&](size_t i) {
        return node.spans[i].role == InlineRole::Metadata &&
               !node.spans[i].text.empty();
      };
      auto header_link = [&](size_t i) -> const InlineSpan* {
        return i < node.spans.size() && !header_field(i) &&
                       node.spans[i].kind == InlineKind::Link
                   ? &node.spans[i]
                   : nullptr;
      };
      Node header = node;
      header.kind = NodeKind::Paragraph;
      header.spans.clear();
      // Header text is synthesized from Message fields. Keep separators as
      // distinct, non-interactive spans, and map the other glyphs back to
      // their semantic Message spans for hit-testing and timestamp hovers.
      constexpr size_t kNoHeaderOwner = static_cast<size_t>(-1);
      std::vector<size_t> header_owner;
      auto add_header = [&](InlineSpan span, size_t owner) {
        header.spans.push_back(std::move(span));
        header_owner.push_back(owner);
      };
      auto add_separator = [&]() {
        InlineSpan sep;
        sep.text = "  ·  ";
        add_header(std::move(sep), kNoHeaderOwner);
      };
      {
        InlineSpan a;
        a.kind = InlineKind::Strong;
        a.text = node.src.empty() ? "(unknown)" : node.src;
        if (const InlineSpan* l = header_link(0)) {
          a.kind = InlineKind::Link;
          a.href = l->href;
        }
        add_header(std::move(a), 0);
      }
      if (!node.summary.empty()) {
        add_separator();
        InlineSpan d;
        d.kind = InlineKind::Text;
        d.text = node.summary;
        if (node.spans.size() > 1 && !header_field(1)) {
          d.datetime = node.spans[1].datetime;
        }
        if (const InlineSpan* l = header_link(1)) {
          d.kind = InlineKind::Link;
          d.href = l->href;
        }
        add_header(std::move(d), 1);
      }
      for (size_t i = 0; i < node.spans.size(); ++i) {
        if (!header_field(i)) continue;
        add_separator();
        InlineSpan f = node.spans[i];
        f.role = InlineRole::Normal;
        add_header(std::move(f), i);
      }
      if (!node.alt.empty()) {
        add_separator();
        InlineSpan r;
        r.kind = InlineKind::Emphasis;
        if (node.alt.rfind("about:", 0) == 0) {
          r.text = "re ";
          r.text += node.alt.substr(6);
        } else if (reply_to != INVALID_NODE &&
                   !doc.get(reply_to).src.empty()) {
          // Name only: "Alice Example <alice@…>" → "Alice Example".
          std::string who = doc.get(reply_to).src;
          const size_t lt = who.find(" <");
          if (lt != std::string::npos && lt > 0) who.resize(lt);
          r.text = "reply to ";
          r.text += who;
        } else {
          r.text = "reply to #";
          r.text += node.alt;
        }
        if (const InlineSpan* l = header_link(2)) {
          r.kind = InlineKind::Link;
          r.href = l->href;
        }
        add_header(std::move(r), 2);
      }
      y += pad;
      const size_t header_glyphs = out.glyphs.size();
      layout_role_text(doc, header, BlockRole::Message, body_x, y, body_w, out);
      const bool is_op = depth > 0 && !thread_op_.empty() &&
                         node.src == thread_op_;
      const BlockStyle& author_st =
          st[is_op ? BlockRole::MessageOp : BlockRole::MessageAuthor];
      const BlockStyle& meta_st = st[BlockRole::MessageMeta];
      for (size_t gi = header_glyphs; gi < out.glyphs.size(); ++gi) {
        PositionedGlyph& g = out.glyphs[gi];
        const size_t owner = g.span_index < header_owner.size()
                                 ? header_owner[g.span_index]
                                 : kNoHeaderOwner;
        if (owner == kNoHeaderOwner) {
          // Decorations have no underlying document span/caret or hyperlink.
          g.node = INVALID_NODE;
        } else {
          g.node = id;
          g.span_index = owner;
        }
        const BlockStyle& bs = owner == 0 ? author_st : meta_st;
        if (bs.has_color) g.color = bs.color;
      }
      y += 4.f;
      // A message card holds its pictures: a floated thumbnail never runs
      // into the next message.
      const size_t first_float = floats_.size();
      for (NodeId child : node.children)
        layout_node(doc, child, body_x, y, body_w, out);
      contain_floats(first_float, y);
      out.message_cards.push_back(
          {{id, x + indent, start_y, max_width - indent, y - start_y},
           alt_card});
      out.message_bars.push_back(
          {id, x + indent, start_y, bar_w, y - start_y});
      y += after(BlockRole::Message);
      // The box keeps the trailing gap: its height is the virtualization hint.
      out.boxes.push_back({id, x + indent, start_y, max_width - indent,
                           y - start_y});
      break;
    }

    case NodeKind::CodeBlock: {
      y += before(BlockRole::CodeBlock);
      float start_y = y;
      const float pad = indent(BlockRole::CodeBlock);
      float text_x = x + pad;
      float text_w = max_width - pad * 2.f;
      if (text_w < 40.f) {
        text_x = x;
        text_w = max_width;
      }
      layout_role_text(doc, node, BlockRole::CodeBlock, text_x, y, text_w, out);
      y += after(BlockRole::CodeBlock);
      out.code_blocks.push_back({id, x, start_y, max_width, y - start_y});
      out.boxes.push_back({id, x, start_y, max_width, y - start_y});
      break;
    }

    case NodeKind::DefList: {
      float start_y = y;
      layout_children_sequential(doc, node.children, x, y, max_width, out);
      y += after(BlockRole::List);
      out.boxes.push_back({id, x, start_y, max_width, y - start_y});
      break;
    }

    case NodeKind::Term: {
      y += before(BlockRole::Term);
      float start_y = y;
      layout_role_text(doc, node, BlockRole::Term, x, y, max_width, out);
      y += after(BlockRole::Term);
      out.boxes.push_back({id, x, start_y, max_width, y - start_y});
      break;
    }

    case NodeKind::Definition: {
      y += before(BlockRole::Definition);
      float start_y = y;
      const float inset = indent(BlockRole::Definition);
      float text_x = x + inset;
      float text_w = max_width - inset;
      if (text_w < 40.f) {
        text_x = x + 8.f;
        text_w = max_width - 8.f;
      }
      layout_role_text(doc, node, BlockRole::Definition, text_x, y, text_w, out);
      y += after(BlockRole::Definition);
      out.boxes.push_back({id, x, start_y, max_width, y - start_y});
      break;
    }

    case NodeKind::Collection:
      layout_collection(doc, id, x, y, max_width, out);
      break;

    case NodeKind::Figure: {
      if (!in_figure_float_ && !in_margin_ &&
          (node.float_side == "left" || node.float_side == "right")) {
        const float em = static_cast<float>(fonts_.regular.pixel_size());
        const float gap = em;
        float fw = std::min(max_width * 0.45f, 20.f * em);
        // Size the figure to its image, so the caption and the exclusion
        // band do not reserve empty space beside a small thumbnail.
        if (images_) {
          for (NodeId child : node.children) {
            const Node& image = doc.get(child);
            if (image.kind != NodeKind::Image || image.src.empty()) continue;
            const bool remote = is_remote_image(image.src);
            const Image& img = remote
                ? images_->thumb(image.src, image_base_, kRemoteThumbEdge)
                : images_->get(image.src, image_base_);
            if (img.ok && img.width > 0 && img.height > 0) {
              float scale = remote
                  ? std::min({options_.px_scale, fw / img.width,
                              480.f * options_.px_scale / img.height})
                  : std::min({1.f, fw / img.width, 480.f / img.height});
              if (!remote) {
                scale *= image.display_scale > 0.05f ? image.display_scale : 1.f;
                scale = std::min(scale, fw / img.width);
              }
              // A local image's interactive scale is applied after fitting
              // to its column; shrinking that column would apply it twice.
              if (remote || image.display_scale >= 0.99f)
                fw = std::min(fw, img.width * scale);
            }
            break;
          }
        }
        if (fw > 0.f && max_width - fw - gap >= 10.f * em) {
          const bool right = node.float_side == "right";
          const float bx = right ? x + max_width - fw : x;
          float top = y;
          for (bool moved = true; moved;) {
            moved = false;
            for (const FloatBox& f : floats_) {
              if (f.top <= top && f.bottom > top && f.right > bx && f.left < bx + fw) {
                top = f.bottom;
                moved = true;
              }
            }
          }
          float bottom = top;
          in_figure_float_ = true;
          in_image_float_ = true;
          layout_node(doc, id, bx, bottom, fw, out);
          in_image_float_ = false;
          in_figure_float_ = false;
          if (bottom > top) {
            FloatBox fb;
            fb.top = top; fb.bottom = bottom;
            fb.side = right ? FloatSide::Right : FloatSide::Left;
            fb.left = right ? bx - gap : bx;
            fb.right = right ? bx + fw : bx + fw + gap;
            floats_.push_back(fb);
          }
          break;
        }
        y = clear_floats(x, max_width, y);
      }
      const bool was_in_image_float = in_image_float_;
      if (!was_in_image_float && !in_margin_ && figure_in_row(doc, id) &&
          layout_figure_row(doc, id, x, y, max_width, out))
        break;
      y += before(BlockRole::Figure);
      float start_y = y;
      in_image_float_ = true;
      for (NodeId c : node.children)
        layout_node(doc, c, x, y, max_width, out);
      in_image_float_ = was_in_image_float;
      y += after(BlockRole::Figure);
      out.boxes.push_back({id, x, start_y, max_width, y - start_y});
      break;
    }

    case NodeKind::Caption: {
      y += before(BlockRole::Caption);
      float start_y = y;
      const float pad = 4.f + indent(BlockRole::Caption);
      const TextAlign was_aligned = line_align_;
      line_align_ = in_figure_column_ && figure_centered() ? TextAlign::Center
                                                           : TextAlign::Start;
      layout_role_text(doc, node, BlockRole::Caption, x + pad, y,
                       max_width - pad * 2.f, out);
      line_align_ = was_aligned;
      y += after(BlockRole::Caption);
      out.boxes.push_back({id, x, start_y, max_width, y - start_y});
      break;
    }

    case NodeKind::Footnote: {
      // Body blocks hang right of the key; the key sits on the first line.
      const float start_y = y;
      y += before(BlockRole::Footnote);
      const FontRole mrole = st[BlockRole::Footnote].font;
      Font& mf = fonts_.pick(mrole);
      // Hang past the widest key of the run ("100." etc.) so keys never run
      // into the body, but never more than a quarter of the column.
      float hang = indent(BlockRole::Footnote);
      {
        float widest = static_cast<float>(mf.measure(footnote_marker(node.label)));
        if (node.parent != INVALID_NODE) {
          auto it = g_footnote_key_w.find(node.parent);
          if (it == g_footnote_key_w.end()) {
            float w = 0.f;
            for (NodeId sib : doc.get(node.parent).children) {
              const Node& sn = doc.get(sib);
              if (sn.id == sib && sn.kind == NodeKind::Footnote)
                w = std::max(w, static_cast<float>(
                                    mf.measure(footnote_marker(sn.label))));
            }
            it = g_footnote_key_w.emplace(node.parent, w).first;
          }
          widest = std::max(widest, it->second);
        }
        const float gap = mf.glyph(' ').advance_px();
        hang = std::max(hang, std::min(widest + gap, 0.25f * max_width));
      }
      const size_t first = out.glyphs.size();
      const float body_y = y;
      // Paragraphs take the footnote role (font, colour, tighter spacing).
      for (size_t ci = 0; ci < node.children.size(); ++ci) {
        const NodeId cid = node.children[ci];
        const Node& child = doc.get(cid);
        if (child.id != cid) continue;
        if (child.kind != NodeKind::Paragraph) {
          layout_node(doc, cid, x + hang, y, max_width - hang, out);
          continue;
        }
        if (ci > 0) y += after(BlockRole::Footnote);
        const float py = y;
        layout_role_text(doc, child, BlockRole::Footnote, x + hang, y,
                         max_width - hang, out);
        out.boxes.push_back({cid, x + hang, py, max_width - hang, y - py});
      }
      float baseline = body_y + static_cast<float>(mf.ascender());
      for (size_t gi = first; gi < out.glyphs.size(); ++gi) {
        if (out.glyphs[gi].node == INVALID_NODE) continue;
        baseline = out.glyphs[gi].y;
        break;
      }
      std::vector<uint32_t> m_cps;
      decode_utf8(footnote_marker(node.label), m_cps);
      float mx = x;
      for (uint32_t cp : m_cps) {
        const Glyph& g = mf.glyph(cp);
        PositionedGlyph pg;
        pg.codepoint = cp;
        pg.x = mx + static_cast<float>(g.bearing_x);
        pg.y = baseline;
        pg.node = INVALID_NODE;  // decoration, not a caret target
        pg.kind = InlineKind::FootnoteRef;
        pg.font = mrole;
        pg.color = st.text_color(BlockRole::Footnote, InlineKind::FootnoteRef);
        pg.advance = mx + g.advance_px() - pg.x;
        out.glyphs.push_back(pg);
        mx += g.advance_px();
      }
      if (node.children.empty()) y += mf.line_height();
      y += after(BlockRole::Footnote);
      out.boxes.push_back({id, x, start_y, max_width, y - start_y});
      break;
    }

    case NodeKind::Biblio: {
      float start_y = y;
      layout_children_sequential(doc, node.children, x, y, max_width, out);
      y += after(BlockRole::List);
      out.boxes.push_back({id, x, start_y, max_width, y - start_y});
      break;
    }

    case NodeKind::BiblioEntry: {
      float start_y = y;
      // Hanging indent style for references
      y += before(BlockRole::BiblioEntry);
      const float hang = indent(BlockRole::BiblioEntry);
      layout_role_text(doc, node, BlockRole::BiblioEntry, x + hang, y,
                       max_width - hang, out);
      y += after(BlockRole::BiblioEntry);
      out.boxes.push_back({id, x, start_y, max_width, y - start_y});
      break;
    }

    case NodeKind::Table: {
      const float em = static_cast<float>(fonts_.regular.pixel_size());
      if (!in_table_float_ && !in_margin_ &&
          (node.float_side == "left" || node.float_side == "right")) {
        const float gap = em;
        const float want = (*style_)[BlockRole::Table].width.px(em, options_.px_scale);
        float fw = std::min(want, max_width * 0.45f);
        if (fw >= 6.f * em && max_width - fw - gap >= 10.f * em) {
          // As wide as its content, up to the float width.
          TablePlan plan;
          plan_table(doc, id, fw, plan);
          if (plan.width > 0.f) fw = std::min(fw, std::max(plan.width, 6.f * em));
          const bool right = node.float_side == "right";
          const float bx = right ? x + max_width - fw : x;
          float top = y;
          for (bool moved = true; moved;) {
            moved = false;
            for (const FloatBox& f : floats_) {
              if (f.top <= top && f.bottom > top && f.right > bx && f.left < bx + fw) {
                top = f.bottom;
                moved = true;
              }
            }
          }
          float bottom = top;
          in_table_float_ = true;
          layout_node(doc, id, bx, bottom, fw, out);
          in_table_float_ = false;
          if (bottom > top) {
            FloatBox fb;
            fb.top = top; fb.bottom = bottom;
            fb.side = right ? FloatSide::Right : FloatSide::Left;
            fb.left = right ? bx - gap : bx;
            fb.right = right ? bx + fw : bx + fw + gap;
            floats_.push_back(fb);
          }
          break;
        }
        y = clear_floats(x, max_width, y);
      }
      y += before(BlockRole::Table);
      float start_y = y;
      bool has_rows = false;
      for (NodeId c : node.children)
        has_rows |= doc.get(c).kind == NodeKind::TableRow;
      if (!has_rows) {
        for (NodeId c : node.children)
          layout_node(doc, c, x, y, max_width, out);
        y += after(BlockRole::Table);
        out.boxes.push_back({id, x, start_y, max_width, y - start_y});
        break;
      }
      TablePlan plan;
      plan_table(doc, id, max_width, plan);
      const float tw = std::min(max_width, plan.width);
      // A table narrower than the column: where the node or the sheet says.
      TextAlign place = (*style_)[BlockRole::Table].align;
      const std::string side = canonical_align(node.align);
      if (side == "left") place = TextAlign::Start;
      else if (side == "center") place = TextAlign::Center;
      else if (side == "right") place = TextAlign::End;
      float tx = x;
      if (!in_table_float_) {
        if (place == TextAlign::Center) tx += std::round((max_width - tw) * 0.5f);
        else if (place == TextAlign::End) tx += max_width - tw;
      }
      // Captions over the table, a readable measure even over a narrow one.
      const float cap_w = std::min(max_width, std::max(tw, 20.f * em));
      const float cap_x = std::clamp(tx + (tw - cap_w) * 0.5f, x, x + max_width - cap_w);
      bool captioned = false;
      for (NodeId c : node.children)
        if (doc.get(c).kind == NodeKind::Caption) {
          layout_node(doc, c, tw >= cap_w ? tx : cap_x, y, cap_w, out);
          captioned = true;
        }
      place_table(doc, id, plan, tx, y, out);
      y += after(BlockRole::Table);
      const float bx = captioned ? std::min(tx, cap_x) : tx;
      const float br = captioned ? std::max(tx + tw, cap_x + cap_w) : tx + tw;
      out.boxes.push_back({id, bx, start_y, br - bx, y - start_y});
      break;
    }

    case NodeKind::TableRow: {
      float start_y = y;
      std::vector<NodeId> cells;
      for (NodeId c : node.children) {
        if (doc.get(c).kind == NodeKind::TableCell) cells.push_back(c);
      }
      if (cells.empty()) {
        for (NodeId c : node.children)
          layout_node(doc, c, x, y, max_width, out);
      } else {
        // Weights: prefer first row of parent table (shared column model)
        std::vector<float> weights(cells.size(), 1.f);
        if (node.parent != INVALID_NODE &&
            doc.get(node.parent).kind == NodeKind::Table) {
          for (NodeId rid : doc.get(node.parent).children) {
            if (doc.get(rid).kind != NodeKind::TableRow) continue;
            size_t i = 0;
            for (NodeId c : doc.get(rid).children) {
              if (doc.get(c).kind != NodeKind::TableCell) continue;
              if (i < weights.size()) {
                float w = doc.get(c).display_scale;
                if (w < 0.15f) w = 0.15f;
                if (w > 20.f) w = 20.f;
                weights[i] = w;
              }
              ++i;
            }
            break;  // first row only
          }
        } else {
          for (size_t i = 0; i < cells.size(); ++i) {
            float w = doc.get(cells[i]).display_scale;
            if (w < 0.15f) w = 0.15f;
            if (w > 20.f) w = 20.f;
            weights[i] = w;
          }
        }
        float weight_sum = 0.f;
        for (float w : weights) weight_sum += w;
        if (weight_sum < 0.01f) weight_sum = static_cast<float>(cells.size());

        float max_bottom = start_y;
        const float cell_pad = 4.f;
        float cx = x;
        for (size_t i = 0; i < cells.size(); ++i) {
          float col_w = max_width * (weights[i] / weight_sum);
          if (col_w < 32.f) col_w = 32.f;
          float cy = start_y + cell_pad;
          LayoutResult cell_out;
          layout_node(doc, cells[i], cx + cell_pad, cy, col_w - cell_pad * 2.f,
                      cell_out);
          append_result(out, cell_out);
          if (cy > max_bottom) max_bottom = cy;
          cx += col_w;
        }
        y = max_bottom + cell_pad + 2.f;
      }
      out.boxes.push_back({id, x, start_y, max_width, y - start_y});
      break;
    }

    case NodeKind::TableCell: {
      float start_y = y;
      // Nested block cells: when the cell has block children, lay those out
      // instead of (or after) the cell's own inline spans.
      bool has_blocks = false;
      for (NodeId c : node.children) {
        const NodeKind ck = doc.get(c).kind;
        if (ck != NodeKind::Document && ck != NodeKind::Column) {
          has_blocks = true;
          break;
        }
      }
      if (has_blocks) {
        // Optional leading inlines become a synthetic first paragraph area
        if (!node.spans.empty()) {
          FontRole role =
              (node.level > 0) ? FontRole::BodyBold : FontRole::Body;
          layout_paragraph(doc, node, x, y, max_width, out, role,
                           BlockRole::Table);
          y += 2.f;
        }
        for (NodeId c : node.children)
          layout_node(doc, c, x, y, max_width, out);
        y += 2.f;
      } else {
        FontRole role =
            (node.level > 0) ? FontRole::BodyBold : FontRole::Body;
        layout_paragraph(doc, node, x, y, max_width, out, role,
                         BlockRole::Table);
        y += 4.f;
      }
      if (node.level > 0) {
        // Header fill covers the laid-out cell content area.
        out.table_header_fills.push_back(
            {id, x - 2.f, start_y - 2.f, max_width + 4.f, y - start_y + 2.f});
      }
      out.boxes.push_back({id, x, start_y, max_width, y - start_y});
      break;
    }

    case NodeKind::ListItem: {
      y += before(BlockRole::ListItem);
      float start_y = y;
      const float bullet_slot = indent(BlockRole::ListItem);
      const ListSlot shared = list_slot(doc, node);
      const bool foldable = list_item_foldable(doc, id);
      const bool folded = list_item_folded(doc, id, folds_);
      const char* triangle = folded ? "▸" : "▾";
      // Marker: task checkbox, ordered "N.", or unordered "•" (a fold
      // triangle when the item hosts a list)
      std::string marker;
      bool marker_folds = false;
      if (node.label == "task-done")
        marker = "☑";
      else if (node.label == "task")
        marker = "☐";
      else if (node.level > 0)
        marker = std::to_string(node.level) + ".";
      else if (foldable && !shared.fold_column) {
        marker = triangle;
        marker_folds = true;
      } else {
        marker = "•";
      }
      // Fold triangles and cues look like Stretch markers: link colour.
      const Rgba fold_color =
          st.text_color(BlockRole::ListItem, InlineKind::Link);
      Font& marker_font = fonts_.pick(FontRole::Body);
      float baseline = y + static_cast<float>(marker_font.ascender());
      auto emit = [&](const std::string& text, float mx, NodeId owner,
                      const Rgba& color) {
        std::vector<uint32_t> m_cps;
        decode_utf8(text, m_cps);
        for (uint32_t cp : m_cps) {
          const Glyph& g = marker_font.glyph(cp);
          PositionedGlyph pg;
          pg.codepoint = cp;
          pg.x = mx + static_cast<float>(g.bearing_x);
          pg.y = baseline;
          pg.node = owner;
          pg.span_index = 0;
          pg.char_index = 0;
          pg.kind = InlineKind::Text;
          pg.font = FontRole::Body;
          pg.color = color;
          pg.advance = mx + g.advance_px() - pg.x;
          out.glyphs.push_back(pg);
          mx += g.advance_px();
        }
        return mx;
      };
      const float marker_end =
          emit(marker, x, id,
               marker_folds
                   ? fold_color
                   : st.text_color(BlockRole::ListItem, InlineKind::Text));
      // Long ordered lists: grow the slot so "100." etc. never run into
      // the text; sized by the list's widest number so items stay aligned.
      float slot = bullet_slot;
      if (node.level > 0) {
        const int widest = std::max(node.level, shared.max_number);
        float wide = 0.f;
        // Measure with '0's: the widest digit in most faces.
        for (char c : std::to_string(widest) + ".")
          wide += marker_font.glyph(c == '.' ? '.' : '0').advance_px();
        const float gap = marker_font.glyph(' ').advance_px();
        slot = std::max(slot, wide + gap);
      }
      const float line_h = static_cast<float>(marker_font.line_height());
      if (foldable && !shared.fold_column)
        out.fold_marks.push_back({id, x, start_y, slot, line_h});
      if (shared.fold_column) {
        // The triangle sits between the number / task box and the text, past
        // the task box's click column.
        const float gap = marker_font.glyph(' ').advance_px();
        const float tri_x =
            x + std::max({slot, marker_end - x + gap, kTaskColumn});
        const float tri_w =
            std::max(marker_font.glyph(0x25B8).advance_px(),
                     marker_font.glyph(0x25BE).advance_px()) + gap;
        slot = tri_x + tri_w - x;
        if (foldable) {
          emit(triangle, tri_x, INVALID_NODE, fold_color);
          const float left = node.label.empty() ? x : tri_x - gap * 0.5f;
          out.fold_marks.push_back(
              {id, left, start_y, x + slot - left, line_h});
        }
      }
      float text_x = x + slot;
      float text_w = max_width - slot;
      if (text_w < 40.f) {
        text_x = x + 16.f;
        text_w = max_width - 16.f;
      }
      if (folded) {
        // A cue after the text says how much is hidden.
        const size_t hidden = list_item_descendants(doc, id);
        layout_cued_text(doc, node, BlockRole::ListItem, text_x, y, text_w,
                         (node.spans.empty() ? "… " : " … ") +
                             std::to_string(hidden) +
                             (hidden == 1 ? " item" : " items"),
                         id, out);
      } else {
        layout_role_text(doc, node, BlockRole::ListItem, text_x, y, text_w,
                         out);
      }
      // Nested lists under this item
      for (NodeId child : node.children) {
        if (folded) break;
        if (doc.get(child).kind == NodeKind::List) {
          layout_node(doc, child, x + slot, y, max_width - slot, out);
        }
      }
      y += after(BlockRole::ListItem);  // tighter spacing between list items
      out.boxes.push_back({id, x, start_y, max_width, y - start_y});
      break;
    }

    case NodeKind::Page: {
      // Fit the column width; the size is the page's own (never guessed).
      if (node.width <= 0 || node.height <= 0) break;
      y = clear_floats(x, max_width, y);
      const float w = max_width;
      const float h = w * static_cast<float>(node.height) /
                      static_cast<float>(node.width);
      PositionedImage pi{id, x, y, w, h, node.src, image_base_};
      pi.page_w = node.width;
      pi.page_h = node.height;
      out.images.push_back(std::move(pi));
      out.boxes.push_back({id, x, y, w, h});
      y += h + after(BlockRole::Page);
      break;
    }

    case NodeKind::Image: {
      const float em = static_cast<float>(fonts_.regular.pixel_size());
      const float gap = em;
      const float fw = std::min(max_width * 0.45f, 20.f * em);
      if (!in_image_float_ && !in_margin_ &&
          (node.float_side == "left" || node.float_side == "right") &&
          fw >= 6.f * em && max_width - fw - gap >= 10.f * em) {
        const bool right = node.float_side == "right";
        const float bx = right ? x + max_width - fw : x;
        float top = y;
        for (bool moved = true; moved;) {
          moved = false;
          for (const FloatBox& f : floats_) {
            if (f.top <= top && f.bottom > top && f.right > bx && f.left < bx + fw) {
              top = f.bottom;
              moved = true;
            }
          }
        }
        float bottom = top;
        const size_t first_img = out.images.size();
        in_image_float_ = true;
        layout_node(doc, id, bx, bottom, fw, out);
        in_image_float_ = false;
        if (bottom > top) {
          // The image may be narrower than the room reserved for it (a
          // remote thumbnail has a fixed edge): wrap text at what is drawn
          // (left floats; a right float stays against the column edge).
          float used = fw;
          for (size_t k = first_img; k < out.images.size(); ++k)
            if (out.images[k].node == id) used = std::min(fw, out.images[k].width);
          FloatBox fb;
          fb.top = top; fb.bottom = bottom;
          fb.side = right ? FloatSide::Right : FloatSide::Left;
          fb.left = right ? bx - gap : bx;
          fb.right = right ? bx + fw : bx + used + gap;
          floats_.push_back(fb);
        }
        break;
      }
      if (!in_image_float_ && !node.float_side.empty())
        y = clear_floats(x, max_width, y);
      // Consecutive block images share rows like figures.
      if (!in_image_float_ && !in_margin_ && figure_in_row(doc, id) &&
          layout_figure_row(doc, id, x, y, max_width, out))
        break;
      // A block image outside a figure gets the same centred ~60% column.
      const bool block_image = !in_image_float_ && !in_margin_;
      if (block_image) {
        const float cw = std::min(max_width, std::max(max_width * 0.6f, 16.f * em));
        x += (max_width - cw) * 0.5f;
        max_width = cw;
      }
      // Centred as a block image, or in a figure column per the figure
      // style; a floating image stays at its float's edge (text wraps at
      // the width it draws, measured from there).
      const bool center =
          block_image || (in_figure_column_ && figure_centered());
      if (is_remote_image(node.src)) {
        layout_remote_image(doc, node, x, y, max_width, out, center);
        break;
      }
      float disp_w = max_width;
      float disp_h = 120.f;
      if (images_ && !node.src.empty()) {
        const Image& img = images_->get(node.src, image_base_);
        if (img.ok && img.width > 0) {
          float scale = 1.f;
          if (static_cast<float>(img.width) > max_width) {
            scale = max_width / static_cast<float>(img.width);
          }
          float max_h = 480.f;
          if (img.height * scale > max_h) {
            scale = max_h / static_cast<float>(img.height);
          }
          // User interactive scale on top of fit-to-column
          float user = node.display_scale > 0.05f ? node.display_scale : 1.f;
          scale *= user;
          // Keep within column
          if (img.width * scale > max_width) {
            scale = max_width / static_cast<float>(img.width);
          }
          disp_w = img.width * scale;
          disp_h = img.height * scale;
        }
      }
      // Centre the image in its column; the alt text is not shown below
      // it (it stands in for the image only while that is missing).
      const float ix = center ? x + std::max(0.f, (max_width - disp_w) * 0.5f) : x;
      out.images.push_back({id, ix, y, disp_w, disp_h, node.src, image_base_});
      out.boxes.push_back({id, ix, y, disp_w, disp_h});
      y += disp_h + 14.f;
      break;
    }

    case NodeKind::HorizontalRule: {
      float start_y = y;
      y += before(BlockRole::Rule);
      const float thickness = 2.f;
      out.hr_lines.push_back({id, x, y, max_width, thickness});
      y += after(BlockRole::Rule) + thickness;
      out.boxes.push_back({id, x, start_y, max_width, y - start_y});
      break;
    }

    case NodeKind::Transclude: {
      // Live embedding: resolve src as local #anchor or path.md[#anchor]
      // and layout a snapshot of the target's text (glyphs tagged with
      // this Transclude node). Cross-document paths are relative to the
      // host document directory (image_base_).
      y += before(BlockRole::Transclusion);
      float start_y = y;
      const float inset = indent(BlockRole::Transclusion);
      const float bar_w = 3.f;

      std::string path_part, href_part;
      split_include_src(node.src, path_part, href_part);

      const Document* src_doc = &doc;
      bool external = false;
      if (!path_part.empty()) {
        std::filesystem::path resolved = path_part;
        if (!image_base_.empty() && resolved.is_relative())
          resolved = std::filesystem::path(image_base_) / path_part;
        const Document* ext = load_external_doc(resolved.string());
        if (ext) {
          src_doc = ext;
          external = true;
        } else {
          src_doc = nullptr;
        }
      }

      NodeId target_id = INVALID_NODE;
      std::vector<InlineSpan> body_spans;
      if (src_doc) {
        if (!href_part.empty()) {
          target_id = transclude_text_node(
              *src_doc, find_anchor_node(*src_doc, href_part));
          if (target_id != INVALID_NODE)
            body_spans = src_doc->get(target_id).spans;
          // Range/excerpt: keep only the span(s) containing the phrase,
          // or trim text around the first match (semantic range, not bytes).
          if (!body_spans.empty() && !node.summary.empty()) {
            const std::string& ex = node.summary;
            std::vector<InlineSpan> filtered;
            for (const auto& sp : body_spans) {
              if (sp.text.find(ex) != std::string::npos) {
                InlineSpan cut = sp;
                // Prefer exposing the excerpt itself when the span is long
                size_t pos = sp.text.find(ex);
                if (sp.text.size() > ex.size() * 3 && pos != std::string::npos) {
                  size_t start = pos > 40 ? pos - 40 : 0;
                  size_t end = std::min(sp.text.size(), pos + ex.size() + 40);
                  cut.text = sp.text.substr(start, end - start);
                  if (start > 0) cut.text = "…" + cut.text;
                  if (end < sp.text.size()) cut.text += "…";
                }
                filtered.push_back(std::move(cut));
              }
            }
            if (!filtered.empty()) body_spans = std::move(filtered);
          }
        } else if (external) {
          // Whole-document include: concatenate first few text nodes.
          std::vector<NodeId> stack = {src_doc->root};
          int taken = 0;
          while (!stack.empty() && taken < 8) {
            NodeId cur = stack.back();
            stack.pop_back();
            if (cur == INVALID_NODE) continue;
            const Node& cn = src_doc->get(cur);
            if (cn.kind == NodeKind::Paragraph || cn.kind == NodeKind::Heading ||
                cn.kind == NodeKind::Header ||
                cn.kind == NodeKind::ListItem || cn.kind == NodeKind::BlockQuote) {
              if (!body_spans.empty()) {
                InlineSpan gap;
                gap.kind = InlineKind::Text;
                gap.text = " ";
                body_spans.push_back(std::move(gap));
              }
              for (const auto& sp : cn.spans) body_spans.push_back(sp);
              ++taken;
            }
            for (auto it = cn.children.rbegin(); it != cn.children.rend(); ++it)
              stack.push_back(*it);
          }
        }
      }

      std::string label_href = node.src.empty() ? std::string{"(no target)"}
                                                : node.src;

      // Label line (always tagged with the Transclude node so clicks can
      // follow the include and structure highlight marks the embed).
      {
        Node label;
        label.id = id;
        label.kind = NodeKind::Paragraph;
        InlineSpan s;
        s.kind = InlineKind::Emphasis;
        s.text = "↗ ";
        s.text += label_href;
        label.spans.push_back(std::move(s));
        layout_role_text(doc, label, BlockRole::Transclusion, x + bar_w + inset,
                         y, max_width - bar_w - inset, out);
        y += 4.f;
      }

      // Body: copy of target spans, or a missing marker.
      // Same-document includes tag glyphs with the *source* node id so
      // caret/edit hit-tests write through to the live passage. External
      // includes stay tagged with the Transclude node (read-only mirror).
      {
        Node body;
        body.kind = NodeKind::Paragraph;
        const bool local_writable =
            !external && target_id != INVALID_NODE && src_doc == &doc;
        body.id = local_writable ? target_id : id;
        if (!body_spans.empty()) {
          body.spans = std::move(body_spans);
        } else {
          InlineSpan s;
          s.kind = InlineKind::Emphasis;
          if (!src_doc && !path_part.empty())
            s.text = "[missing file: " + path_part + "]";
          else
            s.text = "[missing transclusion]";
          body.spans.push_back(std::move(s));
          body.id = id;  // missing marker is not writable
        }
        layout_role_text(doc, body, BlockRole::Transclusion, x + bar_w + inset,
                         y, max_width - bar_w - inset, out);
      }
      y += after(BlockRole::Transclusion);
      out.quote_bars.push_back({id, x, start_y, bar_w, y - start_y});
      out.boxes.push_back({id, x, start_y, max_width, y - start_y});
      break;
    }

    default:
      break;
  }
}

// The codepoint at byte `i` of `text`; advances `i` past it. Invalid bytes
// are skipped (they are no character). False at the end.
static bool next_utf8(const std::string& text, size_t& i, uint32_t& cp) {
  while (i < text.size()) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) {
      cp = c;
      ++i;
    } else if ((c & 0xe0) == 0xc0 && i + 1 < text.size()) {
      cp = ((c & 0x1f) << 6) | (text[i + 1] & 0x3f);
      i += 2;
    } else if ((c & 0xf0) == 0xe0 && i + 2 < text.size()) {
      cp = ((c & 0x0f) << 12) | ((text[i + 1] & 0x3f) << 6) | (text[i + 2] & 0x3f);
      i += 3;
    } else if ((c & 0xf8) == 0xf0 && i + 3 < text.size()) {
      cp = ((c & 0x07) << 18) | ((text[i + 1] & 0x3f) << 12) |
           ((text[i + 2] & 0x3f) << 6) | (text[i + 3] & 0x3f);
      i += 4;
    } else {
      ++i;
      continue;
    }
    return true;
  }
  return false;
}

static void decode_utf8(const std::string& text,
                        std::vector<uint32_t>& out) {
  size_t i = 0;
  uint32_t cp = 0;
  while (next_utf8(text, i, cp)) out.push_back(cp);
}

void LayoutEngine::layout_aside(const Document& doc, const Node& node,
                                float x, float& y, float max_width,
                                LayoutResult& out) {
  const BlockStyle& bs = (*style_)[BlockRole::Aside];
  const float em = static_cast<float>(fonts_.regular.pixel_size());
  const float dp = options_.px_scale;
  const float pad = bs.indent.px(em, dp);
  const float gap = em;
  FloatSide side = bs.side;
  if (node.src == "left") side = FloatSide::Left;
  else if (node.src == "right") side = FloatSide::Right;
  if (bs.placement == Placement::Margin && side != FloatSide::None &&
      !in_margin_ && layout_margin_box(doc, node, BlockRole::Aside, side, {},
                                       y, out))
    return;
  // In the column: small text and the aside colour all the same.
  const bool was_small = small_;
  const BlockRole was_ctx = ctx_role_;
  small_ = is_small_role(bs.font);
  ctx_role_ = BlockRole::Aside;
  struct Restore {
    bool& s; BlockRole& c; bool sv; BlockRole cv;
    ~Restore() { s = sv; c = cv; }
  } restore{small_, ctx_role_, was_small, was_ctx};
  float want = bs.width.px(em, dp);
  if (want < 1.f) want = 16.f * em;
  const float aw = std::min(want, max_width * 0.45f);
  // Margin placement falls back to a float inside the existing text column
  // when the page margin has no space. Neither mode may shrink the column to
  // manufacture a wider margin for the aside.
  const bool floating = (bs.placement == Placement::Float ||
                         bs.placement == Placement::Margin) &&
                        side != FloatSide::None && !in_margin_ &&
                        aw >= 6.f * em && max_width - aw - gap >= 10.f * em;

  auto frame = [&](float bx, float top, float w, float bottom) {
    const LayoutBox box{node.id, bx, top, w, bottom - top};
    if (bs.background.visible()) out.role_fills.push_back({box, BlockRole::Aside});
    if (bs.border.visible()) out.role_frames.push_back({box, BlockRole::Aside});
  };

  if (!floating) {
    y = clear_floats(x, max_width, y);
    y += bs.space_before.px(em, dp);
    const float top = y;
    const size_t fill_at = out.role_fills.size();
    float iy = top + pad;
    layout_children_sequential(doc, node.children, x + pad, iy,
                               std::max(40.f, max_width - 2.f * pad), out);
    const float bottom = iy + pad;
    frame(x, top, max_width, bottom);
    // The fill goes under the children laid out before it.
    if (bs.background.visible())
      std::rotate(out.role_fills.begin() + static_cast<std::ptrdiff_t>(fill_at),
                  out.role_fills.end() - 1, out.role_fills.end());
    out.boxes.push_back({node.id, x, top, max_width, bottom - top});
    y = bottom + bs.space_after.px(em, dp);
    return;
  }

  float top = y + bs.space_before.px(em, dp);
  const float bx = side == FloatSide::Right ? x + max_width - aw : x;
  // Below earlier boxes on the same spot (stacked asides).
  for (bool moved = true; moved;) {
    moved = false;
    for (const FloatBox& f : floats_) {
      if (f.top <= top && f.bottom > top && f.right > bx && f.left < bx + aw) {
        top = f.bottom;
        moved = true;
      }
    }
  }
  const size_t fill_at = out.role_fills.size();
  float iy = top + pad;
  layout_children_sequential(doc, node.children, bx + pad, iy,
                             std::max(20.f, aw - 2.f * pad), out);
  // Trailing gap of the last child is padding enough.
  const float bottom = iy + pad * 0.5f;
  frame(bx, top, aw, bottom);
  if (bs.background.visible())
    std::rotate(out.role_fills.begin() + static_cast<std::ptrdiff_t>(fill_at),
                out.role_fills.end() - 1, out.role_fills.end());
  FloatBox fb;
  fb.top = top;
  fb.bottom = bottom + bs.space_after.px(em, dp);
  fb.side = side;
  fb.left = side == FloatSide::Right ? bx - gap : bx;
  fb.right = side == FloatSide::Right ? bx + aw : bx + aw + gap;
  floats_.push_back(fb);
  out.boxes.push_back({node.id, bx, top, aw, bottom - top});
  // y stays: the following blocks run beside the box.
}

// Margin room for side boxes of `doc`: use only the margin left by the view
// settings / text measure. Never take width from the text column for asides.
void LayoutEngine::setup_margins(const Document& doc, float mx,
                                 float content_w, float max_width) {
  margins_[0] = margins_[1] = MarginArea{};
  in_margin_ = 0;
  small_ = false;
  ctx_role_ = BlockRole::Count;
  const DocStyle& st = *style_;
  const float em = static_cast<float>(fonts_.regular.pixel_size());
  const float dp = options_.px_scale;
  float want[2] = {0.f, 0.f};
  auto need = [&](const BlockStyle& bs, FloatSide side) {
    if (bs.placement != Placement::Margin || side == FloatSide::None) return;
    float w = bs.width.px(em, dp);
    if (w < 1.f) w = 14.f * em;
    float& slot = want[side == FloatSide::Left ? 0 : 1];
    slot = std::max(slot, w);
  };
  for (const Node& n : doc.nodes) {
    if (n.parent == INVALID_NODE) continue;  // unhooked arena entries
    if (n.kind == NodeKind::Aside) {
      DocStyle scoped = st;
      // The margin prepass must agree with the actual, possibly nested,
      // variant scope used when laying out this aside.
      std::vector<NodeId> ancestors;
      for (NodeId id = n.id; id != INVALID_NODE; id = doc.get(id).parent)
        ancestors.push_back(id);
      for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
        const std::string& name = doc.get(*it).style;
        if (!name.empty()) apply_style_variant(scoped, name);
      }
      const BlockStyle& bs = scoped[BlockRole::Aside];
      FloatSide side = bs.side;
      if (n.src == "left") side = FloatSide::Left;
      else if (n.src == "right") side = FloatSide::Right;
      need(bs, side);
    }
  }
  if (want[0] <= 0.f && want[1] <= 0.f) return;
  const float gap = 1.25f * em;  // margin box ↔ text column
  const float edge = std::max(options_.margin_x, 0.5f * em);
  const float min_box = 8.f * em;    // below this, fall back inside the column
  for (int s = 0; s < 2; ++s) {
    if (want[s] <= 0.f) continue;
    const float free = s == 0 ? mx - edge - gap
                              : max_width - (mx + content_w) - edge - gap;
    if (free < min_box) continue;
    const float w = std::min(free, want[s]);
    MarginArea& m = margins_[s];
    m.w = w;
    m.x = s == 0 ? mx - gap - w : mx + content_w + gap;
  }
}

bool LayoutEngine::layout_margin_box(const Document& doc, const Node& node,
                                     BlockRole role, FloatSide side,
                                     const std::string& label, float y,
                                     LayoutResult& out) {
  MarginArea& m = margins_[side == FloatSide::Left ? 0 : 1];
  if (m.w <= 0.f) return false;
  const BlockStyle& bs = (*style_)[role];
  const float em = static_cast<float>(fonts_.regular.pixel_size());
  const float dp = options_.px_scale;
  const float pad = bs.indent.px(em, dp);
  // Beside its flow position, below the boxes already in this margin.
  const float top = std::max(y + bs.space_before.px(em, dp), m.bottom);

  const bool was_small = small_;
  const BlockRole was_ctx = ctx_role_;
  small_ = is_small_role(bs.font);
  ctx_role_ = role;
  ++in_margin_;
  const size_t fill_at = out.role_fills.size();
  const float ix = m.x + pad;
  const float iw = std::max(20.f, m.w - 2.f * pad);
  float iy = top + pad;
  if (!label.empty()) {
    // Decoration: no node, so it is neither text to edit nor a link.
    Node head;
    head.id = INVALID_NODE;
    head.kind = NodeKind::Paragraph;
    InlineSpan sp;
    sp.kind = InlineKind::Strong;
    sp.text = label;
    head.spans.push_back(std::move(sp));
    layout_paragraph(doc, head, ix, iy, iw, out, FontRole::Body, role);
    iy += 0.3f * em;
  }
  layout_children_sequential(doc, node.children, ix, iy, iw, out);
  --in_margin_;
  small_ = was_small;
  ctx_role_ = was_ctx;
  // The padding replaces the trailing gap of a last paragraph.
  if (!node.children.empty() && node.children.back() != INVALID_NODE &&
      doc.get(node.children.back()).kind == NodeKind::Paragraph)
    iy -= std::min((*style_)[BlockRole::Paragraph].space_after.px(em, dp),
                   iy - top);
  const float bottom = iy + pad;
  const LayoutBox box{node.id, m.x, top, m.w, bottom - top};
  if (bs.background.visible()) {
    out.role_fills.push_back({box, role});
    // Under the text laid out before it.
    std::rotate(out.role_fills.begin() + static_cast<std::ptrdiff_t>(fill_at),
                out.role_fills.end() - 1, out.role_fills.end());
  }
  if (bs.border.visible()) {
    if (bs.background.visible()) {
      out.role_frames.push_back({box, role});
    } else {
      // A bar on the side facing the text.
      const float bar = std::max(2.f, 2.f * dp);
      const float bx = side == FloatSide::Left ? m.x + m.w - bar : m.x;
      out.role_rules.push_back({{node.id, bx, top, bar, bottom - top}, role});
    }
  }
  out.boxes.push_back(box);
  m.bottom = bottom + bs.space_after.px(em, dp);
  return true;
}

void LayoutEngine::inline_image_size(const InlineSpan& sp, float max_w,
                                     float& w, float& h, bool& remote) {
  const float dp = options_.px_scale;
  const float em = static_cast<float>(fonts_.regular.pixel_size());
  remote = is_remote_image(sp.src);
  float nat_w = 0.f, nat_h = 0.f;  // natural size in CSS px
  if (images_ && !sp.src.empty()) {
    const Image& img = remote
                           ? images_->thumb(sp.src, image_base_, kInlineThumbEdge)
                           : images_->get(sp.src, image_base_);
    if (img.ok && img.width > 0 && img.height > 0) {
      nat_w = static_cast<float>(img.width);
      nat_h = static_cast<float>(img.height);
    }
  }
  if (sp.width > 0 && sp.height > 0) {
    w = sp.width * dp;
    h = sp.height * dp;
  } else if (sp.width > 0 || sp.height > 0) {
    const float aspect = nat_w > 0.f ? nat_h / nat_w : 1.f;
    w = sp.width > 0 ? sp.width * dp : sp.height * dp / aspect;
    h = sp.height > 0 ? sp.height * dp : sp.width * dp * aspect;
  } else if (nat_w > 0.f) {
    w = nat_w * dp;
    h = nat_h * dp;
  } else {
    w = h = em;  // pending / missing: a box the size of a letter
  }
  if (w > max_w && w > 0.f) {
    h *= max_w / w;
    w = max_w;
  }
  w = std::max(1.f, w);
  h = std::max(1.f, h);
}

// Pictures for text_image_urls under their paragraph: remote thumbnails
// side by side, wrapping at the column edge; ones that failed to load are
// left out (the address stays a link). A click follows the address.
void LayoutEngine::layout_text_images(const std::vector<TextImageUrl>& urls,
                                      const Node& node, float x, float& y,
                                      float max_width, LayoutResult& out) {
  const float dp = options_.px_scale;
  const float gap = 6.f * dp;
  const float max_h = 360.f * dp;
  float cx = x;
  float row_h = 0.f;
  bool any = false;
  for (const TextImageUrl& t : urls) {
    float w = std::min(max_width, 320.f * dp);
    float h = 180.f * dp;
    if (images_) {
      const Image& img = images_->thumb(t.url, image_base_, kInlineThumbEdge);
      if (!img.ok && !img.pending) continue;
      if (img.ok && img.width > 0 && img.height > 0) {
        const float sc = std::min({dp, max_width / img.width,
                                   max_h / img.height});
        w = img.width * sc;
        h = img.height * sc;
      }
    }
    if (!any) y += gap;
    if (cx > x && cx + w > x + max_width) {
      y += row_h + gap;
      cx = x;
      row_h = 0.f;
    }
    PositionedImage im{node.id, cx, y, w, h, t.url, image_base_};
    im.thumb = kInlineThumbEdge;
    im.in_text = true;
    im.span_index = t.span;
    if (!t.link) {
      out.auto_links.push_back(t.url);
      im.auto_link = static_cast<uint32_t>(out.auto_links.size());
    }
    out.images.push_back(std::move(im));
    cx += w + gap;
    row_h = std::max(row_h, h);
    any = true;
  }
  if (any) y += row_h + gap;
}

void LayoutEngine::layout_chrome_notice(const Document& doc, float x,
                                        float& y, float max_width,
                                        LayoutResult& out) {
  const std::string what = chrome_hidden_text(doc, chrome_regions(doc));
  if (what.empty()) return;
  // Laid out as the root's text, then made decoration: not editable, and
  // the click goes through out.chrome_notice.
  Node notice;
  notice.id = doc.root;
  notice.kind = NodeKind::Paragraph;
  InlineSpan lead;
  lead.kind = InlineKind::Emphasis;
  lead.text = "Reader mode: " + what + " · ";
  notice.spans.push_back(std::move(lead));
  InlineSpan show;
  show.kind = InlineKind::Link;
  show.text = "Show all";
  notice.spans.push_back(std::move(show));
  const DocStyle& st = *style_;
  const float em = static_cast<float>(fonts_.regular.pixel_size());
  const float dp = options_.px_scale;
  const float start_y = y;
  y += st[BlockRole::Paragraph].space_before.px(em, dp);
  const size_t first = out.glyphs.size();
  layout_role_text(doc, notice, BlockRole::Paragraph, x, y, max_width, out);
  for (size_t i = first; i < out.glyphs.size(); ++i)
    if (out.glyphs[i].node == doc.root) out.glyphs[i].node = INVALID_NODE;
  y += st[BlockRole::Paragraph].space_after.px(em, dp);
  out.chrome_notice = {INVALID_NODE, x, start_y, max_width, y - start_y};
}

std::shared_ptr<const MathBox> LayoutEngine::math_box(const InlineSpan& sp) {
  MathFont* face = fonts_.math_font();
  if (!face) return nullptr;
  const std::string key = (sp.display ? "D" : "T") + sp.src;
  auto it = math_cache_.find(key);
  if (it != math_cache_.end()) return it->second;
  const MathParse parsed = parse_math(sp.src);
  std::shared_ptr<const MathBox> box;
  // Unknown commands stay visible as text inside the formula; only a source
  // that does not parse at all falls back to showing the TeX.
  if (parsed.error.empty() || !parsed.root.kids.empty())
    box = std::make_shared<MathBox>(layout_math(parsed.root, *face, sp.display));
  if (math_cache_.size() > 2000) math_cache_.clear();
  math_cache_[key] = box;
  return box;
}

void LayoutEngine::layout_role_text(const Document& doc, const Node& node,
                                    BlockRole role, float x, float& y,
                                    float max_width, LayoutResult& out) {
  layout_paragraph(doc, node, x, y, max_width, out, (*style_)[role].font, role);
}

void LayoutEngine::layout_cued_text(const Document& doc, const Node& node,
                                    BlockRole role, float x, float& y,
                                    float max_width, const std::string& cue,
                                    NodeId fold, LayoutResult& out) {
  // Decoration like a Header's tags: caret and edits stay on the node's own
  // text.
  const size_t glyph0 = out.glyphs.size();
  const size_t cue_span = node.spans.size();
  Node vis = node;
  InlineSpan sp;
  sp.text = cue;
  vis.spans.push_back(std::move(sp));
  layout_role_text(doc, vis, role, x, y, max_width, out);
  const Rgba color = style_->text_color(role, InlineKind::Link);
  Font& font = fonts_.pick((*style_)[role].font);
  LayoutBox line;
  line.node = fold;
  auto flush = [&] {
    if (line.width > 0.f) out.fold_marks.push_back(line);
    line = {};
    line.node = fold;
  };
  float line_base = 0.f;
  for (size_t gi = glyph0; gi < out.glyphs.size(); ++gi) {
    PositionedGlyph& g = out.glyphs[gi];
    if (g.node != node.id || g.span_index != cue_span) continue;
    g.node = INVALID_NODE;
    g.color = color;
    if (line.width > 0.f && std::abs(g.y - line_base) > 0.5f) flush();
    if (line.width <= 0.f) {
      line_base = g.y;
      line.x = g.x;
      line.y = g.y - static_cast<float>(font.ascender());
      line.height = static_cast<float>(font.line_height());
    }
    line.width = std::max(line.width, g.x + g.advance - line.x);
  }
  flush();
}

void LayoutEngine::layout_fold_line(const Document& doc, const Node& stretch,
                                    const std::string& text, NodeId glyph_node,
                                    float x, float& y, float max_width,
                                    LayoutResult& out) {
  const size_t glyph0 = out.glyphs.size();
  Node fake = stretch;  // its glyphs carry the stretch's id
  fake.kind = NodeKind::Paragraph;
  fake.spans.clear();
  InlineSpan s;
  // The stretch's own line looks like a link; a cue takes just the colour,
  // as in layout_cued_text.
  const bool cue = glyph_node == INVALID_NODE;
  s.kind = cue ? InlineKind::Text : InlineKind::Link;
  s.text = text;
  fake.spans.push_back(std::move(s));
  const float top = y;
  layout_role_text(doc, fake, BlockRole::Paragraph, x, y, max_width, out);
  float band_x = x, band_w = max_width;
  float_band(top, y, band_x, band_w);
  // A cue takes just its text; the stretch's own line its whole width.
  if (cue) {
    const Rgba color =
        style_->text_color(BlockRole::Paragraph, InlineKind::Link);
    float right = band_x;
    for (size_t gi = glyph0; gi < out.glyphs.size(); ++gi) {
      PositionedGlyph& g = out.glyphs[gi];
      g.node = INVALID_NODE;
      g.color = color;
      right = std::max(right, g.x + g.advance);
    }
    band_w = right - band_x;
  }
  if (band_w > 0.f)
    out.fold_marks.push_back({stretch.id, band_x, top, band_w, y - top});
}

void LayoutEngine::layout_stretch(const Document& doc, const Node& node,
                                  float x, float& y, float max_width,
                                  LayoutResult& out) {
  const DocStyle& st = *style_;
  const float em = static_cast<float>(fonts_.regular.pixel_size());
  const float gap = st[BlockRole::Paragraph].space_after.px(
      em, options_.px_scale);
  const NodeId id = node.id;
  const NodeId summary = stretch_summary(doc, id);
  const bool folded = stretch_folded(doc, id, folds_);
  const float start_y = y;
  const size_t first_float = floats_.size();
  const std::string label =
      (folded ? "▸ " : "▾ ") + (node.summary.empty() ? "…" : node.summary);
  std::vector<NodeId> body;
  for (NodeId c : node.children)
    if (c != summary) body.push_back(c);

  if (summary == INVALID_NODE) {
    // Classic stretchtext: the label stands for the text.
    if (folded || !node.summary.empty()) {
      layout_fold_line(doc, node, label, id, x, y, max_width, out);
      if (folded) y += gap;
    }
    if (!folded) {
      const float inset = 0.75f * em;
      layout_children_sequential(doc, body, x + inset, y, max_width - inset,
                                 out);
    }
  } else if (folded) {
    if (!node.summary.empty())
      layout_fold_line(doc, node, label, id, x, y, max_width, out);
    const std::vector<NodeId> blocks = doc.get(summary).children;
    const std::string cue = "▸ Full Text";
    const NodeId last = blocks.empty() ? INVALID_NODE : blocks.back();
    const bool in_text = last != INVALID_NODE &&
                         doc.get(last).kind == NodeKind::Paragraph;
    const FoldCue outer = cue_;
    cue_ = in_text ? FoldCue{last, id, cue} : FoldCue{};
    layout_children_sequential(doc, blocks, x, y, max_width, out);
    cue_ = outer;
    if (!in_text) {
      layout_fold_line(doc, node, cue, INVALID_NODE, x, y, max_width, out);
      y += gap;
    }
    contain_floats(first_float, y);
  } else {
    const std::string cue = "▴ Summary";
    layout_fold_line(doc, node, node.summary.empty() ? cue : label,
                     node.summary.empty() ? INVALID_NODE : id, x, y, max_width,
                     out);
    y += gap;
    layout_children_sequential(doc, body, x, y, max_width, out);
    contain_floats(first_float, y);
    layout_fold_line(doc, node, cue, INVALID_NODE, x, y, max_width, out);
    y += gap;
  }
  // The whole extent, so the box also stands for the block's height.
  out.boxes.push_back({id, x, start_y, max_width, y - start_y});
}

void LayoutEngine::add_role_band(BlockRole role, NodeId id, float x,
                                 float top, float width, float bottom,
                                 LayoutResult& out) {
  const BlockStyle& bs = (*style_)[role];
  const float em = static_cast<float>(fonts_.regular.pixel_size());
  const float dp = options_.px_scale;
  // The band reaches a little past the text so it does not touch the glyphs.
  const float pad_x = 0.35f * em;
  const float pad_y = 0.2f * em;
  if (bs.background.visible())
    out.role_fills.push_back({{id, x - pad_x, top - pad_y, width + 2.f * pad_x,
                               bottom - top + 2.f * pad_y},
                              role});
  if (bs.border.visible())
    out.role_rules.push_back(
        {{id, x, bottom + pad_y, width, std::max(1.f, dp)}, role});
}

void LayoutEngine::layout_paragraph(const Document& doc, const Node& node,
                                    float x, float& y, float max_width,
                                    LayoutResult& out, FontRole base_role,
                                    BlockRole block) {
  (void)doc;
  if (small_) base_role = small_role(base_role);
  // Text inside a side box takes the box's colour unless its own block or
  // inline role has one.
  const bool ctx_color = ctx_role_ != BlockRole::Count &&
                         (*style_)[ctx_role_].has_color &&
                         !(*style_)[block].has_color;
  // An inline image or formula: one CharInfo of this size.
  struct Atom {
    float w = 0.f;  // advance
    float h = 0.f;  // above the baseline
    float d = 0.f;  // below the baseline (formulas)
    bool remote = false;
    float math_px = 0.f;
    bool display = false;
    std::shared_ptr<const MathBox> math = nullptr;
  };
  // One codepoint (or atom) of the paragraph. Kept small: a huge block
  // holds one per character while its lines are broken.
  struct CharInfo {
    uint32_t cp;
    uint32_t span_idx;
    uint32_t char_idx;
    InlineKind kind;
    FontRole role;
    uint32_t auto_link = 0;  // see PositionedGlyph::auto_link
    float rise = 0.f;  // baseline lift (footnote marks without superscripts)
    uint32_t atom = 0;  // 1 + index into `atoms`; 0 = text
  };
  std::vector<Atom> atoms;
  std::vector<CharInfo> chars;
  auto atom_of = [&](const CharInfo& c) -> const Atom* {
    return c.atom ? &atoms[c.atom - 1] : nullptr;
  };
  // Advance of an atom; 0 for text (and for an image without a size,
  // which is laid out as its placeholder glyph).
  auto atom_w = [&](const CharInfo& c) {
    return c.atom ? atoms[c.atom - 1].w : 0.f;
  };
  const bool small_base = is_small_role(base_role);
  // Font of a span's text (its inline style and metadata may override).
  auto span_role = [&](const InlineSpan& span) {
    FontRole r = style_role(base_role, span.kind, span.marks);
    const InlineStyle& inl = (*style_)[span.kind];
    if (inl.has_font) r = inl.font;
    if (span.role == InlineRole::Metadata && style_->metadata.has_font)
      r = style_->metadata.font;
    return r;
  };

  // ---- Line table (LineTable in layout.hpp) ----
  // Plain text without floats beside it, while virtualizing: every
  // character is one CharInfo with char_idx = its index in the span, and
  // lines do not depend on the block's y. Math, inline images and footnote
  // marks change that, so such blocks are always broken in full.
  const bool cull_lines = viewport_.active();
  bool plain_text = cull_lines && floats_.empty();
  for (const InlineSpan& sp : node.spans)
    if (sp.kind == InlineKind::Math || sp.kind == InlineKind::Image ||
        sp.kind == InlineKind::FootnoteRef)
      plain_text = false;
  // Pictures of image addresses in the text, under the lines (this layout
  // only; the document keeps its text).
  std::vector<TextImageUrl> text_images;
  if (options_.auto_images && base_role != FontRole::Code &&
      base_role != FontRole::SmallCode && block != BlockRole::CodeBlock)
    text_images = text_image_urls(node);
  if (!text_images.empty()) plain_text = false;
  auto fingerprint = [&]() {
    uint64_t h = 0xcbf29ce484222325ull;
    auto mix = [&](uint64_t v) {
      h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    };
    auto mix_f = [&](float f) {
      uint32_t b = 0;
      std::memcpy(&b, &f, sizeof b);
      mix(b);
    };
    mix_f(x);
    mix_f(max_width);
    mix_f(options_.line_spacing);
    mix(options_.auto_links);
    mix(options_.auto_images);
    mix(static_cast<uint64_t>(base_role));
    mix(static_cast<uint64_t>(block));
    for (const InlineSpan& sp : node.spans) {
      const FontRole r = span_role(sp);
      const Font& f = fonts_.pick(r);
      mix(static_cast<uint64_t>(sp.kind));
      mix(static_cast<uint64_t>(r));
      mix(static_cast<uint64_t>(f.pixel_size()));
      mix(std::hash<std::string>{}(f.path()));
      mix(std::hash<std::string>{}(sp.text));
    }
    return h;
  };
  const std::pair<const Document*, NodeId> table_key{&doc, node.id};
  const LineTable* table = nullptr;  // valid table of this block
  uint64_t print = 0;
  if (plain_text) {
    auto it = line_tables_.find(table_key);
    if (it != line_tables_.end()) {
      print = fingerprint();
      if (it->second.fingerprint == print) {
        it->second.used = ++line_table_clock_;
        table = &it->second;
      }
    }
  }
  // With a table: only chars[flat0, flat_end) of the block are built and
  // broken, from the line start `first` up to the start of the line after
  // `stop` (the first line below the band); lines before `first` and from
  // `stop` on come from the table.
  const float y0 = y;
  size_t first = 0;  // into table->lines
  size_t stop = 0;   // into table->lines; == size(): to the block's end
  size_t flat0 = 0;
  size_t flat_end = std::numeric_limits<size_t>::max();
  size_t emit_end = std::numeric_limits<size_t>::max();  // flat
  if (table) {
    const auto& L = table->lines;
    const float band0 = viewport_.top - viewport_.overscan;
    const float band1 = viewport_.bottom + viewport_.overscan;
    const Font& m = fonts_.pick(base_role == FontRole::Heading ? FontRole::Heading
                                : small_base                  ? FontRole::Small
                                                              : FontRole::Body);
    const float advance = m.line_height() * options_.line_spacing;
    stop = L.size();
    for (size_t i = 0; i < L.size(); ++i) {
      if (!L[i].start) continue;
      if (y0 + L[i].dy + advance < band0) first = i;
      if (y0 + L[i].dy - static_cast<float>(m.ascender()) > band1) {
        stop = i;
        break;
      }
    }
    if (!L[first].start) first = 0;
    flat0 = L[first].flat;
    if (stop < L.size()) {
      emit_end = L[stop].flat;
      for (size_t i = stop + 1; i < L.size(); ++i)
        if (L[i].start) {
          flat_end = L[i].flat;
          break;
        }
    }
  }

  std::vector<uint32_t> cps;
  const size_t first_span = table ? table->lines[first].span : 0;
  for (size_t si = first_span; si < node.spans.size(); ++si) {
    if (flat0 + chars.size() >= flat_end) break;
    const InlineSpan& span = node.spans[si];
    const auto si32 = static_cast<uint32_t>(si);
    // A table range starts inside its first span.
    const bool from_table = table && si == first_span;
    const uint32_t ch0 = from_table ? table->lines[first].ch : 0;
    size_t byte = from_table ? table->lines[first].byte : 0;
    cps.clear();
    for (uint32_t cp = 0; flat0 + chars.size() + cps.size() < flat_end &&
                          next_utf8(span.text, byte, cp);)
      cps.push_back(cp);
    const InlineKind kind = span.kind;
    const FontRole r = span_role(span);
    if (kind == InlineKind::Math) {
      // One atom set from the font's math table; without a math font (or a
      // formula that does not parse) the TeX source is shown as code.
      const InlineSpan& sp = node.spans[si];
      const float em = static_cast<float>(fonts_.pick(r).pixel_size());
      if (auto box = math_box(sp)) {
        Atom a;
        a.w = std::max(1.f, box->width * em);
        a.h = box->ascent * em;
        a.d = box->depth * em;
        a.math_px = em;
        a.display = sp.display;
        a.math = std::move(box);
        atoms.push_back(std::move(a));
        CharInfo c{kInlineImageCp, si32, 0, kind, r};
        c.atom = static_cast<uint32_t>(atoms.size());
        chars.push_back(c);
      } else {
        const FontRole code = small_base ? FontRole::SmallCode : FontRole::Code;
        std::vector<uint32_t> src;
        decode_utf8(sp.src, src);
        for (uint32_t cp : src) chars.push_back(CharInfo{cp, si32, 0, kind, code});
      }
      continue;
    }
    // Adjacent footnote marks ("[^1][^2]") get a raised comma between them
    // so they do not read as one number. Display only: it shares the next
    // mark's first char index, like the caret slot before that mark.
    if (kind == InlineKind::FootnoteRef && si > 0 && !cps.empty() &&
        node.spans[si - 1].kind == InlineKind::FootnoteRef && ch0 == 0) {
      CharInfo c{',', si32, 0, kind, r};
      // Sits on the superscript digits' baseline, or on the raised digits'.
      Font& f = fonts_.pick(r);
      const uint32_t sup = superscript_digit(cps[0]);
      c.rise = static_cast<float>(f.ascender()) *
               (sup && f.has_glyph(sup) ? 0.25f : 0.4f);
      chars.push_back(c);
    }
    for (size_t ci = 0; ci < cps.size(); ++ci) {
      CharInfo c{cps[ci], si32, static_cast<uint32_t>(ch0 + ci), kind, r};
      if (kind == InlineKind::FootnoteRef) {
        // Footnote mark: superscript digit when the face has one (same
        // char index, so caret / selection are unaffected), else raised.
        Font& f = fonts_.pick(r);
        const uint32_t sup = superscript_digit(c.cp);
        if (sup && f.has_glyph(sup))
          c.cp = sup;
        else
          c.rise = static_cast<float>(f.ascender()) * 0.4f;
      } else if (kind == InlineKind::Image && c.cp == kInlineImageCp) {
        Atom a;
        inline_image_size(node.spans[si], max_width, a.w, a.h, a.remote);
        atoms.push_back(std::move(a));
        c.atom = static_cast<uint32_t>(atoms.size());
      }
      chars.push_back(c);
    }
  }

  // Plain-text URLs become links for this layout only (the span stays Text).
  // A table has them for the whole block (a URL may start before chars).
  std::vector<LineTable::Link> found_links;
  if (table) {
    for (const LineTable::Link& l : table->links) {
      const size_t b = std::max(l.flat_b, static_cast<uint32_t>(flat0));
      const size_t e = std::min<size_t>(l.flat_e, flat0 + chars.size());
      if (b >= e) continue;
      out.auto_links.push_back(l.href);
      const uint32_t id = static_cast<uint32_t>(out.auto_links.size());
      for (size_t i = b; i < e; ++i) {
        chars[i - flat0].kind = InlineKind::Link;
        chars[i - flat0].auto_link = id;
      }
    }
  } else if (options_.auto_links && base_role != FontRole::Code &&
             base_role != FontRole::SmallCode) {
    for (size_t lo = 0; lo < chars.size();) {
      size_t hi = lo;
      while (hi < chars.size() && chars[hi].span_idx == chars[lo].span_idx) ++hi;
      const InlineKind k = chars[lo].kind;
      if (k == InlineKind::Text || k == InlineKind::Emphasis ||
          k == InlineKind::Strong || k == InlineKind::Strike) {
        std::vector<uint32_t> cps;
        cps.reserve(hi - lo);
        for (size_t i = lo; i < hi; ++i) cps.push_back(chars[i].cp);
        for (const auto& [b, e] : find_urls(cps)) {
          std::string href;
          for (size_t i = b; i < e; ++i) autolink_append_utf8(href, cps[i]);
          if (plain_text)
            found_links.push_back({static_cast<uint32_t>(lo + b),
                                   static_cast<uint32_t>(lo + e), href});
          out.auto_links.push_back(std::move(href));
          const uint32_t id = static_cast<uint32_t>(out.auto_links.size());
          for (size_t i = lo + b; i < lo + e; ++i) {
            chars[i].kind = InlineKind::Link;
            chars[i].auto_link = id;
          }
        }
      }
      lo = hi;
    }
  }

  Font& metrics = fonts_.pick(base_role == FontRole::Heading ? FontRole::Heading
                              : small_base                  ? FontRole::Small
                                                            : FontRole::Body);
  if (chars.empty()) {
    y += metrics.line_height();
    return;
  }

  float baseline = y + metrics.ascender();
  if (table && flat0 > 0) baseline = y0 + table->lines[first].dy;
  // The current line: left edge and width, narrowed beside floating boxes.
  float lx0 = x;
  float lw = max_width;
  auto place_line = [&]() {
    for (;;) {
      lx0 = x;
      lw = max_width;
      if (floats_.empty()) return;
      const float top = baseline - metrics.ascender();
      const float bottom = top + metrics.line_height();
      float_band(top, bottom, lx0, lw);
      const float min_width = std::min(max_width,
          static_cast<float>(metrics.pixel_size()));
      if (lw >= min_width) return;
      // No usable line remains between the floats. Resume at the earliest
      // bottom that can open space in this column.
      float next = 0.f;
      for (const FloatBox& f : floats_) {
        if (f.bottom <= top || f.top >= bottom || f.right <= x ||
            f.left >= x + max_width) continue;
        if (next == 0.f || f.bottom < next) next = f.bottom;
      }
      if (next <= top) return;
      baseline = next + metrics.ascender();
    }
  };
  place_line();

  auto char_adv = [&](const CharInfo& c) -> float {
    if (const float w = atom_w(c); w > 0.f) return w;
    return fonts_.pick(c.role).glyph(c.cp).advance_px();
  };

  // Line-level virtualization: a line wholly outside the materialised band
  // is broken and measured like any other (the block keeps its exact
  // height) but emits no glyphs, images or formulas; LayoutResult::
  // skipped_lines keeps where it starts. A huge block then costs glyphs only
  // for the lines near the view.
  const float band0 = viewport_.top - viewport_.overscan;
  const float band1 = viewport_.bottom + viewport_.overscan;
  auto in_band = [&](float top, float bottom) {
    return !cull_lines || (bottom >= band0 && top <= band1);
  };
  bool skipped_here = false;
  auto skip_line = [&](const CharInfo& c, float lx) {
    out.skipped_lines.push_back(
        {node.id, c.span_idx, c.char_idx, lx, baseline});
    skipped_any_ = true;
    skipped_here = true;
  };
  // Lines of a table range outside it (before / from `stop`).
  auto skip_table_lines = [&](size_t from, size_t to) {
    for (size_t i = from; i < to; ++i) {
      const LineTable::Line& l = table->lines[i];
      out.skipped_lines.push_back({node.id, l.span, l.ch, x, y0 + l.dy});
    }
    if (to > from) skipped_any_ = true;
  };
  // Recorded for a new table (plain text broken in full).
  const bool recording = plain_text && !table;
  std::vector<LineTable::Line> recorded;
  auto record = [&](const CharInfo& c, size_t i, bool start) {
    if (recording)
      recorded.push_back({static_cast<uint32_t>(i), c.span_idx, c.char_idx, 0,
                          baseline - y0, start});
  };

  // Emit one visual line: chars[lo, hi). last_or_hard → no justify.
  // Returns false when the line was skipped (outside the band).
  auto emit_line = [&](size_t lo, size_t hi, bool last_or_hard) -> bool {
    if (flat0 + lo >= emit_end) return false;  // from the table
    record(chars[lo], lo, true);
    // Trim trailing spaces from the measured/justified run.
    size_t content_hi = hi;
    while (content_hi > lo && chars[content_hi - 1].cp == ' ') --content_hi;

    float natural = 0.f;
    int gaps = 0;
    bool seen_nonspace = false;
    bool in_space = false;
    float img_up = 0.f;  // inline images taller than the ascender lower the line
    float img_down = 0.f;  // formulas deeper than the descender: extra below
    float space_adv = metrics.glyph(' ').advance_px();
    if (space_adv < 1.f) space_adv = 4.f;
    for (size_t i = lo; i < content_hi; ++i) {
      natural += char_adv(chars[i]);
      if (const Atom* a = atom_of(chars[i])) {
        if (a->h > 0.f)
          img_up = std::max(img_up,
                            a->h - static_cast<float>(metrics.ascender()));
        if (a->d > 0.f)
          img_down = std::max(
              img_down, a->d + static_cast<float>(metrics.descender()));
      }
      if (chars[i].cp == ' ') {
        if (seen_nonspace) in_space = true;
      } else {
        if (in_space) ++gaps;
        in_space = false;
        seen_nonspace = true;
      }
    }
    baseline += img_up;
    const float line_advance =
        metrics.line_height() * options_.line_spacing + img_down;
    if (!in_band(baseline - static_cast<float>(metrics.ascender()) - img_up,
                 baseline + line_advance)) {
      skip_line(chars[lo], lx0);
      baseline += line_advance;
      return false;
    }

    float gap_extra = 0.f;
    const bool can_justify =
        options_.justify && line_align_ == TextAlign::Start && !in_cell_ &&
        !last_or_hard && gaps > 0 &&
        base_role != FontRole::Code && base_role != FontRole::SmallCode &&
        base_role != FontRole::Heading;
    if (can_justify) {
      float room = lw - natural;
      if (room > 0.5f) {
        gap_extra = room / static_cast<float>(gaps);
        const float cap = space_adv * 3.f;
        if (gap_extra > cap) gap_extra = cap;
      }
    }

    float lx = lx0;
    if (line_align_ == TextAlign::Center && natural < lw)
      lx += (lw - natural) * 0.5f;
    else if (line_align_ == TextAlign::End && natural < lw)
      lx += lw - natural;
    // A display formula alone on its line is centred.
    if (content_hi == lo + 1 && atom_of(chars[lo]) &&
        atom_of(chars[lo])->display && natural < lw)
      lx += (lw - natural) * 0.5f;
    bool after_space_run = false;
    bool had_nonspace = false;
    for (size_t i = lo; i < hi; ++i) {
      const CharInfo& c = chars[i];
      if (i < content_hi) {
        if (c.cp == ' ') {
          if (had_nonspace) after_space_run = true;
        } else {
          if (after_space_run && gap_extra > 0.f) {
            lx += gap_extra;
            after_space_run = false;
          }
          had_nonspace = true;
        }
      }
      PositionedGlyph pg;
      pg.codepoint = c.cp;
      pg.y = baseline - c.rise;
      pg.node = node.id;
      pg.span_index = c.span_idx;
      pg.char_index = c.char_idx;
      pg.auto_link = c.auto_link;
      pg.kind = c.kind;
      pg.marks = node.spans[c.span_idx].marks;
      pg.font = c.role;
      const InlineRole semantic = node.spans[c.span_idx].role;
      pg.color = ctx_color && !(*style_)[c.kind].has_color &&
                             !(semantic == InlineRole::Metadata &&
                               style_->metadata.has_color)
                     ? (*style_)[ctx_role_].color
                     : style_->text_color(block, c.kind, semantic);
      const Atom* atom = atom_of(c);
      if (atom && atom->math) {
        // Formula: drawn from its box at the baseline; the glyph is its
        // caret slot.
        pg.x = lx;
        pg.advance = atom->w;
        PositionedMath pm;
        pm.node = node.id;
        pm.span_index = c.span_idx;
        pm.x = lx;
        pm.y = baseline;
        pm.px = atom->math_px;
        pm.display = atom->display;
        pm.box = atom->math;
        pm.color = pg.color;
        out.maths.push_back(std::move(pm));
        out.glyphs.push_back(pg);
        lx += atom->w;
        continue;
      }
      if (atom && atom->w > 0.f) {
        // Inline image: sits on the baseline; the glyph is its caret slot.
        pg.x = lx;
        pg.advance = atom->w;
        PositionedImage im{node.id, lx, baseline - atom->h, atom->w, atom->h,
                           node.spans[c.span_idx].src, image_base_};
        if (atom->remote) im.thumb = kInlineThumbEdge;
        im.in_text = true;
        im.span_index = c.span_idx;
        out.images.push_back(std::move(im));
        out.glyphs.push_back(pg);
        lx += atom->w;
        continue;
      }
      Font& font = fonts_.pick(c.role);
      const Glyph& g = font.glyph(c.cp);
      const float adv = g.advance_px();
      pg.x = lx + static_cast<float>(g.bearing_x);
      pg.advance = lx + adv - pg.x;
      out.glyphs.push_back(pg);
      lx += adv;
    }
    baseline += line_advance;
    return true;
  };

  // Word-wrap: build runs separated by spaces / hard breaks.
  size_t line_lo = 0;       // start of current line
  size_t word_lo = 0;       // start of current word
  float line_w = 0.f;       // width of committed content on this line
  float word_w = 0.f;       // width of current word (no leading spaces)
  float word_full_w = 0.f;  // … of all of it (capped just above lw)
  bool in_word = false;

  auto flush_line = [&](size_t line_hi, bool hard) {
    if (line_hi > line_lo) {
      emit_line(line_lo, line_hi, hard);
      place_line();
    }
    line_lo = line_hi;
    line_w = 0.f;
  };
  // Commit the word in progress: it moves to a new line when it does not fit.
  auto end_word = [&]() {
    if (!in_word) return;
    if (line_w > 0.f && line_w + word_w > lw && word_w <= lw)
      flush_line(word_lo, false);
    line_w += word_w;
    in_word = false;
    word_w = 0.f;
  };

  // Draw a muted continuation mark at synthetic breaks. It has no document
  // node, so it remains visual only and cannot become a caret/selection slot.
  auto add_forced_break_mark = [&](FontRole role) {
    constexpr uint32_t kReturnMark = 0x21A9; // ↩
    constexpr uint32_t kFallbackMark = 0x21B5; // ↵
    Font& font = fonts_.pick(role);
    const uint32_t cp = font.has_glyph(kReturnMark) ? kReturnMark : kFallbackMark;
    const Glyph& glyph = font.glyph(cp);
    PositionedGlyph mark;
    mark.codepoint = cp;
    if (out.glyphs.empty()) return;
    const PositionedGlyph& previous = out.glyphs.back();
    mark.x = previous.x + std::max(previous.advance, 1.f);
    mark.y = previous.y;
    mark.advance = glyph.advance_px();
    // Very narrow float bands (or a wide fallback glyph) may not have room
    // for the decoration. Never let the decoration itself spill outside the
    // available line; the content break still happens without it.
    if (mark.x - static_cast<float>(glyph.bearing_x) + mark.advance >
        lx0 + lw) return;
    mark.kind = InlineKind::Text;
    mark.font = role;
    mark.color = {128, 128, 128, 190};
    out.glyphs.push_back(mark);
  };

  auto forced_break_mark_width = [&](FontRole role) {
    constexpr uint32_t kReturnMark = 0x21A9;
    constexpr uint32_t kFallbackMark = 0x21B5;
    Font& font = fonts_.pick(role);
    const uint32_t cp = font.has_glyph(kReturnMark) ? kReturnMark : kFallbackMark;
    return font.glyph(cp).advance_px();
  };

  // Lines above the table range.
  if (table) skip_table_lines(0, first);

  for (size_t i = 0; i < chars.size(); ++i) {
    const uint32_t cp = chars[i].cp;

    if (cp == '\n' || cp == '\r') {
      // Commit any partial word, then hard-break.
      end_word();
      flush_line(i, true);
      // Placeholder so caret can sit on the newline index (past emit_end
      // the table has it).
      if (flat0 + i < emit_end) {
        record(chars[i], i, false);
        if (in_band(baseline - static_cast<float>(metrics.ascender()),
                    baseline + static_cast<float>(metrics.descender()))) {
          PositionedGlyph pg;
          pg.codepoint = ' ';
          pg.x = lx0;
          pg.y = baseline;
          pg.node = node.id;
          pg.span_index = chars[i].span_idx;
          pg.char_index = chars[i].char_idx;
          pg.kind = chars[i].kind;
          pg.font = chars[i].role;
          pg.color = style_->text_color(
              block, chars[i].kind, node.spans[chars[i].span_idx].role);
          out.glyphs.push_back(pg);
        } else {
          skip_line(chars[i], lx0);
        }
      }
      if (cp == '\r' && i + 1 < chars.size() && chars[i + 1].cp == '\n') ++i;
      line_lo = i + 1;
      word_lo = line_lo;
      line_w = 0.f;
      word_w = 0.f;
      in_word = false;
      continue;
    }

    float adv = char_adv(chars[i]);

    if (atom_w(chars[i]) > 0.f) {
      // An inline image is a word of its own: breaks before and after it.
      end_word();
      if (line_w > 0.f && line_w + adv > lw) flush_line(i, false);
      line_w += adv;
      word_lo = i + 1;
      continue;
    }

    if (cp == ' ') {
      if (in_word) {
        // Finish word: does it fit on this line?
        if (line_w > 0.f && line_w + word_w > lw && word_w <= lw) {
          flush_line(word_lo, false);
        }
        // If the word alone is wider than the measure, emit it on its own
        // line (may overflow slightly; rare for Latin body text).
        if (line_w == 0.f && word_w > lw) {
          flush_line(i, false);  // emit the overlong word
          word_lo = i;
          word_w = 0.f;
          in_word = false;
          // fall through to treat this space as leading on the new line
        } else {
          line_w += word_w;
          in_word = false;
          word_w = 0.f;
        }
      }
      // Space: only keep it if it still fits; otherwise start a new line
      // (space is discarded as trailing / not carried).
      if (line_w > 0.f && line_w + adv > lw) {
        // The space ends this line (trimmed there), never starts the next.
        flush_line(i + 1, false);
        word_lo = i + 1;
        continue;
      }
      if (line_w > 0.f) {
        line_w += adv;  // keep inter-word space
      } else {
        // Leading space on an empty line — skip (word_lo advances)
        word_lo = i + 1;
      }
      continue;
    }

    // Non-space: accumulate into the current word. If an unbreakable run
    // exceeds the measure, split at this codepoint boundary and mark the
    // synthetic break with a return arrow.
    // The rest of the word's width: one that fits the line is never split
    // (the test below reserves room for the arrow on every character).
    auto measure_rest = [&](size_t from) {
      word_full_w = 0.f;
      for (size_t j = from; j < chars.size(); ++j) {
        if (chars[j].cp == ' ' || chars[j].cp == '\n' || chars[j].cp == '\r' ||
            atom_w(chars[j]) > 0.f) break;
        word_full_w += char_adv(chars[j]);
        if (word_full_w > lw) break;
      }
    };
    if (!in_word) {
      in_word = true;
      word_lo = i;
      word_w = 0.f;
      measure_rest(i);
    }
    // Decide whether a word fits alongside the text already on this line
    // *before* accumulating any of its characters. Otherwise an overlong
    // word can reach the forced-break path with a nonzero line_w: that path
    // measures only word_w, then emits the preceding text as well, pushing
    // both the word and its continuation arrow beyond the right edge.
    if (word_lo == i && line_w > 0.f) {
      float remaining_word_w = 0.f;
      for (size_t j = i; j < chars.size(); ++j) {
        if (chars[j].cp == ' ' || chars[j].cp == '\n' ||
            chars[j].cp == '\r' || atom_w(chars[j]) > 0.f) break;
        remaining_word_w += char_adv(chars[j]);
        if (line_w + remaining_word_w > lw) break;
      }
      if (line_w + remaining_word_w > lw)
        flush_line(word_lo, false);
    }
    if (word_w > 0.f && word_full_w > lw &&
        word_w + adv + forced_break_mark_width(chars[i - 1].role) > lw) {
      const FontRole role = chars[i - 1].role;
      if (emit_line(line_lo, i, false)) add_forced_break_mark(role);
      place_line();
      line_lo = i;
      word_lo = i;
      line_w = 0.f;
      word_w = 0.f;
      measure_rest(i);
    }
    word_w += adv;
  }

  // Tail: last word + line.
  if (table && stop < table->lines.size()) {
    // The rest of the block: from the table.
    skip_table_lines(stop, table->lines.size());
    y = y0 + table->end_dy;
    return;
  }
  end_word();
  if (line_lo < chars.size()) flush_line(chars.size(), true);

  y = baseline - metrics.ascender() + metrics.line_height() * 0.2f;
  if (!text_images.empty())
    layout_text_images(text_images, node, x, y, max_width, out);

  if (recording && skipped_here) {
    // Bigger than the band: keep its lines for the next layout.
    LineTable t;
    t.fingerprint = print ? print : fingerprint();
    t.lines = std::move(recorded);
    t.links = std::move(found_links);
    t.end_dy = y - y0;
    t.used = ++line_table_clock_;
    // Byte offsets of the line starts (lines are in text order).
    size_t span = std::numeric_limits<size_t>::max(), byte = 0;
    uint32_t ch = 0;
    for (LineTable::Line& l : t.lines) {
      if (l.span != span) {
        span = l.span;
        byte = 0;
        ch = 0;
      }
      uint32_t cp = 0;
      while (ch < l.ch && next_utf8(node.spans[span].text, byte, cp)) ++ch;
      l.byte = byte;
    }
    line_tables_[table_key] = std::move(t);
    while (line_tables_.size() > kMaxLineTables) {
      auto oldest = line_tables_.begin();
      for (auto it = line_tables_.begin(); it != line_tables_.end(); ++it)
        if (it->second.used < oldest->second.used) oldest = it;
      line_tables_.erase(oldest);
    }
  }
}

}  // namespace xanatoo
