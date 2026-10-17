#include "graphics/skirt_collision.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace dayo::graphics {
DescriptorSetLayoutDesc skirtCollisionDescriptorLayout() {
    DescriptorSetLayoutDesc desc;
    for (std::uint32_t i = 0; i < 11; ++i)
        desc.bindings.push_back({i, DescriptorKind::storageBuffer, 1, ShaderStageMask::compute});
    return desc;
}
PipelineLayoutDesc skirtCollisionPipelineLayout(handles::DescriptorSetLayoutHandle layout) {
    return {.setLayouts = {layout},
            .pushConstants = {{ShaderStageMask::compute, 0, sizeof(SkirtCollisionPushConstants)}}};
}
SkirtCollisionRuntime::~SkirtCollisionRuntime() {
    reset();
}
void SkirtCollisionRuntime::reset() noexcept {
    if (device_ != nullptr) {
        try {
            device_->waitIdle();
        } catch (...) {
        }
        for (auto& slot : slots_) {
            if (slot.set.valid())
                try {
                    device_->destroyDescriptorSetEx(slot.set);
                } catch (...) {
                }
            for (auto buffer : slot.buffers)
                if (buffer.valid())
                    try {
                        device_->destroyBufferEx(buffer);
                    } catch (...) {
                    }
        }
        if (history_.valid())
            try {
                device_->destroyBufferEx(history_);
            } catch (...) {
            }
        if (capsuleHistory_.valid())
            try {
                device_->destroyBufferEx(capsuleHistory_);
            } catch (...) {
            }
    }
    device_ = nullptr;
    slots_ = {};
    history_ = {};
    capsuleHistory_ = {};
    shape_ = {};
    historyGeneration_ = 0;
    historyInitialized_ = false;
}
void SkirtCollisionRuntime::record(Device& device, CommandList& commands, handles::BufferHandle output,
                                   const SkirtCollisionUpload& upload, std::uint32_t vertexStride,
                                   std::uint32_t normalOffset) {
    if (upload.vertices.empty() || upload.capsules.empty())
        return;
    if (!output.valid() || (vertexStride != 36 && vertexStride != 48) || normalOffset + 12 > vertexStride ||
        upload.vertices.size() > std::numeric_limits<std::uint32_t>::max() || upload.iterations > 64 ||
        (!upload.legRanges.empty() && upload.legRanges.size() != upload.capsules.size()))
        throw std::invalid_argument("invalid skirt collision upload");
    for (const auto range : upload.legRanges)
        if (range[0] > upload.legVertices.size() || range[1] > upload.legVertices.size() - range[0])
            throw std::invalid_argument("invalid skirt collision leg range");
    const std::array<std::size_t, 6> shape{upload.vertices.size(), upload.triangles.size(), upload.adjacency.size(),
                                           upload.capsules.size(), upload.legRanges.size(), upload.legVertices.size()};
    if (device_ != &device || shape_ != shape) {
        reset();
        device_ = &device;
        shape_ = shape;
        const auto create = [&](std::size_t size, bool host) {
            return device.createBufferEx(
                {.size = std::max<std::size_t>(size, 16),
                 .usage = ResourceUsage::storageRead | ResourceUsage::storageWrite | ResourceUsage::transferSrc,
                 .cpuVisible = host});
        };
        try {
            history_ = create(shape[0] * sizeof(std::array<float, 4>), false);
            capsuleHistory_ = create(shape[3] * sizeof(std::array<float, 4>) * 2, false);
            for (auto& slot : slots_) {
                slot.buffers[0] = create(upload.vertices.size_bytes(), true);
                slot.buffers[1] = create(upload.triangles.size_bytes(), true);
                slot.buffers[2] = create(upload.adjacency.size_bytes(), true);
                slot.buffers[3] = create(upload.capsules.size_bytes(), true);
                slot.buffers[4] = create(shape[0] * sizeof(std::array<float, 4>) * 3, false);
                slot.buffers[5] = create(shape[0] * sizeof(SkirtCollisionGpuStats), true);
                slot.buffers[6] = create(upload.legRanges.size_bytes(), true);
                slot.buffers[7] = create(upload.legVertices.size_bytes(), true);
            }
        } catch (...) {
            reset();
            throw;
        }
    }
    lastSlot_ = device.currentFrameSlot() % kNativeFramesInFlight;
    auto& slot = slots_[lastSlot_];
    device.uploadBufferEx(slot.buffers[0], std::as_bytes(upload.vertices), 0);
    device.uploadBufferEx(slot.buffers[1], std::as_bytes(upload.triangles), 0);
    device.uploadBufferEx(slot.buffers[2], std::as_bytes(upload.adjacency), 0);
    device.uploadBufferEx(slot.buffers[3], std::as_bytes(upload.capsules), 0);
    if (!upload.legRanges.empty())
        device.uploadBufferEx(slot.buffers[6], std::as_bytes(upload.legRanges), 0);
    if (!upload.legVertices.empty())
        device.uploadBufferEx(slot.buffers[7], std::as_bytes(upload.legVertices), 0);
    if (slot.output != output) {
        if (slot.set.valid())
            device.destroyDescriptorSetEx(slot.set);
        const std::array<DescriptorBindingEx, 11> bindings{{{.slot = 0, .buffer = output, .mipLevel = {}},
                                                            {.slot = 1, .buffer = slot.buffers[0], .mipLevel = {}},
                                                            {.slot = 2, .buffer = slot.buffers[1], .mipLevel = {}},
                                                            {.slot = 3, .buffer = slot.buffers[2], .mipLevel = {}},
                                                            {.slot = 4, .buffer = slot.buffers[3], .mipLevel = {}},
                                                            {.slot = 5, .buffer = slot.buffers[4], .mipLevel = {}},
                                                            {.slot = 6, .buffer = history_, .mipLevel = {}},
                                                            {.slot = 7, .buffer = slot.buffers[5], .mipLevel = {}},
                                                            {.slot = 8, .buffer = slot.buffers[6], .mipLevel = {}},
                                                            {.slot = 9, .buffer = slot.buffers[7], .mipLevel = {}},
                                                            {.slot = 10, .buffer = capsuleHistory_, .mipLevel = {}}}};
        slot.set = device.allocateDescriptorSetEx(device.skirtCollisionDescriptorLayout(), bindings);
        slot.output = output;
    }
    SkirtCollisionPushConstants constants{
        .vertexCount = static_cast<std::uint32_t>(shape[0]),
        .vertexStride = vertexStride,
        .normalOffset = normalOffset,
        .resetHistory = !historyInitialized_ || historyGeneration_ != upload.historyGeneration ? 1U : 0U,
        .padding = upload.iterations == 0 ? 1U : 0U};
    // Covers skinning -> collision and the preceding submission's history writes.
    commands.memoryBarrierEx();
    commands.bindPipelineEx(device.skirtCollisionPipeline());
    commands.bindDescriptorSetEx(slot.set);
    if (!upload.legRanges.empty()) {
        constants.phase = 4;
        commands.pushConstantsEx(std::as_bytes(std::span(&constants, 1)));
        commands.dispatch(static_cast<std::uint32_t>(upload.legRanges.size()), 1, 1);
        commands.memoryBarrierEx();
        constants.phase = 0;
    }
    const auto dispatch = [&] {
        commands.pushConstantsEx(std::as_bytes(std::span(&constants, 1)));
        commands.dispatch((constants.vertexCount + 63) / 64, 1, 1);
        commands.memoryBarrierEx();
    };
    dispatch();
    for (std::uint32_t i = 0; i < upload.iterations; ++i) {
        constants.phase = i < std::min(6U, upload.iterations / 2) ? 1U : 3U;
        dispatch();
        std::swap(constants.sourceSlice, constants.destinationSlice);
    }
    constants.phase = 2;
    dispatch();
    historyGeneration_ = upload.historyGeneration;
    historyInitialized_ = true;
}
core::SkirtCollisionStats SkirtCollisionRuntime::readStats() const {
    core::SkirtCollisionStats result;
    if (device_ == nullptr)
        return result;
    const auto bytes =
        device_->readbackBufferEx(slots_[lastSlot_].buffers[5], 0, shape_[0] * sizeof(SkirtCollisionGpuStats));
    for (std::size_t offset = 0; offset + sizeof(SkirtCollisionGpuStats) <= bytes.size();
         offset += sizeof(SkirtCollisionGpuStats)) {
        SkirtCollisionGpuStats stats;
        std::memcpy(&stats, bytes.data() + offset, sizeof(stats));
        result.penetratingVertexCount += stats.penetratingVertexCount;
        result.penetratingTriangleCount += stats.penetratingTriangleCount;
        result.maxMeshPenetration = std::max(result.maxMeshPenetration, stats.maxPenetration);
        result.totalCorrectionDistance += stats.correctionDistance;
    }
    return result;
}
std::vector<core::SkirtCapsule> SkirtCollisionRuntime::readCapsules() const {
    if (device_ == nullptr)
        return {};
    const auto bytes =
        device_->readbackBufferEx(slots_[lastSlot_].buffers[3], 0, shape_[3] * sizeof(core::SkirtCapsule));
    std::vector<core::SkirtCapsule> result(shape_[3]);
    std::memcpy(result.data(), bytes.data(), bytes.size());
    return result;
}
} // namespace dayo::graphics
