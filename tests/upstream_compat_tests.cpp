#include "core/animation.hpp"
#include "core/asset.hpp"
#include "core/effect.hpp"
#include "core/fx/fx_pass.hpp"
#include "core/image.hpp"
#include "core/motion.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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

    // Do not silently run the compatibility suite against a stale 1.20 install.
    {
        std::ifstream lock(std::filesystem::path(DAYO_SOURCE_DIR) / "deps/mikumikudayo.lock");
        std::ifstream marker(sourceDirectory / ".mikumikudayo-ready");
        if (!check(lock.good() && marker.good(), "run scripts/fetch-mikumikudayo.py before upstream tests"))
            return 1;
        std::string expected;
        for (std::string line; std::getline(lock, line);) {
            if (!line.empty() && line.front() != '#')
                expected += line + '\n';
        }
        const std::string actual((std::istreambuf_iterator<char>(marker)), std::istreambuf_iterator<char>());
        if (!check(actual == expected, "upstream installation must match the pinned release lock"))
            return 1;
    }

    try {
        const auto previewEffect = dayo::core::loadEffectGraph(sourceDirectory / "renderer/Preview.fxdayo");
        ok &= check(previewEffect.passes.size() == 5 && !previewEffect.hlsl.empty(), "Preview fxdayo graph");
        const auto previewRaster =
            std::ranges::find_if(previewEffect.passes, [](const auto& pass) { return pass.name == "MMD"; });
        const auto previewGBuffer =
            std::ranges::find_if(previewEffect.passes, [](const auto& pass) { return pass.name == "GBuffer"; });
        ok &= check(previewRaster != previewEffect.passes.end() &&
                        previewRaster->graphics.rasterizer.cullMode == dayo::core::EffectCullMode::none &&
                        previewRaster->graphics.blend.size() == 1 && previewRaster->graphics.blend[0].enabled &&
                        previewRaster->graphics.blend[0].srcColor == "src_alpha" &&
                        previewRaster->graphics.blend[0].dstColor == "inv_src_alpha" &&
                        previewRaster->depth.clearValue.depth == 1.0F,
                    "Preview graphics state is lossless");
        ok &= check(previewGBuffer != previewEffect.passes.end() && previewGBuffer->renderTargets.size() == 3 &&
                        previewGBuffer->renderTargets[1].clearValue.color[0] == -1.0F,
                    "Preview MRT clear values");
        const auto subayaiEffect = dayo::core::loadEffectGraph(sourceDirectory / "renderer/Subayai.fxdayo");
        ok &= check(subayaiEffect.passes.size() >= 20 &&
                        std::ranges::any_of(
                            subayaiEffect.passes,
                            [](const auto& pass) { return pass.type == dayo::core::EffectPassType::raytracing; }) &&
                        subayaiEffect.hlsl.find("resources.hlsli") != std::string::npos &&
                        !subayaiEffect.controllers.empty(),
                    "Subayai Jsonnet expansion");
        const auto subayaiRaster =
            std::ranges::find_if(subayaiEffect.passes, [](const auto& pass) { return pass.name == "MMD"; });
        ok &= check(subayaiRaster != subayaiEffect.passes.end() && subayaiRaster->renderTargets.size() == 4 &&
                        subayaiRaster->graphics.blend.size() == 4 && subayaiRaster->graphics.depthStencil.depthWrite &&
                        subayaiRaster->graphics.depthStencil.depthFunc == dayo::core::EffectDepthFunc::lessEqual,
                    "Subayai MRT/depth/blend state");
        const auto rayPass = std::ranges::find_if(
            subayaiEffect.passes, [](const auto& pass) { return pass.type == dayo::core::EffectPassType::raytracing; });
        ok &= check(rayPass != subayaiEffect.passes.end() && !rayPass->hitGroups.empty() &&
                        rayPass->maxPayloadSize != 0 && rayPass->maxRecursionDepth != 0,
                    "Subayai ray-tracing pipeline metadata");
        const auto cloneEffect = dayo::core::loadEffectGraph(sourceDirectory / "sample/clone_sample.fxdayo");
        const auto cloneBuffer =
            std::ranges::find_if(cloneEffect.buffers, [](const auto& buffer) { return buffer.name == "skinned"; });
        ok &= check(cloneEffect.meshCloneCount == 4 && cloneBuffer != cloneEffect.buffers.end() &&
                        cloneBuffer->size.base == "VERTEXCOUNT",
                    "1.30 mesh cloning and buffer size metadata");
        const auto fluidEffect = dayo::core::loadEffectGraph(sourceDirectory / "postprocess/Fog/fluid3D.fxdayo");
        const auto fluidTexture =
            std::ranges::find_if(fluidEffect.textures3D, [](const auto& texture) { return texture.name == "VMap"; });
        const auto fluidBaseTexture =
            std::ranges::find_if(fluidEffect.textures3D, [](const auto& texture) { return texture.name == "WMap"; });
        ok &= check(fluidTexture != fluidEffect.textures3D.end() && fluidBaseTexture != fluidEffect.textures3D.end() &&
                        fluidTexture->size.base == "WMap" && fluidBaseTexture->size.absolute &&
                        fluidBaseTexture->size.depth > 0 && !fluidEffect.buffers.empty(),
                    "1.30 3D texture size and buffer metadata");
        const auto bdptEffect = dayo::core::loadEffectGraph(sourceDirectory / "renderer/BDPT.fxdayo");
        ok &= check(!bdptEffect.passes.empty() && std::ranges::any_of(bdptEffect.passes,
                                                                      [](const auto& pass) {
                                                                          return pass.type ==
                                                                                 dayo::core::EffectPassType::raytracing;
                                                                      }),
                    "BDPT fxdayo graph");

        const std::string fixture = R"FX([YRZFX]
{
  "fx": {
    "category": "render",
    "passes": [{
      "name": "Fixture",
      "type": "rasterizer",
      "vertexShader": "VS",
      "pixelShader": "PS",
      "RTV": [{"name":"Color", "clear":true, "value":{"x":1,"y":0.5,"z":0,"w":1}}],
      "DSV": {"name":"Depth", "clear":true, "depth":0.25},
      "rasterizerDesc": {"cullMode":"front"},
      "depthStencilDesc": {"depthWriteMask":"zero", "depthFunc":"less_equal"},
      "blendDesc": {"renderTarget0": {"blendEnable":true, "srcBlend":"one", "destBlend":"inv_src_alpha", "blendOp":"add"}},
      "rasterModelTarget":"other"
    }]
  }
}
[HLSL]
float4 PS() : SV_TARGET { return 1; }
)FX";
        const auto fixtureEffect = dayo::core::loadEffectGraphFromText("lossless-fixture.fxdayo", fixture);
        const auto& fixturePass = fixtureEffect.passes.front();
        ok &= check(fixturePass.graphics.rasterizer.cullMode == dayo::core::EffectCullMode::front &&
                        !fixturePass.graphics.depthStencil.depthWrite &&
                        fixturePass.graphics.depthStencil.depthFunc == dayo::core::EffectDepthFunc::lessEqual &&
                        fixturePass.graphics.modelTarget == dayo::core::fx::RasterModelTarget::other &&
                        fixturePass.graphics.blend.front().srcColor == "one" &&
                        fixturePass.renderTargets.front().clearValue.color[1] == 0.5F &&
                        fixturePass.depth.clearValue.depth == 0.25F,
                    "YRZFX graphics state fixture");
        bool rejectedUnknown = false;
        try {
            auto invalid = fixture;
            const auto marker = invalid.find("front");
            invalid.replace(marker, 5, "diagonal");
            static_cast<void>(dayo::core::loadEffectGraphFromText("invalid-lossless-fixture.fxdayo", invalid));
        } catch (const std::runtime_error&) {
            rejectedUnknown = true;
        }
        ok &= check(rejectedUnknown, "YRZFX rejects unknown cull values");
        bool rejectedUnsupportedBlend = false;
        try {
            auto invalid = fixture;
            const auto marker = invalid.find("\"srcBlend\":\"one\"");
            if (marker == std::string::npos)
                throw std::runtime_error("blend fixture marker missing");
            invalid.replace(marker, std::string("\"srcBlend\":\"one\"").size(), "\"srcBlend\":\"src1_color\"");
            static_cast<void>(dayo::core::loadEffectGraphFromText("unsupported-blend-fixture.fxdayo", invalid));
        } catch (const std::runtime_error&) {
            rejectedUnsupportedBlend = true;
        }
        ok &= check(rejectedUnsupportedBlend, "YRZFX rejects blend factors without a native pipeline contract");
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
