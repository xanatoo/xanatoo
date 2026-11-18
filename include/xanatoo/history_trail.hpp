// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// History panel as a trail tree (docs/HISTORY.md "C"): visits newest
// first, each drawn with a lane graph like `git log --graph`. Pure data,
// no SDL; the panel draws the rows.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace xanatoo {

struct VisitRecord;

// One node of the list: a visit id and the visit it was reached from.
struct TrailNode {
  int64_t id = 0;
  int64_t source = 0;  // 0 = starts a trail
};

// Lane geometry of one row. Lines in the upper half of the row run from
// the columns active above it into the row; lines in the lower half leave
// the dot (or pass by) towards the rows below.
struct LaneRow {
  int lane = 0;              // column of this row's dot
  bool from_above = false;   // a newer visit in `lane` was reached from here
  bool to_below = false;     // `lane` continues to this visit's source
  std::vector<int> merges;   // other columns whose line ends at the dot
  std::vector<int> through;  // columns passing by (vertical, whole row)
  int width = 1;             // columns used in this row (dot included)
};

// Assigns lanes in one pass, newest → oldest, the way git does: a visit
// takes the lane of its newest child, further children's lanes merge into
// it at the visit, a visit without children opens the leftmost free lane,
// and a lane ends at a visit without source. A source that is not in
// `nodes` (older than the loaded page, or dropped) keeps its lane open to
// the end of the list.
std::vector<LaneRow> history_lanes(const std::vector<TrailNode>& nodes);

// Columns with a line crossing the gap just below `row` (for separator
// rows such as day headers inserted between two rows).
std::vector<int> lanes_below(const LaneRow& row);

// A row of the History panel: one visit, or a run of visits within one
// page (anchors, reloads) that is shown once.
struct TrailEntry {
  size_t first = 0;   // index of the newest visit of the run
  size_t count = 1;   // visits in the run (first … first+count-1)
};

// Address without its `#fragment` (the page the anchor is in).
std::string_view trail_page_of(std::string_view address);

// Groups runs of visits (newest first) of the same page where each older
// one is the source of the newer one directly above it, reached by an
// anchor jump (link / nav to another #fragment) or a reload: they collapse
// into one entry. Other kinds (a session restore, the same address opened
// again) stay rows of their own.
std::vector<TrailEntry> collapse_page_runs(
    const std::vector<VisitRecord>& visits);

// The trail nodes of `entries`: each entry carries the newest visit's id
// and the oldest one's source; sources pointing into a run are redirected
// to the run's id. With `known_only`, sources not among `visits` become 0
// (a filtered list shows only the links between the rows it has).
std::vector<TrailNode> trail_nodes(const std::vector<VisitRecord>& visits,
                                   const std::vector<TrailEntry>& entries,
                                   bool known_only);

// Case-insensitive (ASCII) substring match on title and address; an empty
// filter matches everything.
bool visit_matches(const VisitRecord& v, std::string_view filter);

}  // namespace xanatoo
