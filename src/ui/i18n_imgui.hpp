#pragma once

#include "ui/i18n.hpp"

#include <imgui_internal.h>

#include <iterator>

namespace dayo::ui {

// The editor pins Dear ImGui; localize its built-in docking/table menus as well as app widgets.
inline void localizeImGui() {
    const ImGuiLocEntry entries[]{
        {ImGuiLocKey_TableSizeOne, label("Size column to fit###SizeOne")},
        {ImGuiLocKey_TableSizeAllFit, label("Size all columns to fit###SizeAll")},
        {ImGuiLocKey_TableSizeAllDefault, label("Size all columns to default###SizeAll")},
        {ImGuiLocKey_TableReset, label("Reset")},
        {ImGuiLocKey_TableResetOrder, label("Reset order###ResetOrder")},
        {ImGuiLocKey_TableResetVisibility, label("Reset visibility###ResetVisibility")},
        {ImGuiLocKey_WindowingMainMenuBar, tr("(Main menu bar)")},
        {ImGuiLocKey_WindowingPopup, tr("(Popup)")},
        {ImGuiLocKey_WindowingUntitled, tr("(Untitled)")},
        {ImGuiLocKey_OpenLink_s, tr("Open '%s'")},
        {ImGuiLocKey_CopyLink, label("Copy link###CopyLink")},
        {ImGuiLocKey_DockingHideTabBar, label("Hide tab bar###HideTabBar")},
        {ImGuiLocKey_DockingHoldShiftToDock, tr("Hold Shift to enable window docking.")},
        {ImGuiLocKey_DockingDragToUndockOrMoveNode, tr("Drag to move or undock the whole group.")},
    };
    ImGui::LocalizeRegisterEntries(entries, static_cast<int>(std::size(entries)));
}

} // namespace dayo::ui
