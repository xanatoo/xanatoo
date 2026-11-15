// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/quote_links.hpp"

#include <algorithm>
#include <cstdint>
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

// A caret in 12 bytes (an Index keeps one per character).
struct Pos {
  uint32_t node, span, ch;
  Caret caret() const { return Caret{node, span, ch}; }
  Caret after() const { return Caret{node, span, ch + 1}; }
};

// Normalized text with the position of each character (a space carries the
// position of the word character after it).
struct Index {
  std::u32string s;
  std::vector<Pos> at;  // empty when built without positions
  bool pending = false;
  bool with_pos = true;

  static Pos pos(const Caret& c) {
    return {static_cast<uint32_t>(c.node), static_cast<uint32_t>(c.span_index),
            static_cast<uint32_t>(c.char_index)};
  }

  void add(uint32_t c, const Caret& where) {
    if (!is_word(c)) {
      pending = !s.empty();
      return;
    }
    if (pending) {
      s.push_back(U' ');
      if (with_pos) at.push_back(pos(where));
      pending = false;
    }
    s.push_back(fold(c));
    if (with_pos) at.push_back(pos(where));
  }
  void add(const std::vector<Cp>& cps, size_t b, size_t e) {
    pending = !s.empty();
    for (size_t i = b; i < e; ++i) add(cps[i].cp, cps[i].at);
  }
  size_t words() const {
    return s.empty() ? 0 : 1 + std::count(s.begin(), s.end(), U' ');
  }
};

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

// Normalized text of a message without positions (4 bytes a character);
// positions are rebuilt for the few messages a quote matched in.
struct MsgText {
  std::u32string own;  // depth-0 text only
  std::vector<Quote> quotes;
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
  w.reserve(starts.size());
  for (size_t i = 0; i < starts.size(); ++i)
    w.push_back(word_hash(s, starts[i], ends[i]));
  return w;
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

// [first, last) of the pieces of `q` in order in `hay`.
bool match_exact(const Quote& q, const std::u32string& hay, size_t& first,
                 size_t& last) {
  if (hay.empty()) return false;
  size_t from = 0;
  for (size_t i = 0; i < q.pieces.size(); ++i) {
    const size_t p = find_words(hay, q.pieces[i].s, from);
    if (p == std::u32string::npos) return false;
    if (i == 0) first = p;
    last = p + q.pieces[i].s.size();
    from = last;
  }
  return true;
}

// Share of the quote's word trigrams found in the best window of `hay`, and
// that window as [first, last).
float match_fuzzy(const Quote& q, const std::u32string& hay, size_t& first,
                  size_t& last) {
  // Quote trigram → its ids (a trigram may occur more than once).
  std::unordered_map<uint64_t, std::vector<uint32_t>> ids;
  uint32_t total = 0;
  for (const Index& piece : q.pieces) {
    std::vector<uint32_t> st, en;
    words_of(piece.s, st, en);
    const auto w = word_hashes(piece.s, st, en);
    for (size_t i = 0; i + 2 < w.size(); ++i) ids[trigram(w, i)].push_back(total++);
  }
  if (total == 0) return 0.f;
  std::vector<uint32_t> st, en;
  words_of(hay, st, en);
  const auto w = word_hashes(hay, st, en);
  std::vector<std::pair<uint32_t, uint32_t>> hits;  // (source word, id)
  for (size_t i = 0; i + 2 < w.size(); ++i) {
    auto it = ids.find(trigram(w, i));
    if (it == ids.end()) continue;
    for (uint32_t id : it->second) hits.push_back({static_cast<uint32_t>(i), id});
  }
  if (hits.empty()) return 0.f;
  const uint32_t width = static_cast<uint32_t>(q.words + q.words / 2 + 2);
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
  first = st[hits[best_b].first];
  last = en[hits[best_e].first + 2];
  return score;
}

// Sorted (key, message) pairs, one per message and key: an inverted index
// in flat memory.
struct Postings {
  std::vector<std::pair<uint64_t, uint32_t>> v;
  void finish() {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
  }
  std::pair<size_t, size_t> find(uint64_t key) const {
    auto lo = std::lower_bound(v.begin(), v.end(),
                               std::pair<uint64_t, uint32_t>{key, 0});
    auto hi = lo;
    while (hi != v.end() && hi->first == key) ++hi;
    return {static_cast<size_t>(lo - v.begin()),
            static_cast<size_t>(hi - v.begin())};
  }
};

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

  // Lines are big (a Caret per character): keep only the normalized own
  // text and the quotes of each message.
  std::vector<MsgText> texts(msgs.size());
  std::vector<Line> lines;
  auto build_index = [&](size_t i, bool own_only, bool with_pos) {
    lines.clear();
    collect_lines(doc, msgs[i], msgs[i], lines);
    Index ix;
    ix.with_pos = with_pos;
    for (const Line& line : lines)
      if (!own_only || line.depth == 0) ix.add(line.cps, 0, line.cps.size());
    return ix;
  };
  for (size_t i = 0; i < msgs.size(); ++i) {
    texts[i].own = build_index(i, true, false).s;
    texts[i].quotes = quotes_of(msgs[i], lines);  // lines of that call
  }
  // [first, last) of a message's normalized text as a Selection.
  auto selection = [&](size_t i, bool own_only, size_t first, size_t last) {
    const Index ix = build_index(i, own_only, true);
    return Selection{ix.at[first].caret(), ix.at[last - 1].after()};
  };

  // Inverted index over the own text of every message: word → messages and
  // word trigram → messages. Only the messages these name can match, so a
  // quote never scans the whole thread.
  Postings word_post, tri_post;
  for (size_t i = 0; i < msgs.size(); ++i) {
    std::vector<uint32_t> st, en;
    words_of(texts[i].own, st, en);
    const auto w = word_hashes(texts[i].own, st, en);
    const auto m = static_cast<uint32_t>(i);
    for (uint64_t h : w) word_post.v.push_back({h, m});
    for (size_t k = 0; k + 2 < w.size(); ++k)
      tri_post.v.push_back({trigram(w, k), m});
  }
  word_post.finish();
  tri_post.finish();

  std::vector<char> cand(msgs.size(), 0);
  std::vector<uint32_t> tri_hits(msgs.size(), 0);
  std::vector<char> seen(msgs.size(), 0);
  std::vector<size_t> order;
  std::unordered_map<size_t, std::u32string> all_cache;  // text with quotes
  for (size_t i = 0; i < msgs.size(); ++i) {
    const std::vector<Quote>& quotes = texts[i].quotes;
    if (quotes.empty()) continue;

    // Candidates: ancestors, the thread's head, then by distance (earlier
    // first). `near` ones accept shorter quotes.
    order.clear();
    std::fill(seen.begin(), seen.end(), 0);
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
      link.quote = {q.pieces.front().at.front().caret(),
                    q.pieces.back().at.back().after()};
      std::vector<uint64_t> qwords, qtris;
      for (const Index& piece : q.pieces) {
        std::vector<uint32_t> st, en;
        words_of(piece.s, st, en);
        const auto w = word_hashes(piece.s, st, en);
        qwords.insert(qwords.end(), w.begin(), w.end());
        for (size_t k = 0; k + 2 < w.size(); ++k) qtris.push_back(trigram(w, k));
      }
      size_t first = 0, last = 0;

      // Exact: only messages with the quote's rarest word.
      std::pair<size_t, size_t> rare{0, 0};
      bool possible = !qwords.empty();
      for (uint64_t h : qwords) {
        const auto r = word_post.find(h);
        if (r.first == r.second) {
          possible = false;  // a word no message has: no exact match
          break;
        }
        if (rare.first == rare.second ||
            r.second - r.first < rare.second - rare.first)
          rare = r;
      }
      bool found = false;
      if (possible) {
        for (size_t k = rare.first; k < rare.second; ++k)
          cand[word_post.v[k].second] = 1;
        for (size_t k = 0; k < order.size() && !found; ++k) {
          if (k >= near_count && !far_ok) break;
          if (!cand[order[k]]) continue;
          if (match_exact(q, texts[order[k]].own, first, last)) {
            link.source_msg = msgs[order[k]];
            link.source = selection(order[k], true, first, last);
            link.score = 1.f;
            found = true;
          }
        }
        for (size_t k = rare.first; k < rare.second; ++k)
          cand[word_post.v[k].second] = 0;
      }
      // The original is not here, but an ancestor quoted it as well.
      for (size_t k = 0; k < near_count && !found; ++k) {
        auto it = all_cache.find(order[k]);
        if (it == all_cache.end())
          it = all_cache.emplace(order[k], build_index(order[k], false, false).s)
                   .first;
        if (match_exact(q, it->second, first, last)) {
          link.source_msg = msgs[order[k]];
          link.source = selection(order[k], false, first, last);
          link.score = 1.f;
          link.via_quote = true;
          found = true;
        }
      }
      // Fuzzy: only messages that hold enough of the quote's trigrams.
      if (!found && q.words >= kFuzzyWords && !qtris.empty()) {
        std::vector<uint32_t> touched;
        std::sort(qtris.begin(), qtris.end());
        for (size_t a = 0; a < qtris.size();) {
          size_t b = a;
          while (b < qtris.size() && qtris[b] == qtris[a]) ++b;
          const auto r = tri_post.find(qtris[a]);
          for (size_t k = r.first; k < r.second; ++k) {
            const uint32_t m = tri_post.v[k].second;
            if (tri_hits[m] == 0) touched.push_back(m);
            tri_hits[m] += static_cast<uint32_t>(b - a);
          }
          a = b;
        }
        const float need = kFuzzyAccept * static_cast<float>(qtris.size());
        for (uint32_t m : touched)
          cand[m] = static_cast<float>(tri_hits[m]) >= need ? 1 : 0;
        float best = 0.f;
        size_t best_msg = 0, best_first = 0, best_last = 0;
        for (size_t k = 0; k < order.size(); ++k) {
          if (!cand[order[k]]) continue;
          const float s = match_fuzzy(q, texts[order[k]].own, first, last);
          if (s > best + 1e-6f) {
            best = s;
            best_msg = order[k];
            best_first = first;
            best_last = last;
          }
        }
        for (uint32_t m : touched) {
          cand[m] = 0;
          tri_hits[m] = 0;
        }
        if (best > 0.f) {
          link.source_msg = msgs[best_msg];
          link.source = selection(best_msg, true, best_first, best_last);
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
