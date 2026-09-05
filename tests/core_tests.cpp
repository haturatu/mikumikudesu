#include "core/animation.hpp"
#include "core/asset.hpp"
#include "core/audio_export.hpp"
#include "core/editor.hpp"
#include "core/image.hpp"
#include "core/media.hpp"
#include "core/model_probe.hpp"
#include "core/motion.hpp"
#include "core/output.hpp"
#include "core/physics.hpp"
#include "core/project.hpp"
#include "core/scene.hpp"
#include "core/video_export.hpp"
#include "core/vmdayo.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <string_view>

namespace {

template <typename T> void append(std::ofstream& output, T value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void appendText(std::ofstream& output, std::string_view text) {
    append(output, static_cast<std::int32_t>(text.size()));
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

bool check(bool value, std::string_view message) {
    if (!value)
        std::cerr << "FAIL: " << message << '\n';
    return value;
}

} // namespace

int main() {
    using dayo::core::AssetKind;
    bool ok = true;
    ok &= check(dayo::core::classifyAsset("Miku.PMX") == AssetKind::pmx, "PMX extension");
    ok &= check(dayo::core::classifyAsset("motion.vmd") == AssetKind::vmd, "VMD extension");
    ok &= check(dayo::core::classifyAsset("sound.M4A") == AssetKind::audio, "audio extension");
    ok &= check(dayo::core::classifyAsset("movie.webm") == AssetKind::video, "video extension");
    {
        dayo::core::MotionDocument document;
        document.bones.push_back({.name = "arm", .frame = 5});
        document.morphs.push_back({"smile", 10, 0.75F});
        const std::vector<dayo::core::MotionKeyRef> selected{
            {dayo::core::MotionTrack::bone, 0},
            {dayo::core::MotionTrack::morph, 0},
        };
        const auto clipboard = dayo::core::MotionEditor::copy(document, selected);
        const auto pasted = dayo::core::MotionEditor::paste(document, clipboard, 20);
        ok &= check(pasted.size() == 2 && document.bones.size() == 2 && document.morphs.size() == 2 &&
                        document.bones.back().frame == 20 && document.morphs.back().frame == 25,
                    "motion editor copies and offsets mixed tracks");
        dayo::core::MotionEditor::move(document, {{dayo::core::MotionTrack::bone, 0}}, -20);
        ok &= check(document.bones.front().frame == 0, "motion editor clamps moved keys at frame zero");
        dayo::core::MotionEditor::erase(document, {{dayo::core::MotionTrack::morph, 0}});
        ok &= check(document.morphs.size() == 1, "motion editor deletes selected keys");
    }
    {
        dayo::core::VmdMotion cameraMotion;
        dayo::core::VmdCameraKey first;
        first.frame = 0;
        first.distance = -4.0F;
        first.viewAngle = 30;
        dayo::core::VmdCameraKey second;
        second.frame = 10;
        second.distance = -8.0F;
        second.position = {2.0F, 4.0F, 6.0F};
        second.viewAngle = 50;
        for (std::size_t channel = 0; channel < 6; ++channel) {
            second.interpolation[channel * 4] = 20;
            second.interpolation[channel * 4 + 1] = 107;
            second.interpolation[channel * 4 + 2] = 20;
            second.interpolation[channel * 4 + 3] = 107;
        }
        cameraMotion.cameras = {first, second};
        const auto camera = dayo::core::evaluateCamera(cameraMotion, 5.0F);
        ok &= check(std::abs(camera.distance + 6.0F) < 0.001F && std::abs(camera.position[1] - 2.0F) < 0.001F &&
                        std::abs(camera.viewAngle - 40.0F) < 0.001F,
                    "VMD camera interpolation");
        ok &= check(std::abs(dayo::core::catmullRom(0.0F, 1.0F, 2.0F, 3.0F, 0.5F) - 1.5F) < 0.001F,
                    "Catmull-Rom interpolation");
        cameraMotion.interpolation = dayo::core::InterpolationMode::catmullRom;
        const auto catmull = dayo::core::evaluateCamera(cameraMotion, 5.0F);
        ok &= check(std::isfinite(catmull.distance), "Catmull-Rom camera evaluation");
    }
    try {
        const auto projectPath = std::filesystem::temp_directory_path() / "mikumikudesu-project-test.dayo";
        dayo::core::DayoProject project;
        project.renderer = "subayai";
        project.frame = 42.5F;
        project.playing = false;
        project.embeddedVmdayo = {0x56, 0x4D, 0x44, 0x01};
        project.assets.push_back({"pmx", std::filesystem::absolute("model.pmx")});
        project.assets.push_back({"vmd", std::filesystem::absolute("motion.vmd")});
        dayo::core::saveProject(projectPath, project);
        const auto loaded = dayo::core::loadProject(projectPath);
        ok &= check(loaded.renderer == "subayai" && loaded.frame == 42.5F && !loaded.playing,
                    ".dayo project settings round trip");
        ok &=
            check(loaded.assets.size() == 2 && loaded.assets[0].path.is_absolute(), ".dayo relative asset round trip");
        ok &= check(loaded.version == 3, ".dayo v3 writer");
        ok &= check(loaded.embeddedVmdayo == project.embeddedVmdayo, ".dayo embedded VMdayo payload");
        project.embeddedVmdayo.clear();
        project.embeddedMotion.emplace();
        project.embeddedMotion->modelName = "Miku";
        project.embeddedMotion->morphs.push_back({"smile", 24, 1.0F});
        dayo::core::saveProject(projectPath, project);
        const auto editableProject = dayo::core::loadProject(projectPath);
        ok &= check(editableProject.embeddedMotion && editableProject.embeddedMotion->morphs.size() == 1 &&
                        editableProject.embeddedMotion->morphs[0].frame == 24,
                    ".dayo v3 editable VMdayo motion round trip");
        ok &= check(editableProject.embeddedMotions.size() == 2 &&
                        editableProject.embeddedMotions[0].modelName == "Camera/Light",
                    ".dayo v3 camera and model subsets round trip");
        {
            std::ifstream saved(projectPath, std::ios::binary);
            const std::string contents((std::istreambuf_iterator<char>(saved)), std::istreambuf_iterator<char>());
            ok &= check(contents.find("\"MikuMikuDayo\"") != std::string::npos &&
                            contents.find("\"cereal_class_version\": 3") != std::string::npos,
                        "official .dayo v3 JSON header");
        }
        {
            std::ofstream legacy(projectPath, std::ios::binary | std::ios::trunc);
            legacy << "[MikuMikuDayo]\n"
                      "{\"MikuMikuDayo\":{\"ver\":1,\"assetPath\":\".\","
                      "\"editor\":{\"frame\":12},\"models\":[{\"filename\":\"legacy.pmx\"}],"
                      "\"fxinfo\":[]}}\n[BinaryDayo]\n";
            for (int section = 0; section < 7; ++section)
                append(legacy, std::int32_t{0});
            append(legacy, std::int32_t{1});
            appendText(legacy, "Bone");
            append(legacy, std::int32_t{7});
            const std::array<float, 3> translation{1.0F, 2.0F, 3.0F};
            const std::array<float, 4> rotation{0.0F, 0.0F, 0.0F, 1.0F};
            const std::array<std::uint8_t, 16> interpolation{};
            legacy.write(reinterpret_cast<const char*>(translation.data()), sizeof(translation));
            legacy.write(reinterpret_cast<const char*>(rotation.data()), sizeof(rotation));
            legacy.write(reinterpret_cast<const char*>(interpolation.data()), sizeof(interpolation));
            append(legacy, std::uint8_t{1});
            for (int section = 0; section < 6; ++section)
                append(legacy, std::int32_t{0});
        }
        const auto legacy = dayo::core::loadProject(projectPath);
        ok &= check(legacy.frame == 12.0F && legacy.assets.size() == 1 && legacy.embeddedMotion &&
                        legacy.embeddedMotion->bones.size() == 1 && legacy.embeddedMotion->bones[0].frame == 7,
                    "legacy .dayo binary keyframe import");
        if (legacy.embeddedMotion) {
            dayo::core::Scene embeddedScene;
            embeddedScene.attachMotion(*legacy.embeddedMotion);
            ok &= check(embeddedScene.timeline().duration == 7.0F,
                        "embedded .dayo motion attachment updates timeline duration");
        }
        std::error_code projectError;
        std::filesystem::remove(projectPath, projectError);
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: project: " << exception.what() << '\n';
        ok = false;
    }

    try {
        const auto vmdayoPath = std::filesystem::temp_directory_path() / "mikumikudesu-motion.vmdayo";
        dayo::core::VmdayoDocument document;
        document.modelName = "Miku";
        document.modelDictionary.emplace(7, "Parent Model");
        document.metadata.emplace("author", "mikumikudesu");
        document.motion.interpolation = dayo::core::InterpolationMode::catmullRom;
        dayo::core::VmdBoneKey physicsBone;
        physicsBone.name = "arm";
        physicsBone.frame = 9;
        physicsBone.physics = false;
        physicsBone.methods = {1, 0, 1, 0};
        document.motion.bones.push_back(physicsBone);
        document.motion.morphs.push_back({"smile", 10, 0.75F});
        dayo::core::VmdCameraKey cameraKey;
        cameraKey.frame = 12;
        cameraKey.distance = -30.0F;
        cameraKey.viewAngle = 45;
        cameraKey.parentModel = 2;
        cameraKey.parentBone = 7;
        cameraKey.parentBoneName = "head";
        cameraKey.methods = {1, 0, 1, 0, 1, 0};
        cameraKey.interpolation[0] = 17;
        document.motion.cameras.push_back(cameraKey);
        document.motion.lights.push_back({13, {0.5F, 0.6F, 0.7F}, {-1.0F, -2.0F, 1.0F}});
        document.motion.shadows.push_back({14, 2, 75.0F});
        document.motion.ik.push_back({15, true, {{"leg IK", false}}});
        document.motion.externalParents.push_back({16, 1, "parent", "child"});
        document.motion.gravity.push_back({17, 9.8F, {0.0F, -1.0F, 0.0F}, 0.1F, 2.0F});
        dayo::core::saveVmdayo(vmdayoPath, document);
        {
            std::ifstream vmdayoBytes(vmdayoPath, std::ios::binary);
            std::array<char, 8> signature{};
            vmdayoBytes.read(signature.data(), static_cast<std::streamsize>(signature.size()));
            ok &= check(std::string_view(signature.data()) == "VMDAYO!", "official VMdayo binary signature");
        }
        const auto loaded = dayo::core::loadVmdayo(vmdayoPath);
        ok &= check(loaded.motion.morphs.size() == 1 &&
                        loaded.motion.interpolation == dayo::core::InterpolationMode::catmullRom,
                    "VMdayo round trip");
        ok &= check(loaded.version == 3 && loaded.motion.bones.size() == 1 && !loaded.motion.bones[0].physics &&
                        loaded.motion.bones[0].methods[0] == 1,
                    "VMdayo v3 bone track round trip");
        ok &= check(loaded.motion.cameras.size() == 1 && loaded.motion.cameras[0].parentModel == 2 &&
                        loaded.motion.cameras[0].parentBone == 7 && loaded.motion.cameras[0].parentBoneName == "head" &&
                        loaded.motion.cameras[0].methods[0] == 1 && loaded.motion.cameras[0].interpolation[0] == 17,
                    "VMdayo v3 camera tracking round trip");
        ok &= check(loaded.motion.lights.size() == 1 && loaded.motion.shadows.size() == 1,
                    "VMdayo v3 light and shadow tracks round trip");
        ok &= check(loaded.motion.ik.size() == 1 && loaded.motion.ik[0].states.size() == 1 &&
                        !loaded.motion.ik[0].states[0].enabled,
                    "VMdayo v3 IK track round trip");
        ok &= check(loaded.motion.externalParents.size() == 1 && loaded.motion.gravity.size() == 1,
                    "VMdayo v3 extension tracks round trip");
        ok &= check(loaded.modelDictionary.at(7) == "Parent Model" && loaded.metadata.at("author") == "mikumikudesu",
                    "VMdayo v3 header dictionaries round trip");
        dayo::core::Scene vmdayoScene;
        vmdayoScene.attachMotion(loaded.motion, 0, loaded.modelName);
        ok &= check(vmdayoScene.timeline().duration == 17.0F, "VMdayo attachment updates timeline duration");
        document.opaque = {0x00, 0xFF, 0x56, 0x4D, 0x44};
        dayo::core::saveVmdayo(vmdayoPath, document);
        const auto opaque = dayo::core::loadVmdayo(vmdayoPath);
        ok &= check(opaque.opaque == document.opaque, "opaque VMdayo payload preservation");
        std::error_code vmdayoError;
        std::filesystem::remove(vmdayoPath, vmdayoError);
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: VMdayo: " << exception.what() << '\n';
        ok = false;
    }

    {
        dayo::core::Scene scene;
        dayo::core::CommandHistory history;
        scene.setTimelineDuration(10.0F);
        scene.setFrame(0.0F);
        ok &= check(scene.advanceFrame(0.5F, true) && scene.timeline().frame > 0.0F,
                    "scene timeline advances without selected model");
        scene.setRuntimeMode(dayo::core::RuntimeMode::idle);
        ok &= check(!scene.advanceFrame(0.5F, true), "idle runtime pauses scene timeline");
        scene.setRuntimeMode(dayo::core::RuntimeMode::accumulate);
        scene.setFrame(0.0F);
        ok &= check(!scene.advanceFrame(0.5F, true) && scene.timeline().frame == 0.0F,
                    "accumulate runtime freezes scene timeline");
        scene.advanceAccumulation();
        scene.advanceAccumulation();
        scene.advanceAccumulation();
        ok &= check(scene.accumulatedSamples() == 3, "accumulate runtime advances samples without advancing timeline");
        scene.setRuntimeMode(dayo::core::RuntimeMode::realtime);
        history.execute(scene, std::make_unique<dayo::core::SetFrameCommand>(0.0F, 24.0F));
        ok &= check(scene.timeline().frame == 24.0F && history.canUndo(), "editor command apply");
        ok &= check(history.undo(scene) && scene.timeline().frame == 0.0F, "editor command undo");
        ok &= check(history.redo(scene) && scene.timeline().frame == 24.0F, "editor command redo");
        history.clear();
        ok &= check(!history.canUndo() && !history.canRedo(), "project reset clears command history");
        dayo::core::MaterialParameterBlock parameters;
        parameters.set("roughness", 0.5F);
        ok &=
            check(parameters.find("roughness") != nullptr && parameters.erase("roughness"), "material parameter block");
    }

    const auto path = std::filesystem::temp_directory_path() / "mikumikudesu-core-test.pmx";
    {
        std::ofstream output(path, std::ios::binary);
        output.write("PMX ", 4);
        append(output, 2.0F);
        append(output, static_cast<std::uint8_t>(8));
        const std::array<std::uint8_t, 8> settings{1, 0, 4, 4, 4, 4, 4, 4};
        output.write(reinterpret_cast<const char*>(settings.data()), settings.size());
        appendText(output, "Miku");
        appendText(output, "Miku English");
        appendText(output, "comment");
        appendText(output, "comment en");
        append(output, static_cast<std::int32_t>(3));
        for (std::int32_t vertex = 0; vertex < 3; ++vertex) {
            const std::array<float, 3> position{static_cast<float>(vertex), static_cast<float>(vertex & 1), 0.0F};
            const std::array<float, 3> normal{0.0F, 0.0F, 1.0F};
            const std::array<float, 2> uv{0.0F, 0.0F};
            output.write(reinterpret_cast<const char*>(position.data()), sizeof(position));
            output.write(reinterpret_cast<const char*>(normal.data()), sizeof(normal));
            output.write(reinterpret_cast<const char*>(uv.data()), sizeof(uv));
            append(output, static_cast<std::uint8_t>(0));
            append(output, static_cast<std::int32_t>(0));
            append(output, 1.0F);
        }
        append(output, static_cast<std::int32_t>(3));
        append(output, static_cast<std::uint32_t>(0));
        append(output, static_cast<std::uint32_t>(1));
        append(output, static_cast<std::uint32_t>(2));
        append(output, static_cast<std::int32_t>(0)); // textures
        append(output, static_cast<std::int32_t>(1)); // materials
        appendText(output, "Material");
        appendText(output, "Material");
        const std::array<float, 4> diffuse{1.0F, 1.0F, 1.0F, 1.0F};
        const std::array<float, 3> vector3{};
        const std::array<float, 4> vector4{};
        output.write(reinterpret_cast<const char*>(diffuse.data()), sizeof(diffuse));
        output.write(reinterpret_cast<const char*>(vector3.data()), sizeof(vector3));
        append(output, 0.0F);
        output.write(reinterpret_cast<const char*>(vector3.data()), sizeof(vector3));
        append(output, static_cast<std::uint8_t>(0));
        output.write(reinterpret_cast<const char*>(vector4.data()), sizeof(vector4));
        append(output, 0.0F);
        append(output, static_cast<std::int32_t>(-1));
        append(output, static_cast<std::int32_t>(-1));
        append(output, static_cast<std::uint8_t>(0));
        append(output, static_cast<std::uint8_t>(0));
        append(output, static_cast<std::int32_t>(-1));
        appendText(output, "");
        append(output, static_cast<std::int32_t>(3));
        // bones, morphs, display frames, bodies and joints
        for (int section = 0; section < 5; ++section)
            append(output, static_cast<std::int32_t>(0));
    }
    try {
        const auto metadata = dayo::core::probePmx(path);
        ok &= check(metadata.version == 2.0F, "PMX version");
        ok &= check(metadata.modelName == "Miku", "PMX UTF-8 model name");
        ok &= check(metadata.vertexCount == 3, "PMX vertex count");
        const auto mesh = dayo::core::loadPmxMesh(path);
        ok &= check(mesh.vertices.size() == 3, "PMX vertex loading");
        ok &= check(mesh.indices.size() == 3 && mesh.indices[2] == 2, "PMX index loading");
        const auto model = dayo::core::loadPmxModel(path);
        ok &= check(model.materials.size() == 1 && model.bones.empty(), "complete PMX sections");
        dayo::core::Scene scene;
        ok &= check(scene.physicsSettings().gravity == 98.0F, "MMD gravity default");
        const auto initialTopologyGeneration = scene.topologyGeneration();
        const auto firstModel = scene.addModel(path);
        const auto secondModel = scene.addModel(path);
        ok &= check(scene.models().size() == 2 && scene.selectedModel() != nullptr, "multi-model scene instances");
        const auto modelTopologyGeneration = scene.topologyGeneration();
        ok &= check(modelTopologyGeneration > initialTopologyGeneration && scene.setModelVisible(firstModel, false) &&
                        scene.setCloneCount(firstModel, 2) && scene.topologyGeneration() == modelTopologyGeneration + 2,
                    "scene topology generation tracks visibility and clone changes");
        static_cast<void>(scene.setModelVisible(firstModel, true));
        dayo::core::VmdMotion shortMotion;
        shortMotion.morphs.push_back({"smile", 20, 1.0F});
        scene.attachMotion(std::move(shortMotion), firstModel);
        dayo::core::VmdMotion longMotion;
        longMotion.morphs.push_back({"smile", 100, 1.0F});
        scene.attachMotion(std::move(longMotion), secondModel);
        ok &= check(scene.timeline().duration == 100.0F, "timeline duration follows the longest model motion");
        scene.setFrame(80.0F);
        ok &= check(scene.removeModel(secondModel) && scene.timeline().duration == 20.0F,
                    "removing longest motion recalculates timeline duration");
        ok &= check(scene.timeline().frame == 20.0F, "removing longest motion clamps the current frame");
        std::string parentError;
        ok &= check(!scene.addExternalParent({1, "missing", 2, "missing"}, &parentError) && !parentError.empty() &&
                        !scene.hasExternalParentCycle(),
                    "external parent validation without fake solve");
        scene.setGravityTrack({{12, {3.0F, {0.0F, -1.0F, 0.0F}, 0.0F, 0.0F, false}}});
        ok &= check(scene.evaluatePhysicsSettings(12.0F).gravity == 3.0F, "animated gravity settings");
        const auto beforeDirty = scene.accumulatedSamples();
        scene.advanceAccumulation();
        scene.setPhysicsSettings({6.0F, {0.0F, -1.0F, 0.0F}, 0.5F, 2.0F, true});
        ok &= check(scene.physicsSettings().floorCollision && scene.accumulatedSamples() == 0 && beforeDirty == 0,
                    "physics settings invalidate accumulation");

        dayo::core::VmdMotion cameraMotion;
        cameraMotion.cameras.push_back({500, -45.0F, {}, {}, {}, 30, true, -1, -1, {}, {}});
        scene.attachMotion(std::move(cameraMotion), firstModel);
        scene.setFrame(400.0F);
        ok &= check(scene.cameraMotion() != nullptr && scene.timeline().duration == 500.0F,
                    "camera motion is attached as global scene state");
        const auto beforeProjectResetGeneration = scene.topologyGeneration();
        scene.clearProjectState();
        ok &= check(scene.models().empty() && scene.cameraMotion() == nullptr && scene.timeline().duration == 0.0F &&
                        scene.timeline().frame == 0.0F && scene.topologyGeneration() > beforeProjectResetGeneration,
                    "project reset clears camera motion and timeline state");
        const auto resetModel = scene.addModel(path);
        dayo::core::VmdMotion resetMotion;
        resetMotion.morphs.push_back({"smile", 100, 1.0F});
        scene.attachMotion(std::move(resetMotion), resetModel);
        ok &= check(scene.cameraMotion() == nullptr && scene.timeline().duration == 100.0F,
                    "new project motion is not shadowed by previous camera motion");
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: PMX probe: " << exception.what() << '\n';
        ok = false;
    }
    try {
        const auto outputDirectory = std::filesystem::temp_directory_path() / "mikumikudesu-output-test";
        dayo::core::OutputSettings settings;
        settings.directory = outputDirectory;
        settings.firstFrame = settings.lastFrame = 3;
        dayo::core::OutputQueue queue(settings);
        dayo::core::ImageRgba8 image{1, 1, {255, 64, 32, 255}};
        queue.push(3, std::move(image));
        static_cast<void>(queue.written());
        queue.close();
        ok &= check(queue.written() == 1 && std::filesystem::exists(outputDirectory / "frame_000003.ppm"),
                    "asynchronous frame output");
        dayo::core::ImageRgba8 pngImage{1, 1, {255, 64, 32, 255}};
        dayo::core::writeFrame(outputDirectory / "frame.png", pngImage, dayo::core::OutputFormat::png);
        ok &= check(std::filesystem::file_size(outputDirectory / "frame.png") > 8, "PNG frame output");
        ok &= check(queue.written() == 1, "thread-safe output counter");
        dayo::core::OutputSettings failingSettings;
        failingSettings.directory = outputDirectory;
        failingSettings.format = dayo::core::OutputFormat::exr;
        dayo::core::OutputQueue failingQueue(failingSettings);
        failingQueue.push(0, dayo::core::ImageRgba8{1, 1, {0, 0, 0, 255}});
        failingQueue.close();
        bool propagated = false;
        try {
            failingQueue.rethrowIfFailed();
        } catch (const std::exception&) {
            propagated = true;
        }
        ok &= check(propagated, "output worker error propagation");
        std::error_code outputError;
        std::filesystem::remove_all(outputDirectory, outputError);
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: output: " << exception.what() << '\n';
        ok = false;
    }
    try {
        const auto mediaPath = std::filesystem::temp_directory_path() / "mikumikudesu-media-test.wav";
        const auto exportPath = std::filesystem::temp_directory_path() / "mikumikudesu-audio-export-test.m4a";
        const auto videoPath = std::filesystem::temp_directory_path() / "mikumikudesu-video-export-test.mp4";
        {
            std::ofstream output(mediaPath, std::ios::binary);
            constexpr std::uint32_t sampleCount = 800;
            output.write("RIFF", 4);
            append(output, 36U + sampleCount * 2U);
            output.write("WAVEfmt ", 8);
            append(output, 16U);
            append(output, static_cast<std::uint16_t>(1));
            append(output, static_cast<std::uint16_t>(1));
            append(output, 8'000U);
            append(output, 16'000U);
            append(output, static_cast<std::uint16_t>(2));
            append(output, static_cast<std::uint16_t>(16));
            output.write("data", 4);
            append(output, sampleCount * 2U);
            for (std::uint32_t i = 0; i < sampleCount; ++i) {
                append(output, static_cast<std::int16_t>((i % 32U) * 512U));
            }
        }
#if DAYO_HAS_MEDIA
        dayo::core::MediaFile media(mediaPath);
        ok &= check(media.info().hasAudio && !media.info().hasVideo, "FFmpeg media probing");
        const auto audio = media.decodeAudio();
        ok &= check(audio.channels == 2 && audio.sampleRate == 48'000 && !audio.samples.empty(),
                    "FFmpeg audio decode and resample");
        dayo::core::AudioExportRequest exportRequest;
        exportRequest.source = mediaPath;
        exportRequest.destination = exportPath;
        exportRequest.overwrite = true;
        double exportRatio = 0.0;
        const auto exportResult = dayo::core::exportM4a(
            exportRequest, [&](const dayo::core::AudioExportProgress& progress) { exportRatio = progress.ratio(); });
        ok &= check(dayo::core::canExportM4a() && exportResult.encodedSamples > 0 && exportRatio >= 0.99 &&
                        std::filesystem::exists(exportPath),
                    "streaming AAC M4A export");
        dayo::core::MediaFile exportedMedia(exportPath);
        ok &= check(exportedMedia.info().hasAudio && !exportedMedia.info().hasVideo &&
                        exportedMedia.info().durationSeconds > 0.05 && exportedMedia.info().durationSeconds < 0.2,
                    "M4A export round trip metadata");
        const auto exportedAudio = exportedMedia.decodeAudio();
        ok &= check(exportedAudio.sampleRate == 48'000 && exportedAudio.channels == 2 && !exportedAudio.samples.empty(),
                    "M4A export round trip decode");
        dayo::core::VideoExportRequest videoRequest;
        videoRequest.destination = videoPath;
        videoRequest.width = 64;
        videoRequest.height = 48;
        videoRequest.fps = 10.0;
        videoRequest.includeAudio = true;
        videoRequest.overwrite = true;
        dayo::core::VideoExporter videoExporter(videoRequest);
        videoExporter.writeAudio(audio.samples, audio.sampleRate, audio.channels);
        for (std::uint8_t value : {std::uint8_t{32}, std::uint8_t{96}, std::uint8_t{192}}) {
            dayo::core::ImageRgba8 image;
            image.width = videoRequest.width;
            image.height = videoRequest.height;
            image.pixels.resize(static_cast<std::size_t>(image.width) * image.height * 4U, value);
            for (std::size_t index = 3; index < image.pixels.size(); index += 4)
                image.pixels[index] = 255;
            videoExporter.writeVideoFrame(image);
        }
        const auto videoResult = videoExporter.finish();
        ok &=
            check(dayo::core::canExportVideo() && videoResult.encodedFrames == 3 && std::filesystem::exists(videoPath),
                  "streaming H.264 MP4 export");
        dayo::core::MediaFile exportedVideo(videoPath);
        ok &= check(exportedVideo.info().hasVideo && exportedVideo.info().hasAudio &&
                        exportedVideo.info().videoWidth == 64 && exportedVideo.info().videoHeight == 48 &&
                        std::abs(exportedVideo.info().durationSeconds - 0.3) < 0.03,
                    "MP4 export round trip metadata");
        const auto exportedFrame = exportedVideo.decodeVideoFrame(0.1);
        ok &= check(exportedFrame.width == 64 && exportedFrame.height == 48 && !exportedFrame.pixels.empty(),
                    "MP4 export round trip decode");
        const auto exportedVideoAudio = exportedVideo.decodeAudio();
        ok &= check(exportedVideoAudio.sampleRate == 48'000 && exportedVideoAudio.channels == 2 &&
                        !exportedVideoAudio.samples.empty(),
                    "MP4 video audio round trip decode");
        dayo::core::Scene backgroundScene;
        backgroundScene.setBackgroundScreenSource(dayo::core::ScreenTextureSource::backgroundImage);
        backgroundScene.setMedia(mediaPath);
        ok &= check(backgroundScene.background().screenSource == dayo::core::ScreenTextureSource::backgroundImage,
                    "audio-only media preserves the selected background source");
#endif
        std::error_code mediaError;
        std::filesystem::remove(mediaPath, mediaError);
        std::filesystem::remove(exportPath, mediaError);
        std::filesystem::remove(videoPath, mediaError);
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: media: " << exception.what() << '\n';
        ok = false;
    }
    try {
        dayo::core::PmxModel physicsModel;
        dayo::core::PmxRigidBody falling;
        falling.shape = 0;
        falling.size = {0.5F, 0.5F, 0.5F};
        falling.position = {0.0F, 2.0F, 0.0F};
        falling.mass = 1.0F;
        falling.mode = 1;
        falling.collisionMask = 0;
        falling.bone = 0;
        physicsModel.rigidBodies.push_back(falling);
        dayo::core::MmdPhysics physics(physicsModel);
#if DAYO_HAS_BULLET
        ok &= check(physics.available() && physics.bodyCount() == 1, "Bullet PMX body creation");
        const auto before = physics.bodyTransform(0);
        for (int i = 0; i < 60; ++i)
            physics.step(1.0F / 60.0F);
        const auto after = physics.bodyTransform(0);
        ok &= check(after.position[1] < before.position[1], "Bullet gravity simulation");
        physics.setFloorCollision(true);
        physics.reset();
        for (int i = 0; i < 120; ++i)
            physics.step(1.0F / 60.0F);
        ok &= check(physics.bodyTransform(0).position[1] > -0.1F, "PMX collision mask permits Bullet floor collision");
        physics.setGravityNoise(1.0F, 2.0F);
        physicsModel.vertices.resize(1);
        physicsModel.vertices[0].position = {0.0F, 2.0F, 0.0F};
        physicsModel.vertices[0].normal = {0.0F, 1.0F, 0.0F};
        physicsModel.vertices[0].bones[0] = 0;
        dayo::core::PmxBone rootBone;
        rootBone.position = {0.0F, 2.0F, 0.0F};
        physicsModel.bones.push_back(rootBone);
        dayo::core::MmdPhysics animatedPhysics(physicsModel);
        dayo::core::MmdAnimator physicalAnimator(physicsModel);
        physicalAnimator.setPhysics(&animatedPhysics);
        const auto physicalBefore = physicalAnimator.evaluate(0.0F, 0.0F);
        const auto physicalAfter = physicalAnimator.evaluate(1.0F, 1.0F / 30.0F);
        ok &= check(physicalAfter.vertices[0].position[1] < physicalBefore.vertices[0].position[1],
                    "Bullet body drives PMX bone skinning");

        dayo::core::PmxModel alignedModel;
        alignedModel.vertices.resize(2);
        alignedModel.vertices[0].position = {0.0F, 2.0F, 0.0F};
        alignedModel.vertices[0].normal = {0.0F, 1.0F, 0.0F};
        alignedModel.vertices[0].bones[0] = 0;
        alignedModel.vertices[1] = alignedModel.vertices[0];
        alignedModel.vertices[1].position = {1.0F, 2.0F, 0.0F};
        dayo::core::PmxBone alignedBone;
        alignedBone.name = "aligned";
        alignedBone.position = {0.0F, 2.0F, 0.0F};
        alignedModel.bones.push_back(alignedBone);
        dayo::core::PmxRigidBody alignedBody;
        alignedBody.shape = 0;
        alignedBody.size = {0.5F, 0.5F, 0.5F};
        alignedBody.position = {0.0F, 2.0F, 0.0F};
        alignedBody.bone = 0;
        alignedBody.mass = 1.0F;
        alignedBody.mode = 2;
        alignedBody.collisionMask = 0;
        alignedModel.rigidBodies.push_back(alignedBody);
        dayo::core::MmdPhysics alignedPhysics(alignedModel);
        dayo::core::MmdAnimator alignedAnimator(alignedModel);
        alignedAnimator.setPhysics(&alignedPhysics);
        const auto alignedBefore = alignedAnimator.evaluate(0.0F, 0.0F);
        const auto alignedBodyBefore = alignedPhysics.bodyTransform(0);
        alignedPhysics.applyImpulse(0, {}, {0.0F, 0.0F, 10.0F}, false);
        const auto alignedAfter = alignedAnimator.evaluate(1.0F, 1.0F / 30.0F);
        const auto alignedBodyAfter = alignedPhysics.bodyTransform(0);
        ok &=
            check(std::abs(alignedBodyAfter.position[1] - alignedBodyBefore.position[1]) < 1e-4F &&
                      std::abs(alignedAfter.vertices[0].position[1] - alignedBefore.vertices[0].position[1]) < 1e-4F &&
                      std::abs(alignedAfter.vertices[1].position[1] - alignedBefore.vertices[1].position[1]) > 1e-4F,
                  "PMX mode 2 aligns body position, preserves bone translation, and imports rotation");

        dayo::core::PmxModel skirtChainModel;
        skirtChainModel.vertices.resize(2);
        skirtChainModel.vertices[0].position = {0.0F, 2.0F, 0.0F};
        skirtChainModel.vertices[0].normal = {0.0F, 1.0F, 0.0F};
        skirtChainModel.vertices[0].bones[0] = 1;
        skirtChainModel.vertices[1] = skirtChainModel.vertices[0];
        skirtChainModel.vertices[1].position = {0.0F, 3.0F, 0.0F};
        skirtChainModel.vertices[1].bones[0] = 2;
        skirtChainModel.bones.resize(3);
        skirtChainModel.bones[0].name = "root";
        skirtChainModel.bones[1].name = "skirt0";
        skirtChainModel.bones[1].position = {0.0F, 2.0F, 0.0F};
        skirtChainModel.bones[1].parent = 0;
        skirtChainModel.bones[2].name = "skirt1";
        skirtChainModel.bones[2].position = {0.0F, 3.0F, 0.0F};
        skirtChainModel.bones[2].parent = 1;
        dayo::core::PmxRigidBody skirtRoot;
        skirtRoot.shape = 0;
        skirtRoot.size = {0.2F, 0.2F, 0.2F};
        skirtRoot.position = {0.0F, 0.0F, 0.0F};
        skirtRoot.bone = 0;
        skirtRoot.collisionMask = 0xFFFFU;
        skirtChainModel.rigidBodies.push_back(skirtRoot);
        auto skirtMode2 = skirtRoot;
        skirtMode2.position = {0.0F, 2.0F, 0.0F};
        skirtMode2.bone = 1;
        skirtMode2.mass = 0.2F;
        skirtMode2.mode = 2;
        skirtChainModel.rigidBodies.push_back(skirtMode2);
        auto skirtMode1 = skirtMode2;
        skirtMode1.position = {0.0F, 3.0F, 0.0F};
        skirtMode1.bone = 2;
        skirtMode1.mode = 1;
        skirtChainModel.rigidBodies.push_back(skirtMode1);
        const auto addLockedJoint = [&skirtChainModel](std::int32_t bodyA, std::int32_t bodyB, float height) {
            dayo::core::PmxJoint joint;
            joint.bodyA = bodyA;
            joint.bodyB = bodyB;
            joint.position = {0.0F, height, 0.0F};
            joint.translationMinimum = {};
            joint.translationMaximum = {};
            joint.rotationMinimum = {};
            joint.rotationMaximum = {};
            skirtChainModel.joints.push_back(joint);
        };
        addLockedJoint(0, 1, 2.0F);
        addLockedJoint(1, 2, 3.0F);
        dayo::core::VmdMotion skirtChainMotion;
        skirtChainMotion.bones.reserve(300);
        for (std::uint32_t frame = 0; frame < 300; ++frame) {
            dayo::core::VmdBoneKey key;
            key.name = "root";
            key.frame = frame;
            const auto angle = (frame % 2 == 0 ? 0.35F : -0.35F);
            key.translation = {(frame % 2 == 0 ? 1.5F : -1.5F), (frame % 3 == 0 ? 0.75F : -0.75F), 0.0F};
            key.rotation = {0.0F, 0.0F, std::sin(angle * 0.5F), std::cos(angle * 0.5F)};
            skirtChainMotion.bones.push_back(key);
        }
        dayo::core::MmdPhysics skirtChainPhysics(skirtChainModel);
        dayo::core::MmdAnimator skirtChainAnimator(skirtChainModel);
        skirtChainAnimator.setMotion(&skirtChainMotion);
        skirtChainAnimator.setPhysics(&skirtChainPhysics);
        bool skirtChainStable = true;
        for (std::uint32_t frame = 0; frame < 300; ++frame) {
            const auto animated =
                skirtChainAnimator.evaluate(static_cast<float>(frame), frame == 0 ? 0.0F : 1.0F / 30.0F);
            const auto angle = (frame % 2 == 0 ? 0.35F : -0.35F);
            const auto expectedSkirt0 = dayo::core::Float3{
                (frame % 2 == 0 ? 1.5F : -1.5F) - 2.0F * std::sin(angle),
                (frame % 3 == 0 ? 0.75F : -0.75F) + 2.0F * std::cos(angle),
                0.0F,
            };
            const auto mode2Body = skirtChainPhysics.bodyTransform(1);
            const auto mode1Body = skirtChainPhysics.bodyTransform(2);
            dayo::core::PhysicsTransform expectedMode2;
            expectedMode2.position = expectedSkirt0;
            const auto finiteVector = [](const auto& value) {
                return std::ranges::all_of(value, [](float component) { return std::isfinite(component); });
            };
            const auto bodyDistance = [](const auto& left, const auto& right) {
                const auto x = left.position[0] - right.position[0];
                const auto y = left.position[1] - right.position[1];
                const auto z = left.position[2] - right.position[2];
                return std::sqrt(x * x + y * y + z * z);
            };
            skirtChainStable =
                skirtChainStable && finiteVector(mode2Body.position) && finiteVector(mode1Body.position) &&
                animated.vertices.size() == 2 && finiteVector(animated.vertices[0].position) &&
                finiteVector(animated.vertices[1].position) && bodyDistance(mode2Body, expectedMode2) < 1e-3F &&
                bodyDistance(mode2Body, mode1Body) < 2.5F;
            if (!skirtChainStable)
                break;
        }
        ok &= check(skirtChainStable,
                    "mode 0 to mode 2 to mode 1 locked skirt chain stays finite and aligned during fast animation");

        dayo::core::PmxModel seekModel;
        seekModel.vertices.resize(1);
        seekModel.vertices[0].position = {0.0F, 2.0F, 0.0F};
        seekModel.vertices[0].normal = {0.0F, 1.0F, 0.0F};
        seekModel.vertices[0].bones[0] = 1;
        seekModel.bones.resize(2);
        seekModel.bones[0].name = "root";
        seekModel.bones[0].position = {0.0F, 0.0F, 0.0F};
        seekModel.bones[1].name = "dynamic";
        seekModel.bones[1].position = {0.0F, 2.0F, 0.0F};
        seekModel.bones[1].parent = 0;
        dayo::core::PmxRigidBody seekAnchor;
        seekAnchor.shape = 0;
        seekAnchor.size = {0.5F, 0.5F, 0.5F};
        seekAnchor.position = {0.0F, 0.0F, 0.0F};
        seekAnchor.bone = 0;
        seekAnchor.collisionMask = 0;
        seekModel.rigidBodies.push_back(seekAnchor);
        auto seekDynamic = seekAnchor;
        seekDynamic.position = {0.0F, 2.0F, 0.0F};
        seekDynamic.bone = 1;
        seekDynamic.mass = 1.0F;
        seekDynamic.mode = 1;
        seekModel.rigidBodies.push_back(seekDynamic);
        dayo::core::PmxJoint seekJoint;
        seekJoint.bodyA = 0;
        seekJoint.bodyB = 1;
        seekJoint.position = {0.0F, 2.0F, 0.0F};
        seekJoint.translationMinimum = {-100.0F, -100.0F, -100.0F};
        seekJoint.translationMaximum = {100.0F, 100.0F, 100.0F};
        seekJoint.rotationMinimum = {-100.0F, -100.0F, -100.0F};
        seekJoint.rotationMaximum = {100.0F, 100.0F, 100.0F};
        seekModel.joints.push_back(seekJoint);
        dayo::core::MmdPhysics seekPhysics(seekModel);
        dayo::core::MmdAnimator seekAnimator(seekModel);
        dayo::core::VmdMotion seekMotion;
        dayo::core::VmdBoneKey seekStart;
        seekStart.name = "root";
        seekStart.frame = 0;
        dayo::core::VmdBoneKey seekTarget = seekStart;
        seekTarget.frame = 1047;
        seekTarget.translation = {0.0F, 3.0F, 0.0F};
        seekMotion.bones = {seekStart, seekTarget};
        seekAnimator.setMotion(&seekMotion);
        seekAnimator.setPhysics(&seekPhysics);
        static_cast<void>(seekAnimator.evaluate(0.0F, 0.0F));
        static_cast<void>(seekAnimator.evaluate(1.0F, 1.0F / 30.0F));
        const auto seekMoved = seekPhysics.bodyTransform(1);
        static_cast<void>(seekAnimator.evaluate(1047.0F, 0.0F));
        const auto seekResetAnchor = seekPhysics.bodyTransform(0);
        const auto seekResetDynamic = seekPhysics.bodyTransform(1);
        ok &= check(seekMoved.position[1] < 2.0F && std::abs(seekResetAnchor.position[1] - 3.0F) < 1e-4F &&
                        std::abs(seekResetDynamic.position[1] - 5.0F) < 1e-4F,
                    "forward timeline seek aligns Bullet state to the target bone pose");

        dayo::core::PmxModel jointModel;
        jointModel.vertices.resize(1);
        jointModel.vertices[0].position = {0.0F, 2.0F, 0.0F};
        jointModel.vertices[0].normal = {0.0F, 1.0F, 0.0F};
        jointModel.vertices[0].bones[0] = 1;
        jointModel.bones.resize(2);
        jointModel.bones[0].name = "root";
        jointModel.bones[1].name = "dynamic";
        jointModel.bones[1].position = {0.0F, 2.0F, 0.0F};
        jointModel.bones[1].parent = 0;
        dayo::core::PmxRigidBody anchor;
        anchor.shape = 0;
        anchor.size = {0.5F, 0.5F, 0.5F};
        anchor.position = {0.0F, 0.0F, 0.0F};
        anchor.bone = 0;
        anchor.collisionMask = 0;
        jointModel.rigidBodies.push_back(anchor);
        auto dynamic = anchor;
        dynamic.position = {0.0F, 2.0F, 0.0F};
        dynamic.bone = 1;
        dynamic.mass = 1.0F;
        dynamic.mode = 1;
        jointModel.rigidBodies.push_back(dynamic);
        dayo::core::PmxJoint joint;
        joint.bodyA = 0;
        joint.bodyB = 1;
        joint.position = {0.0F, 2.0F, 0.0F};
        joint.translationMinimum = {-100.0F, -100.0F, -100.0F};
        joint.translationMaximum = {100.0F, 100.0F, 100.0F};
        joint.rotationMinimum = {-100.0F, -100.0F, -100.0F};
        joint.rotationMaximum = {100.0F, 100.0F, 100.0F};
        jointModel.joints.push_back(joint);
        dayo::core::MmdPhysics jointPhysics(jointModel);
        ok &= check(jointPhysics.available() && jointPhysics.bodyCount() == 2 && jointPhysics.jointCount() == 1,
                    "Bullet keeps a valid PMX joint constraint");
        dayo::core::MmdAnimator jointAnimator(jointModel);
        jointAnimator.setPhysics(&jointPhysics);
        const auto jointBefore = jointAnimator.evaluate(0.0F, 0.0F);
        const auto jointAfter = jointAnimator.evaluate(1.0F, 1.0F / 30.0F);
        ok &= check(jointAfter.vertices[0].position[1] < jointBefore.vertices[0].position[1],
                    "Bullet joint model still drives a dynamic PMX bone");

        dayo::core::PmxModel invalidInertiaModel;
        dayo::core::PmxRigidBody invalidInertiaBody;
        invalidInertiaBody.shape = 0;
        invalidInertiaBody.size = {0.0F, 0.0F, 0.0F};
        invalidInertiaBody.mass = 2.0F;
        invalidInertiaBody.mode = 1;
        invalidInertiaModel.rigidBodies.push_back(invalidInertiaBody);
        auto invalidInertiaAnchor = invalidInertiaBody;
        invalidInertiaAnchor.mass = 0.0F;
        invalidInertiaAnchor.mode = 0;
        invalidInertiaModel.rigidBodies.push_back(invalidInertiaAnchor);
        dayo::core::PmxJoint invalidInertiaJoint;
        invalidInertiaJoint.bodyA = 0;
        invalidInertiaJoint.bodyB = 1;
        invalidInertiaModel.joints.push_back(invalidInertiaJoint);
        dayo::core::MmdPhysics invalidInertiaPhysics(invalidInertiaModel);
        ok &= check(invalidInertiaPhysics.bodyCount() == 2 && invalidInertiaPhysics.jointCount() == 0,
                    "invalid inertia body uses zero effective mass and skips its constraint");
        ok &= check(invalidInertiaPhysics.bodyMode(0) == 0 && invalidInertiaPhysics.bodyMode(1) == 0,
                    "invalid inertia body exposes its sanitized kinematic mode");
        invalidInertiaModel.bones.resize(1);
        invalidInertiaModel.bones[0].position = {0.0F, 2.0F, 0.0F};
        invalidInertiaModel.vertices.resize(1);
        invalidInertiaModel.vertices[0].position = {0.0F, 2.0F, 0.0F};
        invalidInertiaModel.vertices[0].bones[0] = 0;
        invalidInertiaModel.rigidBodies[0].bone = 0;
        invalidInertiaModel.rigidBodies[0].position = {0.0F, 2.0F, 0.0F};
        invalidInertiaModel.rigidBodies[1].bone = 0;
        invalidInertiaModel.rigidBodies[1].position = {0.0F, 2.0F, 0.0F};
        dayo::core::MmdPhysics sanitizedPhysics(invalidInertiaModel);
        dayo::core::MmdAnimator sanitizedAnimator(invalidInertiaModel);
        sanitizedAnimator.setPhysics(&sanitizedPhysics);
        const auto sanitizedBefore = sanitizedAnimator.evaluate(0.0F, 0.0F);
        const auto sanitizedAfter = sanitizedAnimator.evaluate(1.0F, 1.0F / 30.0F);
        ok &= check(std::abs(sanitizedAfter.vertices[0].position[1] - sanitizedBefore.vertices[0].position[1]) < 1e-4F,
                    "sanitized dynamic body does not snap its PMX bone");

        dayo::core::PmxModel postPhysicsModel;
        postPhysicsModel.metadata.modelName = "synthetic-post-physics-ik-model";
        postPhysicsModel.vertices.resize(1);
        postPhysicsModel.vertices[0].position = {1.0F, 1.0F, 0.0F};
        postPhysicsModel.vertices[0].normal = {0.0F, 0.0F, 1.0F};
        postPhysicsModel.vertices[0].bones[0] = 1;
        postPhysicsModel.bones.resize(4);
        postPhysicsModel.bones[0].name = "root";
        postPhysicsModel.bones[1].name = "post-link";
        postPhysicsModel.bones[1].position = {0.0F, 1.0F, 0.0F};
        postPhysicsModel.bones[1].parent = 0;
        postPhysicsModel.bones[2].name = "post-effector";
        postPhysicsModel.bones[2].position = {1.0F, 1.0F, 0.0F};
        postPhysicsModel.bones[2].parent = 1;
        postPhysicsModel.bones[3].name = "post-ik";
        postPhysicsModel.bones[3].position = {0.0F, 2.0F, 0.0F};
        postPhysicsModel.bones[3].parent = 0;
        postPhysicsModel.bones[3].flags = 0x1020U;
        postPhysicsModel.bones[3].ikTarget = 2;
        postPhysicsModel.bones[3].ikLoopCount = 8;
        postPhysicsModel.bones[3].ikLimitAngle = std::numbers::pi_v<float> * 0.5F;
        postPhysicsModel.bones[3].ikLinks.push_back({1, false, {}, {}});
        dayo::core::PmxRigidBody postPhysicsBody;
        postPhysicsBody.shape = 0;
        postPhysicsBody.size = {0.25F, 0.25F, 0.25F};
        postPhysicsBody.position = {0.0F, 2.0F, 0.0F};
        postPhysicsBody.bone = 3;
        postPhysicsBody.mass = 1.0F;
        postPhysicsBody.mode = 1;
        postPhysicsBody.collisionMask = 0;
        postPhysicsModel.rigidBodies.push_back(postPhysicsBody);
        dayo::core::MmdPhysics postPhysics(postPhysicsModel);
        postPhysics.setGravity({9.8F, -9.8F, 0.0F});
        dayo::core::MmdAnimator postPhysicsAnimator(postPhysicsModel);
        postPhysicsAnimator.setPhysics(&postPhysics);
        const auto postBodyBefore = postPhysics.bodyTransform(0);
        const auto postPhysicsBefore = postPhysicsAnimator.evaluate(0.0F, 0.0F);
        const auto postPhysicsAfter = postPhysicsAnimator.evaluate(1.0F, 1.0F / 30.0F);
        const auto& beforeVertex = postPhysicsBefore.vertices[0].position;
        const auto& afterVertex = postPhysicsAfter.vertices[0].position;
        const auto postPhysicsMovement = std::abs(afterVertex[0] - beforeVertex[0]) +
                                         std::abs(afterVertex[1] - beforeVertex[1]) +
                                         std::abs(afterVertex[2] - beforeVertex[2]);
        const auto postBodyAfter = postPhysics.bodyTransform(0);
        ok &= check(postBodyAfter.position != postBodyBefore.position && postPhysicsMovement > 1e-4F,
                    "post-physics IK uses the dynamic rigid-body result before skinning");

        auto reversePhaseModel = postPhysicsModel;
        reversePhaseModel.bones[1].flags = 0x1000U;
        reversePhaseModel.bones[2].flags = 0x1000U;
        reversePhaseModel.bones[3].flags = 0x0020U;
        reversePhaseModel.rigidBodies.clear();
        dayo::core::MmdAnimator reversePhaseAnimator(reversePhaseModel);
        const auto reversePhaseFrame = reversePhaseAnimator.evaluate(0.0F);
        ok &= check(reversePhaseFrame.vertices[0].position[1] > 1.0F,
                    "pre-physics IK preserves a rotation on a post-physics link");
#else
        ok &= check(!physics.available(), "optional Bullet fallback");
#endif
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: physics: " << exception.what() << '\n';
        ok = false;
    }
    try {
        dayo::core::PmxModel model;
        model.metadata.modelName = "synthetic-motion-model";
        model.vertices.resize(1);
        model.vertices[0].position = {0.0F, 2.0F, 0.0F};
        model.vertices[0].normal = {0.0F, 0.0F, 1.0F};
        model.vertices[0].bones[0] = 1;
        model.bones.resize(2);
        model.bones[0].name = "root";
        model.bones[0].position = {0.0F, 0.0F, 0.0F};
        model.bones[1].name = "child";
        model.bones[1].position = {0.0F, 1.0F, 0.0F};
        model.bones[1].parent = 0;

        dayo::core::VmdMotion motion;
        dayo::core::VmdBoneKey first;
        first.name = "child";
        first.frame = 0;
        first.interpolation.fill(127);
        dayo::core::VmdBoneKey last = first;
        last.frame = 10;
        last.rotation = {0.0F, 0.0F, std::sin(std::numbers::pi_v<float> * 0.25F),
                         std::cos(std::numbers::pi_v<float> * 0.25F)};
        motion.bones = {first, last};
        motion.lastFrame = 10;

        dayo::core::MmdAnimator animator(model);
        animator.setMotion(&motion);
        const auto compatibility = animator.motionCompatibility();
        ok &= check(compatibility.vmdBoneTrackCount == 1 && compatibility.matchedBoneTrackCount == 1,
                    "motion compatibility reports matched bone tracks");
        const auto before = animator.evaluate(0.0F);
        const auto after = animator.evaluate(10.0F);
        const auto seekBack = animator.evaluate(0.0F);
        const auto gpuFrame = animator.evaluate(10.0F, 0.0F, true);
        ok &= check(before.vertices.size() == 1 && after.vertices.size() == 1 &&
                        before.vertices[0].position != after.vertices[0].position,
                    "matched VMD bone changes skinned vertices");
        ok &= check(std::abs(after.vertices[0].position[0] + 1.0F) < 1e-4F &&
                        std::abs(after.vertices[0].position[1] - 1.0F) < 1e-4F,
                    "skinned vertex follows the animated bone rotation");
        ok &= check(seekBack.vertices[0].position == before.vertices[0].position,
                    "VMD track cursor falls back to binary search for backward seeks");
        const auto& gpuBone = gpuFrame.bones[1];
        const auto& bindPosition = gpuFrame.vertices[0].position;
        const dayo::core::Float3 quaternionAxis{
            gpuBone.rotation[0],
            gpuBone.rotation[1],
            gpuBone.rotation[2],
        };
        const auto dotAxis = quaternionAxis[0] * bindPosition[0] + quaternionAxis[1] * bindPosition[1] +
                             quaternionAxis[2] * bindPosition[2];
        const auto dotSelf = quaternionAxis[0] * quaternionAxis[0] + quaternionAxis[1] * quaternionAxis[1] +
                             quaternionAxis[2] * quaternionAxis[2];
        const dayo::core::Float3 crossAxis{
            quaternionAxis[1] * bindPosition[2] - quaternionAxis[2] * bindPosition[1],
            quaternionAxis[2] * bindPosition[0] - quaternionAxis[0] * bindPosition[2],
            quaternionAxis[0] * bindPosition[1] - quaternionAxis[1] * bindPosition[0],
        };
        dayo::core::Float3 gpuPosition{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            gpuPosition[axis] = 2.0F * dotAxis * quaternionAxis[axis] +
                                (gpuBone.rotation[3] * gpuBone.rotation[3] - dotSelf) * bindPosition[axis] +
                                2.0F * gpuBone.rotation[3] * crossAxis[axis] + gpuBone.translation[axis];
        }
        ok &= check(gpuFrame.bones.size() == model.bones.size() &&
                        gpuFrame.vertices[0].position == model.vertices[0].position &&
                        std::abs(gpuPosition[0] - after.vertices[0].position[0]) < 1e-4F &&
                        std::abs(gpuPosition[1] - after.vertices[0].position[1]) < 1e-4F,
                    "GPU BDEF transform matches CPU skinning output");

        dayo::core::PmxModel sdefModel;
        sdefModel.vertices.resize(1);
        sdefModel.vertices[0].position = {1.0F, 0.0F, 0.0F};
        sdefModel.vertices[0].normal = {0.0F, 1.0F, 0.0F};
        sdefModel.vertices[0].weightType = dayo::core::PmxWeightType::sdef;
        sdefModel.vertices[0].bones = {0, 1, -1, -1};
        sdefModel.vertices[0].weights = {0.5F, 0.5F, 0.0F, 0.0F};
        sdefModel.vertices[0].sdefC = {0.0F, 0.0F, 0.0F};
        sdefModel.vertices[0].sdefR0 = {0.0F, 0.0F, 0.0F};
        sdefModel.vertices[0].sdefR1 = {0.0F, 0.0F, 0.0F};
        sdefModel.bones.resize(2);
        sdefModel.bones[0].name = "sdef-root";
        sdefModel.bones[1].name = "sdef-static";
        dayo::core::VmdMotion sdefMotion;
        sdefMotion.bones.push_back({.name = "sdef-root",
                                    .frame = 0,
                                    .rotation = {0.0F, 0.0F, std::sin(std::numbers::pi_v<float> * 0.25F),
                                                 std::cos(std::numbers::pi_v<float> * 0.25F)}});
        dayo::core::MmdAnimator sdefCpuAnimator(sdefModel);
        dayo::core::MmdAnimator sdefGpuAnimator(sdefModel);
        sdefCpuAnimator.setMotion(&sdefMotion);
        sdefGpuAnimator.setMotion(&sdefMotion);
        const auto sdefCpu = sdefCpuAnimator.evaluate(0.0F, 0.0F, false);
        const auto sdefGpu = sdefGpuAnimator.evaluate(0.0F, 0.0F, true);
        ok &= check(sdefGpu.vertices[0].position == sdefModel.vertices[0].position &&
                        sdefCpu.vertices[0].position != sdefGpu.vertices[0].position,
                    "GPU SDEF path preserves source data for shader deformation");

        sdefModel.vertices[0].weightType = dayo::core::PmxWeightType::qdef;
        sdefModel.vertices[0].weights = {0.5F, 0.5F, 0.0F, 0.0F};
        dayo::core::MmdAnimator qdefCpuAnimator(sdefModel);
        dayo::core::MmdAnimator qdefGpuAnimator(sdefModel);
        qdefCpuAnimator.setMotion(&sdefMotion);
        qdefGpuAnimator.setMotion(&sdefMotion);
        const auto qdefCpu = qdefCpuAnimator.evaluate(0.0F, 0.0F, false);
        const auto qdefGpu = qdefGpuAnimator.evaluate(0.0F, 0.0F, true);
        ok &= check(qdefGpu.vertices[0].position == sdefModel.vertices[0].position &&
                        qdefCpu.vertices[0].position != qdefGpu.vertices[0].position,
                    "GPU QDEF path preserves source data for dual-quaternion deformation");
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: synthetic animation: " << exception.what() << '\n';
        ok = false;
    }
    try {
        dayo::core::PmxModel grantModel;
        grantModel.metadata.modelName = "synthetic-grant-model";
        grantModel.vertices.resize(1);
        grantModel.vertices[0].position = {0.0F, 2.0F, 0.0F};
        grantModel.vertices[0].normal = {0.0F, 0.0F, 1.0F};
        grantModel.vertices[0].bones[0] = 1;
        grantModel.bones.resize(4);
        grantModel.bones[0].name = "root";
        grantModel.bones[1].name = "grant-target";
        grantModel.bones[1].position = {0.0F, 1.0F, 0.0F};
        grantModel.bones[1].parent = 0;
        grantModel.bones[1].deformLayer = 3;
        grantModel.bones[1].flags = 0x0180U;
        grantModel.bones[1].inheritParent = 2;
        grantModel.bones[1].inheritRatio = 1.0F;
        grantModel.bones[2].name = "grant-source";
        grantModel.bones[2].parent = 0;
        grantModel.bones[2].deformLayer = 2;
        grantModel.bones[2].flags = 0x0100U;
        grantModel.bones[2].inheritParent = 3;
        grantModel.bones[2].inheritRatio = 1.0F;
        grantModel.bones[3].name = "grant-source-root";
        grantModel.bones[3].parent = 0;
        grantModel.bones[3].deformLayer = 1;

        dayo::core::VmdMotion grantMotion;
        dayo::core::VmdBoneKey grantKey;
        grantKey.name = "grant-source-root";
        grantKey.frame = 0;
        grantKey.rotation = {0.0F, 0.0F, std::sin(std::numbers::pi_v<float> * 0.25F),
                             std::cos(std::numbers::pi_v<float> * 0.25F)};
        grantMotion.bones.push_back(grantKey);
        dayo::core::VmdBoneKey appendBaseKey;
        appendBaseKey.name = "grant-source";
        appendBaseKey.frame = 0;
        appendBaseKey.rotation = {0.0F, 0.0F, std::sin(std::numbers::pi_v<float> * 0.125F),
                                  std::cos(std::numbers::pi_v<float> * 0.125F)};
        grantMotion.bones.push_back(appendBaseKey);
        grantMotion.lastFrame = 1;
        dayo::core::MmdAnimator grantAnimator(grantModel);
        grantAnimator.setMotion(&grantMotion);
        const auto granted = grantAnimator.evaluate(0.0F);
        const auto half = std::sqrt(0.5F);
        ok &= check(std::abs(granted.vertices[0].position[0] + half) < 1e-4F &&
                        std::abs(granted.vertices[0].position[1] - (1.0F - half)) < 1e-4F,
                    "PMX local append uses the source local transform");

        grantModel.bones[1].flags = 0x0100U;
        dayo::core::MmdAnimator nonLocalGrantAnimator(grantModel);
        nonLocalGrantAnimator.setMotion(&grantMotion);
        const auto nonLocalGranted = nonLocalGrantAnimator.evaluate(0.0F);
        ok &= check(std::abs(nonLocalGranted.vertices[0].position[0] + 1.0F) < 1e-4F &&
                        std::abs(nonLocalGranted.vertices[0].position[1] - 1.0F) < 1e-4F,
                    "PMX non-local append includes a multiple-append source");

        dayo::core::PmxModel fixedAxisModel;
        fixedAxisModel.vertices.resize(1);
        fixedAxisModel.vertices[0].position = {0.0F, 2.0F, 0.0F};
        fixedAxisModel.vertices[0].normal = {0.0F, 0.0F, 1.0F};
        fixedAxisModel.vertices[0].bones[0] = 1;
        fixedAxisModel.bones.resize(2);
        fixedAxisModel.bones[0].name = "root";
        fixedAxisModel.bones[1].name = "fixed-axis";
        fixedAxisModel.bones[1].position = {0.0F, 1.0F, 0.0F};
        fixedAxisModel.bones[1].parent = 0;
        fixedAxisModel.bones[1].flags = 0x0400U;
        fixedAxisModel.bones[1].fixedAxis = {0.0F, 0.0F, 1.0F};
        dayo::core::VmdMotion fixedAxisMotion;
        dayo::core::VmdBoneKey fixedAxisKey;
        fixedAxisKey.name = "fixed-axis";
        fixedAxisKey.frame = 0;
        fixedAxisKey.rotation = {std::sin(std::numbers::pi_v<float> * 0.25F), 0.0F, 0.0F,
                                 std::cos(std::numbers::pi_v<float> * 0.25F)};
        fixedAxisMotion.bones.push_back(fixedAxisKey);
        dayo::core::MmdAnimator fixedAxisAnimator(fixedAxisModel);
        fixedAxisAnimator.setMotion(&fixedAxisMotion);
        const auto fixedAxisFrame = fixedAxisAnimator.evaluate(0.0F);
        ok &= check(fixedAxisFrame.vertices[0].position[2] > 0.5F,
                    "PMX fixed axis does not project an imported VMD rotation");

        dayo::core::PmxModel ikModel;
        ikModel.metadata.modelName = "synthetic-ik-model";
        ikModel.vertices.resize(1);
        ikModel.vertices[0].position = {1.5F, 0.0F, 0.0F};
        ikModel.vertices[0].normal = {0.0F, 0.0F, 1.0F};
        ikModel.vertices[0].bones[0] = 2;
        ikModel.bones.resize(4);
        ikModel.bones[0].name = "root";
        ikModel.bones[1].name = "ik-link";
        ikModel.bones[1].parent = 0;
        ikModel.bones[2].name = "ik-effector";
        ikModel.bones[2].position = {1.0F, 0.0F, 0.0F};
        ikModel.bones[2].parent = 1;
        ikModel.bones[3].name = "ik";
        ikModel.bones[3].position = {0.0F, 1.0F, 0.0F};
        ikModel.bones[3].parent = 0;
        ikModel.bones[3].flags = 0x0020U;
        ikModel.bones[3].ikTarget = 2;
        ikModel.bones[3].ikLoopCount = 8;
        ikModel.bones[3].ikLimitAngle = std::numbers::pi_v<float> * 0.5F;
        ikModel.bones[3].ikLinks.push_back({1, false, {}, {}});
        dayo::core::VmdMotion ikMotion;
        ikMotion.lastFrame = 1;
        ikMotion.ik.push_back({0, true, {{"ik", true}}});
        dayo::core::VmdMotion disabledIkMotion = ikMotion;
        disabledIkMotion.ik[0].states[0].enabled = false;
        dayo::core::MmdAnimator ikAnimator(ikModel);
        ikAnimator.setMotion(&disabledIkMotion);
        const auto ikDisabled = ikAnimator.evaluate(0.0F);
        ikAnimator.setMotion(&ikMotion);
        const auto ikEnabled = ikAnimator.evaluate(0.0F);
        ok &= check(std::abs(ikDisabled.vertices[0].position[1]) < 1e-4F && ikEnabled.vertices[0].position[1] > 1.0F,
                    "VMD IK state toggles PMX IK evaluation");

        dayo::core::VmdMotion unsortedIkMotion = ikMotion;
        unsortedIkMotion.ik = {
            {120, true, {{"ik", false}}},
            {0, true, {{"ik", true}}},
            {60, true, {{"ik", false}}},
        };
        dayo::core::MmdAnimator unsortedIkAnimator(ikModel);
        unsortedIkAnimator.setMotion(&unsortedIkMotion);
        const auto unsortedIkAt0 = unsortedIkAnimator.evaluate(0.0F);
        const auto unsortedIkAt60 = unsortedIkAnimator.evaluate(60.0F);
        ok &= check(unsortedIkAt0.vertices[0].position[1] > 1.0F &&
                        std::abs(unsortedIkAt60.vertices[0].position[1]) < 1e-4F,
                    "VMD IK state evaluation handles unsorted keys");
        const auto normalizedIk = dayo::core::toVmdMotion(dayo::core::toMotionDocument(unsortedIkMotion));
        ok &= check(std::is_sorted(normalizedIk.ik.begin(), normalizedIk.ik.end(),
                                   [](const auto& left, const auto& right) { return left.frame < right.frame; }),
                    "VMD IK keys are sorted when attached");

        dayo::core::PmxModel noSoftBodyModel;
        noSoftBodyModel.vertices.resize(2);
        noSoftBodyModel.vertices[0].position = {0.0F, 0.0F, 0.0F};
        noSoftBodyModel.vertices[1].position = {10.0F, 0.0F, 0.0F};
        dayo::core::SoftBodySimulation noSoftBody(noSoftBodyModel);
        ok &= check(!noSoftBody.available(), "soft-body fallback stays unavailable without PMX soft bodies");
        noSoftBodyModel.indices = {0, 1};
        dayo::core::PmxMaterial softMaterial;
        softMaterial.indexCount = 1;
        noSoftBodyModel.materials.push_back(softMaterial);
        dayo::core::PmxMaterial rigidMaterial;
        rigidMaterial.indexCount = 1;
        noSoftBodyModel.materials.push_back(rigidMaterial);
        dayo::core::PmxSoftBody softBody;
        softBody.material = 0;
        softBody.pinnedVertices = {};
        noSoftBodyModel.softBodies.push_back(softBody);
        dayo::core::SoftBodySimulation softSimulation(noSoftBodyModel);
        std::vector<dayo::core::PmxVertex> skinned(2);
        skinned[0].position = {10.0F, 20.0F, 30.0F};
        skinned[1].position = {40.0F, 50.0F, 60.0F};
        softSimulation.step(1.0F / 30.0F, {0.0F, -9.8F, 0.0F});
        softSimulation.apply(skinned);
        ok &= check(
            softSimulation.available() && softSimulation.bodyCount() == 1 &&
                std::abs(skinned[0].position[0] - 10.0F) < 1e-4F && std::abs(skinned[0].position[2] - 30.0F) < 1e-4F &&
                skinned[0].position[1] < 20.0F && std::abs(skinned[1].position[0] - 40.0F) < 1e-4F &&
                std::abs(skinned[1].position[1] - 50.0F) < 1e-4F && std::abs(skinned[1].position[2] - 60.0F) < 1e-4F,
            "soft-body fallback applies displacement only to its material vertices");
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: PMX transform evaluation: " << exception.what() << '\n';
        ok = false;
    }
    std::error_code error;
    std::filesystem::remove(path, error);
    return ok ? 0 : 1;
}
