// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
//
// Offline tests for the History panel's trail tree (history_trail.hpp):
// lane assignment, page runs, filtering. Part of xanatoo_tests
// (test_history_trail is called from unit_tests.cpp).

#include "xanatoo/history_trail.hpp"
#include "xanatoo/visits.hpp"

#include <cstdio>
#include <vector>

namespace {
int g_passed = 0;
int g_failed = 0;
void expect(bool cond, const char* expr, const char* file, int line) {
  if (cond) {
    ++g_passed;
    return;
  }
  ++g_failed;
  std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
}

xanatoo::VisitRecord visit(int64_t id, int64_t source, const char* address,
                           const char* title = "") {
  xanatoo::VisitRecord v;
  v.id = id;
  v.source = source;
  v.address = address;
  v.title = title;
  return v;
}
}  // namespace

#define EXPECT(c) expect(!!(c), #c, __FILE__, __LINE__)
#define EXPECT_EQ(a, b) expect((a) == (b), #a " == " #b, __FILE__, __LINE__)

void test_history_trail(int& passed, int& failed) {
  using namespace xanatoo;
  using V = std::vector<int>;

  // One straight trail: 3 ← 2 ← 1 (1 typed, no source).
  {
    const auto rows = history_lanes({{3, 2}, {2, 1}, {1, 0}});
    EXPECT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0].lane, 0);
    EXPECT(!rows[0].from_above);
    EXPECT(rows[0].to_below);
    EXPECT(rows[1].from_above && rows[1].to_below);
    EXPECT(rows[2].from_above && !rows[2].to_below);
    EXPECT(rows[2].through.empty());
    EXPECT_EQ(rows[2].width, 1);
    EXPECT(lanes_below(rows[2]).empty());
    EXPECT((lanes_below(rows[0]) == V{0}));
  }

  // The plan's example: a fork from 1 (HN) into 4 and 3 (another tab).
  //   5 ← 4 ← 1, 3 ← 1, 2 typed earlier with nothing after it… newest first:
  //   5 (src 4), 3 (src 1), 4 (src 1), 1 (src 0), 0-less 2 below.
  {
    const auto rows =
        history_lanes({{5, 4}, {3, 1}, {4, 1}, {1, 0}, {2, 0}});
    EXPECT_EQ(rows[0].lane, 0);  // 5 opens lane 0, awaits 4
    EXPECT_EQ(rows[1].lane, 1);  // 3 opens lane 1, awaits 1
    EXPECT((rows[1].through == V{0}));
    EXPECT_EQ(rows[1].width, 2);
    EXPECT_EQ(rows[2].lane, 0);  // 4 continues lane 0, now awaits 1
    EXPECT(rows[2].from_above);
    EXPECT((rows[2].through == V{1}));
    EXPECT_EQ(rows[3].lane, 0);  // 1: lane 1 merges into lane 0
    EXPECT((rows[3].merges == V{1}));
    EXPECT(rows[3].through.empty());
    EXPECT(!rows[3].to_below);
    EXPECT_EQ(rows[4].lane, 0);  // 2: new trail in the freed lane 0
    EXPECT(!rows[4].from_above);
    EXPECT_EQ(rows[4].width, 1);
  }

  // A lane freed on the left is reused; trailing free lanes shrink width.
  {
    // 6 (src 1), 5 (src 2), 2 (src 0), 4 (src 3), 3 (src 0), 1 (src 0)
    const auto rows = history_lanes(
        {{6, 1}, {5, 2}, {2, 0}, {4, 3}, {3, 0}, {1, 0}});
    EXPECT_EQ(rows[1].lane, 1);
    EXPECT_EQ(rows[2].lane, 1);   // 2 ends lane 1
    EXPECT_EQ(rows[3].lane, 1);   // 4 reuses free lane 1
    EXPECT((rows[3].through == V{0}));
    EXPECT_EQ(rows[4].lane, 1);
    EXPECT_EQ(rows[5].lane, 0);
    EXPECT_EQ(rows[5].width, 1);
    EXPECT((lanes_below(rows[4]) == V{0}));
  }

  // A source outside the list keeps its lane open to the end.
  {
    const auto rows = history_lanes({{9, 100}, {8, 0}});
    EXPECT(rows[0].to_below);
    EXPECT_EQ(rows[1].lane, 1);
    EXPECT((rows[1].through == V{0}));
    EXPECT((lanes_below(rows[1]) == V{0}));
  }

  // Self-reference does not hang a lane.
  {
    const auto rows = history_lanes({{7, 7}, {6, 0}});
    EXPECT(!rows[0].to_below);
    EXPECT_EQ(rows[1].lane, 0);
  }

  // Page runs: anchors and reloads in one page collapse; a different page,
  // a missing source link or a restore breaks the run.
  {
    auto k = [](VisitRecord v, VisitKind kind) {
      v.kind = kind;
      return v;
    };
    std::vector<VisitRecord> vs = {
        k(visit(8, 7, "https://a/x#c"), VisitKind::Restore),
        k(visit(7, 6, "https://a/x#c"), VisitKind::Reload),
        k(visit(6, 5, "https://a/x#c"), VisitKind::Link),  // anchor in x
        k(visit(5, 4, "https://a/x#b"), VisitKind::Nav),   // anchor in x
        k(visit(4, 3, "https://a/x"), VisitKind::Link),    // link from y
        k(visit(3, 0, "https://a/y"), VisitKind::Address),
        k(visit(2, 0, "https://a/y"), VisitKind::Address), // same page again
        k(visit(1, 0, "https://b/"), VisitKind::Address),
    };
    const auto entries = collapse_page_runs(vs);
    EXPECT_EQ(entries.size(), 5u);
    EXPECT_EQ(entries[0].count, 1u);  // the restore stays its own row
    EXPECT_EQ(entries[1].first, 1u);
    EXPECT_EQ(entries[1].count, 4u);  // reload + two anchors + the page
    EXPECT_EQ(entries[2].first, 5u);
    EXPECT_EQ(entries[3].first, 6u);
    const auto nodes = trail_nodes(vs, entries, false);
    EXPECT_EQ(nodes[0].source, 7);   // into the run below it
    EXPECT_EQ(nodes[1].id, 7);
    EXPECT_EQ(nodes[1].source, 3);   // the oldest member's source
    EXPECT_EQ(nodes[2].source, 0);
  }

  // A child of a run member points at the run; known_only drops sources
  // that are not in the list.
  {
    std::vector<VisitRecord> vs = {
        visit(10, 8, "https://c/"),     // new tab from the anchor visit 8
        visit(9, 8, "https://a/x#b"),
        visit(8, 7, "https://a/x#a"),
        visit(7, 2, "https://a/x"),     // source 2 is not loaded
    };
    const auto entries = collapse_page_runs(vs);
    EXPECT_EQ(entries.size(), 2u);
    auto nodes = trail_nodes(vs, entries, false);
    EXPECT_EQ(nodes[0].source, 9);  // redirected to the run's id
    EXPECT_EQ(nodes[1].source, 2);
    nodes = trail_nodes(vs, entries, true);
    EXPECT_EQ(nodes[1].source, 0);
  }

  EXPECT(trail_page_of("https://a/x#y") == "https://a/x");
  EXPECT(trail_page_of("file:///tmp/a.md") == "file:///tmp/a.md");

  {
    const VisitRecord v = visit(1, 0, "https://Example.org/Rust", "Closures");
    EXPECT(visit_matches(v, ""));
    EXPECT(visit_matches(v, "example"));
    EXPECT(visit_matches(v, "CLOS"));
    EXPECT(!visit_matches(v, "python"));
  }

  passed += g_passed;
  failed += g_failed;
}
