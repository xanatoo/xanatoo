// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Two-finger gutter grab (app_touch.cpp): the gutter point on the line
// between two fingers, searched from their midpoint outward so the gutter
// nearest the middle wins when the fingers span several.

#include <cmath>
#include <optional>
#include <utility>

namespace xanatoo {

// First point (1 px steps) on the segment (x0,y0)-(x1,y1), nearest its
// midpoint, for which `hit(x, y)` holds; nullopt if there is none.
template <class Hit>
std::optional<std::pair<int, int>> segment_hit_near_middle(
    float x0, float y0, float x1, float y1, Hit&& hit) {
  const float len = std::hypot(x1 - x0, y1 - y0);
  const int half = static_cast<int>(std::ceil(len * 0.5f));
  const float mx = (x0 + x1) * 0.5f, my = (y0 + y1) * 0.5f;
  const float ux = len > 0.f ? (x1 - x0) / len : 0.f;
  const float uy = len > 0.f ? (y1 - y0) / len : 0.f;
  for (int d = 0; d <= half; ++d) {
    for (int sign : {1, -1}) {
      if (d == 0 && sign < 0) continue;
      const float t = static_cast<float>(d * sign);
      if (std::abs(t) > len * 0.5f + 0.5f) continue;
      const int x = static_cast<int>(std::floor(mx + ux * t));
      const int y = static_cast<int>(std::floor(my + uy * t));
      if (hit(x, y)) return std::pair{x, y};
    }
  }
  return std::nullopt;
}

}  // namespace xanatoo
