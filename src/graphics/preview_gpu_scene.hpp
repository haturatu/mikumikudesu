#pragma once

#include "graphics/device.hpp"

namespace dayo::graphics {

// CPU-side ownership for the persistent preview scene. VulkanDevice keeps the
// actual Vulkan resources, while this object owns the payloads and their
// current logical sizes used to update those resources.
struct PreviewGpuScene {
    PreviewScene view;
    std::vector<PreviewMaterial> materials;
    std::vector<PreviewDraw> draws;
    std::vector<PreviewMorphDelta> morphDeltas;
    std::vector<float> morphWeights;
    std::vector<PreviewMaterialGpu> materialData;

    void clear() {
        view = {};
        materials.clear();
        draws.clear();
        morphDeltas.clear();
        morphWeights.clear();
        materialData.clear();
    }
};

} // namespace dayo::graphics
