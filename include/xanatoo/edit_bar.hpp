// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Geometry of the edit strip that sits on top of an editable view (docs/UI.md
// "Editable documents"). Pure: the app reserves the strip's height above the
// text with it, the ImGui strip wraps its button groups the same way.

#include <cmath>
#include <span>

namespace xanatoo {

// Buttons per group, left to right: lock · save · undo · redo |
// cut · copy · paste | bold · italic · link · more | structure | lists |
// insert.
inline constexpr int kEditBarGroups[] = {4, 3, 4, 1, 1, 1};

inline float edit_bar_button_step(float scale) { return 32.f * scale + 10.f; }
inline constexpr float kEditBarSeparatorStep = 15.f;
inline constexpr float kEditBarPadX = 6.f;  // window padding, each side
inline constexpr float kEditBarPadY = 4.f;
inline float edit_bar_row_h(float scale) { return 36.f * scale; }
inline float edit_bar_row_gap(float scale) { return 4.f * scale; }

// Rows the groups need in a strip `width` wide: a group moves to the next
// row whole unless it is wider than a row by itself.
inline int edit_bar_rows(float width, float scale,
                         std::span<const int> groups = kEditBarGroups) {
  const float avail = width - 2.f * kEditBarPadX;
  const float step = edit_bar_button_step(scale);
  int rows = 1;
  float used = 0.f;
  for (int count : groups) {
    const float group_w = static_cast<float>(count) * step;
    if (used > 0.f && used + kEditBarSeparatorStep + group_w > avail) {
      ++rows;
      used = 0.f;
    } else if (used > 0.f) {
      used += kEditBarSeparatorStep;
    }
    if (group_w <= avail) {
      used += group_w;
      continue;
    }
    for (int i = 0; i < count; ++i) {
      if (used > 0.f && used + step > avail) {
        ++rows;
        used = 0.f;
      }
      used += step;
    }
  }
  return rows;
}

inline int edit_bar_height(float width, float scale) {
  const int rows = edit_bar_rows(width, scale);
  return static_cast<int>(std::lround(
      static_cast<float>(rows) * edit_bar_row_h(scale) +
      static_cast<float>(rows - 1) * edit_bar_row_gap(scale) +
      2.f * kEditBarPadY));
}

}  // namespace xanatoo
