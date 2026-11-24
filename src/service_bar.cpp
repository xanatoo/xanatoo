// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
//
// Which service an address belongs to, and its strip (service_bar.hpp). The
// services describe their own sections: hn_service_bar (hackernews.cpp),
// forum_service_bar (forums.cpp and forum_*.cpp), nntp_service_bar (nntp.cpp).

#include "xanatoo/service_bar.hpp"

#include "xanatoo/forums.hpp"
#include "xanatoo/hackernews.hpp"
#include "xanatoo/nntp.hpp"
#include "xanatoo/util.hpp"

#include <cmath>

namespace xanatoo {

std::optional<ServiceBar> service_bar(std::string_view address) {
  trim_ascii(address);
  if (address.empty()) return std::nullopt;
  if (auto hn = hn_service_bar(address)) return hn;
  if (auto r = parse_forum_ref(address)) return forum_service_bar(*r);
  if (starts_with_ci(address, "https://") || starts_with_ci(address, "http://")) {
    // A forum's own web pages (lobste.rs, …) belong to it as well.
    const std::string ref = forum_ref_from_web_url(address);
    if (!ref.empty())
      if (auto r = parse_forum_ref(ref)) return forum_service_bar(*r);
    return std::nullopt;
  }
  if (auto r = parse_nntp_url(address)) return nntp_service_bar(*r);
  return std::nullopt;
}

uint32_t service_color_for(std::string_view id) {
  // FNV-1a → hue; fixed saturation and value keep text on the tint legible.
  uint32_t h = 2166136261u;
  for (unsigned char c : id) h = (h ^ c) * 16777619u;
  const float hue = static_cast<float>(h % 360u) / 60.f;
  const float v = 0.62f, s = 0.70f;
  const float c = v * s;
  const float x = c * (1.f - std::abs(std::fmod(hue, 2.f) - 1.f));
  float r = 0, g = 0, b = 0;
  switch (static_cast<int>(hue)) {
    case 0: r = c; g = x; break;
    case 1: r = x; g = c; break;
    case 2: g = c; b = x; break;
    case 3: g = x; b = c; break;
    case 4: r = x; b = c; break;
    default: r = c; b = x; break;
  }
  const float m = v - c;
  auto byte = [&](float f) { return static_cast<uint32_t>(std::lround((f + m) * 255.f)); };
  return byte(r) << 16 | byte(g) << 8 | byte(b);
}

}  // namespace xanatoo
