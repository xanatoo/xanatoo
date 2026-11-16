// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// Quote links in discussion threads (docs/QUOTES.md): the quote_links.hpp
// matcher per buffer, the margin arrow on a linked quote, the underline
// under quoted text in the original, context menu jumps both ways.

#include "xanatoo/app.hpp"
#include "xanatoo/locator.hpp"
#include "xanatoo/util.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>

namespace xanatoo::app {

namespace {

bool in_range(const xanatoo::Document& d, const xanatoo::Selection& s,
              const xanatoo::Caret& c) {
  return xanatoo::caret_cmp(d, s.anchor, c) <= 0 &&
         xanatoo::caret_cmp(d, c, s.focus) <= 0;
}

// Pane state the quote overlays work on: the buffer's own document only
// (a source view projection has other nodes).
DocBuffer* quote_buffer(App& app, Pane& p) {
  if (p.source_doc) return nullptr;
  DocBuffer* b = find_buf(app, p.buf);
  if (!b || p.layout_buf != p.buf) return nullptr;
  return b;
}

// The laid-out box of `node` or of its nearest ancestor that has one.
const xanatoo::LayoutBox* box_of(
    const xanatoo::Document& d,
    const std::unordered_map<xanatoo::NodeId, const xanatoo::LayoutBox*>& boxes,
    xanatoo::NodeId node) {
  for (; node != xanatoo::INVALID_NODE; node = d.get(node).parent) {
    auto it = boxes.find(node);
    if (it != boxes.end()) return it->second;
  }
  return nullptr;
}

std::unordered_map<xanatoo::NodeId, const xanatoo::LayoutBox*> box_map(
    const Pane& p) {
  std::unordered_map<xanatoo::NodeId, const xanatoo::LayoutBox*> m;
  m.reserve(p.layout.boxes.size());
  for (const auto& b : p.layout.boxes) m.emplace(b.node, &b);
  return m;
}

std::string author_of(const xanatoo::Document& d, xanatoo::NodeId msg) {
  const std::string& a = d.get(msg).src;
  return a.empty() ? std::string("someone") : a;
}

}  // namespace

const std::vector<xanatoo::QuoteLink>& quote_links_of(DocBuffer& b) {
  QuoteLinks& q = b.quotes;
  if (!q.valid || q.doc_gen != b.doc_gen || q.nodes != b.doc.nodes.size() ||
      q.next_id != b.doc.next_id) {
    q.links.clear();
    bool thread = false;
    for (const auto& n : b.doc.nodes)
      if (n.kind == xanatoo::NodeKind::Thread ||
          n.kind == xanatoo::NodeKind::Message) {
        thread = true;
        break;
      }
    if (thread) q.links = xanatoo::find_quote_links(b.doc);
    q.valid = true;
    q.doc_gen = b.doc_gen;
    q.nodes = b.doc.nodes.size();
    q.next_id = b.doc.next_id;
  }
  return q.links;
}

std::vector<QuoteIcon> quote_icons(App& app, Pane& p) {
  std::vector<QuoteIcon> out;
  DocBuffer* b = quote_buffer(app, p);
  if (!b) return out;
  const auto& links = quote_links_of(*b);
  if (links.empty()) return out;
  const xanatoo::Document& d = b->doc;
  const auto boxes = box_map(p);
  const float lh = static_cast<float>(app.fonts.regular.line_height());
  const float right = static_cast<float>(p.w - kSbW) - 4.f;
  for (size_t i = 0; i < links.size(); ++i) {
    const xanatoo::Caret& at = links[i].quote.anchor;
    const xanatoo::LayoutBox* box = box_of(d, boxes, at.node);
    if (!box) continue;
    const float top = box->y - p.scroll_y;
    if (top + box->height < -lh || top > static_cast<float>(p.h) + lh)
      continue;
    const auto cp = xanatoo::caret_screen_pos(p.layout, at, p.scroll_y, lh);
    if (!cp.valid) continue;
    QuoteIcon ic;
    ic.link = i;
    ic.w = 11.f;
    ic.h = std::max(11.f, cp.height * 0.7f);
    ic.x = std::min(box->x + box->width + 4.f, right - ic.w);
    ic.y = cp.y - cp.height * 0.75f;
    // Several quotes starting on one line: side by side, to the left.
    for (const QuoteIcon& o : out)
      if (std::abs(o.y - ic.y) < 2.f && std::abs(o.x - ic.x) < ic.w + 2.f)
        ic.x = o.x - ic.w - 3.f;
    out.push_back(ic);
  }
  return out;
}

long quote_icon_at(App& app, Pane& p, float lx, float my) {
  for (const QuoteIcon& ic : quote_icons(app, p))
    if (lx >= ic.x - 2.f && lx < ic.x + ic.w + 2.f && my >= ic.y - 2.f &&
        my < ic.y + ic.h + 2.f)
      return static_cast<long>(ic.link);
  return -1;
}

void collect_context_quotes(App& app, Pane& p,
                            std::optional<xanatoo::Caret> at) {
  app.context_quotes.clear();
  app.context_quote_buf = kNoBuffer;
  DocBuffer* b = quote_buffer(app, p);
  if (!b || !at) return;
  const auto& links = quote_links_of(*b);
  const xanatoo::Document& d = b->doc;
  constexpr size_t kMaxReplies = 8;
  size_t replies = 0;
  for (size_t i = 0; i < links.size(); ++i) {
    const xanatoo::QuoteLink& l = links[i];
    if (in_range(d, l.quote, *at)) {
      std::string label = "Go to Quoted Message (" +
                          author_of(d, l.source_msg) + ")";
      if (l.score < 1.f) label += " \xE2\x89\x88";  // ≈: approximate match
      app.context_quotes.push_back({std::move(label), i, true});
    }
    if (in_range(d, l.source, *at) && replies < kMaxReplies) {
      ++replies;
      app.context_quotes.push_back(
          {"Go to Reply Quoting This (" + author_of(d, l.quoting_msg) + ")",
           i, false});
    }
  }
  if (!app.context_quotes.empty()) app.context_quote_buf = p.buf;
}

void jump_quote(App& app, int pi, size_t link, bool to_source) {
  Pane& p = app.panes[pi];
  DocBuffer* b = quote_buffer(app, p);
  if (!b) return;
  const auto& links = quote_links_of(*b);
  if (link >= links.size()) return;
  const xanatoo::QuoteLink& l = links[link];
  const xanatoo::Selection target = to_source ? l.source : l.quote;
  xanatoo::Locator loc = xanatoo::parse_locator(pane_address(app, p));
  if (loc.base.empty()) return;
  loc.point = xanatoo::text_selector(b->doc, xanatoo::INVALID_NODE, target);
  app.quote_focus_buf = p.buf;
  app.quote_focus = static_cast<long>(link);
  navigate_pane(app, pi, xanatoo::format_locator(loc));
  if (to_source && l.score < 1.f)
    flash_status(app, "Approximate match: the quote differs from the original");
  else if (to_source && l.via_quote)
    flash_status(app, "Original not in this thread; " +
                          author_of(b->doc, l.source_msg) + " quoted it too");
}

std::string quote_tooltip(App& app, int pi, size_t link) {
  DocBuffer* b = quote_buffer(app, app.panes[pi]);
  if (!b) return {};
  const auto& links = quote_links_of(*b);
  if (link >= links.size()) return {};
  const xanatoo::QuoteLink& l = links[link];
  std::string tip = "Quoted from " + author_of(b->doc, l.source_msg);
  if (l.via_quote) tip += " (who quoted it too)";
  if (l.score < 1.f) tip += " \xC2\xB7 approximate match";
  return tip + " \xC2\xB7 click to jump";
}

void draw_quote_marks(App& app, int pi, float ox, float py,
                      float draw_scroll) {
  Pane& p = app.panes[pi];
  DocBuffer* b = quote_buffer(app, p);
  if (!b) return;
  const auto& links = quote_links_of(*b);
  if (links.empty()) return;
  const xanatoo::Document& d = b->doc;
  const auto boxes = box_map(p);
  const xanatoo::Rgba& c = app.style.mark;
  SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_BLEND);

  // The original of every laid-out quote source: a dotted underline (the
  // reverse link; right-click there to go to the replies).
  const float view_top = p.scroll_y - static_cast<float>(p.h);
  const float view_bot = p.scroll_y + 2.f * static_cast<float>(p.h);
  for (const xanatoo::QuoteLink& l : links) {
    const xanatoo::LayoutBox* box = box_of(d, boxes, l.source.anchor.node);
    if (!box || box->y + box->height < view_top || box->y > view_bot)
      continue;
    const auto rects = xanatoo::selection_rects(
        p.layout, l.source, draw_scroll, app.renderer.glyph_extents());
    SDL_SetRenderDrawColor(app.sdl, c.r, c.g, c.b, 230);
    for (const auto& r : rects) {
      const int y = static_cast<int>(r.y + r.h) - 1;
      for (int x = static_cast<int>(r.x + ox); x < static_cast<int>(r.x + ox + r.w);
           x += 4)
        xanatoo::sdl::draw_line(app.sdl, x, y, x + 1, y);
    }
  }

  // Both ends of the link last jumped along, while its mark is shown.
  if (app.quote_focus >= 0 && app.quote_focus_buf == p.buf &&
      static_cast<size_t>(app.quote_focus) < links.size()) {
    bool marked = false;
    for (int v : live_views(app))
      if (app.panes[v].buf == p.buf && !app.panes[v].marks.empty())
        marked = true;
    if (marked) {
      const xanatoo::QuoteLink& l = links[static_cast<size_t>(app.quote_focus)];
      for (const xanatoo::Selection* s : {&l.quote, &l.source}) {
        // The jump target is already the view's `#` mark.
        if (std::any_of(p.marks.begin(), p.marks.end(),
                        [&](const xanatoo::Selection& m) {
                          return m.anchor == s->anchor && m.focus == s->focus;
                        }))
          continue;
        const auto rects = xanatoo::selection_rects(
            p.layout, *s, draw_scroll, app.renderer.glyph_extents());
        app.renderer.draw_selection(rects, ox, c.r, c.g, c.b, c.a);
      }
    } else {
      app.quote_focus = -1;
    }
  }

  // Margin arrows on linked quotes: solid for exact matches, an outline for
  // approximate ones.
  for (const QuoteIcon& ic : quote_icons(app, p)) {
    const xanatoo::QuoteLink& l = links[ic.link];
    const bool hot = app.hover.quote_pane == pi &&
                     app.hover.quote == static_cast<long>(ic.link);
    SDL_SetRenderDrawColor(app.sdl, static_cast<Uint8>(c.r * 3 / 4),
                           static_cast<Uint8>(c.g * 3 / 4),
                           static_cast<Uint8>(c.b * 3 / 4), hot ? 255 : 190);
    const int x = static_cast<int>(ox + ic.x);
    const int y = static_cast<int>(py + ic.y);
    const int w = static_cast<int>(ic.w);
    const int h = static_cast<int>(ic.h);
    const int head = std::max(3, w / 2);
    const int cx = x + w / 2;
    // Up arrow: a triangle head over a stem.
    for (int k = 0; k <= head; ++k) {
      const bool edge = k == head || k == 0;
      if (l.score >= 1.f || edge) {
        xanatoo::sdl::draw_line(app.sdl, cx - k, y + k, cx + k, y + k);
      } else {
        xanatoo::sdl::draw_line(app.sdl, cx - k, y + k, cx - k, y + k);
        xanatoo::sdl::draw_line(app.sdl, cx + k, y + k, cx + k, y + k);
      }
    }
    const SDL_Rect stem{cx - 1, y + head, 3, std::max(2, h - head)};
    xanatoo::sdl::fill_rect(app.sdl, &stem);
  }
}

void refresh_context_panel(App& app) {
  app.context_kind.clear();
  app.context_title.clear();
  app.context_body.clear();
  app.context_meta.clear();
  app.context_can_jump = false;
  app.context_jump_pane = -1;
  app.context_jump_link = -1;
  app.context_jump_to_source = true;

  // Prefer an explicit quote-link hover (margin arrow or quote text).
  if (app.hover.quote >= 0 && app.hover.quote_pane >= 0 &&
      app.hover.quote_pane < kMaxViews) {
    const int pi = app.hover.quote_pane;
    Pane& p = app.panes[pi];
    DocBuffer* b = quote_buffer(app, p);
    if (!b) return;
    const auto& links = quote_links_of(*b);
    if (static_cast<size_t>(app.hover.quote) >= links.size()) return;
    const xanatoo::QuoteLink& l = links[static_cast<size_t>(app.hover.quote)];
    const xanatoo::Document& d = b->doc;
    app.context_kind = "Quote source";
    app.context_title = "Quoted from " + author_of(d, l.source_msg);
    if (l.via_quote)
      app.context_title += " (who also quoted it)";
    // Source message body with the matched span emphasized as plain text.
    std::string source = xanatoo::selected_text(d, l.source);
    std::string quote = xanatoo::selected_text(d, l.quote);
    // Surrounding message context: plain_text of the whole source message.
    std::string message = xanatoo::plain_text(d, l.source_msg);
    if (message.size() > 1200) {
      // Prefer a window around the match inside the message.
      size_t at = message.find(source.substr(0, std::min<size_t>(source.size(), 40)));
      if (at == std::string::npos) at = 0;
      size_t begin = at > 180 ? at - 180 : 0;
      size_t end = std::min(message.size(), at + source.size() + 220);
      app.context_body = (begin ? "…" : "") +
                         message.substr(begin, end - begin) +
                         (end < message.size() ? "…" : "");
    } else {
      app.context_body = message.empty() ? source : message;
    }
    if (app.context_body.empty()) app.context_body = source;
    if (l.score < 1.f)
      app.context_meta = "Approximate match — the quote wording differs";
    else if (l.via_quote)
      app.context_meta = "Original is outside this thread";
    else
      app.context_meta = "Exact match in this thread";
    if (!quote.empty() && quote.size() < 200) {
      app.context_meta += "\nQuote: “" + quote + "”";
    }
    app.context_can_jump = true;
    app.context_jump_pane = pi;
    app.context_jump_link = app.hover.quote;
    app.context_jump_to_source = true;
    return;
  }

  // Transclude node under the pointer: show its target label and embedded text.
  if (app.hover.transclude != xanatoo::INVALID_NODE &&
      app.hover.transclude_pane >= 0 &&
      app.hover.transclude_pane < kMaxViews) {
    Pane& p = app.panes[app.hover.transclude_pane];
    const xanatoo::Document& d = doc_of(app, p);
    const xanatoo::Node& n = d.get(app.hover.transclude);
    app.context_kind = "Transclusion";
    app.context_title = n.src.empty() ? std::string("(unnamed target)") : n.src;
    if (!n.summary.empty())
      app.context_meta = "Excerpt: " + n.summary;
    // Prefer laid-out transclude children / resolved text node.
    std::string body = xanatoo::plain_text(d, app.hover.transclude);
    if (body.empty() && !app.hover.transclude_src.empty())
      body = app.hover.transclude_src;
    if (body.size() > 1500)
      body = body.substr(0, 1500) + "…";
    app.context_body = body.empty()
                           ? "No resolved text for this transclusion yet."
                           : body;
    return;
  }

  app.context_kind = "Context";
  app.context_body =
      "Hover a quote arrow or a quoted passage in a thread to see its "
      "source message here. Transclusions show their target text.";
}


}  // namespace xanatoo::app
