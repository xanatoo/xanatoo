// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#include "xanatoo/edit_bar.hpp"

#include <cassert>

// The strip's height is reserved by the app before ImGui wraps its groups:
// both must agree on when a group moves to the next row.
int main() {
  using namespace xanatoo;
  const float step = edit_bar_button_step(1.f);  // 42
  int buttons = 0;
  for (int g : kEditBarGroups) buttons += g;
  constexpr int groups = sizeof kEditBarGroups / sizeof kEditBarGroups[0];
  const float one_row = static_cast<float>(buttons) * step +
                        static_cast<float>(groups - 1) * kEditBarSeparatorStep +
                        2.f * kEditBarPadX;
  assert(edit_bar_rows(one_row, 1.f) == 1);
  assert(edit_bar_rows(one_row - 1.f, 1.f) == 2);
  assert(edit_bar_height(one_row, 1.f) == 36 + 8);
  assert(edit_bar_height(one_row - 1.f, 1.f) == 2 * 36 + 4 + 8);
  // Narrower than the widest group: that group wraps by button.
  const float three = 3.f * step + 2.f * kEditBarPadX;
  assert(edit_bar_rows(three, 1.f) > buttons / 3);
  // Groups never split while they fit a row: {4} {3} need two rows here.
  const int two_groups[] = {4, 3};
  assert(edit_bar_rows(4.f * step + 2.f * kEditBarPadX, 1.f, two_groups) == 2);
  assert(edit_bar_rows(7.f * step + kEditBarSeparatorStep + 2.f * kEditBarPadX,
                       1.f, two_groups) == 1);
  // Scale grows the rows.
  assert(edit_bar_height(one_row * 3.f, 2.f) == 72 + 8);
  return 0;
}
