#include "core/image_hdr.hpp"
#include "fx/fx_compiler.hpp"
#include "graphics/fx_raster_semantics.hpp"
#include "graphics/native_fx_pending_events.hpp"
#include "graphics/native_renderer.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>

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
