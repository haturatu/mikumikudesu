#include "core/animation.hpp"
#include "core/asset.hpp"
#include "core/effect.hpp"
#include "core/image.hpp"
#include "core/motion.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

bool check(bool value, std::string_view message) {
    if (!value)
        std::cerr << "FAIL: " << message << '\n';
    return value;
}

} // namespace

int main() {
    const auto sourceDirectory = std::filesystem::path(DAYO_SOURCE_DIR) / "MikuMikuDayo";
    bool ok = true;

    try {
        const auto previewEffect = dayo::core::loadEffectGraph(sourceDirectory / "renderer/Preview.fxdayo");
        ok &= check(previewEffect.passes.size() == 5 && !previewEffect.hlsl.empty(), "Preview fxdayo graph");
        const auto subayaiEffect = dayo::core::loadEffectGraph(sourceDirectory / "renderer/Subayai.fxdayo");
        ok &= check(subayaiEffect.passes.size() >= 20 &&
                        std::ranges::any_of(
                            subayaiEffect.passes,
                            [](const auto& pass) { return pass.type == dayo::core::EffectPassType::raytracing; }) &&
                        subayaiEffect.hlsl.find("resources.hlsli") != std::string::npos &&
                        !subayaiEffect.controllers.empty(),
                    "Subayai Jsonnet expansion");
        const auto rayPass = std::ranges::find_if(
            subayaiEffect.passes, [](const auto& pass) { return pass.type == dayo::core::EffectPassType::raytracing; });
        ok &= check(rayPass != subayaiEffect.passes.end() && !rayPass->hitGroups.empty() &&
                        rayPass->maxPayloadSize != 0 && rayPass->maxRecursionDepth != 0,
                    "Subayai ray-tracing pipeline metadata");
        const auto bdptEffect = dayo::core::loadEffectGraph(sourceDirectory / "renderer/BDPT.fxdayo");
        ok &= check(!bdptEffect.passes.empty() && std::ranges::any_of(bdptEffect.passes,
                                                                      [](const auto& pass) {
                                                                          return pass.type ==
                                                                                 dayo::core::EffectPassType::raytracing;
                                                                      }),
                    "BDPT fxdayo graph");
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: effect graph: " << exception.what() << '\n';
        ok = false;
    }

    try {
        const auto icon = dayo::core::loadImageRgba8(sourceDirectory / "res/dayoicon.png");
        ok &= check(icon.width > 0 && icon.height > 0 && icon.pixels.size() == icon.width * icon.height * 4U,
                    "RGBA image decode");
        const auto dds = dayo::core::loadImageRgba8(sourceDirectory / "particle/Smoke.dds");
        ok &= check(dds.width > 0 && dds.height > 0 && dds.pixels.size() == dds.width * dds.height * 4U,
                    "DDS image decode");
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: image load: " << exception.what() << '\n';
        ok = false;
    }

    try {
        std::filesystem::path sampleVmd;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(sourceDirectory / "sample")) {
            if (entry.path().extension() == ".vmd") {
                sampleVmd = entry.path();
                break;
            }
        }
        if (sampleVmd.empty())
            throw std::runtime_error("no sample VMD was found");
        const auto motion = dayo::core::loadVmd(sampleVmd);
        ok &= check(!motion.modelName.empty(), "VMD CP932 model name");
        ok &= check(!motion.bones.empty(), "VMD bone keys");
        const auto exportedVmd = std::filesystem::temp_directory_path() / "mikumikudesu-vmd-export-test.vmd";
        dayo::core::saveVmd(exportedVmd, motion);
        const auto exported = dayo::core::loadVmd(exportedVmd);
        ok &= check(exported.modelName == motion.modelName && exported.bones.size() == motion.bones.size() &&
                        exported.morphs.size() == motion.morphs.size() &&
                        exported.cameras.size() == motion.cameras.size() &&
                        exported.lights.size() == motion.lights.size() &&
                        exported.shadows.size() == motion.shadows.size() && exported.ik.size() == motion.ik.size(),
                    "VMD export round trip");
        std::error_code exportError;
        std::filesystem::remove(exportedVmd, exportError);
        bool evaluatedFixture = false;
        for (const auto& entry : std::filesystem::directory_iterator(sourceDirectory / "sample")) {
            if (entry.path().extension() != ".pmx")
                continue;
            try {
                auto candidate = dayo::core::loadPmxModel(entry.path());
                if (candidate.metadata.modelName != motion.modelName || candidate.vertices.empty())
                    continue;
                dayo::core::MmdAnimator animator(candidate);
                animator.setMotion(&motion);
                const auto compatibility = animator.motionCompatibility();
                ok &= check(compatibility.matchedBoneTrackCount > 0, "sample VMD/PMX has compatible bone tracks");
                const auto first = animator.evaluate(0.0F);
                const auto animated = animator.evaluate(10.0F);
                bool changed = false;
                for (std::size_t i = 0; i < first.vertices.size(); ++i) {
                    if (first.vertices[i].position != animated.vertices[i].position) {
                        changed = true;
                        break;
                    }
                }
                ok &= check(changed, "VMD CPU skinning changes vertices");
                evaluatedFixture = true;
                break;
            } catch (const std::exception&) {
                // Some tiny effect descriptors use the PMX extension without model sections.
            }
        }
        ok &= check(evaluatedFixture, "sample VMD has a matching PMX fixture");
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: VMD load: " << exception.what() << '\n';
        ok = false;
    }

    return ok ? 0 : 1;
}
