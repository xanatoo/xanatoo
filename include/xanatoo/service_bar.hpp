// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// The service strip (docs/UI.md "Service strips"): the toolbar on top of a
// page of a native service (Hacker News, Lobsters, Bluesky, Usenet, …). It
// carries what used to be the "Hacker News: Top · New · Best" line at the
// top of each generated page — the service's sections — plus the service's
// identity (name, favicon, accent colour, also used for its tabs) and its
// search engine. Pure: everything follows from the page's address, so the
// strip is there while a page is still loading and stays put across all
// pages of the service; the generated documents carry no navigation rows.

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xanatoo {

struct ServiceLink {
  std::string text;
  std::string ref;       // address to open; "" = plain text (the current page)
  bool current = false;  // the section the page belongs to
};

// One line of links. The first row follows the service's name; further
// rows ("Sort", "From") start with their label.
struct ServiceRow {
  std::string label;
  std::vector<ServiceLink> links;
};

struct ServiceBar {
  std::string id;      // "hn", "lobsters", "discourse:HOST", "nntp:HOST", …
  std::string name;    // "Hacker News", a forum's host
  std::string home;    // address of the service's front page
  uint32_t color = 0;  // accent 0xRRGGBB (strip, tabs)
  std::vector<ServiceRow> rows;
};

// The strip for a page address (the locator's base, no selectors), or
// nullopt when the address belongs to no native service.
std::optional<ServiceBar> service_bar(std::string_view address);

// Extra groups of links for the Page Navigation panel, generated from a
// native service address independently of both the document and the strip.
std::vector<ServiceRow> service_navigation(std::string_view address);

// Accent of a service without a colour of its own (a Discourse or Flarum
// host): a hue derived from the id, dark enough for a light strip.
uint32_t service_color_for(std::string_view id);

// Geometry, shared by the app (Pane::head_h) and the ImGui strip.
inline float service_bar_row_h(float scale) { return 26.f * scale; }
inline constexpr float kServiceBarPadX = 6.f;
inline constexpr float kServiceBarPadY = 3.f;

inline int service_bar_height(const ServiceBar& bar, float scale) {
  const int rows = bar.rows.size() > 1 ? static_cast<int>(bar.rows.size()) : 1;
  return static_cast<int>(std::lround(static_cast<float>(rows) *
                                          service_bar_row_h(scale) +
                                      2.f * kServiceBarPadY));
}

}  // namespace xanatoo
