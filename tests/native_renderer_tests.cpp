#include "core/image_hdr.hpp"
#include "fx/fx_compiler.hpp"
#include "graphics/deformer_resource_registry.hpp"
#include "graphics/fx_raster_semantics.hpp"
#include "graphics/fx_shared_resource_registry.hpp"
#include "graphics/native_fx_pending_events.hpp"
#include "graphics/native_renderer.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

bool check(bool value, std::string_view message) {
    if (!value)
        std::cerr << "FAIL: " << message << '\n';
    return value;
}

dayo::graphics::DeviceCapabilities capableSubayai() {
    return {
        .gpuName = "test",
        .driverName = "test",
        .swapchain = true,
        .bufferDeviceAddress = true,
        .descriptorIndexing = true,
        .accelerationStructure = true,
        .rayQuery = true,
        .fragmentShaderBarycentric = true,
        .nativeSubayai = true,
    };
}

} // namespace

int main() {
    bool ok = true;

    dayo::graphics::FxResourceStore exportedStore;
    dayo::graphics::FxResourceStore::Resource exportedTexture{
        .name = "OutBuf",
        .kind = dayo::graphics::FxResourceStore::Kind::texture,
        .texture = {4, 2},
        .extent = {32, 16, 1},
        .format = dayo::graphics::PixelFormat::rgba16Float,
        .dimension = 2,
        .allocationBytes = 4096,
        .elementSize = 0,
        .elementType = {},
    };
    ok &= check(exportedStore.add(exportedTexture), "deformer resource fixture enters physical store");
    dayo::graphics::FxResourceStore::Resource exportedBuffer{
        .name = "Particles",
        .kind = dayo::graphics::FxResourceStore::Kind::buffer,
        .buffer = {5, 1},
        .extent = {64, 1, 1},
        .format = dayo::graphics::PixelFormat::rgba8Unorm,
        .dimension = 1,
        .allocationBytes = 2048,
        .elementSize = 32,
        .elementType = "Particle",
    };
    ok &= check(exportedStore.add(exportedBuffer), "buffer resource fixture enters physical store");
    dayo::graphics::FxResourceStore::Resource exportedSampler{
        .name = "LinearSampler",
        .kind = dayo::graphics::FxResourceStore::Kind::sampler,
        .sampler = {6, 1},
        .extent = {},
        .format = dayo::graphics::PixelFormat::rgba8Unorm,
        .dimension = 0,
        .allocationBytes = 0,
        .elementSize = 0,
        .elementType = {},
    };
    ok &= check(exportedStore.add(exportedSampler), "sampler resource fixture enters physical store");
    const auto liveResourceFixture = dayo::graphics::snapshotFxResources("deform.fxdayo", exportedStore);
    const auto findLiveResource = [&liveResourceFixture](std::string_view name) {
        return std::ranges::find_if(liveResourceFixture,
                                    [name](const auto& resource) { return resource.name == name; });
    };
    const auto liveTexture = findLiveResource("OutBuf");
    const auto liveBuffer = findLiveResource("Particles");
    const auto liveSampler = findLiveResource("LinearSampler");
    ok &= check(liveResourceFixture.size() == 3 && liveTexture != liveResourceFixture.end() &&
                    liveTexture->effect == "deform.fxdayo" && liveTexture->kind == "Texture" &&
                    liveTexture->format == "RGBA16_FLOAT" && liveTexture->extent.width == 32 &&
                    liveTexture->extent.height == 16 && liveTexture->dimension == 2 &&
                    liveTexture->allocationBytes == 4096,
                "FX texture snapshots retain format, extent, and allocation size without handles");
    ok &= check(liveBuffer != liveResourceFixture.end() && liveBuffer->kind == "Buffer" && liveBuffer->format.empty() &&
                    liveBuffer->allocationBytes == 2048 && liveBuffer->elementSize == 32 &&
                    liveBuffer->elementType == "Particle",
                "FX buffer snapshots show byte/type metadata instead of a placeholder pixel format");
    ok &= check(liveSampler != liveResourceFixture.end() && liveSampler->kind == "Sampler" &&
                    liveSampler->format.empty() && liveSampler->allocationBytes == 0,
                "FX sampler snapshots do not claim a texture format or byte allocation");
    dayo::graphics::DeformerResourceRegistry deformerResources;
    deformerResources.publish(7, 101, exportedStore);
    auto exported = deformerResources.resolve(7, "OutBuf");
    ok &= check(exported.status == dayo::graphics::DeformerResourceRegistry::LookupStatus::unique &&
                    exported.value.has_value() && exported.value->resource.texture == exportedTexture.texture,
                "deformer registry publishes a non-owning physical resource handle");
    const auto initialGeneration = exported.value.has_value() ? exported.value->generation : 0;
    deformerResources.publish(7, 101, exportedStore);
    exported = deformerResources.resolve(7, "OutBuf");
    ok &= check(exported.value.has_value() && exported.value->generation == initialGeneration,
                "unchanged deformer exports preserve their runtime generation");

    dayo::graphics::FxResourceStore otherStore;
    exportedTexture.texture = {8, 1};
    ok &= check(otherStore.add(exportedTexture), "second deformer resource fixture enters physical store");
    deformerResources.publish(7, 102, otherStore);
    exported = deformerResources.resolve(7, "OutBuf");
    ok &= check(exported.status == dayo::graphics::DeformerResourceRegistry::LookupStatus::ambiguous &&
                    !exported.value.has_value(),
                "same-named resources from multiple deformers are reported as ambiguous");
    deformerResources.invalidateEffect(102);
    exported = deformerResources.resolve(7, "OutBuf");
    ok &= check(exported.status == dayo::graphics::DeformerResourceRegistry::LookupStatus::unique,
                "hot-reload invalidation removes only the selected effect exports");
    exportedTexture.texture = {9, 3};
    dayo::graphics::FxResourceStore reloadedStore;
    ok &= check(reloadedStore.add(exportedTexture), "reloaded deformer fixture enters physical store");
    deformerResources.publish(7, 101, reloadedStore);
    exported = deformerResources.resolve(7, "OutBuf");
    ok &= check(exported.value.has_value() && exported.value->generation > initialGeneration &&
                    exported.value->resource.texture == exportedTexture.texture,
                "changed physical handles receive a fresh resource generation");

    const auto makeTextureDeclaration = [](std::string name, std::string shared) {
        dayo::core::EffectTexture declaration;
        declaration.name = std::move(name);
        declaration.shared = std::move(shared);
        return declaration;
    };
    const auto makeBufferDeclaration = [](std::string name, std::string shared) {
        dayo::core::EffectBuffer declaration;
        declaration.name = std::move(name);
        declaration.shared = std::move(shared);
        return declaration;
    };
    dayo::fx::FxProgram sharedProgram;
    sharedProgram.textures.push_back(makeTextureDeclaration("SharedColor", "SOURCE"));
    sharedProgram.textures3D.push_back(makeTextureDeclaration("SharedVolume", "source"));
    sharedProgram.buffers.push_back(makeBufferDeclaration("SharedData", " source "));
    sharedProgram.textures.push_back(makeTextureDeclaration("ReferenceOnly", "REF"));
    dayo::graphics::FxResourceStore sharedStore;
    ok &= check(sharedStore.add({.name = "SharedColor",
                                 .kind = dayo::graphics::FxResourceStore::Kind::texture,
                                 .texture = {20, 1},
                                 .extent = {8, 4, 1},
                                 .format = dayo::graphics::PixelFormat::rgba16Float,
                                 .dimension = 2}),
                "2D shared source fixture enters physical store");
    ok &= check(sharedStore.add({.name = "SharedVolume",
                                 .kind = dayo::graphics::FxResourceStore::Kind::texture,
                                 .texture = {21, 2},
                                 .extent = {8, 4, 2},
                                 .format = dayo::graphics::PixelFormat::rgba16Float,
                                 .dimension = 3}),
                "3D shared source fixture enters physical store");
    ok &= check(sharedStore.add(
                    {.name = "SharedData", .kind = dayo::graphics::FxResourceStore::Kind::buffer, .buffer = {22, 3}}),
                "buffer shared source fixture enters physical store");
    ok &= check(sharedStore.add({.name = "ReferenceOnly",
                                 .kind = dayo::graphics::FxResourceStore::Kind::texture,
                                 .texture = {23, 4},
                                 .extent = {1, 1, 1},
                                 .dimension = 2}),
                "shared reference-only fixture enters physical store");
    dayo::graphics::FxSharedResourceRegistry sharedResources;
    sharedResources.publish("effect:source", sharedProgram, sharedStore);
    auto shared = sharedResources.resolve("SharedColor");
    ok &= check(shared.status == dayo::graphics::FxSharedResourceRegistry::LookupStatus::unique &&
                    shared.value.has_value() &&
                    shared.value->resource.texture == dayo::graphics::handles::TextureHandle{20, 1},
                "shared source registry publishes a non-owning 2D texture handle");
    const auto sharedGeneration = shared.value.has_value() ? shared.value->generation : 0;
    sharedResources.publish("effect:source", sharedProgram, sharedStore);
    shared = sharedResources.resolve("SharedColor");
    ok &= check(shared.value.has_value() && shared.value->generation == sharedGeneration,
                "unchanged shared source snapshots preserve their generation");
    auto replacedSharedStore = sharedStore;
    replacedSharedStore.find("SharedColor")->texture = {25, 5};
    sharedResources.publish("effect:source", sharedProgram, replacedSharedStore);
    shared = sharedResources.resolve("SharedColor");
    ok &= check(shared.value.has_value() && shared.value->generation > sharedGeneration &&
                    shared.value->resource.texture == dayo::graphics::handles::TextureHandle{25, 5},
                "replaced shared physical resources receive a fresh generation");
    const auto sharedVolume = sharedResources.resolve("SharedVolume");
    const auto sharedBuffer = sharedResources.resolve("SharedData");
    ok &= check(sharedVolume.value.has_value() && sharedVolume.value->resource.dimension == 3 &&
                    sharedBuffer.value.has_value() &&
                    sharedBuffer.value->resource.kind == dayo::graphics::FxResourceStore::Kind::buffer,
                "shared source registry retains 3D texture and buffer kinds");
    ok &= check(sharedResources.resolve("ReferenceOnly").status ==
                    dayo::graphics::FxSharedResourceRegistry::LookupStatus::notFound,
                "shared ref declarations are not incorrectly published as sources");
    auto wrongKindProgram = sharedProgram;
    wrongKindProgram.textures3D.clear();
    wrongKindProgram.buffers.clear();
    wrongKindProgram.textures.clear();
    wrongKindProgram.textures.push_back(makeTextureDeclaration("WrongKind", "source"));
    dayo::graphics::FxResourceStore wrongKindStore;
    ok &= check(wrongKindStore.add(
                    {.name = "WrongKind", .kind = dayo::graphics::FxResourceStore::Kind::buffer, .buffer = {24, 1}}),
                "wrong-kind shared resource fixture enters physical store");
    sharedResources.publish("effect:wrong-kind", wrongKindProgram, wrongKindStore);
    ok &= check(sharedResources.resolve("WrongKind").status ==
                    dayo::graphics::FxSharedResourceRegistry::LookupStatus::notFound,
                "a mismatched physical resource kind is not published");
    auto wrongDimensionProgram = sharedProgram;
    wrongDimensionProgram.textures.clear();
    wrongDimensionProgram.textures3D.clear();
    wrongDimensionProgram.buffers.clear();
    wrongDimensionProgram.textures3D.push_back(makeTextureDeclaration("WrongDimension", "source"));
    dayo::graphics::FxResourceStore wrongDimensionStore;
    ok &= check(wrongDimensionStore.add({.name = "WrongDimension",
                                         .kind = dayo::graphics::FxResourceStore::Kind::texture,
                                         .texture = {26, 1},
                                         .extent = {4, 4, 1},
                                         .dimension = 2}),
                "wrong-dimension shared resource fixture enters physical store");
    sharedResources.publish("effect:wrong-dimension", wrongDimensionProgram, wrongDimensionStore);
    ok &= check(sharedResources.resolve("WrongDimension").status ==
                    dayo::graphics::FxSharedResourceRegistry::LookupStatus::notFound,
                "a mismatched physical texture dimension is not published");
    auto duplicateStore = sharedStore;
    auto duplicateProgram = wrongKindProgram;
    duplicateProgram.textures.clear();
    duplicateProgram.textures.push_back(makeTextureDeclaration("SharedColor", "source"));
    sharedResources.publish("effect:duplicate", duplicateProgram, duplicateStore);
    shared = sharedResources.resolve("SharedColor");
    ok &= check(shared.status == dayo::graphics::FxSharedResourceRegistry::LookupStatus::ambiguous &&
                    !shared.value.has_value(),
                "duplicate global shared source names are reported as ambiguous");
    sharedResources.invalidateOwner("effect:duplicate");
    shared = sharedResources.resolve("SharedColor");
    ok &= check(shared.status == dayo::graphics::FxSharedResourceRegistry::LookupStatus::unique,
                "shared source invalidation removes only one runtime owner");
    sharedProgram.textures.clear();
    sharedProgram.textures3D.clear();
    sharedProgram.buffers.clear();
    sharedResources.publish("effect:source", sharedProgram, sharedStore);
    ok &= check(sharedResources.resolve("SharedColor").status ==
                    dayo::graphics::FxSharedResourceRegistry::LookupStatus::notFound,
                "publishing an empty shared-source snapshot invalidates stale exports");

    dayo::graphics::Rgba16fSampleAccumulator sampleAccumulator;
    sampleAccumulator.begin({1, 1, 1}, 2);
    const auto makeHalfPixel = [](std::array<float, 4> values) {
        std::array<std::uint8_t, 8> bytes{};
        for (std::size_t channel = 0; channel < values.size(); ++channel) {
            const auto half = dayo::core::floatToHalf(values[channel]);
            std::memcpy(bytes.data() + channel * sizeof(half), &half, sizeof(half));
        }
        return bytes;
    };
    const auto firstLinearSample = makeHalfPixel({2.0F, 0.25F, -2.0F, 1.0F});
    const auto secondLinearSample = makeHalfPixel({4.0F, 0.75F, 2.0F, 1.0F});
    sampleAccumulator.add(firstLinearSample);
    ok &= check(!sampleAccumulator.complete(), "linear sample accumulator waits for every sample");
    sampleAccumulator.add(secondLinearSample);
    const auto averagedHalfPixel = sampleAccumulator.resolve();
    std::array<float, 4> averagedValues{};
    for (std::size_t channel = 0; channel < averagedValues.size(); ++channel) {
        std::uint16_t half{};
        std::memcpy(&half, averagedHalfPixel.data() + channel * sizeof(half), sizeof(half));
        averagedValues[channel] = dayo::core::halfToFloat(half);
    }
    ok &= check(std::abs(averagedValues[0] - 3.0F) < 0.01F && std::abs(averagedValues[1] - 0.5F) < 0.01F &&
                    std::abs(averagedValues[2]) < 0.01F && std::abs(averagedValues[3] - 1.0F) < 0.01F,
                "RGBA16F samples average in linear float without clipping HDR values");

    dayo::graphics::NativeFxPendingEvents pendingEvents;
    pendingEvents.latch(true, false);
    pendingEvents.latch(false, true);
    ok &= check(pendingEvents.modelChanged && pendingEvents.materialChanged,
                "native FX change events remain latched across scene dirty-flag clearing");
    pendingEvents.clear();
    ok &= check(!pendingEvents.modelChanged && !pendingEvents.materialChanged,
                "native FX change events clear after the effect stack completes");

    const auto sceneContext = dayo::fx::makeFxFrameContext(12.0F, 3, 640, 480, 7, 0, 100, 2, 1, 1);
    const dayo::graphics::NativeEffectModel modelA{
        .modelId = 7, .modelIndex = 0, .vertexCount = 100, .materialCount = 2, .cloneCount = 1};
    const dayo::graphics::NativeEffectModel modelB{
        .modelId = 8, .modelIndex = 1, .vertexCount = 250, .materialCount = 5, .cloneCount = 2};
    const auto deformContext = dayo::graphics::makeDeformFxFrameContext(sceneContext, modelB, 4);
    ok &= check(sceneContext.currentModel == modelA.modelId && sceneContext.vertexCount == modelA.vertexCount,
                "scene frame context remains based on the selected model");
    ok &= check(deformContext.currentModel == modelB.modelId && deformContext.modelIndex == modelB.modelIndex &&
                    deformContext.vertexCount == 250 && deformContext.totalMaterial == 5 &&
                    deformContext.cloneCount == 4 && deformContext.clonedVertexCount == 1000,
                "deform frame context uses its owner and effect clone count");

    std::array<dayo::core::SceneEffectInstance, 1> deformEffects{};
    deformEffects[0].controllerModel = modelB.modelId;
    deformEffects[0].graph.meshCloneCount = 6;
    ok &= check(dayo::graphics::resolveNativeModelCloneCount(2, modelB.modelId, deformEffects) == 6,
                "native model clone count unifies scene count with its assigned deformer");
    ok &= check(dayo::graphics::resolveNativeModelCloneCount(8, modelB.modelId, deformEffects) == 8,
                "native model clone resolution preserves a larger scene count");
    ok &= check(dayo::graphics::resolveNativeModelCloneCount(0, 99, deformEffects) == 1,
                "native model clone resolution defaults unassigned models to one instance");
    auto secondDeformer = deformEffects.front();
    secondDeformer.graph.meshCloneCount = 4;
    const std::array multipleDeformers{deformEffects.front(), secondDeformer};
    bool rejectedMultipleDeformers = false;
    try {
        static_cast<void>(dayo::graphics::resolveNativeModelCloneCount(2, modelB.modelId, multipleDeformers));
    } catch (const std::logic_error&) {
        rejectedMultipleDeformers = true;
    }
    ok &= check(rejectedMultipleDeformers,
                "native model clone resolution rejects unsupported multiple-deformer chain semantics");

    dayo::fx::FxRequiredFeatures compute;
    const auto computeDecision =
        dayo::graphics::decideNativeRenderer(capableSubayai(), dayo::graphics::RendererKind::subayai, compute);
    ok &= check(computeDecision.active == dayo::graphics::RendererKind::subayai && !computeDecision.fellBack(),
                "compute Subayai graph is eligible on a native-capable device");

    auto rtCapabilities = capableSubayai();
    rtCapabilities.rayTracingPipeline = false;
    dayo::fx::FxRequiredFeatures rayTracing;
    rayTracing.accelerationStructure = true;
    rayTracing.rayQuery = true;
    rayTracing.rayTracingPipeline = true;
    const auto missing = dayo::graphics::missingEffectFeatures(rtCapabilities, rayTracing);
    ok &= check(missing.find("VK_KHR_ray_tracing_pipeline") != std::string::npos,
                "effect feature diagnostics include a missing RT pipeline");
    const auto rtDecision =
        dayo::graphics::decideNativeRenderer(rtCapabilities, dayo::graphics::RendererKind::subayai, rayTracing);
    ok &= check(rtDecision.active == dayo::graphics::RendererKind::preview && rtDecision.fellBack() &&
                    rtDecision.reason.find("VK_KHR_ray_tracing_pipeline") != std::string::npos,
                "Subayai falls back when its graph requires unavailable RT features");

    auto disabled = capableSubayai();
    disabled.nativeSubayai = false;
    const auto disabledDecision =
        dayo::graphics::decideNativeRenderer(disabled, dayo::graphics::RendererKind::subayai, compute);
    ok &= check(disabledDecision.active == dayo::graphics::RendererKind::preview &&
                    disabledDecision.reason.find("native Subayai pass implementation") != std::string::npos,
                "native implementation gate keeps Preview as the safe fallback");

    const auto initializationDecision =
        dayo::graphics::decideNativeRendererForInitialization(disabled, dayo::graphics::RendererKind::subayai, compute);
    ok &= check(initializationDecision.active == dayo::graphics::RendererKind::subayai &&
                    !initializationDecision.fellBack() && initializationDecision.reason.empty(),
                "runtime initialization is not blocked by its own not-yet-published implementation gate");

    const auto previewDecision =
        dayo::graphics::decideNativeRenderer(disabled, dayo::graphics::RendererKind::preview, rayTracing);
    ok &= check(previewDecision.active == dayo::graphics::RendererKind::preview && previewDecision.reason.empty(),
                "Preview selection does not inherit native effect requirements");

    return ok ? 0 : 1;
}
