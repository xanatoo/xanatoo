// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/history_trail.hpp"
#include "xanatoo/util.hpp"
#include "xanatoo/visits.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace xanatoo {

std::vector<LaneRow> history_lanes(const std::vector<TrailNode>& nodes) {
  std::vector<LaneRow> rows;
  rows.reserve(nodes.size());
  // Per column: the visit id the line coming down it is waiting for
  // (0 = free column).
  std::vector<int64_t> awaiting;
  for (const TrailNode& n : nodes) {
    LaneRow r;
    int lane = -1;
    for (size_t c = 0; c < awaiting.size(); ++c) {
      if (awaiting[c] == 0) continue;
      if (n.id != 0 && awaiting[c] == n.id) {
        if (lane < 0) {
          lane = static_cast<int>(c);
          r.from_above = true;
        } else {
          r.merges.push_back(static_cast<int>(c));
        }
      } else {
        r.through.push_back(static_cast<int>(c));
      }
    }
    if (lane < 0) {  // newest visit of its trail: leftmost free column
      const auto it = std::find(awaiting.begin(), awaiting.end(), 0);
      lane = static_cast<int>(it - awaiting.begin());
      if (it == awaiting.end()) awaiting.push_back(0);
    }
    r.lane = lane;
    for (int m : r.merges) awaiting[static_cast<size_t>(m)] = 0;
    // A source equal to the visit itself would wait forever on itself.
    const int64_t src = n.source != n.id ? n.source : 0;
    awaiting[static_cast<size_t>(lane)] = src;
    r.to_below = src != 0;
    int width = lane + 1;
    for (int c : r.through) width = std::max(width, c + 1);
    for (int c : r.merges) width = std::max(width, c + 1);
    r.width = width;
    while (!awaiting.empty() && awaiting.back() == 0) awaiting.pop_back();
    rows.push_back(std::move(r));
  }
  return rows;
}

std::vector<int> lanes_below(const LaneRow& row) {
  std::vector<int> out = row.through;
  if (row.to_below) out.push_back(row.lane);
  std::sort(out.begin(), out.end());
  return out;
}

std::string_view trail_page_of(std::string_view address) {
  const size_t hash = address.find('#');
  return hash == std::string_view::npos ? address : address.substr(0, hash);
}

std::vector<TrailEntry> collapse_page_runs(
    const std::vector<VisitRecord>& visits) {
  std::vector<TrailEntry> out;
  for (size_t i = 0; i < visits.size(); ++i) {
    if (!out.empty()) {
      TrailEntry& last = out.back();
      const VisitRecord& older_end = visits[last.first + last.count - 1];
      const VisitRecord& v = visits[i];
      // `older_end` is the newer of the two: reached from `v` by an
      // anchor jump inside the page, or a reload.
      const bool in_page =
          older_end.kind == VisitKind::Reload ||
          ((older_end.kind == VisitKind::Link ||
            older_end.kind == VisitKind::Nav) &&
           older_end.address != v.address);
      if (in_page && older_end.source != 0 && older_end.source == v.id &&
          trail_page_of(older_end.address) == trail_page_of(v.address)) {
        ++last.count;
        continue;
      }
    }
    out.push_back(TrailEntry{i, 1});
  }
  return out;
}

std::vector<TrailNode> trail_nodes(const std::vector<VisitRecord>& visits,
                                   const std::vector<TrailEntry>& entries,
                                   bool known_only) {
  std::unordered_map<int64_t, int64_t> run_of;  // member id → run's id
  for (const TrailEntry& e : entries)
    for (size_t k = 0; k < e.count; ++k)
      run_of[visits[e.first + k].id] = visits[e.first].id;
  std::vector<TrailNode> out;
  out.reserve(entries.size());
  for (const TrailEntry& e : entries) {
    TrailNode n;
    n.id = visits[e.first].id;
    n.source = visits[e.first + e.count - 1].source;
    if (n.source != 0) {
      const auto it = run_of.find(n.source);
      if (it != run_of.end())
        n.source = it->second;
      else if (known_only)
        n.source = 0;
    }
    out.push_back(n);
  }
  return out;
}

bool visit_matches(const VisitRecord& v, std::string_view filter) {
  if (filter.empty()) return true;
  const std::string f = ascii_lower(filter);
  return ascii_lower(v.title).find(f) != std::string::npos ||
         ascii_lower(v.address).find(f) != std::string::npos;
}

}  // namespace xanatoo
