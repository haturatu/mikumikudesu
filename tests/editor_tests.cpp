#include "editor/editor_session.hpp"
#include "editor/interpolation_window.hpp"
#include "editor/model_commands.hpp"
#include "editor/pose_binding.hpp"
#include "editor/viewport_math.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
bool near(float a, float b) {
    return std::abs(a - b) < 0.0001F;
}
void addModel(dayo::core::Scene& scene, dayo::core::ModelId id) {
    dayo::core::ModelInstance model;
    model.id = id;
    model.displayName = "Model" + std::to_string(id);
    model.model = std::make_shared<dayo::core::PmxModel>();
    dayo::core::PmxBone bone;
    bone.name = "Root";
    model.model->bones.push_back(bone);
    bone.name = "Child";
    bone.parent = 0;
    model.model->bones.push_back(bone);
    model.animator = std::make_unique<dayo::core::MmdAnimator>(*model.model);
    scene.restoreModel(std::move(model), scene.models().size());
}
} // namespace
int main() {
    try {
        using namespace dayo;
        core::Scene scene;
        addModel(scene, 1);
        addModel(scene, 2);
        scene.selectModel(1);
        core::CommandHistory history;
        editor::EditorSession session(&scene, &history);
        session.setTarget(1, false);
        core::VmdMotion motion;
        for (const auto frame : {0U, 10U, 20U}) {
            core::VmdBoneKey key;
            key.name = "Root";
            key.frame = frame;
            key.translation[0] = static_cast<float>(frame);
            motion.bones.push_back(key);
        }
        scene.replaceMotion(motion, 1, false);
        session.stableIds().rebuild(core::toMotionDocument(motion));
        const auto first = session.stableIds().keyId(core::MotionTrack::bone, 0);
        const auto middle = session.stableIds().keyId(core::MotionTrack::bone, 1);
        session.selection().set({first});
        session.beginKeyframeDrag();
        session.moveKeyframeDrag(10);
        session.moveKeyframeDrag(20);
        session.commitKeyframeDrag();
        require(history.undoCount() == 1, "drag must coalesce to one undo");
        auto document = core::toMotionDocument(*scene.motion(1, false));
        auto index = session.stableIds().resolve(document, first);
        require(index && document.bones[*index].translation[0] == 0 && document.bones[*index].frame == 20,
                "drag uses original baseline and preserves collision identity");
        require(history.undo(scene), "undo move");
        document = core::toMotionDocument(*scene.motion(1, false));
        index = session.stableIds().resolve(document, first);
        require(index && document.bones[*index].frame == 0, "undo restores stable identity");
        require(history.redo(scene), "redo move");
        document = core::toMotionDocument(*scene.motion(1, false));
        index = session.stableIds().resolve(document, middle);
        require(index && document.bones[*index].frame == 10, "redo does not steal adjacent key");
        session.beginKeyframeDrag();
        session.moveKeyframeDrag(-20);
        session.cancelKeyframeDrag();
        document = core::toMotionDocument(*scene.motion(1, false));
        index = session.stableIds().resolve(document, first);
        require(index && document.bones[*index].frame == 20 && history.undoCount() == 1,
                "escape restores motion and identity without history");
        session.beginKeyframeDrag();
        session.commitKeyframeDrag();
        require(history.undoCount() == 1, "click without move does not add history");
        session.setTarget(2, false);
        session.setTarget(1, false);
        document = core::toMotionDocument(*scene.motion(1, false));
        require(session.stableIds().resolve(document, first).has_value(),
                "model switches preserve each identity table");
        session.selection().set({first});
        editor::InterpolationWindow interpolation;
        editor::CurveEditState curve;
        curve.points = {0, 127, 127, 0};
        curve.method = 1;
        interpolation.setCurve(curve);
        require(interpolation.commit(session, 1) == 1, "per-key interpolation commit");
        require(session.flushOperations() == 1, "queued curve applies");
        document = core::toMotionDocument(*scene.motion(1, false));
        index = session.stableIds().resolve(document, first);
        require(index && document.bones[*index].methods[1] == 1 && document.bones[*index].methods[0] == 0,
                "axis editing preserves other axes");
        require(document.bones[*index].interpolation[5] == 127, "bone curve uses canonical byte offsets");
        require(history.undo(scene), "undo curve");
        require(history.redo(scene), "redo curve");
        editor::PoseBinding binding;
        const auto evaluated = scene.model(1)->animator->evaluate(20);
        binding.observe(1, evaluated);
        binding.synchronize(scene, 20);
        binding.selectBone(0);
        auto* edit = binding.activeEdit();
        require(edit != nullptr, "bone scratch exists");
        edit->translation[1] = 3;
        edit->modified = true;
        binding.synchronize(scene, 20);
        require(near(binding.activeEdit()->translation[1], 3), "scratch survives repeated UI frames");
        require(binding.overrides(1, 21, scene.motionRevision()).empty(),
                "edits cannot leak into another animation frame");
        require(binding.overrides(1, 20, scene.motionRevision()).size() == 1, "modified bone becomes live override");
        binding.revertSelected();
        require(binding.overrides(1, 20, scene.motionRevision()).empty(), "revert removes preview override");
        binding.initializeSelected();
        binding.registerBones(session, 20);
        session.flushOperations();
        require(history.undoNames().front() == "Register bone keys", "register is undoable");
        const auto orderBefore = scene.model(1)->order;
        history.execute(scene,
                        std::make_unique<editor::SetModelOrdersCommand>(scene, std::vector<core::ModelId>{2, 1}, 2));
        require(scene.model(2)->order.postprocess == 0 && scene.model(1)->order.postprocess == 1,
                "D&D normalizes selected stage");
        history.undo(scene);
        require(scene.model(1)->order.postprocess == orderBefore.postprocess, "order undo");
        std::vector<core::ExternalParentLink> links{{2, "Root", 1, "Child"}};
        history.execute(scene, std::make_unique<editor::ExternalParentsCommand>(scene, links));
        auto invalid = links;
        invalid.push_back({1, "Root", 2, "Child"});
        require(!scene.setExternalParents(invalid) && scene.externalParents().size() == 1,
                "invalid cycles leave links atomically unchanged");
        history.execute(scene, std::make_unique<editor::DeleteModelCommand>(scene, 1));
        require(!scene.model(1) && scene.externalParents().empty(), "delete removes dependent runtime links");
        history.undo(scene);
        require(scene.model(1) && scene.externalParents().size() == 1 && scene.selectedModelId() == 1,
                "delete undo restores model, order, selection and links");
        history.redo(scene);
        history.undo(scene);
        const std::array<float, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        const auto screen = editor::projectToViewport({0, 0, 0.5F}, identity, {10, 20, 200, 100});
        require(screen.visible && near(screen.x, 110) && near(screen.y, 70),
                "projection maps renderer-independent viewport origin and dimensions");
        const auto matrix = editor::poseMatrix({1, 2, 3}, {0, 0, 1, 0});
        const auto rotation = editor::matrixRotation(matrix);
        const auto rotated = editor::rotatePoint(rotation, {1, 0, 0});
        require(near(rotated[0], -1) && near(rotated[1], 0), "matrix quaternion roundtrip preserves rotation");
        std::cout << "Editor regression tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
