// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/examples.hpp"
#include "xanatoo/ui.hpp"
#include "xanatoo/edit_bar.hpp"
#include "xanatoo/snapshot.hpp"
#include "xanatoo/locator.hpp"
#include "xanatoo/theme.hpp"
#include "xanatoo/view_settings.hpp"
#include <cmath>
#include "xanatoo/font.hpp"
#include "xanatoo/nostr.hpp"
#include "xanatoo/paths.hpp"
#include "xanatoo/platform.hpp"
#include "xanatoo/reddit_credentials.hpp"
#include "xanatoo/catalog_credentials.hpp"
#include "xanatoo/reply.hpp"
#include "xanatoo/search.hpp"
#include "xanatoo/util.hpp"
#include "ui_panel_imgui_order.hpp"

#include "imgui.h"
#include "imgui_internal.h"
#if defined(XANATOO_SDL3) && XANATOO_SDL3
#include "xanatoo/imgui_sdl3.hpp"
#else
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"
#endif

#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <functional>
#include <unordered_map>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#define STBI_ONLY_PNG
#include "stb_image.h"

namespace fs = std::filesystem;

// ---- Help panel: Dear ImGui item hooks ----
// With IMGUI_ENABLE_TEST_ENGINE every labelled widget reports its label
// (ImGuiTestEngineHook_ItemInfo) right after it is submitted; while
// TestEngineHookItems is on (Ui::set_hover_tracking) the hovered one is
// kept for the Help panel. No test engine is linked: the other hooks are
// empty.
namespace {
xanatoo::UiHoverItem g_hover_capture;
}  // namespace

void ImGuiTestEngineHook_ItemAdd(ImGuiContext*, ImGuiID, const ImRect&,
                                 const ImGuiLastItemData*) {}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext* ctx, ImGuiID id,
                                  const char* label, ImGuiItemStatusFlags) {
  // Begin() also registers whole windows; those are not the last item.
  if (!label || !ctx || ctx->LastItemData.ID != id) return;
  if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) return;
  g_hover_capture.label = label;
  g_hover_capture.tip.clear();
}
void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) {
  return nullptr;
}

namespace xanatoo {

namespace {
// A widget whose label says little (icon buttons): its help key and live
// tooltip, when it is the hovered item.
void note_hover_help(const char* key, const char* tip) {
  if (!ImGui::GetCurrentContext()->TestEngineHookItems) return;
  if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) return;
  g_hover_capture.label = key ? key : "";
  g_hover_capture.tip = tip ? tip : "";
}
}  // namespace

// Fit covers as well as square favicons into the available icon box.
static ImVec2 icon_size(SDL_Texture* texture, float side) {
#if XANATOO_SDL3
  float w = 0, h = 0;
  if (!SDL_GetTextureSize(texture, &w, &h) || w <= 0 || h <= 0)
    return ImVec2(side, side);
#else
  int w = 0, h = 0;
  if (SDL_QueryTexture(texture, nullptr, nullptr, &w, &h) != 0 || w <= 0 || h <= 0)
    return ImVec2(side, side);
#endif
  const float factor = side / static_cast<float>(std::max(w, h));
  return ImVec2(w * factor, h * factor);
}



namespace {

// The immediate-mode menu helpers below are free functions shared by several
// menus. Keep their current renderer's icon textures available for the frame.
std::unordered_map<std::string, SDL_Texture*> menu_icon_textures;

// Middle-click on the item just drawn: open `addr` in a new tab instead.
bool middle_open(UiCommand& cmd, std::string& arg, const std::string& addr) {
  if (addr.empty() || !ImGui::IsItemHovered() ||
      !ImGui::IsMouseReleased(ImGuiMouseButton_Middle))
    return false;
  cmd = UiCommand::OpenLinkInTab;
  arg = addr;
  return true;
}

// First and last `keep` bytes (on character boundaries) around an ellipsis;
// a string too short to gain from eliding comes back unchanged.
std::string elide_middle(const std::string& s, size_t keep) {
  if (s.size() <= 2 * keep + 3) return s;
  auto cont = [&](size_t i) {
    return i < s.size() && (static_cast<unsigned char>(s[i]) & 0xc0) == 0x80;
  };
  size_t head = keep, tail = s.size() - keep;
  while (head > 0 && cont(head)) --head;
  while (cont(tail)) ++tail;
  return s.substr(0, head) + "…" + s.substr(tail);
}

bool near_eq(float a, float b, float eps) { return std::fabs(a - b) < eps; }

// Toolbar buttons are flat until hovered/pressed, unlike the always-raised
// buttons in dialogs and panels. Icon and text fallbacks share this style.
int push_toolbar_button_style(bool latched = false) {
  const theme::Palette& p = theme::palette();
  ImGuiStyle& s = ImGui::GetStyle();
  auto color = [](theme::Rgb c, float alpha = 1.f) {
    return ImVec4(c.r / 255.f, c.g / 255.f, c.b / 255.f, alpha);
  };
  // An inactive tool is flat; an enabled toggle remains sunken even after
  // hover ends. Set these colours here (not outside the helper): ImageButton
  // uses the helper's last pushed style, just like the text fallback.
  ImGui::PushStyleColor(ImGuiCol_Button,
                        latched ? color(p.face_dark) : color(p.face_light, 0.f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                        color(latched ? p.face_dark : p.highlight));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, color(p.face_dark));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, s.FrameRounding);
  return 3;
}

void draw_button_inner_bevel(bool latched = false);

// Only hovered/pressed toolbar items get a raised face. The round outline and
// light top-left / dark bottom-right inset establish the raised direction;
// pressing reverses those edges instead of relying on a blue hover fill.
void draw_toolbar_hover_face(bool latched = false) {
  if (!latched && !ImGui::IsItemHovered() && !ImGui::IsItemActive()) return;
  const bool pressed = latched || ImGui::IsItemActive();
  const theme::Palette& p = theme::palette();
  const ImVec2 min = ImGui::GetItemRectMin();
  const ImVec2 max = ImGui::GetItemRectMax();
  const int alpha = std::clamp(static_cast<int>(255.f * ImGui::GetStyle().Alpha),
                               0, 255);
  const theme::Rgb edge = pressed ? p.dark : p.frame;
  ImGui::GetWindowDrawList()->AddRect(
      ImVec2(min.x + 0.5f, min.y + 0.5f),
      ImVec2(max.x - 0.5f, max.y - 0.5f),
      IM_COL32(edge.r, edge.g, edge.b, alpha),
      ImGui::GetStyle().FrameRounding);
  draw_button_inner_bevel(latched);
}

// Keep the bevel inside ImGui's rounded frame so the highlight and shadow
// lines do not cut across the corners. Regular buttons show this at rest too.
void draw_button_inner_bevel(bool latched) {
  const ImVec2 min = ImGui::GetItemRectMin();
  const ImVec2 max = ImGui::GetItemRectMax();
  if (max.x - min.x < 12.f || max.y - min.y < 12.f) return;
  const theme::Palette& p = theme::palette();
  const bool pressed = latched || ImGui::IsItemActive();
  const theme::Rgb edge = pressed ? p.shadow : p.highlight;
  const theme::Rgb lower_edge = pressed ? p.highlight : p.shadow;
  const int alpha = std::clamp(static_cast<int>(255.f * ImGui::GetStyle().Alpha),
                               0, 255);
  const ImU32 col = IM_COL32(edge.r, edge.g, edge.b, alpha);
  const ImU32 lower_col = IM_COL32(lower_edge.r, lower_edge.g,
                                  lower_edge.b, alpha);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddLine(ImVec2(min.x + 3.f, min.y + 1.5f),
                ImVec2(max.x - 4.f, min.y + 1.5f), col);
  draw->AddLine(ImVec2(min.x + 1.5f, min.y + 3.f),
                ImVec2(min.x + 1.5f, max.y - 4.f), col);
  draw->AddLine(ImVec2(min.x + 3.f, max.y - 2.f),
                ImVec2(max.x - 4.f, max.y - 2.f), lower_col);
  draw->AddLine(ImVec2(max.x - 2.f, min.y + 3.f),
                ImVec2(max.x - 2.f, max.y - 4.f), lower_col);
}

// All ordinary panel actions use the same raised detail and GTK-sized frame,
// rather than mixing ImGui's small (zero vertical padding) and regular buttons.
bool classic_button(const char* label, const ImVec2& size = ImVec2(0.f, 0.f)) {
  const bool clicked = ImGui::Button(label, size);
  draw_button_inner_bevel();
  return clicked;
}

// Keep ImGui's window movement, resizing and stacking while drawing the
// dialog title with the same colours and controls as the canvas panels.
bool begin_panel_dialog(const char* title, bool& open, theme::Tint tint,
                        float scale, ImGuiWindowFlags flags = 0) {
  const theme::Palette& pal = theme::palette();
  const auto color = [](theme::Rgb c) { return IM_COL32(c.r, c.g, c.b, 255); };
  const float title_h = std::round(28.f * scale);
  const float padding = std::max(0.f, (title_h - ImGui::GetFontSize()) / 2.f);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                      ImVec2(ImGui::GetStyle().FramePadding.x, padding));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, color(pal.face));
  ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 0));
  const bool visible = ImGui::Begin(title, nullptr, flags | ImGuiWindowFlags_NoCollapse);
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar();
  if (!visible) return false;

  ImGuiWindow* window = ImGui::GetCurrentWindow();
  const ImRect bar = window->TitleBarRect();
  ImDrawList* draw = window->DrawList;
  ImGui::PushClipRect(window->OuterRectClipped.Min, window->OuterRectClipped.Max,
                      false);
  const bool active = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
  const theme::TitleTint& tt = theme::title_tint(tint);
  const ImU32 top = color(active ? tt.top : tt.idle_top);
  const ImU32 bottom = color(active ? tt.bottom : tt.idle_bottom);
  const ImVec2 lo(bar.Min.x + 1.f, bar.Min.y + 1.f);
  const ImVec2 hi(bar.Max.x - 1.f, bar.Max.y);
  draw->AddRectFilledMultiColor(lo, hi, top, top, bottom, bottom);
  draw->AddLine(ImVec2(lo.x, lo.y + 0.5f), ImVec2(hi.x, lo.y + 0.5f),
                color(pal.highlight));
  draw->AddLine(ImVec2(lo.x, hi.y - 0.5f), ImVec2(hi.x, hi.y - 0.5f),
                color(active ? tt.line : pal.shadow));

  const ImGuiID close_id = window->GetID("##panel_close");
  const ImRect close_zone(bar.Min, ImVec2(bar.Min.x + title_h, bar.Max.y));
  bool hovered = false;
  bool held = false;
  const ImGuiNavLayer nav_layer = window->DC.NavLayerCurrent;
  window->DC.NavLayerCurrent = ImGuiNavLayer_Menu;
  ImGui::ItemAdd(close_zone, close_id, nullptr, ImGuiItemFlags_NoNavDefaultFocus);
  if (ImGui::ButtonBehavior(close_zone, close_id, &hovered, &held))
    open = false;
  ImGui::RenderNavCursor(close_zone, close_id, ImGuiNavRenderCursorFlags_Compact);
  window->DC.NavLayerCurrent = nav_layer;
  const float side = std::round(20.f * scale);
  const ImVec2 center = close_zone.GetCenter();
  const ImVec2 a(std::round(center.x - side / 2.f),
                 std::round(center.y - side / 2.f));
  const ImVec2 b(a.x + side, a.y + side);
  const ImU32 face_top = color(held ? pal.face_dark : pal.highlight);
  const ImU32 face_bottom = color(hovered && !held ? pal.face_light : pal.face_dark);
  draw->AddRectFilledMultiColor(a, b, face_top, face_top, face_bottom, face_bottom);
  draw->AddRect(a, b, color(pal.frame));
  const float arm = std::round(4.f * scale);
  draw->AddLine(ImVec2(center.x - arm, center.y - arm),
                ImVec2(center.x + arm, center.y + arm), color(pal.text));
  draw->AddLine(ImVec2(center.x - arm, center.y + arm),
                ImVec2(center.x + arm, center.y - arm), color(pal.text));
  draw->PushClipRect(ImVec2(close_zone.Max.x, bar.Min.y), hi, true);
  draw->AddText(ImVec2(close_zone.Max.x + 4.f * scale,
                       bar.Min.y + (bar.GetHeight() - ImGui::GetFontSize()) / 2.f),
                color(active ? tt.text : pal.text_dim), title);
  draw->PopClipRect();
  ImGui::PopClipRect();
  return true;
}

// Favicon-style tile of a search engine: a rounded square in the engine's
// colour with its badge letters, so engines stay told apart by shorthand.
void draw_engine_tile(ImDrawList* draw, ImVec2 p, float side,
                      const xanatoo::UiSearchEngine* e) {
  const uint32_t c = e && e->color ? e->color : 0x707070u;
  const int r = static_cast<int>((c >> 16) & 0xff);
  const int g = static_cast<int>((c >> 8) & 0xff);
  const int b = static_cast<int>(c & 0xff);
  const ImVec2 q(p.x + side, p.y + side);
  const float round = side * 0.22f;
  draw->AddRectFilled(p, q, IM_COL32(r, g, b, 255), round);
  draw->AddRectFilled(ImVec2(p.x + 1.f, p.y + 1.f),
                      ImVec2(q.x - 1.f, p.y + side * 0.48f),
                      IM_COL32(255, 255, 255, 46), round,
                      ImDrawFlags_RoundCornersTop);
  draw->AddRect(p, q, IM_COL32(r * 3 / 5, g * 3 / 5, b * 3 / 5, 255), round);
  const std::string text = e && !e->badge.empty() ? e->badge : "?";
  const bool light = 299 * r + 587 * g + 114 * b > 165000;
  const ImU32 ink = light ? IM_COL32(28, 28, 28, 255) : IM_COL32(255, 255, 255, 255);
  ImFont* font = ImGui::GetFont();
  float size = side * (text.size() <= 1 ? 0.8f : text.size() == 2 ? 0.66f : 0.54f);
  ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.f, text.c_str());
  const float room = side - 3.f;
  if (ts.x > room) {
    size *= room / ts.x;
    ts = font->CalcTextSizeA(size, FLT_MAX, 0.f, text.c_str());
  }
  const ImVec2 at(std::floor(p.x + (side - ts.x) * 0.5f),
                  std::floor(p.y + (side - ts.y) * 0.5f));
  // A one-pixel offset copy thickens the small letters a little.
  draw->AddText(font, size, at, ink, text.c_str());
  draw->AddText(font, size, ImVec2(at.x + 0.6f, at.y), ink, text.c_str());
}

// One checkbox per engine, on = in the search menu; a click issues
// ToggleSearchEngine (saved in view.conf). The note says why one is off
// by default.
void search_engine_checkboxes(const xanatoo::UiState& state,
                              xanatoo::UiCommand& cmd, std::string& arg) {
  for (const xanatoo::UiSearchEngine& e : state.search_engines) {
    bool on = e.on;
    ImGui::PushID(e.id.c_str());
    if (ImGui::Checkbox(e.name.c_str(), &on)) {
      cmd = xanatoo::UiCommand::ToggleSearchEngine;
      arg = e.id;
    }
    if (!e.note.empty()) {
      ImGui::SameLine();
      ImGui::TextDisabled("(%s)", e.note.c_str());
    }
    ImGui::PopID();
  }
}

// Go button at the right end of an address / search field: a green
// triangle, Enter for the mouse. An item without layout (ItemAdd), so it can
// sit inside the field's frame.
bool go_button(const char* id, const ImRect& bb, const char* tip) {
  const ImGuiID gid = ImGui::GetID(id);
  bool hot = false, held = false;
  bool pressed = false;
  if (ImGui::ItemAdd(bb, gid))
    pressed = ImGui::ButtonBehavior(bb, gid, &hot, &held);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  if (hot || held)
    draw->AddRectFilled(bb.Min, bb.Max,
                        ImGui::GetColorU32(held ? ImGuiCol_ButtonActive
                                                : ImGuiCol_ButtonHovered),
                        ImGui::GetStyle().FrameRounding);
  const float h = std::min(bb.GetWidth(), bb.GetHeight()) * 0.56f;
  const ImVec2 c = bb.GetCenter();
  const float dx = held ? 1.f : 0.f;
  const ImVec2 a(c.x - h * 0.42f + dx, c.y - h * 0.5f + dx);
  const ImVec2 b(c.x + h * 0.52f + dx, c.y + dx);
  const ImVec2 d(c.x - h * 0.42f + dx, c.y + h * 0.5f + dx);
  draw->AddTriangleFilled(a, b, d, hot ? IM_COL32(0x5c, 0xb8, 0x3c, 255)
                                       : IM_COL32(0x4a, 0xa0, 0x2c, 255));
  // Upper facet lighter, like the bevelled toolbar icons.
  draw->AddTriangleFilled(a, b, ImVec2(a.x, c.y + dx),
                          IM_COL32(255, 255, 255, 60));
  draw->AddTriangle(a, b, d, IM_COL32(0x2e, 0x5e, 0x1e, 255), 1.f);
  if (hot) ImGui::SetTooltip("%s", tip);
  return pressed;
}

void same_line_if_fits(const char* next_label) {
  const ImGuiStyle& s = ImGui::GetStyle();
  const float available = ImGui::GetWindowPos().x +
      ImGui::GetWindowContentRegionMax().x - ImGui::GetItemRectMax().x;
  const float needed = ImGui::CalcTextSize(next_label).x +
      2.f * s.FramePadding.x + s.ItemSpacing.x;
  if (available >= needed) ImGui::SameLine();
}

void draw_toolbar_menu_marker(float scale) {
  const ImVec2 max = ImGui::GetItemRectMax();
  const float inset = 3.f * scale;
  const float width = 6.f * scale;
  const float height = 4.f * scale;
  const ImVec2 a(max.x - inset - width, max.y - inset - height);
  const ImVec2 b(max.x - inset, max.y - inset - height);
  const ImVec2 c(max.x - inset - width * 0.5f, max.y - inset);
  const theme::Palette& p = theme::palette();
  ImGui::GetWindowDrawList()->AddTriangleFilled(
      a, b, c, IM_COL32(p.shadow.r, p.shadow.g, p.shadow.b, 255));
}

void pop_toolbar_button_style(int colors) {
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(colors);
}

// Shared View / toolbar items for display prefs (not document format).
void fill_line_spacing_items(const UiState& state, UiCommand& cmd) {
  using VS = xanatoo::ViewSettings;
  const float ls = state.view_line_spacing;
  if (ImGui::MenuItem("Tight", nullptr, near_eq(ls, VS::kLineTight, 0.05f)))
    cmd = UiCommand::ViewLineSpacingTight;
  if (ImGui::MenuItem("Normal", nullptr, near_eq(ls, VS::kLineNormal, 0.05f)))
    cmd = UiCommand::ViewLineSpacingNormal;
  if (ImGui::MenuItem("Relaxed", nullptr, near_eq(ls, VS::kLineRelaxed, 0.05f)))
    cmd = UiCommand::ViewLineSpacingRelaxed;
  if (ImGui::MenuItem("Loose", nullptr, near_eq(ls, VS::kLineLoose, 0.05f)))
    cmd = UiCommand::ViewLineSpacingLoose;
}

void fill_margins_items(const UiState& state, UiCommand& cmd,
                        std::string& arg) {
  for (float pct : xanatoo::ViewSettings::kMarginSteps) {
    char label[16];
    std::snprintf(label, sizeof label, "%g%%", pct);
    if (ImGui::MenuItem(label, nullptr,
                        near_eq(state.view_margin_pct, pct, 0.5f))) {
      cmd = UiCommand::ViewMargins;
      arg = std::to_string(static_cast<int>(pct));
    }
  }
}

void fill_measure_items(const UiState& state, UiCommand& cmd,
                        std::string& arg) {
  for (int chars : xanatoo::ViewSettings::kMeasureSteps) {
    std::string label =
        chars == 0 ? "Full width" : std::to_string(chars) + " characters";
    if (ImGui::MenuItem(label.c_str(), nullptr,
                        state.view_measure_chars == chars)) {
      cmd = UiCommand::ViewMeasure;
      arg = std::to_string(chars);
    }
  }
}

void style_menu_item(const char* label, bool selected, UiCommand choice,
                     const std::string& name, UiCommand& cmd,
                     UiFrameResult& result) {
  if (ImGui::MenuItem(label, nullptr, selected)) {
    cmd = choice;
    result.arg = name;
  }
  if (!selected && ImGui::IsItemHovered())
    result.style_hover.emplace(choice, name);
}

// Colours / fonts lent by another sheet: "From style sheet" + all sheets.
void fill_style_layer_items(const UiState& state, bool colors, UiCommand& cmd,
                            UiFrameResult& result) {
  const std::string& current = colors ? state.style_colors : state.style_fonts;
  const UiCommand choice =
      colors ? UiCommand::SelectStyleColors : UiCommand::SelectStyleFonts;
  style_menu_item("From style sheet", current.empty(), choice, {}, cmd, result);
  ImGui::Separator();
  for (const auto& [name, title] : state.styles) {
    if (name == state.style) continue;  // same as "From style sheet"
    style_menu_item(title.c_str(), name == current, choice, name, cmd, result);
  }
}

// Layout → Style sheet / Colours / Fonts as three submenus.
void fill_style_menus(const UiState& state, UiCommand& cmd,
                      UiFrameResult& result) {
  if (ImGui::BeginMenu("Style sheet")) {
    for (const auto& [name, title] : state.styles)
      style_menu_item(title.c_str(), name == state.style, UiCommand::SelectStyle,
                      name, cmd, result);
    ImGui::Separator();
    if (ImGui::MenuItem("Manage / edit style sheets..."))
      cmd = UiCommand::ToggleStyleManagerPanel;
    if (ImGui::MenuItem("Reload style sheets"))
      cmd = UiCommand::ReloadStyles;
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Colours")) {
    fill_style_layer_items(state, true, cmd, result);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Fonts")) {
    fill_style_layer_items(state, false, cmd, result);
    ImGui::EndMenu();
  }
}

const char* menu_icon_for(UiCommand command) {
  switch (command) {
    case UiCommand::NewDocument:
    case UiCommand::NewTab: return "new";
    case UiCommand::OpenDocument:
    case UiCommand::OpenInWindow: return "open";
    case UiCommand::OpenUrlBar:
    case UiCommand::Find: return "find";
    case UiCommand::CloseTab:
    case UiCommand::Quit: return "stop";
    case UiCommand::PopOut:
    case UiCommand::PopIn:
    case UiCommand::ToggleSplit: return "split";
    case UiCommand::ToggleTrail: return "history";
    case UiCommand::Save:
    case UiCommand::SaveAs: return "save";
    case UiCommand::Undo: return "undo";
    case UiCommand::Redo: return "redo";
    case UiCommand::Cut: return "cut";
    case UiCommand::Copy: return "copy";
    case UiCommand::CopyPageUrl:
    case UiCommand::CopyPageWebUrl:
    case UiCommand::Link: return "link";
    case UiCommand::OpenInBrowser: return "site-web";
    case UiCommand::Paste: return "paste";
    case UiCommand::Replace:
    case UiCommand::ReplaceAll: return "replace";
    case UiCommand::Bold: return "bold";
    case UiCommand::Italic: return "italic";
    case UiCommand::Strike: return "strike";
    case UiCommand::Code:
    case UiCommand::InsertCodeBlock: return "code";
    case UiCommand::Cite:
    case UiCommand::InsertBiblioEntry: return "cite";
    case UiCommand::InsertParagraph:
    case UiCommand::ParagraphLevel:
    case UiCommand::InsertHr: return "para";
    case UiCommand::InsertImage: return "image";
    case UiCommand::InsertFigure: return "figure";
    case UiCommand::InsertNote: return "note";
    case UiCommand::InsertTable: return "table";
    case UiCommand::InsertDefList: return "lists";
    case UiCommand::ToggleUnorderedList: return "list";
    case UiCommand::ToggleOrderedList: return "olist";
    case UiCommand::ToggleTask: return "task";
    case UiCommand::ToggleBlockQuote: return "quote";
    case UiCommand::MakeHeader:
    case UiCommand::Heading1: return "h1";
    case UiCommand::Heading2: return "h2";
    case UiCommand::Heading3: return "h3";
    case UiCommand::NavBack: return "back";
    case UiCommand::NavForward: return "forward";
    case UiCommand::GoUp: return "up";
    case UiCommand::GoHome: return "home";
    case UiCommand::AddBookmark:
    case UiCommand::BookmarkPage:
    case UiCommand::BookmarkPosition:
    case UiCommand::RemoveBookmark: return "bookmark";
    case UiCommand::ToggleHistory: return "history";
    case UiCommand::ToggleDownloads: return "downloads";
    case UiCommand::ToggleCookies: return "cookies";
    case UiCommand::ToggleCertificates: return "certificates";
    case UiCommand::ToggleOutline: return "outline";
    case UiCommand::ToggleStructure: return "structure";
    case UiCommand::ToggleDiff: return "diff";
    case UiCommand::ToggleLock: return "lock";
    case UiCommand::ToggleNative: return "native";
    case UiCommand::ToggleArchive: return "archive";
    case UiCommand::ToggleReader: return "reader";
    case UiCommand::ToggleHelp: return "keyboard";
    case UiCommand::ToggleHoverHelp: return "help";
    case UiCommand::ToggleBacklinks: return "backlinks";
    case UiCommand::ToggleSitemap: return "sitemap";
    case UiCommand::ToggleRelatedSites: return "related";
    case UiCommand::TogglePage: return "navigation";
    case UiCommand::ToggleMedia: return "media";
    case UiCommand::ToggleSubscriptions: return "subscriptions";
    case UiCommand::ToggleIdentities: return "identities";
    case UiCommand::ToggleStylesPanel: return "style";
    case UiCommand::ToggleStyleManagerPanel: return "style-manager";
    case UiCommand::ToggleAnnotationsPanel: return "annotations";
    case UiCommand::ToggleContextPanel: return "context";
    case UiCommand::ToggleRevisionsPanel: return "revisions";
    case UiCommand::ToggleNostrExplorer: return "site-nostr";
    case UiCommand::ToggleLemmyExplorer: return "lemmy";
    case UiCommand::ToggleMatrixExplorer: return "matrix";
    case UiCommand::ToggleNostrAccounts: return "nostr-accounts";
    case UiCommand::ZoomIn: return "zoomin";
    case UiCommand::ZoomOut: return "zoomout";
    case UiCommand::OpenFormatDoc: return "format";
    default: return nullptr;
  }
}

// ---- Bookmark folders (menu, margin-icon menu, Bookmarks panel) ----

// "Move To" items: the top level and every folder, indented by depth, the
// current one checked. Moves the bookmark `address`, or with an empty
// address the folder `folder` (never into itself).
void bookmark_move_items(const UiState& state, const std::string& address,
                         const std::string& folder, UiCommand& cmd,
                         std::string& arg) {
  const std::string current =
      address.empty() ? bookmark_folder_parent(folder) : folder;
  auto pick = [&](const std::string& dest) {
    if (!address.empty()) {
      cmd = UiCommand::MoveBookmark;
      arg = address + "\n" + dest;
    } else {
      const std::string leaf(bookmark_folder_leaf(folder));
      cmd = UiCommand::MoveBookmarkFolder;
      arg = folder + "\n" + (dest.empty() ? leaf : dest + "/" + leaf);
    }
  };
  if (ImGui::MenuItem("Top Level", nullptr, current.empty()) && !current.empty())
    pick({});
  for (const std::string& f : state.bookmark_folders) {
    if (address.empty() && bookmark_folder_within(f, folder)) continue;
    const auto depth = std::count(f.begin(), f.end(), '/');
    const std::string label =
        std::string(static_cast<size_t>(depth) * 3, ' ') +
        std::string(bookmark_folder_leaf(f));
    ImGui::PushID(f.c_str());
    if (ImGui::MenuItem(label.c_str(), nullptr, f == current) && f != current)
      pick(f);
    ImGui::PopID();
  }
}

// One level of the Bookmarks menu: subfolders as submenus, then the
// folder's bookmarks (newest last, at most kMenuBookmarks).
void bookmark_menu_level(const UiState& state, const std::string& folder,
                         UiCommand& cmd, std::string& arg) {
  constexpr size_t kMenuBookmarks = 40;
  bool any_folder = false;
  for (const std::string& f : state.bookmark_folders) {
    if (bookmark_folder_parent(f) != folder) continue;
    any_folder = true;
    ImGui::PushID(f.c_str());
    if (ImGui::BeginMenu(std::string(bookmark_folder_leaf(f)).c_str())) {
      bookmark_menu_level(state, f, cmd, arg);
      ImGui::EndMenu();
    }
    ImGui::PopID();
  }
  std::vector<size_t> marks;
  for (size_t i = 0; i < state.bookmarks.size(); ++i)
    if (state.bookmarks[i].folder == folder) marks.push_back(i);
  if (any_folder && !marks.empty()) ImGui::Separator();
  if (!any_folder && marks.empty()) ImGui::TextDisabled("(empty)");
  const size_t start = marks.size() > kMenuBookmarks ? marks.size() - kMenuBookmarks : 0;
  if (start) ImGui::TextDisabled("%zu older in the Bookmarks panel", start);
  for (size_t k = start; k < marks.size(); ++k) {
    const size_t i = marks[k];
    const Bookmark& b = state.bookmarks[i];
    std::string label = bookmark_label(b);
    if (label.size() > 64) label = elide_middle(label, 28);
    ImGui::PushID(static_cast<int>(i));
    if (i < state.bookmark_icons.size() && state.bookmark_icons[i]) {
      ImGui::Image(reinterpret_cast<ImTextureID>(state.bookmark_icons[i]),
                   icon_size(state.bookmark_icons[i], ImGui::GetTextLineHeight()));
      ImGui::SameLine();
    }
    if (ImGui::MenuItem(label.c_str())) {
      cmd = UiCommand::NavigateUrl;
      arg = b.address;
    }
    middle_open(cmd, arg, b.address);
    ImGui::PopID();
  }
}

bool menu_item_with_icon(const char* label, const char* icon_name,
                         const char* shortcut, bool selected,
                         bool enabled = true) {
  auto it = menu_icon_textures.find(icon_name ? icon_name : "");
  if (it == menu_icon_textures.end() || !it->second)
    return ImGui::MenuItem(label, shortcut, selected, enabled);

  // Reserve ImGui's icon column with spaces, then replace it visually with
  // the matching toolbar texture after MenuItemEx creates the hit area.
  const bool clicked = ImGui::MenuItemEx(label, "    ", shortcut, selected,
                                         enabled);
  const ImVec2 min = ImGui::GetItemRectMin();
  const ImVec2 max = ImGui::GetItemRectMax();
  const float side = ImGui::GetFontSize() * 1.05f;
  const float x = min.x + 1.f;
  const float y = min.y + (max.y - min.y - side) * 0.5f;
  const ImU32 tint = enabled ? IM_COL32(255, 255, 255, 255)
                             : IM_COL32(160, 160, 160, 150);
  ImGui::GetWindowDrawList()->AddImage(
      reinterpret_cast<ImTextureID>(it->second), ImVec2(x, y),
      ImVec2(x + side, y + side), ImVec2(0, 0), ImVec2(1, 1), tint);
  return clicked;
}

bool begin_panel(const UiState& state, UiPanelLayer layer, const char* name,
                 ImGuiWindowFlags flags, int x, int y, int w, int h,
                 int inset = 3, int min_size = 1) {
  const bool left = (state.left_docked_panels & ui_panel_bit(layer)) != 0;
  const int right = x + std::max(min_size, w - 2 * inset) + inset;
  const int body_x = left ? 0 : x + inset;
  ImGui::SetNextWindowPos(ImVec2(float(body_x), float(y + inset)));
  ImGui::SetNextWindowSize(ImVec2(float(right - body_x),
                                 float(std::max(min_size, h - 2 * inset))));
  if (left) {
    flags |= ImGuiWindowFlags_LeftScrollbar;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
  }
  const bool visible = ImGui::Begin(name, nullptr, flags);
  if (left) ImGui::PopStyleVar();
  return visible;
}

// Full-width lists can share the panel edge if its own scrollbar is absent.
bool begin_panel_list(const char* name, ImVec2 size,
                      ImGuiChildFlags child_flags = ImGuiChildFlags_None,
                      ImGuiWindowFlags flags = ImGuiWindowFlags_None) {
  const ImGuiWindow* panel = ImGui::GetCurrentWindowRead();
  if (!(panel->Flags & ImGuiWindowFlags_LeftScrollbar))
    return ImGui::BeginChild(name, size, child_flags, flags);
  flags |= ImGuiWindowFlags_LeftScrollbar;
  if (!panel->ScrollbarY) {
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float left = panel->InnerClipRect.Min.x;
    ImGui::SetNextWindowPos(ImVec2(left, cursor.y));
    const float width = size.x > 0.f ? size.x : ImGui::GetContentRegionAvail().x;
    size.x = std::max(1.f, width + cursor.x - left);
  }
  return ImGui::BeginChild(name, size, child_flags, flags);
}

}  // namespace


Ui::~Ui() { shutdown(); }

// ---------------------------------------------------------------------------
// Classic desktop theme — Clearlooks / Gtk2 greys with BeOS-ish blue selection.
// ---------------------------------------------------------------------------
void Ui::apply_classic_theme() {
  ImGui::StyleColorsLight();
  ImGuiStyle& s = ImGui::GetStyle();

  s.WindowPadding = ImVec2(8.f, 8.f);  // canvas windows: help_pad well inset
  s.FramePadding = ImVec2(8.f, 5.f);
  s.ItemSpacing = ImVec2(7.f, 6.f);
  s.ItemInnerSpacing = ImVec2(5.f, 4.f);
  s.ScrollbarSize = 14.f;
  s.GrabMinSize = 16.f;

  // Soft corners like Clearlooks; not fully flat, not fully modern.
  // Windows and scrollbars are square like the canvas windows (theme.cpp).
  s.WindowRounding = 0.f;
  s.ChildRounding = 2.f;
  s.FrameRounding = 3.f;
  s.PopupRounding = 3.f;
  s.ScrollbarRounding = 0.f;
  s.GrabRounding = 2.f;
  s.TabRounding = 2.f;

  s.WindowBorderSize = 1.f;
  s.FrameBorderSize = 1.f;
  s.PopupBorderSize = 1.f;

  auto C = [](int r, int g, int b, int a = 255) {
    return ImVec4(r / 255.f, g / 255.f, b / 255.f, a / 255.f);
  };

  // Colours come from the shared classic palette (theme.hpp) so canvas
  // windows and ImGui chrome stay in step.
  const theme::Palette& pal = theme::palette();
  auto P = [](theme::Rgb c) { return ImVec4(c.r / 255.f, c.g / 255.f, c.b / 255.f, 1.f); };
  const ImVec4 bg = P(pal.face);
  const ImVec4 bg2 = P(pal.face_light);
  const ImVec4 border = P(pal.frame);
  const ImVec4 text = P(pal.text);
  const ImVec4 text_dis = P(pal.text_dim);
  const ImVec4 sel = P(pal.accent);
  const ImVec4 sel_hi = C(0x6A, 0xA8, 0xE8);
  const ImVec4 header = P(pal.face_dark);
  const ImVec4 title = P(pal.tab_idle_top);

  ImVec4* c = s.Colors;
  c[ImGuiCol_Text] = text;
  c[ImGuiCol_TextDisabled] = text_dis;
  c[ImGuiCol_WindowBg] = bg2;
  c[ImGuiCol_ChildBg] = bg;
  c[ImGuiCol_PopupBg] = C(0xFA, 0xF9, 0xF7);
  c[ImGuiCol_Border] = border;
  c[ImGuiCol_BorderShadow] = C(0, 0, 0, 0);
  c[ImGuiCol_FrameBg] = C(0xFF, 0xFF, 0xFF);
  c[ImGuiCol_FrameBgHovered] = C(0xF0, 0xF6, 0xFC);
  c[ImGuiCol_FrameBgActive] = C(0xE0, 0xEC, 0xF8);
  c[ImGuiCol_TitleBg] = title;
  c[ImGuiCol_TitleBgActive] = P(pal.tab_top);
  c[ImGuiCol_TitleBgCollapsed] = header;
  c[ImGuiCol_MenuBarBg] = bg2;
  c[ImGuiCol_ScrollbarBg] = P(pal.scroll_trough);  // = theme::draw_vscrollbar
  c[ImGuiCol_ScrollbarGrab] = P(pal.scroll_thumb_bottom);
  c[ImGuiCol_ScrollbarGrabHovered] = C(0x7E, 0x9F, 0xB9);
  c[ImGuiCol_ScrollbarGrabActive] = P(pal.scroll_thumb_edge);
  c[ImGuiCol_CheckMark] = sel;
  c[ImGuiCol_SliderGrab] = sel;
  c[ImGuiCol_SliderGrabActive] = sel_hi;
  c[ImGuiCol_Button] = P(pal.face_light);
  c[ImGuiCol_ButtonHovered] = P(pal.highlight);
  c[ImGuiCol_ButtonActive] = P(pal.face_dark);
  c[ImGuiCol_Header] = header;
  c[ImGuiCol_HeaderHovered] = C(0xC0, 0xD8, 0xF0);
  c[ImGuiCol_HeaderActive] = sel;
  c[ImGuiCol_Separator] = border;
  c[ImGuiCol_SeparatorHovered] = sel;
  c[ImGuiCol_SeparatorActive] = sel_hi;
  c[ImGuiCol_ResizeGrip] = P(pal.shadow);
  c[ImGuiCol_ResizeGripHovered] = sel_hi;
  c[ImGuiCol_ResizeGripActive] = sel;
  // A light, outlined active tab lifts off the muted inactive tabs. The warm
  // overline echoes the BeOS window titles without colouring the whole tab.
  c[ImGuiCol_Tab] = C(0xDC, 0xE0, 0xDE);
  c[ImGuiCol_TabHovered] = P(pal.highlight);
  c[ImGuiCol_TabSelected] = P(pal.face_light);
  c[ImGuiCol_TabSelectedOverline] = P(pal.tab_bottom);
  c[ImGuiCol_TabDimmed] = P(pal.face_dark);
  c[ImGuiCol_TabDimmedSelected] = P(pal.face_light);
  c[ImGuiCol_TabDimmedSelectedOverline] = P(pal.tab_bottom);
  c[ImGuiCol_TableHeaderBg] = header;
  c[ImGuiCol_TableBorderStrong] = border;
  c[ImGuiCol_TableBorderLight] = C(0xC8, 0xC6, 0xC2);
  c[ImGuiCol_TextSelectedBg] = C(0x4A, 0x90, 0xD9, 90);
  c[ImGuiCol_NavHighlight] = sel;
}

void Ui::load_icons(SDL_Renderer* renderer) {
  if (!renderer) return;
  renderer_ = renderer;
  menu_icon_textures.clear();

  // Toolbar icons are installed data: <data dir>/icons (paths.hpp).
  std::vector<fs::path> roots;
  if (const std::string data = xanatoo::data_dir(); !data.empty())
    roots.push_back(fs::path(data) / "icons");

  const char* names[] = {
      "save", "undo", "redo", "bold", "italic", "code", "link", "list",
      "olist", "quote", "image", "split", "join", "find", "help", "cut",
      "strike", "cite", "h1", "h2", "h3", "para", "task", "table",
      "figure", "note", "replace", "outline", "paste", "copy", "new", "open",
      "lock", "lock-open", "zoomin", "zoomout", "structure", "diff",
      "back", "forward", "up", "home", "reload", "stop", "bookmark", "bookmark-outline", "history", "downloads", "edit", "native", "reader", "sitemap", "navigation",
      "linespacing", "measure", "style", "gemini", "gemini-changed",
      "site-file", "site-folder", "site-about", "site-store", "site-web",
      "site-feed", "site-nostr", "language",
      "format", "insert", "view", "lists",
      "select-all", "tab-select", "tab-window", "tab-close", "tab-close-others",
      "tab-close-left", "tab-close-right", "tab-sort",
      // Panels menu
      "keyboard", "backlinks", "related", "media", "subscriptions",
      "certificates", "identities", "style-manager", "annotations",
      "context", "revisions", "lemmy", "matrix", "nostr-accounts",
  };

  // Keep the candidate directories that exist.
  std::vector<fs::path> dirs;
  for (const auto& root : roots) {
    std::error_code ec;
    auto canon = fs::weakly_canonical(root, ec);
    const fs::path& probe = ec ? root : canon;
    if (fs::is_directory(probe)) dirs.push_back(probe);
  }
  if (dirs.empty()) {
    std::cerr << "xanatoo: toolbar icons not found "
                 "(no icons/ in the data directory, see --data-dir)\n";
    return;
  }

  // Per-icon search so an older dir with save.png does not hide newer icons
  // that only exist in the build output.
  for (const char* name : names) {
    fs::path path;
    for (const auto& dir : dirs) {
      fs::path cand = dir / (std::string(name) + ".png");
      if (fs::exists(cand)) {
        path = cand;
        break;
      }
    }
    if (path.empty()) continue;
    int w = 0, h = 0, comp = 0;
    unsigned char* data = stbi_load(path.string().c_str(), &w, &h, &comp, 4);
    if (!data) continue;

    SDL_Surface* surf = xanatoo::sdl::rgba_surface_from(
        data, w, h, w * 4);
    if (!surf) {
      stbi_image_free(data);
      continue;
    }
    Icon ic;
    ic.texture = SDL_CreateTextureFromSurface(renderer, surf);
    xanatoo::sdl::destroy_surface(surf);
    stbi_image_free(data);
    if (!ic.texture) continue;
    SDL_SetTextureBlendMode(ic.texture, SDL_BLENDMODE_BLEND);
    ic.w = w;
    ic.h = h;
    icons_[name] = ic;
    menu_icon_textures[name] = ic.texture;
  }
  std::cerr << "icons: " << icons_.size() << "/" << (sizeof(names) / sizeof(names[0]))
            << " from";
  for (const auto& dir : dirs) std::cerr << " " << dir.string();
  std::cerr << "\n";
}

const Ui::Icon* Ui::icon(const char* name) const {
  auto it = icons_.find(name);
  if (it == icons_.end() || !it->second.texture) return nullptr;
  return &it->second;
}

bool Ui::tool_button(const char* id, const char* icon_name, const char* tip,
                     UiCommand c, UiCommand& out, bool enabled,
                     bool* right_clicked, bool latched) {
  const Icon* ic = icon(icon_name);
  bool pressed = false;
  ImGui::BeginDisabled(!enabled);
  const int style_colors = push_toolbar_button_style(latched && enabled);
  if (ic) {
    ImGui::PushID(id);
    const ImVec2 size(28.f * scale_, 28.f * scale_);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(4.f * scale_, 4.f * scale_));
    if (ImGui::ImageButton(
            id, reinterpret_cast<ImTextureID>(ic->texture), size,
            ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0),
            ImVec4(1, 1, 1, enabled ? 1.f : 0.4f))) {
      pressed = true;
      out = c;
    }
    draw_toolbar_hover_face(latched && enabled);
    ImGui::PopStyleVar();
    ImGui::PopID();
  } else {
    // Text fallback when PNG missing (e.g. icons not rasterized yet).
    static const std::unordered_map<std::string, const char*> kShort = {
        {"back", "◀"}, {"forward", "▶"}, {"reload", "⟳"}, {"stop", "✕"},
        {"bookmark", "★"}, {"bookmark-outline", "☆"}, {"join", "Join"},
        {"history", "Hist"}, {"downloads", "DL"}, {"edit", "Edit"}, {"native", "Native"},
        {"reader", "Reader"},
        {"linespacing", "≡↕"}, {"measure", "▣|"},
        {"format", "Aa▾"},
        {"insert", "+▾"}, {"view", "View▾"}, {"lists", "List▾"},
        {"lock", "Lock"}, {"lock-open", "Unlk"},
        {"new", "New"}, {"open", "Open"}, {"save", "Save"}, {"undo", "Undo"},
        {"redo", "Redo"}, {"cut", "Cut"}, {"copy", "Copy"}, {"paste", "Paste"},
        {"bold", "B"}, {"italic", "I"}, {"link", "Link"}, {"find", "Find"},
        {"code", "Code"}, {"list", "List"}, {"structure", "Struct"},
    };
    std::string label;
    auto it = kShort.find(icon_name ? icon_name : "");
    if (it != kShort.end())
      label = it->second;
    else if (tip && tip[0]) {
      label = tip;
      size_t cut = label.find(" (");
      if (cut != std::string::npos) label.resize(cut);
      if (label.size() > 8) label.resize(8);
    } else {
      label = id;
    }
    label += "##";
    label += id;
    if (ImGui::Button(label.c_str(), ImVec2(0.f, 36.f * scale_))) {
      pressed = true;
      out = c;
    }
    draw_toolbar_hover_face(latched && enabled);
  }
  pop_toolbar_button_style(style_colors);
  ImGui::EndDisabled();
  const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
  if (hovered) note_hover_help(id, tip);
  if (right_clicked)
    *right_clicked = hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right);
  if (hovered && tip && tip[0]) {
    ImGui::SetTooltip("%s", tip);
  }
  ImGui::SameLine(0.f, 2.f);
  return pressed;
}



void Ui::open_revision_menu(float x, float y) {
  revision_menu_open_ = true;
  context_x_ = x;
  context_y_ = y;
}

static bool menu_item_cmd(const char* label, UiCommand c, UiCommand& out,
                          bool enabled = true, const char* shortcut = nullptr);

// The edit strip of each editable view, on top of its text (docs/UI.md
// "Editable documents"). The groups wrap as edit_bar_rows() predicts, so
// the height the app reserved fits. A locked document keeps its strip (and
// layout) with every tool but the lock disabled.
void Ui::edit_bars(const UiState& state, UiFrameResult& result,
                   UiCommand& cmd) {
  constexpr ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing;
  const float row_h = edit_bar_row_h(scale_);
  const float step = edit_bar_button_step(scale_);
  for (const UiEditBar& bar : state.edit_bars) {
    if (bar.w <= 0 || bar.h <= 0) continue;
    ImGui::SetNextWindowPos(ImVec2(static_cast<float>(bar.x),
                                   static_cast<float>(bar.y)));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(bar.w),
                                    static_cast<float>(bar.h)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.f, 1.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(kEditBarPadX, kEditBarPadY));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(ImGui::GetStyle().ItemSpacing.x,
                               edit_bar_row_gap(scale_)));
    // A document window's strip draws with that window's other embedded
    // ImGui (FindBar layer); a centre tile's lies under every window.
    char name[40];
    if (bar.floating)
      std::snprintf(name, sizeof name, "##find_bar/edit_strip");
    else
      std::snprintf(name, sizeof name, "##edit_bar/v%d", bar.view);
    const UiCommand before = cmd;
    if (ImGui::Begin(name, nullptr, flags)) {
      const ImVec2 at = ImGui::GetWindowPos();
      const float width = ImGui::GetWindowWidth();
      ImGui::GetWindowDrawList()->AddLine(
          ImVec2(at.x, at.y + static_cast<float>(bar.h) - 1.f),
          ImVec2(at.x + width, at.y + static_cast<float>(bar.h) - 1.f),
          ImGui::GetColorU32(ImGuiCol_Border));
      const float avail_w = width - 2.f * kEditBarPadX;
      const bool on = !bar.locked;
      const bool sel = on && bar.focused && bar.selection;
      bool line_has_group = false;
      bool wrap_inside_group = false;
      bool line_has_item = false;
      auto separator = [&] {
        ImGui::SameLine(0.f, 4.f);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(p.x + 3.f, p.y + 4.f), ImVec2(p.x + 3.f, p.y + row_h - 4.f),
            ImGui::GetColorU32(ImGuiCol_Separator));
        ImGui::Dummy(ImVec2(7.f, row_h));
        ImGui::SameLine(0.f, 4.f);
      };
      auto start_group = [&](int count) {
        const float group_w = static_cast<float>(count) * step;
        if (line_has_group && ImGui::GetContentRegionAvail().x <
                                  kEditBarSeparatorStep + group_w) {
          ImGui::NewLine();
          line_has_group = false;
        }
        if (line_has_group) separator();
        wrap_inside_group = group_w > avail_w;
        line_has_item = false;
        line_has_group = true;
      };
      auto before_item = [&] {
        if (wrap_inside_group && line_has_item &&
            ImGui::GetContentRegionAvail().x < step) {
          ImGui::NewLine();
          line_has_item = false;
        }
        line_has_item = true;
      };
      auto button = [&](const char* id, const char* icon, const char* tip,
                        UiCommand action, bool enabled) {
        before_item();
        tool_button(id, icon, tip, action, cmd, enabled);
      };
      auto menu_begin = [&](const char* id, const char* icon, const char* tip) {
        before_item();
        ImGui::BeginDisabled(!on);
        const bool open = tool_menu_begin(id, icon, tip);
        if (!open) ImGui::EndDisabled();
        return open;
      };
      auto menu_end = [&] {
        tool_menu_end();
        ImGui::EndDisabled();
      };

      // Lock, file, history
      start_group(kEditBarGroups[0]);
      before_item();
      tool_button("eb_lock", bar.locked ? "lock" : "lock-open",
                  bar.locked ? "Locked — click to allow editing (F2)"
                             : "Editable — click to lock (F2)",
                  UiCommand::ToggleLock, cmd, true, nullptr, bar.locked);
      if (!bar.autosaved_document)
        button("eb_save", "save",
               bar.dirty ? "Save (Ctrl+S) — unsaved changes" : "Save (Ctrl+S)",
               UiCommand::Save, on);
      button("eb_undo", "undo", "Undo (Ctrl+Z)", UiCommand::Undo, on);
      button("eb_redo", "redo", "Redo (Ctrl+Y)", UiCommand::Redo, on);

      // Clipboard
      start_group(kEditBarGroups[1]);
      button("eb_cut", "cut", "Cut (Ctrl+X)", UiCommand::Cut, sel);
      button("eb_copy", "copy", "Copy (Ctrl+C)", UiCommand::Copy,
             bar.focused && bar.selection);
      button("eb_paste", "paste", "Paste (Ctrl+V)", UiCommand::Paste, on);

      // Common inline styles
      start_group(kEditBarGroups[2]);
      button("eb_bold", "bold", "Bold (Ctrl+B)", UiCommand::Bold, sel);
      button("eb_italic", "italic", "Italic (Ctrl+I)", UiCommand::Italic, sel);
      button("eb_link", "link", "Link (Ctrl+Shift+K)", UiCommand::Link, sel);
      if (menu_begin("eb_format", "format", "More formatting")) {
        menu_item_cmd("Code", UiCommand::Code, cmd, sel, "Ctrl+`");
        menu_item_cmd("Strikethrough", UiCommand::Strike, cmd, sel,
                      "Ctrl+Shift+X");
        menu_item_cmd("Citation…", UiCommand::Cite, cmd, sel, "Ctrl+Shift+C");
        menu_end();
      }

      start_group(kEditBarGroups[3]);
      if (menu_begin("eb_structure", "structure", "Structure")) {
        menu_item_cmd("Heading", UiCommand::MakeHeader, cmd, true, "Ctrl+1");
        menu_item_cmd("Wrap Section", UiCommand::WrapSection, cmd, true,
                      "Ctrl+Shift+1");
        menu_item_cmd("Wrap Article", UiCommand::WrapArticle, cmd, true,
                      "Ctrl+Shift+2");
        menu_item_cmd("Promote Section", UiCommand::PromoteSection, cmd, true,
                      "Ctrl+Alt+]");
        menu_item_cmd("Demote Section", UiCommand::DemoteSection, cmd, true,
                      "Ctrl+Alt+[");
        menu_item_cmd("Paragraph", UiCommand::ParagraphLevel, cmd, true,
                      "Ctrl+0");
        ImGui::Separator();
        menu_item_cmd("Wrap Stretch…", UiCommand::WrapStretch, cmd, true,
                      "Ctrl+E");
        menu_item_cmd("Unwrap Stretch", UiCommand::UnwrapStretch, cmd,
                      bar.focused && bar.in_stretch, "Ctrl+Shift+U");
        menu_end();
      }

      start_group(kEditBarGroups[4]);
      if (menu_begin("eb_lists", "lists", "Lists & blocks")) {
        menu_item_cmd("Unordered list", UiCommand::ToggleUnorderedList, cmd);
        menu_item_cmd("Ordered list", UiCommand::ToggleOrderedList, cmd);
        menu_item_cmd("Task item", UiCommand::ToggleTask, cmd, true,
                      "Ctrl+Alt+T");
        menu_item_cmd("Block quote", UiCommand::ToggleBlockQuote, cmd);
        ImGui::Separator();
        menu_item_cmd("Indent", UiCommand::IndentList, cmd,
                      bar.focused && bar.on_list_item);
        menu_item_cmd("Outdent", UiCommand::OutdentList, cmd,
                      bar.focused && bar.on_list_item);
        menu_end();
      }

      start_group(kEditBarGroups[5]);
      if (menu_begin("eb_insert", "insert", "Insert")) {
        menu_item_cmd("Table", UiCommand::InsertTable, cmd);
        menu_item_cmd("Image…", UiCommand::InsertImage, cmd, true,
                      "Ctrl+Shift+I");
        menu_item_cmd("Figure…", UiCommand::InsertFigure, cmd);
        menu_item_cmd("Note", UiCommand::InsertNote, cmd);
        menu_item_cmd("Code block", UiCommand::InsertCodeBlock, cmd);
        menu_item_cmd("Horizontal rule", UiCommand::InsertHr, cmd);
        menu_item_cmd("Columns (2)", UiCommand::InsertColumns, cmd);
        menu_item_cmd("Transclude…", UiCommand::InsertTransclude, cmd);
        menu_item_cmd("Bibliography entry", UiCommand::InsertBiblioEntry, cmd);
        menu_end();
      }
    }
    ImGui::End();
    ImGui::PopStyleVar(5);
    if (cmd != before) result.edit_bar_view = bar.view;
  }
}

// `a` tinted towards the accent `b` (0xRRGGBB) by `t`.
static ImVec4 tint_rgb(theme::Rgb a, uint32_t b, float t) {
  auto ch = [&](int x, uint32_t y) {
    return (static_cast<float>(x) * (1.f - t) + static_cast<float>(y & 0xff) * t) /
           255.f;
  };
  return ImVec4(ch(a.r, b >> 16), ch(a.g, b >> 8), ch(a.b, b), 1.f);
}

// The service strip on top of each view showing a native service's page
// (docs/UI.md "Service strips"): favicon and name (the service's front
// page), its sections with the page's own marked, further rows ("Sort",
// "From") under the first, and the service's search on the right. Links
// that do not fit go into a "»" menu, so the height service_bar_height()
// reserved always fits.
void Ui::service_bars(const UiState& state, UiFrameResult& result,
                      UiCommand& cmd) {
  constexpr ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing;
  const theme::Palette& pal = theme::palette();
  const float row_h = service_bar_row_h(scale_);
  const float control_h = row_h - 4.f * scale_;
  const float line = std::max(2.f, std::round(2.f * scale_));
  for (const UiServiceBar& bar : state.service_bars) {
    if (bar.w <= 0 || bar.h <= 0) continue;
    ImGui::SetNextWindowPos(ImVec2(static_cast<float>(bar.x),
                                   static_cast<float>(bar.y)));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(bar.w),
                                    static_cast<float>(bar.h)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.f, 1.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(kServiceBarPadX, kServiceBarPadY));
    const float pad_y = std::max(0.f, (control_h - ImGui::GetFontSize()) * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.f * scale_, pad_y));
    const float field_border = ImGui::GetStyle().FrameBorderSize;
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);  // flat links
    ImGui::PushStyleColor(ImGuiCol_WindowBg, tint_rgb(pal.face, bar.color, 0.14f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          tint_rgb(pal.face, bar.color, 0.30f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                          tint_rgb(pal.face, bar.color, 0.45f));
    // Like the edit strip: a document window's strip draws with that
    // window's other embedded ImGui (FindBar layer), a tile's under all
    // windows (EditBar layer).
    char name[40];
    if (bar.floating)
      std::snprintf(name, sizeof name, "##find_bar/service_strip");
    else
      std::snprintf(name, sizeof name, "##edit_bar/svc%d", bar.view);
    const UiCommand before = cmd;
    if (ImGui::Begin(name, nullptr, flags)) {
      const ImVec2 at = ImGui::GetWindowPos();
      const float width = ImGui::GetWindowWidth();
      ImGui::GetWindowDrawList()->AddRectFilled(
          ImVec2(at.x, at.y + static_cast<float>(bar.h) - line),
          ImVec2(at.x + width, at.y + static_cast<float>(bar.h)),
          IM_COL32(bar.color >> 16 & 0xff, bar.color >> 8 & 0xff,
                   bar.color & 0xff, 255));
      const ImGuiStyle& style = ImGui::GetStyle();
      auto open = [&](const std::string& ref) {
        cmd = UiCommand::NavigateUrl;
        result.arg = ref;
      };
      auto link_w = [&](const ServiceLink& l) {
        return ImGui::CalcTextSize(l.text.c_str()).x + 2.f * style.FramePadding.x +
               style.ItemSpacing.x;
      };
      auto link = [&](const ServiceLink& l, size_t i) {
        ImGui::PushID(static_cast<int>(i));
        const bool hot = l.current;
        if (hot)
          ImGui::PushStyleColor(ImGuiCol_Button,
                                tint_rgb(pal.face, bar.color, 0.40f));
        ImGui::BeginDisabled(l.ref.empty() && !hot);
        if (ImGui::Button(l.text.c_str(), ImVec2(0.f, control_h)) && !l.ref.empty())
          open(l.ref);
        if (!l.ref.empty() && ImGui::IsItemHovered())
          ImGui::SetTooltip("%s", l.ref.c_str());
        ImGui::EndDisabled();
        if (hot) ImGui::PopStyleColor();
        ImGui::PopID();
        ImGui::SameLine();
      };
      auto separator = [&] {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(p.x + 3.f, p.y + 4.f), ImVec2(p.x + 3.f, p.y + control_h - 4.f),
            ImGui::GetColorU32(ImGuiCol_Separator));
        ImGui::Dummy(ImVec2(7.f, control_h));
        ImGui::SameLine();
      };
      std::string& query = service_search_[bar.id];
      const float search_w =
          bar.search_engine.empty()
              ? 0.f
              : std::min(220.f * scale_, std::max(90.f * scale_, width * 0.28f));
      const float more_w = ImGui::CalcTextSize("\xC2\xBB").x +
                           2.f * style.FramePadding.x + style.ItemSpacing.x;
      for (size_t r = 0; r < std::max<size_t>(1, bar.rows.size()); ++r) {
        const float row_y = kServiceBarPadY + static_cast<float>(r) * row_h;
        // Center in the strip above the accent rule.
        const float control_y = std::round(row_y + (row_h - control_h - line) * 0.5f);
        ImGui::SetCursorPos(ImVec2(kServiceBarPadX, control_y));
        ImGui::PushID(static_cast<int>(r));
        if (r == 0) {
          if (bar.icon) {
            const float side = 16.f * scale_;
            const ImVec2 size = icon_size(bar.icon, side);
            const ImVec2 c = ImGui::GetCursorScreenPos();
            const ImVec2 p(c.x + 2.f, c.y + std::round((control_h - size.y) * 0.5f));
            ImGui::GetWindowDrawList()->AddImage(
                reinterpret_cast<ImTextureID>(bar.icon), p,
                ImVec2(p.x + size.x, p.y + size.y));
            ImGui::Dummy(ImVec2(2.f + size.x, control_h));
            ImGui::SameLine();
          }
          ImGui::PushStyleColor(ImGuiCol_Text,
                                tint_rgb(pal.text, bar.color, 0.35f));
          ImGui::BeginDisabled(bar.home.empty());
          if (ImGui::Button(bar.name.c_str(), ImVec2(0.f, control_h)))
            open(bar.home);
          if (!bar.home.empty() && ImGui::IsItemHovered())
            ImGui::SetTooltip("Front page — %s", bar.home.c_str());
          ImGui::EndDisabled();
          ImGui::PopStyleColor();
          ImGui::SameLine();
        } else if (!bar.rows[r].label.empty()) {
          ImGui::AlignTextToFramePadding();
          ImGui::TextDisabled("%s", bar.rows[r].label.c_str());
          ImGui::SameLine();
        }
        if (r < bar.rows.size() && !bar.rows[r].links.empty()) {
          if (r == 0 || bar.rows[r].label.empty()) separator();
          const auto& links = bar.rows[r].links;
          const float right = width - kServiceBarPadX -
                              (r == 0 && search_w > 0.f ? search_w + style.ItemSpacing.x
                                                        : 0.f);
          // How many fit, keeping room for "»" when some do not.
          float x = ImGui::GetCursorPosX();
          size_t fit = 0;
          float total = x;
          for (const auto& l : links) total += link_w(l);
          if (total <= right) {
            fit = links.size();
          } else {
            for (; fit < links.size() && x + link_w(links[fit]) + more_w <= right; ++fit)
              x += link_w(links[fit]);
          }
          for (size_t i = 0; i < fit; ++i) link(links[i], i);
          if (fit < links.size()) {
            if (ImGui::Button("\xC2\xBB", ImVec2(0.f, control_h)))
              ImGui::OpenPopup("##more");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("More");
            if (ImGui::BeginPopup("##more")) {
              for (size_t i = fit; i < links.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::MenuItem(links[i].text.c_str(), nullptr,
                                    links[i].current, !links[i].ref.empty()))
                  open(links[i].ref);
                ImGui::PopID();
              }
              ImGui::EndPopup();
            }
            ImGui::SameLine();
          }
        }
        if (r == 0 && search_w > 0.f) {
          ImGui::SetCursorPos(ImVec2(width - kServiceBarPadX - search_w, control_y));
          ImGui::SetNextItemWidth(search_w);
          ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, field_border);
          char buf[512];
          std::snprintf(buf, sizeof buf, "%s", query.c_str());
          const std::string hint = "Search " + bar.search_name;
          if (ImGui::InputTextWithHint("##search", hint.c_str(), buf, sizeof buf,
                                       ImGuiInputTextFlags_EnterReturnsTrue)) {
            std::string_view q(buf);
            trim_ascii(q);
            if (!q.empty()) open(search_ref(bar.search_engine, q));
          }
          ImGui::PopStyleVar();
          query = buf;
        }
        ImGui::NewLine();
        ImGui::PopID();
      }
    }
    ImGui::End();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(6);
    if (cmd != before) result.edit_bar_view = bar.view;
  }
}

// A copied-in field of the find bar: `buf` follows `text` whenever the app
// changed it, and is left alone while it holds typing the app has not seen.
static void sync_find_field(char* buf, size_t size, const std::string& text,
                            std::string& seen, bool force) {
  if (!force && text == seen) return;
  seen = text;
  std::snprintf(buf, size, "%s", text.c_str());
}

// Strip below the text of the focused view (UiState::find_*): the query with
// previous / next, the count and a close button; the replacement under it.
void Ui::find_bar(const UiState& state, UiCommand& cmd, std::string& arg) {
  if (!state.find_open) {
    find_field_active_ = false;
    find_caret_frames_ = 0;
    return;
  }
  const bool focus = find_seq_seen_ != state.find_focus_seq;
  find_seq_seen_ = state.find_focus_seq;
  sync_find_field(find_input_, sizeof(find_input_), state.find_text,
                  find_text_seen_, focus);
  sync_find_field(find_replace_input_, sizeof(find_replace_input_),
                  state.find_replacement, find_replacement_seen_, focus);
  find_caret_select_ = state.find_select_all;

  ImGui::SetNextWindowPos(ImVec2(static_cast<float>(state.find_x),
                                 static_cast<float>(state.find_y)));
  ImGui::SetNextWindowSize(ImVec2(static_cast<float>(state.find_w),
                                  static_cast<float>(state.find_h)));
  constexpr ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
      ImGuiWindowFlags_NoScrollWithMouse;
  const float row = 26.f * scale_, gap = 4.f * scale_;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.f, 1.f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(gap * 1.5f, gap));
  ImGui::PushStyleVar(
      ImGuiStyleVar_FramePadding,
      ImVec2(5.f * scale_,
             std::max(0.f, (row - ImGui::GetFontSize()) * 0.5f)));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
  bool active = false;
  if (ImGui::Begin("##find_bar", nullptr, flags)) {
    const ImVec2 at = ImGui::GetWindowPos();
    const float width = ImGui::GetWindowWidth();
    ImGui::GetWindowDrawList()->AddLine(
        at, ImVec2(at.x + width, at.y),
        ImGui::GetColorU32(ImGuiCol_Border));
    const auto command = [&](UiCommand c) {
      cmd = c;
      arg = std::string(find_input_) + "\n" + find_replace_input_;
    };
    // Enter keeps the field; the caret goes to the end (ImGui selects all).
    const auto place_caret = [](ImGuiInputTextCallbackData* cb) -> int {
      auto* ui = static_cast<Ui*>(cb->UserData);
      const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, true) ||
                         ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, true);
      if (ui->find_caret_frames_ <= 0 && !enter) return 0;
      cb->SelectionStart =
          ui->find_caret_frames_ > 0 && ui->find_caret_select_ ? 0
                                                               : cb->BufTextLen;
      cb->SelectionEnd = cb->CursorPos = cb->BufTextLen;
      ui->find_caret_frames_ = 0;
      return 0;
    };
    const auto field = [&](const char* id, char* buf, size_t size) {
      ImGuiIO& io = ImGui::GetIO();
      const bool keep = io.ConfigInputTextEnterKeepActive;
      io.ConfigInputTextEnterKeepActive = true;
      const bool enter = ImGui::InputText(
          id, buf, size,
          ImGuiInputTextFlags_EnterReturnsTrue |
              ImGuiInputTextFlags_CallbackAlways,
          place_caret, this);
      io.ConfigInputTextEnterKeepActive = keep;
      active |= ImGui::IsItemActive();
      return enter;
    };

    const float close_x = width - row - gap;
    const float label_w =
        std::max(ImGui::CalcTextSize("Find").x,
                 state.find_replace ? ImGui::CalcTextSize("Replace").x : 0.f) +
        gap * 2.f;
    const bool narrow = width < 420.f * scale_;
    const float field_x = narrow ? gap * 1.5f : gap * 1.5f + label_w;
    const char* const all_label = narrow ? "All" : "Replace All";
    const float pad_x = 2.f * ImGui::GetStyle().FramePadding.x;
    const float count_w =
        narrow ? 0.f : ImGui::CalcTextSize("1000+ matches").x + 2.f * gap;
    const float tail =
        std::max(2.f * (row + gap) + count_w,
                 state.find_replace ? ImGui::CalcTextSize("Replace").x +
                                          ImGui::CalcTextSize(all_label).x +
                                          2.f * (pad_x + gap)
                                    : 0.f);
    const float field_w = std::clamp(close_x - field_x - tail - gap,
                                     40.f * scale_, 420.f * scale_);

    if (!narrow) {
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Find");
      ImGui::SameLine();
    }
    ImGui::SetCursorPosX(field_x);
    ImGui::SetNextItemWidth(field_w);
    if (std::exchange(find_refocus_, false) || focus) {
      ImGui::SetKeyboardFocusHere();
      find_caret_frames_ = 4;
    }
    const bool mod_all = state.find_replace && state.find_can_replace &&
                         enter_ctrl_ && enter_shift_;
    if (field("##find_text", find_input_, sizeof(find_input_)))
      command(mod_all        ? UiCommand::ReplaceAll
              : enter_shift_ ? UiCommand::FindPrev
                             : UiCommand::FindNext);
    // After a button the keys go back to the field: Enter keeps stepping.
    const auto press = [&](UiCommand c) {
      command(c);
      find_refocus_ = true;
    };
    const auto tip = [](const char* key, const char* text) {
      note_hover_help(key, text);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", text);
    };
    const bool none = find_input_[0] == '\0';
    // Tab goes between the two fields.
    ImGui::PushItemFlag(ImGuiItemFlags_NoTabStop, true);
    ImGui::BeginDisabled(none);
    ImGui::SameLine();
    if (ImGui::ArrowButtonEx("##find_prev", ImGuiDir_Up, ImVec2(row, row)))
      press(UiCommand::FindPrev);
    draw_button_inner_bevel();
    tip("find_prev", "Previous match (Shift+Enter, Shift+F3)");
    ImGui::SameLine();
    if (ImGui::ArrowButtonEx("##find_next", ImGuiDir_Down, ImVec2(row, row)))
      press(UiCommand::FindNext);
    draw_button_inner_bevel();
    tip("find_next", "Next match (Enter, F3)");
    ImGui::EndDisabled();
    if (!narrow && !none) {
      ImGui::SameLine(0.f, 2.f * gap);
      ImGui::AlignTextToFramePadding();
      const char* const more = state.find_capped ? "+" : "";
      if (state.find_count == 0)
        ImGui::TextColored(ImVec4(0.75f, 0.1f, 0.1f, 1.f), "Not found");
      else if (state.find_index > 0)
        ImGui::Text("%zu of %zu%s", state.find_index, state.find_count, more);
      else
        ImGui::Text("%zu%s %s", state.find_count, more,
                    state.find_count == 1 ? "match" : "matches");
    }
    ImGui::SameLine();
    ImGui::SetCursorPosX(close_x);
    if (ImGui::Button("##find_close", ImVec2(row, row)))
      command(UiCommand::FindClose);
    draw_button_inner_bevel();
    {
      const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
      const float r = row * 0.18f;
      const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
      ImDrawList* draw = ImGui::GetWindowDrawList();
      draw->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), ink,
                    1.6f * scale_);
      draw->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), ink,
                    1.6f * scale_);
    }
    tip("find_close", "Close the find bar (Esc)");
    ImGui::PopItemFlag();

    if (state.find_replace) {
      if (!narrow) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Replace");
        ImGui::SameLine();
      }
      ImGui::SetCursorPosX(field_x);
      ImGui::SetNextItemWidth(field_w);
      if (field("##find_replacement", find_replace_input_,
                sizeof(find_replace_input_)))
        command(mod_all ? UiCommand::ReplaceAll : UiCommand::ReplaceOne);
      ImGui::PushItemFlag(ImGuiItemFlags_NoTabStop, true);
      ImGui::BeginDisabled(none || !state.find_can_replace);
      ImGui::SameLine();
      if (classic_button("Replace", ImVec2(0.f, row)))
        press(UiCommand::ReplaceOne);
      tip("find_replace", "Replace this match and go to the next (Enter)");
      ImGui::SameLine();
      if (classic_button(all_label, ImVec2(0.f, row)))
        press(UiCommand::ReplaceAll);
      tip("find_replace_all", "Replace every match (Ctrl+Shift+Enter)");
      ImGui::EndDisabled();
      ImGui::PopItemFlag();
    }
    // Typing the app has not been told yet.
    if (cmd == UiCommand::None &&
        (state.find_text != find_input_ ||
         state.find_replacement != find_replace_input_))
      command(UiCommand::FindText);
  }
  ImGui::End();
  ImGui::PopStyleVar(6);
  find_field_active_ = active;
}

// Icon (or text) button that opens a popup menu. When true, fill items then tool_menu_end().
bool Ui::tool_menu_begin(const char* id, const char* icon_name, const char* tip) {
  const Icon* ic = icon(icon_name);
  ImGui::PushID(id);
  bool pressed = false;
  ImVec2 popup_pos;
  const int style_colors = push_toolbar_button_style();
  if (ic) {
    const ImVec2 size(28.f * scale_, 28.f * scale_);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(4.f * scale_, 4.f * scale_));
    if (ImGui::ImageButton(
            "##btn", reinterpret_cast<ImTextureID>(ic->texture), size,
            ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0),
            ImVec4(1, 1, 1, 1.f)))
      pressed = true;
    draw_toolbar_hover_face();
    draw_toolbar_menu_marker(scale_);
    ImGui::PopStyleVar();
  } else {
    static const std::unordered_map<std::string, const char*> kShort = {
        {"format", "Aa"}, {"insert", "+"}, {"view", "View"}, {"lists", "List"},
        {"structure", "Struct"}, {"list", "List"}, {"code", "Aa"},
        {"table", "+"}, {"h1", "H"}, {"outline", "Outline"},
        {"linespacing", "Sp"}, {"measure", "Me"}, {"style", "Style"},
    };
    std::string label;
    auto it = kShort.find(icon_name ? icon_name : "");
    if (it != kShort.end())
      label = it->second;
    else if (tip && tip[0]) {
      label = tip;
      size_t cut = label.find(" (");
      if (cut != std::string::npos) label.resize(cut);
      if (label.size() > 10) label.resize(10);
    } else {
      label = id;
    }
    label += " ▾";
    if (ImGui::Button(label.c_str(), ImVec2(0.f, 36.f * scale_))) pressed = true;
    draw_toolbar_hover_face();
  }
  const ImVec2 button_min = ImGui::GetItemRectMin();
  const ImVec2 button_max = ImGui::GetItemRectMax();
  popup_pos = ImVec2(button_min.x, button_max.y);
  pop_toolbar_button_style(style_colors);
  if (pressed) ImGui::OpenPopup("##menu");
  note_hover_help(id, tip);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && tip && tip[0])
    ImGui::SetTooltip("%s", tip);
  // Set this after the tooltip: SetTooltip() begins another ImGui window and
  // consumes SetNextWindowPos before the popup gets a chance to use it.
  if (ImGui::IsPopupOpen("##menu"))
    ImGui::SetNextWindowPos(popup_pos, ImGuiCond_Appearing);
  // Advance toolbar cursor before opening the popup window.
  ImGui::SameLine(0.f, 2.f);
  bool open = ImGui::BeginPopup("##menu");
  if (!open) {
    ImGui::PopID();
    return false;
  }
  // Popup is open; keep ID pushed until tool_menu_end().
  return true;
}

void Ui::tool_menu_end() {
  ImGui::EndPopup();
  ImGui::PopID();
}

static void load_ui_fonts(ImGuiIO& io, float scale) {
  // GNOME 2 commonly used Sans 10. Keep its Sans-family look, with a little
  // more pixel size to balance xanatoo's intentionally large 32 px icons.
  const float size_px = std::round(16.0f * scale);
  const FontFace face = ui_font_face();
  const std::string& path = face.path;
  ImFontConfig cfg;
  cfg.FontNo = face.index;  // member of a .ttc collection
  cfg.OversampleH = 2;
  cfg.OversampleV = 1;
  cfg.PixelSnapH = true;  // crisper menus on integer positions
  if (!path.empty()) {
    // Latin (with Extended-A/B: "Čeština"), Greek and Cyrillic for page
    // titles and language names, plus the punctuation, arrows and shapes
    // menu labels use ("…", "—", "→", "▸"); the default range alone draws
    // them as "?". Other scripts fall back (ui_can_draw).
    static const ImWchar kRanges[] = {0x0020, 0x024F, 0x0370, 0x03FF,
                                      0x0400, 0x052F, 0x1E00, 0x1EFF,
                                      0x2010, 0x205E, 0x2190, 0x21FF,
                                      0x25A0, 0x25FF, 0};
    ImFont* font =
        io.Fonts->AddFontFromFileTTF(path.c_str(), size_px, &cfg, kRanges);
    if (font) {
      std::cerr << "menu font: " << path << " @ " << size_px << " px\n";
      return;
    }
    std::cerr << "UI font load failed: " << path << " — using ImGui default\n";
  } else {
    std::cerr << "No DejaVu/Vera/Liberation Sans found — using ImGui default\n";
  }
  io.Fonts->AddFontDefault();
}

bool Ui::init(SDL_Window* window, SDL_Renderer* renderer, float ui_scale) {
  if (ready_) return true;
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  // Gamepad navigates ImGui menus when a controller is present.
#if !defined(XANATOO_SDL3) || !XANATOO_SDL3
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
#endif
  io.IniFilename = nullptr;
  scale_ = ui_scale > 0.f ? ui_scale : 1.f;
  window_ = window;

  load_ui_fonts(io, scale_);
  apply_classic_theme();
  ImGui::GetStyle().ScaleAllSizes(scale_);
  load_icons(renderer);

#if defined(XANATOO_SDL3) && XANATOO_SDL3
  if (!xanatoo::imgui_sdl3::init(window, renderer)) return false;
#else
  if (!ImGui_ImplSDL2_InitForSDLRenderer(window, renderer)) return false;
  if (!ImGui_ImplSDLRenderer2_Init(renderer)) return false;
  if (ImGui_ImplSDLRenderer2_UsesTriangleFallback()) {
    // Old SDL (R36S): triangles are drawn as their bounding rectangles, so
    // keep every shape an axis-aligned quad (no fringes, square corners).
    ImGuiStyle& s = ImGui::GetStyle();
    s.AntiAliasedLines = s.AntiAliasedLinesUseTex = s.AntiAliasedFill = false;
    s.WindowRounding = s.ChildRounding = s.FrameRounding = 0.f;
    s.PopupRounding = s.ScrollbarRounding = s.GrabRounding = 0.f;
    s.TabRounding = 0.f;
  }
#endif
  ready_ = true;
  return true;
}

void Ui::shutdown() {
  if (!ready_) return;
  menu_icon_textures.clear();
  for (auto& [k, ic] : icons_) {
    if (ic.texture) SDL_DestroyTexture(ic.texture);
  }
  icons_.clear();
#if defined(XANATOO_SDL3) && XANATOO_SDL3
  xanatoo::imgui_sdl3::shutdown();
#else
  ImGui_ImplSDLRenderer2_Shutdown();
  ImGui_ImplSDL2_Shutdown();
#endif
  ImGui::DestroyContext();
  ready_ = false;
}

void Ui::process_event(const SDL_Event& ev) {
  if (!ready_) return;
  // Ctrl+Tab / Ctrl+Shift+Tab belong to the document tab strip; keep ImGui's
  // built-in window switcher overlay from popping up over the page.
  const auto kind = xanatoo::sdl::event_kind(ev);
  if ((kind == xanatoo::sdl::EventKind::KeyDown ||
       kind == xanatoo::sdl::EventKind::KeyUp) &&
      xanatoo::sdl::keycode(ev) == SDLK_TAB &&
      (xanatoo::sdl::keymod(ev) & KMOD_CTRL)) {
    return;
  }
  // Shift can be up again by the time the frame sees Enter (find bar).
  if (kind == xanatoo::sdl::EventKind::KeyDown &&
      (xanatoo::sdl::keycode(ev) == SDLK_RETURN ||
       xanatoo::sdl::keycode(ev) == SDLK_KP_ENTER)) {
    enter_shift_ = (xanatoo::sdl::keymod(ev) & KMOD_SHIFT) != 0;
    enter_ctrl_ = (xanatoo::sdl::keymod(ev) & KMOD_CTRL) != 0;
  }
  // Middle click on a text field pastes the primary selection, X11 style.
  // Whether a field is under the press is known only once a frame has laid
  // out the widgets at that point: hold the press (and a quick release)
  // until resolve_middle_click.
  if (xanatoo::sdl::kMiddleClickPastesPrimary &&
      (kind == xanatoo::sdl::EventKind::MouseButtonDown ||
       kind == xanatoo::sdl::EventKind::MouseButtonUp) &&
      ev.button.button == SDL_BUTTON_MIDDLE) {
    if (kind == xanatoo::sdl::EventKind::MouseButtonDown) {
      ImGui::GetIO().AddMousePosEvent(static_cast<float>(ev.button.x),
                                      static_cast<float>(ev.button.y));
      middle_click_ = {true, false};
      return;
    }
    if (middle_click_.pending) {
      middle_click_.released = true;
      return;
    }
    if (std::exchange(middle_paste_release_, false)) return;
  }
#if defined(XANATOO_SDL3) && XANATOO_SDL3
  xanatoo::imgui_sdl3::process_event(ev);
#else
  ImGui_ImplSDL2_ProcessEvent(&ev);
#endif
}

// The frame after a middle press has hovered the widgets under it: a text
// field (ImGui shows its text cursor) gets a left click there, which places
// the caret, and the primary selection as typed input trickled in after the
// click; anything else gets the middle press as it was.
void Ui::claim_cursor(xanatoo::sdl::SystemCursor kind) {
  if (!ready_) return;
  using K = xanatoo::sdl::SystemCursor;
  ImGuiMouseCursor c = ImGuiMouseCursor_Arrow;
  switch (kind) {
    case K::Arrow: c = ImGuiMouseCursor_Arrow; break;
    case K::Hand: c = ImGuiMouseCursor_Hand; break;
    case K::ResizeEW: c = ImGuiMouseCursor_ResizeEW; break;
    case K::ResizeNS: c = ImGuiMouseCursor_ResizeNS; break;
    case K::ResizeNW:
    case K::ResizeSE: c = ImGuiMouseCursor_ResizeNWSE; break;
    case K::ResizeNE:
    case K::ResizeSW: c = ImGuiMouseCursor_ResizeNESW; break;
    case K::Move: c = ImGuiMouseCursor_ResizeAll; break;
  }
  ImGui::SetMouseCursor(c);
}

void Ui::resolve_middle_click() {
  if (!std::exchange(middle_click_.pending, false)) return;
  ImGuiIO& io = ImGui::GetIO();
  std::string text;
  if (ImGui::GetMouseCursor() == ImGuiMouseCursor_TextInput)
    text = xanatoo::sdl::primary_selection();
  if (text.empty()) {
    io.AddMouseButtonEvent(2, true);
    if (middle_click_.released) io.AddMouseButtonEvent(2, false);
    return;
  }
  io.AddMouseButtonEvent(0, true);
  io.AddMouseButtonEvent(0, false);
  io.AddInputCharactersUTF8(text.c_str());
  middle_paste_release_ = !middle_click_.released;
  middle_paste_frames_ = 4;  // click, release and text trickle over frames
}

// The selection of the active ImGui text field becomes the primary
// selection once the mouse lets go of it (keyboard selections at once).
// Password fields keep theirs; read-only fields keep their text outside the
// edit state and are not offered either.
void Ui::publish_text_field_selection() {
  if (!xanatoo::sdl::kMiddleClickPastesPrimary) return;
  ImGuiContext& g = *ImGui::GetCurrentContext();
  const ImGuiInputTextState& st = g.InputTextState;
  if (st.ID == 0 || st.ID != g.ActiveId) {
    field_primary_.id = 0;
    return;
  }
  if (g.IO.MouseDown[0] || !st.HasSelection() ||
      (st.Flags & (ImGuiInputTextFlags_Password | ImGuiInputTextFlags_ReadOnly)))
    return;
  const int from = std::max(0, st.GetSelectionStart());
  const int to = std::min(st.TextLen, st.GetSelectionEnd());
  if (to <= from || st.TextA.Size < to) return;
  std::string text(st.TextA.Data + from, static_cast<size_t>(to - from));
  if (field_primary_.id == st.ID && field_primary_.text == text) return;
  field_primary_ = {st.ID, text};
  xanatoo::sdl::set_primary_selection(text);
}

bool Ui::want_capture_keyboard() const {
  if (!ready_) return false;
  // Claim keys only when ImGui has a real reason: text input, popup/menu, or
  // an active widget. Full WantCaptureKeyboard stays true while the menu bar /
  // toolbar holds focus and would block document shortcuts and status prompts.
  ImGuiIO& io = ImGui::GetIO();
  if (io.WantTextInput) return true;
  if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return true;
  if (ImGui::IsAnyItemActive()) return true;
  return false;
}

bool Ui::want_capture_mouse() const {
  if (!ready_) return false;
  return ImGui::GetIO().WantCaptureMouse;
}

bool Ui::pointer_obscured(float x, float y,
                          std::optional<UiPanelLayer> native_top) const {
  if (!ready_ || !native_top) return false;
  if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId |
                                 ImGuiPopupFlags_AnyPopupLevel)) return false;
  ImGuiWindow* hovered = nullptr;
  ImGui::FindHoveredWindowEx(ImVec2(x, y), true, &hovered, nullptr);
  return hovered && ui_panel_obscured(ui_panel_layer(hovered->Name), native_top);
}

bool Ui::popup_open() const {
  return ready_ && ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId |
                                              ImGuiPopupFlags_AnyPopupLevel);
}

bool Ui::captures_point(float x, float y,
                        std::optional<UiPanelLayer> native_top,
                        bool dismiss_popup) const {
  if (!ready_) return false;
  if (!dismiss_popup && popup_open()) return true;
  ImGuiWindow* hovered = nullptr;
  ImGui::FindHoveredWindowEx(ImVec2(x, y), true, &hovered, nullptr);
  if (!hovered || (dismiss_popup && (hovered->Flags & ImGuiWindowFlags_Popup)))
    return false;
  return !ui_panel_obscured(ui_panel_layer(hovered->Name), native_top);
}

std::optional<UiPanelLayer> Ui::panel_at(float x, float y) const {
  if (!ready_) return std::nullopt;
  if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId |
                                 ImGuiPopupFlags_AnyPopupLevel))
    return std::nullopt;
  ImGuiWindow* hovered = nullptr;
  ImGui::FindHoveredWindowEx(ImVec2(x, y), true, &hovered, nullptr);
  if (!hovered) return std::nullopt;
  const UiPanelLayer layer = ui_panel_layer(hovered->Name);
  if (layer == UiPanelLayer::Chrome) return std::nullopt;
  return layer;
}

void Ui::release_input_capture() {
  if (!ready_) return;
  // Drop the active widget (menu item / toolbar button) and clear keyboard
  // nav focus so the next keystroke is not eaten by ImGui chrome.
  // Field names match Dear ImGui 1.91+ (HoveredId, NavCursorVisible).
  if (GImGui->ActiveId != 0) ImGui::ClearActiveID();
  if (GImGui->HoveredId != 0) GImGui->HoveredId = 0;
  ImGui::FocusWindow(nullptr);
  GImGui->NavId = 0;
  GImGui->NavCursorVisible = false;
}

void Ui::request_menu(const char* name) {
  if (name && name[0]) menu_request_ = name;
}

void Ui::focus_url_bar() { focus_url_bar_ = true; }
void Ui::focus_search() { focus_search_ = true; }

void Ui::open_context_menu(float x, float y) {
  context_open_ = true;
  context_x_ = x;
  context_y_ = y;
}

void Ui::touch_long_press(bool right_click) {
  if (!ready_) return;
  ImGuiIO& io = ImGui::GetIO();
  touch_cancel_press_ = true;
  // No position event: the finger's own motion events placed the pointer,
  // and a move would hold the button events back a frame (touch trickling).
  io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
  io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
  if (right_click) {
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
  }
}

void Ui::close_popups() {
  close_popups_ = true;
  // A menu requested this frame is not open yet: drop the request too.
  context_open_ = window_menu_open_ = tile_menu_open_ = false;
  bookmark_menu_open_ = download_menu_open_ = false;
  media_menu_open_ = history_menu_open_ = revision_menu_open_ = false;
}

void Ui::open_window_menu(float x, float y, uint32_t buffer_id) {
  window_menu_open_ = true;
  window_menu_x_ = x;
  window_menu_y_ = y;
  window_menu_buffer_id_ = buffer_id;
}

void Ui::open_tile_menu(float x, float y) {
  tile_menu_open_ = true;
  tile_menu_x_ = x;
  tile_menu_y_ = y;
}

void Ui::open_bookmark_menu(float x, float y) {
  bookmark_menu_open_ = true;
  context_x_ = x;
  context_y_ = y;
}

void Ui::open_download_menu(float x, float y) {
  download_menu_open_ = true;
  context_x_ = x;
  context_y_ = y;
}

void Ui::open_media_menu(float x, float y) {
  media_menu_open_ = true;
  context_x_ = x;
  context_y_ = y;
}

void Ui::open_history_menu(float x, float y) {
  history_menu_open_ = true;
  context_x_ = x;
  context_y_ = y;
}

static bool menu_item_cmd(const char* label, UiCommand c, UiCommand& out,
                          bool enabled, const char* shortcut) {
  if (menu_item_with_icon(label, menu_icon_for(c), shortcut, false, enabled)) {
    out = c;
    return true;
  }
  return false;
}

// Shared insert/format items used by Format menu and context menu.
// Help → Examples: local pages under examples/ (OpenExample, arg = file).
static void menu_example_items(UiCommand& cmd, std::string& arg) {
  auto ex = [&](const char* label, const char* file) {
    if (ImGui::MenuItem(label)) {
      cmd = UiCommand::OpenExample;
      arg = file;
    }
  };
  for (const xanatoo::ExampleEntry& e : xanatoo::kExamples) {
    if (e.label) ex(e.label, e.file);
    else ImGui::Separator();
  }
  ImGui::Separator();
  if (ImGui::MenuItem("Diff demo (v1 | v2, marks on)"))
    cmd = UiCommand::LoadDiffDemo;
}

static void menu_insert_items(UiCommand& cmd) {
  menu_item_cmd("Paragraph", UiCommand::InsertParagraph, cmd);
  menu_item_cmd("Horizontal Rule", UiCommand::InsertHr, cmd);
  menu_item_cmd("Image…", UiCommand::InsertImage, cmd, true, "Ctrl+Shift+I");
  menu_item_cmd("Figure…", UiCommand::InsertFigure, cmd);
  menu_item_cmd("Note", UiCommand::InsertNote, cmd);
  menu_item_cmd("Code Block", UiCommand::InsertCodeBlock, cmd);
  menu_item_cmd("Table", UiCommand::InsertTable, cmd);
  menu_item_cmd("Definition List", UiCommand::InsertDefList, cmd);
  menu_item_cmd("Bibliography Entry", UiCommand::InsertBiblioEntry, cmd);
  menu_item_cmd("Columns (2)", UiCommand::InsertColumns, cmd);
  menu_item_cmd("Include / Transclude…", UiCommand::InsertTransclude, cmd);
}

// Collection view: layout, card size, sort, fullscreen viewer.
static void menu_collection_items(const UiState& state, UiCommand& cmd,
                                  std::string& arg) {
  auto item = [&](const char* label, const char* a, bool checked,
                  const char* shortcut = nullptr) {
    if (ImGui::MenuItem(label, shortcut, checked, state.collection)) {
      cmd = UiCommand::CollectionView;
      arg = a;
    }
  };
  item("View Fullscreen", "view", false);
  ImGui::Separator();
  item("Grid", "grid", state.collection_layout == 0);
  item("List", "list", state.collection_layout == 1);
  item("Full Size", "full", state.collection_layout == 2);
  ImGui::Separator();
  item("Small Cards", "small", state.collection_thumb < 140.f);
  item("Medium Cards", "medium",
       state.collection_thumb >= 140.f && state.collection_thumb < 200.f);
  item("Large Cards", "large", state.collection_thumb >= 200.f);
  ImGui::Separator();
  item("Document Order", "sort-doc", state.collection_sort == 0);
  item("Sort by Name", "sort-name", state.collection_sort == 1);
  item("Sort by Name (Reverse)", "sort-name-desc", state.collection_sort == 2);
}

static void menu_structure_items(const UiState& state, UiCommand& cmd) {
  menu_item_cmd("Unordered List", UiCommand::ToggleUnorderedList, cmd, true,
                "Ctrl+Shift+8");
  menu_item_cmd("Ordered List", UiCommand::ToggleOrderedList, cmd, true,
                "Ctrl+Shift+7");
  menu_item_cmd("Task Item", UiCommand::ToggleTask, cmd, true, "Ctrl+Alt+T");
  menu_item_cmd("Block Quote", UiCommand::ToggleBlockQuote, cmd);
  menu_item_cmd("Indent List Item", UiCommand::IndentList, cmd,
                state.on_list_item, "Tab");
  menu_item_cmd("Outdent List Item", UiCommand::OutdentList, cmd,
                state.on_list_item, "Shift+Tab");
  ImGui::Separator();
  menu_item_cmd("Wrap in Stretch", UiCommand::WrapStretch, cmd, true, "Ctrl+E");
  menu_item_cmd("Edit Stretch Summary…", UiCommand::EditStretchSummary, cmd,
                state.in_stretch, "Ctrl+Shift+E");
  menu_item_cmd("Unwrap Stretch", UiCommand::UnwrapStretch, cmd, state.in_stretch,
                "Ctrl+Shift+U");
  ImGui::Separator();
  menu_item_cmd("Heading", UiCommand::MakeHeader, cmd, true, "Ctrl+1");
  menu_item_cmd("Wrap in Section", UiCommand::WrapSection, cmd, true,
                "Ctrl+Shift+1");
  menu_item_cmd("Wrap in Article", UiCommand::WrapArticle, cmd, true,
                "Ctrl+Shift+2");
  menu_item_cmd("Unwrap Section/Article", UiCommand::UnwrapSectioning, cmd);
  menu_item_cmd("Promote Section", UiCommand::PromoteSection, cmd, true,
                "Ctrl+Alt+]");
  menu_item_cmd("Demote Section", UiCommand::DemoteSection, cmd, true,
                "Ctrl+Alt+[");
  menu_item_cmd("Move Section Up", UiCommand::MoveSectionUp, cmd, true,
                "Alt+Shift+Up");
  menu_item_cmd("Move Section Down", UiCommand::MoveSectionDown, cmd, true,
                "Alt+Shift+Down");
  ImGui::Separator();
  menu_item_cmd("Heading (depth 2)", UiCommand::Heading2, cmd, true, "Ctrl+2");
  menu_item_cmd("Heading (depth 3)", UiCommand::Heading3, cmd, true, "Ctrl+3");
  menu_item_cmd("Paragraph (demote)", UiCommand::ParagraphLevel, cmd, true,
                "Ctrl+0");
}

// Toolbar "View tools" menu: also the display menus (line spacing, text
// width, style sheet).
static void view_tools_items(const UiState& state, UiCommand& cmd,
                             UiFrameResult& result) {
  auto& arg = result.arg;
  {
    if (ImGui::BeginMenu("Line spacing")) {
      fill_line_spacing_items(state, cmd);
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Text width")) {
      fill_measure_items(state, cmd, arg);
      ImGui::Separator();
      ImGui::TextDisabled("Margins");
      fill_margins_items(state, cmd, arg);
      ImGui::EndMenu();
    }
    fill_style_menus(state, cmd, result);
  }
  if (!state.browse)
    menu_item_cmd("Replace…", UiCommand::Replace, cmd, true, "Ctrl+H");
  ImGui::Separator();
  if (ImGui::MenuItem("Backlinks", "Ctrl+Alt+B", state.backlinks))
    cmd = UiCommand::ToggleBacklinks;
  if (ImGui::MenuItem("Site map", "Ctrl+Alt+M", state.sitemap))
    cmd = UiCommand::ToggleSitemap;
  if (ImGui::MenuItem("Page navigation", "Ctrl+Alt+P", state.page_panel))
    cmd = UiCommand::TogglePage;
  menu_item_cmd("Open include → other pane", UiCommand::OpenTranscludeOther,
                cmd);
  if (ImGui::MenuItem("Structure highlight", "Ctrl+Shift+L", state.structure))
    cmd = UiCommand::ToggleStructure;
  if (ImGui::MenuItem("Compare tiles", "Ctrl+Shift+D", state.diff, state.can_diff))
    cmd = UiCommand::ToggleDiff;
  ImGui::Separator();
  menu_item_cmd("Zoom in", UiCommand::ZoomIn, cmd, true, "Ctrl++");
  menu_item_cmd("Zoom out", UiCommand::ZoomOut, cmd, true, "Ctrl+-");
  ImGui::Separator();
  menu_item_cmd("Content only", UiCommand::ToggleChrome, cmd, true, "F11");
  menu_item_cmd("Help", UiCommand::ToggleHelp, cmd, true, "F1");
  if (ImGui::MenuItem("Virtual keyboard", "F3", state.virtual_keyboard))
    cmd = UiCommand::ToggleVirtualKeyboard;
}

// Browse (navigation) row, edit row (not while browsing) and tab strip.
// Whether the menu font has a glyph for every character of `s` (UTF-8).
static bool ui_can_draw(const std::string& s) {
  ImFont* font = ImGui::GetFont();
  const char* p = s.c_str();
  const char* end = p + s.size();
  while (p < end) {
    unsigned int c = 0;
    const int n = ImTextCharFromUtf8(&c, p, end);
    if (n <= 0) return false;
    p += n;
    if (c >= 0x20 && !font->FindGlyphNoFallback(static_cast<ImWchar>(c)))
      return false;
  }
  return true;
}

// Preferred languages as a comma list; Enter or Set issues SetLanguages.
// Follows the saved list while not being edited.
// Bookmarks panel: a folder tree (click a folder to work in it, drag rows
// onto folders to move them), inline renaming, and a filter that lists
// matches flat. docs/UI.md "Bookmarks".
void Ui::bookmarks_panel(const UiState& state, UiCommand& cmd, std::string& arg) {
  static constexpr const char* kDragMark = "XT_BOOKMARK";
  static constexpr const char* kDragFolder = "XT_BM_FOLDER";
  const auto& folders = state.bookmark_folders;
  if (!bm_target_folder_.empty() &&
      std::find(folders.begin(), folders.end(), bm_target_folder_) == folders.end())
    bm_target_folder_.clear();

  auto start_edit = [&](std::string key, const std::string& text) {
    bm_edit_key_ = std::move(key);
    std::snprintf(bm_edit_, sizeof bm_edit_, "%s", text.c_str());
    bm_edit_focus_ = true;
  };
  // 0: `key` is not being edited; 1: its field is drawn; 2: committed
  // (the text is in bm_edit_).
  auto edit_field = [&](const std::string& key, const char* hint) -> int {
    if (bm_edit_key_ != key) return 0;
    if (bm_edit_focus_) {
      ImGui::SetKeyboardFocusHere();
      bm_edit_focus_ = false;
    }
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::InputTextWithHint("##bm_edit", hint, bm_edit_, sizeof bm_edit_,
                                 ImGuiInputTextFlags_EnterReturnsTrue |
                                 ImGuiInputTextFlags_AutoSelectAll)) {
      bm_edit_key_.clear();
      return 2;
    }
    // Esc or a click elsewhere: cancel.
    if (ImGui::IsItemDeactivated()) bm_edit_key_.clear();
    return 1;
  };
  auto drag_source = [&](const char* type, const std::string& value,
                         const std::string& label) {
    if (!ImGui::BeginDragDropSource()) return;
    ImGui::SetDragDropPayload(type, value.data(), value.size());
    ImGui::TextUnformatted(label.c_str());
    ImGui::EndDragDropSource();
  };
  // Dropping a bookmark or folder on the item just drawn moves it into
  // `folder`.
  auto drop_target = [&](const std::string& folder) {
    if (!ImGui::BeginDragDropTarget()) return;
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kDragMark)) {
      cmd = UiCommand::MoveBookmark;
      arg = std::string(static_cast<const char*>(p->Data),
                        static_cast<size_t>(p->DataSize)) + "\n" + folder;
    }
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kDragFolder)) {
      const std::string from(static_cast<const char*>(p->Data),
                             static_cast<size_t>(p->DataSize));
      const std::string leaf(bookmark_folder_leaf(from));
      if (!bookmark_folder_within(folder, from) &&
          bookmark_folder_parent(from) != folder) {
        cmd = UiCommand::MoveBookmarkFolder;
        arg = from + "\n" + (folder.empty() ? leaf : folder + "/" + leaf);
      }
    }
    ImGui::EndDragDropTarget();
  };

  // ---- Toolbar ----
  const char* add_label = state.current_bookmarked ? "Remove Bookmark"
                          : state.selection        ? "Bookmark Selection"
                                                   : "Bookmark Page";
  ImGui::BeginDisabled(!state.has_path);
  if (classic_button(add_label)) {
    cmd = state.current_bookmarked ? UiCommand::RemoveBookmark
                                   : UiCommand::AddBookmark;
    arg = state.current_bookmarked ? std::string() : bm_target_folder_;
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) &&
      !state.current_bookmarked)
    ImGui::SetTooltip("Into %s", bm_target_folder_.empty()
                                     ? "the top level (click a folder to pick one)"
                                     : bm_target_folder_.c_str());
  ImGui::SameLine();
  if (classic_button("New Folder")) start_edit("n:" + bm_target_folder_, "");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("New folder in %s (\"a/b\" makes nested folders)",
                      bm_target_folder_.empty() ? "the top level"
                                                : bm_target_folder_.c_str());
  ImGui::SameLine();
  ImGui::SetNextItemWidth(-1.f);
  ImGui::InputTextWithHint("##bm_filter", "Filter…", bm_filter_, sizeof bm_filter_);
  if (starts_with(bm_edit_key_, "n:")) {
    const std::string parent = bm_edit_key_.substr(2);
    if (edit_field(bm_edit_key_, parent.empty() ? "New folder name"
                                                : ("New folder in " + parent).c_str()) == 2) {
      const std::string name = normalize_bookmark_folder(bm_edit_);
      if (!name.empty()) {
        cmd = UiCommand::NewBookmarkFolder;
        arg = parent.empty() ? name : parent + "/" + name;
      }
    }
  }
  ImGui::Separator();

  const float footer = ImGui::GetFrameHeightWithSpacing();
  if (begin_panel_list("##bookmarks_list", ImVec2(0.f, -footer), false)) {
    std::unordered_map<std::string, int> counts;
    for (const Bookmark& b : state.bookmarks)
      for (std::string f = b.folder; !f.empty(); f = bookmark_folder_parent(f))
        ++counts[f];

    auto bookmark_row = [&](size_t i, bool show_folder) {
      const Bookmark& b = state.bookmarks[i];
      ImGui::PushID(static_cast<int>(i));
      if (const int e = edit_field("b:" + b.address, "Title")) {
        if (e == 2) {
          cmd = UiCommand::RetitleBookmark;
          arg = b.address + "\n" + bm_edit_;
        }
        ImGui::PopID();
        return;
      }
      // The quote gets a wrapped line of its own below.
      const std::string label =
          b.quote.empty() || b.title.empty() ? bookmark_label(b) : b.title;
      if (i < state.bookmark_icons.size() && state.bookmark_icons[i]) {
        ImGui::Image(reinterpret_cast<ImTextureID>(state.bookmark_icons[i]),
                     icon_size(state.bookmark_icons[i], ImGui::GetTextLineHeight()));
        ImGui::SameLine();
      }
      if (ImGui::Selectable(label.c_str())) {
        cmd = UiCommand::NavigateUrl;
        arg = b.address;
      }
      middle_open(cmd, arg, b.address);
      drag_source(kDragMark, b.address, label);
      drop_target(b.folder);
      if (ImGui::IsItemHovered() && label != b.address)
        ImGui::SetTooltip("%s", b.address.c_str());
      if (ImGui::BeginPopupContextItem("##bm_ctx")) {
        if (ImGui::MenuItem("Open")) {
          cmd = UiCommand::NavigateUrl;
          arg = b.address;
        }
        if (ImGui::MenuItem("Open in New Tab")) {
          cmd = UiCommand::OpenLinkInTab;
          arg = b.address;
        }
        // The page as it was when bookmarked (its pinned snapshot).
        if (ImGui::MenuItem("Open as Bookmarked", nullptr, false,
                            !b.snapshot.empty())) {
          cmd = UiCommand::NavigateUrl;
          arg = xanatoo::snapshot_ref_string(
              b.snapshot, xanatoo::parse_locator(b.address).base);
        }
        if (ImGui::MenuItem("Rename…"))
          start_edit("b:" + b.address, b.title.empty() ? label : b.title);
        if (ImGui::BeginMenu("Move To")) {
          bookmark_move_items(state, b.address, b.folder, cmd, arg);
          ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Copy Address")) {
          cmd = UiCommand::CopyText;
          arg = b.address;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Remove")) {
          cmd = UiCommand::RemoveBookmark;
          arg = b.address;
        }
        ImGui::EndPopup();
      }
      std::string note = b.quote.empty() || b.title.empty() ? std::string() : b.quote;
      if (show_folder && !b.folder.empty())
        note = note.empty() ? b.folder : b.folder + " · " + note;
      if (!note.empty()) {
        ImGui::Indent();
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", note.c_str());
        ImGui::PopStyleColor();
        ImGui::Unindent();
      }
      ImGui::PopID();
    };

    std::function<void(const std::string&)> folder_level =
        [&](const std::string& parent) {
      for (const std::string& f : folders) {
        if (bookmark_folder_parent(f) != parent) continue;
        ImGui::PushID(f.c_str());
        const std::string leaf(bookmark_folder_leaf(f));
        bool open = false;
        if (const int e = edit_field("f:" + f, "Folder name")) {
          const std::string name = normalize_bookmark_folder(bm_edit_);
          if (e == 2 && !name.empty()) {
            cmd = UiCommand::MoveBookmarkFolder;
            arg = f + "\n" + (parent.empty() ? name : parent + "/" + name);
          }
        } else {
          const auto it = counts.find(f);
          const int n = it == counts.end() ? 0 : it->second;
          const ImGuiTreeNodeFlags flags =
              ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
              ImGuiTreeNodeFlags_SpanAvailWidth |
              (f == bm_target_folder_ ? ImGuiTreeNodeFlags_Selected : 0);
          open = ImGui::TreeNodeEx("##folder", flags, "%s  (%d)", leaf.c_str(), n);
          if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
            bm_target_folder_ = f == bm_target_folder_ ? std::string() : f;
          drag_source(kDragFolder, f, leaf);
          drop_target(f);
          if (ImGui::IsItemHovered() && !ImGui::GetDragDropPayload())
            ImGui::SetTooltip("%s\nClick: bookmark into this folder", f.c_str());
          if (ImGui::BeginPopupContextItem("##folder_ctx")) {
            if (ImGui::MenuItem("Bookmark Page Here", nullptr, false,
                                state.has_path && !state.current_bookmarked)) {
              cmd = UiCommand::AddBookmark;
              arg = f;
            }
            if (ImGui::MenuItem("New Subfolder…")) start_edit("n:" + f, "");
            if (ImGui::MenuItem("Rename…")) start_edit("f:" + f, leaf);
            if (ImGui::BeginMenu("Move To")) {
              bookmark_move_items(state, {}, f, cmd, arg);
              ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Remove Folder")) {
              cmd = UiCommand::RemoveBookmarkFolder;
              arg = f;
            }
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Its bookmarks and subfolders move up one level");
            ImGui::EndPopup();
          }
        }
        if (open) {
          folder_level(f);
          for (size_t i = 0; i < state.bookmarks.size(); ++i)
            if (state.bookmarks[i].folder == f) bookmark_row(i, false);
          ImGui::TreePop();
        }
        ImGui::PopID();
      }
    };

    const std::string filter = ascii_lower(std::string(bm_filter_));
    if (state.bookmarks.empty() && folders.empty()) {
      ImGui::TextWrapped("No bookmarks yet. Bookmark Page adds the current page, "
                         "New Folder makes a folder to sort them into.");
    } else if (!filter.empty()) {
      int shown = 0;
      for (size_t i = 0; i < state.bookmarks.size(); ++i) {
        const Bookmark& b = state.bookmarks[i];
        auto has = [&](const std::string& s) {
          return ascii_lower(s).find(filter) != std::string::npos;
        };
        if (!has(b.address) && !has(b.title) && !has(b.quote) && !has(b.folder))
          continue;
        bookmark_row(i, true);
        ++shown;
      }
      if (!shown) ImGui::TextDisabled("No bookmark matches.");
    } else {
      folder_level({});
      for (size_t i = 0; i < state.bookmarks.size(); ++i)
        if (state.bookmarks[i].folder.empty()) bookmark_row(i, false);
      // The free space below the rows: drop here for the top level, click
      // here to bookmark into the top level again.
      const float rest = std::max(ImGui::GetTextLineHeight(),
                                  ImGui::GetContentRegionAvail().y);
      ImGui::InvisibleButton("##bm_top", ImVec2(-FLT_MIN, rest));
      if (ImGui::IsItemClicked()) bm_target_folder_.clear();
      drop_target({});
    }
  }
  ImGui::EndChild();
  ImGui::TextDisabled(bm_target_folder_.empty()
                          ? "Click: open · middle: new tab · drag onto a folder: move"
                          : "Click a folder again or empty space: back to the top level");
}

void Ui::languages_field(const UiState& state, UiCommand& cmd, std::string& arg,
                         const char* id) {
  if (!languages_editing_ && languages_synced_ != state.languages) {
    std::snprintf(languages_input_, sizeof(languages_input_), "%s",
                  state.languages.c_str());
    languages_synced_ = state.languages;
  }
  ImGui::PushID(id);
  ImGui::SetNextItemWidth(220.f * scale_);
  const bool enter = ImGui::InputTextWithHint(
      "##languages", "de, en  (empty: en-US, en)", languages_input_,
      sizeof(languages_input_), ImGuiInputTextFlags_EnterReturnsTrue);
  languages_editing_ = ImGui::IsItemActive();
  ImGui::SameLine();
  if (classic_button("Set") || enter) {
    cmd = UiCommand::SetLanguages;
    arg = languages_input_;
    languages_editing_ = false;
    languages_synced_.clear();  // take the normalised list back
  }
  ImGui::PopID();
}

void Ui::toolbars(const UiState& state, UiFrameResult& result, UiCommand& cmd) {
  // These controls are actions as well as toggles. Keep the current mode
  // sunken, rather than giving it a bright (hover-like) selection wash.
  auto toggle_button = [&](const char* id, const char* icon, const char* tip,
                           UiCommand c, bool on) {
    tool_button(id, icon, tip, c, cmd, true, nullptr, on);
  };

  // ---- The toolbar (navigation); editing has a strip on each page ----
  // An icon button is 28·s plus 4·s frame padding above and below.
  const float tool_button_h = 36.f * scale_;
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGuiWindowFlags tb_flags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoScrollWithMouse;


  // -- Browse / navigation row (browser chrome) --
  // Icon row, then the location row (its field is centred in 32·s + 6),
  // sized to their contents with only a small gap between them.
  const float field_row_h = 32.f * scale_ + 6.f;
  const float field_row_y = 4.f + tool_button_h + 2.f * scale_;
  const int url_extras = (state.alternate_url.empty() ? 0 : 1) +
                         (state.page_feeds.empty() ? 0 : 1) +
                         (state.page_languages.empty() ? 0 : 1) +
                         (state.has_chrome ? 1 : 0) +
                         (state.has_nav ? 1 : 0) + (state.has_site ? 1 : 0) +
                         (state.has_headings ? 1 : 0) +
                         (state.site_search.empty() ? 0 : 1);
  // The location field has a row of its own, shared with the search bar.
  // Both fields carry their icon at the left end inside the frame (favicon,
  // engine tile) and a green Go triangle at the right end.
  const float go_w = 26.f * scale_;
  const float field_icon_w = 30.f * scale_;
  // Search bar width: engine tile + a field for an average query + Go,
  // never more than ~40 % of the row so the location keeps most of it.
  const float search_field_w = std::min(
      ImGui::CalcTextSize("an average search query here").x + 16.f * scale_,
      std::max(80.f * scale_, vp->Size.x * 0.4f - field_icon_w - go_w));
  const float search_w = state.searchbar
      ? 6.f * scale_ + field_icon_w + search_field_w + go_w
      : 0.f;
  const float browse_h = field_row_y + field_row_h + 4.f;
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + result.top_reserved));
  ImGui::SetNextWindowSize(ImVec2(vp->Size.x, browse_h));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.f, 4.f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
  if (ImGui::Begin("##toolbar_browse", nullptr, tb_flags)) {
    auto history_menu = [&](const char* id,
                            const std::vector<HistoryMenuItem>& entries,
                            const char* empty_label) {
      if (!ImGui::BeginPopup(id)) return;
      if (entries.empty()) ImGui::TextDisabled("%s", empty_label);
      for (const HistoryMenuItem& e : entries) {
        const std::string label = elide_middle(e.url, 68);
        ImGui::PushID(e.steps);
        if (ImGui::MenuItem(label.c_str())) {
          cmd = UiCommand::NavJump;
          result.arg = std::to_string(e.steps);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
          ImGui::SetTooltip("%s", e.url.c_str());
        ImGui::PopID();
      }
      ImGui::EndPopup();
    };
    bool back_context = false;
    tool_button("tb_back", "back", "Back (Alt+Left)", UiCommand::NavBack, cmd,
                state.can_nav_back, &back_context);
    if (back_context) ImGui::OpenPopup("##back_history");
    history_menu("##back_history", state.back_history, "No previous pages");
    bool forward_context = false;
    tool_button("tb_fwd", "forward", "Forward (Alt+Right)", UiCommand::NavForward,
                cmd, state.can_nav_forward, &forward_context);
    if (forward_context) ImGui::OpenPopup("##forward_history");
    history_menu("##forward_history", state.forward_history, "No next pages");
    {
      const std::string tip = state.up_url.empty()
                                  ? std::string("Up (at the top)")
                                  : "Up (Alt+Up when read-only): " + state.up_url;
      tool_button("tb_up", "up", tip.c_str(), UiCommand::GoUp, cmd,
                  !state.up_url.empty());
    }
    tool_button("tb_home", "home", "Home (Alt+Home)", UiCommand::GoHome, cmd);
    if (state.loading)
      tool_button("tb_stop", "stop", "Stop loading (Esc)",
                  UiCommand::StopLoading, cmd, true);
    else {
      // Shift+click forces; right-click offers both.
      bool reload_context = false;
      tool_button("tb_reload", "reload",
                  "Reload (Ctrl+R, F5) · Shift+click: Force Reload "
                  "(Ctrl+Shift+R, Shift+F5)",
                  UiCommand::Reload, cmd, state.has_path, &reload_context);
      if (cmd == UiCommand::Reload && ImGui::GetIO().KeyShift)
        cmd = UiCommand::ForceReload;
      if (reload_context) ImGui::OpenPopup("##reload_menu");
      if (ImGui::BeginPopup("##reload_menu")) {
        menu_item_cmd("Reload", UiCommand::Reload, cmd, state.has_path,
                      "Ctrl+R / F5");
        menu_item_cmd("Force Reload", UiCommand::ForceReload, cmd,
                      state.has_path, "Ctrl+Shift+R / Shift+F5");
        ImGui::EndPopup();
      }
    }

    // Location / URL field (path or gemini://)
    auto location_field = [&] {
      const float row_y = ImGui::GetCursorPosY();
      const float button_h = 32.f * scale_ + 6.f;
      const float field_h = 32.f * scale_;
      const std::string& want = state.doc_label;
      if (state.dirty) {
        ImGui::SetCursorPosY(row_y +
                             (button_h - ImGui::GetTextLineHeight()) * 0.5f);
        ImGui::TextDisabled("*");
        ImGui::SameLine(0.f, 2.f);
      }
      // Context controls stay with the address on either toolbar row.
      const float right_pad = url_extras ? 2.f * scale_ : 0.f;
      const float extra_w = url_extras * 28.f * scale_ + right_pad + go_w;
      // Leave room for the search bar right of the field.
      float field_w = ImGui::GetContentRegionAvail().x -
                      (search_w > 0.f ? search_w + 6.f : 0.f);
      if (field_w < 40.f + extra_w) field_w = 40.f + extra_w;
      ImGui::SetNextItemWidth(std::max(40.f, field_w - extra_w));
      ImGui::SetCursorPosY(row_y + (button_h - field_h) * 0.5f);
      const ImVec2 field_pos = ImGui::GetCursorScreenPos();
      const ImVec2 field_end(field_pos.x + field_w, field_pos.y + field_h);
      ImDrawList* draw = ImGui::GetWindowDrawList();
      draw->AddRectFilled(field_pos, field_end, IM_COL32(242, 242, 247, 255),
                          ImGui::GetStyle().FrameRounding);
      draw->AddRect(field_pos, field_end, ImGui::GetColorU32(ImGuiCol_Border),
                    ImGui::GetStyle().FrameRounding);

      const ImGuiID url_id = ImGui::GetID("##urlbar");
      const bool context_open = ImGui::IsPopupOpen("##url_context");
      ImGuiContext& imgui = *ImGui::GetCurrentContext();
      const bool escape_from_context = url_context_was_open_ && !context_open &&
          url_context_was_active_ && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
      if (escape_from_context && !focus_url_bar_) {
        // ImGui closed the popup during NewFrame. Resume this input now,
        // before it draws, with its existing caret and selection.
        imgui.NavActivateId = url_id;
        imgui.NavActivateFlags = ImGuiActivateFlags_PreferInput |
                                 ImGuiActivateFlags_TryToPreserveState;
      }
      const bool resume_context = (escape_from_context || url_context_resume_) &&
          !focus_url_bar_ && imgui.NavActivateId == url_id;
      url_context_resume_ = false;
      if (focus_url_bar_) {
        // The new location supersedes text left by the previous activation.
        if (imgui.InputTextDeactivatedState.ID == url_id)
          imgui.InputTextDeactivatedState.ID = 0;
        if (imgui.ActiveId == url_id && imgui.InputTextState.ID == url_id)
          imgui.InputTextState.ReloadUserBufAndSelectAll();
        else
          ImGui::SetKeyboardFocusHere();
        std::snprintf(url_input_, sizeof(url_input_), "%s", want.c_str());
        url_synced_from_ = want;
        focus_url_bar_ = false;
      } else if (want != url_synced_from_ && !url_bar_editing_ &&
                 !context_open && !resume_context) {
        std::snprintf(url_input_, sizeof(url_input_), "%s", want.c_str());
        url_synced_from_ = want;
      }

      ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.f, 0.f, 0.f, 0.f));
      ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.f, 0.f, 0.f, 0.f));
      ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.f, 0.f, 0.f, 0.f));
      ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
      // Site identity takes the favicon's place: Gemini has no favicons, its
      // icon opens the certificate (and warns when that changed).
      SDL_Texture* loc_icon = state.location_icon;
      const bool site_button =
          state.site_identity != UiState::SiteIdentity::None;
      if (site_button) {
        const Icon* ic = icon(state.site_identity ==
                                      UiState::SiteIdentity::GeminiChanged
                                  ? "gemini-changed" : "gemini");
        loc_icon = ic ? ic->texture : nullptr;
      }
      // No favicon of its own: a search shows its engine's tile, local
      // files, built-in pages and the like a pseudo-favicon.
      const UiSearchEngine* loc_engine = nullptr;
      if (!loc_icon && !site_button) {
        for (const UiSearchEngine& e : state.search_engines)
          if (e.id == state.location_engine) loc_engine = &e;
        if (!loc_engine && !state.location_pseudo_icon.empty())
          if (const Icon* ic = icon(state.location_pseudo_icon.c_str()))
            loc_icon = ic->texture;
      }
      const bool has_loc_icon = loc_icon || site_button || loc_engine;
      ImGui::PushStyleVar(
          ImGuiStyleVar_FramePadding,
          ImVec2((has_loc_icon ? 30.f : 6.f) * scale_,
                 std::max(0.f, (field_h - ImGui::GetFontSize()) * 0.5f)));
      // The site button below overlaps the field's left padding.
      if (site_button) ImGui::SetNextItemAllowOverlap();
      ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue;
      // Middle-click paste inserts at the click. Returning from the menu
      // keeps the caret and anchor of the existing selection.
      if (middle_paste_frames_ == 0 && !resume_context)
        flags |= ImGuiInputTextFlags_AutoSelectAll;
      if (context_open || resume_context)
        flags |= ImGuiInputTextFlags_RenderInactiveSelection;
      if (resume_context && (url_context_edit_ || url_context_select_all_))
        flags |= ImGuiInputTextFlags_CallbackAlways;
      bool enter = ImGui::InputText(
          "##urlbar", url_input_, sizeof(url_input_), flags,
          [](ImGuiInputTextCallbackData* data) {
            auto& ui = *static_cast<Ui*>(data->UserData);
            if (const auto& edit = ui.url_context_edit_) {
              const int removed = edit->end - edit->begin;
              const int inserted = static_cast<int>(edit->replacement.size());
              if (edit->end <= data->BufTextLen &&
                  inserted < data->BufSize - (data->BufTextLen - removed)) {
                if (removed) data->DeleteChars(edit->begin, removed);
                if (inserted)
                  data->InsertChars(edit->begin, edit->replacement.c_str());
                data->CursorPos = data->SelectionStart = data->SelectionEnd =
                    edit->begin + inserted;
              }
            }
            if (ui.url_context_select_all_) {
              data->SelectionStart = 0;
              data->CursorPos = data->SelectionEnd = data->BufTextLen;
            }
            return 0;
          }, this);
      url_context_edit_.reset();
      url_context_select_all_ = false;
      const ImGuiInputTextState* input = ImGui::GetInputTextState(url_id);
      const bool has_selection = input && input->HasSelection();
      const int selection_start = has_selection ? input->GetSelectionStart() : 0;
      const int selection_end = has_selection ? input->GetSelectionEnd() : 0;
      const bool url_active = ImGui::IsItemActive();
      url_field_active_ = url_active;
      url_suggestion_x_ = field_pos.x;
      url_suggestion_y_ = field_end.y + 2.f * scale_;
      url_suggestion_w_ = field_w;
      if (url_input_[0] == '\0') {
        url_suggestions_open_ = false;
        url_suggestion_selected_ = -1;
        url_suggestion_query_.clear();
      } else if (url_active) {
        if (ImGui::IsItemActivated() || ImGui::IsItemEdited()) {
          if (!resume_context || ImGui::IsItemEdited())
            url_suggestions_dismissed_ = false;
          if (ImGui::IsItemActivated()) url_suggestion_selected_ = -1;
        }
        if (!url_suggestions_dismissed_) url_suggestions_open_ = true;
        // Ctrl+L selects the whole location: offer recent pages rather than
        // matching only the current address. Editing replaces that query.
        const bool all_selected = has_selection &&
            std::min(selection_start, selection_end) == 0 &&
            std::max(selection_start, selection_end) ==
                static_cast<int>(std::strlen(url_input_));
        std::string query = all_selected ? "" : url_input_;
        if (!ImGui::IsItemEdited() && url_suggestion_selected_ >= 0)
          query = url_suggestion_query_;  // arrows mustn't change the query
        if (query != url_suggestion_query_) {
          url_suggestion_query_ = std::move(query);
          url_suggestion_selected_ = -1;
        }
        const auto candidates = xanatoo::urlbar_suggestions(
            url_suggestion_query_, state.urlbar_entries);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && !candidates.empty())
          url_suggestion_selected_ = std::min(
              url_suggestion_selected_ + 1,
              static_cast<int>(candidates.size()) - 1);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && !candidates.empty())
          url_suggestion_selected_ = url_suggestion_selected_ <= 0
              ? static_cast<int>(candidates.size()) - 1
              : url_suggestion_selected_ - 1;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
          url_suggestions_open_ = false;
          url_suggestions_dismissed_ = true;
        }
      }
      const bool url_right_clicked = ImGui::IsItemHovered() &&
          ImGui::IsMouseReleased(ImGuiMouseButton_Right);
      if (url_right_clicked) {
        url_suggestions_open_ = false;
        url_suggestions_dismissed_ = true;
        url_context_was_active_ = url_active;
      }
      const bool url_hovered =
          ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal);
      if (has_loc_icon) {
        const ImVec2 p = ImGui::GetItemRectMin();
        const float side = 20.f * scale_;
        const float x0 = p.x + 5.f * scale_;
        if (site_button) {
          // A button the size of the icon box: tap / click opens the
          // Certificates panel on this server (touch has no Enter / Esc).
          // ItemAdd + ButtonBehavior: a real item for hit-testing that
          // places nothing, so the row's SameLine layout is unchanged.
          const float pad = 3.f * scale_;
          const float by = p.y + (field_h - side) * 0.5f - pad;
          const ImRect bb(ImVec2(x0 - pad, by),
                          ImVec2(x0 + side + pad, by + side + 2 * pad));
          const ImGuiID id = ImGui::GetID("##site_identity");
          bool hot = false, held = false;
          if (ImGui::ItemAdd(bb, id) &&
              ImGui::ButtonBehavior(bb, id, &hot, &held) &&
              cmd == UiCommand::None) {
            cmd = UiCommand::ToggleCertificates;
            result.arg = state.site_identity_key;
          }
          if (hot || held)
            ImGui::GetWindowDrawList()->AddRectFilled(
                bb.Min, bb.Max,
                ImGui::GetColorU32(held ? ImGuiCol_ButtonActive
                                        : ImGuiCol_ButtonHovered),
                ImGui::GetStyle().FrameRounding);
          if (hot) ImGui::SetTooltip("%s", state.site_identity_tip.c_str());
        }
        if (loc_engine) {
          draw_engine_tile(ImGui::GetWindowDrawList(),
                           ImVec2(x0, p.y + (field_h - side) * 0.5f), side,
                           loc_engine);
        } else if (loc_icon) {
          const ImVec2 size = icon_size(loc_icon, side);
          const float y = p.y + (field_h - size.y) * 0.5f;
          ImGui::GetWindowDrawList()->AddImage(
              reinterpret_cast<ImTextureID>(loc_icon),
              ImVec2(x0 + (side - size.x) * 0.5f, y),
              ImVec2(x0 + (side + size.x) * 0.5f, y + size.y));
        } else if (site_button) {
          // No icon files (no --data-dir): a plain marker still works.
          ImGui::GetWindowDrawList()->AddText(
              ImVec2(x0, p.y + (field_h - ImGui::GetFontSize()) * 0.5f),
              ImGui::GetColorU32(ImGuiCol_Text),
              state.site_identity == UiState::SiteIdentity::GeminiChanged
                  ? "!" : "G");
        }
      }
      ImGui::PopStyleVar(2);
      ImGui::PopStyleColor(3);
      url_bar_editing_ = url_active;
      if (url_hovered && !(site_button && ImGui::IsItemHovered()))
        ImGui::SetTooltip(
            "Location — URL or path · Ctrl+L · ↑↓ suggestions · Enter opens");
      if (url_right_clicked) ImGui::OpenPopup("##url_context");
      url_context_was_open_ = ImGui::IsPopupOpen("##url_context");
      if (ImGui::BeginPopup("##url_context")) {
        auto url_item = [&](const char* label, const char* icon_name,
                            bool enabled = true) {
          return menu_item_with_icon(label, icon_name, nullptr, false, enabled);
        };
        const std::string value = url_input_;
        const int len = static_cast<int>(value.size());
        const bool current_input = input && input->TextLen == len &&
            value == input->TextA.Data;
        const int cursor = current_input ? input->GetCursorPos() : len;
        const int from = current_input && has_selection
            ? std::min(selection_start, selection_end) : cursor;
        const int to = current_input && has_selection
            ? std::max(selection_start, selection_end) : cursor;
        const bool selected = from < to;
        const char* clip = ImGui::GetClipboardText();
        const std::string clipboard = clip ? clip : "";
        auto resume_url = [&] {
          url_context_resume_ = true;
          ImGui::ActivateItemByID(url_id);
          imgui.NavNextActivateFlags = ImGuiActivateFlags_PreferInput |
                                       ImGuiActivateFlags_TryToPreserveState;
        };
        auto edit_url = [&](std::string replacement) {
          url_context_edit_ = UrlContextEdit{from, to, std::move(replacement)};
          resume_url();
        };
        if (url_item("Cut", "cut", selected)) {
          ImGui::SetClipboardText(value.substr(from, to - from).c_str());
          edit_url("");
        }
        if (url_item("Copy", "copy", selected)) {
          ImGui::SetClipboardText(value.substr(from, to - from).c_str());
          resume_url();
        }
        if (url_item("Paste", "paste", !clipboard.empty()))
          edit_url(clipboard);
        if (url_item("Select All", "select-all")) {
          url_context_select_all_ = true;
          resume_url();
        }
        ImGui::Separator();
        if (url_item("Copy Address", "link", !value.empty()))
          ImGui::SetClipboardText(value.c_str());
        if (url_item("Open Address", "open", !value.empty())) {
          cmd = UiCommand::NavigateUrl;
          result.arg = value;
        }
        if (url_item("Paste and Go", "forward", !clipboard.empty())) {
          cmd = UiCommand::NavigateUrl;
          result.arg = clipboard;
        }
        if (url_item("Open in Browser", "site-web", !state.browser_url.empty()))
          cmd = UiCommand::OpenInBrowser;
        if (state.has_path) {
          ImGui::Separator();
          if (state.page_bookmarked) {
            if (url_item("Remove Page Bookmark", "bookmark"))
              cmd = UiCommand::RemoveBookmark;
          } else if (url_item("Bookmark This Page", "bookmark-outline")) {
            cmd = UiCommand::BookmarkPage;
          }
        }
        ImGui::EndPopup();
      }
      {
        const float inset = 3.f * scale_;
        const ImRect go_bb(ImVec2(field_end.x - go_w + 1.f, field_pos.y + inset),
                           ImVec2(field_end.x - inset, field_end.y - inset));
        if (go_button("##url_go", go_bb, "Go to the address (Enter)"))
          enter = true;
      }
      if (enter) {
        cmd = UiCommand::NavigateUrl;
        result.arg = url_input_;
        std::string selected_address = result.arg;
        if (url_suggestions_open_ && url_suggestion_selected_ >= 0) {
          const auto candidates = xanatoo::urlbar_suggestions(
              url_suggestion_query_, state.urlbar_entries);
          if (url_suggestion_selected_ < static_cast<int>(candidates.size())) {
            const auto& choice = candidates[static_cast<size_t>(url_suggestion_selected_)];
            cmd = choice.source == xanatoo::UrlBarSource::Tab
                ? UiCommand::SelectTab : UiCommand::NavigateUrl;
            result.arg = cmd == UiCommand::SelectTab
                ? std::to_string(choice.tab_index) : choice.address;
            selected_address = choice.address;
          }
        }
        url_synced_from_ = selected_address;
        url_bar_editing_ = false;
        url_suggestions_open_ = false;
        url_suggestions_dismissed_ = true;
        url_suggestion_selected_ = -1;
        ImGui::ClearActiveID();
      }
      auto extra = [&](const char* id, const char* icon_name, const char* fallback,
                       const char* tip, UiCommand action, bool on) {
        ImGui::SameLine(0.f, 0.f);
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x,
                                       field_pos.y + 3.f * scale_));
        const int style_colors = push_toolbar_button_style(on);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.f * scale_, 3.f * scale_));
        const Icon* ic = icon(icon_name);
        bool pressed;
        if (ic) {
          pressed = ImGui::ImageButton(id, reinterpret_cast<ImTextureID>(ic->texture),
                                       ImVec2(20.f * scale_, 20.f * scale_));
        } else {
          ImGui::PushID(id);
          pressed = ImGui::Button(fallback, ImVec2(28.f * scale_, 26.f * scale_));
          ImGui::PopID();
        }
        draw_toolbar_hover_face(on);
        ImGui::PopStyleVar();
        pop_toolbar_button_style(style_colors);
        if (pressed) cmd = action;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", tip);
      };
      if (!state.alternate_url.empty())
        // Pressed while showing the native form; a click flips to the other.
        extra("url_native", "native", "W",
              state.native_view ? ("Back to the web page: " + state.alternate_url).c_str()
                                : ("Native view: " + state.alternate_url).c_str(),
              UiCommand::ToggleNative, state.native_view);
      if (!state.site_search.empty()) {
        std::string name = state.site_search;
        for (const UiSearchEngine& e : state.search_engines)
          if (e.id == state.site_search) name = e.name;
        // Regular button, not a toggle: never drawn pressed, never hides the bar.
        const std::string tip = "Search " + name;
        extra("url_search", "find", "?", tip.c_str(), UiCommand::SearchSite,
              /*on=*/false);
        if (cmd == UiCommand::SearchSite) result.arg = state.site_search;
      }
      if (!state.page_languages.empty()) {
        std::string tip = "This page in other languages:";
        for (const auto& l : state.page_languages)
          if (!l.current)
            tip += "\n  " + (ui_can_draw(l.label) ? l.label : l.fallback_label);
        UiCommand before = cmd;
        extra("url_language", "language", "L", tip.c_str(), UiCommand::SetLanguages,
              /*on=*/false);
        if (cmd == UiCommand::SetLanguages) {
          cmd = before;
          ImGui::OpenPopup("##url_languages");
        }
        if (ImGui::BeginPopup("##url_languages")) {
          // The preference first: the list below can be long.
          ImGui::TextDisabled("Preferred languages (sent to web sites)");
          languages_field(state, cmd, result.arg, "url_languages_field");
          ImGui::Separator();
          ImGui::TextDisabled("This page in");
          for (size_t i = 0; i < state.page_languages.size(); ++i) {
            const auto& l = state.page_languages[i];
            ImGui::PushID(static_cast<int>(i));
            const std::string& label =
                ui_can_draw(l.label) ? l.label : l.fallback_label;
            if (ImGui::MenuItem(label.c_str(), nullptr, l.current, !l.current)) {
              cmd = UiCommand::NavigateUrl;
              result.arg = l.url;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
              ImGui::SetTooltip("%s", l.url.c_str());
            ImGui::PopID();
          }
          if (cmd == UiCommand::SetLanguages) ImGui::CloseCurrentPopup();
          ImGui::EndPopup();
        }
      }
      if (!state.page_feeds.empty()) {
        // One feed opens at once; several are offered in a small menu.
        const bool one = state.page_feeds.size() == 1;
        const std::string tip = one
            ? "Open the page's feed: " + state.page_feeds[0].label + "\n" +
                  state.page_feeds[0].url
            : "The page has " + std::to_string(state.page_feeds.size()) +
                  " feeds";
        UiCommand before = cmd;
        extra("url_feed", "site-feed", "F", tip.c_str(), UiCommand::OpenPageFeed,
              /*on=*/false);
        if (cmd == UiCommand::OpenPageFeed) {
          if (one) {
            result.arg = state.page_feeds[0].url;
          } else {
            cmd = before;
            ImGui::OpenPopup("##url_feeds");
          }
        }
        if (ImGui::BeginPopup("##url_feeds")) {
          for (size_t i = 0; i < state.page_feeds.size(); ++i) {
            const auto& f = state.page_feeds[i];
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::MenuItem(f.label.c_str())) {
              cmd = UiCommand::OpenPageFeed;
              result.arg = f.url;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
              ImGui::SetTooltip("%s", f.url.c_str());
            ImGui::PopID();
          }
          ImGui::EndPopup();
        }
      }
      if (state.has_chrome)
        extra("url_reader", "reader", "R", state.reader ? "Show the whole page (F9)" : "Reader mode (F9)",
              UiCommand::ToggleReader, state.reader);
      if (state.has_headings)
        extra("url_outline", "outline", "H",
              state.outline ? "Close heading outline (Ctrl+Shift+O)"
                            : "Heading outline (Ctrl+Shift+O)",
              UiCommand::ToggleOutline, state.outline);
      if (state.has_nav)
        extra("url_nav", "navigation", "N", state.page_panel ? "Close page navigation" : "Open page navigation",
              UiCommand::TogglePage, state.page_panel);
      if (state.has_site)
        extra("url_sitemap", "sitemap", "S", state.sitemap ? "Close site map" : "Open site map",
              UiCommand::ToggleSitemap, state.sitemap);
    };
    // Search box right of the location, like the old Firefox search box.
    // The engine only changes which search: address a search builds
    // (docs/SEARCH.md).
    auto search_field = [&](float row_y) {
      const float button_h = 32.f * scale_ + 6.f;
      const float field_h = 32.f * scale_;
      const UiSearchEngine* cur = nullptr;
      for (const UiSearchEngine& e : state.search_engines)
        if (e.id == state.search_engine) cur = &e;
      if (!cur && !state.search_engines.empty()) cur = &state.search_engines[0];
      ImGui::SameLine(0.f, 0.f);
      ImGui::SetCursorPosX(ImGui::GetWindowWidth() -
                           ImGui::GetStyle().WindowPadding.x - search_w +
                           6.f * scale_);
      ImGui::SetCursorPosY(row_y + (button_h - field_h) * 0.5f);
      // One frame like the location field: engine tile, query, Go.
      const ImVec2 field_pos = ImGui::GetCursorScreenPos();
      const ImVec2 field_end(field_pos.x + search_w - 6.f * scale_,
                             field_pos.y + field_h);
      ImDrawList* draw = ImGui::GetWindowDrawList();
      draw->AddRectFilled(field_pos, field_end, IM_COL32(242, 242, 247, 255),
                          ImGui::GetStyle().FrameRounding);
      draw->AddRect(field_pos, field_end, ImGui::GetColorU32(ImGuiCol_Border),
                    ImGui::GetStyle().FrameRounding);
      {
        // The engine tile is the engine menu, like a favicon button. Added
        // before the field, so it wins the hover over the field's padding
        // without AllowOverlap (which costs a tap a frame of hover first).
        const float side = 20.f * scale_;
        const float pad = 3.f * scale_;
        const float x0 = field_pos.x + 5.f * scale_;
        const float y0 = field_pos.y + (field_h - side) * 0.5f;
        const ImRect bb(ImVec2(x0 - pad, y0 - pad),
                        ImVec2(x0 + side + pad, y0 + side + pad));
        const ImGuiID id = ImGui::GetID("##search_engine_tile");
        bool hot = false, held = false;
        if (ImGui::ItemAdd(bb, id) &&
            ImGui::ButtonBehavior(bb, id, &hot, &held))
          ImGui::OpenPopup("##search_engines");
        if (hot || held || ImGui::IsPopupOpen("##search_engines"))
          draw->AddRectFilled(bb.Min, bb.Max,
                              ImGui::GetColorU32(held ? ImGuiCol_ButtonActive
                                                      : ImGuiCol_ButtonHovered),
                              ImGui::GetStyle().FrameRounding);
        draw_engine_tile(draw, ImVec2(x0, y0), side, cur);
        if (!state.offered_engines.empty()) {
          // The page offers a search engine: a small green plus, as
          // browsers marked their search box.
          const float r = 5.f * scale_;
          const ImVec2 c(x0 + side - r * 0.4f, y0 + side - r * 0.4f);
          draw->AddCircleFilled(c, r, IM_COL32(60, 150, 60, 255));
          draw->AddCircle(c, r, IM_COL32(255, 255, 255, 255), 0, 1.f * scale_);
          const float a = r * 0.55f, t = std::max(1.f, 1.4f * scale_);
          draw->AddLine(ImVec2(c.x - a, c.y), ImVec2(c.x + a, c.y),
                        IM_COL32(255, 255, 255, 255), t);
          draw->AddLine(ImVec2(c.x, c.y - a), ImVec2(c.x, c.y + a),
                        IM_COL32(255, 255, 255, 255), t);
        }
        if (hot)
          ImGui::SetTooltip("Search engine: %s · click to change",
                            cur ? cur->name.c_str() : "none");
      }
      ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.f, 0.f, 0.f, 0.f));
      ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.f, 0.f, 0.f, 0.f));
      ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.f, 0.f, 0.f, 0.f));
      ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
      ImGui::PushStyleVar(
          ImGuiStyleVar_FramePadding,
          ImVec2(field_icon_w,
                 std::max(0.f, (field_h - ImGui::GetFontSize()) * 0.5f)));
      ImGui::SetNextItemWidth(field_icon_w + search_field_w);
      if (focus_search_ || search_select_all_) {
        ImGui::SetKeyboardFocusHere();
        focus_search_ = false;
      }
      const std::string hint =
          std::string("Search ") + (cur ? cur->name : "") + " …";
      bool enter = ImGui::InputTextWithHint(
          "##searchfield", hint.c_str(), search_input_, sizeof(search_input_),
          ImGuiInputTextFlags_EnterReturnsTrue);
      const ImGuiID search_id = ImGui::GetItemID();
      const int search_len = static_cast<int>(std::strlen(search_input_));
      if (search_select_all_) {
        // Focused above; the selection applies on the next frame.
        if (ImGuiInputTextState* st = ImGui::GetInputTextState(search_id)) {
          st->WantReloadUserBuf = true;
          st->ReloadSelectionStart = 0;
          st->ReloadSelectionEnd = search_len;
        }
        search_select_all_ = false;
      }
      // Right click (or a touch long press): Cut, Copy, Paste… The range is
      // taken now, before the menu takes the focus from the field.
      if (ImGui::IsItemHovered() &&
          ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        const ImGuiInputTextState* st = ImGui::GetInputTextState(search_id);
        int a = search_len, b = search_len;
        if (st && st->HasSelection()) {
          a = std::min(st->GetSelectionStart(), st->GetSelectionEnd());
          b = std::max(st->GetSelectionStart(), st->GetSelectionEnd());
        } else if (st) {
          a = b = st->GetCursorPos();
        }
        search_context_begin_ = std::clamp(a, 0, search_len);
        search_context_end_ = std::clamp(b, search_context_begin_, search_len);
        ImGui::OpenPopup("##search_context");
      }
      if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        cmd = UiCommand::ToggleSearchBar;
      ImGui::PopStyleVar(2);
      ImGui::PopStyleColor(3);
      if (ImGui::BeginPopup("##search_context")) {
        const std::string value = search_input_;
        const int from = std::min(search_context_begin_,
                                  static_cast<int>(value.size()));
        const int to = std::clamp(search_context_end_, from,
                                  static_cast<int>(value.size()));
        const bool selected = to > from;
        const char* clip = ImGui::GetClipboardText();
        const std::string clipboard = clip ? clip : "";
        auto set_query = [&](const std::string& text) {
          std::snprintf(search_input_, sizeof(search_input_), "%s",
                        text.c_str());
          if (ImGuiInputTextState* st = ImGui::GetInputTextState(search_id))
            st->ReloadUserBufAndMoveToEnd();
        };
        auto replaced = [&](const std::string& with) {
          return value.substr(0, from) + with + value.substr(to);
        };
        if (menu_item_with_icon("Cut", "cut", nullptr, false, selected)) {
          ImGui::SetClipboardText(value.substr(from, to - from).c_str());
          set_query(replaced(""));
        }
        if (menu_item_with_icon("Copy", "copy", nullptr, false, selected))
          ImGui::SetClipboardText(value.substr(from, to - from).c_str());
        if (menu_item_with_icon("Paste", "paste", nullptr, false,
                                !clipboard.empty()))
          set_query(replaced(clipboard));
        if (menu_item_with_icon("Select All", "select-all", nullptr, false,
                                !value.empty()))
          search_select_all_ = true;
        if (menu_item_with_icon("Clear", "stop", nullptr, false, !value.empty()))
          set_query("");
        ImGui::Separator();
        if (menu_item_with_icon("Paste and Search", "find", nullptr, false,
                                !clipboard.empty()) &&
            cmd == UiCommand::None) {
          set_query(clipboard);
          cmd = UiCommand::Search;
          result.arg = clipboard;
        }
        ImGui::EndPopup();
      }
      // The bar sits at the right window edge: the menu hangs leftwards.
      ImGui::SetNextWindowPos(ImVec2(field_end.x, field_end.y + 2.f * scale_),
                              ImGuiCond_Always, ImVec2(1.f, 0.f));
      if (ImGui::BeginPopup("##search_engines")) {
        // Engines the page offers first, above the long list.
        if (!state.offered_engines.empty()) {
          for (size_t i = 0; i < state.offered_engines.size(); ++i) {
            const UiOfferedEngine& o = state.offered_engines[i];
            ImGui::PushID(static_cast<int>(i));
            const std::string label = "Add \u201c" + o.title + "\u201d";
            if (ImGui::MenuItem(label.c_str(), nullptr, false, !state.adding_engine)) {
              cmd = UiCommand::AddSearchEngine;
              result.arg = o.url;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
              ImGui::SetTooltip("The page offers this search engine\n%s", o.url.c_str());
            ImGui::PopID();
          }
          ImGui::Separator();
        }
        // First, so a long list does not hide it below the fold.
        if (ImGui::BeginMenu("Choose engines")) {
          search_engine_checkboxes(state, cmd, result.arg);
          ImGui::EndMenu();
        }
        ImGui::Separator();
        const float side = ImGui::GetTextLineHeight() + 4.f * scale_;
        for (const UiSearchEngine& e : state.search_engines) {
          if (!e.on) continue;
          std::string label = e.name;
          if (!e.keyword.empty()) label += "   (" + e.keyword + ")";
          ImGui::PushID(e.id.c_str());
          const ImVec2 rp = ImGui::GetCursorScreenPos();
          const float text_x = side + 8.f * scale_;
          if (ImGui::Selectable(
                  "##engine", cur && e.id == cur->id, 0,
                  ImVec2(text_x + ImGui::CalcTextSize(label.c_str()).x, side))) {
            cmd = UiCommand::SelectSearchEngine;
            result.arg = e.id;
          }
          ImDrawList* pd = ImGui::GetWindowDrawList();
          draw_engine_tile(pd, rp, side, &e);
          pd->AddText(ImVec2(rp.x + text_x,
                             rp.y + (side - ImGui::GetTextLineHeight()) * 0.5f),
                      ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
          ImGui::PopID();
        }
        ImGui::TextDisabled("(keyword): Ctrl+L, e.g. \"w xanadu\"");
        ImGui::EndPopup();
      }
      const float inset = 3.f * scale_;
      const ImRect go_bb(ImVec2(field_end.x - go_w + 1.f, field_pos.y + inset),
                         ImVec2(field_end.x - inset, field_end.y - inset));
      if (go_button("##search_go", go_bb, "Search (Enter)")) enter = true;
      if (enter) {
        cmd = UiCommand::Search;
        result.arg = search_input_;
      }
    };

    {
      std::string bm_tip =
          state.current_bookmarked ? "Remove bookmark (Ctrl+Shift+B)"
          : state.selection        ? "Bookmark the selection (Ctrl+Shift+B)"
                                   : "Bookmark this page (Ctrl+Shift+B)";
      if (state.page_bookmarks > 0)
        bm_tip += state.page_bookmarks == 1
                      ? " · 1 bookmark in this page"
                      : " · " + std::to_string(state.page_bookmarks) +
                            " bookmarks in this page";
      // The empty/filled ribbon conveys state without a latched button face.
      tool_button("tb_bookmark",
                  state.current_bookmarked ? "bookmark" : "bookmark-outline",
                  bm_tip.c_str(),
                  state.current_bookmarked ? UiCommand::RemoveBookmark
                                           : UiCommand::AddBookmark,
                  cmd, state.has_path || !state.doc_label.empty());
    }
    // The same tools whatever the page: editing has its strip on the page.
    tool_button("tb_find", "find", "Find (Ctrl+F)", UiCommand::Find, cmd);
    tool_button("tb_split", state.split && !state.trail && !state.diff
                                  ? "join" : "split",
                  state.split && !state.trail && !state.diff
                      ? "Join panes (keep left, Ctrl+\\)"
                      : "Independent split panes (Ctrl+\\)",
                  UiCommand::ToggleSplit, cmd);
    toggle_button("tb_trail", "history", "Linked navigation trail",
                  UiCommand::ToggleTrail, state.trail);
    toggle_button("tb_outline", "outline",
                  "Heading outline (Ctrl+Shift+O; Ctrl+Alt+O docks it)",
                  UiCommand::ToggleOutline, state.outline);
    if (tool_menu_begin("tb_viewmenu", "view", "View tools")) {
      view_tools_items(state, cmd, result);
      tool_menu_end();
    }
    toggle_button("tb_history", "history", "Session history (Ctrl+Shift+H)",
                  UiCommand::ToggleHistory, state.history_open);
    toggle_button("tb_downloads", "downloads", "Downloads",
                  UiCommand::ToggleDownloads, state.downloads_open);
    // tool_button ends with SameLine, so the cursor sits after the group.
    {
      const float row_y = field_row_y;
      ImGui::SetCursorPos(ImVec2(6.f, row_y));
      location_field();
      if (state.searchbar) search_field(row_y);
    }
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
  result.top_reserved += browse_h;

  // ---- Document tab strip (browser-style) ----
  if (state.tab_bar_visible && !state.tab_labels.empty()) {
    const float tab_h = 32.f * scale_;
    // Tab-strip spacing is intentionally tight, but its context popup must
    // use the same comfortable padding as the other menus.
    const ImVec2 menu_window_padding = ImGui::GetStyle().WindowPadding;
    const ImVec2 menu_item_spacing = ImGui::GetStyle().ItemSpacing;
    const ImVec2 menu_frame_padding = ImGui::GetStyle().FramePadding;
    ImGui::SetNextWindowPos(
        ImVec2(vp->Pos.x, vp->Pos.y + result.top_reserved));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, tab_h));
    ImGuiWindowFlags tab_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.f * scale_, 3.f * scale_));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.f * scale_, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(9.f * scale_, 4.f * scale_));
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 4.f * scale_);
    ImGui::PushStyleVar(ImGuiStyleVar_TabBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_TabBarOverlineSize, 2.f * scale_);
    if (ImGui::Begin("##tabs", nullptr, tab_flags)) {
      // Dragging reorders; the new order goes back as ReorderTabs. Ids are
      // the buffer ids, so a tab keeps its place when the order changes.
      std::vector<std::pair<ImGuiID, uint32_t>> tab_item_ids;
      const float strip_top = vp->Pos.y + result.top_reserved;
      const float button_w = 26.f * scale_;
      const float tab_button_h = tab_h - 6.f * scale_;
      const int button_colors = push_toolbar_button_style();
      // New document: a blank local (database) document in a new tab, ready
      // to type into; it is listed under Local documents at once.
      bool new_doc = false;
      if (const Icon* ic = icon("new")) {
        const float pad = 2.f * scale_;
        const float side = std::max(1.f, tab_button_h - 2.f * pad);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2((button_w - side) * 0.5f, pad));
        new_doc = ImGui::ImageButton(
            "##new-doc", reinterpret_cast<ImTextureID>(ic->texture),
            ImVec2(side, side));
        ImGui::PopStyleVar();
      } else {
        new_doc = ImGui::Button("New##new-doc", ImVec2(button_w, tab_button_h));
      }
      if (new_doc) cmd = UiCommand::NewDocument;
      draw_toolbar_hover_face();
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("New document (Ctrl+N)");
      bool open_new_menu = ImGui::IsItemClicked(ImGuiMouseButton_Right);
      ImGui::PopStyleVar(2);
      ImGui::PopStyleColor(button_colors);
      ImGui::SameLine();
      ImGui::BeginChild("##tab-scroll-area", ImVec2(0.f, tab_button_h), false,
                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
      const float tab_offset = std::max(1.f, std::round(scale_));
      ImDrawList* strip_draw = ImGui::GetWindowDrawList();
      ImVec2 clip_top = strip_draw->GetClipRectMin();
      // Let the raised accent use the strip padding above the child.
      clip_top.y = std::max(strip_top, clip_top.y - tab_offset - 1.f);
      ImGui::PushClipRect(clip_top, strip_draw->GetClipRectMax(), false);
      if (ImGui::BeginTabBar("##doctabs",
                             ImGuiTabBarFlags_FittingPolicyScroll |
                                 ImGuiTabBarFlags_Reorderable |
                                 ImGuiTabBarFlags_DrawSelectedOverline)) {
        ImGuiTabBar* bar = ImGui::GetCurrentTabBar();
        bar->GroupSpacing = 8.f * scale_;
        bar->ScrollMinTabWidth = std::round(72.f * scale_);
        std::unordered_map<ImGuiID, size_t> tab_indices;
        for (size_t i = 0; i < state.tab_ids.size(); ++i) {
          const ImGuiID id =
              ImGui::GetID(("###tab" + std::to_string(state.tab_ids[i])).c_str());
          tab_indices.emplace(id, i);
        }
        size_t known_tabs = 0;
        for (ImGuiTabItem& tab : bar->Tabs) {
          if (const auto it = tab_indices.find(tab.ID); it != tab_indices.end()) {
            ++known_tabs;
            tab.GroupId = it->second < state.tab_roots.size()
                              ? state.tab_roots[it->second] : 0;
          }
        }
        // Newly submitted items enter ImGui's list after layout. Wait until
        // they are all registered before accepting a drag order from ImGui.
        const bool tabs_registered = known_tabs == state.tab_ids.size();
        if (tab_submitted_ids_ != state.tab_ids) {
          if (bar->Tabs.Size > 1) {
            const auto index = [&](ImGuiID id) {
              const auto it = tab_indices.find(id);
              return it == tab_indices.end() ? state.tab_ids.size() : it->second;
            };
            std::stable_sort(bar->Tabs.begin(), bar->Tabs.end(),
                             [&](const ImGuiTabItem& a, const ImGuiTabItem& b) {
              return index(a.ID) < index(b.ID);
            });
          }
          bar->ReorderRequestTabId = 0;
          if (tabs_registered) tab_submitted_ids_ = state.tab_ids;
        }
        const bool force_sel = tab_force_select_;
        tab_force_select_ = false;
        for (int i = 0; i < static_cast<int>(state.tab_labels.size()); ++i) {
          // One-shot SetSelected when main changed active_tab (Next/Prev tab).
          // Continuous SetSelected fights ImGui clicks → flicker / stacked docs.
          ImGuiTabItemFlags flags = 0;
          if (force_sel && i == state.active_tab)
            flags |= ImGuiTabItemFlags_SetSelected;
          bool open = true;
          SDL_Texture* icon = i < static_cast<int>(state.tab_icons.size())
                                  ? state.tab_icons[static_cast<size_t>(i)] : nullptr;
          std::string id = icon ? "    " : "";
          id += state.tab_labels[static_cast<size_t>(i)];
          const uint32_t bid = i < static_cast<int>(state.tab_ids.size())
                                   ? state.tab_ids[static_cast<size_t>(i)]
                                   : static_cast<uint32_t>(i);
          id += "###tab";
          id += std::to_string(bid);
          const bool active = i == state.active_tab;
          const theme::Palette& pal = theme::palette();
          const auto col = [](theme::Rgb c) {
            return ImVec4(c.r / 255.f, c.g / 255.f, c.b / 255.f, 1.f);
          };
          ImGui::PushStyleColor(ImGuiCol_Text,
                                col(active ? pal.text : pal.doc_tab_text));
          int tab_colors = 1;
          if (i < static_cast<int>(state.tab_editable.size()) &&
              state.tab_editable[static_cast<size_t>(i)]) {
            // An editable, unlocked document: a green tab (docs/UI.md).
            ImGui::PushStyleColor(ImGuiCol_Tab, col(pal.doc_tab_edit));
            ImGui::PushStyleColor(ImGuiCol_TabDimmed, col(pal.doc_tab_edit));
            ImGui::PushStyleColor(ImGuiCol_TabHovered, col(pal.doc_tab_edit_hot));
            ImGui::PushStyleColor(ImGuiCol_TabSelected,
                                  col(pal.doc_tab_edit_selected));
            ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected,
                                  col(pal.doc_tab_edit_selected));
            ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline,
                                  col(pal.doc_tab_edit_line));
            ImGui::PushStyleColor(ImGuiCol_TabDimmedSelectedOverline,
                                  col(pal.doc_tab_edit_line));
            tab_colors += 7;
          } else if (i < static_cast<int>(state.tab_service_color.size()) &&
                     state.tab_service_color[static_cast<size_t>(i)]) {
            // A native service's page: its accent (docs/UI.md "Service
            // strips").
            const uint32_t c = state.tab_service_color[static_cast<size_t>(i)];
            const ImVec4 line = tint_rgb(pal.face, c, 1.f);
            ImGui::PushStyleColor(ImGuiCol_Tab, tint_rgb(pal.face_dark, c, 0.18f));
            ImGui::PushStyleColor(ImGuiCol_TabDimmed, tint_rgb(pal.face_dark, c, 0.18f));
            ImGui::PushStyleColor(ImGuiCol_TabHovered, tint_rgb(pal.face_light, c, 0.22f));
            ImGui::PushStyleColor(ImGuiCol_TabSelected, tint_rgb(pal.face_light, c, 0.14f));
            ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected, tint_rgb(pal.face_light, c, 0.14f));
            ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, line);
            ImGui::PushStyleColor(ImGuiCol_TabDimmedSelectedOverline, line);
            tab_colors += 7;
          }
          ImGui::TabItemEx(bar, id.c_str(), &open, flags, nullptr,
                           active ? -tab_offset : tab_offset, tab_offset);
          // TabItemEx reports its label before ItemAdd: the hook misses it.
          note_hover_help("tab", nullptr);
          ImGui::PopStyleColor(tab_colors);
          if ((active || icon) && ImGui::IsItemVisible()) {
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->PushClipRect(ImVec2(bar->ScrollingRectMinX, min.y),
                               ImVec2(bar->ScrollingRectMaxX, max.y), true);
            if (active) {
              // Leave the accent overline visible above the raised top edge.
              const float inset = std::max(1.f, scale_);
              const ImGuiStyle& style = ImGui::GetStyle();
              const float top = min.y + 1.f +
                                (style.TabBarOverlineSize + inset) * 0.5f;
              const float bottom = max.y - style.TabBarBorderSize;
              const float left = min.x + 1.5f * inset;
              const float right = max.x - 1.5f * inset;
              const float radius = std::max(
                  0.f, std::min(style.TabRounding - 1.5f * inset,
                                (right - left) * 0.5f));
              const ImVec2 upper_left(left + radius, top + radius);
              const ImVec2 upper_right(right - radius, top + radius);
              const ImU32 light = ImGui::GetColorU32(col(pal.highlight));
              const ImU32 shade = ImGui::GetColorU32(col(pal.shadow));
              draw->PathLineTo(ImVec2(left, bottom));
              draw->PathArcToFast(upper_left, radius, 6, 9);
              draw->PathArcToFast(upper_right, radius, 9, 10);
              draw->PathStroke(light, 0, inset);
              draw->PathArcToFast(upper_right, radius, 10, 12);
              draw->PathLineTo(ImVec2(right, bottom));
              draw->PathStroke(shade, 0, inset);
            }
            if (icon) {
              const float side = 16.f * scale_;
              const ImVec2 size = icon_size(icon, side);
              const float x = min.x + 8.f * scale_ + (side - size.x) * 0.5f;
              const float y = min.y + (max.y - min.y - size.y) * 0.5f -
                              (active ? tab_offset : 0.f);
              draw->AddImage(reinterpret_cast<ImTextureID>(icon), ImVec2(x, y),
                             ImVec2(x + size.x, y + size.y));
            }
            draw->PopClipRect();
          }
          tab_item_ids.emplace_back(ImGui::GetItemID(), bid);
          // Dragged well above / below the strip: a tear-off on release.
          if (ImGui::IsItemActive() &&
              ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.f)) {
            const float my = ImGui::GetMousePos().y;
            if (my > strip_top + tab_h * 1.5f || my < strip_top - tab_h * 0.5f) {
              tab_tear_id_ = bid;
              result.tab_drag_out = true;
            } else if (tab_tear_id_ == bid) {
              tab_tear_id_ = 0;
            }
          }
          if (ImGui::IsItemClicked(ImGuiMouseButton_Left) &&
              i != state.active_tab && cmd == UiCommand::None) {
            cmd = UiCommand::SelectTab;
            result.arg = std::to_string(i);
          }
          if (!open && cmd == UiCommand::None) {
            cmd = UiCommand::CloseTab;
            result.arg = std::to_string(i);
          }
          // The popup is attached to the specific tab item, so a right-click
          // does not require selecting the tab first or moving focus.
          ImGui::PushID(static_cast<int>(bid));
          ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, menu_window_padding);
          ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, menu_item_spacing);
          ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, menu_frame_padding);
          if (ImGui::BeginPopupContextItem("##tab_context",
                                           ImGuiPopupFlags_MouseButtonRight)) {
            if (menu_item_with_icon("Select Tab", "tab-select", nullptr,
                                    i == state.active_tab,
                                    i != state.active_tab) &&
                cmd == UiCommand::None) {
              cmd = UiCommand::SelectTab;
              result.arg = std::to_string(i);
            }
            if (menu_item_with_icon("Move Tab to Window", "tab-window",
                                    nullptr, false) &&
                cmd == UiCommand::None) {
              cmd = UiCommand::TearTab;
              const int x = static_cast<int>(vp->Pos.x + vp->Size.x * 0.5f);
              const int y = static_cast<int>(vp->Pos.y + vp->Size.y * 0.45f);
              result.arg = std::to_string(bid) + "," + std::to_string(x) +
                           "," + std::to_string(y);
            }
            ImGui::Separator();
            if (menu_item_with_icon("Close Tab", "tab-close", nullptr, false) &&
                cmd == UiCommand::None) {
              cmd = UiCommand::CloseTab;
              result.arg = std::to_string(i);
            }
            const auto tab_action = [&](const char* label, const char* icon,
                                        UiCommand action, bool enabled) {
              if (menu_item_with_icon(label, icon, nullptr, false, enabled) &&
                  cmd == UiCommand::None) {
                cmd = action;
                result.arg = std::to_string(i);
              }
            };
            tab_action("Close other tabs", "tab-close-others",
                       UiCommand::CloseOtherTabs, state.tab_ids.size() > 1);
            tab_action("Close tabs to the right", "tab-close-right",
                       UiCommand::CloseTabsRight,
                       i + 1 < static_cast<int>(state.tab_ids.size()));
            tab_action("Close tabs to the left", "tab-close-left",
                       UiCommand::CloseTabsLeft, i > 0);
            ImGui::Separator();
            tab_action("Sort tabs by title", "tab-sort",
                       UiCommand::SortTabsByTitle, state.tab_ids.size() > 1);
            ImGui::EndPopup();
          }
          ImGui::PopStyleVar(3);
          ImGui::PopID();
        }
        if (ImGui::TabItemButton("+##new-tab", ImGuiTabItemFlags_Trailing) &&
            cmd == UiCommand::None)
          cmd = UiCommand::NewTab;
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("New tab (Ctrl+T) · right-click for more");
        open_new_menu |= ImGui::IsItemClicked(ImGuiMouseButton_Right);

        // The trailing button is registered with ImGui, but has no buffer.
        if (tabs_registered && cmd == UiCommand::None && !result.tab_drag_out &&
            tab_tear_id_ == 0 &&
            static_cast<int>(tab_item_ids.size()) + 1 == bar->Tabs.Size) {
          std::string order;
          bool known = true, changed = false;
          for (size_t k = 0; k < tab_item_ids.size(); ++k) {
            uint32_t bid = 0;
            known = false;
            for (const auto& [iid, b] : tab_item_ids)
              if (iid == bar->Tabs[static_cast<int>(k)].ID) {
                bid = b;
                known = true;
              }
            if (!known) break;
            if (k < state.tab_ids.size() && state.tab_ids[k] != bid)
              changed = true;
            order += (k ? "," : "") + std::to_string(bid);
          }
          if (known && changed) {
            cmd = UiCommand::ReorderTabs;
            result.arg = order;
          }
        }
        ImGui::EndTabBar();
        // After main applies SelectTab/NextTab, active_tab changes → sync ImGui.
        const uint32_t active_id =
            state.tab_ids[static_cast<size_t>(state.active_tab)];
        if (active_id != tab_synced_active_) {
          tab_synced_active_ = active_id;
          tab_force_select_ = true;
        }
      }
      ImGui::PopClipRect();
      ImGui::EndChild();
      if (tab_tear_id_ != 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const ImVec2 mp = ImGui::GetMousePos();
        cmd = UiCommand::TearTab;
        result.arg = std::to_string(tab_tear_id_) + "," +
                     std::to_string(static_cast<int>(mp.x)) + "," +
                     std::to_string(static_cast<int>(mp.y));
        tab_tear_id_ = 0;
      } else if (tab_tear_id_ != 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        tab_tear_id_ = 0;
      }
      if (open_new_menu) ImGui::OpenPopup("##new-tab-menu");
      if (ImGui::BeginPopup("##new-tab-menu")) {
        menu_item_cmd("New Tab", UiCommand::NewTab, cmd, true, "Ctrl+T");
        menu_item_cmd("New Document", UiCommand::NewDocument, cmd, true,
                      "Ctrl+N");
        ImGui::Separator();
        menu_item_cmd("Open File…", UiCommand::OpenDocument, cmd, true,
                      "Ctrl+O");
        menu_item_cmd("Local Documents", UiCommand::OpenStoredDocuments, cmd);
        ImGui::EndPopup();
      }
    }
    ImGui::End();
    ImGui::PopStyleVar(6);
    result.top_reserved += tab_h;
  }
}

void Ui::draw_url_suggestions(const UiState& state, UiFrameResult& result,
                              UiCommand& cmd) {
  if (state.chrome_hidden || !url_suggestions_open_ || url_suggestion_w_ <= 0.f)
    return;
  if (ImGui::IsKeyPressed(ImGuiKey_Tab)) {
    url_suggestions_open_ = false;
    url_suggestions_dismissed_ = true;
    return;
  }
  const auto matches = xanatoo::urlbar_suggestions(
      url_suggestion_query_, state.urlbar_entries);
  if (matches.empty()) return;

  ImGuiViewport* vp = ImGui::GetMainViewport();
  const float row_h = std::max(36.f * scale_, ImGui::GetTextLineHeight() * 2.f + 8.f);
  const float width = std::min(std::max(190.f * scale_, url_suggestion_w_),
                               std::max(1.f, vp->Size.x - 12.f));
  const float x = std::clamp(url_suggestion_x_, vp->Pos.x + 6.f,
                              vp->Pos.x + vp->Size.x - width - 6.f);
  const float available_h = std::max(0.f,
      vp->Pos.y + vp->Size.y - url_suggestion_y_ - 8.f -
          (state.virtual_keyboard
               ? (vp->Size.y < 500.f ? 200.f : 220.f) * scale_ : 0.f));
  const float height = std::min(row_h * static_cast<float>(matches.size()) + 8.f,
                                 available_h);
  if (height < row_h) return;

  const ImVec2 a(x, url_suggestion_y_);
  const ImVec2 b(x + width, url_suggestion_y_ + height);
  // Keep the field's keyboard focus. A click on a result may focus the popup
  // for that frame; it closes immediately after issuing the navigation.
  if (!url_field_active_ && !ImGui::IsMouseHoveringRect(a, b, false)) {
    url_suggestions_open_ = false;
    url_suggestions_dismissed_ = true;
    return;
  }

  ImGui::SetNextWindowPos(a);
  ImGui::SetNextWindowSize(ImVec2(width, height));
  const ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
      ImGuiWindowFlags_NoNavFocus;
  if (ImGui::Begin("##url_suggestions", nullptr, flags)) {
    // A plain window: clicking the tab strip or toolbar raises *their*
    // window above it. The dropdown must stay over all chrome (draw and
    // hit-test order alike).
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    const auto& pal = theme::palette();
    auto col = [](theme::Rgb c) { return IM_COL32(c.r, c.g, c.b, 255); };
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, 0.f));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(.80f, .83f, .82f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(.94f, .96f, .94f, 1.f));
    for (int i = 0; i < static_cast<int>(matches.size()); ++i) {
      const UrlBarSuggestion& entry = matches[static_cast<size_t>(i)];
      ImGui::PushID(i);
      const bool activated = ImGui::Selectable(
          "##suggestion", url_suggestion_selected_ == i, 0,
          ImVec2(0.f, row_h));
      if (ImGui::IsItemHovered()) url_suggestion_selected_ = i;
      if (entry.source != UrlBarSource::Tab &&
          middle_open(cmd, result.arg, entry.address)) {
        url_synced_from_ = entry.address;
        url_bar_editing_ = false;
        url_suggestions_open_ = false;
        url_suggestions_dismissed_ = true;
        url_suggestion_selected_ = -1;
        ImGui::ClearActiveID();
      }
      const ImVec2 min = ImGui::GetItemRectMin();
      const ImVec2 max = ImGui::GetItemRectMax();
      const char* kind = entry.source == UrlBarSource::Tab ? "Tab" :
                         entry.source == UrlBarSource::Bookmark ? "Bookmark" :
                         "History";
      const float kind_w = ImGui::CalcTextSize(kind).x;
      ImDrawList* draw = ImGui::GetWindowDrawList();
      draw->PushClipRect(ImVec2(min.x + 8.f, min.y),
                         ImVec2(max.x - kind_w - 16.f, max.y), true);
      const std::string primary = entry.title.empty() ? entry.address : entry.title;
      draw->AddText(ImVec2(min.x + 8.f, min.y + 2.f), col(pal.text),
                    primary.c_str());
      draw->AddText(ImVec2(min.x + 8.f, min.y + row_h * .50f),
                    col(pal.text_dim), entry.address.c_str());
      draw->PopClipRect();
      draw->AddText(ImVec2(max.x - kind_w - 8.f, min.y + 2.f),
                    col(pal.text_dim), kind);
      if (activated) {
        cmd = entry.source == UrlBarSource::Tab
                  ? UiCommand::SelectTab : UiCommand::NavigateUrl;
        result.arg = entry.source == UrlBarSource::Tab
                  ? std::to_string(entry.tab_index) : entry.address;
        url_synced_from_ = entry.address;
        url_bar_editing_ = false;
        url_suggestions_open_ = false;
        url_suggestions_dismissed_ = true;
        url_suggestion_selected_ = -1;
        ImGui::ClearActiveID();
      }
      ImGui::PopID();
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
  }
  ImGui::End();
}

UiFrameResult Ui::begin_frame(const UiState& state) {
  UiFrameResult result;
  if (!ready_) return result;
  render_prepared_ = false;
  url_field_active_ = false;
  if (state.chrome_hidden) url_suggestions_open_ = false;

#if defined(XANATOO_SDL3) && XANATOO_SDL3
  xanatoo::imgui_sdl3::new_frame();
#else
  ImGui_ImplSDLRenderer2_NewFrame();
  ImGui_ImplSDL2_NewFrame();
#endif
  // The SDL backend may have queued the physical mouse over a form obscured
  // by a native-only (or different) FloatWindow. Keep hidden widgets from
  // receiving hover, wheel and activation while the native frame is above.
  // Button/wheel events at such positions are also filtered at event entry.
  const bool pointer_hidden = pointer_obscured(
      state.pointer_x, state.pointer_y, state.pointer_native_top);
  if (pointer_hidden || state.pointer_away)
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
  ImGui::GetCurrentContext()->TestEngineHookItems = hover_tracking_;
  g_hover_capture = UiHoverItem{};
  ImGui::NewFrame();
  if (touch_cancel_press_) {
    // A held button or tab then sees the release without being active:
    // no click. A text field gives up the keys (and the soft keyboard).
    ImGui::ClearActiveID();
    if (!ImGui::GetIO().MouseDown[ImGuiMouseButton_Left])
      touch_cancel_press_ = false;
  }
  if (std::exchange(close_popups_, false) &&
      !ImGui::GetCurrentContext()->OpenPopupStack.empty())
    ImGui::ClosePopupToLevel(0, true);
  // Stick is an *extra* pointer. Only drive ImGui from it while the stick is
  // actively used (axis motion or A/B held); otherwise leave the real mouse.
  if (state.stick_cursor && state.stick_active) {
    ImGuiIO& io = ImGui::GetIO();
    io.MousePos = pointer_hidden ? ImVec2(-FLT_MAX, -FLT_MAX)
                                 : ImVec2(state.stick_cx, state.stick_cy);
    io.MouseDown[0] = !pointer_hidden && state.stick_lmb;
    io.MouseDown[1] = !pointer_hidden && state.stick_rmb;
  }

  UiCommand cmd = UiCommand::None;

  // ---- Main menu bar ----
  if (!state.chrome_hidden && ImGui::BeginMainMenuBar()) {
    // Alt+letter: OpenPopup must run inside the menu-bar window so IDs match
    // BeginMenu()'s GetID(label). Consume the request this frame.
    auto force_menu = [&](const char* name) {
      if (menu_request_ == name) {
        ImGui::OpenPopup(name);
        menu_request_.clear();
      }
    };
    // Fitts's law: the menu titles reach the screen edge. ImGui clips the
    // bar half a border below its top (rounded to a whole pixel) and a title
    // only covers its label plus half the item spacing, which leaves dead
    // rows above (and below) the titles — the pointer pushed against the top
    // edge, mouse or gamepad stick, hit nothing. Clip to the full bar and
    // grow each title's spacing to the bar height; popped again before the
    // menu's own items, so the popups keep the normal spacing. The bar also
    // starts max(WindowPadding.x, ItemSpacing.x) in, which left the corner
    // dead: start "File" so its box (label minus ItemSpacing.x / 2) begins at
    // the left edge.
    const ImVec2 bar_min = ImGui::GetWindowPos();
    const ImVec2 bar_max(bar_min.x + ImGui::GetWindowWidth(),
                         bar_min.y + ImGui::GetFrameHeight());
    ImGui::PushClipRect(bar_min, bar_max, false);
    {
      const float sx = ImGui::GetStyle().ItemSpacing.x;
      const ImVec2 cur = ImGui::GetCursorScreenPos();
      const float edge_x = bar_min.x + sx - std::floor(sx * 0.5f);
      if (cur.x > edge_x) ImGui::SetCursorScreenPos(ImVec2(edge_x, cur.y));
    }
    const float title_gap = ImGui::GetStyle().FramePadding.y -
                            std::floor(ImGui::GetStyle().ItemSpacing.y * 0.5f);
    auto bar_menu = [&](const char* label, bool enabled = true) {
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                          ImVec2(ImGui::GetStyle().ItemSpacing.x,
                                 2.f * ImGui::GetStyle().FramePadding.y));
      const bool open = ImGui::BeginMenu(label, enabled);
      ImGui::PopStyleVar();
      return open;
    };

    force_menu("File");
    if (bar_menu("File")) {
      menu_item_cmd("New Document", UiCommand::NewDocument, cmd, true, "Ctrl+N");
      menu_item_cmd("Open…", UiCommand::OpenDocument, cmd, true, "Ctrl+O");
      menu_item_cmd("Open URL…", UiCommand::OpenUrlBar, cmd, true, "Ctrl+L");
      menu_item_cmd("New Tab", UiCommand::NewTab, cmd, true, "Ctrl+T");
      menu_item_cmd("Close Tab", UiCommand::CloseTab, cmd, true, "Ctrl+W");
      ImGui::Separator();
      menu_item_cmd("Open in Window…", UiCommand::OpenInWindow, cmd);
      menu_item_cmd("Pop Out to Window", UiCommand::PopOut, cmd, true,
                    "Ctrl+Shift+P");
      menu_item_cmd("Move Window to Tab", UiCommand::PopIn, cmd, state.has_floating,
                    "Ctrl+Shift+P");
      ImGui::Separator();
      if (!state.autosaved_document)
        menu_item_cmd("Save", UiCommand::Save, cmd, !state.doc_label.empty(), "Ctrl+S");
      menu_item_cmd("Edit local copy", UiCommand::EditLocalCopy, cmd,
                    state.can_make_local_copy);
      menu_item_cmd("View original", UiCommand::ViewOriginal, cmd,
                    state.has_original);
      menu_item_cmd("Export a copy…", UiCommand::SaveAs, cmd, !state.doc_label.empty(), "Ctrl+Shift+S");
      menu_item_cmd("Export as XML…", UiCommand::ExportXml, cmd, !state.doc_label.empty());
      menu_item_cmd("Export as Markdown…", UiCommand::ExportMarkdown, cmd, !state.doc_label.empty());
      ImGui::Separator();
      menu_item_cmd("Reload", UiCommand::Reload, cmd, state.has_path, "Ctrl+R / F5");
      menu_item_cmd("Force Reload", UiCommand::ForceReload, cmd, state.has_path,
                    "Ctrl+Shift+R / Shift+F5");
      if (!state.autosaved_document)
        menu_item_cmd("Revert to Saved", UiCommand::RevertToSaved, cmd,
                      state.has_path && state.dirty);
      menu_item_cmd("Drafts", UiCommand::OpenDrafts, cmd);
      menu_item_cmd("Local documents", UiCommand::OpenStoredDocuments, cmd);
      menu_item_cmd("Document revisions…", UiCommand::ToggleRevisionsPanel,
                    cmd, state.can_show_revisions);
      ImGui::Separator();
      menu_item_cmd("Quit", UiCommand::Quit, cmd, true, "Ctrl+Q");
      ImGui::EndMenu();
    }
    force_menu("Edit");
    if (bar_menu("Edit")) {
      menu_item_cmd("Undo", UiCommand::Undo, cmd, true, "Ctrl+Z");
      menu_item_cmd("Redo", UiCommand::Redo, cmd, true, "Ctrl+Y");
      ImGui::Separator();
      menu_item_cmd("Cut", UiCommand::Cut, cmd, state.selection, "Ctrl+X");
      menu_item_cmd("Copy", UiCommand::Copy, cmd, state.selection, "Ctrl+C");
      menu_item_cmd("Copy Page URL", UiCommand::CopyPageUrl, cmd, true,
                    "Ctrl+Shift+U");
      menu_item_cmd("Copy Web Page URL", UiCommand::CopyPageWebUrl, cmd,
                    !state.web_page_url.empty());
      menu_item_cmd("Paste", UiCommand::Paste, cmd, true, "Ctrl+V");
      menu_item_cmd("Select Block", UiCommand::SelectBlock, cmd, true);
      menu_item_cmd("Select All", UiCommand::SelectAll, cmd, true, "Ctrl+Shift+A");
      menu_item_cmd("Duplicate Block", UiCommand::DuplicateBlock, cmd, true,
                    "Ctrl+D");
      menu_item_cmd("Move Block Up", UiCommand::MoveBlockUp, cmd, true,
                    "Alt+Up");
      menu_item_cmd("Move Block Down", UiCommand::MoveBlockDown, cmd, true,
                    "Alt+Down");
      ImGui::Separator();
      menu_item_cmd("Find…", UiCommand::Find, cmd, true, "Ctrl+F");
      menu_item_cmd("Replace…", UiCommand::Replace, cmd, true, "Ctrl+H");
      menu_item_cmd("Replace All", UiCommand::ReplaceAll, cmd, true,
                    "Ctrl+Shift+Enter");
      ImGui::Separator();
      menu_item_cmd("Preferences…", UiCommand::TogglePreferences, cmd);
      menu_item_cmd("Credentials…", UiCommand::ToggleCredentials, cmd);
      ImGui::EndMenu();
    }
    // Format / Insert only edit: not shown while browsing.
    if (!state.browse) force_menu("Format");
    if (!state.browse && bar_menu("Format")) {
      menu_item_cmd("Bold", UiCommand::Bold, cmd, state.selection, "Ctrl+B");
      menu_item_cmd("Italic", UiCommand::Italic, cmd, state.selection, "Ctrl+I");
      menu_item_cmd("Strikethrough", UiCommand::Strike, cmd, state.selection,
                    "Ctrl+Shift+X");
      menu_item_cmd("Code", UiCommand::Code, cmd, state.selection, "Ctrl+`");
      menu_item_cmd("Link…", UiCommand::Link, cmd, state.selection, "Ctrl+Shift+K");
      menu_item_cmd("Citation…", UiCommand::Cite, cmd, state.selection,
                    "Ctrl+Shift+C");
      ImGui::Separator();
      if (ImGui::BeginMenu("Structure")) {
        menu_structure_items(state, cmd);
        ImGui::EndMenu();
      }
      ImGui::EndMenu();
    }
    if (!state.browse) force_menu("Insert");
    if (!state.browse && bar_menu("Insert")) {
      menu_insert_items(cmd);
      ImGui::EndMenu();
    }
    force_menu("View");
    if (bar_menu("View")) {
      if (ImGui::MenuItem("Tab Bar", nullptr, state.tab_bar_visible))
        cmd = UiCommand::ToggleTabBar;
      menu_item_cmd(state.native_view ? "Web Page View" : "Native View",
                    UiCommand::ToggleNative, cmd, !state.alternate_url.empty());
      menu_item_cmd(state.archive_view ? "Live Page" : "Old Version (archive.org)",
                    UiCommand::ToggleArchive, cmd, !state.archive_url.empty());
      if (ImGui::MenuItem("Reader Mode", "F9", state.reader,
                          state.has_chrome || state.reader))
        cmd = UiCommand::ToggleReader;
      if (ImGui::BeginMenu("Summaries", state.has_summaries)) {
        if (ImGui::MenuItem("All Full Texts")) {
          cmd = UiCommand::StretchFold;
          result.arg = "full";
        }
        if (ImGui::MenuItem("All Summaries")) {
          cmd = UiCommand::StretchFold;
          result.arg = "summaries";
        }
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("View Source", !state.doc_label.empty())) {
        if (ImGui::MenuItem("Rendered Document", nullptr,
                            state.source_mode.empty()))
          cmd = UiCommand::ViewSourceRendered;
        ImGui::Separator();
        if (ImGui::MenuItem("Raw Source", nullptr,
                            state.source_mode == "source-raw"))
          cmd = UiCommand::ViewSourceRaw;
        if (ImGui::MenuItem("XML", nullptr,
                            state.source_mode == "source-xml"))
          cmd = UiCommand::ViewSourceXml;
        ImGui::EndMenu();
      }
      if (ImGui::MenuItem("Image Gallery", "Ctrl+Alt+I", state.gallery,
                          state.has_path || state.gallery))
        cmd = UiCommand::ToggleGallery;
      ImGui::Separator();
      if (ImGui::MenuItem("Focus current section", "Ctrl+Alt+F", state.section_focus))
        cmd = UiCommand::ToggleSectionFocus;
      if (ImGui::MenuItem("Structure Highlight", "Ctrl+Shift+L", state.structure))
        cmd = UiCommand::ToggleStructure;
      if (ImGui::MenuItem("Lock Document", "F2",
                          state.can_edit && state.browse, state.can_edit))
        cmd = UiCommand::ToggleLock;
      menu_item_cmd("Fullscreen", UiCommand::ToggleChrome, cmd, true, "F11");
      ImGui::Separator();
      menu_item_cmd("Zoom In", UiCommand::ZoomIn, cmd, true, "Ctrl++");
      menu_item_cmd("Zoom Out", UiCommand::ZoomOut, cmd, true, "Ctrl+-");
      if (ImGui::MenuItem("Footnote overlay", nullptr, state.footnote_overlay))
        cmd = UiCommand::ToggleFootnoteOverlay;
      if (ImGui::MenuItem("Bookmarks in the page", nullptr,
                          state.bookmark_marks))
        cmd = UiCommand::ToggleBookmarkMarks;
      if (ImGui::MenuItem("Headings on the scrollbar", nullptr,
                          state.heading_ticks))
        cmd = UiCommand::ToggleHeadingTicks;
      if (ImGui::MenuItem("Make web addresses clickable", nullptr,
                          state.auto_links))
        cmd = UiCommand::ToggleAutoLinks;
      if (ImGui::MenuItem("Show images of image addresses", nullptr,
                          state.auto_images))
        cmd = UiCommand::ToggleAutoImages;
      ImGui::EndMenu();
    }
    force_menu("Layout");
    if (bar_menu("Layout")) {
      if (ImGui::MenuItem("Paged Reading", "F8", state.paged))
        cmd = UiCommand::TogglePaged;
      if (ImGui::BeginMenu("Page columns", state.paged)) {
        static const struct { const char* label; const char* arg; int n; } cols[] = {
            {"Fit the width", "0", 0}, {"1 column", "1", 1},
            {"2 columns", "2", 2}, {"3 columns", "3", 3}};
        for (const auto& c : cols)
          if (ImGui::MenuItem(c.label, nullptr, state.page_columns == c.n)) {
            cmd = UiCommand::PageColumns;
            result.arg = c.arg;
          }
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("Collection", state.collection)) {
        menu_collection_items(state, cmd, result.arg);
        ImGui::EndMenu();
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Independent tiles", "Ctrl+\\",
                          state.split && !state.trail && !state.diff))
        cmd = UiCommand::ToggleSplit;
      if (ImGui::MenuItem("Trail (linked navigation)", nullptr, state.trail))
        cmd = UiCommand::ToggleTrail;
      if (ImGui::MenuItem("Side by side", nullptr, !state.tiles_stacked))
        cmd = UiCommand::TilesColumns;
      if (ImGui::MenuItem("Stacked", nullptr, state.tiles_stacked))
        cmd = UiCommand::TilesRows;
      if (ImGui::MenuItem("Compare tiles", "Ctrl+Shift+D", state.diff,
                          state.can_diff))
        cmd = UiCommand::ToggleDiff;
      ImGui::Separator();
      if (ImGui::BeginMenu("Line spacing")) {
        fill_line_spacing_items(state, cmd);
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("Margins")) {
        fill_margins_items(state, cmd, result.arg);
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("Text width")) {
        fill_measure_items(state, cmd, result.arg);
        ImGui::EndMenu();
      }
      if (ImGui::MenuItem("Justify lines", nullptr, state.justify))
        cmd = UiCommand::ToggleJustify;
      fill_style_menus(state, cmd, result);
      ImGui::EndMenu();
    }
    force_menu("Panels");
    if (bar_menu("Panels")) {
      if (ImGui::MenuItem("Vertical Tabs", nullptr, state.vertical_tabs_open))
        cmd = UiCommand::ToggleVerticalTabs;
      if (menu_item_with_icon("Keyboard Reference", "keyboard", "F1", state.help))
        cmd = UiCommand::ToggleHelp;
      if (menu_item_with_icon("Help Panel", "help", "Shift+F1", state.hover_help))
        cmd = UiCommand::ToggleHoverHelp;
      ImGui::Separator();
      if (menu_item_with_icon("Heading Outline", "outline", "Ctrl+Shift+O", state.outline))
        cmd = UiCommand::ToggleOutline;
      if (menu_item_with_icon("Backlinks", "backlinks", "Ctrl+Alt+B", state.backlinks))
        cmd = UiCommand::ToggleBacklinks;
      if (menu_item_with_icon("Site Map", "sitemap", "Ctrl+Alt+M", state.sitemap))
        cmd = UiCommand::ToggleSitemap;
      if (menu_item_with_icon("Related Sites", "related", nullptr, state.related_open))
        cmd = UiCommand::ToggleRelatedSites;
      if (menu_item_with_icon("Page Navigation", "navigation", "Ctrl+Alt+P", state.page_panel))
        cmd = UiCommand::TogglePage;
      if (menu_item_with_icon("History", "history", "Ctrl+Shift+H", state.history_open))
        cmd = UiCommand::ToggleHistory;
      if (menu_item_with_icon("Downloads", "downloads", nullptr, state.downloads_open))
        cmd = UiCommand::ToggleDownloads;
      if (menu_item_with_icon("Media", "media", nullptr, state.media_open))
        cmd = UiCommand::ToggleMedia;
      if (menu_item_with_icon("Subscriptions", "subscriptions", nullptr, state.subscriptions_open))
        cmd = UiCommand::ToggleSubscriptions;
#if XANATOO_HTTP_COOKIES
      if (ImGui::MenuItem("Cookies", nullptr, state.cookies_open))
        cmd = UiCommand::ToggleCookies;
#endif
      if (menu_item_with_icon("Certificates", "certificates", nullptr, state.certs_open))
        cmd = UiCommand::ToggleCertificates;
      if (menu_item_with_icon("Identities", "identities", nullptr, state.identities_open))
        cmd = UiCommand::ToggleIdentities;
      if (menu_item_with_icon("Style Sheets", "style", nullptr, state.styles_open))
        cmd = UiCommand::ToggleStylesPanel;
      if (menu_item_with_icon("Manage Style Sheets", "style-manager", nullptr,
                          state.styles_manager_open))
        cmd = UiCommand::ToggleStyleManagerPanel;
      if (menu_item_with_icon("Annotations", "annotations", nullptr, state.annotations_panel_open))
        cmd = UiCommand::ToggleAnnotationsPanel;
      if (menu_item_with_icon("Context", "context", nullptr, state.context_open))
        cmd = UiCommand::ToggleContextPanel;
      if (menu_item_with_icon("Document revisions", "revisions", nullptr,
                          state.revisions_panel_open,
                          state.can_show_revisions || state.revisions_panel_open))
        cmd = UiCommand::ToggleRevisionsPanel;
      ImGui::Separator();
      if (menu_item_with_icon("Nostr Explorer", "site-nostr", nullptr, state.nostr_explorer_open))
        cmd = UiCommand::ToggleNostrExplorer;
      if (menu_item_with_icon("Lemmy Explorer", "lemmy", nullptr, state.lemmy_explorer_open,
                          state.lemmy_supported || state.lemmy_explorer_open))
        cmd = UiCommand::ToggleLemmyExplorer;
      if (menu_item_with_icon("Matrix Explorer", "matrix", nullptr, state.matrix_explorer_open,
                          state.matrix_supported || state.matrix_explorer_open))
        cmd = UiCommand::ToggleMatrixExplorer;
      if (menu_item_with_icon("Nostr Accounts", "nostr-accounts", nullptr,
                          state.identities_open &&
                          state.identity_section == IdentitySection::Nostr))
        cmd = UiCommand::ToggleNostrAccounts;
      ImGui::EndMenu();
    }
    // Go: moving through and between pages, plus the panels that do so.
    force_menu("Go");
    if (bar_menu("Go")) {
      menu_item_cmd("Back", UiCommand::NavBack, cmd, state.can_nav_back, "Alt+Left");
      menu_item_cmd("Forward", UiCommand::NavForward, cmd, state.can_nav_forward,
                    "Alt+Right");
      menu_item_cmd("Up", UiCommand::GoUp, cmd, !state.up_url.empty(), "Alt+Up");
      menu_item_cmd("Home", UiCommand::GoHome, cmd, true, "Alt+Home");
      ImGui::Separator();
      menu_item_cmd("Previous Page in Site", UiCommand::SitemapPrev, cmd, true,
                    "Alt+PgUp");
      menu_item_cmd("Next Page in Site", UiCommand::SitemapNext, cmd, true,
                    "Alt+PgDn");
      ImGui::Separator();
      if (menu_item_with_icon("Heading Outline", "outline", "Ctrl+Shift+O", state.outline))
        cmd = UiCommand::ToggleOutline;
      ImGui::TextDisabled("  Ctrl+Alt+O docks outline | body");
      if (menu_item_with_icon("Backlinks", "backlinks", "Ctrl+Alt+B", state.backlinks))
        cmd = UiCommand::ToggleBacklinks;
      if (menu_item_with_icon("Site Map", "sitemap", "Ctrl+Alt+M", state.sitemap))
        cmd = UiCommand::ToggleSitemap;
      if (menu_item_with_icon("Related Sites", "related", nullptr, state.related_open))
        cmd = UiCommand::ToggleRelatedSites;
      if (menu_item_with_icon("Page Navigation", "navigation", "Ctrl+Alt+P", state.page_panel))
        cmd = UiCommand::TogglePage;
      if (menu_item_with_icon("Search Bar", "find", "Ctrl+K", state.searchbar))
        cmd = state.searchbar ? UiCommand::ToggleSearchBar
                              : UiCommand::FocusSearchBar;
      if (menu_item_with_icon("History", "history", "Ctrl+Shift+H", state.history_open))
        cmd = UiCommand::ToggleHistory;
      ImGui::Separator();
      // The status line's sound player (a link to an audio file starts it).
      if (ImGui::BeginMenu("Sound", state.audio)) {
        if (ImGui::MenuItem(state.audio_playing ? "Pause" : "Play"))
          cmd = UiCommand::AudioToggle;
        if (ImGui::MenuItem("Back 10 s")) {
          cmd = UiCommand::AudioSkip;
          result.arg = "-10";
        }
        if (ImGui::MenuItem("Forward 30 s")) {
          cmd = UiCommand::AudioSkip;
          result.arg = "30";
        }
        ImGui::Separator();
        if (ImGui::MenuItem("A-B Markers", nullptr, state.audio_ab)) cmd = UiCommand::AudioAB;
        if (ImGui::MenuItem(state.audio_ab ? "Loop Section" : "Loop", nullptr, state.audio_loop))
          cmd = UiCommand::AudioLoop;
        if (ImGui::MenuItem("Fullscreen Video", nullptr, state.audio_fullscreen,
                            state.audio_video))
          cmd = UiCommand::AudioFullscreen;
        if (ImGui::MenuItem("Hide Player Controls", "H", state.audio_controls_hidden,
                            state.audio_video))
          cmd = UiCommand::AudioControls;
        ImGui::Separator();
        if (ImGui::MenuItem("Stop")) cmd = UiCommand::AudioStop;
        ImGui::EndMenu();
      }
      ImGui::EndMenu();
    }
    force_menu("Bookmarks");
    if (bar_menu("Bookmarks")) {
      const char* add_label = state.current_bookmarked ? "Remove Bookmark"
                              : state.selection        ? "Bookmark Selection"
                                                       : "Bookmark This Page";
      if (ImGui::MenuItem(add_label, "Ctrl+Shift+B", false, state.has_path)) {
        cmd = state.current_bookmarked ? UiCommand::RemoveBookmark
                                       : UiCommand::AddBookmark;
      }
      if (ImGui::MenuItem("Bookmark Reading Position", nullptr, false,
                          state.has_path))
        cmd = UiCommand::BookmarkPosition;
      if (ImGui::MenuItem("Show Bookmarks…", nullptr, state.bookmarks_open))
        cmd = UiCommand::ToggleBookmarks;
      if (!state.bookmarks.empty() || !state.bookmark_folders.empty()) {
        ImGui::Separator();
        bookmark_menu_level(state, {}, cmd, result.arg);
      }
      ImGui::EndMenu();
    }
    force_menu("History");
    if (bar_menu("History")) {
      menu_item_cmd("Back", UiCommand::NavBack, cmd, state.can_nav_back, "Alt+Left");
      menu_item_cmd("Forward", UiCommand::NavForward, cmd, state.can_nav_forward,
                    "Alt+Right");
      if (ImGui::MenuItem("Show History…", "Ctrl+Shift+H", state.history_open))
        cmd = UiCommand::ToggleHistory;
      // The page as shown, for good (snapshot.hpp): opens as it is now
      // even when the live page changed.
      if (ImGui::MenuItem("Copy Snapshot Address", nullptr, false,
                          !state.snapshot_address.empty())) {
        cmd = UiCommand::CopyText;
        result.arg = state.snapshot_address;
      }
      if (!state.recent_visited.empty()) {
        ImGui::Separator();
        int i = 0;
        for (const std::string& url : state.recent_visited) {
          std::string label = url.size() > 64 ? elide_middle(url, 28) : url;
          ImGui::PushID(i++);
          if (ImGui::MenuItem(label.c_str())) {
            cmd = UiCommand::NavigateHistoryUrl;
            result.arg = url;
          }
          middle_open(cmd, result.arg, url);
          ImGui::PopID();
        }
      }
      ImGui::EndMenu();
    }
    force_menu("Subscriptions");
    uint32_t subs_unread = 0;
    for (const auto& sub : state.subscriptions) subs_unread += sub.unread;
    // New-items marker: an orange dot after the menu title.
    const bool subs_open = bar_menu("Subscriptions");
    if (subs_unread) {
      const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      ImGui::GetWindowDrawList()->AddCircleFilled(
          ImVec2(b.x - 3.0f, a.y + title_gap + 5.0f), 3.5f,
          IM_COL32(240, 130, 20, 255));
    }
    if (subs_open) {
      if (ImGui::MenuItem(state.current_subscribed ? "Unsubscribe current page" :
                          "Subscribe to current page", nullptr, false,
                          state.current_subscribed || state.current_subscribable)) {
        cmd = state.current_subscribed ? UiCommand::Unsubscribe : UiCommand::Subscribe;
        result.arg = state.subscription_address;
      }
      if (ImGui::MenuItem("Show Subscriptions…", nullptr, state.subscriptions_open))
        cmd = UiCommand::ToggleSubscriptions;
      menu_item_cmd("Refresh All", UiCommand::SubscriptionRefresh, cmd,
                    !state.subscriptions.empty());
      if (!state.subscriptions.empty()) {
        ImGui::Separator();
        int i = 0;
        for (const auto& sub : state.subscriptions) {
          std::string label = sub.title.empty() ? sub.address : sub.title;
          if (label.size() > 64) label = elide_middle(label, 28);
          if (sub.unread)
            label = "    " + label + " (" + std::to_string(sub.unread) + " new)";
          ImGui::PushID(i++);
          if (ImGui::MenuItem(label.c_str())) {
            cmd = UiCommand::SubscriptionOpen;
            result.arg = sub.address;
          }
          if (middle_open(cmd, result.arg, sub.address))
            cmd = UiCommand::SubscriptionOpenInTab;
          if (sub.unread) {
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2(a.x + 12.0f, a.y + (b.y - a.y) * 0.5f), 3.5f,
                IM_COL32(240, 130, 20, 255));
          }
          ImGui::PopID();
        }
      }
      ImGui::EndMenu();
    }
    force_menu("Services");
    if (bar_menu("Services")) {
      for (const UiService& s : state.services) {
        if (!s.group.empty()) ImGui::SeparatorText(s.group.c_str());
        if (menu_item_with_icon(s.label.c_str(), s.icon.c_str(), nullptr,
                                false, s.enabled)) {
          cmd = s.example ? UiCommand::OpenExample : UiCommand::NavigateUrl;
          result.arg = s.target;
        }
        if (!s.example && s.enabled) middle_open(cmd, result.arg, s.target);
        if (!s.tip.empty() &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled |
                                 ImGuiHoveredFlags_DelayShort))
          ImGui::SetTooltip("%s", s.tip.c_str());
      }
      ImGui::Separator();
      if (menu_item_with_icon("Open URL…", "site-web", "Ctrl+L", false))
        cmd = UiCommand::OpenUrlBar;
      ImGui::EndMenu();
    }
    force_menu("Help");
    if (bar_menu("Help")) {
      if (ImGui::MenuItem("Keyboard Reference", "F1", state.help))
        cmd = UiCommand::ToggleHelp;
      if (ImGui::MenuItem("Help Panel", "Shift+F1", state.hover_help))
        cmd = UiCommand::ToggleHoverHelp;
      ImGui::Separator();
      menu_item_cmd("Format Reference (FORMAT.md)", UiCommand::OpenFormatDoc, cmd);
      ImGui::Separator();
      if (ImGui::BeginMenu("Examples")) {
        menu_example_items(cmd, result.arg);
        ImGui::EndMenu();
      }
      ImGui::Separator();
      menu_item_cmd("About xanatoo", UiCommand::OpenAbout, cmd);
      ImGui::EndMenu();
    }

    // Unmatched Alt+letter request (e.g. after menus reordered) — drop it.
    menu_request_.clear();
    result.top_reserved = ImGui::GetFrameHeight();
    ImGui::PopClipRect();
    ImGui::EndMainMenuBar();
  }

  // ---- Toolbars + tab strip (all hidden while content only) ----
  if (!state.chrome_hidden) toolbars(state, result, cmd);

  // The divider owns the relationship between tiles. Its badge opens this
  // menu on left click; right-clicking anywhere on the divider does likewise.
  if (tile_menu_open_) {
    ImGui::SetNextWindowPos(ImVec2(tile_menu_x_, tile_menu_y_),
                            ImGuiCond_Appearing);
    ImGui::OpenPopup("##tile_menu");
    tile_menu_open_ = false;
  }
  if (ImGui::BeginPopup("##tile_menu")) {
    if (ImGui::MenuItem("Independent tiles", nullptr,
                        state.split && !state.trail && !state.diff))
      cmd = UiCommand::TilesIndependent;
    if (ImGui::MenuItem("Trail (linked navigation)", nullptr, state.trail))
      cmd = UiCommand::TilesTrail;
    if (ImGui::MenuItem("Compare tiles", nullptr, state.diff, state.can_diff))
      cmd = UiCommand::TilesCompare;
    ImGui::Separator();
    if (ImGui::MenuItem("Side by side", nullptr, !state.tiles_stacked))
      cmd = UiCommand::TilesColumns;
    if (ImGui::MenuItem("Stacked", nullptr, state.tiles_stacked))
      cmd = UiCommand::TilesRows;
    ImGui::Separator();
    if (ImGui::MenuItem("Swap Panes", nullptr, false, state.split))
      cmd = UiCommand::SwapPanes;
    if (ImGui::MenuItem(state.tiles_stacked ? "Join Top (keep top)"
                                             : "Join Left (keep left)",
                        nullptr, false, state.split))
      cmd = UiCommand::JoinLeftPane;
    if (ImGui::MenuItem(state.tiles_stacked ? "Join Bottom (keep bottom)"
                                             : "Join Right (keep right)",
                        nullptr, false, state.split))
      cmd = UiCommand::JoinRightPane;
    ImGui::EndPopup();
  }

  // ---- Menu of a bookmark's margin icon ----
  if (bookmark_menu_open_) {
    ImGui::SetNextWindowPos(ImVec2(context_x_, context_y_), ImGuiCond_Appearing);
    ImGui::OpenPopup("##bookmark_menu");
    bookmark_menu_open_ = false;
  }
  if (ImGui::BeginPopup("##bookmark_menu")) {
    const auto& bm = state.bookmark_menu;
    if (bm.address.empty()) {
      ImGui::CloseCurrentPopup();
    } else {
      std::string label = bm.label;
      if (label.size() > 64) label = elide_middle(label, 28);
      ImGui::TextDisabled("%s", label.c_str());
      ImGui::Separator();
      auto item = [&](const char* text, UiCommand c, const std::string& arg) {
        if (ImGui::MenuItem(text)) {
          cmd = c;
          result.arg = arg;
        }
      };
      item("Select Passage", UiCommand::SelectBookmark, bm.address);
      item("Go to Bookmark", UiCommand::NavigateUrl, bm.address);
      item("Show Only This", UiCommand::NavigateUrl, bm.cut);
      item("Copy Address", UiCommand::CopyText, bm.address);
      if (ImGui::BeginMenu("Move To")) {
        bookmark_move_items(state, bm.address, bm.folder, cmd, result.arg);
        ImGui::EndMenu();
      }
      ImGui::Separator();
      item("Remove Bookmark", UiCommand::RemoveBookmark, bm.address);
      if (ImGui::MenuItem("Show Bookmarks…", nullptr, state.bookmarks_open))
        cmd = UiCommand::ToggleBookmarks;
    }
    ImGui::EndPopup();
  }

  // ---- Menu of a History panel row ----
  if (history_menu_open_) {
    ImGui::SetNextWindowPos(ImVec2(context_x_, context_y_), ImGuiCond_Appearing);
    ImGui::OpenPopup("##history_menu");
    history_menu_open_ = false;
  }
  if (ImGui::BeginPopup("##history_menu")) {
    const auto& hm = state.history_menu;
    if (hm.address.empty()) {
      ImGui::CloseCurrentPopup();
    } else {
      std::string label = hm.label;
      if (label.size() > 64) label = elide_middle(label, 28);
      ImGui::TextDisabled("%s", label.c_str());
      ImGui::Separator();
      auto item = [&](const char* text, const char* shortcut,
                      const char* action, bool enabled = true) {
        if (ImGui::MenuItem(text, shortcut, false, enabled)) {
          cmd = UiCommand::HistoryAction;
          result.arg = action;
        }
      };
      item("Open", "Enter", "open");
      item("Open in New Tab", "Ctrl+Enter", "open-tab");
      item(hm.rewinds ? "Open as New Visit" : "Go Back to This Visit",
           "Shift+Enter", "open-other", hm.from_db);
      item("Copy Address", nullptr, "copy");
      ImGui::Separator();
      item("Remove from History", "Delete", "remove");
    }
    ImGui::EndPopup();
  }

  if (revision_menu_open_) {
    ImGui::SetNextWindowPos(ImVec2(context_x_, context_y_), ImGuiCond_Appearing);
    ImGui::OpenPopup("##revision_menu");
    revision_menu_open_ = false;
  }
  if (ImGui::BeginPopup("##revision_menu")) {
    const auto& menu = state.revision_menu;
    if (!menu.valid) {
      ImGui::CloseCurrentPopup();
    } else {
      if (menu.can_compare && ImGui::MenuItem("Compare with selected revision")) {
        cmd = UiCommand::RevisionCompareAction;
        result.arg = "compare";
      }
      if (ImGui::MenuItem(menu.selected ? "Clear comparison selection" :
                                        "Select for comparison")) {
        cmd = UiCommand::RevisionCompareAction;
        result.arg = "select";
      }
    }
    ImGui::EndPopup();
  }

  // ---- Menu of a Downloads panel row ----
  if (download_menu_open_) {
    ImGui::SetNextWindowPos(ImVec2(context_x_, context_y_), ImGuiCond_Appearing);
    ImGui::OpenPopup("##download_menu");
    download_menu_open_ = false;
  }
  if (ImGui::BeginPopup("##download_menu")) {
    const auto& dm = state.download_menu;
    if (dm.name.empty() && dm.url.empty()) {
      ImGui::CloseCurrentPopup();
    } else {
      std::string label = dm.name;
      if (label.size() > 64) label = elide_middle(label, 28);
      ImGui::TextDisabled("%s", label.c_str());
      ImGui::Separator();
      auto item = [&](const char* text, const char* action, bool enabled) {
        if (ImGui::MenuItem(text, nullptr, false, enabled)) {
          cmd = UiCommand::DownloadAction;
          result.arg = action;
        }
      };
      if (dm.active) {
        item("Cancel Download", "cancel", true);
      } else {
        item("Open", "open", dm.complete);
        item("Open in New Tab", "open-tab", dm.complete);
        item("Open with System Application", "system", dm.complete);
        item("Open Containing Folder", "folder", dm.complete);
      }
      ImGui::Separator();
      item("Copy Path", "copy-path", dm.complete);
      item("Copy Download Link", "copy-url", !dm.url.empty());
      if (!dm.active) {
        ImGui::Separator();
        item("Remove from List", "forget", true);
      }
    }
    ImGui::EndPopup();
  }

  // ---- Menu of the Media panel's seek bar ----
  if (media_menu_open_) {
    ImGui::SetNextWindowPos(ImVec2(context_x_, context_y_), ImGuiCond_Appearing);
    ImGui::OpenPopup("##media_menu");
    media_menu_open_ = false;
  }
  if (ImGui::BeginPopup("##media_menu")) {
    const auto& mm = state.media_menu;
    if (mm.title.empty()) {
      ImGui::CloseCurrentPopup();
    } else {
      std::string label = mm.title;
      if (label.size() > 64) label = elide_middle(label, 28);
      ImGui::TextDisabled("%s", label.c_str());
      ImGui::Separator();
      auto item = [&](const std::string& text, const char* action, bool enabled = true) {
        if (ImGui::MenuItem(text.c_str(), nullptr, false, enabled)) {
          cmd = UiCommand::MediaAction;
          result.arg = action;
        }
      };
      if (mm.time_bookmarked)
        item("Remove Bookmark at " + mm.time, "unbookmark-time");
      else
        item("Bookmark Position " + mm.time, "bookmark-time");
      item("Go to " + mm.time, "seek", mm.seekable);
      item("Copy Address at " + mm.time, "copy-time");
      item("Set A at " + mm.time, "set-a", mm.seekable);
      item("Set B at " + mm.time, "set-b", mm.seekable);
      if (!mm.section.empty()) {
        ImGui::Separator();
        if (mm.section_bookmarked)
          item("Remove Section Bookmark " + mm.section, "unbookmark-section");
        else
          item("Bookmark Section " + mm.section, "bookmark-section");
        item("Copy Section Address", "copy-section");
        if (!mm.section_selected) item("Set A-B to " + mm.section, "use-section", mm.seekable);
      }
      ImGui::Separator();
      if (ImGui::MenuItem("A-B Markers", nullptr, mm.ab, mm.seekable)) cmd = UiCommand::AudioAB;
      if (ImGui::MenuItem(mm.ab ? "Loop Section" : "Loop File", nullptr, mm.loop))
        cmd = UiCommand::AudioLoop;
      if (mm.video &&
          ImGui::MenuItem(mm.fullscreen ? "Leave Fullscreen" : "Fullscreen", "F", mm.fullscreen))
        cmd = UiCommand::AudioFullscreen;
      if (mm.video && !mm.fullscreen &&
          ImGui::MenuItem("Hide Controls", "H", mm.controls_hidden))
        cmd = UiCommand::AudioControls;
      ImGui::Separator();
      if (mm.file_bookmarked)
        item("Remove File Bookmark", "unbookmark-file");
      else
        item("Bookmark File", "bookmark-file");
    }
    ImGui::EndPopup();
  }

  // ---- Context menu (right-click in document) ----
  if (context_open_) {
    ImGui::SetNextWindowPos(ImVec2(context_x_, context_y_), ImGuiCond_Appearing);
    ImGui::OpenPopup("##doc_context");
    context_open_ = false;
  }
  if (ImGui::BeginPopup("##doc_context")) {
    if (state.document_fullscreen) {
      menu_item_cmd("Exit Fullscreen", UiCommand::ToggleChrome, cmd, true, "F11");
      ImGui::Separator();
    }
    if (state.context_save_pdf) {
      if (ImGui::MenuItem("Save PDF…")) cmd = UiCommand::SavePdf;
      ImGui::Separator();
    }
    if (!state.context_image_address.empty()) {
      auto item = [&](const char* label, UiCommand action, bool enabled = true) {
        if (ImGui::MenuItem(label, nullptr, false, enabled)) {
          cmd = action;
          result.arg = state.context_image_address;
        }
      };
#if defined(XANATOO_SDL3) && XANATOO_SDL3
      item("Copy image", UiCommand::CopyImage);
#else
      item("Copy image", UiCommand::CopyImage, false);
#endif
      item("Copy image address", UiCommand::CopyText);
      item("Open image in new tab", UiCommand::OpenImageInTab);
      ImGui::Separator();
    }
    if (!state.context_link.empty()) {
      if (ImGui::MenuItem("Open Link in New Tab")) {
        cmd = UiCommand::OpenLinkInTab;
        result.arg = state.context_link_href;
      }
      if (ImGui::MenuItem("Open Link in Trail View")) {
        cmd = UiCommand::OpenLinkInTrail;
        result.arg = state.context_link_href;
      }
      if (ImGui::MenuItem("Copy Link Address")) {
        cmd = UiCommand::CopyText;
        result.arg = state.context_link;
      }
      if (!state.context_link_text.empty() &&
          ImGui::MenuItem(state.context_link_rel == "tag" ? "Copy Hashtag"
                                                          : "Copy Handle")) {
        cmd = UiCommand::CopyText;
        result.arg = state.context_link_text;
      }
      if (ImGui::MenuItem("Download Link", nullptr, false,
                          state.context_link_web)) {
        cmd = UiCommand::DownloadUrl;
        result.arg = state.context_link;
      }
      ImGui::Separator();
    }
    if (state.list_fold != 0) {
      auto item = [&](const char* label, const char* a) {
        if (ImGui::MenuItem(label)) {
          cmd = UiCommand::ListFold;
          result.arg = a;
        }
      };
      if (state.list_fold == 2) item("Fold List Item", "toggle");
      if (state.list_fold == 3) item("Expand List Item", "toggle");
      item("Fold All", "fold-all");
      item("Expand All", "expand-all");
      ImGui::Separator();
    }
    if (state.stretch_fold != 0) {
      auto item = [&](const char* label, const char* a) {
        if (ImGui::MenuItem(label)) {
          cmd = UiCommand::StretchFold;
          result.arg = a;
        }
      };
      item(state.stretch_fold == 2 ? "Show Full Text" : "Show Summary",
           "toggle");
      item("All Full Texts", "full");
      item("All Summaries", "summaries");
      ImGui::Separator();
    }
    if (state.collection) {
      if (ImGui::BeginMenu("Collection")) {
        menu_collection_items(state, cmd, result.arg);
        ImGui::EndMenu();
      }
      ImGui::Separator();
    }
    if (!state.context_quote_items.empty()) {
      for (size_t i = 0; i < state.context_quote_items.size(); ++i) {
        const std::string label =
            state.context_quote_items[i] + "##quote" + std::to_string(i);
        if (ImGui::MenuItem(label.c_str())) {
          cmd = UiCommand::JumpQuote;
          result.arg = std::to_string(i);
        }
      }
      ImGui::Separator();
    }
    if (state.context_can_reply) {
      menu_item_cmd("Reply…", UiCommand::OpenReply, cmd);
      ImGui::Separator();
    }
    if (!state.context_blocks.empty()) {
      // Cut the view to a block around the pointer (page//id:…); hovering
      // an item washes its block in the page.
      const auto block_item = [&](const char* label, int index, UiCommand c) {
        if (ImGui::MenuItem(label)) {
          cmd = c;
          result.arg = std::to_string(index);
        }
        if (ImGui::IsItemHovered()) result.context_block_hot = index;
      };
      const auto block_menu = [&](const char* title, UiCommand c) {
        if (!ImGui::BeginMenu(title)) return;
        for (size_t i = 0; i < state.context_blocks.size(); ++i) {
          const std::string label =
              state.context_blocks[i] + "##block" + std::to_string(i);
          block_item(label.c_str(), static_cast<int>(i), c);
        }
        ImGui::EndMenu();
      };
      if (state.context_article >= 0) {
        block_item("Open Article", state.context_article, UiCommand::OpenBlock);
        block_item("Open Article in New Window", state.context_article,
                   UiCommand::OpenBlockInWindow);
      }
      if (state.context_section >= 0) {
        block_item("Open Section", state.context_section, UiCommand::OpenBlock);
        block_item("Open Section in New Window", state.context_section,
                   UiCommand::OpenBlockInWindow);
      }
      block_menu("Open Block", UiCommand::OpenBlock);
      block_menu("Open Block in New Window", UiCommand::OpenBlockInWindow);
      block_menu("Copy Block Address", UiCommand::CopyBlockAddress);
      ImGui::Separator();
    }
    if (state.browse) {
      // Read-only: no edit / format / insert / structure items.
      menu_item_cmd("Copy", UiCommand::Copy, cmd, state.selection, "Ctrl+C");
      menu_item_cmd("Copy Page URL", UiCommand::CopyPageUrl, cmd, true,
                    "Ctrl+Shift+U");
      if (!state.web_page_url.empty())
        menu_item_cmd("Copy Web Page URL", UiCommand::CopyPageWebUrl, cmd);
      if (!state.browser_url.empty())
        menu_item_cmd("Open in Browser", UiCommand::OpenInBrowser, cmd);
      if (!state.context_comment_native.empty()) {
        if (ImGui::MenuItem("Copy Link to Comment")) {
          cmd = UiCommand::CopyText;
          result.arg = state.context_comment_native;
        }
        if (ImGui::MenuItem("Copy Web Permalink")) {
          cmd = UiCommand::CopyText;
          result.arg = state.context_comment_web;
        }
      }
      menu_item_cmd("Select Block", UiCommand::SelectBlock, cmd, true);
      ImGui::Separator();
    } else {
      menu_item_cmd("Cut", UiCommand::Cut, cmd, state.selection, "Ctrl+X");
      menu_item_cmd("Copy", UiCommand::Copy, cmd, state.selection, "Ctrl+C");
      menu_item_cmd("Paste", UiCommand::Paste, cmd, true, "Ctrl+V");
      ImGui::Separator();
      menu_item_cmd("Bold", UiCommand::Bold, cmd, state.selection, "Ctrl+B");
      menu_item_cmd("Italic", UiCommand::Italic, cmd, state.selection, "Ctrl+I");
      menu_item_cmd("Strikethrough", UiCommand::Strike, cmd, state.selection,
                    "Ctrl+Shift+X");
      menu_item_cmd("Code", UiCommand::Code, cmd, state.selection, "Ctrl+`");
      menu_item_cmd("Link…", UiCommand::Link, cmd, state.selection, "Ctrl+Shift+K");
      menu_item_cmd("Citation…", UiCommand::Cite, cmd, state.selection,
                    "Ctrl+Shift+C");
      ImGui::Separator();
      if (ImGui::BeginMenu("Insert")) {
        menu_insert_items(cmd);
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu("Structure")) {
        menu_structure_items(state, cmd);
        ImGui::EndMenu();
      }
      ImGui::Separator();
      menu_item_cmd("Select Block", UiCommand::SelectBlock, cmd, true);
      menu_item_cmd("Duplicate Block", UiCommand::DuplicateBlock, cmd, true,
                    "Ctrl+D");
      menu_item_cmd("Move Block Up", UiCommand::MoveBlockUp, cmd, true, "Alt+Up");
      menu_item_cmd("Move Block Down", UiCommand::MoveBlockDown, cmd, true,
                    "Alt+Down");
      ImGui::Separator();
    }
    if (state.has_path) {
      if (state.selection)
        menu_item_cmd("Bookmark Selection", UiCommand::AddBookmark, cmd);
      menu_item_cmd(state.page_bookmarked ? "Remove Page Bookmark"
                                         : "Bookmark This Page",
                    state.page_bookmarked ? UiCommand::RemoveBookmark
                                          : UiCommand::BookmarkPage, cmd);
      menu_item_cmd("Bookmark Reading Position", UiCommand::BookmarkPosition,
                    cmd);
      ImGui::Separator();
    }
    if (state.selection)
      menu_item_cmd("Highlight Selection", UiCommand::AddAnnotationHighlight, cmd);
    if (state.selection)
      menu_item_cmd("Annotate Selection…", UiCommand::ToggleAnnotationsPanel,
                    cmd, !state.annotations_panel_open);
    if (state.selection) ImGui::Separator();
    menu_item_cmd("Find…", UiCommand::Find, cmd, true, "Ctrl+F");
    if (!state.browse)
      menu_item_cmd("Replace…", UiCommand::Replace, cmd, true, "Ctrl+H");
    ImGui::EndPopup();
  }

  if (window_menu_open_) {
    ImGui::SetNextWindowPos(ImVec2(window_menu_x_, window_menu_y_),
                            ImGuiCond_Appearing);
    ImGui::OpenPopup("##document_window_menu");
    window_menu_open_ = false;
  }
  if (ImGui::BeginPopup("##document_window_menu")) {
    if (ImGui::MenuItem("Move Window to Tab")) {
      cmd = UiCommand::PopIn;
      result.arg = std::to_string(window_menu_buffer_id_);
    }
    ImGui::EndPopup();
  }

  find_bar(state, cmd, result.arg);
  edit_bars(state, result, cmd);
  service_bars(state, result, cmd);

  // ---- Reply composer: custom floating/dockable frame underneath, ImGui
  // text widget and Markdown tools above it. Only Nostr targets post (as the
  // unlocked active account); for other services Send is a mockup.
  if (state.reply_open &&
      !(state.shaded_panels & ui_panel_bit(UiPanelLayer::Reply))) {
    if (reply_generation_seen_ != state.reply_generation) {
      reply_generation_seen_ = state.reply_generation;
      std::snprintf(reply_input_, sizeof(reply_input_), "%s",
                    state.reply_text.c_str());
      reply_selection_ = {};
      reply_focus_ = true;
      reply_restore_selection_ = false;
    }
    if (reply_sent_seen_ != state.reply_sent) {
      reply_sent_seen_ = state.reply_sent;  // went out: empty the composer
      reply_input_[0] = '\0';
      reply_selection_ = {};
    }
    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoCollapse;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(7.f * scale_, 6.f * scale_));
    if (begin_panel(state, UiPanelLayer::Reply, "##reply_editor",
                    flags, state.reply_x, state.reply_y,
                    state.reply_w, state.reply_h, 4, 40)) {
      auto muted_wrapped = [](const char* label) {
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", label);
        ImGui::PopStyleColor();
      };
      // What is being answered: whose message, its first words (quoted,
      // with a bar like the Quote style) and a button to scroll to it.
      const std::string who =
          state.reply_author.empty() ? "a message" : state.reply_author;
      const char* show = "Show";
      const float show_w = ImGui::CalcTextSize(show).x +
                           2.f * ImGui::GetStyle().FramePadding.x;
      const float right_edge = ImGui::GetWindowContentRegionMax().x;
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Replying to");
      ImGui::SameLine(0.f, ImGui::CalcTextSize(" ").x);
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.12f, 0.33f, 0.62f, 1.f));
      ImGui::TextUnformatted(who.c_str());
      ImGui::PopStyleColor();
      if (!state.reply_label.empty()) {
        const float x = right_edge - show_w;
        if (ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x +
                ImGui::GetStyle().ItemSpacing.x < x)
          ImGui::SameLine(x);
        if (classic_button(show)) cmd = UiCommand::ShowReplyTarget;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
          ImGui::SetTooltip("Scroll to the message being answered");
      }
      if (!state.reply_excerpt.empty()) {
        const ImVec2 top = ImGui::GetCursorScreenPos();
        const float bar = 3.f * scale_;
        ImGui::Indent(bar + 6.f * scale_);
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", state.reply_excerpt.c_str());
        ImGui::PopStyleColor();
        if (!state.reply_label.empty() &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
          ImGui::SetTooltip("Message id: %s", state.reply_label.c_str());
        ImGui::Unindent(bar + 6.f * scale_);
        const float bottom = ImGui::GetItemRectMax().y;
        ImGui::GetWindowDrawList()->AddRectFilled(
            top, ImVec2(top.x + bar, bottom), IM_COL32(220, 140, 40, 200));
      }
      if (state.reply_nostr)
        muted_wrapped(("Threaded under this message; " + who +
                       " and the people it tags are notified.").c_str());
      if (!state.reply_nostr) {
        muted_wrapped("Markdown · preview only, no account connected");
      } else if (state.reply_blocker.empty()) {
        muted_wrapped(("Nostr · plain text · as " + state.reply_account).c_str());
      } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.16f, 0.10f, 1.f));
        ImGui::TextWrapped("Cannot send: %s", state.reply_blocker.c_str());
        ImGui::PopStyleColor();
      }
      const char* format_before = nullptr;
      const char* format_after = nullptr;
      auto format = [&](const char* label, const char* before,
                        const char* after) {
        if (classic_button(label)) {
          format_before = before;
          format_after = after;
        }
      };
      auto next_format_on_same_line = [&](const char* label) {
        const float next_width = ImGui::CalcTextSize(label).x +
                                 2.f * ImGui::GetStyle().FramePadding.x;
        const float right = ImGui::GetWindowPos().x +
                            ImGui::GetWindowContentRegionMax().x;
        if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x +
                next_width < right)
          ImGui::SameLine();
      };
      // Nostr notes are plain text: only the quote prefix means anything.
      if (!state.reply_nostr) {
        format("Bold", "**", "**");
        next_format_on_same_line("Italic");
        format("Italic", "*", "*");
        next_format_on_same_line("Code");
        format("Code", "`", "`");
        next_format_on_same_line("Link");
        format("Link", "[", "](https://)");
        next_format_on_same_line("Quote");
      }
      format("Quote", "> ", "");

      if (reply_focus_) {
        ImGui::SetKeyboardFocusHere();
        reply_focus_ = false;
      }
      const auto track_selection = [](ImGuiInputTextCallbackData* cb) -> int {
        auto* ui = static_cast<Ui*>(cb->UserData);
        if (ui->reply_restore_selection_) {
          cb->SelectionStart = ui->reply_selection_.begin;
          cb->SelectionEnd = ui->reply_selection_.end;
          cb->CursorPos = ui->reply_selection_.end;
          ui->reply_restore_selection_ = false;
        }
        ui->reply_selection_.begin = cb->SelectionStart;
        ui->reply_selection_.end = cb->SelectionEnd;
        return 0;
      };
      // Reserve room for the Send control and a wrapped status line. Docking
      // to a narrow side column may turn the tools and text into more lines;
      // use the remaining space instead of assuming a one-row toolbar.
      const float footer_h = ImGui::GetFrameHeightWithSpacing() +
                             3.f * ImGui::GetTextLineHeightWithSpacing();
      const float edit_h = std::max(48.f * scale_,
                                    ImGui::GetContentRegionAvail().y - footer_h);
      if (ImGui::InputTextMultiline(
              "##reply_text", reply_input_, sizeof(reply_input_),
              ImVec2(-1.f, edit_h), ImGuiInputTextFlags_CallbackAlways,
              track_selection, this)) {
        cmd = UiCommand::EditReply;
        result.arg = reply_input_;
      }
      // Apply formatting *after* InputText has finished its frame, so its
      // retained editor buffer cannot overwrite the newly inserted markup.
      if (format_before) {
        const auto wrapped = wrap_reply_markup(
            reply_input_, static_cast<size_t>(std::max(0, reply_selection_.begin)),
            static_cast<size_t>(std::max(0, reply_selection_.end)),
            format_before, format_after);
        if (wrapped.text.size() < sizeof(reply_input_)) {
          std::memcpy(reply_input_, wrapped.text.c_str(), wrapped.text.size() + 1);
          reply_selection_.begin = static_cast<int>(wrapped.selection_begin);
          reply_selection_.end = static_cast<int>(wrapped.selection_end);
          reply_restore_selection_ = true;
          reply_focus_ = true;
          cmd = UiCommand::EditReply;
          result.arg = reply_input_;
        }
      }
      const bool can_send = reply_input_[0] != '\0' && !state.reply_sending &&
          (!state.reply_nostr || state.reply_blocker.empty());
      ImGui::BeginDisabled(!can_send);
      if (classic_button(state.reply_sending ? "Sending…" : "Send reply")) {
        cmd = UiCommand::SendReply;
        result.arg = reply_input_;
      }
      ImGui::EndDisabled();
      if (!state.reply_nostr)
        muted_wrapped("Mockup: Send does not post anything");
      else if (!state.reply_status.empty())
        muted_wrapped(state.reply_status.c_str());
      else
        muted_wrapped("Signed and sent to public relays; it cannot be taken back.");
    }
    ImGui::End();
    ImGui::PopStyleVar();
  }

  // ---- Download confirmation ----
  if (state.download_confirm_open) {
    bool open = true;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(440.f * scale_, 0.f), ImGuiCond_Appearing);
    ImGui::SetNextWindowFocus();
    if (begin_panel_dialog("Download", open, theme::Tint::Orange, scale_,
                           ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextUnformatted("Download this file?");
      ImGui::Spacing();
      {
        std::string shown = state.download_confirm_url;
        if (shown.size() > 72) {
          // Cut on UTF-8 character boundaries (no continuation bytes).
          auto boundary = [&](size_t i) {
            while (i > 0 && i < shown.size() &&
                   (static_cast<unsigned char>(shown[i]) & 0xC0) == 0x80)
              --i;
            return i;
          };
          const size_t head = boundary(34);
          const size_t tail = boundary(shown.size() - 34);
          shown = shown.substr(0, head) + "…" + shown.substr(tail);
        }
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 400.f * scale_);
        ImGui::TextDisabled("%s", shown.c_str());
        ImGui::PopTextWrapPos();
      }
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();
      const float btn_w = 120.f * scale_;
      if (classic_button("Cancel", ImVec2(btn_w, 0)))
        cmd = UiCommand::CancelDownload;
      ImGui::SameLine();
#if !defined(EMSCRIPTEN)
      // Folder picker availability is decided when the command runs.
      if (classic_button("Download to…", ImVec2(btn_w + 16.f * scale_, 0)))
        cmd = UiCommand::DownloadToFolder;
      ImGui::SameLine();
#endif
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered]);
      if (classic_button("Download", ImVec2(btn_w, 0)))
        cmd = UiCommand::ConfirmDownload;
      ImGui::PopStyleColor();
      if (ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
      if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
          cmd = UiCommand::CancelDownload;
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) ||
            ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
          cmd = UiCommand::ConfirmDownload;
      }
    }
    ImGui::End();
    if (!open) cmd = UiCommand::CancelDownload;
  }

  // ---- Preferences ----
  if (state.preferences_open) {
    bool open = true;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_FirstUseEver,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520.f * scale_, 560.f * scale_),
                            ImGuiCond_FirstUseEver);
    if (begin_panel_dialog("Preferences", open, theme::Tint::Violet, scale_)) {
      ImGui::PushTextWrapPos(0.f);
      // Checkboxes show the state; a click issues the toggle command.
      auto toggle = [&](const char* label, bool on, UiCommand c,
                        const char* tip) {
        if (ImGui::Checkbox(label, &on)) cmd = c;
        ImGui::TextDisabled("  %s", tip);
      };
      ImGui::SeparatorText("Browsing");
      toggle("Preview links on hover", state.hover_preview,
             UiCommand::ToggleHoverPreview,
             "Split view: a link hovered on the left shows on the right");
      toggle("History clicks go back in the view", state.history_rewinds,
             UiCommand::ToggleHistoryRewinds,
             "A visit still in Back / Forward is stepped to, not visited anew "
             "(Shift: the other way)");
      toggle("Make web addresses clickable", state.auto_links,
             UiCommand::ToggleAutoLinks,
             "http:// and https:// text in any document becomes a link");
      toggle("Show images of image addresses", state.auto_images,
             UiCommand::ToggleAutoImages,
             "A picture under text that is the address of an image file");
      toggle("Reader mode by default", state.reader_default,
             UiCommand::ToggleReaderDefault,
             "Web pages without navigation, sidebars and footers (F9 per view)");
      toggle("Paged reading by default", state.paged_default,
             UiCommand::TogglePagedDefault,
             "Ebook style pages instead of scrolling (F8 per view)");
      ImGui::SeparatorText("Reading");
      toggle("Footnote overlay", state.footnote_overlay,
             UiCommand::ToggleFootnoteOverlay,
             "Notes of on-screen footnote marks at the pane bottom");
      toggle("Mark bookmarks in the page", state.bookmark_marks,
             UiCommand::ToggleBookmarkMarks,
             "Wash over bookmarked passages, a ribbon at reading positions");
      toggle("Headings on the scrollbar", state.heading_ticks,
             UiCommand::ToggleHeadingTicks,
             "A tick on the scrollbar track for each heading of the page");
      toggle("Justify text", state.justify, UiCommand::ToggleJustify,
             "Wrapped body lines fill the column");
#ifdef __ANDROID__
      ImGui::SeparatorText("Updates");
      toggle("Automatic update checks", state.auto_updates,
             UiCommand::ToggleAutoUpdates,
             "Notify when a new Android version is available");
#endif
      ImGui::SeparatorText("Interface");
      static constexpr float scales[] = {
          0.75f, 0.9f, 1.f, 1.1f, 1.25f, 1.4f, 1.6f, 1.8f, 2.f};
      static constexpr const char* labels[] = {
          "75%", "90%", "100%", "110%", "125%", "140%", "160%",
          "180%", "200%"};
      int selected_scale = 0;
      for (int i = 1; i < IM_ARRAYSIZE(scales); ++i)
        if (std::abs(state.ui_scale - scales[i]) <
            std::abs(state.ui_scale - scales[selected_scale]))
          selected_scale = i;
      if (ImGui::BeginCombo("UI scale", labels[selected_scale])) {
        for (int i = 0; i < IM_ARRAYSIZE(scales); ++i) {
          if (ImGui::Selectable(labels[i], i == selected_scale)) {
            cmd = UiCommand::SetUiScale;
            result.arg = std::to_string(scales[i]);
          }
        }
        ImGui::EndCombo();
      }
      ImGui::TextDisabled("32 px icons at the platform default; text scales too.");
      ImGui::SeparatorText("Search engines");
      {
        // Default engine for the search bar and the "?" location-bar alias.
        const UiSearchEngine* cur = nullptr;
        for (const UiSearchEngine& e : state.search_engines)
          if (e.id == state.search_engine && e.on) {
            cur = &e;
            break;
          }
        if (!cur) {
          for (const UiSearchEngine& e : state.search_engines)
            if (e.on) {
              cur = &e;
              break;
            }
        }
        const char* preview =
            cur ? cur->name.c_str()
                : (state.search_engines.empty() ? "(none)" : "(first on)");
        if (ImGui::BeginCombo("Default search engine", preview)) {
          for (const UiSearchEngine& e : state.search_engines) {
            if (!e.on) continue;
            const bool selected = cur && e.id == cur->id;
            if (ImGui::Selectable(e.name.c_str(), selected)) {
              cmd = UiCommand::SelectSearchEngine;
              result.arg = e.id;
            }
            if (selected) ImGui::SetItemDefaultFocus();
          }
          ImGui::EndCombo();
        }
        ImGui::TextDisabled("Used by the search bar and by \"? query\" in the "
                            "location bar (Ctrl+L).");
      }
      if (ImGui::TreeNode("Engines in the search menu")) {
        search_engine_checkboxes(state, cmd, result.arg);
        ImGui::TreePop();
      }
      ImGui::TextDisabled("  Engines that need an API key or refuse anonymous "
                          "use start off");
      ImGui::SeparatorText("Languages");
      languages_field(state, cmd, result.arg, "prefs_languages");
      ImGui::TextDisabled("Preferred languages for web pages, best first "
                          "(Accept-Language). Reload a page to apply.");
      ImGui::SeparatorText("Accounts and APIs");
      ImGui::TextWrapped("API keys are managed in a separate dialog, not in "
                         "view.conf or a document.");
      if (classic_button("Credentials…")) cmd = UiCommand::ToggleCredentials;
      ImGui::Spacing();
      ImGui::TextDisabled("Display preferences are saved to view.conf.");
      if (classic_button("Close")) open = false;
      ImGui::PopTextWrapPos();
    }
    ImGui::End();
    if (!open) cmd = UiCommand::TogglePreferences;
  }

  // ---- Credentials (all API clients) ----
  if (state.credentials_open) {
    bool open = true;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_FirstUseEver,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(610.f * scale_, 620.f * scale_),
                            ImGuiCond_FirstUseEver);
    if (begin_panel_dialog("Credentials", open, theme::Tint::Steel, scale_)) {
      ImGui::PushTextWrapPos(0.f);
      if (!catalog_credentials_loaded_) {
        const CatalogCredentials stored = load_catalog_credentials();
        for (size_t i = 0; i < catalog_key_count; ++i) {
          std::snprintf(catalog_credential_input_[i],
                        sizeof(catalog_credential_input_[i]), "%s",
                        stored.values[i].c_str());
          catalog_credential_shown_[i] = false;
        }
        catalog_credentials_loaded_ = true;
      }
      ImGui::TextWrapped("Keys are stored only on this device, separate from "
                         "history, bookmarks and documents. Environment "
                         "variables override saved values. Leave a field "
                         "empty and Save to remove that key.");
      if (no_save())
        ImGui::TextDisabled("--no-save: credential changes are disabled.");

      const CatalogCredentials stored = load_catalog_credentials();
      auto catalog_field = [&](CatalogKey key, const char* label) {
        const size_t i = catalog_key_index(key);
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetNextItemWidth(-92.f * scale_);
        ImGui::InputText(label, catalog_credential_input_[i],
                         sizeof(catalog_credential_input_[i]),
                         catalog_credential_shown_[i] ? 0 : ImGuiInputTextFlags_Password);
        ImGui::SameLine();
        ImGui::Checkbox("Show", &catalog_credential_shown_[i]);
        if (catalog_credential_from_environment(key)) {
          ImGui::TextDisabled("  Environment override: %s",
                              catalog_key_info[i].environment);
        } else if (!stored.values[i].empty()) {
          ImGui::TextDisabled("  Saved locally");
        } else {
          ImGui::TextDisabled("  Not configured");
        }
        if (!valid_catalog_credential(catalog_credential_input_[i]))
          ImGui::TextDisabled("  Invalid value (ASCII token, max 511 bytes, no spaces)");
        ImGui::PopID();
      };

      ImGui::SeparatorText("IGDB / Twitch (games)");
      catalog_field(CatalogKey::IgdbClientId, "Client ID");
      catalog_field(CatalogKey::IgdbAccessToken, "Access token");
      ImGui::TextDisabled("The Twitch app access token expires and must be renewed.");
      if (classic_button("IGDB API docs"))
        xanatoo::sdl::open_url("https://api-docs.igdb.com/");

      ImGui::SeparatorText("RAWG (games)");
      catalog_field(CatalogKey::RawgApiKey, "API key");
      if (classic_button("RAWG API docs"))
        xanatoo::sdl::open_url("https://rawg.io/apidocs");

      ImGui::SeparatorText("TMDB (movies and TV)");
      catalog_field(CatalogKey::TmdbReadToken, "API read token");
      catalog_field(CatalogKey::TmdbApiKey, "API key v3");
      ImGui::TextDisabled("Read token is preferred; API key v3 is a fallback.");
      if (classic_button("TMDB API settings"))
        xanatoo::sdl::open_url("https://www.themoviedb.org/settings/api");

      ImGui::SeparatorText("Discogs (music)");
      catalog_field(CatalogKey::DiscogsToken, "Personal token");
      if (classic_button("Discogs developer settings"))
        xanatoo::sdl::open_url("https://www.discogs.com/settings/developers");

      ImGui::SeparatorText("Similar websites (Related Sites panel)");
      ImGui::TextDisabled("Optional: similarsites.com needs no key. Free "
                          "accounts add more results.");
      catalog_field(CatalogKey::WebxtrendToken, "webXtrend token");
      if (classic_button("webXtrend API"))
        xanatoo::sdl::open_url("https://webxtrend.com/similar-websites");
      catalog_field(CatalogKey::SimilarsNetKey, "similars.net key");
      if (classic_button("similars.net API"))
        xanatoo::sdl::open_url("https://similars.net/api/");

      ImGui::SeparatorText("Matrix (room messages, spaces)");
      ImGui::TextDisabled("Optional: directories, room summaries and profiles "
                          "need no account. The token is sent to this "
                          "homeserver only; xanatoo never joins or posts.");
      catalog_field(CatalogKey::MatrixHomeserver, "Homeserver");
      catalog_field(CatalogKey::MatrixAccessToken, "Access token");
      ImGui::TextDisabled("Homeserver: matrix.org or https://matrix-client.matrix.org. "
                          "Element: Settings → Help & About → Access token.");

      ImGui::Spacing();
      bool catalog_values_valid = true;
      for (const auto& input : catalog_credential_input_)
        if (!valid_catalog_credential(input)) catalog_values_valid = false;
      ImGui::BeginDisabled(no_save() || !catalog_values_valid);
      if (classic_button("Save catalog keys")) {
        CatalogCredentials values;
        for (size_t i = 0; i < catalog_key_count; ++i)
          values.values[i] = catalog_credential_input_[i];
        cmd = UiCommand::SaveCatalogCredentials;
        result.arg = catalog_credentials_command(values);
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::BeginDisabled(no_save());
      if (classic_button("Remove all catalog keys")) {
        cmd = UiCommand::ClearCatalogCredentials;
        for (auto& input : catalog_credential_input_) input[0] = '\0';
      }
      ImGui::EndDisabled();

#if defined(XANATOO_REDDIT) && XANATOO_REDDIT
      ImGui::SeparatorText("Reddit (read-only OAuth)");
      if (!reddit_credentials_loaded_) {
        const RedditCredentials c = load_reddit_credentials();
        std::snprintf(reddit_client_id_, sizeof(reddit_client_id_), "%s",
                      c.client_id.c_str());
        std::snprintf(reddit_client_secret_, sizeof(reddit_client_secret_), "%s",
                      c.client_secret.c_str());
        reddit_credentials_loaded_ = true;
      }
      ImGui::TextWrapped("For approved Reddit Data API clients. Do not enter "
                         "your Reddit account password.");
      if (reddit_credentials_from_environment())
        ImGui::TextDisabled("Environment variables override saved credentials.");
      else if (load_reddit_credentials().complete())
        ImGui::TextDisabled("Saved locally");
      else
        ImGui::TextDisabled("Not configured");
      ImGui::InputText("Client ID##reddit", reddit_client_id_,
                       sizeof(reddit_client_id_));
      ImGui::InputText("Client secret##reddit", reddit_client_secret_,
                       sizeof(reddit_client_secret_),
                       reddit_secret_visible_ ? 0 : ImGuiInputTextFlags_Password);
      ImGui::SameLine();
      ImGui::Checkbox("Show##reddit", &reddit_secret_visible_);
      ImGui::BeginDisabled(no_save() || !reddit_client_id_[0] ||
                           !reddit_client_secret_[0]);
      if (classic_button("Save Reddit credentials")) {
        cmd = UiCommand::SaveRedditCredentials;
        result.arg = std::string(reddit_client_id_) + "\n" + reddit_client_secret_;
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::BeginDisabled(no_save());
      if (classic_button("Remove Reddit credentials")) {
        cmd = UiCommand::ClearRedditCredentials;
        reddit_client_id_[0] = reddit_client_secret_[0] = '\0';
      }
      ImGui::EndDisabled();
      if (classic_button("Request Reddit API access"))
        xanatoo::sdl::open_url("https://support.reddithelp.com/hc/en-us/requests/new?"
                               "ticket_form_id=14868593862164");
      ImGui::SameLine();
      if (classic_button("Reddit app settings"))
        xanatoo::sdl::open_url("https://www.reddit.com/prefs/apps");
#endif
      ImGui::Separator();
      ImGui::TextDisabled("Stored in catalog-credentials.conf and reddit.conf "
                           "in the private config directory.");
      if (classic_button("Close##credentials")) open = false;
      ImGui::PopTextWrapPos();
    }
    ImGui::End();
    if (!open) cmd = UiCommand::ToggleCredentials;
  } else {
    if (catalog_credentials_loaded_ || reddit_credentials_loaded_) {
      for (auto& input : catalog_credential_input_)
        std::memset(input, 0, sizeof(input));
      std::memset(reddit_client_id_, 0, sizeof(reddit_client_id_));
      std::memset(reddit_client_secret_, 0, sizeof(reddit_client_secret_));
    }
    catalog_credentials_loaded_ = false;
    reddit_credentials_loaded_ = false;
    reddit_secret_visible_ = false;
    for (bool& shown : catalog_credential_shown_) shown = false;
  }

  // ---- Style sheet picker (simple list) ----
  if (state.styles_open &&
      !(state.shaded_panels & ui_panel_bit(UiPanelLayer::Styles))) {
    constexpr ImGuiWindowFlags style_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoCollapse;
    if (begin_panel(state, UiPanelLayer::Styles, "##styles_panel_body",
                    style_flags, state.styles_x, state.styles_y,
                    state.styles_w, state.styles_h)) {
      static const char* const kTabs[] = {"Sheet", "Colours", "Fonts"};
      if (ImGui::BeginTabBar("##style_layers")) {
        for (int t = 0; t < 3; ++t)
          if (ImGui::BeginTabItem(kTabs[t])) {
            style_panel_tab_ = t;
            ImGui::EndTabItem();
          }
        ImGui::EndTabBar();
      }
      auto title_of = [&](const std::string& name) {
        for (const StyleEntry& e : state.style_entries)
          if (e.name == name) return e.title;
        return name;
      };
      const int tab = style_panel_tab_;
      const std::string& lent = tab == 1 ? state.style_colors : state.style_fonts;
      ImGui::TextDisabled("%s", tab == 0 ? "Applied sheet"
                                : tab == 1 ? "Colours taken from"
                                           : "Fonts taken from");
      ImGui::Separator();
      const float row_h = ImGui::GetTextLineHeight();
      ImDrawList* draw = ImGui::GetWindowDrawList();
      if (tab > 0) {
        const bool active = lent.empty();
        std::string label = (active ? "● " : "  ") + std::string("From style sheet");
        if (ImGui::Selectable(label.c_str(), active) && !active) {
          cmd = tab == 1 ? UiCommand::SelectStyleColors : UiCommand::SelectStyleFonts;
          result.arg.clear();
        }
      }
      for (const StyleEntry& entry : state.style_entries) {
        if (tab > 0 && entry.name == state.style) continue;
        ImGui::PushID(entry.name.c_str());
        const bool active = tab == 0 ? state.style == entry.name : lent == entry.name;
        std::string label = (active ? "● " : "  ") + entry.title;
        const ImVec2 row = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable(label.c_str(), active)) {
          if (!active) {
            cmd = tab == 0 ? UiCommand::SelectStyle
                : tab == 1 ? UiCommand::SelectStyleColors
                           : UiCommand::SelectStyleFonts;
            result.arg = entry.name;
          }
          if (tab == 0) style_editor_sheet_ = entry.name;
        }
        const float right = ImGui::GetWindowContentRegionMax().x +
                            ImGui::GetWindowPos().x;
        if (tab == 1) {
          // Paper chip with the text and link colour as two bars on it.
          const float w = row_h * 1.6f;
          const ImVec2 a(right - w, row.y + 1), b(right, row.y + row_h - 1);
          auto col = [](const Rgba& c) { return IM_COL32(c.r, c.g, c.b, 255); };
          draw->AddRectFilled(a, b, col(entry.paper), 2.f);
          draw->AddRect(a, b, IM_COL32(0, 0, 0, 90), 2.f);
          const float bar = (b.y - a.y) * 0.22f;
          draw->AddRectFilled(ImVec2(a.x + 3, a.y + bar), ImVec2(b.x - 3, a.y + 2 * bar),
                              col(entry.ink));
          draw->AddRectFilled(ImVec2(a.x + 3, b.y - 2 * bar),
                              ImVec2(a.x + (b.x - a.x) * 0.6f, b.y - bar),
                              col(entry.accent));
        } else if (tab == 2) {
          const std::string face =
              entry.body_font.empty() ? std::string("default") : entry.body_font;
          const float tw = ImGui::CalcTextSize(face.c_str()).x;
          const float title_end =
              row.x + ImGui::CalcTextSize(label.c_str()).x + row_h;
          if (right - tw > title_end)  // else only in the tooltip
            draw->AddText(ImVec2(right - tw, row.y),
                        ImGui::GetColorU32(ImGuiCol_TextDisabled), face.c_str());
        }
        if (ImGui::IsItemHovered()) {
          if (tab == 2)
            ImGui::SetTooltip("%s: body %s, headings %s", entry.name.c_str(),
                              entry.body_font.empty() ? "default" : entry.body_font.c_str(),
                              entry.heading_font.empty()
                                  ? (entry.body_font.empty() ? "default" : "as body")
                                  : entry.heading_font.c_str());
          else
            ImGui::SetTooltip("%s%s", entry.name.c_str(),
                              active ? " (currently applied)" : "");
        }
        ImGui::PopID();
      }
      ImGui::Separator();
      {
        std::string mix = title_of(state.style);
        if (!state.style_colors.empty())
          mix += " · " + title_of(state.style_colors) + " colours";
        if (!state.style_fonts.empty())
          mix += " · " + title_of(state.style_fonts) + " fonts";
        ImGui::TextWrapped("%s", mix.c_str());
      }
      ImGui::Separator();
      if (classic_button("Reload")) cmd = UiCommand::ReloadStyles;
      ImGui::TextDisabled("Default: %s", state.default_style.c_str());
      ImGui::Spacing();
      if (classic_button("Manage style sheets…"))
        cmd = UiCommand::ToggleStyleManagerPanel;
    }
    ImGui::End();
  }

  // ---- Style sheet manager (create / edit / fonts / pins) ----
  if (state.styles_manager_open &&
      !(state.shaded_panels & ui_panel_bit(UiPanelLayer::StyleManager))) {
    if (state.style_save_generation != style_save_seen_) {
      style_save_seen_ = state.style_save_generation;
      style_editor_dirty_ = false;
      style_editor_loaded_.clear();
    }
    constexpr ImGuiWindowFlags mgr_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoCollapse;
    if (begin_panel(state, UiPanelLayer::StyleManager, "##style_manager_body",
                    mgr_flags, state.styles_manager_x, state.styles_manager_y,
                    state.styles_manager_w, state.styles_manager_h)) {
      ImGui::TextWrapped(
          "Create personal themes, edit source, fonts, and automatic assignments");
      ImGui::Separator();
      const float available_width = ImGui::GetContentRegionAvail().x;
      const bool narrow_panel = available_width < 480.f;
      const float list_width = narrow_panel
          ? available_width
          : std::clamp(available_width * 0.28f, 130.f, 220.f);
      begin_panel_list("##style_manager_list",
                       ImVec2(list_width, narrow_panel ? 120.f : 0),
                       ImGuiChildFlags_Borders);
      ImGui::TextDisabled("Edit theme");
      ImGui::Separator();
      if (style_editor_sheet_.empty()) style_editor_sheet_ = state.style;
      for (const StyleEntry& entry : state.style_entries) {
        ImGui::PushID(("mgr_" + entry.name).c_str());
        const bool selected = style_editor_sheet_ == entry.name;
        const bool active = state.style == entry.name;
        std::string label = (active ? "● " : "  ") + entry.title;
        if (ImGui::Selectable(label.c_str(), selected)) {
          if (!style_editor_dirty_) {
            style_editor_sheet_ = entry.name;
            style_editor_[0] = '\0';
            style_delete_confirm_ = false;
          }
        }
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("%s%s", entry.name.c_str(),
                            active ? " (currently applied)" : "");
        ImGui::PopID();
      }
      if (style_editor_dirty_)
        ImGui::TextWrapped("Save or discard edits before switching themes.");
      ImGui::EndChild();
      if (!narrow_panel) ImGui::SameLine();
      ImGui::BeginGroup();
      const StyleEntry* chosen = nullptr;
      for (const StyleEntry& entry : state.style_entries)
        if (entry.name == style_editor_sheet_) { chosen = &entry; break; }
      if (!chosen && !state.style_entries.empty()) {
        style_editor_sheet_ = state.style;
        for (const StyleEntry& entry : state.style_entries)
          if (entry.name == style_editor_sheet_) { chosen = &entry; break; }
        if (!chosen) {
          chosen = &state.style_entries.front();
          style_editor_sheet_ = chosen->name;
        }
      }
      if (chosen) {
        const fs::path user_styles = fs::path(user_data_dir()) / "styles";
        std::error_code ec;
        const bool writable = !chosen->path.empty() &&
            fs::weakly_canonical(fs::path(chosen->path).parent_path(), ec) ==
            fs::weakly_canonical(user_styles, ec);
        // Editor changes are held in the ImGui state until explicitly saved;
        // switching themes is disabled while there are unsaved changes.
        if (style_editor_sheet_ != style_editor_loaded_) {
          style_editor_loaded_ = style_editor_sheet_;
          std::ifstream file(chosen->path);
          std::ostringstream contents;
          if (file) contents << file.rdbuf();
          const std::string source = contents.str();
          std::snprintf(style_editor_, sizeof(style_editor_), "%s", source.c_str());
          style_editor_dirty_ = false;
          DocStyle resolved;
          if (load_style(chosen->name, resolved)) {
            auto first = [](const std::vector<std::string>& names,
                            const char* fallback) {
              return names.empty() ? std::string(fallback) : names.front();
            };
            std::snprintf(style_body_font_, sizeof(style_body_font_), "%s",
                          first(resolved.fonts.body, "sans-serif").c_str());
            std::snprintf(style_heading_font_, sizeof(style_heading_font_), "%s",
                          first(resolved.fonts.heading, "sans-serif").c_str());
            std::snprintf(style_code_font_, sizeof(style_code_font_), "%s",
                          first(resolved.fonts.code, "monospace").c_str());
          }
        }
        ImGui::Text("%s", chosen->title.c_str());
        ImGui::SeparatorText("Create / duplicate");
        ImGui::BeginDisabled(style_editor_dirty_);
        ImGui::SetNextItemWidth(narrow_panel ? -FLT_MIN :
            std::min(190.f, ImGui::GetContentRegionAvail().x * 0.60f));
        ImGui::InputTextWithHint("##new_style", "my-theme", style_new_name_,
                                 sizeof(style_new_name_));
        same_line_if_fits("Create from selected");
        if (classic_button("Create from selected")) {
          std::string name = style_new_name_;
          if (name.empty()) name = chosen->name + "-custom";
          cmd = UiCommand::StyleCreate;
          result.arg = name + "\n" + chosen->name;
          style_editor_sheet_ = name;
          style_editor_loaded_.clear();
          style_editor_dirty_ = false;
        }
        ImGui::EndDisabled();
        ImGui::SeparatorText("Source editor");
        if (writable) {
          ImGui::TextDisabled("%s", chosen->path.c_str());
          if (ImGui::InputTextMultiline("##style_source", style_editor_,
                                        sizeof(style_editor_),
                                        ImVec2(-FLT_MIN, std::max(115.f, ImGui::GetContentRegionAvail().y * 0.26f)),
                                        ImGuiInputTextFlags_AllowTabInput))
            style_editor_dirty_ = true;
          if (classic_button(style_editor_dirty_ ? "Save changes *" : "Save")) {
            cmd = UiCommand::StyleSave;
            result.arg = chosen->name + "\n" + style_editor_;
          }
          same_line_if_fits("Discard changes");
          if (classic_button("Discard changes")) {
            style_editor_dirty_ = false;
            style_editor_loaded_.clear();
          }
          same_line_if_fits(style_delete_confirm_ ? "Confirm delete" : "Delete...");
          if (style_delete_confirm_) {
            if (classic_button("Confirm delete")) {
              cmd = UiCommand::StyleDelete;
              result.arg = chosen->name;
              style_editor_sheet_ = state.default_style;
              style_editor_loaded_.clear();
              style_editor_dirty_ = false;
              style_delete_confirm_ = false;
            }
          } else if (classic_button("Delete...")) {
            style_delete_confirm_ = true;
          }
        } else {
          ImGui::TextWrapped("Bundled preset (read-only). Create a personal copy above to edit it.");
          ImGui::TextDisabled("Your files are saved under XDG_DATA_HOME/xanatoo/styles.");
        }
        ImGui::SeparatorText("Font selection");
        if (writable) {
          // List the actual installed FontConfig families, not a guessed
          // handful of presets that might silently resolve to other faces.
          const auto& faces = fontconfig_list_families();
          ImGui::SetNextItemWidth(-FLT_MIN);
          ImGui::InputTextWithHint("##font_filter", "Search fonts...",
                                   style_font_filter_, sizeof(style_font_filter_));
          auto font_field = [&](const char* label, char* value, size_t cap) {
            ImGui::PushID(label);
            // InputText renders a visible label *after* the input. With a
            // full-width field that label ends up outside the panel, so put
            // it on its own line and use hidden IDs for both controls.
            ImGui::TextUnformatted(label);
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputText("##family", value, cap);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##installed_fonts", "Browse fonts...")) {
              std::string filter(style_font_filter_);
              std::transform(filter.begin(), filter.end(), filter.begin(),
                             [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
              for (const std::string& face : faces) {
                if (!filter.empty()) {
                  std::string lower = face;
                  std::transform(lower.begin(), lower.end(), lower.begin(),
                                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                  if (lower.find(filter) == std::string::npos) continue;
                }
                if (ImGui::Selectable(face.c_str(), std::string_view(value) == face))
                  std::snprintf(value, cap, "%s", face.c_str());
                if (ImGui::IsItemHovered())
                  ImGui::SetTooltip("%s", face.c_str());
              }
              ImGui::EndCombo();
            }
            ImGui::PopID();
          };
          font_field("Body", style_body_font_, sizeof(style_body_font_));
          font_field("Headings", style_heading_font_, sizeof(style_heading_font_));
          font_field("Code", style_code_font_, sizeof(style_code_font_));
          if (classic_button("Insert font overrides")) {
            auto quoted = [](const char* in) {
              std::string safe;
              for (char c : std::string_view(in)) {
                if (c == '"' || c == '\\') safe.push_back('\\');
                if (c != '\n' && c != '\r') safe.push_back(c);
              }
              return "\"" + safe + "\"";
            };
            const std::string rule =
                "\n(fonts (body " + quoted(style_body_font_) + ")" +
                " (heading " + quoted(style_heading_font_) + ")" +
                " (code " + quoted(style_code_font_) + "))\n";
            std::string source(style_editor_);
            // Parenthesized sheets must keep the extra rule inside the
            // (stylesheet ...) wrapper. Our new templates are unwrapped.
            const auto forms = SExprParser(source).parse_all();
            if (forms.size() == 1 && is_form(forms.front(), "stylesheet")) {
              const auto close = source.find_last_of(')');
              if (close != std::string::npos) source.insert(close, rule);
            } else source += rule;
            if (source.size() < sizeof(style_editor_)) {
              std::snprintf(style_editor_, sizeof(style_editor_), "%s", source.c_str());
              style_editor_dirty_ = true;
            }
          }
        } else {
          ImGui::TextDisabled("Duplicate this preset to customize its fonts.");
        }
        ImGui::SeparatorText("Automatic styles");
        for (const std::string& key : state.style_pin_candidates) {
          ImGui::PushID(key.c_str());
          const auto it = state.style_pins.find(key);
          ImGui::TextWrapped("%s: %s", key.c_str(),
                            it == state.style_pins.end() ? "(not assigned)" : it->second.c_str());
          same_line_if_fits("Pin here");
          if (classic_button("Pin here")) {
            cmd = UiCommand::StylePin;
            result.arg = key + "\n" + chosen->name;
          }
          if (it != state.style_pins.end()) {
            same_line_if_fits("Unpin");
            if (classic_button("Unpin")) {
              cmd = UiCommand::StyleUnpin;
              result.arg = key;
            }
          }
          ImGui::PopID();
        }
        if (state.style_pin_candidates.empty())
          ImGui::TextDisabled("Open a web domain or service to assign its style.");
        ImGui::SetNextItemWidth(narrow_panel ? -FLT_MIN : -105.f);
        ImGui::InputTextWithHint("##manual_style_pin", "domain:example.org or service:lemmy",
                                 style_pin_input_, sizeof(style_pin_input_));
        same_line_if_fits("Pin target");
        if (classic_button("Pin target")) {
          cmd = UiCommand::StylePin;
          result.arg = std::string(style_pin_input_) + "\n" + chosen->name;
        }
        if (ImGui::TreeNode("All saved assignments")) {
          for (const auto& [key, name] : state.style_pins) {
            ImGui::PushID(key.c_str());
            ImGui::Text("%s = %s", key.c_str(), name.c_str());
            same_line_if_fits("Remove");
            if (classic_button("Remove")) {
              cmd = UiCommand::StyleUnpin;
              result.arg = key;
            }
            ImGui::PopID();
          }
          ImGui::TreePop();
        }
      }
      ImGui::EndGroup();
    }
    ImGui::End();
  }
  if (!state.styles_open && !state.styles_manager_open) {
    style_editor_sheet_.clear();
    style_editor_loaded_.clear();
    style_editor_dirty_ = false;
  }

  // ---- Bookmarks manager panel ----
  if (state.bookmarks_open &&
      !(state.shaded_panels & ui_panel_bit(UiPanelLayer::Bookmarks))) {
    // Like Reply / Annotations, this is an editor *inside* the native
    // FloatWindow. It must never have an independent ImGui title or geometry.
    constexpr ImGuiWindowFlags bookmarks_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoCollapse;
    if (begin_panel(state, UiPanelLayer::Bookmarks, "##bookmarks_panel_body",
                    bookmarks_flags, state.bookmarks_x, state.bookmarks_y,
                    state.bookmarks_w, state.bookmarks_h))
      bookmarks_panel(state, cmd, result.arg);
    ImGui::End();
  }


  // ---- Context / transclusion source ----
  // When a real Document is loaded, draw_context_window renders it natively.
  if (state.context_open && !state.context_has_doc &&
      !(state.shaded_panels & ui_panel_bit(UiPanelLayer::Context))) {
    constexpr ImGuiWindowFlags ctx_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoCollapse;
    if (begin_panel(state, UiPanelLayer::Context, "##context_panel_body",
                    ctx_flags, state.context_x, state.context_y,
                    state.context_w, state.context_h)) {
      if (!state.context_kind.empty())
        ImGui::TextUnformatted(state.context_kind.c_str());
      else
        ImGui::TextDisabled("Context");
      if (!state.context_title.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", state.context_title.c_str());
      }
      ImGui::Separator();
      begin_panel_list("##context_body", ImVec2(0, -28), false,
                        ImGuiWindowFlags_HorizontalScrollbar);
      if (!state.context_body.empty())
        ImGui::TextWrapped("%s", state.context_body.c_str());
      else
        ImGui::TextDisabled("Nothing under the pointer.");
      if (!state.context_meta.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", state.context_meta.c_str());
      }
      ImGui::EndChild();
      if (state.context_can_jump) {
        if (ImGui::Button("Jump to source"))
          cmd = UiCommand::ContextJump;
      }
    }
    ImGui::End();
  }

  // ---- Subscriptions: changing remote feeds (distinct from static bookmarks) ----
  if (state.subscriptions_open &&
      !(state.shaded_panels & ui_panel_bit(UiPanelLayer::Subscriptions))) {
    constexpr ImGuiWindowFlags subscriptions_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoCollapse;
    if (begin_panel(state, UiPanelLayer::Subscriptions, "##subscriptions_panel_body",
                    subscriptions_flags, state.subscriptions_x, state.subscriptions_y,
                    state.subscriptions_w, state.subscriptions_h)) {
      int unread_total = 0;
      for (const auto& s : state.subscriptions)
        unread_total += std::min<uint32_t>(s.unread, 9999);
      ImGui::Text("%zu watched sources", state.subscriptions.size());
      if (unread_total > 0) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.18f, 0.40f, 0.75f, 1.f),
                           "  %d new", unread_total);
      }
      ImGui::Separator();
      if (state.current_subscribable && !state.current_subscribed &&
          classic_button("Subscribe to current page")) {
        cmd = UiCommand::Subscribe;
        result.arg = state.subscription_address;
      }
      if (state.current_subscribed && classic_button("Unsubscribe current page")) {
        cmd = UiCommand::Unsubscribe;
        result.arg = state.subscription_address;
      }
      ImGui::TextUnformatted("Source address");
      ImGui::SetNextItemWidth(-1.f);
      ImGui::InputText("##subscription_url", subscription_input_,
                       sizeof(subscription_input_));
      if (classic_button("Add subscription")) {
        cmd = UiCommand::Subscribe;
        result.arg = subscription_input_;
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("For example: lemmy:lemmy.world/c/technology,\n"
                          "nostr:npub1..., or https://example.org/news");
      same_line_if_fits("Refresh all");
      if (classic_button("Refresh all")) cmd = UiCommand::SubscriptionRefresh;
      ImGui::Separator();
      if (state.subscriptions.empty()) {
        ImGui::TextWrapped("No subscriptions yet.");
      }
      for (const auto& s : state.subscriptions) {
        ImGui::PushID(s.address.c_str());
        const std::string name = s.title.empty() ? s.address : s.title;
        if (s.unread) {
          ImGui::TextColored(ImVec4(0.17f, 0.41f, 0.76f, 1.f),
                             "● %u new", s.unread);
          ImGui::SameLine();
        }
        ImGui::TextWrapped("%s", name.c_str());
        if (name != s.address) {
          ImGui::PushStyleColor(ImGuiCol_Text,
                                ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
          ImGui::TextWrapped("%s", s.address.c_str());
          ImGui::PopStyleColor();
        }
        if (s.pending) {
          ImGui::TextDisabled("Checking for new content…");
        } else if (!s.error.empty()) {
          ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.74f, 0.27f, 0.14f, 1.f));
          ImGui::TextWrapped("Check failed: %s", s.error.c_str());
          ImGui::PopStyleColor();
        } else if (!s.initialized) {
          ImGui::TextDisabled("Waiting for first successful check");
        } else {
          const auto now = static_cast<int64_t>(std::time(nullptr));
          const int64_t mins = std::max<int64_t>(0, (now - s.checked_at) / 60);
          if (mins < 60) ImGui::TextDisabled("Last checked %lld min ago",
                                            static_cast<long long>(mins));
          else ImGui::TextDisabled("Last checked %lld h ago",
                                   static_cast<long long>(mins / 60));
        }
        if (classic_button("Open")) {
          cmd = UiCommand::SubscriptionOpen;
          result.arg = s.address;
        }
        same_line_if_fits("Refresh");
        if (classic_button("Refresh")) {
          cmd = UiCommand::SubscriptionRefresh;
          result.arg = s.address;
        }
        if (s.unread) {
          same_line_if_fits("Mark read");
          if (classic_button("Mark read")) {
            cmd = UiCommand::SubscriptionMarkRead;
            result.arg = s.address;
          }
        }
        same_line_if_fits("Remove…");
        if (subscription_remove_confirm_ == s.address) {
          if (classic_button("Confirm removal")) {
            cmd = UiCommand::Unsubscribe;
            result.arg = s.address;
            subscription_remove_confirm_.clear();
          }
        } else if (classic_button("Remove…")) {
          subscription_remove_confirm_ = s.address;
        }
        auto frequency_label = [](int minutes) {
          if (minutes >= 60)
            return "Every " + std::to_string(minutes / 60) + " h";
          return "Every " + std::to_string(minutes) + " min";
        };
        ImGui::SetNextItemWidth(-1.f);
        std::string frequency = frequency_label(s.interval_minutes);
        if (ImGui::BeginCombo("##subscription_frequency", frequency.c_str())) {
          for (int mins : kSubscriptionIntervals) {
            std::string option = frequency_label(mins);
            if (ImGui::Selectable(option.c_str(), mins == s.interval_minutes)) {
              cmd = UiCommand::SubscriptionInterval;
              result.arg = s.address + "\n" + std::to_string(mins);
            }
          }
          ImGui::EndCombo();
        }
        ImGui::Separator();
        ImGui::PopID();
      }
    }
    ImGui::End();
  }

  // ---- Independent annotation layers (source documents remain untouched) ----
  if (state.annotations_panel_open &&
      !(state.shaded_panels & ui_panel_bit(UiPanelLayer::Annotations))) {
    if (annotation_note_created_seen_ != state.annotation_note_created_gen) {
      annotation_note_created_seen_ = state.annotation_note_created_gen;
      annotation_new_note_[0] = '\0';
    }
    // The title, close/maximize buttons, docking, resize and stacking belong
    // to FloatWindow::Annotations. ImGui provides only the form controls in
    // that native panel's client area (as in the Reply composer).
    constexpr ImGuiWindowFlags annotation_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoCollapse;
    if (begin_panel(state, UiPanelLayer::Annotations, "##annotation_panel_body",
                    annotation_flags, state.annotation_x, state.annotation_y,
                    state.annotation_w, state.annotation_h)) {
      if (classic_button("New personal layer"))
        cmd = UiCommand::NewAnnotationLayer;
      ImGui::SameLine();
      if (classic_button("Highlight selection"))
        cmd = UiCommand::AddAnnotationHighlight;
      if (!state.selection && ImGui::IsItemHovered())
        ImGui::SetTooltip("Select text in a document first");
      ImGui::Separator();
      if (state.annotation_layers.empty()) {
        ImGui::TextWrapped("No annotation layers yet. Create one to start highlighting.");
      }
      for (const auto& layer : state.annotation_layers) {
        ImGui::PushID(layer.id.c_str());
        bool visible = layer.visible;
        if (ImGui::Checkbox("##visible", &visible)) {
          cmd = UiCommand::ToggleAnnotationLayer;
          result.arg = layer.id;
        }
        ImGui::SameLine();
        const std::string label = layer.name + " (" + layer.owner + ", " +
                                  std::to_string(layer.count) + ")";
        if (layer.owner == "local") {
          if (ImGui::RadioButton(label.c_str(), layer.active)) {
            cmd = UiCommand::SelectAnnotationLayer;
            result.arg = layer.id;
          }
        } else {
          ImGui::TextUnformatted(label.c_str());
        }
        ImGui::PopID();
      }
      ImGui::TextDisabled("Checkbox: visibility  ·  radio: local edit target");
      ImGui::Separator();
      ImGui::TextUnformatted("Attach a note to selected text");
      ImGui::InputTextMultiline("##new_annotation_note", annotation_new_note_,
                                sizeof(annotation_new_note_), ImVec2(-1.f, 66.f));
      ImGui::BeginDisabled(!state.selection || annotation_new_note_[0] == '\0');
      if (classic_button("Attach note")) {
        cmd = UiCommand::AddAnnotationNote;
        result.arg = annotation_new_note_;
      }
      ImGui::EndDisabled();
      if (!state.selection) {
        ImGui::SameLine();
        ImGui::TextDisabled("Select a passage first");
      }
      ImGui::Separator();
      ImGui::TextUnformatted("Annotations on this page");
      if (state.annotations.empty())
        ImGui::TextDisabled("None in visible layers");
      const UiState::AnnotationItem* selected = nullptr;
      for (const auto& item : state.annotations) {
        ImGui::PushID(item.id.c_str());
        const char* label = item.kind == "note" ? "Note" : "Highlight";
        if (ImGui::Selectable(label, item.selected)) {
          cmd = UiCommand::SelectAnnotation;
          result.arg = item.id;
        }
        if (item.selected) selected = &item;
        ImGui::SameLine();
        ImGui::TextDisabled("%s%s", item.owner.c_str(),
                            item.resolved ? "" : " (anchor not found)");
        if (!item.body.empty()) {
          ImGui::Indent();
          ImGui::TextWrapped("%s", item.body.c_str());
          ImGui::Unindent();
        }
        ImGui::PopID();
      }
      const std::string selected_id = selected ? selected->id : std::string();
      const std::string selected_body = selected ? selected->body : std::string();
      if (selected_id != annotation_edit_synced_id_ ||
          selected_body != annotation_edit_synced_body_) {
        annotation_edit_synced_id_ = selected_id;
        annotation_edit_synced_body_ = selected_body;
        std::snprintf(annotation_edit_note_, sizeof(annotation_edit_note_),
                      "%s", selected_body.c_str());
      }
      bool pending_note_changes = false;
      if (selected) {
        ImGui::Separator();
        ImGui::Text("Selected: %s", selected->kind.c_str());
        if (selected->owner == "local") {
          if (selected->kind == "note") {
            const bool too_long = selected->body.size() >=
                                  sizeof(annotation_edit_note_) ||
                                  selected->body.find('\0') != std::string::npos;
            const bool changed = ImGui::InputTextMultiline(
                "##edit_annotation_note", annotation_edit_note_,
                sizeof(annotation_edit_note_), ImVec2(-1.f, 88.f));
            if (changed) annotation_note_last_edit_ = ImGui::GetTime();
            const bool pending = selected->body != annotation_edit_note_;
            pending_note_changes = pending;
            // Commit after typing pauses or the editor loses focus. Each
            // commit creates a layer revision, never a source document edit.
            if (!too_long && pending &&
                (ImGui::IsItemDeactivatedAfterEdit() ||
                 (annotation_note_last_edit_ > 0.0 &&
                  ImGui::GetTime() - annotation_note_last_edit_ > 0.8))) {
              cmd = UiCommand::UpdateAnnotationNote;
              result.arg = annotation_edit_note_;
              annotation_note_last_edit_ = 0.0;
            }
            if (too_long) ImGui::TextDisabled("Note too long to edit here");
            ImGui::BeginDisabled(too_long || !pending);
            if (classic_button("Save note")) {
              cmd = UiCommand::UpdateAnnotationNote;
              result.arg = annotation_edit_note_;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
          }
          if (classic_button("Remove annotation")) {
            cmd = UiCommand::RemoveAnnotation;
            result.arg = selected->id;
          }
          ImGui::TextDisabled("Previous versions remain in layer history");
        } else {
          ImGui::TextDisabled("Other people's layers are read-only");
        }
      }
      if (!state.annotation_history_layer.empty() &&
          ImGui::CollapsingHeader("Local layer history")) {
        ImGui::TextDisabled("Restore creates a new version; it does not erase history");
        if (pending_note_changes)
          ImGui::TextDisabled("Finish saving the note before restoring a version");
        for (size_t i = 0; i < state.annotation_history.size(); ++i) {
          const auto& revision = state.annotation_history[i];
          ImGui::PushID(revision.id.c_str());
          const std::time_t seconds = static_cast<std::time_t>(revision.time);
          char when[40] = {};
          if (const std::tm* local = std::localtime(&seconds))
            std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", local);
          ImGui::Text("%s · %zu annotations · %s",
                      i == 0 ? "Current" : "Earlier",
                      revision.count, when);
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", revision.id.c_str());
          if (i != 0) {
            ImGui::SameLine();
            ImGui::BeginDisabled(pending_note_changes);
            if (classic_button("Restore")) {
              cmd = UiCommand::RestoreAnnotationLayerVersion;
              result.arg = state.annotation_history_layer + "\n" + revision.id;
            }
            ImGui::EndDisabled();
          }
          ImGui::PopID();
        }
      }
    }
    ImGui::End();
  }

  // ---- Nostr catalogue, embedded in a native FloatWindow ----
  // Geometry, title, stacking, docking and clipping belong to Xanatoo.
  // ImGui supplies only the searchable list controls within its client well.
  constexpr ImGuiWindowFlags embedded_panel_flags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoCollapse;
  if (state.nostr_explorer_open &&
      !(state.shaded_panels & ui_panel_bit(UiPanelLayer::NostrExplorer))) {
    if (begin_panel(state, UiPanelLayer::NostrExplorer, "##nostr_explorer_body",
                    embedded_panel_flags, state.nostr_explorer_x, state.nostr_explorer_y,
                    state.nostr_explorer_w, state.nostr_explorer_h)) {
      ImGui::Text("Observed: %lld events, %lld notes, %lld people",
                  static_cast<long long>(state.nostr_stats.events),
                  static_cast<long long>(state.nostr_stats.notes),
                  static_cast<long long>(state.nostr_stats.pubkeys));
      ImGui::TextDisabled("%lld profiles, %lld relays (local sample)",
                          static_cast<long long>(state.nostr_stats.profiles),
                          static_cast<long long>(state.nostr_stats.relays));
      ImGui::TextDisabled("%lld signed, %lld hash-only, %lld legacy unverified",
                          static_cast<long long>(state.nostr_stats.signed_events),
                          static_cast<long long>(state.nostr_stats.events -
                                                 state.nostr_stats.signed_events),
                          static_cast<long long>(state.nostr_stats.legacy_events));
      ImGui::TextDisabled("%lld relay-scoped NIP-29 groups observed",
                          static_cast<long long>(state.nostr_stats.groups));
      ImGui::SetNextItemWidth(-80.f * scale_);
      if (ImGui::InputText("##nostr_search", nostr_local_search_,
                           sizeof nostr_local_search_)) {
        cmd = UiCommand::NostrSearchLocal;
        result.arg = nostr_local_search_;
      }
      ImGui::SameLine();
      if (classic_button("Refresh")) cmd = UiCommand::NostrRefreshLocal;
      if (ImGui::RadioButton("People", state.nostr_tab == 0))
        cmd = UiCommand::NostrShowPeople;
      ImGui::SameLine();
      if (ImGui::RadioButton("Events", state.nostr_tab == 1))
        cmd = UiCommand::NostrShowEvents;
      ImGui::SameLine();
      if (ImGui::RadioButton("Groups", state.nostr_tab == 2))
        cmd = UiCommand::NostrShowGroups;
      if (state.nostr_tab == 2) {
        ImGui::TextDisabled("Discover automatically, or specify a group relay:");
        ImGui::SetNextItemWidth(-95.f * scale_);
        ImGui::InputTextWithHint("##nostr_group_relay", "relay host or wss:// URL (optional)",
                                 nostr_group_relay_, sizeof nostr_group_relay_);
        ImGui::SameLine();
        if (classic_button("Discover")) {
          cmd = UiCommand::NavigateUrl;
          result.arg = "nostr:groups";
          if (nostr_group_relay_[0] != '\0') {
            result.arg += "@";
            result.arg += nostr_group_relay_;
          }
        }
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Empty: scan known NIP-29 hosts and relays from public group lists.\n"
                            "A hostname is enough for a specific relay (TLS is always used).");
      } else if (classic_button("Fetch recent")) {
        cmd = UiCommand::NavigateUrl;
        result.arg = "nostr:recent";
      }
      ImGui::BeginDisabled(state.nostr_page == 0);
      if (classic_button("Previous")) cmd = UiCommand::NostrPrevPage;
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::Text("Page %d", state.nostr_page + 1);
      ImGui::SameLine();
      ImGui::BeginDisabled(!(state.nostr_tab == 0 ? state.nostr_people_more :
                            state.nostr_tab == 1 ? state.nostr_events_more :
                                                   state.nostr_groups_more));
      if (classic_button("Next")) cmd = UiCommand::NostrNextPage;
      ImGui::EndDisabled();
      ImGui::Separator();
      // A child scroll area avoids producing thousands of text widgets when
      // an explorer panel is shrunk or docked beneath another panel.
      if (begin_panel_list("##nostr_results", ImVec2(0.f, 0.f), false)) {
        if (state.nostr_tab == 0) {
          if (state.nostr_people.empty())
            ImGui::TextWrapped("No matching public keys cached yet. Fetch recent notes, or open an npub1 / note1 address.");
          for (const auto& person : state.nostr_people) {
            ImGui::PushID(person.pubkey.c_str());
            const std::string label = (person.name.empty() ?
                person.pubkey.substr(0, 14) + "…" : person.name) +
                "  ·  " + std::to_string(person.post_count) + " notes" +
                (person.profile_verification == 2 ? " [sig]" : "");
            if (ImGui::Selectable(label.c_str())) {
              cmd = UiCommand::NavigateUrl;
              result.arg = "nostr:" + nostr_bech32("npub", person.pubkey);
            }
            middle_open(cmd, result.arg, "nostr:" + nostr_bech32("npub", person.pubkey));
            if (ImGui::BeginPopupContextItem("##nostr_person_sub")) {
              if (ImGui::MenuItem("Subscribe to author")) {
                cmd = UiCommand::Subscribe;
                result.arg = "nostr:" + nostr_bech32("npub", person.pubkey);
              }
              ImGui::EndPopup();
            }
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("%s\n%lld unique events observed\n%s\n%s",
                                person.pubkey.c_str(),
                                static_cast<long long>(person.event_count),
                                person.nip05.c_str(),
                                person.profile_verification == 2 ?
                                  "Profile signature checked" : "Profile not signature verified");
            ImGui::PopID();
          }
        } else if (state.nostr_tab == 1) {
          if (state.nostr_events.empty())
            ImGui::TextWrapped("No matching cached events. Only events downloaded by this client appear here.");
          for (const auto& ev : state.nostr_events) {
            ImGui::PushID(ev.id.c_str());
            const std::string name = ev.author_name.empty() ?
                ev.pubkey.substr(0, 12) + "…" : ev.author_name;
            std::string summary = ev.content;
            for (char& c : summary) if (c == '\n' || c == '\r') c = ' ';
            if (summary.size() > 110) summary = summary.substr(0, 110) + "…";
            const std::string label = std::string(ev.verification == 2 ? "[sig] " : "[hash] ") +
                "kind " + std::to_string(ev.kind) +
                " · " + name + " · " + summary;
            if (ImGui::Selectable(label.c_str(), false,
                                  (ev.kind == 1 || ev.kind == 9 || ev.kind == 11 || ev.kind == 1111) ?
                                    0 : ImGuiSelectableFlags_Disabled)) {
              cmd = UiCommand::NavigateUrl;
              result.arg = "nostr:" + nostr_bech32("note", ev.id);
            }
            if (ev.kind == 1 || ev.kind == 9 || ev.kind == 11 || ev.kind == 1111)
              middle_open(cmd, result.arg, "nostr:" + nostr_bech32("note", ev.id));
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("%s\n%s", ev.id.c_str(), ev.pubkey.c_str());
            ImGui::PopID();
          }
        } else {
          if (state.nostr_groups.empty())
            ImGui::TextWrapped("No NIP-29 groups cached yet. Use Discover to scan known group relays, or enter a specific host.");
          for (const auto& group : state.nostr_groups) {
            ImGui::PushID(group.relay.c_str());
            ImGui::PushID(group.group_id.c_str());
            const std::string href = nostr_group_ref(group.relay, group.group_id);
            const std::string name = group.name.empty() ? group.group_id : group.name;
            const std::string label = (group.relay_master_verified ? "[relay] " :
                                       group.metadata_verification == 2 ? "[sig] " :
                                       group.metadata_verification == 1 ? "[hash] " : "[?] ") +
                name + " · " + std::to_string(group.message_count) +
                " messages · " + group.relay;
            if (ImGui::Selectable(label.c_str(), false,
                                  href.empty() ? ImGuiSelectableFlags_Disabled : 0)) {
              cmd = UiCommand::NavigateUrl;
              result.arg = href;
            }
            middle_open(cmd, result.arg, href);
            if (!href.empty() && ImGui::BeginPopupContextItem("##nostr_group_sub")) {
              if (ImGui::MenuItem("Subscribe to group")) {
                cmd = UiCommand::Subscribe;
                result.arg = href;
              }
              ImGui::EndPopup();
            }
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Group: %s\nRelay: %s\nParent: %s\n%s\n"
                                "%lld unique group events\n%s%s%s%s\n"
                                "%s",
                                group.group_id.c_str(), group.relay.c_str(),
                                group.parent_id.empty() ? "(root/unknown)" : group.parent_id.c_str(),
                                group.about.c_str(),
                                static_cast<long long>(group.event_count),
                                group.private_group ? "private " : "",
                                group.hidden ? "hidden " : "",
                                group.restricted ? "restricted " : "",
                                group.closed ? "closed" : "",
                                group.relay_master_verified ?
                                  "Relay master: event signature matches NIP-11 self (verified HTTPS)" :
                                group.metadata_verification == 2 ?
                                  "Event signature valid; relay master not authenticated" :
                                  "Relay master not authenticated (signature unverified)");
            ImGui::PopID();
            ImGui::PopID();
          }
        }
      }
      ImGui::EndChild();
    }
    ImGui::End();
  }

  // ---- Lemmy discovery catalogue, inside its native dockable panel ----
  if (state.lemmy_explorer_open &&
      !(state.shaded_panels & ui_panel_bit(UiPanelLayer::LemmyExplorer))) {
    if (begin_panel(state, UiPanelLayer::LemmyExplorer, "##lemmy_explorer_body",
                    embedded_panel_flags, state.lemmy_explorer_x, state.lemmy_explorer_y,
                    state.lemmy_explorer_w, state.lemmy_explorer_h)) {
      int counts[4] = {};
      for (const auto& entry : state.lemmy_entries)
        ++counts[static_cast<int>(entry.kind)];
      ImGui::TextDisabled("%d servers · %d communities · %d people · %d posts",
                          counts[0], counts[1], counts[2], counts[3]);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Public read-only catalogue stored locally; no login required");
      ImGui::SetNextItemWidth(-1.f);
      if (ImGui::BeginCombo("##lemmy_instance", state.lemmy_instance.c_str())) {
        for (const auto& entry : state.lemmy_entries) {
          if (entry.kind != LemmyEntryKind::Instance) continue;
          const bool selected = entry.name == state.lemmy_instance;
          if (ImGui::Selectable(entry.name.c_str(), selected)) {
            cmd = UiCommand::LemmySetInstance;
            result.arg = entry.name;
          }
          if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("API instance for Feed, Communities, Search and Peer discovery");

      ImGui::SetNextItemWidth(-100.f * scale_);
      if (ImGui::InputTextWithHint("##lemmy_filter", "Filter observed entries…",
                                   lemmy_search_input_, sizeof lemmy_search_input_)) {
        cmd = UiCommand::LemmySetSearch;
        result.arg = lemmy_search_input_;
      }
      ImGui::SameLine();
      bool only_this_server = state.lemmy_instance_only;
      if (ImGui::Checkbox("This server", &only_this_server))
        cmd = UiCommand::LemmyToggleInstanceOnly;
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Filter discovered communities, people and posts to the selected server");

      if (ImGui::RadioButton("Servers", state.lemmy_tab == 0)) {
        cmd = UiCommand::LemmySetTab; result.arg = "0";
      }
      ImGui::SameLine();
      if (ImGui::RadioButton("Communities", state.lemmy_tab == 1)) {
        cmd = UiCommand::LemmySetTab; result.arg = "1";
      }
      ImGui::SameLine();
      if (ImGui::RadioButton("People", state.lemmy_tab == 2)) {
        cmd = UiCommand::LemmySetTab; result.arg = "2";
      }
      ImGui::SameLine();
      if (ImGui::RadioButton("Posts", state.lemmy_tab == 3)) {
        cmd = UiCommand::LemmySetTab; result.arg = "3";
      }

      const std::string base = "lemmy:" + state.lemmy_instance + "/api/v3/";
      ImGui::BeginDisabled(!state.lemmy_supported);
      if (classic_button("Feed")) {
        cmd = UiCommand::NavigateUrl;
        result.arg = base + "post/list?limit=25&sort=" + state.lemmy_sort;
      }
      ImGui::SameLine();
      if (classic_button("Find communities")) {
        cmd = UiCommand::NavigateUrl;
        result.arg = base + "community/list?limit=50&sort=Active";
      }
      ImGui::SameLine();
      if (classic_button("Federated peers")) {
        cmd = UiCommand::NavigateUrl;
        result.arg = base + "federated_instances";
      }
      if (lemmy_search_input_[0] != '\0') {
        if (classic_button("Search server")) {
          cmd = UiCommand::NavigateUrl;
          result.arg = base + "search?q=" + lemmy_query_escape(lemmy_search_input_) +
                       "&type_=All&limit=50";
        }
        ImGui::SameLine();
      }
      ImGui::SetNextItemWidth(114.f * scale_);
      if (ImGui::BeginCombo("Feed sort", state.lemmy_sort.c_str())) {
        for (const char* option : {"Hot", "Active", "New", "TopDay", "TopWeek"}) {
          if (ImGui::Selectable(option, state.lemmy_sort == option)) {
            cmd = UiCommand::LemmySetSort;
            result.arg = option;
          }
        }
        ImGui::EndCombo();
      }
      ImGui::EndDisabled();

      ImGui::SetNextItemWidth(-1.f);
      ImGui::InputTextWithHint("##lemmy_add_instance", "Add server hostname or URL",
                               lemmy_instance_input_, sizeof lemmy_instance_input_);
      if (classic_button("Add instance")) {
        cmd = UiCommand::LemmyAddInstance;
        result.arg = lemmy_instance_input_;
      }
      ImGui::SameLine();
      if (classic_button("Directory")) {
        cmd = UiCommand::NavigateUrl;
        result.arg = "https://join-lemmy.org/instances";
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Official Lemmy server directory on join-lemmy.org");
      ImGui::Separator();

      if (begin_panel_list("##lemmy_results", ImVec2(0.f, 0.f), false)) {
        const LemmyEntryKind current = static_cast<LemmyEntryKind>(state.lemmy_tab);
        const std::string query = ascii_lower(state.lemmy_search);
        int matching = 0;
        for (const auto& item : state.lemmy_entries) {
          if (item.kind != current) continue;
          if (state.lemmy_instance_only && current != LemmyEntryKind::Instance &&
              item.instance != state.lemmy_instance) continue;
          if (!query.empty() &&
              ascii_lower(item.title).find(query) == std::string::npos &&
              ascii_lower(item.name).find(query) == std::string::npos &&
              ascii_lower(item.instance).find(query) == std::string::npos)
            continue;
          if (++matching > 250) continue; // bounded widget count for large catalogues
          ImGui::PushID(item.ref.c_str());
          std::string label = item.title;
          if (item.title != item.name) label += " · " + item.name;
          if (current != LemmyEntryKind::Instance)
            label += " · " + item.instance;
          if (item.score) {
            label += " · " + std::to_string(item.score);
            label += current == LemmyEntryKind::Community ? " subscribers" :
                     current == LemmyEntryKind::Person ? " posts" : " points";
          }
          const bool selected = current == LemmyEntryKind::Instance &&
                                item.name == state.lemmy_instance;
          if (ImGui::Selectable(label.c_str(), selected)) {
            if (current == LemmyEntryKind::Instance) {
              cmd = UiCommand::LemmySetInstance;
              result.arg = item.name;
            } else {
              cmd = UiCommand::NavigateUrl;
              result.arg = item.ref;
            }
          }
          if ((current == LemmyEntryKind::Community ||
               current == LemmyEntryKind::Person) &&
              ImGui::BeginPopupContextItem("##lemmy_sub")) {
            if (ImGui::MenuItem("Subscribe")) {
              cmd = UiCommand::Subscribe;
              result.arg = item.ref;
            }
            ImGui::EndPopup();
          }
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", current == LemmyEntryKind::Instance ?
                "Select this server for feed, community and search actions" :
                item.ref.c_str());
          ImGui::PopID();
        }
        if (matching == 0) {
          ImGui::TextWrapped(current == LemmyEntryKind::Instance ?
              "No matching instances. Add a server or discover federated peers." :
              "No cached matches yet. Browse a server feed, search it, or find communities to collect public names and posts.");
        } else if (matching > 250) {
          ImGui::TextDisabled("Showing first 250 of %d matches. Narrow the filter to find more.", matching);
        }
      }
      ImGui::EndChild();
    }
    ImGui::End();
  }

  // ---- Matrix catalogue, inside its native dockable panel ----
  if (state.matrix_explorer_open &&
      !(state.shaded_panels & ui_panel_bit(UiPanelLayer::MatrixExplorer))) {
    if (begin_panel(state, UiPanelLayer::MatrixExplorer, "##matrix_explorer_body",
                    embedded_panel_flags, state.matrix_explorer_x, state.matrix_explorer_y,
                    state.matrix_explorer_w, state.matrix_explorer_h)) {
      int counts[4] = {};
      for (const auto& entry : state.matrix_entries)
        ++counts[static_cast<int>(entry.kind)];
      ImGui::TextDisabled("%d servers · %d rooms · %d spaces · %d people",
                          counts[0], counts[1], counts[2], counts[3]);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Public names seen in Matrix responses, stored locally");

      ImGui::SetNextItemWidth(-1.f);
      if (ImGui::BeginCombo("##matrix_server", state.matrix_server.c_str())) {
        for (const auto& entry : state.matrix_entries) {
          if (entry.kind != MatrixEntryKind::Server) continue;
          const bool selected = entry.name == state.matrix_server;
          if (ImGui::Selectable(entry.name.c_str(), selected)) {
            cmd = UiCommand::MatrixSetServer;
            result.arg = entry.name;
          }
          if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Server whose public room directory Room directory opens");

      ImGui::BeginDisabled(!state.matrix_supported);
      MatrixRef dir;
      dir.kind = MatrixRefKind::Directory;
      dir.id = state.matrix_server;
      if (classic_button("Room directory")) {
        cmd = UiCommand::NavigateUrl;
        result.arg = format_matrix_ref(dir);
      }
      if (matrix_search_input_[0] != '\0') {
        ImGui::SameLine();
        if (classic_button("Search directory")) {
          dir.query = matrix_search_input_;
          cmd = UiCommand::NavigateUrl;
          result.arg = format_matrix_ref(dir);
        }
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip(state.matrix_has_token ?
              "Search this server's room directory for the filter words" :
              "Directory search needs an access token (Credentials)");
      }
      ImGui::EndDisabled();

      ImGui::SetNextItemWidth(-70.f * scale_);
      const bool open_enter = ImGui::InputTextWithHint(
          "##matrix_open", "#room:server, @user:server, matrix.to link or server",
          matrix_open_input_, sizeof matrix_open_input_,
          ImGuiInputTextFlags_EnterReturnsTrue);
      ImGui::SameLine();
      if ((classic_button("Open") || open_enter) && matrix_open_input_[0] != '\0') {
        std::string_view in = matrix_open_input_;
        trim_ascii(in);
        std::string ref;
        if (starts_with_ci(in, "matrix:")) ref = std::string(in);
        else if (!in.empty() && (in[0] == '#' || in[0] == '!' || in[0] == '@'))
          ref = matrix_ref_for_id(in);
        else ref = matrix_ref_from_url(in);
        if (!ref.empty()) {
          cmd = UiCommand::NavigateUrl;
          result.arg = ref;
        } else {
          cmd = UiCommand::MatrixAddServer;  // a bare server name
          result.arg = std::string(in);
        }
      }

      ImGui::SetNextItemWidth(-1.f);
      if (ImGui::InputTextWithHint("##matrix_filter", "Filter observed entries…",
                                   matrix_search_input_, sizeof matrix_search_input_)) {
        cmd = UiCommand::MatrixSetSearch;
        result.arg = matrix_search_input_;
      }
      static const char* const kTabs[] = {"Servers", "Rooms", "Spaces", "People"};
      for (int i = 0; i < 4; ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::RadioButton(kTabs[i], state.matrix_tab == i)) {
          cmd = UiCommand::MatrixSetTab;
          result.arg = std::to_string(i);
        }
      }
      if (!state.matrix_has_token) {
        ImGui::TextDisabled("Anonymous: rooms show their summary only.");
        ImGui::SameLine();
        if (classic_button("Credentials…")) cmd = UiCommand::ToggleCredentials;
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("An access token for your homeserver shows room "
                            "messages and space contents (read-only)");
      }
      ImGui::Separator();

      if (begin_panel_list("##matrix_results", ImVec2(0.f, 0.f), false)) {
        const MatrixEntryKind current = static_cast<MatrixEntryKind>(state.matrix_tab);
        const std::string query = ascii_lower(state.matrix_search);
        int matching = 0;
        for (const auto& item : state.matrix_entries) {
          if (item.kind != current) continue;
          if (!query.empty() &&
              ascii_lower(item.title).find(query) == std::string::npos &&
              ascii_lower(item.name).find(query) == std::string::npos &&
              ascii_lower(item.topic).find(query) == std::string::npos)
            continue;
          if (++matching > 250) continue;
          ImGui::PushID(item.ref.c_str());
          std::string label = item.title;
          if (item.title != item.name) label += " · " + item.name;
          if (item.members) label += " · " + std::to_string(item.members) + " members";
          const bool selected = current == MatrixEntryKind::Server &&
                                item.name == state.matrix_server;
          if (ImGui::Selectable(label.c_str(), selected)) {
            if (current == MatrixEntryKind::Server) {
              cmd = UiCommand::MatrixSetServer;
              result.arg = item.name;
            } else {
              cmd = UiCommand::NavigateUrl;
              result.arg = item.ref;
            }
          }
          if (current == MatrixEntryKind::Server && ImGui::IsItemHovered() &&
              ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            cmd = UiCommand::NavigateUrl;
            result.arg = item.ref;
          }
          if (ImGui::IsItemHovered()) {
            if (current == MatrixEntryKind::Server)
              ImGui::SetTooltip("Select for Room directory; double-click opens it");
            else if (!item.topic.empty() && ImGui::BeginTooltip()) {
              ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.f);
              ImGui::TextUnformatted(item.ref.c_str());
              ImGui::TextWrapped("%s", item.topic.c_str());
              ImGui::PopTextWrapPos();
              ImGui::EndTooltip();
            } else
              ImGui::SetTooltip("%s", item.ref.c_str());
          }
          ImGui::PopID();
        }
        if (matching == 0) {
          ImGui::TextWrapped(current == MatrixEntryKind::Server ?
              "No matching servers. Type a server name above and press Open." :
              "Nothing collected yet. Open a room directory, a room or a "
              "profile to collect public rooms, spaces and people.");
        } else if (matching > 250) {
          ImGui::TextDisabled("Showing first 250 of %d matches. Narrow the filter to find more.", matching);
        }
      }
      ImGui::EndChild();
    }
    ImGui::End();
  }

#if XANATOO_HTTP_COOKIES
  if (state.cookies_open && state.cookie_snapshot &&
      !(state.shaded_panels & ui_panel_bit(UiPanelLayer::Cookies))) {
    if (begin_panel(state, UiPanelLayer::Cookies, "##cookies_body",
                    embedded_panel_flags, state.cookies_x, state.cookies_y,
                    state.cookies_w, state.cookies_h)) {
      const auto& sites = state.cookie_snapshot->sites;
      const auto& cookies = state.cookie_snapshot->cookies;
      const auto& selected = state.cookie_selected;
      const auto site = std::ranges::find(sites, selected, &CookieSite::origin);
      const char* label = selected.empty() ? "No HTTPS page" : selected.c_str();
      if (ImGui::BeginCombo("Site", label)) {
        const std::string current = state.cookie_current.empty() ?
            "Current page (no HTTPS site)" : "Current page (" + state.cookie_current + ")";
        if (ImGui::Selectable(current.c_str())) {
          cmd = UiCommand::CookieSelect;
          result.arg.clear();
        }
        for (const auto& row : sites) {
          if (ImGui::Selectable(row.origin.c_str(), row.origin == selected)) {
            cmd = UiCommand::CookieSelect;
            result.arg = row.origin;
          }
        }
        ImGui::EndCombo();
      }
      if (state.cookies_read_only)
        ImGui::TextDisabled("Cookies are unavailable with --no-save.");
      if (!state.cookie_error.empty())
        ImGui::TextWrapped("%s", state.cookie_error.c_str());
      if (!selected.empty()) {
        const bool enabled = site != sites.end() && site->enabled;
        ImGui::BeginDisabled(state.cookies_read_only);
        if (ImGui::Button(enabled ? "Disable site cookies" : "Enable site cookies")) {
          cmd = UiCommand::CookieSiteToggle;
          result.arg = selected;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(state.cookie_reveal ? "Hide values" : "Reveal values"))
          cmd = UiCommand::CookieReveal;
        if (site != sites.end() && !site->last_names.empty())
          ImGui::TextWrapped("Last cookie-bearing request (%s): %s", site->last_path.c_str(),
                             site->last_names.c_str());
        else
          ImGui::TextDisabled("No cookies sent to this site this session.");
        if (state.cookie_reveal && selected == state.cookie_current)
          ImGui::TextWrapped("Cookie header for this page: %s",
              state.cookie_preview.empty() ? "(empty)" : state.cookie_preview.c_str());

        size_t count = 0;
        for (size_t i = 0; i < cookies.size(); ++i) {
          const auto& cookie = cookies[i];
          if (cookie.origin != selected) continue;
          ++count;
          ImGui::PushID(static_cast<int>(i));
          ImGui::Separator();
          ImGui::TextWrapped("%s  (%s)", cookie.name.c_str(), cookie.path.c_str());
          if (cookie.expires && cookie.expires <= std::time(nullptr))
            ImGui::TextDisabled("Expired");
          if (state.cookie_reveal)
            ImGui::TextWrapped("%s", cookie.value.empty() ? "(empty value)" : cookie.value.c_str());
          if (ImGui::Button(cookie.enabled ? "Disable" : "Enable")) {
            cmd = UiCommand::CookieToggle;
            result.arg = std::to_string(i);
          }
          ImGui::SameLine();
          if (ImGui::Button("Delete")) {
            cmd = UiCommand::CookieDelete;
            result.arg = std::to_string(i);
          }
          ImGui::PopID();
        }
        if (count == 0) ImGui::TextDisabled("No stored cookies.");
      }
    }
    ImGui::End();
  }
#endif

  // ---- Certificates: Gemini TOFU pins in a native dockable panel ----
  if (state.certs_open &&
      !(state.shaded_panels & ui_panel_bit(UiPanelLayer::Certificates))) {
    if (begin_panel(state, UiPanelLayer::Certificates, "##certificates_body",
                    embedded_panel_flags, state.certs_x, state.certs_y,
                    state.certs_w, state.certs_h)) {
      auto grouped = [](const std::string& hash) {
        std::string out;
        for (size_t i = 0; i < hash.size(); i += 8) {
          if (i) out += ' ';
          out += hash.substr(i, 8);
        }
        return out;
      };
      const ImVec4 warn(0.70f, 0.16f, 0.10f, 1.f);
      ImGui::TextWrapped("Gemini servers are trusted on the first visit. "
                         "You are only warned when a server later presents "
                         "a different certificate.");
      if (state.cert_no_save)
        ImGui::TextDisabled("--no-save: new trust lasts for this session only.");
      if (!state.cert_error.empty())
        ImGui::TextColored(warn, "%s", state.cert_error.c_str());
      const UiState::CertRow* sel = nullptr;
      for (const auto& row : state.cert_rows)
        if (row.key == state.cert_selected) sel = &row;
      if (state.cert_rows.empty()) {
        ImGui::TextDisabled("No Gemini servers known yet.");
      } else {
        const float list_h = sel ? std::max(80.f, ImGui::GetContentRegionAvail().y * 0.35f)
                                 : 0.f;
        if (begin_panel_list("##cert_list", ImVec2(0.f, list_h),
                              ImGuiChildFlags_Borders)) {
          for (const auto& row : state.cert_rows) {
            ImGui::PushID(row.key.c_str());
            std::string label = row.key;
            if (row.changed) label = "[!] " + label + "  CHANGED";
            else if (row.current) label += "  (this page)";
            const char* tag = row.changed ? "" :
                row.sha256.empty() ? "  not pinned" :
                row.first_contact ? "  new this session" :
                row.saved ? "" : "  session only";
            label += tag;
            if (row.changed) ImGui::PushStyleColor(ImGuiCol_Text, warn);
            if (ImGui::Selectable(label.c_str(), &row == sel) &&
                cmd == UiCommand::None) {
              cmd = UiCommand::CertSelect;
              result.arg = &row == sel ? std::string{} : row.key;
            }
            if (row.changed) ImGui::PopStyleColor();
            ImGui::PopID();
          }
        }
        ImGui::EndChild();
      }
      if (sel) {
        ImGui::Separator();
        ImGui::PushID(sel->key.c_str());
        auto field = [&](const char* label, const std::string& value) {
          ImGui::TextDisabled("%s", label);
          ImGui::Indent();
          ImGui::TextWrapped("%s", value.empty() ? "Unavailable" : value.c_str());
          ImGui::Unindent();
        };
        ImGui::TextUnformatted(sel->key.c_str());
        if (sel->changed) {
          ImGui::PushStyleColor(ImGuiCol_Text, warn);
          ImGui::TextWrapped("This server presented a different certificate "
                             "than before. Nothing was sent to it. Servers "
                             "renew certificates, but this can also mean that "
                             "someone intercepts the connection.");
          ImGui::PopStyleColor();
          field("Previously trusted SHA-256", grouped(sel->previous));
          field("Presented SHA-256", grouped(sel->presented));
          if (classic_button("Trust new certificate") && cmd == UiCommand::None) {
            cmd = UiCommand::CertAccept;
            result.arg = sel->key;
          }
          ImGui::SameLine();
          if (classic_button("Keep old certificate") && cmd == UiCommand::None) {
            cmd = UiCommand::CertReject;
            result.arg = sel->key;
          }
        } else if (!sel->sha256.empty()) {
          ImGui::TextWrapped("%s", sel->first_contact ?
              "Trusted automatically on the first visit in this session." :
              sel->saved ? "Trusted; the pin is saved." :
                           "Trusted for this session only.");
          field("Trusted SHA-256", grouped(sel->sha256));
        } else {
          ImGui::TextWrapped("Not pinned: the next visit trusts the "
                             "certificate the server presents then.");
        }
        if (sel->seen && !sel->subject.empty()) {
          field("Subject", sel->subject);
          field("Issuer", sel->issuer);
          field("Valid from", sel->not_before);
          field("Valid until", sel->not_after);
          field("Self-signed", sel->self_signed);
        } else if (!sel->seen) {
          ImGui::TextDisabled("Visit the server to see certificate details.");
        }
        const std::string& fp = sel->changed ? sel->presented : sel->sha256;
        if (!fp.empty() && classic_button("Copy fingerprint"))
          ImGui::SetClipboardText(fp.c_str());
        if (!sel->sha256.empty() || sel->changed) {
          ImGui::SameLine();
          if (classic_button("Forget") && cmd == UiCommand::None) {
            cmd = UiCommand::CertForget;
            result.arg = sel->key;
          }
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Remove the pin: the next visit trusts the "
                              "certificate the server presents then.");
        }
        ImGui::PopID();
      } else if (!state.cert_rows.empty()) {
        ImGui::TextDisabled("Select a server to see its certificate.");
      }
    }
    ImGui::End();
  }

  // ---- Identities: Gemini client certificates, Nostr accounts ----
  if (state.identities_open &&
      !(state.shaded_panels & ui_panel_bit(UiPanelLayer::Identities))) {
    if (begin_panel(state, UiPanelLayer::Identities, "##identities_body",
                    embedded_panel_flags, state.identities_x, state.identities_y,
                    state.identities_w, state.identities_h)) {
      const ImVec4 warn(0.70f, 0.16f, 0.10f, 1.f);
      const bool on_page = !state.identity_page.empty();
      auto pick = [&](UiCommand c, std::string arg) {
        if (cmd != UiCommand::None) return;
        cmd = c;
        result.arg = std::move(arg);
      };
      // One tab per protocol. The app chooses the tab (it follows the
      // focused page): SetSelected once per change, then a click reports
      // back. ImGui may show the old tab for a frame after SetSelected.
      const bool told = !identity_section_init_ ||
                        state.identity_section != identity_section_shown_;
      if (told) {
        identity_section_shown_ = state.identity_section;
        identity_section_init_ = true;
        identity_section_settle_ = 2;
      }
      IdentitySection shown = identity_section_shown_;
      if (ImGui::BeginTabBar("##identity_tabs")) {
        auto tab = [&](const char* name, IdentitySection sec) {
          const ImGuiTabItemFlags f =
              told && state.identity_section == sec ?
                  ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
          if (!ImGui::BeginTabItem(name, nullptr, f)) return;
          shown = sec;
          ImGui::EndTabItem();
        };
        tab("Gemini", IdentitySection::Gemini);
        tab("Nostr", IdentitySection::Nostr);
        ImGui::EndTabBar();
      }
      if (identity_section_settle_ > 0) {
        --identity_section_settle_;
        shown = identity_section_shown_;
      } else if (shown != identity_section_shown_) {
        identity_section_shown_ = shown;
        pick(UiCommand::IdentitySection,
             shown == IdentitySection::Nostr ? "nostr" : "gemini");
      }
      if (shown == IdentitySection::Gemini) {
        if (state.identity_request_status != 0) {
          ImGui::PushStyleColor(ImGuiCol_Text, warn);
          ImGui::TextWrapped("%s", state.identity_request_status == 60 ?
              "This page asks for an identity." :
              state.identity_request_status == 61 ?
              "This page does not accept the identity you sent." :
              "This page did not accept the identity you sent.");
          ImGui::PopStyleColor();
          if (!state.identity_request_meta.empty())
            ImGui::TextWrapped("Server: %s", state.identity_request_meta.c_str());
        } else {
          ImGui::TextWrapped("A Gemini identity is a client certificate. A "
                             "server knows you by it, like an account; it is "
                             "only sent where you chose to use it.");
        }
        if (on_page) {
          ImGui::TextDisabled("This page: %s",
                              state.identity_active_name.empty() ?
                                  "no identity" :
                                  state.identity_active_name.c_str());
          if (!state.identity_active_name.empty()) {
            same_line_if_fits("Stop using here");
            if (classic_button("Stop using here"))
              pick(UiCommand::IdentityStopHere, {});
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("This page and what is below it go out "
                                "without a certificate.");
          }
        }
        if (state.identity_no_save)
          ImGui::TextDisabled("--no-save: identities last for this session only.");
        if (!state.identity_error.empty())
          ImGui::TextColored(warn, "%s", state.identity_error.c_str());

        // New identity: a name, then create (and on a Gemini page, use it on
        // the whole server right away).
        ImGui::Separator();
        ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x,
                                         220.f * scale_));
        ImGui::InputTextWithHint("##identity_new", "Name of a new identity",
                                 identity_new_name_, sizeof identity_new_name_);
        const bool named = identity_new_name_[0] != '\0';
        ImGui::BeginDisabled(!named);
        if (on_page) {
          same_line_if_fits("Create for this site");
          if (classic_button("Create for this site")) {
            pick(UiCommand::IdentityCreateHere, identity_new_name_);
            identity_new_name_[0] = '\0';
          }
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Make a new certificate and use it on %s",
                              state.identity_page_site.c_str());
        }
        same_line_if_fits("Create");
        if (classic_button("Create")) {
          pick(UiCommand::IdentityCreate, identity_new_name_);
          identity_new_name_[0] = '\0';
        }
        ImGui::EndDisabled();

        const UiState::IdentityRow* sel = nullptr;
        for (const auto& row : state.identity_rows)
          if (row.sha256 == state.identity_selected) sel = &row;
        ImGui::Separator();
        if (state.identity_rows.empty()) {
          ImGui::TextDisabled("No identities yet.");
        } else {
          const float list_h = sel ?
              std::max(80.f, ImGui::GetContentRegionAvail().y * 0.35f) : 0.f;
          if (begin_panel_list("##identity_list", ImVec2(0.f, list_h),
                                ImGuiChildFlags_Borders)) {
            for (const auto& row : state.identity_rows) {
              ImGui::PushID(row.sha256.c_str());
              std::string label = row.name;
              if (row.active_here) label += "  (this page)";
              if (!row.saved) label += "  session only";
              if (ImGui::Selectable(label.c_str(), &row == sel))
                pick(UiCommand::IdentitySelect,
                     &row == sel ? std::string{} : row.sha256);
              ImGui::PopID();
            }
          }
          ImGui::EndChild();
        }
        if (sel) {
          ImGui::PushID(sel->sha256.c_str());
          if (identity_rename_for_ != sel->sha256) {
            identity_rename_for_ = sel->sha256;
            std::snprintf(identity_rename_, sizeof identity_rename_, "%s",
                          sel->name.c_str());
            identity_delete_armed_.clear();
          }
          ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x,
                                           220.f * scale_));
          ImGui::InputText("##identity_rename", identity_rename_,
                           sizeof identity_rename_);
          const bool renamed = identity_rename_[0] != '\0' &&
                               sel->name != identity_rename_;
          same_line_if_fits("Rename");
          ImGui::BeginDisabled(!renamed);
          if (classic_button("Rename"))
            pick(UiCommand::IdentityRename,
                 sel->sha256 + "\n" + identity_rename_);
          ImGui::EndDisabled();
          if (!sel->created.empty())
            ImGui::TextDisabled("Created %s", sel->created.c_str());
          if (on_page) {
            ImGui::BeginDisabled(sel->scope_here == state.identity_page_site);
            if (classic_button("Use on this site"))
              pick(UiCommand::IdentityUseSite, sel->sha256);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
              ImGui::SetTooltip("%s", state.identity_page_site.c_str());
            same_line_if_fits("Use on this page");
            ImGui::BeginDisabled(sel->scope_here == state.identity_page_scope);
            if (classic_button("Use on this page"))
              pick(UiCommand::IdentityUsePage, sel->sha256);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
              ImGui::SetTooltip("%s and below",
                                state.identity_page_scope.c_str());
          }
          ImGui::TextDisabled("%s", sel->scopes.empty() ?
              "Not used anywhere." : "Used on:");
          for (const auto& scope : sel->scopes) {
            ImGui::PushID(scope.c_str());
            if (classic_button("x"))
              pick(UiCommand::IdentityDropScope, sel->sha256 + "\n" + scope);
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Stop using it here");
            ImGui::SameLine();
            ImGui::TextWrapped("%s", scope.c_str());
            ImGui::PopID();
          }
          ImGui::TextDisabled("SHA-256");
          ImGui::Indent();
          std::string fp;
          for (size_t i = 0; i < sel->sha256.size(); i += 8)
            fp += (i ? " " : "") + sel->sha256.substr(i, 8);
          ImGui::TextWrapped("%s", fp.c_str());
          ImGui::Unindent();
          if (classic_button("Copy fingerprint"))
            ImGui::SetClipboardText(sel->sha256.c_str());
          same_line_if_fits("Delete…");
          if (identity_delete_armed_ != sel->sha256) {
            if (classic_button("Delete…")) identity_delete_armed_ = sel->sha256;
          } else {
            ImGui::PushStyleColor(ImGuiCol_Text, warn);
            ImGui::TextWrapped("Deleting loses the accounts it is for, for "
                               "good: no server can link a new identity to "
                               "them.");
            ImGui::PopStyleColor();
            if (classic_button("Delete for good")) {
              pick(UiCommand::IdentityDelete, sel->sha256);
              identity_delete_armed_.clear();
            }
            ImGui::SameLine();
            if (classic_button("Keep")) identity_delete_armed_.clear();
          }
          ImGui::PopID();
        } else if (!state.identity_rows.empty()) {
          ImGui::TextDisabled(on_page ? "Select an identity to use it here." :
                                        "Select an identity to see it.");
        }
      } else {
        // Nostr: accounts are public keys; those with a signing key here
        // (NIP-49 encrypted and unlocked per session, or kept without a
        // password) can post.
        ImGui::TextWrapped("A Nostr account is a key pair. With its secret "
                           "key here (kept encrypted by a password, or "
                           "without one) you post as it; without, it is "
                           "watch-only.");
        if (!state.nostr_can_sign)
          ImGui::TextDisabled("This build cannot sign (no secp256k1 library): "
                              "accounts are watch-only.");
        if (state.identity_no_save)
          ImGui::TextDisabled("--no-save: no accounts can be saved.");
        if (!state.identity_error.empty())
          ImGui::TextColored(warn, "%s", state.identity_error.c_str());
        auto account = [&](const std::string& pk) -> const NostrLocalIdentity* {
          for (const auto& i : state.nostr_identities)
            if (i.pubkey == pk) return &i;
          return nullptr;
        };
        auto listed = [](const std::vector<std::string>& v,
                         const std::string& pk) {
          return std::find(v.begin(), v.end(), pk) != v.end();
        };
        auto has_key = [&](const std::string& pk) {
          return listed(state.nostr_key_pubkeys, pk);
        };
        auto unlocked = [&](const std::string& pk) {
          return listed(state.nostr_unlocked_pubkeys, pk);
        };
        auto plain = [&](const std::string& pk) {
          return listed(state.nostr_plain_pubkeys, pk);
        };
        const char* const no_password_tip =
            "The key is kept unencrypted (only the file's permissions guard "
            "it): no unlocking, but anyone who can read your files can post "
            "as this account.";
        auto wipe = [](char* buf, size_t n) { std::memset(buf, 0, n); };
        if (!state.nostr_page_pubkey.empty()) {
          const NostrLocalIdentity* known = account(state.nostr_page_pubkey);
          ImGui::TextDisabled("This page: %s",
                              known ? known->label.c_str() :
                                      "an author not saved here");
          if (!known && !state.identity_no_save) {
            same_line_if_fits("Save this author");
            if (classic_button("Save this author"))
              pick(UiCommand::NostrAddWatchIdentity,
                   "\n" + state.nostr_page_pubkey);
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("Keep the page's author as a watch-only "
                                "account.");
          }
        }

        // Post: a kind-1 note as the active account.
        const float field_w = std::min(ImGui::GetContentRegionAvail().x,
                                       220.f * scale_);
        if (nostr_note_sent_seen_ != state.nostr_post_sent) {
          nostr_note_sent_seen_ = state.nostr_post_sent;
          nostr_note_[0] = '\0';  // went out: start a fresh note
        }
        const NostrLocalIdentity* poster = account(state.nostr_selected_identity);
        ImGui::Separator();
        if (poster && unlocked(poster->pubkey)) {
          ImGui::TextDisabled("Post a note as %s", poster->label.c_str());
          ImGui::InputTextMultiline(
              "##nostr_note", nostr_note_, sizeof nostr_note_,
              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4.5f));
          const bool blank = std::string_view(nostr_note_)
              .find_first_not_of(" \t\r\n") == std::string_view::npos;
          ImGui::BeginDisabled(blank || state.nostr_posting);
          if (classic_button(state.nostr_posting ? "Posting…" : "Post"))
            pick(UiCommand::NostrPublishNote, nostr_note_);
          ImGui::EndDisabled();
          if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Signs the note and sends it to public relays; "
                              "a posted note cannot be taken back.");
        } else if (poster && has_key(poster->pubkey)) {
          ImGui::TextDisabled("Unlock %s below to post as it.",
                              poster->label.c_str());
        } else if (poster) {
          ImGui::TextDisabled("%s is watch-only: it cannot post.",
                              poster->label.c_str());
        } else {
          ImGui::TextDisabled("Create or import an account to post.");
        }
        if (!state.nostr_post_status.empty()) {
          ImGui::TextWrapped("%s", state.nostr_post_status.c_str());
          if (!state.nostr_posting && !state.nostr_post_note.empty() &&
              state.nostr_post_sent > 0) {
            same_line_if_fits("View note");
            if (classic_button("View note"))
              pick(UiCommand::NavigateUrl, state.nostr_post_note);
          }
          for (const auto& r : state.nostr_post_results) {
            const std::string line = r.relay + (r.accepted ? "  ok" : "  failed") +
                (r.message.empty() ? std::string{} : ": " + r.message);
            if (r.accepted)
              ImGui::TextDisabled("%s", line.c_str());
            else
              ImGui::TextColored(warn, "%s", line.c_str());
          }
        }

        // Add an account: a new key, an imported one, or a public key only.
        ImGui::Separator();
        ImGui::RadioButton("New key", &nostr_new_mode_, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Import key", &nostr_new_mode_, 1);
        ImGui::SameLine();
        ImGui::RadioButton("Watch only", &nostr_new_mode_, 2);
        const bool can_keep = state.nostr_can_sign && !state.identity_no_save;
        ImGui::SetNextItemWidth(field_w);
        ImGui::InputTextWithHint("##nostr_new_label", "Label",
                                 nostr_identity_label_,
                                 sizeof nostr_identity_label_);
        if (nostr_new_mode_ == 0) {
          ImGui::Checkbox("No password", &nostr_no_password_);
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", no_password_tip);
          bool same = true;
          if (!nostr_no_password_) {
            ImGui::SetNextItemWidth(field_w);
            ImGui::InputTextWithHint("##nostr_pw", "Password",
                                     nostr_key_password_,
                                     sizeof nostr_key_password_,
                                     ImGuiInputTextFlags_Password);
            ImGui::SetNextItemWidth(field_w);
            ImGui::InputTextWithHint("##nostr_pw2", "Password again",
                                     nostr_key_password2_,
                                     sizeof nostr_key_password2_,
                                     ImGuiInputTextFlags_Password);
            same = std::strcmp(nostr_key_password_, nostr_key_password2_) == 0;
            if (!same && nostr_key_password2_[0] != '\0')
              ImGui::TextColored(warn, "The passwords differ.");
          }
          ImGui::BeginDisabled(!can_keep || !same ||
                               (!nostr_no_password_ &&
                                nostr_key_password_[0] == '\0'));
          if (classic_button("Create account")) {
            pick(UiCommand::NostrCreateKey,
                 std::string(nostr_identity_label_) + "\n" +
                     (nostr_no_password_ ? "" : nostr_key_password_));
            nostr_identity_label_[0] = '\0';
            wipe(nostr_key_password_, sizeof nostr_key_password_);
            wipe(nostr_key_password2_, sizeof nostr_key_password2_);
          }
          ImGui::EndDisabled();
          if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", nostr_no_password_ ?
                "Makes a new key pair, kept here without a password." :
                "Makes a new key pair. The password encrypts the key here; "
                "it cannot be recovered.");
        } else if (nostr_new_mode_ == 1) {
          ImGui::SetNextItemWidth(field_w);
          ImGui::InputTextWithHint("##nostr_import", "nsec1… or ncryptsec1…",
                                   nostr_key_import_, sizeof nostr_key_import_,
                                   ImGuiInputTextFlags_Password);
          // An ncryptsec needs its own password; an nsec may stay plain.
          const bool encrypted = std::string_view(nostr_key_import_)
              .find("ncryptsec1") != std::string_view::npos;
          const bool bare = !encrypted && nostr_no_password_;
          if (!encrypted) {
            ImGui::Checkbox("No password", &nostr_no_password_);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", no_password_tip);
          }
          if (!bare) {
            ImGui::SetNextItemWidth(field_w);
            ImGui::InputTextWithHint("##nostr_import_pw", "Password",
                                     nostr_key_password_,
                                     sizeof nostr_key_password_,
                                     ImGuiInputTextFlags_Password);
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("An ncryptsec's own password, or a new one to "
                                "encrypt an nsec with.");
          }
          ImGui::BeginDisabled(!can_keep || nostr_key_import_[0] == '\0' ||
                               (!bare && nostr_key_password_[0] == '\0'));
          if (classic_button("Import key")) {
            pick(UiCommand::NostrImportKey,
                 std::string(nostr_identity_label_) + "\n" +
                     nostr_key_import_ + "\n" +
                     (bare ? "" : nostr_key_password_));
            nostr_identity_label_[0] = '\0';
            wipe(nostr_key_import_, sizeof nostr_key_import_);
            wipe(nostr_key_password_, sizeof nostr_key_password_);
          }
          ImGui::EndDisabled();
        } else {
          ImGui::SetNextItemWidth(field_w);
          ImGui::InputTextWithHint("##nostr_new_key", "npub1… or hex key",
                                   nostr_identity_pubkey_,
                                   sizeof nostr_identity_pubkey_);
          ImGui::BeginDisabled(nostr_identity_pubkey_[0] == '\0' ||
                               state.identity_no_save);
          if (classic_button("Save account")) {
            pick(UiCommand::NostrAddWatchIdentity,
                 std::string(nostr_identity_label_) + "\n" +
                     nostr_identity_pubkey_);
            nostr_identity_label_[0] = '\0';
            nostr_identity_pubkey_[0] = '\0';
          }
          ImGui::EndDisabled();
        }

        const NostrLocalIdentity* nsel = account(state.nostr_shown_identity);
        ImGui::Separator();
        if (state.nostr_identities.empty()) {
          ImGui::TextDisabled("No Nostr accounts yet.");
        } else {
          // Sized to its rows (a few at least, a third of the panel at
          // most) so the details and the footer stay in view.
          const float row = ImGui::GetTextLineHeightWithSpacing();
          const float rows = float(std::max<size_t>(
              4, state.nostr_identities.size() + 1));
          const float list_h = std::min(
              row * rows + ImGui::GetStyle().WindowPadding.y * 2.f,
              std::max(80.f, ImGui::GetContentRegionAvail().y *
                                 (nsel ? 0.35f : 0.6f)));
          if (begin_panel_list("##nostr_identity_list", ImVec2(0.f, list_h),
                                ImGuiChildFlags_Borders)) {
            for (const auto& acc : state.nostr_identities) {
              ImGui::PushID(acc.pubkey.c_str());
              std::string label = acc.label;
              if (plain(acc.pubkey)) label += "  (no password)";
              else if (unlocked(acc.pubkey)) label += "  (unlocked)";
              else if (has_key(acc.pubkey)) label += "  (locked)";
              if (acc.pubkey == state.nostr_selected_identity)
                label += "  (active)";
              if (acc.pubkey == state.nostr_page_pubkey) label += "  (this page)";
              if (ImGui::Selectable(label.c_str(), &acc == nsel))
                pick(UiCommand::NostrIdentityShow,
                     &acc == nsel ? std::string{} : acc.pubkey);
              ImGui::PopID();
            }
          }
          ImGui::EndChild();
        }
        if (nsel) {
          ImGui::PushID(nsel->pubkey.c_str());
          if (nostr_identity_rename_for_ != nsel->pubkey) {
            nostr_identity_rename_for_ = nsel->pubkey;
            std::snprintf(nostr_identity_rename_, sizeof nostr_identity_rename_,
                          "%s", nsel->label.c_str());
          }
          ImGui::SetNextItemWidth(field_w);
          ImGui::InputText("##nostr_rename", nostr_identity_rename_,
                           sizeof nostr_identity_rename_);
          same_line_if_fits("Rename");
          ImGui::BeginDisabled(nostr_identity_rename_[0] == '\0' ||
                               nsel->label == nostr_identity_rename_ ||
                               state.identity_no_save);
          if (classic_button("Rename"))
            pick(UiCommand::NostrAddWatchIdentity,
                 std::string(nostr_identity_rename_) + "\n" + nsel->pubkey);
          ImGui::EndDisabled();
          const std::string npub = nostr_bech32("npub", nsel->pubkey);
          ImGui::TextDisabled("Public key");
          ImGui::Indent();
          ImGui::TextWrapped("%s", npub.c_str());
          ImGui::Unindent();
          if (classic_button("View profile"))
            pick(UiCommand::NavigateUrl, "nostr:" + npub);
          same_line_if_fits("Copy npub");
          if (classic_button("Copy npub")) ImGui::SetClipboardText(npub.c_str());
          const bool active = nsel->pubkey == state.nostr_selected_identity;
          same_line_if_fits("Make active");
          ImGui::BeginDisabled(active || state.identity_no_save);
          if (classic_button(active ? "Active" : "Make active"))
            pick(UiCommand::NostrSelectIdentity, nsel->pubkey);
          ImGui::EndDisabled();
          if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("The account notes are posted as.");
          const bool keyed = has_key(nsel->pubkey);
          same_line_if_fits("Remove");
          ImGui::BeginDisabled(state.identity_no_save || keyed);
          if (classic_button("Remove"))
            pick(UiCommand::NostrIdentityRemove, nsel->pubkey);
          ImGui::EndDisabled();
          if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", keyed ?
                "Delete its signing key first." :
                "Forget the label here; the account itself is not touched.");
          if (keyed) {
            const bool open = unlocked(nsel->pubkey);
            const bool bare = plain(nsel->pubkey);
            ImGui::Separator();
            ImGui::TextDisabled("%s", bare ? "Signing key: no password" :
                                open ? "Signing key: unlocked for this session" :
                                       "Signing key: locked");
            if (bare && ImGui::IsItemHovered())
              ImGui::SetTooltip("%s", no_password_tip);
            if (bare) {
              // Nothing to unlock or lock.
            } else if (!open) {
              ImGui::SetNextItemWidth(field_w);
              const bool enter = ImGui::InputTextWithHint(
                  "##nostr_unlock_pw", "Password", nostr_unlock_password_,
                  sizeof nostr_unlock_password_,
                  ImGuiInputTextFlags_Password |
                      ImGuiInputTextFlags_EnterReturnsTrue);
              same_line_if_fits("Unlock");
              ImGui::BeginDisabled(nostr_unlock_password_[0] == '\0');
              if ((classic_button("Unlock") || enter) &&
                  nostr_unlock_password_[0] != '\0') {
                pick(UiCommand::NostrUnlockKey,
                     nsel->pubkey + "\n" + nostr_unlock_password_);
                wipe(nostr_unlock_password_, sizeof nostr_unlock_password_);
              }
              ImGui::EndDisabled();
            } else if (classic_button("Lock")) {
              pick(UiCommand::NostrLockKey, nsel->pubkey);
              nostr_reveal_armed_.clear();
            }
            if (open && nostr_change_pw_for_ != nsel->pubkey) {
              const char* what = bare ? "Set password…" : "Change password…";
              same_line_if_fits(what);
              if (classic_button(what)) {
                nostr_change_pw_for_ = nsel->pubkey;
                wipe(nostr_new_password_, sizeof nostr_new_password_);
                wipe(nostr_new_password2_, sizeof nostr_new_password2_);
              }
            }
            if (open && nostr_change_pw_for_ == nsel->pubkey) {
              auto done = [&] {
                nostr_change_pw_for_.clear();
                wipe(nostr_new_password_, sizeof nostr_new_password_);
                wipe(nostr_new_password2_, sizeof nostr_new_password2_);
              };
              ImGui::SetNextItemWidth(field_w);
              ImGui::InputTextWithHint("##nostr_new_pw", "New password",
                                       nostr_new_password_,
                                       sizeof nostr_new_password_,
                                       ImGuiInputTextFlags_Password);
              ImGui::SetNextItemWidth(field_w);
              ImGui::InputTextWithHint("##nostr_new_pw2", "New password again",
                                       nostr_new_password2_,
                                       sizeof nostr_new_password2_,
                                       ImGuiInputTextFlags_Password);
              const bool same = std::strcmp(nostr_new_password_,
                                            nostr_new_password2_) == 0;
              if (!same && nostr_new_password2_[0] != '\0')
                ImGui::TextColored(warn, "The passwords differ.");
              ImGui::BeginDisabled(!same || nostr_new_password_[0] == '\0' ||
                                   state.identity_no_save);
              if (classic_button("Set password")) {
                pick(UiCommand::NostrSetKeyPassword,
                     nsel->pubkey + "\n" + nostr_new_password_);
                done();
              }
              ImGui::EndDisabled();
              if (!bare) {
                same_line_if_fits("Remove password");
                ImGui::BeginDisabled(state.identity_no_save);
                if (classic_button("Remove password")) {
                  pick(UiCommand::NostrSetKeyPassword, nsel->pubkey + "\n");
                  done();
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                  ImGui::SetTooltip("%s", no_password_tip);
              }
              ImGui::SameLine();
              if (classic_button("Cancel##pw")) done();
            }
            if (!state.nostr_shown_ncryptsec.empty()) {
              same_line_if_fits("Copy backup key");
              if (classic_button("Copy backup key"))
                ImGui::SetClipboardText(state.nostr_shown_ncryptsec.c_str());
              if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The encrypted key (ncryptsec1…). Keep it "
                                  "somewhere safe; with its password it "
                                  "restores the account anywhere.");
            }
            if (open) {
              if (nostr_reveal_armed_ != nsel->pubkey) {
                same_line_if_fits("Copy nsec…");
                if (classic_button("Copy nsec…"))
                  nostr_reveal_armed_ = nsel->pubkey;
              } else {
                ImGui::PushStyleColor(ImGuiCol_Text, warn);
                ImGui::TextWrapped("The nsec is the plain secret key: anyone "
                                   "who sees it can post as you, for good.");
                ImGui::PopStyleColor();
                if (classic_button("Copy nsec")) {
                  pick(UiCommand::NostrCopyNsec, nsel->pubkey);
                  nostr_reveal_armed_.clear();
                }
                ImGui::SameLine();
                if (classic_button("Cancel")) nostr_reveal_armed_.clear();
              }
            }
            if (nostr_delete_key_armed_ != nsel->pubkey) {
              same_line_if_fits("Delete key…");
              if (classic_button("Delete key…"))
                nostr_delete_key_armed_ = nsel->pubkey;
            } else {
              ImGui::PushStyleColor(ImGuiCol_Text, warn);
              ImGui::TextWrapped("Without a backup key the account is lost "
                                 "for good: nobody can post as it again. "
                                 "It stays here as watch-only.");
              ImGui::PopStyleColor();
              if (classic_button("Delete key for good")) {
                pick(UiCommand::NostrDeleteKey, nsel->pubkey);
                nostr_delete_key_armed_.clear();
              }
              ImGui::SameLine();
              if (classic_button("Keep")) nostr_delete_key_armed_.clear();
            }
          }
          ImGui::PopID();
        } else if (!state.nostr_identities.empty()) {
          ImGui::TextDisabled("Select an account to see it.");
        }
      }
    }
    ImGui::End();
  }

  // ---- Related Sites: similar websites of the focused page's site ----
  if (state.related_open &&
      !(state.shaded_panels & ui_panel_bit(UiPanelLayer::RelatedSites))) {
    if (begin_panel(state, UiPanelLayer::RelatedSites, "##related_sites_body",
                    embedded_panel_flags, state.related_x, state.related_y,
                    state.related_w, state.related_h)) {
      if (state.related_host.empty()) {
        ImGui::TextWrapped("Open a web page to see sites similar to it.");
      } else {
        const std::string& shown = state.related_domain.empty()
                                       ? state.related_host
                                       : state.related_domain;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Similar to");
        ImGui::SameLine();
        ImGui::TextUnformatted(shown.c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(state.related_loading);
        if (classic_button("Refresh") && cmd == UiCommand::None)
          cmd = UiCommand::RelatedRefresh;
        ImGui::EndDisabled();
        // Off holds the list while its rows are explored (a click would
        // otherwise replace it with the clicked site's neighbours).
        bool follow = state.related_follow;
        same_line_if_fits("[x] Follow page");
        if (ImGui::Checkbox("Follow page", &follow) && cmd == UiCommand::None)
          cmd = UiCommand::RelatedFollow;
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("On: show sites related to the focused page.\n"
                            "Off: keep this list while browsing its sites.");
        // On the checkbox's line: a new line would shift the rows under the
        // pointer right after the click that opened one of them.
        if (state.related_behind) {
          same_line_if_fits("held");
          ImGui::TextDisabled("held");
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The focused page is another site.");
        }
        if (state.related_loading)
          ImGui::TextDisabled("Asking similar-site services\xe2\x80\xa6");
        for (const auto& note : state.related_notes)
          ImGui::TextDisabled("%s", note.c_str());
        if (!state.related_loading && state.related_rows.empty())
          ImGui::TextDisabled("No similar sites known for %s.", shown.c_str());
        const float foot = ImGui::GetFrameHeightWithSpacing();
        const bool list = !state.related_rows.empty();
        if (list && begin_panel_list("##related_list", ImVec2(0.f, -foot),
                                      ImGuiChildFlags_Borders)) {
          for (size_t i = 0; i < state.related_rows.size(); ++i) {
            const auto& row = state.related_rows[i];
            ImGui::PushID(static_cast<int>(i));
            const std::string url = "https://" + row.domain + "/";
            if (ImGui::Selectable(row.domain.c_str(), false) &&
                cmd == UiCommand::None) {
              cmd = UiCommand::RelatedOpen;
              result.arg = url;
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Middle) &&
                cmd == UiCommand::None) {
              cmd = UiCommand::RelatedOpenTab;
              result.arg = url;
            }
            if (ImGui::IsItemHovered()) {
              ImGui::BeginTooltip();
              ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.f);
              ImGui::TextUnformatted(url.c_str());
              if (!row.description.empty())
                ImGui::TextUnformatted(row.description.c_str());
              if (!row.category.empty())
                ImGui::TextDisabled("%s", row.category.c_str());
              ImGui::TextDisabled("from %s \xc2\xb7 middle-click: new tab",
                                  row.source.c_str());
              ImGui::PopTextWrapPos();
              ImGui::EndTooltip();
            }
            if (ImGui::BeginPopupContextItem("##related_row")) {
              if (ImGui::MenuItem("Open") && cmd == UiCommand::None) {
                cmd = UiCommand::RelatedOpen;
                result.arg = url;
              }
              if (ImGui::MenuItem("Open in New Tab") && cmd == UiCommand::None) {
                cmd = UiCommand::RelatedOpenTab;
                result.arg = url;
              }
              if (ImGui::MenuItem("Copy Address"))
                ImGui::SetClipboardText(url.c_str());
              ImGui::EndPopup();
            }
            // Title, else the start of the description (all of it in the tooltip).
            std::string sub = !row.title.empty() && row.title != row.domain
                                  ? row.title : row.description;
            if (sub.size() > 110) {
              size_t cut = 110;
              while (cut > 0 && (static_cast<unsigned char>(sub[cut]) & 0xC0) == 0x80)
                --cut;
              sub.resize(cut);
              sub += "\xe2\x80\xa6";
            }
            if (!sub.empty()) {
              ImGui::Indent();
              ImGui::PushTextWrapPos(0.f);
              ImGui::TextDisabled("%s", sub.c_str());
              ImGui::PopTextWrapPos();
              ImGui::Unindent();
            }
            if (row.score > 0.f) {
              // Similarity as a thin bar under the row.
              const ImVec2 p = ImGui::GetCursorScreenPos();
              const float w = ImGui::GetContentRegionAvail().x;
              const float h = std::max(2.f, 3.f * scale_);
              ImDrawList* dl = ImGui::GetWindowDrawList();
              dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(0, 0, 0, 28));
              dl->AddRectFilled(p, ImVec2(p.x + w * std::clamp(row.score, 0.f, 1.f),
                                          p.y + h),
                                IM_COL32(70, 120, 190, 200));
              ImGui::Dummy(ImVec2(w, h));
            }
            ImGui::PopID();
          }
        }
        if (list) ImGui::EndChild();
        // Credit the services (similars.net asks for a link back).
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Data:");
        ImGui::SameLine();
        if (ImGui::SmallButton("similarsites.com"))
          xanatoo::sdl::open_url(("https://similarsites.com/site/" + shown).c_str());
        if (state.related_similars_net) {
          ImGui::SameLine();
          if (ImGui::SmallButton("similars.net"))
            xanatoo::sdl::open_url("https://similars.net/");
        }
      }
    }
    ImGui::End();
  }

  // Draw last among normal windows so it sits above all chrome and dockable
  // panel bodies, without stealing focus from the location text editor.
  draw_url_suggestions(state, result, cmd);
  result.cmd = cmd;

  // ---- On-screen keyboard (handheld / no physical keyboard) ----
  if (state.virtual_keyboard) {
    ImGuiViewport* vk_vp = ImGui::GetMainViewport();
    const float vk_h = (vk_vp->Size.y < 500.f ? 200.f : 220.f) * scale_;
    ImGui::SetNextWindowPos(
        ImVec2(vk_vp->Pos.x, vk_vp->Pos.y + vk_vp->Size.y - vk_h));
    ImGui::SetNextWindowSize(ImVec2(vk_vp->Size.x, vk_h));
    ImGuiWindowFlags vk_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("##vkeyboard", nullptr, vk_flags)) {
      auto keybtn = [&](const char* label, const char* payload, float w) {
        if (classic_button(label, ImVec2(w, 34.f * scale_))) {
          if (cmd == UiCommand::None) {
            cmd = UiCommand::VirtualKey;
            result.arg = payload;
          }
        }
        ImGui::SameLine(0.f, 2.f);
      };
      static bool shift = false;
      const float kw = std::max(26.f, (vk_vp->Size.x - 40.f) / 11.f);
      const char* row1 = shift ? "QWERTYUIOP" : "qwertyuiop";
      const char* row2 = shift ? "ASDFGHJKL" : "asdfghjkl";
      const char* row3 = shift ? "ZXCVBNM" : "zxcvbnm";
      ImGui::SetCursorPosX(4.f);
      for (const char* c = row1; *c; ++c) {
        char lab[2] = {*c, 0};
        char pay[2] = {*c, 0};
        keybtn(lab, pay, kw);
      }
      ImGui::NewLine();
      ImGui::SetCursorPosX(4.f + kw * 0.5f);
      for (const char* c = row2; *c; ++c) {
        char lab[2] = {*c, 0};
        char pay[2] = {*c, 0};
        keybtn(lab, pay, kw);
      }
      ImGui::NewLine();
      if (classic_button(shift ? "SHIFT" : "Shift", ImVec2(kw * 1.6f, 34.f * scale_)))
        shift = !shift;
      ImGui::SameLine(0.f, 2.f);
      for (const char* c = row3; *c; ++c) {
        char lab[2] = {*c, 0};
        char pay[2] = {*c, 0};
        keybtn(lab, pay, kw);
      }
      if (classic_button("Bksp", ImVec2(kw * 1.8f, 34.f * scale_))) {
        if (cmd == UiCommand::None) {
          cmd = UiCommand::VirtualKey;
          result.arg = "\b";
        }
      }
      ImGui::NewLine();
      ImGui::SetCursorPosX(4.f);
      for (const char* d = "1234567890"; *d; ++d) {
        char lab[2] = {*d, 0};
        char pay[2] = {*d, 0};
        keybtn(lab, pay, kw * 0.95f);
      }
      ImGui::NewLine();
      if (classic_button(",", ImVec2(kw, 34.f * scale_))) {
        if (cmd == UiCommand::None) {
          cmd = UiCommand::VirtualKey;
          result.arg = ",";
        }
      }
      ImGui::SameLine(0.f, 2.f);
      const float space_w = std::max(100.f, vk_vp->Size.x - kw * 6.f);
      if (classic_button("Space", ImVec2(space_w, 34.f * scale_))) {
        if (cmd == UiCommand::None) {
          cmd = UiCommand::VirtualKey;
          result.arg = " ";
        }
      }
      ImGui::SameLine(0.f, 2.f);
      if (classic_button(".", ImVec2(kw, 34.f * scale_))) {
        if (cmd == UiCommand::None) {
          cmd = UiCommand::VirtualKey;
          result.arg = ".";
        }
      }
      ImGui::SameLine(0.f, 2.f);
      if (classic_button("Enter", ImVec2(kw * 2.f, 34.f * scale_))) {
        if (cmd == UiCommand::None) {
          cmd = UiCommand::VirtualKey;
          result.arg = "\n";
        }
      }
      ImGui::SameLine(0.f, 2.f);
      if (classic_button("Hide", ImVec2(kw * 1.6f, 34.f * scale_)))
        cmd = UiCommand::ToggleVirtualKeyboard;
    }
    ImGui::End();
    result.bottom_reserved = vk_h;
  }

  // Match the native paint stack for ImGui input, including child widgets.
  sync_embedded_imgui_order(state.native_panel_order);
  return result;
}

void Ui::prepare_render() {
  if (!ready_) return;
  if (render_prepared_) return;
  // A text field (URL bar) just took focus, or a focused one was tapped
  // again: on-screen keyboard (Android). The second case matters after the
  // user hid the keyboard (Back, the IME's hide key, a swipe) while the field
  // kept focus — no new focus edge comes then. A click outside an active
  // InputText clears it during the widget pass, so a click with one still
  // active landed on it.
  const bool wants_text = ImGui::GetIO().WantTextInput;
  const ImGuiContext& g = *ImGui::GetCurrentContext();
  const bool field_tapped = g.ActiveId != 0 && g.InputTextState.ID == g.ActiveId &&
                            ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  if ((wants_text && !wanted_text_input_) || field_tapped)
    platform_request_keyboard(window_);
  wanted_text_input_ = wants_text;
  publish_text_field_selection();
  if (middle_paste_frames_ > 0) --middle_paste_frames_;
  resolve_middle_click();
  if (hover_tracking_) {
    hover_item_ = g_hover_capture;
    const ImGuiWindow* hw = ImGui::GetCurrentContext()->HoveredWindow;
    hover_item_.window = hw && hw->RootWindow ? hw->RootWindow->Name : "";
  } else {
    hover_item_ = UiHoverItem{};
  }
  ImGui::Render();
  render_prepared_ = true;
#if defined(XANATOO_SDL3) && XANATOO_SDL3
  layered_panels_ = true;
#else
  // The pre-2.0.18 SDL fallback caches a *whole* UI render in one texture;
  // rendering individual lists would invalidate that cache repeatedly. Keep
  // the historical single-pass path for those older R36S SDL libraries.
  layered_panels_ = !ImGui_ImplSDLRenderer2_UsesTriangleFallback();
#endif
}

// ImGui produces one draw list per window (and separate lists for child
// windows). Partition those existing lists without rebuilding widgets or
// changing their input state; each panel's lists can then be composited in
// exactly the same order as its native FloatWindow frame.
static void render_ui_layer(SDL_Renderer* renderer, UiPanelLayer layer,
                            bool chrome_only) {
  ImDrawData* full = ImGui::GetDrawData();
  if (!full) return;
  std::vector<ImDrawList*> lists;
  int vertices = 0, indices = 0;
  for (int i = 0; i < full->CmdListsCount; ++i) {
    ImDrawList* list = full->CmdLists[i];
    const UiPanelLayer owner = ui_panel_layer(
        list->_OwnerName ? std::string_view(list->_OwnerName) : std::string_view());
    if ((chrome_only && owner != UiPanelLayer::Chrome) ||
        (!chrome_only && owner != layer))
      continue;
    lists.push_back(list);
    vertices += list->VtxBuffer.Size;
    indices += list->IdxBuffer.Size;
  }
  if (lists.empty()) return;
  // ImDrawData owns the pointer array (ImVector), so never shallow-copy it:
  // a copied array would be freed twice at the end of this temporary pass.
  ImDrawData selected;
  selected.Valid = full->Valid;
  selected.DisplayPos = full->DisplayPos;
  selected.DisplaySize = full->DisplaySize;
  selected.FramebufferScale = full->FramebufferScale;
  selected.CmdLists.resize(static_cast<int>(lists.size()));
  for (int i = 0; i < static_cast<int>(lists.size()); ++i)
    selected.CmdLists[i] = lists[static_cast<size_t>(i)];
  selected.CmdListsCount = static_cast<int>(lists.size());
  selected.TotalVtxCount = vertices;
  selected.TotalIdxCount = indices;
#if defined(XANATOO_SDL3) && XANATOO_SDL3
  xanatoo::imgui_sdl3::render(&selected, renderer);
#else
  ImGui_ImplSDLRenderer2_RenderDrawData(&selected, renderer);
#endif
}

void Ui::render_embedded_panel(SDL_Renderer* renderer, UiPanelLayer panel) {
  if (!ready_ || !render_prepared_ || !layered_panels_ ||
      panel == UiPanelLayer::Chrome) return;
  render_ui_layer(renderer, panel, false);
}

void Ui::end_frame(SDL_Renderer* renderer) {
  if (!ready_) return;
  prepare_render();
  if (layered_panels_) {
    render_ui_layer(renderer, UiPanelLayer::Chrome, true);
  } else {
#if defined(XANATOO_SDL3) && XANATOO_SDL3
    xanatoo::imgui_sdl3::render(ImGui::GetDrawData(), renderer);
#else
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
#endif
  }
  render_prepared_ = false;
}

}  // namespace xanatoo
