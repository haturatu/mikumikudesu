#pragma once

#include <array>
#include <cstdint>

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
    bool imageSequenceExportOpen{};
    bool saveAsOpen{};
    bool resetLayoutRequested{};
    float userScale{1.0F};
    std::array<char, 256> sceneFilter{};
    bool viewportHovered{};
    bool timelineFocused{};
    std::int32_t selectedMaterial{-1};
};

} // namespace dayo::ui
