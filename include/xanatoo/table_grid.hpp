// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#pragma once

#include "xanatoo/types.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace xanatoo {

// Derived occupancy; a spanning cell owns every slot in its rectangle.
struct TablePlacement {
  NodeId id = INVALID_NODE;
  size_t row = 0, col = 0, rows = 1, cols = 1;
};

struct TableGrid {
  std::vector<NodeId> rows;
  std::vector<TablePlacement> cells;
  std::vector<std::vector<NodeId>> slots;
  size_t columns = 0;
};

inline TableGrid build_table_grid(const Document& doc, NodeId table) {
  TableGrid grid;
  for (NodeId child : doc.get(table).children)
    if (doc.get(child).kind == NodeKind::TableRow) grid.rows.push_back(child);
  grid.slots.resize(grid.rows.size());
  for (size_t r = 0; r < grid.rows.size(); ++r) {
    size_t col = 0;
    for (NodeId id : doc.get(grid.rows[r]).children) {
      const Node& cell = doc.get(id);
      if (cell.kind != NodeKind::TableCell) continue;
      const size_t cs = static_cast<size_t>(std::clamp(cell.col_span, 1, 1000));
      const size_t rs = std::min(grid.rows.size() - r,
          static_cast<size_t>(std::clamp(cell.row_span, 1, 65534)));
      for (;;) {
        bool free = true;
        for (size_t c = col; c < col + cs && c < grid.slots[r].size(); ++c)
          if (grid.slots[r][c] != INVALID_NODE) { free = false; col = c + 1; break; }
        if (free) break;
      }
      for (size_t rr = r; rr < r + rs; ++rr) {
        if (grid.slots[rr].size() < col + cs)
          grid.slots[rr].resize(col + cs, INVALID_NODE);
        for (size_t cc = col; cc < col + cs; ++cc) grid.slots[rr][cc] = id;
      }
      grid.cells.push_back({id, r, col, rs, cs});
      col += cs;
      grid.columns = std::max(grid.columns, col);
    }
  }
  for (auto& row : grid.slots) row.resize(grid.columns, INVALID_NODE);
  return grid;
}

// ---- Alignment (docs/TABLES.md "Alignment") ----
// Horizontal: Decimal lines the cells of a column up at their decimal point
// (after the last digit when there is none), the group set flush right.
enum class CellAlign : unsigned char { Left, Center, Right, Decimal };
enum class CellVAlign : unsigned char { Top, Middle, Bottom, Baseline };

// Canonical spelling ("left", "center", …) of an alignment word, or empty
// when it is none. Accepts CSS / HTML / XSL-FO / LaTeX spellings.
inline std::string canonical_align(std::string_view v) {
  std::string s;
  for (char c : v)
    if (c != ' ' && c != '\t') s += static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
  if (s == "left" || s == "start" || s == "l" || s == "justify") return "left";
  if (s == "center" || s == "centre" || s == "middle" || s == "c") return "center";
  if (s == "right" || s == "end" || s == "r") return "right";
  if (s == "decimal" || s == "char" || s == "d" || s == "." || s == "s") return "decimal";
  return {};
}

inline std::string canonical_valign(std::string_view v) {
  std::string s;
  for (char c : v)
    if (c != ' ' && c != '\t') s += static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
  if (s == "top" || s == "before" || s == "t" || s == "text-top") return "top";
  if (s == "middle" || s == "center" || s == "centre" || s == "m") return "middle";
  if (s == "bottom" || s == "after" || s == "b" || s == "text-bottom") return "bottom";
  if (s == "baseline") return "baseline";
  return {};
}

// A column spec: LaTeX-style letters ("l|rr" → "lrr", `p{…}` → `l`) or
// words ("left right decimal"). Unknown parts are dropped.
inline std::string canonical_cols(std::string_view v) {
  std::string out;
  bool words = false;
  for (std::string_view w : {"left", "right", "cent", "decimal", "start", "end"})
    words |= v.find(w) != std::string_view::npos;
  if (words) {
    size_t p = 0;
    while (p < v.size()) {
      size_t e = v.find_first_of(" ,", p);
      if (e == std::string_view::npos) e = v.size();
      const std::string a = canonical_align(v.substr(p, e - p));
      if (!a.empty()) out += a[0];
      p = e + 1;
    }
    return out;
  }
  for (size_t i = 0; i < v.size(); ++i) {
    const char c = v[i];
    if (c == '{') {  // p{3cm}, @{…}: skip the argument
      const size_t e = v.find('}', i);
      if (e == std::string_view::npos) break;
      i = e;
      continue;
    }
    if (c == 'l' || c == 'L' || c == 'p' || c == 'X') out += 'l';
    else if (c == 'c' || c == 'C') out += 'c';
    else if (c == 'r' || c == 'R') out += 'r';
    else if (c == 'd' || c == 'D' || c == 'S') out += 'd';
  }
  return out;
}

// A table width as a percentage of the column: "100%", "full" (100);
// "auto", lengths and anything else: 0 (as wide as the content).
inline int table_width_percent(std::string_view v) {
  while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
  while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.remove_suffix(1);
  if (v == "full") return 100;
  if (v.size() < 2 || v.back() != '%') return 0;
  double n = 0;
  size_t i = 0;
  for (; i + 1 < v.size() && v[i] >= '0' && v[i] <= '9'; ++i) n = n * 10 + (v[i] - '0');
  if (i + 1 < v.size() && v[i] == '.') {
    double f = 0.1;
    for (++i; i + 1 < v.size() && v[i] >= '0' && v[i] <= '9'; ++i, f /= 10) n += (v[i] - '0') * f;
  }
  if (i + 1 != v.size() || n <= 0) return 0;
  return n >= 100 ? 100 : std::max(1, static_cast<int>(n + 0.5));
}

// Text that reads as a number: digits with separators, sign, currency,
// percent ("1,234.5", "−3 %", "$12"); at least one digit.
inline bool numeric_text(std::string_view v) {
  bool digit = false;
  for (size_t i = 0; i < v.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(v[i]);
    if (c >= '0' && c <= '9') { digit = true; continue; }
    if (c == '.' || c == ',' || c == '+' || c == '-' || c == '%' || c == ' ' ||
        c == '$' || c == '\'' || c == '\t')
      continue;
    // − (U+2212), € (U+20AC), £ (U+00A3), no-break / thin spaces.
    if (v.substr(i).starts_with("\u2212") || v.substr(i).starts_with("\u20ac") ||
        v.substr(i).starts_with("\u2009") || v.substr(i).starts_with("\u202f")) {
      i += 2;
      continue;
    }
    if (v.substr(i).starts_with("\u00a3") || v.substr(i).starts_with("\u00a0")) {
      i += 1;
      continue;
    }
    return false;
  }
  return digit;
}

inline CellAlign cell_align_of(std::string_view canonical) {
  if (canonical == "center") return CellAlign::Center;
  if (canonical == "right") return CellAlign::Right;
  if (canonical == "decimal") return CellAlign::Decimal;
  return CellAlign::Left;
}

inline CellVAlign cell_valign_of(std::string_view canonical) {
  if (canonical == "middle") return CellVAlign::Middle;
  if (canonical == "bottom") return CellVAlign::Bottom;
  if (canonical == "baseline") return CellVAlign::Baseline;
  return CellVAlign::Top;
}

// Head rows: rows marked as such (HTML thead); in a table without marked
// rows, the leading rows of header cells only (a GFM table's first row),
// when body rows follow.
inline std::vector<bool> table_head_rows(const Document& doc, const TableGrid& grid) {
  std::vector<bool> head(grid.rows.size(), false);
  bool marked = false;
  for (size_t r = 0; r < grid.rows.size(); ++r) {
    head[r] = doc.get(grid.rows[r]).level == 1;
    marked |= doc.get(grid.rows[r]).level != 0;
  }
  if (marked) return head;
  size_t r = 0;
  for (; r < grid.rows.size(); ++r) {
    bool all = false;
    for (NodeId id : doc.get(grid.rows[r]).children) {
      const Node& c = doc.get(id);
      if (c.kind != NodeKind::TableCell) continue;
      if (c.level == 0) { all = false; break; }
      all = true;
    }
    if (!all) break;
  }
  if (r < grid.rows.size())
    for (size_t i = 0; i < r; ++i) head[i] = true;
  return head;
}

// Horizontal alignment of a placed cell: the cell's own, its row's, its
// first column's letter in the table's `cols`; header cells of a decimal
// column are centred over the numbers.
inline CellAlign table_cell_align(const Document& doc, NodeId table,
                                  const TablePlacement& p) {
  const Node& cell = doc.get(p.id);
  std::string a = canonical_align(cell.align);
  if (a.empty() && cell.parent != INVALID_NODE) a = canonical_align(doc.get(cell.parent).align);
  if (a.empty()) {
    const std::string& cols = doc.get(table).cols;
    if (p.col < cols.size()) a = canonical_align(std::string_view(&cols[p.col], 1));
  }
  CellAlign out = cell_align_of(a);
  if (out == CellAlign::Decimal && (cell.level > 0 || p.cols > 1) && cell.align.empty())
    out = CellAlign::Center;
  return out;
}

// Vertical alignment: cell, row, table; then top for data and bottom for
// header cells (column labels sit on the rule under them).
inline CellVAlign table_cell_valign(const Document& doc, NodeId table,
                                    const TablePlacement& p) {
  const Node& cell = doc.get(p.id);
  std::string v = canonical_valign(cell.valign);
  if (v.empty() && cell.parent != INVALID_NODE) v = canonical_valign(doc.get(cell.parent).valign);
  if (v.empty()) v = canonical_valign(doc.get(table).valign);
  if (v.empty()) return cell.level > 0 ? CellVAlign::Bottom : CellVAlign::Top;
  return cell_valign_of(v);
}

// Column widths (CSS automatic table layout, simplified): every column its
// widest content when that fits `target`; else each gets its narrowest
// (longest word) plus a share of the rest in proportion to how much more
// it would like. `fill`: grow to `target` (an explicit table width).
inline std::vector<float> distribute_table_widths(const std::vector<float>& min_w,
                                                  const std::vector<float>& max_w,
                                                  float target, bool fill) {
  const size_t n = max_w.size();
  std::vector<float> w(n, 0.f);
  float sum_max = 0.f, sum_min = 0.f;
  for (size_t c = 0; c < n; ++c) {
    sum_max += max_w[c];
    sum_min += std::min(c < min_w.size() ? min_w[c] : max_w[c], max_w[c]);
  }
  if (n == 0) return w;
  if (sum_max <= target) {
    const float k = fill && sum_max > 0.f ? target / sum_max : 1.f;
    for (size_t c = 0; c < n; ++c) w[c] = max_w[c] * k;
    if (fill && sum_max <= 0.f)
      for (size_t c = 0; c < n; ++c) w[c] = target / static_cast<float>(n);
    return w;
  }
  if (sum_min >= target) {
    const float k = sum_min > 0.f ? target / sum_min : 1.f;
    for (size_t c = 0; c < n; ++c) w[c] = std::min(min_w[c], max_w[c]) * k;
    return w;
  }
  const float share = (target - sum_min) / (sum_max - sum_min);
  for (size_t c = 0; c < n; ++c) {
    const float lo = std::min(min_w[c], max_w[c]);
    w[c] = lo + (max_w[c] - lo) * share;
  }
  return w;
}

}  // namespace xanatoo
