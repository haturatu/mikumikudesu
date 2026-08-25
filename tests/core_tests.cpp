#include "core/asset.hpp"
#include "core/animation.hpp"
#include "core/image.hpp"
#include "core/media.hpp"
#include "core/effect.hpp"
#include "core/model_probe.hpp"
#include "core/motion.hpp"
#include "core/physics.hpp"
#include "core/project.hpp"
#include "core/editor.hpp"
#include "core/output.hpp"
#include "core/scene.hpp"
#include "core/vmdayo.hpp"

#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

namespace {

template <typename T>
void append(std::ofstream& output, T value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void appendText(std::ofstream& output, std::string_view text) {
    append(output, static_cast<std::int32_t>(text.size()));
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

bool check(bool value, std::string_view message) {
    if (!value) std::cerr << "FAIL: " << message << '\n';
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
        dayo::core::VmdMotion cameraMotion;
        dayo::core::VmdCameraKey first;
        first.frame = 0;
        first.distance = -4.0F;
        first.viewAngle = 30;
        dayo::core::VmdCameraKey second;
        second.frame = 10;
        second.distance = -8.0F;
        second.position = { 2.0F, 4.0F, 6.0F };
        second.viewAngle = 50;
        for (std::size_t channel = 0; channel < 6; ++channel) {
            second.interpolation[channel * 4] = 20;
            second.interpolation[channel * 4 + 1] = 107;
            second.interpolation[channel * 4 + 2] = 20;
            second.interpolation[channel * 4 + 3] = 107;
        }
        cameraMotion.cameras = { first, second };
        const auto camera = dayo::core::evaluateCamera(cameraMotion, 5.0F);
        ok &= check(std::abs(camera.distance + 6.0F) < 0.001F
                    && std::abs(camera.position[1] - 2.0F) < 0.001F
                    && std::abs(camera.viewAngle - 40.0F) < 0.001F, "VMD camera interpolation");
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
        project.embeddedVmdayo = { 0x56, 0x4D, 0x44, 0x01 };
        project.assets.push_back({ "pmx", std::filesystem::absolute("model.pmx") });
        project.assets.push_back({ "vmd", std::filesystem::absolute("motion.vmd") });
        dayo::core::saveProject(projectPath, project);
        const auto loaded = dayo::core::loadProject(projectPath);
        ok &= check(loaded.renderer == "subayai" && loaded.frame == 42.5F && !loaded.playing,
                    ".dayo project settings round trip");
        ok &= check(loaded.assets.size() == 2 && loaded.assets[0].path.is_absolute(),
                    ".dayo relative asset round trip");
        ok &= check(loaded.version == 3, ".dayo v3 writer");
        ok &= check(loaded.embeddedVmdayo == project.embeddedVmdayo, ".dayo embedded VMdayo payload");
        {
            std::ofstream legacy(projectPath, std::ios::binary | std::ios::trunc);
            legacy << "[MikuMikuDayo]\n"
                      "{\"MikuMikuDayo\":{\"ver\":1,\"assetPath\":\".\","
                      "\"editor\":{\"frame\":12},\"models\":[{\"filename\":\"legacy.pmx\"}],"
                      "\"fxinfo\":[]}}\n[BinaryDayo]\n";
            for (int section = 0; section < 7; ++section) append(legacy, std::int32_t { 0 });
            append(legacy, std::int32_t { 1 });
            appendText(legacy, "Bone");
            append(legacy, std::int32_t { 7 });
            const std::array<float, 3> translation { 1.0F, 2.0F, 3.0F };
            const std::array<float, 4> rotation { 0.0F, 0.0F, 0.0F, 1.0F };
            const std::array<std::uint8_t, 16> interpolation {};
            legacy.write(reinterpret_cast<const char*>(translation.data()), sizeof(translation));
            legacy.write(reinterpret_cast<const char*>(rotation.data()), sizeof(rotation));
            legacy.write(reinterpret_cast<const char*>(interpolation.data()), sizeof(interpolation));
            append(legacy, std::uint8_t { 1 });
            for (int section = 0; section < 6; ++section) append(legacy, std::int32_t { 0 });
        }
        const auto legacy = dayo::core::loadProject(projectPath);
        ok &= check(legacy.frame == 12.0F && legacy.assets.size() == 1
                    && legacy.embeddedMotion && legacy.embeddedMotion->bones.size() == 1
                    && legacy.embeddedMotion->bones[0].frame == 7,
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
        document.motion.interpolation = dayo::core::InterpolationMode::catmullRom;
        document.motion.morphs.push_back({ "smile", 10, 0.75F });
        dayo::core::saveVmdayo(vmdayoPath, document);
        const auto loaded = dayo::core::loadVmdayo(vmdayoPath);
        ok &= check(loaded.motion.morphs.size() == 1 && loaded.motion.interpolation
                    == dayo::core::InterpolationMode::catmullRom, "VMdayo round trip");
        dayo::core::Scene vmdayoScene;
        vmdayoScene.attachMotion(loaded.motion, 0, loaded.modelName);
        ok &= check(vmdayoScene.timeline().duration == 10.0F,
                    "VMdayo attachment updates timeline duration");
        document.opaque = { 0x00, 0xFF, 0x56, 0x4D, 0x44 };
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
        ok &= check(scene.accumulatedSamples() == 3,
                    "accumulate runtime advances samples without advancing timeline");
        scene.setRuntimeMode(dayo::core::RuntimeMode::realtime);
        history.execute(scene, std::make_unique<dayo::core::SetFrameCommand>(0.0F, 24.0F));
        ok &= check(scene.timeline().frame == 24.0F && history.canUndo(), "editor command apply");
        ok &= check(history.undo(scene) && scene.timeline().frame == 0.0F, "editor command undo");
        ok &= check(history.redo(scene) && scene.timeline().frame == 24.0F, "editor command redo");
        dayo::core::MaterialParameterBlock parameters;
        parameters.set("roughness", 0.5F);
        ok &= check(parameters.find("roughness") != nullptr && parameters.erase("roughness"),
                    "material parameter block");
    }

    const auto path = std::filesystem::temp_directory_path() / "mikumikudesu-core-test.pmx";
    {
        std::ofstream output(path, std::ios::binary);
        output.write("PMX ", 4);
        append(output, 2.0F);
        append(output, static_cast<std::uint8_t>(8));
        const std::array<std::uint8_t, 8> settings { 1, 0, 4, 4, 4, 4, 4, 4 };
        output.write(reinterpret_cast<const char*>(settings.data()), settings.size());
        appendText(output, "Miku");
        appendText(output, "Miku English");
        appendText(output, "comment");
        appendText(output, "comment en");
        append(output, static_cast<std::int32_t>(3));
        for (std::int32_t vertex = 0; vertex < 3; ++vertex) {
            const std::array<float, 3> position { static_cast<float>(vertex), static_cast<float>(vertex & 1), 0.0F };
            const std::array<float, 3> normal { 0.0F, 0.0F, 1.0F };
            const std::array<float, 2> uv { 0.0F, 0.0F };
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
        const std::array<float, 4> diffuse { 1.0F, 1.0F, 1.0F, 1.0F };
        const std::array<float, 3> vector3 {};
        const std::array<float, 4> vector4 {};
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
        for (int section = 0; section < 5; ++section) append(output, static_cast<std::int32_t>(0));
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
        const auto firstModel = scene.addModel(path);
        const auto secondModel = scene.addModel(path);
        ok &= check(scene.models().size() == 2 && scene.selectedModel() != nullptr,
                    "multi-model scene instances");
        dayo::core::VmdMotion shortMotion;
        shortMotion.morphs.push_back({ "smile", 20, 1.0F });
        scene.attachMotion(std::move(shortMotion), firstModel);
        dayo::core::VmdMotion longMotion;
        longMotion.morphs.push_back({ "smile", 100, 1.0F });
        scene.attachMotion(std::move(longMotion), secondModel);
        ok &= check(scene.timeline().duration == 100.0F,
                    "timeline duration follows the longest model motion");
        scene.setFrame(80.0F);
        ok &= check(scene.removeModel(secondModel) && scene.timeline().duration == 20.0F,
                    "removing longest motion recalculates timeline duration");
        ok &= check(scene.timeline().frame == 20.0F,
                    "removing longest motion clamps the current frame");
        std::string parentError;
        ok &= check(!scene.addExternalParent({ 1, "missing", 2, "missing" }, &parentError)
                    && !parentError.empty() && !scene.hasExternalParentCycle(),
                    "external parent validation without fake solve");
        scene.setGravityTrack({ { 12, { 3.0F, { 0.0F, -1.0F, 0.0F }, 0.0F, 0.0F, false } } });
        ok &= check(scene.evaluatePhysicsSettings(12.0F).gravity == 3.0F, "animated gravity settings");
        const auto beforeDirty = scene.accumulatedSamples();
        scene.advanceAccumulation();
        scene.setPhysicsSettings({ 6.0F, { 0.0F, -1.0F, 0.0F }, 0.5F, 2.0F, true });
        ok &= check(scene.physicsSettings().floorCollision && scene.accumulatedSamples() == 0
                    && beforeDirty == 0, "physics settings invalidate accumulation");

        dayo::core::VmdMotion cameraMotion;
        cameraMotion.cameras.push_back({ 500, -45.0F, {}, {}, {}, 30, true });
        scene.attachMotion(std::move(cameraMotion), firstModel);
        scene.setFrame(400.0F);
        ok &= check(scene.cameraMotion() != nullptr && scene.timeline().duration == 500.0F,
                    "camera motion is attached as global scene state");
        scene.clearProjectState();
        ok &= check(scene.models().empty() && scene.cameraMotion() == nullptr
                    && scene.timeline().duration == 0.0F && scene.timeline().frame == 0.0F,
                    "project reset clears camera motion and timeline state");
        const auto resetModel = scene.addModel(path);
        dayo::core::VmdMotion resetMotion;
        resetMotion.morphs.push_back({ "smile", 100, 1.0F });
        scene.attachMotion(std::move(resetMotion), resetModel);
        ok &= check(scene.cameraMotion() == nullptr && scene.timeline().duration == 100.0F,
                    "new project motion is not shadowed by previous camera motion");
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: PMX probe: " << exception.what() << '\n';
        ok = false;
    }
    try {
        const auto previewEffect = dayo::core::loadEffectGraph(
            std::filesystem::path(DAYO_SOURCE_DIR) / "MikuMikuDayo/renderer/Preview.fxdayo");
        ok &= check(previewEffect.passes.size() == 5 && !previewEffect.hlsl.empty(),
                    "Preview fxdayo graph");
        const auto subayaiEffect = dayo::core::loadEffectGraph(
            std::filesystem::path(DAYO_SOURCE_DIR) / "MikuMikuDayo/renderer/Subayai.fxdayo");
        ok &= check(subayaiEffect.passes.size() >= 20 &&
                    std::ranges::any_of(subayaiEffect.passes, [](const auto& pass) {
                        return pass.type == dayo::core::EffectPassType::raytracing;
                    }), "Subayai Jsonnet expansion");
        const auto bdptEffect = dayo::core::loadEffectGraph(
            std::filesystem::path(DAYO_SOURCE_DIR) / "MikuMikuDayo/renderer/BDPT.fxdayo");
        ok &= check(!bdptEffect.passes.empty() &&
                    std::ranges::any_of(bdptEffect.passes, [](const auto& pass) {
                        return pass.type == dayo::core::EffectPassType::raytracing;
                    }), "BDPT fxdayo graph");
        const auto compiled = dayo::core::compileEffectGraph(previewEffect);
        ok &= check(compiled.passes.size() == previewEffect.passes.size(), "effect render graph compilation");
        const auto stats = dayo::core::EffectExecutor {}.execute(compiled, {});
        ok &= check(stats.rasterPasses > 0, "effect pass execution contract");
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: effect graph: " << exception.what() << '\n';
        ok = false;
    }
    try {
        const auto outputDirectory = std::filesystem::temp_directory_path() / "mikumikudesu-output-test";
        dayo::core::OutputSettings settings;
        settings.directory = outputDirectory;
        settings.firstFrame = settings.lastFrame = 3;
        dayo::core::OutputQueue queue(settings);
        dayo::core::ImageRgba8 image { 1, 1, { 255, 64, 32, 255 } };
        queue.push(3, std::move(image));
        static_cast<void>(queue.written());
        queue.close();
        ok &= check(queue.written() == 1 && std::filesystem::exists(outputDirectory / "frame_000003.ppm"),
                    "asynchronous frame output");
        dayo::core::ImageRgba8 pngImage { 1, 1, { 255, 64, 32, 255 } };
        dayo::core::writeFrame(outputDirectory / "frame.png", pngImage, dayo::core::OutputFormat::png);
        ok &= check(std::filesystem::file_size(outputDirectory / "frame.png") > 8, "PNG frame output");
        ok &= check(queue.written() == 1, "thread-safe output counter");
        dayo::core::OutputSettings failingSettings;
        failingSettings.directory = outputDirectory;
        failingSettings.format = dayo::core::OutputFormat::exr;
        dayo::core::OutputQueue failingQueue(failingSettings);
        failingQueue.push(0, dayo::core::ImageRgba8 { 1, 1, { 0, 0, 0, 255 } });
        failingQueue.close();
        bool propagated = false;
        try { failingQueue.rethrowIfFailed(); }
        catch (const std::exception&) { propagated = true; }
        ok &= check(propagated, "output worker error propagation");
        std::error_code outputError;
        std::filesystem::remove_all(outputDirectory, outputError);
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: output: " << exception.what() << '\n';
        ok = false;
    }
    try {
        const auto mediaPath = std::filesystem::temp_directory_path() / "mikumikudesu-media-test.wav";
        {
            std::ofstream output(mediaPath, std::ios::binary);
            constexpr std::uint32_t sampleCount = 800;
            output.write("RIFF", 4); append(output, 36U + sampleCount * 2U);
            output.write("WAVEfmt ", 8); append(output, 16U);
            append(output, static_cast<std::uint16_t>(1));
            append(output, static_cast<std::uint16_t>(1));
            append(output, 8'000U); append(output, 16'000U);
            append(output, static_cast<std::uint16_t>(2));
            append(output, static_cast<std::uint16_t>(16));
            output.write("data", 4); append(output, sampleCount * 2U);
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
#endif
        std::error_code mediaError;
        std::filesystem::remove(mediaPath, mediaError);
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: media: " << exception.what() << '\n';
        ok = false;
    }
    try {
        dayo::core::PmxModel physicsModel;
        dayo::core::PmxRigidBody falling;
        falling.shape = 0;
        falling.size = { 0.5F, 0.5F, 0.5F };
        falling.position = { 0.0F, 2.0F, 0.0F };
        falling.mass = 1.0F;
        falling.mode = 1;
        falling.collisionMask = 0xFFFFU;
        falling.bone = 0;
        physicsModel.rigidBodies.push_back(falling);
        dayo::core::MmdPhysics physics(physicsModel);
#if DAYO_HAS_BULLET
        ok &= check(physics.available() && physics.bodyCount() == 1, "Bullet PMX body creation");
        const auto before = physics.bodyTransform(0);
        for (int i = 0; i < 60; ++i) physics.step(1.0F / 60.0F);
        const auto after = physics.bodyTransform(0);
        ok &= check(after.position[1] < before.position[1], "Bullet gravity simulation");
        physics.setFloorCollision(true);
        physics.reset();
        for (int i = 0; i < 120; ++i) physics.step(1.0F / 60.0F);
        ok &= check(physics.bodyTransform(0).position[1] > -0.1F, "Bullet floor collision setting");
        physics.setGravityNoise(1.0F, 2.0F);
        physicsModel.vertices.resize(1);
        physicsModel.vertices[0].position = { 0.0F, 2.0F, 0.0F };
        physicsModel.vertices[0].normal = { 0.0F, 1.0F, 0.0F };
        physicsModel.vertices[0].bones[0] = 0;
        dayo::core::PmxBone rootBone;
        rootBone.position = { 0.0F, 2.0F, 0.0F };
        physicsModel.bones.push_back(rootBone);
        dayo::core::MmdPhysics animatedPhysics(physicsModel);
        dayo::core::MmdAnimator physicalAnimator(physicsModel);
        physicalAnimator.setPhysics(&animatedPhysics);
        const auto physicalBefore = physicalAnimator.evaluate(0.0F, 0.0F);
        const auto physicalAfter = physicalAnimator.evaluate(1.0F, 1.0F / 30.0F);
        ok &= check(physicalAfter.vertices[0].position[1] < physicalBefore.vertices[0].position[1],
                    "Bullet body drives PMX bone skinning");
#else
        ok &= check(!physics.available(), "optional Bullet fallback");
#endif
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: physics: " << exception.what() << '\n';
        ok = false;
    }
    try {
        const auto icon = dayo::core::loadImageRgba8(
            std::filesystem::path(DAYO_SOURCE_DIR) / "MikuMikuDayo/res/dayoicon.png");
        ok &= check(icon.width > 0 && icon.height > 0 && icon.pixels.size() == icon.width * icon.height * 4U,
                    "RGBA image decode");
        const auto dds = dayo::core::loadImageRgba8(
            std::filesystem::path(DAYO_SOURCE_DIR) / "MikuMikuDayo/particle/Smoke.dds");
        ok &= check(dds.width > 0 && dds.height > 0 && dds.pixels.size() == dds.width * dds.height * 4U,
                    "DDS image decode");
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: image load: " << exception.what() << '\n';
        ok = false;
    }
    try {
        std::filesystem::path sampleVmd;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                 std::filesystem::path(DAYO_SOURCE_DIR) / "MikuMikuDayo/sample")) {
            if (entry.path().extension() == ".vmd") { sampleVmd = entry.path(); break; }
        }
        const auto motion = dayo::core::loadVmd(sampleVmd);
        ok &= check(!motion.modelName.empty(), "VMD CP932 model name");
        ok &= check(!motion.bones.empty(), "VMD bone keys");
        for (const auto& entry : std::filesystem::directory_iterator(
                 std::filesystem::path(DAYO_SOURCE_DIR) / "MikuMikuDayo/sample")) {
            if (entry.path().extension() != ".pmx") continue;
            try {
                auto candidate = dayo::core::loadPmxModel(entry.path());
                if (candidate.metadata.modelName != motion.modelName || candidate.vertices.empty()) continue;
                dayo::core::MmdAnimator animator(candidate);
                animator.setMotion(&motion);
                const auto first = animator.evaluate(0.0F);
                const auto animated = animator.evaluate(10.0F);
                bool changed = false;
                for (std::size_t i = 0; i < first.vertices.size(); ++i) {
                    if (first.vertices[i].position != animated.vertices[i].position) { changed = true; break; }
                }
                ok &= check(changed, "VMD CPU skinning changes vertices");
                break;
            } catch (const std::exception&) {
                // Some tiny effect descriptors use the PMX extension without model sections.
            }
        }
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: VMD load: " << exception.what() << '\n';
        ok = false;
    }
    std::error_code error;
    std::filesystem::remove(path, error);
    return ok ? 0 : 1;
}
