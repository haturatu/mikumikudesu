#pragma once

#include <imgui.h>

namespace dayo::ui {

struct ViewportInput {
    bool hovered{};
    bool orbit{};
    bool pan{};
    float wheel{};
    bool contextMenu{};
};

// Own the mouse gesture instead of letting the image's empty space move its window.
class ViewportInteraction {
  public:
    ViewportInput image(ImTextureRef texture, ImVec2 size, bool navigationEnabled) {
        const auto position = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##viewport-navigation", size,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        ImGui::GetWindowDrawList()->AddImage(texture, position, {position.x + size.x, position.y + size.y});
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        const auto& io = ImGui::GetIO();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            rightClickStarted_ = true;
            rightDragged_ = false;
        }
        if (rightClickStarted_ && ImGui::IsMouseDragging(ImGuiMouseButton_Right))
            rightDragged_ = true;
        const bool contextMenu =
            rightClickStarted_ && hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !rightDragged_;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Right))
            rightClickStarted_ = false;
        return {
            hovered,
            navigationEnabled && active && ImGui::IsMouseDragging(ImGuiMouseButton_Left),
            navigationEnabled && active && ImGui::IsMouseDragging(ImGuiMouseButton_Right),
            navigationEnabled && hovered ? io.MouseWheel : 0.0F,
            contextMenu,
        };
    }

  private:
    bool rightClickStarted_{};
    bool rightDragged_{};
};

} // namespace dayo::ui
