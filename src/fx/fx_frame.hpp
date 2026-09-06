#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace dayo::fx {

struct FxCameraState {
    std::array<float, 3> position{};
    std::array<float, 3> rotation{};
    float distance{3.0F};
    float verticalFovRadians{0.785398163F};
    bool perspective{true};
};

struct FxLightingState {
    std::array<float, 3> direction{-0.5F, -1.0F, 0.5F};
    std::array<float, 3> color{0.6F, 0.6F, 0.6F};
};

// Frame-boundary ABI shared by compiler/plan, executor, and preview path.
// Dayo:: scene values (camera/lighting/clones) are resolved by the caller
// (application or registry) into this POD before dispatch so the executor
// never reaches back into Scene mid-frame.
struct FxFrameContext {
    float frame{};
    std::uint64_t sample{};
    std::uint32_t renderWidth{};
    std::uint32_t renderHeight{};
    std::uint64_t currentModel{};
    std::uint32_t modelIndex{};
    std::size_t vertexCount{};
    std::size_t totalMaterial{};
    std::uint32_t cloneCount{1};
    std::size_t clonedVertexCount{};
    FxCameraState camera;
    FxLightingState lighting;
};

// Scene cloneCount and effect meshCloneCount unification. The renderer must
// draw every instance the scene requests and every clone the effect needs
// (e.g. clone_sample.fxdayo meshCloning.count=4), so the unified count is
// the maximum, clamped to the Scene range [1, 1024].
[[nodiscard]] constexpr std::uint32_t unifyMeshCloneCount(std::uint32_t sceneCloneCount,
                                                          std::uint32_t effectMeshCloneCount) noexcept {
    const auto scene = sceneCloneCount < 1U ? 1U : sceneCloneCount;
    const auto effect = effectMeshCloneCount < 1U ? 1U : effectMeshCloneCount;
    const auto unified = scene > effect ? scene : effect;
    return unified > 1024U ? 1024U : unified;
}

[[nodiscard]] constexpr std::size_t clonedVertexTotal(std::size_t vertexCount,
                                                      std::uint32_t unifiedCloneCount) noexcept {
    return vertexCount * (unifiedCloneCount < 1U ? 1U : unifiedCloneCount);
}

[[nodiscard]] inline FxFrameContext makeFxFrameContext(float frame, std::uint64_t sample, std::uint32_t renderWidth,
                                                       std::uint32_t renderHeight, std::uint64_t currentModel,
                                                       std::uint32_t modelIndex, std::size_t vertexCount,
                                                       std::size_t totalMaterial, std::uint32_t sceneCloneCount,
                                                       std::uint32_t effectMeshCloneCount, FxCameraState camera = {},
                                                       FxLightingState lighting = {}) {
    FxFrameContext context;
    context.frame = frame;
    context.sample = sample;
    context.renderWidth = renderWidth;
    context.renderHeight = renderHeight;
    context.currentModel = currentModel;
    context.modelIndex = modelIndex;
    context.vertexCount = vertexCount;
    context.totalMaterial = totalMaterial;
    context.cloneCount = unifyMeshCloneCount(sceneCloneCount, effectMeshCloneCount);
    context.clonedVertexCount = clonedVertexTotal(vertexCount, context.cloneCount);
    context.camera = camera;
    context.lighting = lighting;
    return context;
}

} // namespace dayo::fx
