#include "editor/camera_recording.hpp"
#include "editor/editor_session.hpp"
#include "editor/interpolation_window.hpp"
#include "editor/keyframe_window.hpp"
#include "editor/material_window.hpp"
#include "editor/model_commands.hpp"
#include "editor/pose_binding.hpp"
#include "editor/pose_command.hpp"
#include "editor/viewport_math.hpp"
#include "editor/workspace.hpp"
#if DAYO_HAS_IMGUI
#include <imgui.h>
#endif
#include <algorithm>
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
    bone.flags = 0x001EU;
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
        {
            core::MotionDocument visibility;
            visibility.ik.push_back({0, true, {{"IK-A", false}, {"IK-B", true}}});
            visibility.ik.push_back({60, true, {{"IK-A", true}}});
            core::MotionEditor::registerVisibility(visibility, 30, false);
            const auto registered = std::ranges::find(visibility.ik, 30U, &core::VmdIkKey::frame);
            require(registered != visibility.ik.end() && !registered->visible && registered->states.size() == 2 &&
                        !registered->states[0].enabled && registered->states[1].enabled,
                    "new visibility key inherits effective IK states instead of re-enabling IK");
            core::MotionEditor::registerVisibility(visibility, 30, true);
            require(visibility.ik.size() == 3 && !visibility.ik[1].states[0].enabled,
                    "updating visibility preserves same-frame IK states");
        }
        {
            const core::PreviewNormalization from{{10, 2, -3}, 0.1F};
            const core::PreviewNormalization to{{0, -4, 5}, 1.0F};
            core::PmxVertex parentPoint;
            core::PmxVertex childPoint;
            parentPoint.position = {20, 6, 7};
            childPoint.position = editor::convertModelPoint(parentPoint.position, from, to);
            std::vector<core::PmxVertex> parentVertices{parentPoint};
            std::vector<core::PmxVertex> childVertices{childPoint};
            core::normalizeForPreview(parentVertices, from);
            core::normalizeForPreview(childVertices, to);
            for (std::size_t axis = 0; axis < 3; ++axis)
                require(near(parentVertices[0].position[axis], childVertices[0].position[axis]),
                        "conversion preserves common preview position with distinct centers/scales");
            core::Scene attachmentScene;
            addModel(attachmentScene, 1);
            addModel(attachmentScene, 2);
            auto* parentModel = attachmentScene.model(1);
            auto* childModel = attachmentScene.model(2);
            core::VmdMotion parentMotion;
            core::VmdMotion childMotion;
            parentMotion.bones.push_back({"Root", 0, {20, 6, 7}});
            childMotion.bones.push_back({"Root", 0, {5, 0, 0}});
            parentModel->animator->setMotion(&parentMotion);
            childModel->animator->setMotion(&childMotion);
            const auto parentPose = parentModel->animator->evaluate(0).bones[0];
            const std::array replacement{mmd::ExternalParentTransform{
                .childBone = 1,
                .parentPosition = editor::convertModelPoint(parentPose.worldPosition, from, to),
                .parentRotation = parentPose.rotation}};
            const auto attached = childModel->animator->evaluate(0, 0, false, {}, {}, replacement);
            const auto attachedGpu = childModel->animator->evaluate(0, 0, true, {}, {}, replacement);
            childPoint.position = attached.bones[1].worldPosition;
            childVertices = {childPoint};
            core::normalizeForPreview(childVertices, to);
            for (std::size_t axis = 0; axis < 3; ++axis) {
                require(near(parentVertices[0].position[axis], childVertices[0].position[axis]),
                        "reparented child meets parent in preview despite differing model normalization");
                require(near(attached.bones[1].worldPosition[axis], attachedGpu.bones[1].worldPosition[axis]),
                        "normalized attachment has CPU/GPU pose parity");
            }
        }
        {
            core::Scene trackScene;
            addModel(trackScene, 1);
            addModel(trackScene, 2);
            core::CommandHistory trackHistory;
            trackHistory.execute(trackScene,
                                 std::make_unique<editor::ExternalParentsCommand>(
                                     trackScene, std::vector<core::ExternalParentLink>{{1, "Root", 2, "Child"}}));
            require(trackScene.externalParents().empty(), "authored links have no stale static fallback");
            auto trackDocument = core::toMotionDocument(*trackScene.motion(2, false));
            core::MotionEditor::registerVisibility(trackDocument, 0, false);
            trackScene.replaceMotion(core::toVmdMotion(trackDocument), 2, false);
            editor::EditorSession trackSession(&trackScene, &trackHistory);
            trackSession.setTarget(2, false);
            editor::KeyframeWindow trackWindow;
            trackWindow.refresh(trackSession);
            require(trackWindow.rows().size() == 2, "visibility and external-parent keys both appear in Timeline");
            const auto visibilityId = trackSession.stableIds().keyId(core::MotionTrack::ik, 0);
            const auto parentId = trackSession.stableIds().keyId(core::MotionTrack::externalParent, 0);
            trackSession.selection().selectGroup({visibilityId, parentId}, false, false);
            require(trackSession.selection().size() == 2, "overlapping diamond selects both IDs");
            trackSession.selection().selectGroup({visibilityId, parentId}, false, true);
            require(trackSession.selection().empty(), "Ctrl toggles both overlapping IDs");
            trackSession.selection().selectGroup({visibilityId, parentId}, true, false);
            const auto refs = trackSession.selection().resolveTransient(trackDocument, trackSession.stableIds());
            const auto clip = core::MotionEditor::copy(trackDocument, refs);
            require(!clip.empty() && clip.keys.externalParents.size() == 1 && clip.keys.ik.size() == 1,
                    "copy includes external parent and visibility tracks");
            core::MotionEditor::erase(trackDocument, refs);
            trackSession.operations().push(
                editor::ReplaceMotionOperation{2, false, core::toVmdMotion(trackDocument), "Cut keys"});
            trackSession.flushOperations();
            require(trackScene.effectiveExternalParents(30).empty(),
                    "cut removes attachment without static resurrection");
            trackDocument = core::toMotionDocument(*trackScene.motion(2, false));
            const auto pastedRefs = core::MotionEditor::paste(trackDocument, clip, 30);
            require(pastedRefs.size() == 2, "paste returns both track references");
            trackSession.operations().push(
                editor::ReplaceMotionOperation{2, false, core::toVmdMotion(trackDocument), "Paste keys"});
            trackSession.flushOperations();
            trackWindow.refresh(trackSession);
            require(trackScene.effectiveExternalParents(29).empty() &&
                        trackScene.effectiveExternalParents(30).size() == 1 && trackScene.timeline().duration >= 30,
                    "paste at 30 changes runtime attachment at the new frame");
            const auto pastedParent = trackSession.stableIds().keyId(core::MotionTrack::externalParent, 0);
            const auto pastedVisibility = trackSession.stableIds().keyId(core::MotionTrack::ik, 0);
            trackSession.selection().set({pastedParent, pastedVisibility});
            trackSession.beginKeyframeDrag();
            trackSession.moveKeyframeDrag(15);
            trackSession.commitKeyframeDrag();
            require(trackScene.effectiveExternalParents(30).empty() &&
                        trackScene.effectiveExternalParents(45).size() == 1,
                    "group drag moves external-parent runtime timing");
            trackHistory.undo(trackScene);
            trackDocument = core::toMotionDocument(*trackScene.motion(2, false));
            const auto restored = trackSession.stableIds().resolve(trackDocument, pastedParent);
            require(restored && trackDocument.externalParents[*restored].frame == 30,
                    "Undo restores external-parent stable identity");
            core::MotionEditor::erase(trackDocument, {{core::MotionTrack::externalParent, *restored}});
            trackSession.operations().push(
                editor::ReplaceMotionOperation{2, false, core::toVmdMotion(trackDocument), "Delete parent key"});
            trackSession.flushOperations();
            require(trackScene.effectiveExternalParents(100).empty(), "Delete removes the final attachment key");
            trackHistory.undo(trackScene);
            auto& eligibleModel = *trackScene.model(2)->model;
            require(core::externalParentEligible(eligibleModel, 1), "movable child is eligible");
            eligibleModel.bones[1].flags = 0;
            std::string eligibilityError;
            require(!trackScene.addExternalParent({1, "Root", 2, "Child"}, &eligibilityError) &&
                        !eligibilityError.empty() && trackScene.effectiveExternalParents(30).empty(),
                    "immovable child rejected in Scene and imported keys");
            eligibleModel.bones[1].flags = 0x001EU;
            core::PmxRigidBody drivenBody;
            drivenBody.bone = 1;
            drivenBody.mode = 1;
            eligibleModel.rigidBodies.push_back(drivenBody);
            require(!core::externalParentEligible(eligibleModel, 1) &&
                        !trackScene.addExternalParent({1, "Root", 2, "Child"}),
                    "physics-driven child cannot be attached");
            eligibleModel.rigidBodies[0].mode = 0;
            require(core::externalParentEligible(eligibleModel, 1), "kinematic rigid body remains eligible");
        }
        core::Scene scene;
        addModel(scene, 1);
        addModel(scene, 2);
        scene.selectModel(1);
        core::CommandHistory history;
        const auto rootPosition = history.position();
        history.execute(scene, std::make_unique<core::SetFrameCommand>(0, 10));
        const auto savedPosition = history.position();
        history.execute(scene, std::make_unique<core::SetFrameCommand>(10, 20));
        const auto discardedPosition = history.position();
        require(history.undo(scene) && history.position() == savedPosition, "Undo returns to savepoint");
        require(history.redo(scene) && history.position() == discardedPosition, "Redo restores state identity");
        history.undo(scene);
        history.execute(scene, std::make_unique<core::SetFrameCommand>(10, 30));
        require(history.position() != discardedPosition && !history.canRedo(), "new branch has a unique position");
        history.undo(scene);
        require(history.position() == savedPosition, "branching preserves an earlier savepoint");
        history.undo(scene);
        require(history.position() == rootPosition, "Undo reaches original clean state");
        history.clear();
        require(history.position() != rootPosition, "loading a new project creates a fresh history root");
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
        auto physicsMotion = *scene.motion(1, false);
        for (auto& key : physicsMotion.bones)
            if (key.name == "Root")
                key.physics = false;
        scene.replaceMotion(std::move(physicsMotion), 1, false);
        const auto evaluated = scene.model(1)->animator->evaluate(20);
        require(!evaluated.bones[0].inputPhysics, "animator exposes the current authoring physics flag");
        binding.observe(1, evaluated);
        binding.synchronize(scene, 20);
        binding.selectBone(0);
        auto* edit = binding.activeEdit();
        require(edit != nullptr, "bone scratch exists");
        require(!edit->physics, "scratch loads sampled inputPhysics without re-sampling motion keys");
        edit->translation[1] = 3;
        edit->modified = true;
        binding.synchronize(scene, 20);
        require(near(binding.activeEdit()->translation[1], 3), "scratch survives repeated UI frames");
        require(binding.overrides(1, 21, scene.motionRevision()).empty(),
                "edits cannot leak into another animation frame");
        require(binding.overrides(1, 20, scene.motionRevision()).size() == 1, "modified bone becomes live override");
        require(!binding.overrides(1, 20, scene.motionRevision())[0].physics,
                "position-only editing preserves disabled key physics in the live override");
        binding.setPhysicsSelected(true);
        binding.revertSelected();
        require(binding.overrides(1, 20, scene.motionRevision()).empty(), "revert removes preview override");
        require(!binding.activeEdit()->physics, "Revert restores the sampled physics flag");
        binding.initializeSelected();
        binding.registerBones(session, 20);
        session.flushOperations();
        require(history.undoNames().front() == "Register bone keys", "register is undoable");
        const auto registeredPhysics = core::toMotionDocument(*scene.motion(1, false));
        const auto registeredBone = std::ranges::find_if(
            registeredPhysics.bones, [](const auto& key) { return key.name == "Root" && key.frame == 20; });
        require(registeredBone != registeredPhysics.bones.end() && !registeredBone->physics,
                "Register preserves the sampled disabled physics flag");
        core::VpdPose pose;
        pose.bones.push_back({"Root", {4, 0, 0}});
        pose.bones.push_back({"Child", {0, 2, 0}});
        scene.model(1)->pose = std::make_unique<core::VpdPose>(pose);
        scene.model(1)->animator->setPose(scene.model(1)->pose.get());
        auto registered = *scene.motion(1, false);
        auto& poseIds = session.stableIds(1, false);
        poseIds.rebuild(core::toMotionDocument(registered));
        const auto registeredId = poseIds.keyId(core::MotionTrack::bone, 0);
        history.execute(scene, std::make_unique<editor::RegisterPoseCommand>(
                                   *scene.model(1), registered, std::vector<std::string>{"Root"}, &poseIds));
        require(scene.model(1)->pose && scene.model(1)->pose->bones.size() == 1 &&
                    scene.model(1)->pose->bones[0].name == "Child",
                "bone registration removes only corresponding persistent VPD overrides");
        history.undo(scene);
        require(scene.model(1)->pose->bones.size() == 2, "pose register undo restores VPD");
        require(poseIds.keyId(core::MotionTrack::bone, 0) == registeredId,
                "pose registration undo preserves existing key identity");
        history.redo(scene);
        scene.model(1)->materialSettings.resize(1);
        editor::MaterialWindow material;
        material.setEntries({{"roughness", 0.7F}});
        material.queueMaterialEdit(session);
        session.flushOperations();
        require(scene.model(1)->materialSettings[0].parameters.find("roughness") != nullptr,
                "material editor changes actual parameters");
        history.undo(scene);
        require(scene.model(1)->materialSettings[0].parameters.find("roughness") == nullptr,
                "material parameter edit is undoable without a fake motion mutation");
        const auto materialSavepoint = history.position();
        auto materialState = scene.model(1)->materialSettings[0];
        materialState.previewPbrPreset = 3;
        materialState.annotation = "example.fx";
        session.operations().push(editor::MaterialEditOperation{1, 0, materialState});
        session.flushOperations();
        require(history.position() != materialSavepoint && scene.model(1)->materialSettings[0].previewPbrPreset == 3,
                "PBR and annotation changes enter persistent history");
        history.undo(scene);
        require(history.position() == materialSavepoint && scene.model(1)->materialSettings[0].annotation.empty(),
                "Undo material changes returns to savepoint");
        auto numericOrder = scene.model(1)->order;
        numericOrder.motion = 7;
        history.execute(scene, std::make_unique<editor::SetModelOrderCommand>(1, scene.model(1)->order, numericOrder));
        require(history.position() != materialSavepoint && scene.model(1)->order.motion == 7,
                "legacy numeric order is dirty");
        history.undo(scene);
        require(history.position() == materialSavepoint && scene.model(1)->order.motion != 7,
                "numeric order undo is clean");
        const auto orderBefore = scene.model(1)->order;
        history.execute(scene,
                        std::make_unique<editor::SetModelOrdersCommand>(scene, std::vector<core::ModelId>{2, 1}, 2));
        require(scene.model(2)->order.postprocess == 0 && scene.model(1)->order.postprocess == 1,
                "D&D normalizes selected stage");
        history.undo(scene);
        require(scene.model(1)->order.postprocess == orderBefore.postprocess, "order undo");
        std::vector<core::ExternalParentLink> links{{2, "Root", 1, "Child"}};
        history.execute(scene, std::make_unique<editor::ExternalParentsCommand>(scene, links));
        require(scene.effectiveExternalParents(0).size() == 1,
                "external parent registration persists an effective key");
        scene.setExternalParents({});
        require(scene.effectiveExternalParents(0).size() == 1, "reloaded motion-only attachment remains effective");
        history.execute(scene, std::make_unique<editor::ExternalParentsCommand>(
                                   scene, std::vector<core::ExternalParentLink>{}, 10));
        require(scene.effectiveExternalParents(0).size() == 1 && scene.effectiveExternalParents(10).empty(),
                "removing a motion-only attachment registers a frame-specific unlink");
        history.undo(scene);
        scene.setExternalParents(links);
        auto invalid = links;
        invalid.push_back({1, "Root", 2, "Child"});
        require(!scene.setExternalParents(invalid) && scene.externalParents().size() == 1,
                "invalid cycles leave links atomically unchanged");
        core::EffectGraph ownedEffect;
        ownedEffect.category = "deform";
        const auto ownedEffectId = scene.addEffect(ownedEffect, 1);
        history.execute(scene, std::make_unique<editor::DeleteModelCommand>(scene, 1));
        require(scene.effects().deform.empty(), "deleting a model removes its owned effect");
        require(!scene.model(1) && scene.externalParents().empty(), "delete removes dependent runtime links");
        history.undo(scene);
        require(scene.model(1) && scene.externalParents().size() == 1 && scene.selectedModelId() == 1,
                "delete undo restores model, order, selection and links");
        require(scene.effects().deform.size() == 1 && scene.effects().deform.front().id == ownedEffectId,
                "deletion undo restores the owned effect and its identity");
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
        core::VmdMotion references;
        core::VmdCameraKey tracking;
        tracking.parentModel = 3;
        references.cameras.push_back(tracking);
        references.externalParents.push_back({0, 3, "Root", "Child"});
        references.externalParents.push_back({10, 1, "Root", "Child"});
        const auto saved = editor::remapMotionModels(references, {{2, 1}, {3, 2}});
        require(saved.cameras[0].parentModel == 2 && saved.externalParents[0].parentModel == 2 &&
                    saved.externalParents[1].parentModel == -1,
                "project remaps live IDs and unlinks deleted parents");
        const auto loaded = editor::remapMotionModels(saved, {{1, 8}, {2, 9}});
        require(loaded.cameras[0].parentModel == 9 && loaded.externalParents[0].parentModel == 9 &&
                    loaded.externalParents[1].parentModel == -1,
                "project restores camera and external-parent targets");
        core::Scene recordingScene;
        core::VmdMotion recordingBefore;
        tracking.frame = 30;
        recordingBefore.cameras.push_back(tracking);
        recordingBefore.lastFrame = 30;
        recordingScene.replaceMotion(recordingBefore, 0, true);
        const auto recordingEnd = recordingScene.timeline().duration;
        core::CommandHistory recordingHistory;
        {
            editor::UndoTransaction recording(recordingScene, recordingHistory, 0, true, "Record camera range");
            recording.dragTo({});
            recordingScene.setTimelineDuration(recordingEnd);
            require(recordingScene.advanceFrame(0.5F, true, false) && near(recordingScene.timeline().frame, 15),
                    "camera-only recording advances after replacement removes all original keys");
            core::VmdMotion partial;
            tracking.frame = 15;
            partial.cameras.push_back(tracking);
            partial.lastFrame = 15;
            recording.dragTo(partial);
            recordingScene.setTimelineDuration(recordingEnd);
            recordingScene.advanceFrame(1.0F, true, false);
            require(near(recordingScene.timeline().frame, 30), "recording clamps to fixed end without wrapping");
            recording.commit();
        }
        require(recordingHistory.undoCount() == 1, "camera range recording creates one history entry");
        recordingHistory.undo(recordingScene);
        require(recordingScene.cameraMotion()->cameras[0].frame == 30, "recording undo restores original range");
        {
            editor::StableIdTable cameraIds;
            editor::CameraRecording recording(recordingScene, recordingHistory, cameraIds, 0, 30);
            auto key = tracking;
            for (std::uint32_t frame = 0; frame < 10; ++frame) {
                key.frame = frame;
                require(!recording.update(key), "advancing frames alone never records camera keys");
            }
            require(recordingScene.cameraMotion()->cameras.empty(), "no automatic keys during recording");
            key.frame = 10;
            recording.requestKey();
            require(recording.update(key), "Space records the current frame");
            key.position[0] = 3;
            recording.requestKey();
            require(recording.update(key), "another Space at the same frame replaces its key");
            key.frame = 20;
            recording.requestKey();
            require(recording.update(key), "Space can be pressed repeatedly without stopping recording");
            key.frame = 31;
            recording.requestKey();
            require(!recording.update(key), "out-of-range request is consumed without clamping to the endpoint");
            key.frame = 30;
            require(!recording.update(key), "consumed request does not leak to another frame");
            editor::Selection recordedSelection;
            recordedSelection.set(recording.finish());
            const auto recordedDocument = core::toMotionDocument(*recordingScene.cameraMotion());
            const auto refs = recordedSelection.resolveTransient(recordedDocument, cameraIds);
            require(refs.size() == 2 && recordedDocument.cameras.size() == 2,
                    "completion selects only recorded stable IDs");
            require(recordedDocument.cameras[0].frame == 10 && recordedDocument.cameras[1].frame == 20,
                    "only requested frames are stored");
            require(recordingHistory.undoCount() == 1, "sparse recording remains one undo transaction");
            recordingHistory.undo(recordingScene);
            require(recordingScene.cameraMotion()->cameras[0].frame == 30, "Undo restores overwritten camera range");
            const auto recordingSavepoint = recordingHistory.position();
            editor::CameraRecording empty(recordingScene, recordingHistory, cameraIds, 0, 30);
            require(empty.finish().empty() && recordingHistory.position() == recordingSavepoint,
                    "recording without Space rolls back and preserves the savepoint");
            require(recordingScene.cameraMotion()->cameras[0].frame == 30, "empty recording preserves original keys");
        }
        editor::StableIdTable identityIds;
        auto original = core::toMotionDocument(motion);
        identityIds.rebuild(original);
        const auto snapshot = identityIds;
        auto inserted = original;
        core::VmdBoneKey extra;
        extra.name = "extra";
        extra.frame = 50;
        inserted.bones.push_back(extra);
        identityIds.rebuild(inserted);
        const auto erasedId = identityIds.keyId(core::MotionTrack::bone, 3);
        identityIds = snapshot;
        extra.name = "different";
        original.bones.push_back(extra);
        identityIds.rebuild(original);
        require(identityIds.keyId(core::MotionTrack::bone, 3) != erasedId,
                "branching after undo never recycles removed key IDs");
#if DAYO_HAS_IMGUI
        {
            const std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> context(ImGui::CreateContext(),
                                                                                          &ImGui::DestroyContext);
            auto& io = ImGui::GetIO();
            io.DisplaySize = {800, 600};
            io.DeltaTime = 1.0F / 60;
            unsigned char* pixels = nullptr;
            int width = 0, height = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
            require(pixels && width > 0 && height > 0, "headless editor font atlas builds");
            editor::Workspace workspace;
            core::AnimatedModelFrame frame;
            frame.bones.resize(2);
            frame.bones[0].worldPosition = {0, 0, 0.5F};
            frame.bones[1].worldPosition = {0.5F, 0, 0.5F};
            binding = {};
            scene.selectModel(1);
            binding.observe(1, frame);
            binding.synchronize(scene, 0);
            graphics::SceneCameraMatrices camera;
            camera.view = identity;
            camera.projection = identity;
            camera.viewProjection = identity;
            const auto draw = [&] {
                ImGui::NewFrame();
                ImGui::SetNextWindowPos({0, 0});
                ImGui::SetNextWindowSize({800, 600});
                ImGui::Begin("Editor test viewport", nullptr, ImGuiWindowFlags_NoTitleBar);
                workspace.drawViewport(binding, *scene.model(1), camera, {50, 50, 200, 200}, true);
                ImGui::End();
                ImGui::Render();
            };
            draw();
            draw();
            io.AddMousePosEvent(200, 150);
            io.AddMouseButtonEvent(0, true);
            draw();
            draw();
            require(binding.activeBone() == 1, "viewport marker click selects closest bone");
            io.AddMouseButtonEvent(0, false);
            draw();
            draw();
            io.AddMousePosEvent(130, 130);
            io.AddMouseButtonEvent(0, true);
            draw();
            draw();
            io.AddMousePosEvent(210, 170);
            draw();
            draw();
            io.AddMouseButtonEvent(0, false);
            draw();
            draw();
            require(binding.selectedBones().size() == 2, "viewport rectangle selects multiple bone markers");
            require(ImGui::GetDrawData() && ImGui::GetDrawData()->TotalVtxCount > 0,
                    "editor overlay produces visible draw geometry");
        }
#endif
        std::cout << "Editor regression tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
