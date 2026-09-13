#pragma once

#include "graphics/device.hpp"

#include <cstddef>
#include <cstdint>

namespace dayo::graphics {

// Native ray-query/RT shaders consume positions after PMX morphing and
// skinning. PreviewVertex is intentionally not used as the AS vertex format:
// it contains source skinning attributes which are evaluated by the Preview
// vertex shader rather than actual deformed positions.
struct alignas(16) NativeDeformedVertex {
    float position[4]{};
    float normal[4]{};
    float uv[2]{};
    float padding[2]{};
};
static_assert(sizeof(NativeDeformedVertex) == 48);
static_assert(alignof(NativeDeformedVertex) == 16);

struct NativeDeformInput {
    std::uint32_t vertexCount{};
    std::uint32_t indexCount{};
    std::uint32_t boneCount{};
    std::uint32_t morphDeltaCount{};
    std::uint32_t morphCount{};
};

struct NativeDeformPlan {
    BufferResourceDesc baseVertices;
    BufferResourceDesc bones;
    BufferResourceDesc morphDeltas;
    BufferResourceDesc morphWeights;
    BufferResourceDesc deformedVertices;
    BufferResourceDesc indices;
    std::uint32_t workgroupSize{64};
    std::uint32_t workgroupCount{};

    [[nodiscard]] BlasGeometryDesc makeBlasGeometry(handles::BufferHandle deformedVertexBuffer,
                                                    handles::BufferHandle indexBuffer) const;
};

// Describes the compute input/output contract shared by a native deform pass
// and the BLAS builder. The plan is backend-neutral; Vulkan records the actual
// dispatch in a later command-list layer.
[[nodiscard]] NativeDeformPlan makeNativeDeformPlan(const NativeDeformInput& input);

} // namespace dayo::graphics
