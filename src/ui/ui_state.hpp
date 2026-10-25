#pragma once

#include <array>
#include <cstdint>

namespace dayo::ui {

enum class AssetType { none, model, motion, globalMotion, pose, effect, audio, video, image, environment };

struct AssetSelection {
    AssetType type{AssetType::none};
    std::uint64_t id{};
    friend bool operator==(const AssetSelection&, const AssetSelection&) = default;
};

enum class Workspace { layout, animation, camera, render, debug };

struct WorkspacePanels {
    bool sceneVisible{true};
    bool inspectorVisible{true};
    bool timelineVisible{true};
};

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
    std::array<WorkspacePanels, 5> workspacePanels{
        WorkspacePanels{true, true, true},  WorkspacePanels{true, true, true}, WorkspacePanels{true, true, true},
        WorkspacePanels{false, true, true}, WorkspacePanels{true, true, true},
    };
    std::array<char, 256> sceneFilter{};
    bool viewportHovered{};
    bool timelineFocused{};
    bool sceneFocused{};
    AssetSelection selectedAsset;
    bool inspectScene{};
    std::int32_t selectedMaterial{-1};
};

} // namespace dayo::ui
