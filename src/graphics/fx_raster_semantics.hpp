#pragma once

#include "core/fx/fx_pass.hpp"
#include "fx/fx_frame.hpp"
#include "graphics/native_scene_data.hpp"

#include <cstdint>
#include <optional>

namespace dayo::graphics {

struct NativeEffectModel {
    std::uint64_t modelId{};
    std::uint32_t modelIndex{};
    std::size_t vertexCount{};
    std::size_t materialCount{};
    std::uint32_t cloneCount{1};
    std::uint32_t rasterizeOrder{};
    std::uint32_t deformIndex{};
    std::uint32_t deformOrder{};
};

[[nodiscard]] inline fx::FxFrameContext makeDeformFxFrameContext(const fx::FxFrameContext& sceneContext,
                                                                 const NativeEffectModel& owner,
                                                                 std::uint32_t effectMeshCloneCount) noexcept {
    auto context = sceneContext;
    context.currentModel = owner.modelId;
    context.modelIndex = owner.modelIndex;
    context.vertexCount = owner.vertexCount;
    context.totalMaterial = owner.materialCount;
    context.cloneCount = fx::unifyMeshCloneCount(owner.cloneCount, effectMeshCloneCount);
    context.clonedVertexCount = fx::clonedVertexTotal(context.vertexCount, context.cloneCount);
    return context;
}

// Applies the upstream rasterModelTarget contract to one material draw range.
// self/other are intentionally false when no controller model is supplied:
// silently drawing every model would make a missing controller look valid.
[[nodiscard]] inline bool matchesRasterTarget(core::fx::RasterModelTarget target,
                                              std::optional<std::uint32_t> controllerModel,
                                              const NativeSceneDraw& draw) noexcept {
    switch (target) {
    case core::fx::RasterModelTarget::all:
        return true;
    case core::fx::RasterModelTarget::self:
        return controllerModel.has_value() && draw.modelIndex == *controllerModel;
    case core::fx::RasterModelTarget::other:
        return controllerModel.has_value() && draw.modelIndex != *controllerModel;
    case core::fx::RasterModelTarget::buffer:
        return draw.buffer;
    }
    return false;
}

} // namespace dayo::graphics
