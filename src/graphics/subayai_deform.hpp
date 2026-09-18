#pragma once

#include "graphics/device.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

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

struct NativeDeformUpload {
    std::span<const PreviewVertex> baseVertices;
    std::span<const PreviewBoneTransform> bones;
    std::span<const PreviewMorphDelta> morphDeltas;
    std::span<const float> morphWeights;
    std::span<const std::uint32_t> indices;
    // Optional host-evaluated positions used to seed the first BLAS build.
    // The GPU deform pass replaces these values once it is recorded.
    std::span<const NativeDeformedVertex> deformedVertices;
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

struct NativeDeformPushConstants {
    std::uint32_t vertexCount{};
    std::uint32_t boneCount{};
    std::uint32_t morphDeltaCount{};
    std::uint32_t morphCount{};
};
static_assert(sizeof(NativeDeformPushConstants) == 16);

struct NativeDeformResources {
    handles::BufferHandle baseVertices{};
    handles::BufferHandle bones{};
    handles::BufferHandle morphDeltas{};
    handles::BufferHandle morphWeights{};
    handles::BufferHandle deformedVertices{};
    handles::BufferHandle indices{};
    handles::DescriptorSetHandle descriptorSet{};

    [[nodiscard]] bool valid() const noexcept {
        return baseVertices.valid() && bones.valid() && morphDeltas.valid() && morphWeights.valid() &&
               deformedVertices.valid() && indices.valid() && descriptorSet.valid();
    }
};

// Binding 0..4 is the stable native deform shader ABI. Index data is kept in
// the resource set for the following BLAS build, but is not read by the
// vertex deformation compute shader.
[[nodiscard]] DescriptorSetLayoutDesc nativeDeformDescriptorLayout() noexcept;
[[nodiscard]] PipelineLayoutDesc
nativeDeformPipelineLayout(handles::DescriptorSetLayoutHandle descriptorLayout) noexcept;

// Owns the buffers and descriptor set for one native deform pass. The
// pipeline and descriptor-set layout are renderer-owned because they are
// created from the FX graph and its pipeline-layout resolver.
class NativeDeformRuntime {
  public:
    NativeDeformRuntime() = default;
    ~NativeDeformRuntime();

    NativeDeformRuntime(const NativeDeformRuntime&) = delete;
    NativeDeformRuntime& operator=(const NativeDeformRuntime&) = delete;

    [[nodiscard]] bool initialize(Device& device, const NativeDeformUpload& upload, handles::PipelineHandle pipeline,
                                  handles::DescriptorSetLayoutHandle descriptorLayout, std::string* error = nullptr,
                                  std::uint64_t topologyGeneration = 0);
    // Refreshes the CPU-visible deform inputs without replacing resources when
    // the mesh shape is unchanged. A shape change recreates the resource set
    // so descriptor bindings and BLAS geometry remain valid.
    [[nodiscard]] bool update(Device& device, const NativeDeformUpload& upload, std::string* error = nullptr);
    // Validates the next frame's shape without performing a device-local
    // upload. A shape change is an infrequent resource rebuild; unchanged
    // inputs are copied into the active frame command list by record(). The
    // topology generation identifies changes to static geometry data.
    [[nodiscard]] bool prepare(Device& device, const NativeDeformUpload& upload, std::string* error = nullptr,
                               std::uint64_t topologyGeneration = 0);
    void reset() noexcept;

    [[nodiscard]] bool ready() const noexcept {
        return device_ != nullptr &&
               std::all_of(resources_.begin(), resources_.end(),
                           [](const auto& resources) { return resources.valid(); }) &&
               pipeline_.valid() && workgroupCount_ != 0;
    }
    [[nodiscard]] const NativeDeformPlan& plan() const noexcept {
        return plan_;
    }
    [[nodiscard]] const NativeDeformResources& resources() const noexcept {
        return resources_[currentSlot()];
    }
    [[nodiscard]] handles::PipelineHandle pipeline() const noexcept {
        return pipeline_;
    }
    [[nodiscard]] BlasGeometryDesc blasGeometry() const;

    // Records: typed pipeline bind -> descriptor bind -> deform constants ->
    // compute dispatch. The caller must submit/wait this command list before
    // passing blasGeometry() to the acceleration-structure service.
    void record(CommandList& commands) const;
    // Records the current frame's dynamic inputs and the deform dispatch. The
    // upload commands use the active frame staging ring and do not submit or
    // wait on a transfer-only queue.
    void record(CommandList& commands, const NativeDeformUpload& upload, std::uint64_t topologyGeneration = 0);

  private:
    [[nodiscard]] std::size_t currentSlot() const noexcept {
        return device_ == nullptr ? 0 : device_->currentFrameSlot() % kNativeFramesInFlight;
    }

    Device* device_{nullptr};
    NativeDeformPlan plan_;
    std::array<NativeDeformResources, kNativeFramesInFlight> resources_{};
    NativeDeformPushConstants constants_;
    handles::PipelineHandle pipeline_{};
    handles::DescriptorSetLayoutHandle descriptorLayout_{};
    std::uint32_t workgroupCount_{};
    std::uint64_t topologyGeneration_{};
    std::array<std::uint64_t, kNativeFramesInFlight> uploadedTopologyGenerations_{};
};

// Describes the compute input/output contract shared by a native deform pass
// and the BLAS builder. The plan is backend-neutral; Vulkan records the actual
// dispatch in a later command-list layer.
[[nodiscard]] NativeDeformPlan makeNativeDeformPlan(const NativeDeformInput& input);

} // namespace dayo::graphics
