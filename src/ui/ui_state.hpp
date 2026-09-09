#pragma once

#include <array>

namespace dayo::ui {

enum class Workspace { layout, animation, camera, render, debug };

struct UiState {
    Workspace workspace{Workspace::layout};
    bool sceneVisible{true};
    bool inspectorVisible{true};
    bool timelineVisible{true};
    bool statusBarVisible{true};
    bool performanceVisible{};
    bool fxDebugVisible{};
    bool materialDebugVisible{};
    bool physicsVisible{};
    bool audioExportOpen{};
    bool videoExportOpen{};
    bool layoutDirty{true};
    float userScale{1.0F};
    int layoutVersion{1};
    std::array<char, 256> sceneFilter{};
    bool viewportHovered{};
    bool timelineFocused{};
};

} // namespace dayo::ui
