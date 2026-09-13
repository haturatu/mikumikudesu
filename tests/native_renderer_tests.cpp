#include "fx/fx_compiler.hpp"
#include "graphics/native_renderer.hpp"

#include <iostream>
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

    const auto previewDecision =
        dayo::graphics::decideNativeRenderer(disabled, dayo::graphics::RendererKind::preview, rayTracing);
    ok &= check(previewDecision.active == dayo::graphics::RendererKind::preview &&
                    previewDecision.reason.empty(),
                "Preview selection does not inherit native effect requirements");

    return ok ? 0 : 1;
}
