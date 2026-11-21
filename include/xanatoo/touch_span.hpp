// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Two-finger window resize (app_touch.cpp / app_windows.cpp): one finger
// holds a free window's title bar, a second finger lands anywhere; from then
// on each edge follows the finger on its side, so spreading the fingers
// grows the window and moving both together moves it.

#include <algorithm>
#include <cmath>

namespace xanatoo {

struct SpanAxis {
  int lo = 0, len = 0;
};

// One axis of the span resize. The start interval is [lo, lo + len); finger
// a started at a0, finger b at b0 and they are now at a / b. The edge on
// a's side (lo when a0 <= b0, else hi) follows a, the other edge follows b.
// The result stays within [bound_lo, bound_hi] and at least min_len long
// (when the bounds allow); a too-short interval grows on b's edge first.
inline SpanAxis span_axis(int lo, int len, float a0, float b0, float a,
                          float b, int min_len, int bound_lo, int bound_hi) {
  const int da = static_cast<int>(std::lround(a - a0));
  const int db = static_cast<int>(std::lround(b - b0));
  const bool a_low = a0 <= b0;
  int nlo = lo + (a_low ? da : db);
  int nhi = lo + len + (a_low ? db : da);
  if (nhi < nlo) nhi = nlo;  // fingers crossed: collapse, then grow below
  nlo = std::clamp(nlo, bound_lo, bound_hi);
  nhi = std::clamp(nhi, bound_lo, bound_hi);
  if (nhi - nlo < min_len) {
    if (a_low) {  // b drives hi
      nhi = std::min(bound_hi, nlo + min_len);
      nlo = std::max(bound_lo, nhi - min_len);
    } else {  // b drives lo
      nlo = std::max(bound_lo, nhi - min_len);
      nhi = std::min(bound_hi, nlo + min_len);
    }
  }
  return {nlo, nhi - nlo};
}

}  // namespace xanatoo
