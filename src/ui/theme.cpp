#include "ui/theme.hpp"

#if DAYO_HAS_IMGUI
#include <imgui.h>

#include <algorithm>

namespace dayo::ui {

void applyEditorTheme(float scale) {
    auto& style = ImGui::GetStyle();
    ImGui::StyleColorsDark(&style);
    style.WindowPadding = {10.0F, 9.0F};
    style.FramePadding = {8.0F, 5.0F};
    style.CellPadding = {6.0F, 4.0F};
    style.ItemSpacing = {7.0F, 6.0F};
    style.ItemInnerSpacing = {6.0F, 4.0F};
    style.ScrollbarSize = 14.0F;
    style.WindowRounding = 0.0F;
    style.ChildRounding = 3.0F;
    style.FrameRounding = 3.0F;
    style.PopupRounding = 3.0F;
    style.GrabRounding = 3.0F;
    style.TabRounding = 3.0F;
    style.ScaleAllSizes(std::max(scale, 0.1F));

    auto& colors = style.Colors;
    colors[ImGuiCol_WindowBg] = {0.075F, 0.085F, 0.100F, 1.0F};
    colors[ImGuiCol_ChildBg] = {0.060F, 0.070F, 0.082F, 1.0F};
    colors[ImGuiCol_PopupBg] = {0.100F, 0.110F, 0.130F, 0.98F};
    colors[ImGuiCol_FrameBg] = {0.135F, 0.150F, 0.175F, 1.0F};
    colors[ImGuiCol_FrameBgHovered] = {0.190F, 0.220F, 0.270F, 1.0F};
    colors[ImGuiCol_FrameBgActive] = {0.220F, 0.260F, 0.320F, 1.0F};
    colors[ImGuiCol_Header] = {0.140F, 0.185F, 0.250F, 1.0F};
    colors[ImGuiCol_HeaderHovered] = {0.190F, 0.260F, 0.350F, 1.0F};
    colors[ImGuiCol_HeaderActive] = {0.220F, 0.310F, 0.420F, 1.0F};
    colors[ImGuiCol_Button] = {0.140F, 0.180F, 0.240F, 1.0F};
    colors[ImGuiCol_ButtonHovered] = {0.200F, 0.280F, 0.370F, 1.0F};
    colors[ImGuiCol_ButtonActive] = {0.240F, 0.340F, 0.440F, 1.0F};
    colors[ImGuiCol_Tab] = {0.105F, 0.135F, 0.180F, 1.0F};
    colors[ImGuiCol_TabHovered] = {0.200F, 0.300F, 0.410F, 1.0F};
    colors[ImGuiCol_TabSelected] = {0.150F, 0.250F, 0.350F, 1.0F};
    colors[ImGuiCol_CheckMark] = {0.350F, 0.700F, 0.950F, 1.0F};
    colors[ImGuiCol_SliderGrab] = {0.300F, 0.580F, 0.820F, 1.0F};
    colors[ImGuiCol_SliderGrabActive] = {0.430F, 0.720F, 0.980F, 1.0F};
    colors[ImGuiCol_DockingEmptyBg] = {0.045F, 0.050F, 0.060F, 1.0F};
    colors[ImGuiCol_DockingPreview] = {0.220F, 0.500F, 0.760F, 0.55F};
}

} // namespace dayo::ui
#endif
