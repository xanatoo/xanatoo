// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include <optional>
#include <string_view>

namespace xanatoo {

// The embedded ImGui bodies whose title, geometry and stacking belong to native
// FloatWindows. Children use ImGui's "parent/child_ID" window naming scheme.
// Menus, tooltips and popups have their own windows and must remain foreground.
// FindBar belongs to the view it searches: a centre tile or document window;
// in a document window it also holds that view's edit strip. EditBar: the
// edit strips of the centre tiles (under every floating window).
enum class UiPanelLayer { Chrome, FindBar, EditBar, Reply, Bookmarks, Subscriptions, Styles, StyleManager,
                          Annotations, Context,
                          NostrExplorer, LemmyExplorer, Certificates,
                          Identities, RelatedSites, MatrixExplorer };

// Bit of `layer` in a set of embedded panels (UiState::shaded_panels).
inline unsigned ui_panel_bit(UiPanelLayer layer) {
  return 1u << static_cast<unsigned>(layer);
}

inline const char* ui_panel_root(UiPanelLayer layer) {
  switch (layer) {
    case UiPanelLayer::FindBar: return "##find_bar";
    case UiPanelLayer::EditBar: return "##edit_bar";
    case UiPanelLayer::Reply: return "##reply_editor";
    case UiPanelLayer::Bookmarks: return "##bookmarks_panel_body";
    case UiPanelLayer::Subscriptions: return "##subscriptions_panel_body";
    case UiPanelLayer::Styles: return "##styles_panel_body";
    case UiPanelLayer::StyleManager: return "##style_manager_body";
    case UiPanelLayer::Annotations: return "##annotation_panel_body";
    case UiPanelLayer::Context: return "##context_panel_body";
    case UiPanelLayer::NostrExplorer: return "##nostr_explorer_body";
    case UiPanelLayer::LemmyExplorer: return "##lemmy_explorer_body";
    case UiPanelLayer::Certificates: return "##certificates_body";
    case UiPanelLayer::Identities: return "##identities_body";
    case UiPanelLayer::RelatedSites: return "##related_sites_body";
    case UiPanelLayer::MatrixExplorer: return "##matrix_explorer_body";
    case UiPanelLayer::Chrome: return nullptr;
  }
  return nullptr;
}

// A top native frame owns the pointer unless the ImGui hit belongs to that
// frame's own embedded form. Chrome (menus, dialogs, popups) stays above all
// native panels. nullopt means that no native frame covers the pointer.
inline bool ui_panel_obscured(UiPanelLayer hovered,
                              std::optional<UiPanelLayer> native_top) {
  return native_top && hovered != UiPanelLayer::Chrome &&
         hovered != *native_top;
}

inline UiPanelLayer ui_panel_layer(std::string_view owner) {
  constexpr UiPanelLayer panels[] = {
    UiPanelLayer::FindBar, UiPanelLayer::EditBar,
    UiPanelLayer::Reply, UiPanelLayer::Bookmarks, UiPanelLayer::Subscriptions, UiPanelLayer::Styles,
    UiPanelLayer::StyleManager, UiPanelLayer::Annotations, UiPanelLayer::Context,
    UiPanelLayer::NostrExplorer,
    UiPanelLayer::LemmyExplorer, UiPanelLayer::Certificates,
    UiPanelLayer::Identities, UiPanelLayer::RelatedSites, UiPanelLayer::MatrixExplorer,
  };
  for (const auto layer : panels) {
    const std::string_view prefix(ui_panel_root(layer));
    if (owner == prefix ||
        (owner.size() > prefix.size() &&
         owner.substr(0, prefix.size()) == prefix &&
         owner[prefix.size()] == '/'))
      return layer;
  }
  return UiPanelLayer::Chrome;
}

}  // namespace xanatoo
