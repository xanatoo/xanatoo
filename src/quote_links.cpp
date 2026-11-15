// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/quote_links.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace xanatoo {
namespace {

// Thresholds (normalized words / characters). A quote of an ancestor (or of
// the thread's head post) may be short; anywhere else in the thread it has
// to be long enough not to be a common phrase.
constexpr size_t kNearWords = 3, kNearChars = 12;
constexpr size_t kFarWords = 5, kFarChars = 24;
constexpr size_t kFuzzyWords = 6;    // fuzzy matching needs this many words
constexpr float kFuzzyAccept = 0.6f; // share of word trigrams found

struct Cp {
  uint32_t cp;
  Caret at;
};

void decode_span(const std::string& t, NodeId node, size_t si,
                 std::vector<Cp>& out) {
  size_t ci = 0;
  for (size_t i = 0; i < t.size(); ++ci) {
    const auto u = static_cast<unsigned char>(t[i]);
    size_t len = 1;
    uint32_t cp = u;
    if (u >= 0xf0) { len = 4; cp = u & 0x07; }
    else if (u >= 0xe0) { len = 3; cp = u & 0x0f; }
    else if (u >= 0xc0) { len = 2; cp = u & 0x1f; }
    else if (u >= 0x80) { cp = 0xfffd; }  // stray continuation byte
    if (i + len > t.size()) { len = t.size() - i; cp = 0xfffd; }
    else
      for (size_t k = 1; k < len; ++k)
        cp = (cp << 6) | (static_cast<unsigned char>(t[i + k]) & 0x3f);
    out.push_back({cp, Caret{node, si, ci}});
    i += len;
  }
}

bool is_word(uint32_t c) {
  if (c < 0x80)
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z');
  if (c < 0xc0) return c == 0xaa || c == 0xb5 || c == 0xba;
  if (c == 0xd7 || c == 0xf7) return false;
  if (c >= 0x2000 && c <= 0x206f) return false;  // general punctuation
  if (c >= 0x3000 && c <= 0x303f) return false;  // CJK punctuation
  if (c >= 0xfe00 && c <= 0xfe0f) return false;  // variation selectors
  if (c == 0xfeff || c == 0xfffc || c == 0xfffd) return false;
  return true;
}

uint32_t fold(uint32_t c) {
  if (c >= 'A' && c <= 'Z') return c + 32;
  if (c >= 0xc0 && c <= 0xde && c != 0xd7) return c + 32;
  if (c >= 0x391 && c <= 0x3a9) return c + 32;  // Greek
  if (c >= 0x410 && c <= 0x42f) return c + 32;  // Cyrillic
  return c;
}

// Normalized text with the caret of each character (a space carries the
// caret of the word character after it).
struct Index {
  std::u32string s;
  std::vector<Caret> at;
  bool pending = false;

  void add(uint32_t c, const Caret& where) {
    if (!is_word(c)) {
      pending = !s.empty();
      return;
    }
    if (pending) {
      s.push_back(U' ');
      at.push_back(where);
      pending = false;
    }
    s.push_back(fold(c));
    at.push_back(where);
  }
  void add(const std::vector<Cp>& cps, size_t b, size_t e) {
    pending = !s.empty();
    for (size_t i = b; i < e; ++i) add(cps[i].cp, cps[i].at);
  }
  size_t words() const {
    return s.empty() ? 0 : 1 + std::count(s.begin(), s.end(), U' ');
  }
};

Caret after(Caret c) {
  ++c.char_index;
  return c;
}

// One line of a message: its characters after any '>' markers, and its
// quote depth (0 = the author's own text).
struct Line {
  std::vector<Cp> cps;
  int depth = 0;
  NodeId node = INVALID_NODE;
  bool blank = false;
};

bool is_space(uint32_t c) {
  return c == ' ' || c == '\t' || c == '\r' || c == 0xa0;
}

void split_lines(const std::vector<Cp>& cps, NodeId node, int base_depth,
                 std::vector<Line>& out) {
  size_t b = 0;
  for (size_t i = 0; i <= cps.size(); ++i) {
    if (i < cps.size() && cps[i].cp != '\n' && cps[i].cp != 0x2028) continue;
    Line line;
    line.node = node;
    line.depth = base_depth;
    size_t k = b;
    // Leading markers: "> ", ">>", "> > ".
    for (;;) {
      while (k < i && is_space(cps[k].cp)) ++k;
      if (k < i && cps[k].cp == '>') {
        ++line.depth;
        ++k;
      } else {
        break;
      }
    }
    line.cps.assign(cps.begin() + k, cps.begin() + i);
    line.blank = std::none_of(line.cps.begin(), line.cps.end(),
                              [](const Cp& c) { return is_word(c.cp); });
    out.push_back(std::move(line));
    b = i + 1;
  }
}

bool inside_quote(const Document& doc, NodeId id, NodeId msg) {
  for (; id != INVALID_NODE && id != msg; id = doc.get(id).parent)
    if (doc.get(id).kind == NodeKind::BlockQuote) return true;
  return false;
}

void collect_lines(const Document& doc, NodeId msg, NodeId id,
                   std::vector<Line>& out) {
  const Node& n = doc.get(id);
  if (id != msg && n.kind == NodeKind::Message) return;  // a nested reply
  if (id != msg && !n.spans.empty()) {
    std::vector<Cp> cps;
    for (size_t si = 0; si < n.spans.size(); ++si)
      decode_span(n.spans[si].text, id, si, cps);
    split_lines(cps, id, inside_quote(doc, id, msg) ? 1 : 0, out);
    // A node ends its paragraph.
    out.push_back(Line{{}, 0, id, true});
  }
  for (NodeId c : n.children) collect_lines(doc, msg, c, out);
}

struct Quote {
  NodeId msg = INVALID_NODE;
  std::vector<Index> pieces;  // split at ellipses, matched in order
  size_t words = 0, chars = 0;
};

bool is_ellipsis_at(const std::vector<Cp>& cps, size_t i, size_t& len) {
  if (cps[i].cp == 0x2026) {
    len = 1;
    return true;
  }
  size_t k = i;
  while (k < cps.size() && cps[k].cp == '.') ++k;
  len = k - i;
  return len >= 3;
}

void add_quote_text(Quote& q, const Line& line) {
  if (q.pieces.empty()) q.pieces.emplace_back();
  size_t b = 0;
  for (size_t i = 0; i < line.cps.size(); ++i) {
    size_t len = 0;
    if (!is_ellipsis_at(line.cps, i, len)) continue;
    q.pieces.back().add(line.cps, b, i);
    if (!q.pieces.back().s.empty()) q.pieces.emplace_back();
    i += len - 1;
    b = i + 1;
  }
  q.pieces.back().add(line.cps, b, line.cps.size());
}

void finish_quote(Quote& q, std::vector<Quote>& out) {
  while (!q.pieces.empty() && q.pieces.back().s.empty()) q.pieces.pop_back();
  for (const Index& p : q.pieces) {
    q.words += p.words();
    q.chars += p.s.size();
  }
  if (!q.pieces.empty()) out.push_back(std::move(q));
  q = Quote{};
}

// Quotes are runs of lines with the same depth > 0, in one node, with no
// blank line between them.
std::vector<Quote> quotes_of(NodeId msg, const std::vector<Line>& lines) {
  std::vector<Quote> out;
  Quote q;
  int depth = 0;
  NodeId node = INVALID_NODE;
  for (const Line& line : lines) {
    const bool cont = line.depth > 0 && !line.blank && line.depth == depth &&
                      line.node == node;
    if (!cont && !q.pieces.empty()) finish_quote(q, out);
    q.msg = msg;
    depth = line.blank ? 0 : line.depth;
    node = line.node;
    if (line.depth > 0 && !line.blank) add_quote_text(q, line);
  }
  if (!q.pieces.empty()) finish_quote(q, out);
  return out;
}

struct MsgText {
  std::vector<Line> lines;
  Index own;  // depth-0 text only
  Index all;  // own text and quotes
  // Word trigram → word starts in `own`, built on demand.
  std::unordered_map<uint64_t, std::vector<uint32_t>> trigrams;
  std::vector<uint32_t> word_start, word_end;
  bool has_trigrams = false;
};

uint64_t word_hash(const std::u32string& s, size_t b, size_t e) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = b; i < e; ++i) h = (h ^ s[i]) * 1099511628211ull;
  return h;
}

void words_of(const std::u32string& s, std::vector<uint32_t>& starts,
              std::vector<uint32_t>& ends) {
  size_t b = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i < s.size() && s[i] != U' ') continue;
    if (i > b) {
      starts.push_back(static_cast<uint32_t>(b));
      ends.push_back(static_cast<uint32_t>(i));
    }
    b = i + 1;
  }
}

uint64_t trigram(const std::vector<uint64_t>& w, size_t i) {
  return w[i] * 0x9e3779b97f4a7c15ull ^ (w[i + 1] << 1) ^
         (w[i + 2] * 0xc2b2ae3d27d4eb4full);
}

std::vector<uint64_t> word_hashes(const std::u32string& s,
                                  const std::vector<uint32_t>& starts,
                                  const std::vector<uint32_t>& ends) {
  std::vector<uint64_t> w;
  for (size_t i = 0; i < starts.size(); ++i)
    w.push_back(word_hash(s, starts[i], ends[i]));
  return w;
}

void build_trigrams(MsgText& t) {
  if (t.has_trigrams) return;
  t.has_trigrams = true;
  words_of(t.own.s, t.word_start, t.word_end);
  const auto w = word_hashes(t.own.s, t.word_start, t.word_end);
  for (size_t i = 0; i + 2 < w.size(); ++i)
    t.trigrams[trigram(w, i)].push_back(static_cast<uint32_t>(i));
}

// Find `needle` in `hay` from `from` on, starting and ending at word edges.
size_t find_words(const std::u32string& hay, const std::u32string& needle,
                  size_t from) {
  for (size_t p = hay.find(needle, from); p != std::u32string::npos;
       p = hay.find(needle, p + 1)) {
    const size_t e = p + needle.size();
    if ((p == 0 || hay[p - 1] == U' ') && (e == hay.size() || hay[e] == U' '))
      return p;
  }
  return std::u32string::npos;
}

bool match_exact(const Quote& q, const Index& hay, Selection& out) {
  if (hay.s.empty()) return false;
  size_t from = 0, first = 0, last = 0;
  for (size_t i = 0; i < q.pieces.size(); ++i) {
    const size_t p = find_words(hay.s, q.pieces[i].s, from);
    if (p == std::u32string::npos) return false;
    if (i == 0) first = p;
    last = p + q.pieces[i].s.size();
    from = last;
  }
  out = {hay.at[first], after(hay.at[last - 1])};
  return true;
}

// Share of the quote's word trigrams found in the best window of the
// source, and that window's range.
float match_fuzzy(const Quote& q, MsgText& t, Selection& out) {
  build_trigrams(t);
  std::vector<std::pair<uint32_t, uint32_t>> hits;  // (source word, trigram)
  uint32_t total = 0;
  for (const Index& piece : q.pieces) {
    std::vector<uint32_t> st, en;
    words_of(piece.s, st, en);
    const auto w = word_hashes(piece.s, st, en);
    for (size_t i = 0; i + 2 < w.size(); ++i, ++total) {
      auto it = t.trigrams.find(trigram(w, i));
      if (it == t.trigrams.end()) continue;
      for (uint32_t pos : it->second) hits.push_back({pos, total});
    }
  }
  if (total == 0 || hits.empty()) return 0.f;
  // Cheap bound: not enough distinct trigrams at all.
  {
    std::vector<uint32_t> ids;
    for (auto& h : hits) ids.push_back(h.second);
    std::sort(ids.begin(), ids.end());
    const size_t distinct =
        std::unique(ids.begin(), ids.end()) - ids.begin();
    if (static_cast<float>(distinct) / total < kFuzzyAccept) return 0.f;
  }
  std::sort(hits.begin(), hits.end());
  const uint32_t width = q.words + q.words / 2 + 2;
  std::vector<uint32_t> count(total, 0);
  uint32_t distinct = 0, best = 0;
  size_t best_b = 0, best_e = 0;
  for (size_t b = 0, e = 0; e < hits.size(); ++e) {
    if (count[hits[e].second]++ == 0) ++distinct;
    while (hits[e].first - hits[b].first > width)
      if (--count[hits[b++].second] == 0) --distinct;
    if (distinct > best) {
      best = distinct;
      best_b = b;
      best_e = e;
    }
  }
  // Below 1 even when every trigram is there: the exact search missed, so
  // something (a typo, a dropped word) differs.
  const float score = std::min(0.99f, static_cast<float>(best) / total);
  if (score < kFuzzyAccept) return 0.f;
  const uint32_t w0 = hits[best_b].first, w1 = hits[best_e].first + 2;
  out = {t.own.at[t.word_start[w0]], after(t.own.at[t.word_end[w1] - 1])};
  return score;
}

void collect_messages(const Document& doc, NodeId id,
                      std::vector<NodeId>& out) {
  const Node& n = doc.get(id);
  if (n.kind == NodeKind::Message) out.push_back(id);
  for (NodeId c : n.children) collect_messages(doc, c, out);
}

}  // namespace

std::string quote_normalize(std::string_view utf8) {
  std::vector<Cp> cps;
  decode_span(std::string(utf8), INVALID_NODE, 0, cps);
  Index ix;
  ix.add(cps, 0, cps.size());
  std::string out;
  for (char32_t c : ix.s) {
    if (c < 0x80) {
      out += static_cast<char>(c);
    } else if (c < 0x800) {
      out += static_cast<char>(0xc0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 0x3f));
    } else if (c < 0x10000) {
      out += static_cast<char>(0xe0 | (c >> 12));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
      out += static_cast<char>(0x80 | (c & 0x3f));
    } else {
      out += static_cast<char>(0xf0 | (c >> 18));
      out += static_cast<char>(0x80 | ((c >> 12) & 0x3f));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
      out += static_cast<char>(0x80 | (c & 0x3f));
    }
  }
  return out;
}

std::vector<QuoteLink> find_quote_links(const Document& doc, NodeId root) {
  std::vector<QuoteLink> out;
  if (root == INVALID_NODE) root = doc.root;
  if (root == INVALID_NODE) return out;
  std::vector<NodeId> msgs;
  collect_messages(doc, root, msgs);
  if (msgs.size() < 2) return out;

  std::unordered_map<std::string, size_t> by_label;
  std::unordered_map<NodeId, size_t> by_id;
  for (size_t i = 0; i < msgs.size(); ++i) {
    by_id[msgs[i]] = i;
    const std::string& label = doc.get(msgs[i]).label;
    if (!label.empty()) by_label.emplace(label, i);
  }
  auto parent_of = [&](size_t i) -> long {
    const Node& n = doc.get(msgs[i]);
    if (!n.alt.empty()) {
      auto it = by_label.find(n.alt);
      if (it != by_label.end() && it->second != i)
        return static_cast<long>(it->second);
    }
    for (NodeId p = n.parent; p != INVALID_NODE; p = doc.get(p).parent) {
      auto it = by_id.find(p);
      if (it != by_id.end()) return static_cast<long>(it->second);
    }
    return -1;
  };

  std::vector<std::unique_ptr<MsgText>> texts(msgs.size());
  auto text_of = [&](size_t i) -> MsgText& {
    if (!texts[i]) {
      texts[i] = std::make_unique<MsgText>();
      MsgText& t = *texts[i];
      collect_lines(doc, msgs[i], msgs[i], t.lines);
      for (const Line& line : t.lines) {
        if (line.depth == 0) t.own.add(line.cps, 0, line.cps.size());
        t.all.add(line.cps, 0, line.cps.size());
      }
    }
    return *texts[i];
  };

  for (size_t i = 0; i < msgs.size(); ++i) {
    const std::vector<Quote> quotes = quotes_of(msgs[i], text_of(i).lines);
    if (quotes.empty()) continue;

    // Candidates: ancestors, the thread's head, then by distance (earlier
    // first). `near` ones accept shorter quotes.
    std::vector<size_t> order;
    std::vector<char> seen(msgs.size(), 0);
    seen[i] = 1;
    for (long p = parent_of(i); p >= 0 && !seen[p]; p = parent_of(p)) {
      seen[p] = 1;
      order.push_back(static_cast<size_t>(p));
    }
    if (!seen[0]) {
      seen[0] = 1;
      order.push_back(0);
    }
    const size_t near_count = order.size();
    for (size_t d = 1; d < msgs.size(); ++d) {
      if (d <= i && !seen[i - d]) order.push_back(i - d);
      if (i + d < msgs.size() && !seen[i + d]) order.push_back(i + d);
    }

    for (const Quote& q : quotes) {
      if (q.words < kNearWords || q.chars < kNearChars) continue;
      const bool far_ok = q.words >= kFarWords && q.chars >= kFarChars;
      QuoteLink link;
      link.quoting_msg = msgs[i];
      link.quote = {q.pieces.front().at.front(),
                    after(q.pieces.back().at.back())};
      bool found = false;
      for (size_t k = 0; k < order.size() && !found; ++k) {
        if (k >= near_count && !far_ok) break;
        if (match_exact(q, text_of(order[k]).own, link.source)) {
          link.source_msg = msgs[order[k]];
          link.score = 1.f;
          found = true;
        }
      }
      // The original is not here, but an ancestor quoted it as well.
      for (size_t k = 0; k < near_count && !found; ++k) {
        if (match_exact(q, text_of(order[k]).all, link.source)) {
          link.source_msg = msgs[order[k]];
          link.score = 1.f;
          link.via_quote = true;
          found = true;
        }
      }
      if (!found && q.words >= kFuzzyWords) {
        float best = 0.f;
        for (size_t k = 0; k < order.size(); ++k) {
          Selection sel;
          const float s = match_fuzzy(q, text_of(order[k]), sel);
          if (s > best + 1e-6f) {
            best = s;
            link.source = sel;
            link.source_msg = msgs[order[k]];
          }
        }
        if (best > 0.f) {
          link.score = best;
          found = true;
        }
      }
      if (found) out.push_back(link);
    }
  }
  return out;
}

}  // namespace xanatoo
