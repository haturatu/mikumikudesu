#include "ui/i18n.hpp"
#include "ui/i18n_imgui.hpp"
#include "ui/keyframe_edit.hpp"
#include "ui/keyframe_editor.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

void expect(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void checkGroupMoves() {
    dayo::core::VmdMotion motion;
    motion.bones.push_back({.name = "arm", .frame = 3});
    motion.bones.push_back({.name = "arm", .frame = 30});
    motion.morphs.push_back({"smile", 7, .5F});
    motion.gravity.push_back({.frame = 100});
    motion.bones[0].interpolation[7] = 93;
    const std::vector<dayo::core::MotionKeyRef> selection{
        {dayo::core::MotionTrack::bone, 0}, {dayo::core::MotionTrack::morph, 0}, {dayo::core::MotionTrack::bone, 0}};
    expect(dayo::ui::moveKeyframes(motion, selection, -20) == -3, "group move must clamp as one group at zero");
    expect(motion.bones[0].frame == 0 && motion.morphs[0].frame == 4 && motion.bones[1].frame == 30,
           "moving mixed tracks must preserve spacing, unrelated keys, and ignore duplicate refs");
    expect(motion.bones[0].interpolation[7] == 93 && motion.lastFrame == 100,
           "a move must retain interpolation and all other track durations");
    motion.morphs[0].frame = std::numeric_limits<std::uint32_t>::max() - 2;
    expect(dayo::ui::moveKeyframes(motion, selection, 10) == 2 && motion.bones[0].frame == 2,
           "group move must avoid overflow at the upper bound");
}

struct Harness {
    dayo::core::Scene scene;
    dayo::core::CommandHistory history;
    dayo::ui::KeyframeEditor editor;

    Harness() {
        dayo::core::VmdMotion motion;
        motion.bones.push_back({.name = "arm", .frame = 10});
        motion.bones.push_back({.name = "arm", .frame = 30});
        motion.morphs.push_back({"smile", 100, .75F});
        dayo::ui::updateMotionDuration(motion);
        scene.replaceMotion(motion, 0, true);
        editor.synchronize(scene, 0, true);
    }

    dayo::ui::KeyframeEditor::Result frame(ImVec2 mouse, bool down = false, bool ctrl = false, bool escape = false) {
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(mouse.x, mouse.y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
        io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
        io.AddKeyEvent(ImGuiKey_Escape, escape);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({900, 400});
        ImGui::Begin("Timeline test", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
        const auto result = editor.draw(scene, history, 0);
        ImGui::End();
        ImGui::Render();
        return result;
    }

    ImVec2 key(float frameNumber) const {
        const auto origin = editor.canvasOrigin();
        return {origin.x + frameNumber * editor.pixelsPerFrame(), origin.y + 8};
    }
};

void checkCanvasGestures() {
    Harness h;
    h.frame({-10, -10});
    h.frame({-10, -10});
    const auto key = h.key(10);
    h.frame(key);
    const auto click = h.frame(key, true);
    expect(h.editor.selection().size() == 1 && click.seek && *click.seek == 10,
           "clicking a real canvas diamond must select its key and request a seek");
    const auto destination = h.key(20);
    const ImVec2 outside{destination.x, 600};
    h.frame(outside, true);
    expect(h.history.undoCount() == 0 && h.scene.motion(0, true)->bones[0].frame == 10,
           "drag preview must not mutate animation or create undo entries");
    h.frame(outside);
    expect(h.scene.motion(0, true)->bones[0].frame == 20 && h.history.undoCount() == 1,
           "releasing a captured drag must move the key in one undo step");
    expect(h.editor.selection().size() == 1, "selection must survive its own edit");
    expect(h.history.undo(h.scene) && h.scene.motion(0, true)->bones[0].frame == 10,
           "undo must restore the original frame");
    expect(h.history.redo(h.scene) && h.scene.motion(0, true)->bones[0].frame == 20,
           "redo must restore the moved frame");
    h.frame(destination);
    const auto movedKey = h.key(20);
    h.frame(movedKey, true);
    h.frame(h.key(25), true);
    h.frame(h.key(25), true, false, true);
    h.frame(h.key(25));
    expect(h.scene.motion(0, true)->bones[0].frame == 20 && h.history.undoCount() == 1,
           "Escape must cancel a drag without changing animation or history");
    h.frame(h.key(20));
    h.frame(h.key(20), true);
    h.frame(h.key(20));
    h.frame(h.key(30), true, true);
    h.frame(h.key(30), false, true);
    expect(h.editor.selection().size() == 2, "Ctrl-click must add a second canvas key");
    ImGui::GetIO().AddKeyEvent(ImGuiKey_A, true);
    h.frame(h.key(30), false, true);
    expect(h.editor.selection().size() == 3, "Ctrl+A must select all keys in the current track filter");
    ImGui::GetIO().AddKeyEvent(ImGuiKey_A, false);
    dayo::core::VmdMotion unrelated;
    unrelated.morphs.push_back({"other", 5, .5F});
    h.scene.replaceMotion(unrelated, 0, true);
    h.frame(key);
    expect(h.editor.selection().empty(), "external replacement must discard stale key references");
    unrelated.morphs[0].frame = std::numeric_limits<std::uint32_t>::max();
    dayo::ui::updateMotionDuration(unrelated);
    h.scene.replaceMotion(unrelated, 0, true);
    h.frame({-10, -10});
    expect(std::isfinite(h.editor.pixelsPerFrame()) && h.editor.pixelsPerFrame() > 0,
           "the ruler must render the full VMD frame range without overflow or a floating counter loop");
}

void checkLiveLanguageSwitch() {
    Harness h;
    h.frame({-10, -10});
    h.frame({-10, -10});
    h.frame(h.key(10));
    h.frame(h.key(10), true);
    h.frame(h.key(10));
    expect(h.editor.selection().size() == 1, "select a key before changing language");
    ImGuiID stableId{};
    for (const auto candidate : dayo::ui::languages) {
        dayo::ui::setLanguage(candidate);
        dayo::ui::localizeImGui();
        expect(std::string_view(ImGui::LocalizeGetMsg(ImGuiLocKey_DockingHideTabBar)) ==
                   dayo::ui::label("Hide tab bar###HideTabBar"),
               "built-in docking menus must follow the selected language");
        h.frame({-10, -10});
        expect(h.editor.selection().size() == 1 && h.history.undoCount() == 0,
               "language changes must retain selection without modifying motion");
        ImGui::NewFrame();
        ImGui::Begin("Stable ID test");
        const auto id = ImGui::GetID(dayo::ui::label("Bone##InspectorBoneSelector"));
        expect(ImGui::GetID(dayo::ui::label("Viewport##viewport.Layout")) == ImGui::GetID("Viewport##viewport.Layout"),
               "translated window IDs must match previously saved English layouts");
        if (stableId == 0)
            stableId = id;
        expect(id == stableId, "ImGui widget hash must be stable across all four languages");
        ImGui::End();
        ImGui::Render();
    }
    dayo::ui::setLanguage(dayo::ui::Language::english);
}

void checkTranslatedTrackFocus() {
    using namespace dayo::ui;
    Harness h;
    dayo::core::VmdMotion motion;
    dayo::core::VmdCameraKey camera;
    camera.frame = 10;
    motion.cameras.push_back(camera);
    motion.lights.push_back({.frame = 20});
    updateMotionDuration(motion);
    h.scene.replaceMotion(motion, 0, true);
    h.editor.synchronize(h.scene, 0, true);
    ImGuiID previousRowId{};
    for (int row = 0; row < 2; ++row) {
        setLanguage(Language::english);
        h.frame({-10, -10});
        h.frame({-10, -10});
        const ImVec2 labelPosition{30, h.key(10).y + static_cast<float>(row) * h.editor.rowHeight()};
        h.frame(labelPosition);
        h.frame(labelPosition, true);
        const auto id = ImGui::GetActiveID();
        expect(id != 0 && id != previousRowId, "real camera and light labels must have distinct active IDs");
        previousRowId = id;
        for (const auto candidate : languages) {
            setLanguage(candidate);
            h.frame(labelPosition, true);
            expect(ImGui::GetActiveID() == id, "holding a translated track label must retain its active ID");
        }
        setLanguage(Language::english);
        h.frame(labelPosition, true);
        h.frame(labelPosition);
        const auto track = row == 0 ? dayo::core::MotionTrack::camera : dayo::core::MotionTrack::light;
        expect(h.editor.selection().size() == 1 && h.editor.selection().front().track == track,
               "releasing a track label must select its actual camera or light key");
        expect(GImGui->NavId == id, "clicking a track label must establish its navigation focus");
        for (const auto candidate : languages) {
            setLanguage(candidate);
            h.frame({-10, -10});
            expect(GImGui->NavId == id && GImGui->NavIdIsAlive,
                   "the focused track must resubmit the same navigation ID in every language");
            expect(h.history.undoCount() == 0, "switching track label language must not edit motion");
        }
    }
    setLanguage(Language::english);
}

} // namespace

int main() {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = {1000, 700};
    io.DeltaTime = 1.0F / 60.0F;
    io.ConfigInputTrickleEventQueue = false;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    unsigned char* pixels{};
    int width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    int result = 0;
    try {
        checkGroupMoves();
        checkCanvasGestures();
        checkLiveLanguageSwitch();
        checkTranslatedTrackFocus();
        std::cout << "Keyframe selection, captured drag, cancel, undo/redo, bounds and target invalidation passed\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        result = 1;
    }
    ImGui::DestroyContext();
    return result;
}
