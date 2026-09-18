#include "graphics/subayai_deform.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

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

[[nodiscard]] std::uint32_t checkedSpanCount(std::size_t count, const char* label) {
    if (count > std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error(std::string(label) + " count exceeds the native deform ABI");
    return static_cast<std::uint32_t>(count);
}

[[nodiscard]] NativeDeformInput makeInput(const NativeDeformUpload& upload) {
    if (upload.baseVertices.empty())
        throw std::invalid_argument("native deform requires base vertices");
    if (upload.indices.empty())
        throw std::invalid_argument("native deform requires triangle indices");
    return NativeDeformInput{
        .vertexCount = checkedSpanCount(upload.baseVertices.size(), "base vertex"),
        .indexCount = checkedSpanCount(upload.indices.size(), "index"),
        .boneCount = checkedSpanCount(upload.bones.size(), "bone"),
        .morphDeltaCount = checkedSpanCount(upload.morphDeltas.size(), "morph delta"),
        .morphCount = checkedSpanCount(upload.morphWeights.size(), "morph weight"),
    };
}

[[nodiscard]] BufferResourceDesc uploadable(BufferResourceDesc desc, std::size_t minimumBytes) {
    desc.size = std::max(desc.size, minimumBytes);
    desc.usage |= ResourceUsage::transferDst;
    desc.cpuVisible = false;
    desc.lifetime = ResourceLifetime::persistent;
    return desc;
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
                             ResourceUsage::storageWrite | ResourceUsage::vertexRead | ResourceUsage::asBuildRead |
                                 ResourceUsage::transferDst,
                             false, ResourceLifetime::persistent};
    plan.indices = {checkedCountBytes(input.indexCount, sizeof(std::uint32_t), "index"),
                    ResourceUsage::indexRead | ResourceUsage::asBuildRead, false, ResourceLifetime::persistent};
    if (input.vertexCount > std::numeric_limits<std::uint32_t>::max() - (plan.workgroupSize - 1U))
        throw std::overflow_error("native deform workgroup count overflow");
    plan.workgroupCount = (input.vertexCount + plan.workgroupSize - 1U) / plan.workgroupSize;
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

DescriptorSetLayoutDesc nativeDeformDescriptorLayout() noexcept {
    return {.bindings = {{0, DescriptorKind::storageBuffer, 1, ShaderStageMask::compute},
                         {1, DescriptorKind::storageBuffer, 1, ShaderStageMask::compute},
                         {2, DescriptorKind::storageBuffer, 1, ShaderStageMask::compute},
                         {3, DescriptorKind::storageBuffer, 1, ShaderStageMask::compute},
                         {4, DescriptorKind::storageBuffer, 1, ShaderStageMask::compute}}};
}

PipelineLayoutDesc nativeDeformPipelineLayout(handles::DescriptorSetLayoutHandle descriptorLayout) noexcept {
    return {.setLayouts = {descriptorLayout},
            .pushConstants = {{ShaderStageMask::compute, 0, sizeof(NativeDeformPushConstants)}}};
}

NativeDeformRuntime::~NativeDeformRuntime() {
    reset();
}

bool NativeDeformRuntime::initialize(Device& device, const NativeDeformUpload& upload, handles::PipelineHandle pipeline,
                                     handles::DescriptorSetLayoutHandle descriptorLayout, std::string* error,
                                     std::uint64_t topologyGeneration) {
    if (error != nullptr)
        error->clear();
    reset();
    try {
        if (!pipeline.valid())
            throw std::invalid_argument("native deform requires a valid compute pipeline");
        if (!descriptorLayout.valid())
            throw std::invalid_argument("native deform requires a valid descriptor-set layout");
        const NativeDeformInput input = makeInput(upload);
        plan_ = makeNativeDeformPlan(input);
        device_ = &device;
        pipeline_ = pipeline;
        descriptorLayout_ = descriptorLayout;
        constants_ = {.vertexCount = input.vertexCount,
                      .boneCount = input.boneCount,
                      .morphDeltaCount = input.morphDeltaCount,
                      .morphCount = input.morphCount};
        workgroupCount_ = plan_.workgroupCount;

        if (!upload.deformedVertices.empty() && upload.deformedVertices.size() != input.vertexCount)
            throw std::invalid_argument("native deform seed vertex count does not match base vertices");
        std::vector<NativeDeformedVertex> initialDeformed;
        if (upload.deformedVertices.empty()) {
            // A valid finite seed lets the first BLAS build complete before
            // the first recorded deform dispatch. The command-list path
            // overwrites it with the animated result in the same frame.
            initialDeformed.resize(input.vertexCount);
            for (std::size_t index = 0; index < initialDeformed.size(); ++index) {
                std::copy(std::begin(upload.baseVertices[index].position),
                          std::end(upload.baseVertices[index].position), initialDeformed[index].position);
                initialDeformed[index].position[3] = 1.0F;
                std::copy(std::begin(upload.baseVertices[index].normal), std::end(upload.baseVertices[index].normal),
                          initialDeformed[index].normal);
                initialDeformed[index].normal[3] = 0.0F;
                std::copy(std::begin(upload.baseVertices[index].uv), std::end(upload.baseVertices[index].uv),
                          initialDeformed[index].uv);
            }
        }

        for (auto& resources : resources_) {
            resources.baseVertices = device.createBufferEx(uploadable(plan_.baseVertices, sizeof(PreviewVertex)));
            resources.bones = device.createBufferEx(uploadable(plan_.bones, sizeof(PreviewBoneTransform)));
            resources.morphDeltas = device.createBufferEx(uploadable(plan_.morphDeltas, sizeof(PreviewMorphDelta)));
            resources.morphWeights = device.createBufferEx(uploadable(plan_.morphWeights, sizeof(float)));
            resources.deformedVertices = device.createBufferEx(plan_.deformedVertices);
            resources.indices = device.createBufferEx(uploadable(plan_.indices, sizeof(std::uint32_t)));

            if (upload.deformedVertices.empty())
                device.uploadBufferEx(resources.deformedVertices, std::as_bytes(std::span(initialDeformed)), 0);
            else
                device.uploadBufferEx(resources.deformedVertices, std::as_bytes(upload.deformedVertices), 0);
            device.uploadBufferEx(resources.baseVertices, std::as_bytes(upload.baseVertices), 0);
            device.uploadBufferEx(resources.bones, std::as_bytes(upload.bones), 0);
            device.uploadBufferEx(resources.morphDeltas, std::as_bytes(upload.morphDeltas), 0);
            device.uploadBufferEx(resources.morphWeights, std::as_bytes(upload.morphWeights), 0);
            device.uploadBufferEx(resources.indices, std::as_bytes(upload.indices), 0);

            const std::array<DescriptorBindingEx, 5> bindings{
                DescriptorBindingEx{0, 0, resources.baseVertices},
                DescriptorBindingEx{1, 0, resources.bones},
                DescriptorBindingEx{2, 0, resources.morphDeltas},
                DescriptorBindingEx{3, 0, resources.morphWeights},
                DescriptorBindingEx{4, 0, resources.deformedVertices},
            };
            resources.descriptorSet = device.allocateDescriptorSetEx(descriptorLayout, bindings);
            if (!resources.descriptorSet.valid())
                throw std::runtime_error("native deform descriptor-set allocation returned an invalid handle");
        }

        topologyGeneration_ = topologyGeneration;
        uploadedTopologyGenerations_.fill(topologyGeneration);
    } catch (const std::exception& exception) {
        if (error != nullptr)
            *error = exception.what();
        reset();
        return false;
    } catch (...) {
        if (error != nullptr)
            *error = "native deform initialization failed";
        reset();
        return false;
    }
    return true;
}

bool NativeDeformRuntime::update(Device& device, const NativeDeformUpload& upload, std::string* error) {
    if (error != nullptr)
        error->clear();
    try {
        if (!ready() || device_ != &device)
            throw std::logic_error("native deform runtime is not initialized for this device");
        const NativeDeformInput input = makeInput(upload);
        const bool shapeChanged =
            constants_.vertexCount != input.vertexCount || constants_.boneCount != input.boneCount ||
            constants_.morphDeltaCount != input.morphDeltaCount || constants_.morphCount != input.morphCount ||
            plan_.indices.size != checkedCountBytes(input.indexCount, sizeof(std::uint32_t), "index");
        if (shapeChanged)
            return initialize(device, upload, pipeline_, descriptorLayout_, error);

        if (!upload.deformedVertices.empty() && upload.deformedVertices.size() != input.vertexCount)
            throw std::invalid_argument("native deform seed vertex count does not match base vertices");
        for (auto& resources : resources_) {
            device.uploadBufferEx(resources.baseVertices, std::as_bytes(upload.baseVertices), 0);
            device.uploadBufferEx(resources.bones, std::as_bytes(upload.bones), 0);
            device.uploadBufferEx(resources.morphDeltas, std::as_bytes(upload.morphDeltas), 0);
            device.uploadBufferEx(resources.morphWeights, std::as_bytes(upload.morphWeights), 0);
            device.uploadBufferEx(resources.indices, std::as_bytes(upload.indices), 0);
            if (!upload.deformedVertices.empty())
                device.uploadBufferEx(resources.deformedVertices, std::as_bytes(upload.deformedVertices), 0);
        }
        uploadedTopologyGenerations_.fill(topologyGeneration_);
    } catch (const std::exception& exception) {
        if (error != nullptr)
            *error = exception.what();
        return false;
    } catch (...) {
        if (error != nullptr)
            *error = "native deform update failed";
        return false;
    }
    return true;
}

bool NativeDeformRuntime::prepare(Device& device, const NativeDeformUpload& upload, std::string* error,
                                  std::uint64_t topologyGeneration) {
    if (error != nullptr)
        error->clear();
    try {
        if (!ready() || device_ != &device)
            throw std::logic_error("native deform runtime is not initialized for this device");
        const NativeDeformInput input = makeInput(upload);
        const bool shapeChanged =
            constants_.vertexCount != input.vertexCount || constants_.boneCount != input.boneCount ||
            constants_.morphDeltaCount != input.morphDeltaCount || constants_.morphCount != input.morphCount ||
            plan_.indices.size != checkedCountBytes(input.indexCount, sizeof(std::uint32_t), "index");
        if (shapeChanged)
            return initialize(device, upload, pipeline_, descriptorLayout_, error, topologyGeneration);
        if (!upload.deformedVertices.empty() && upload.deformedVertices.size() != input.vertexCount)
            throw std::invalid_argument("native deform seed vertex count does not match base vertices");
        topologyGeneration_ = topologyGeneration;
    } catch (const std::exception& exception) {
        if (error != nullptr)
            *error = exception.what();
        return false;
    } catch (...) {
        if (error != nullptr)
            *error = "native deform frame preparation failed";
        return false;
    }
    return true;
}

void NativeDeformRuntime::reset() noexcept {
    Device* device = device_;
    if (device != nullptr) {
        try {
            device->waitIdle();
        } catch (...) {
        }
        for (auto& resources : resources_) {
            if (resources.descriptorSet.valid()) {
                try {
                    device->destroyDescriptorSetEx(resources.descriptorSet);
                } catch (...) {
                }
            }
            const std::array<handles::BufferHandle, 6> buffers{resources.baseVertices,     resources.bones,
                                                               resources.morphDeltas,      resources.morphWeights,
                                                               resources.deformedVertices, resources.indices};
            for (const auto buffer : buffers) {
                if (!buffer.valid())
                    continue;
                try {
                    device->destroyBufferEx(buffer);
                } catch (...) {
                }
            }
        }
    }
    device_ = nullptr;
    plan_ = {};
    resources_.fill({});
    constants_ = {};
    pipeline_ = {};
    descriptorLayout_ = {};
    workgroupCount_ = 0;
    topologyGeneration_ = 0;
    uploadedTopologyGenerations_.fill(0);
}

BlasGeometryDesc NativeDeformRuntime::blasGeometry() const {
    if (!ready())
        throw std::logic_error("native deform runtime is not initialized");
    const auto& resources = resources_[currentSlot()];
    return plan_.makeBlasGeometry(resources.deformedVertices, resources.indices);
}

void NativeDeformRuntime::record(CommandList& commands) const {
    if (!ready())
        throw std::logic_error("native deform runtime is not initialized");
    const auto& resources = resources_[currentSlot()];
    commands.bindPipelineEx(pipeline_);
    commands.bindDescriptorSetEx(resources.descriptorSet);
    commands.pushConstantsEx(std::as_bytes(std::span<const NativeDeformPushConstants>(&constants_, 1)));
    commands.dispatch(workgroupCount_, 1, 1);
}

void NativeDeformRuntime::record(CommandList& commands, const NativeDeformUpload& upload,
                                 std::uint64_t topologyGeneration) {
    if (!ready())
        throw std::logic_error("native deform runtime is not initialized");
    const NativeDeformInput input = makeInput(upload);
    if (input.vertexCount != constants_.vertexCount || input.boneCount != constants_.boneCount ||
        input.morphDeltaCount != constants_.morphDeltaCount || input.morphCount != constants_.morphCount ||
        plan_.indices.size != checkedCountBytes(input.indexCount, sizeof(std::uint32_t), "index"))
        throw std::invalid_argument("native deform frame input shape changed after preparation");

    const auto slot = currentSlot();
    auto& resources = resources_[slot];
    if (uploadedTopologyGenerations_[slot] != topologyGeneration) {
        commands.uploadBufferEx(resources.baseVertices, std::as_bytes(upload.baseVertices), 0);
        commands.uploadBufferEx(resources.morphDeltas, std::as_bytes(upload.morphDeltas), 0);
        commands.uploadBufferEx(resources.indices, std::as_bytes(upload.indices), 0);
        uploadedTopologyGenerations_[slot] = topologyGeneration;
    }
    if (!upload.bones.empty())
        commands.uploadBufferEx(resources.bones, std::as_bytes(upload.bones), 0);
    if (!upload.morphWeights.empty())
        commands.uploadBufferEx(resources.morphWeights, std::as_bytes(upload.morphWeights), 0);

    commands.bindPipelineEx(pipeline_);
    commands.bindDescriptorSetEx(resources.descriptorSet);
    commands.pushConstantsEx(std::as_bytes(std::span<const NativeDeformPushConstants>(&constants_, 1)));
    commands.dispatch(workgroupCount_, 1, 1);
}

} // namespace dayo::graphics
