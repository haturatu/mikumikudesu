#include "core/asset.hpp"
#include "core/animation.hpp"
#include "core/image.hpp"
#include "core/media.hpp"
#include "core/effect.hpp"
#include "core/model_probe.hpp"
#include "core/motion.hpp"
#include "core/physics.hpp"
#include "core/project.hpp"

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
    }
    try {
        const auto projectPath = std::filesystem::temp_directory_path() / "mikumikudesu-project-test.dayo";
        dayo::core::DayoProject project;
        project.renderer = "subayai";
        project.frame = 42.5F;
        project.playing = false;
        project.assets.push_back({ "pmx", std::filesystem::absolute("model.pmx") });
        project.assets.push_back({ "vmd", std::filesystem::absolute("motion.vmd") });
        dayo::core::saveProject(projectPath, project);
        const auto loaded = dayo::core::loadProject(projectPath);
        ok &= check(loaded.renderer == "subayai" && loaded.frame == 42.5F && !loaded.playing,
                    ".dayo project settings round trip");
        ok &= check(loaded.assets.size() == 2 && loaded.assets[0].path.is_absolute(),
                    ".dayo relative asset round trip");
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
        std::error_code projectError;
        std::filesystem::remove(projectPath, projectError);
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: project: " << exception.what() << '\n';
        ok = false;
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
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: effect graph: " << exception.what() << '\n';
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
        falling.bone = 0;
        physicsModel.rigidBodies.push_back(falling);
        dayo::core::MmdPhysics physics(physicsModel);
#if DAYO_HAS_BULLET
        ok &= check(physics.available() && physics.bodyCount() == 1, "Bullet PMX body creation");
        const auto before = physics.bodyTransform(0);
        for (int i = 0; i < 60; ++i) physics.step(1.0F / 60.0F);
        const auto after = physics.bodyTransform(0);
        ok &= check(after.position[1] < before.position[1], "Bullet gravity simulation");
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
