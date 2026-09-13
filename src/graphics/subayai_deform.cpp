#include "graphics/subayai_deform.hpp"

#include <limits>
#include <stdexcept>

namespace dayo::graphics {
namespace {

[[nodiscard]] std::size_t checkedMultiply(std::size_t left, std::size_t right, const char* label) {
    if (right != 0 && left > std::numeric_limits<std::size_t>::max() / right)
        throw std::overflow_error(std::string(label) + " size overflow");
    return left * right;
}

[[nodiscard]] std::size_t checkedCountBytes(std::uint32_t count, std::size_t elementSize, const char* label) {
    return checkedMultiply(static_cast<std::size_t>(count), elementSize, label);
}

} // namespace

NativeDeformPlan makeNativeDeformPlan(const NativeDeformInput& input) {
    if (input.vertexCount == 0)
        throw std::invalid_argument("native deform requires at least one vertex");
    if (input.indexCount != 0 && input.indexCount % 3 != 0)
        throw std::invalid_argument("native deform index count must describe triangles");

    NativeDeformPlan plan;
    plan.baseVertices = {checkedCountBytes(input.vertexCount, sizeof(PreviewVertex), "base vertex"),
                         ResourceUsage::storageRead, false, ResourceLifetime::persistent};
    plan.bones = {checkedCountBytes(input.boneCount, sizeof(PreviewBoneTransform), "bone"), ResourceUsage::storageRead,
                  false, ResourceLifetime::persistent};
    plan.morphDeltas = {checkedCountBytes(input.morphDeltaCount, sizeof(PreviewMorphDelta), "morph delta"),
                        ResourceUsage::storageRead, false, ResourceLifetime::persistent};
    plan.morphWeights = {checkedCountBytes(input.morphCount, sizeof(float), "morph weight"), ResourceUsage::storageRead,
                         false, ResourceLifetime::persistent};
    plan.deformedVertices = {checkedCountBytes(input.vertexCount, sizeof(NativeDeformedVertex), "deformed vertex"),
                             ResourceUsage::storageWrite | ResourceUsage::vertexRead | ResourceUsage::asBuildRead,
                             false, ResourceLifetime::transient};
    plan.indices = {checkedCountBytes(input.indexCount, sizeof(std::uint32_t), "index"),
                    ResourceUsage::indexRead | ResourceUsage::asBuildRead, false, ResourceLifetime::persistent};
    const auto workgroups = (input.vertexCount + plan.workgroupSize - 1U) / plan.workgroupSize;
    plan.workgroupCount = workgroups;
    return plan;
}

BlasGeometryDesc NativeDeformPlan::makeBlasGeometry(handles::BufferHandle deformedVertexBuffer,
                                                    handles::BufferHandle indexBuffer) const {
    if (!deformedVertexBuffer.valid() || !indexBuffer.valid())
        throw std::invalid_argument("native BLAS geometry requires deformed vertex and index buffers");
    const auto vertexCount = static_cast<std::uint32_t>(deformedVertices.size / sizeof(NativeDeformedVertex));
    const auto indexCount = static_cast<std::uint32_t>(indices.size / sizeof(std::uint32_t));
    return BlasGeometryDesc{.triangles = {{.vertexBuffer = deformedVertexBuffer,
                                           .vertexOffset = 0,
                                           .vertexFormat = VertexFormat::r32g32b32Sfloat,
                                           .vertexStride = sizeof(NativeDeformedVertex),
                                           .vertexCount = vertexCount,
                                           .indexBuffer = indexBuffer,
                                           .indexOffset = 0,
                                           .indexType = IndexType::uint32,
                                           .indexCount = indexCount,
                                           .opaque = true,
                                           .allowUpdate = true}}};
}

} // namespace dayo::graphics
