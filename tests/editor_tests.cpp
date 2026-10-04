#include "editor/editor_session.hpp"
#include "editor/interpolation_window.hpp"
#include "editor/material_window.hpp"
#include "editor/model_commands.hpp"
#include "editor/pose_binding.hpp"
#include "editor/pose_command.hpp"
#include "editor/viewport_math.hpp"
#include "editor/workspace.hpp"
#if DAYO_HAS_IMGUI
#include <imgui.h>
#endif
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
