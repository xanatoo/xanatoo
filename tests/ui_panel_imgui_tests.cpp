// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#include "xanatoo/ui_panel_layers.hpp"
#include "ui_panel_imgui_order.hpp"

#include "imgui.h"
#include "imgui_internal.h"

#include <cassert>
#include <string_view>

// Verify the actual vendored ImGui's draw-list names, including its nested
// BeginChild scheme. The classifier's pure tests alone cannot detect ImGui
// changing how it names or groups its render lists.
int main() {
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;  // test must not write an imgui.ini in the build tree
  io.DisplaySize = ImVec2(800.f, 600.f);
  io.DeltaTime = 1.f / 60.f;
  unsigned char* pixels = nullptr;
  int width = 0, height = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

  for (int frame = 0; frame < 2; ++frame) {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(20.f, 40.f));
    ImGui::SetNextWindowSize(ImVec2(220.f, 210.f));
    if (ImGui::Begin("##nostr_explorer_body")) {
      ImGui::TextUnformatted("Parent");
      if (ImGui::BeginChild("##nostr_results", ImVec2(140.f, 100.f)))
        ImGui::TextUnformatted("Child");
      ImGui::EndChild();
    }
    ImGui::End();
    ImGui::SetNextWindowPos(ImVec2(20.f, 260.f));
    ImGui::SetNextWindowSize(ImVec2(220.f, 210.f));
    if (ImGui::Begin("##subscriptions_panel_body")) {
      ImGui::TextUnformatted("Subscriptions");
      if (ImGui::BeginChild("##subscription_results", ImVec2(140.f, 100.f)))
        ImGui::TextUnformatted("Watched feeds");
      ImGui::EndChild();
    }
    ImGui::End();
    ImGui::SetNextWindowPos(ImVec2(470.f, 240.f));
    ImGui::SetNextWindowSize(ImVec2(220.f, 210.f));
    if (ImGui::Begin("##lemmy_explorer_body")) {
      ImGui::TextUnformatted("Lemmy communities");
      if (ImGui::BeginChild("##lemmy_results", ImVec2(150.f, 100.f)))
        ImGui::TextUnformatted("Discovered servers");
      ImGui::EndChild();
    }
    ImGui::End();
    ImGui::SetNextWindowPos(ImVec2(320.f, 50.f));
    ImGui::SetNextWindowSize(ImVec2(200.f, 200.f));
    if (ImGui::Begin("Preferences"))
      ImGui::TextUnformatted("Settings");
    ImGui::End();
    ImGui::Render();
  }

  bool parent = false, child = false, chrome = false;
  bool lemmy_parent = false, lemmy_child = false;
  bool subs_parent = false, subs_child = false;
  const ImDrawData* data = ImGui::GetDrawData();
  assert(data);
  for (int i = 0; i < data->CmdListsCount; ++i) {
    const char* owner = data->CmdLists[i]->_OwnerName;
    if (!owner) continue;
    const std::string_view name(owner);
    const auto layer = xanatoo::ui_panel_layer(name);
    if (name == "##nostr_explorer_body")
      parent = layer == xanatoo::UiPanelLayer::NostrExplorer;
    if (name.find("##nostr_explorer_body/##nostr_results_") == 0)
      child = layer == xanatoo::UiPanelLayer::NostrExplorer;
    if (name == "##lemmy_explorer_body")
      lemmy_parent = layer == xanatoo::UiPanelLayer::LemmyExplorer;
    if (name.find("##lemmy_explorer_body/##lemmy_results_") == 0)
      lemmy_child = layer == xanatoo::UiPanelLayer::LemmyExplorer;
    if (name == "##subscriptions_panel_body")
      subs_parent = layer == xanatoo::UiPanelLayer::Subscriptions;
    if (name.find("##subscriptions_panel_body/##subscription_results_") == 0)
      subs_child = layer == xanatoo::UiPanelLayer::Subscriptions;
    if (name == "Preferences")
      chrome = layer == xanatoo::UiPanelLayer::Chrome;
  }
  assert(parent && child && lemmy_parent && lemmy_child &&
         subs_parent && subs_child && chrome);

  // Native z-order can change without the two embedded ImGui windows being
  // focused. Both root and scrollable child must follow the same ordering.
  for (int frame = 0; frame < 3; ++frame) {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(50.f, 60.f));
    ImGui::SetNextWindowSize(ImVec2(270.f, 230.f));
    if (ImGui::Begin("##reply_editor")) {
      if (ImGui::BeginChild("##reply_child", ImVec2(240.f, 170.f)))
        ImGui::TextUnformatted("Reply text");
      ImGui::EndChild();
    }
    ImGui::End();
    ImGui::SetNextWindowPos(ImVec2(70.f, 80.f));
    ImGui::SetNextWindowSize(ImVec2(270.f, 230.f));
    if (ImGui::Begin("##bookmarks_panel_body")) {
      if (ImGui::BeginChild("##bookmarks_child", ImVec2(240.f, 170.f)))
        ImGui::TextUnformatted("Bookmarks list");
      ImGui::EndChild();
    }
    ImGui::End();
    xanatoo::sync_embedded_imgui_order({xanatoo::UiPanelLayer::Reply,
                                        xanatoo::UiPanelLayer::Bookmarks});
    ImGui::Render();
  }
  auto hovered_layer = [] {
    ImGuiWindow* hovered = nullptr;
    ImGui::FindHoveredWindowEx(ImVec2(130.f, 140.f), true, &hovered, nullptr);
    assert(hovered);
    return xanatoo::ui_panel_layer(hovered->Name);
  };
  assert(hovered_layer() == xanatoo::UiPanelLayer::Bookmarks);
  xanatoo::sync_embedded_imgui_order({xanatoo::UiPanelLayer::Bookmarks,
                                      xanatoo::UiPanelLayer::Reply});
  assert(hovered_layer() == xanatoo::UiPanelLayer::Reply);
  // Exercise the first screen pixel, where the scrollbar trough extends
  // past the inset thumb. Moving the scrollbar must also move the content.
  for (int frame = 0; frame < 3; ++frame) {
    if (frame == 1) io.AddMousePosEvent(0.f, 470.f);
    if (frame == 2) io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.f, 380.f));
    ImGui::SetNextWindowSize(ImVec2(200.f, 110.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::Begin("Left dock", nullptr, ImGuiWindowFlags_LeftScrollbar |
                 ImGuiWindowFlags_AlwaysVerticalScrollbar |
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoResize);
    ImGui::PopStyleVar();
    ImGuiWindow* panel = ImGui::GetCurrentWindow();
    const ImRect trough = ImGui::GetWindowScrollbarRect(panel, ImGuiAxis_Y);
    assert(trough.Min.x == 0.f);
    assert(trough.Max.x == panel->InnerRect.Min.x);
    ImGui::Dummy(ImVec2(100.f, 700.f));
    if (frame == 2) assert(ImGui::GetScrollY() > 0.f);
    ImGui::End();
    ImGui::Render();
  }

  ImGui::DestroyContext();
  ImGui::CreateContext();
  ImGuiIO& field_io = ImGui::GetIO();
  field_io.IniFilename = nullptr;
  field_io.DisplaySize = ImVec2(800.f, 600.f);
  field_io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
  char location[] = "https://example.org/path";
  for (int frame = 0; frame < 5; ++frame) {
    ImGui::NewFrame();
    ImGui::SetNextWindowSize(ImVec2(360.f, 160.f));
    ImGui::Begin("Location");
    if (frame == 0) ImGui::SetKeyboardFocusHere();
    const bool menu_open = ImGui::IsPopupOpen("Location menu");
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const int before = draw->VtxBuffer.Size;
    ImGui::InputText("##location", location, sizeof(location),
                     menu_open && frame != 4
                         ? ImGuiInputTextFlags_RenderInactiveSelection : 0);
    if (frame == 1) {
      assert(ImGui::IsItemActive());
      ImGuiInputTextState* input = ImGui::GetInputTextState(ImGui::GetItemID());
      input->WantReloadUserBuf = true;
      input->ReloadSelectionStart = 15;
      input->ReloadSelectionEnd = 8;
    }
    if (frame == 2) ImGui::OpenPopup("Location menu");
    if (frame >= 3) {
      assert(menu_open && !ImGui::IsItemActive());
      const ImU32 selection = ImGui::GetColorU32(ImGuiCol_TextSelectedBg, 0.6f);
      bool highlight = false;
      for (int i = before; i < draw->VtxBuffer.Size; ++i)
        highlight |= draw->VtxBuffer[i].col == selection;
      const auto* input = ImGui::GetInputTextState(ImGui::GetItemID());
      assert(input && input->HasSelection());
      assert(highlight == (frame == 3));
    }
    if (ImGui::BeginPopup("Location menu")) {
      ImGui::MenuItem("Copy");
      ImGui::EndPopup();
    }
    ImGui::End();
    ImGui::Render();
  }
  ImGui::DestroyContext();
}
