#pragma once

#include "graphics/device.hpp"
#include <algorithm>
#include <limits>

namespace dayo::graphics {

struct PreviewDirtyRange {
    std::size_t begin{std::numeric_limits<std::size_t>::max()};
    std::size_t end{};
    void mark(std::size_t first, std::size_t last) noexcept {
        begin = std::min(begin, first);
        end = std::max(end, last);
    }
    [[nodiscard]] bool empty() const noexcept {
        return begin >= end;
    }
};

struct PreviewDeformedVertex {
    float position[3];
    float normal[3];
    float uv[2];
    float edgeScale;
};
static_assert(sizeof(PreviewVertex) == 108);
static_assert(offsetof(PreviewVertex, normal) == 12 && offsetof(PreviewVertex, bones) == 32);
static_assert(offsetof(PreviewVertex, sdefC) == 64 && offsetof(PreviewVertex, morphCount) == 104);
static_assert(sizeof(PreviewDeformedVertex) == 36);

// CPU-side ownership for the persistent preview scene. VulkanDevice keeps the
// actual Vulkan resources, while this object owns the payloads and their
// current logical sizes used to update those resources.
struct PreviewGpuScene {
    PreviewScene view;
    std::vector<PreviewVertex> vertices;
    std::vector<PreviewBoneTransform> bones;
    std::vector<PreviewMaterial> materials;
    std::vector<PreviewDraw> draws;
    std::vector<PreviewMorphDelta> morphDeltas;
    std::vector<float> morphWeights;
    std::vector<PreviewMaterialGpu> materialData;

    void clear() {
        view = {};
        vertices.clear();
        bones.clear();
        materials.clear();
        draws.clear();
        morphDeltas.clear();
        morphWeights.clear();
        materialData.clear();
    }
};

} // namespace dayo::graphics
