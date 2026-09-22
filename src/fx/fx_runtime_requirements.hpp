#pragma once

#include "fx/fx_compiler.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace dayo::fx {

enum class FxRuntimeFeature : std::uint8_t {
    textures3D,
    externalDds,
    persistentResources,
    sharedResources,
    matDesc,
    bufferRaster,
    stencil,
    logicOp,
    alphaToCoverage,
    vertexLayout,
    colorWriteMask,
    meshCloning,
    deformHostAbi,
    rayTracing,
    oidn,
    conditions,
    globalVariables,
    fullSamplerState,
    functionalCopy,
    functionalClearRtv,
    functionalClearUav,
    functionalMipmap,
};

struct FxRuntimeRequirements {
    std::vector<FxRuntimeFeature> features;

    [[nodiscard]] bool contains(FxRuntimeFeature feature) const noexcept;
    void add(FxRuntimeFeature feature);
};

[[nodiscard]] const char* toString(FxRuntimeFeature feature) noexcept;
[[nodiscard]] FxRuntimeRequirements analyzeRuntimeRequirements(const FxProgram& program);
[[nodiscard]] std::vector<FxRuntimeFeature> missingRuntimeFeatures(const FxRuntimeRequirements& required,
                                                                    const FxRuntimeRequirements& implemented);
[[nodiscard]] std::string formatRuntimeFeatureList(const FxRuntimeRequirements& requirements);

} // namespace dayo::fx
