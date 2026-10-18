#include "core/editor.hpp"
#include "core/scene.hpp"
#include "editor/editor_session.hpp"
#include "editor/interpolation_window.hpp"
#include "editor/material_window.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

bool check(bool result, std::string_view name) {
    if (!result)
        std::cerr << "FAIL: " << name << '\n';
    return result;
}

} // namespace

int main() {
    using namespace dayo;
    core::Scene scene;
    core::CommandHistory history;
    core::VmdMotion motion;
    core::VmdBoneKey arm;
    arm.name = "arm";
    arm.frame = 7;
    core::VmdBoneKey leg;
    leg.name = "leg";
    leg.frame = 10;
    motion.bones = {arm, leg};
    if (!scene.replaceMotion(std::move(motion), 0, true))
        return 1;

    editor::EditorSession session(&scene, &history);
    session.setTarget(0, true);
    session.stableIds().rebuild(core::toMotionDocument(*scene.motion(0, true)));
    const auto first = session.stableIds().keyId(core::MotionTrack::bone, 0);
    const auto second = session.stableIds().keyId(core::MotionTrack::bone, 1);

    editor::InterpolationWindow window;
    window.setTarget(first.stableId, 0);
    window.setPoints({12, 32, 92, 120});
    window.setMethod(1);
    window.commitMethod(session);

    bool ok = check(session.operations().size() == 1, "edit queued exactly once");
    ok &= check(session.flushOperations() == 1, "edit applied through command history");
    const auto& updated = scene.motion(0, true)->bones;
    ok &= check(updated[0].interpolation[0] == 12 && updated[0].interpolation[4] == 32 &&
                    updated[0].interpolation[8] == 92 && updated[0].interpolation[12] == 120 &&
                    updated[0].methods[0] == 1,
                "selected bone encoded in channel/point layout");
    ok &= check(updated[1].interpolation[0] != 12, "unselected key unchanged");
    window.commitMethod(session);
    ok &= check(session.operations().empty(), "unchanged edit creates no undo entry");
    ok &= check(history.undoCount() == 1, "only changed edit reaches history");
    ok &= check(history.undo(scene), "edit undo succeeds");
    ok &= check(scene.motion(0, true)->bones[0].interpolation[0] != 12, "undo restores interpolation");
    ok &= check(history.redo(scene), "edit redo succeeds");
    ok &= check(scene.motion(0, true)->bones[0].interpolation[0] == 12, "redo restores interpolation");

    session.selection().set({first, second});
    window.setMethod(2);
    window.setPoints({18, 40, 80, 105});
    window.commitMethod(session);
    ok &= check(session.flushOperations() == 1, "multi-key edit applied");
    const auto& selected = scene.motion(0, true)->bones;
    ok &= check(selected[0].interpolation[0] == 18 && selected[1].interpolation[0] == 18 &&
                    selected[0].methods[0] == 2 && selected[1].methods[0] == 2,
                "selected keys both changed");

    editor::MaterialWindow material;
    material.setEntries({{"roughness", 0.5F}});
    material.queueMaterialEdit(session);
    ok &= check(session.operations().empty(), "unimplemented material edit cannot forge motion history");
    return ok ? 0 : 1;
}
