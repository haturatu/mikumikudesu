#pragma once

#include "graphics/handles.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dayo::graphics {

enum class TextureDimension : std::uint8_t { d1, d2, d3, cube };

enum class PixelFormat : std::uint8_t { rgba8Unorm, rgba8Srgb, rgba16Float, rgba32Float, depth32Float };

enum class ResourceLifetime : std::uint8_t { transient, persistent };

// Typed image/buffer usage. Each enumerator occupies one bit so descriptors
// can express combined access (e.g. sampled + storage) as a bitmask.
enum class ResourceUsage : std::uint32_t {
    none = 0,
    sampledRead = 1U << 0U,
    storageRead = 1U << 1U,
    storageWrite = 1U << 2U,
    storageReadWrite = 1U << 3U,
    uniformRead = 1U << 4U,
    vertexRead = 1U << 5U,
    indexRead = 1U << 6U,
    indirectRead = 1U << 7U,
    colorAttachment = 1U << 8U,
    depthRead = 1U << 9U,
    depthWrite = 1U << 10U,
    transferSrc = 1U << 11U,
    transferDst = 1U << 12U,
    asBuildRead = 1U << 13U,
    asBuildWrite = 1U << 14U,
    rayTracingRead = 1U << 15U,
    hostRead = 1U << 16U,
    externalRead = 1U << 17U,
    externalWrite = 1U << 18U,
    present = 1U << 19U,
};

[[nodiscard]] constexpr std::uint32_t toBits(ResourceUsage usage) noexcept {
    return static_cast<std::uint32_t>(usage);
}

constexpr ResourceUsage operator|(ResourceUsage left, ResourceUsage right) noexcept {
    return static_cast<ResourceUsage>(toBits(left) | toBits(right));
}
constexpr ResourceUsage operator&(ResourceUsage left, ResourceUsage right) noexcept {
    return static_cast<ResourceUsage>(toBits(left) & toBits(right));
}
constexpr ResourceUsage operator~(ResourceUsage usage) noexcept {
    return static_cast<ResourceUsage>(~toBits(usage));
}
constexpr ResourceUsage& operator|=(ResourceUsage& left, ResourceUsage right) noexcept {
    left = left | right;
    return left;
}
constexpr ResourceUsage& operator&=(ResourceUsage& left, ResourceUsage right) noexcept {
    left = left & right;
    return left;
}

struct Extent3D {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t depth{1};
};

struct TextureResourceDesc {
    TextureDimension dimension{TextureDimension::d2};
    Extent3D extent{};
    PixelFormat format{PixelFormat::rgba8Unorm};
    std::uint32_t mipLevels{1};
    std::uint32_t arrayLayers{1};
    ResourceUsage usage{ResourceUsage::sampledRead};
    ResourceLifetime lifetime{ResourceLifetime::transient};
};

struct BufferResourceDesc {
    std::size_t size{};
    ResourceUsage usage{ResourceUsage::uniformRead};
    bool cpuVisible{};
    ResourceLifetime lifetime{ResourceLifetime::transient};
};

[[nodiscard]] constexpr bool isWriteUsage(ResourceUsage usage) noexcept {
    constexpr std::uint32_t kWriteMask = toBits(ResourceUsage::storageWrite) | toBits(ResourceUsage::storageReadWrite) |
                                         toBits(ResourceUsage::colorAttachment) | toBits(ResourceUsage::depthWrite) |
                                         toBits(ResourceUsage::transferDst) | toBits(ResourceUsage::asBuildWrite) |
                                         toBits(ResourceUsage::externalWrite) | toBits(ResourceUsage::present);
    return (toBits(usage) & kWriteMask) != 0;
}

[[nodiscard]] constexpr bool isReadUsage(ResourceUsage usage) noexcept {
    constexpr std::uint32_t kReadMask =
        toBits(ResourceUsage::sampledRead) | toBits(ResourceUsage::storageRead) |
        toBits(ResourceUsage::storageReadWrite) | toBits(ResourceUsage::uniformRead) |
        toBits(ResourceUsage::vertexRead) | toBits(ResourceUsage::indexRead) | toBits(ResourceUsage::indirectRead) |
        toBits(ResourceUsage::depthRead) | toBits(ResourceUsage::transferSrc) | toBits(ResourceUsage::asBuildRead) |
        toBits(ResourceUsage::rayTracingRead) | toBits(ResourceUsage::hostRead) | toBits(ResourceUsage::externalRead);
    return (toBits(usage) & kReadMask) != 0;
}

[[nodiscard]] inline std::string_view toString(ResourceUsage usage) noexcept {
    switch (usage) {
    case ResourceUsage::none:
        return "none";
    case ResourceUsage::sampledRead:
        return "sampledRead";
    case ResourceUsage::storageRead:
        return "storageRead";
    case ResourceUsage::storageWrite:
        return "storageWrite";
    case ResourceUsage::storageReadWrite:
        return "storageReadWrite";
    case ResourceUsage::uniformRead:
        return "uniformRead";
    case ResourceUsage::vertexRead:
        return "vertexRead";
    case ResourceUsage::indexRead:
        return "indexRead";
    case ResourceUsage::indirectRead:
        return "indirectRead";
    case ResourceUsage::colorAttachment:
        return "colorAttachment";
    case ResourceUsage::depthRead:
        return "depthRead";
    case ResourceUsage::depthWrite:
        return "depthWrite";
    case ResourceUsage::transferSrc:
        return "transferSrc";
    case ResourceUsage::transferDst:
        return "transferDst";
    case ResourceUsage::asBuildRead:
        return "asBuildRead";
    case ResourceUsage::asBuildWrite:
        return "asBuildWrite";
    case ResourceUsage::rayTracingRead:
        return "rayTracingRead";
    case ResourceUsage::hostRead:
        return "hostRead";
    case ResourceUsage::externalRead:
        return "externalRead";
    case ResourceUsage::externalWrite:
        return "externalWrite";
    case ResourceUsage::present:
        return "present";
    }
    return "mixed";
}

[[nodiscard]] inline std::string_view toString(TextureDimension dimension) noexcept {
    switch (dimension) {
    case TextureDimension::d1:
        return "1D";
    case TextureDimension::d2:
        return "2D";
    case TextureDimension::d3:
        return "3D";
    case TextureDimension::cube:
        return "cube";
    }
    return "2D";
}

[[nodiscard]] constexpr bool isAliasingAllowed(ResourceLifetime lifetime) noexcept {
    return lifetime == ResourceLifetime::transient;
}

// Only transient resources may alias each other. Persistent resources back
// history that must survive across frames (PreviousFrame, BDPT accumulation,
// particles, temporal history) and therefore can never share physical memory.
[[nodiscard]] constexpr bool canAlias(const TextureResourceDesc& left, const TextureResourceDesc& right) noexcept {
    return isAliasingAllowed(left.lifetime) && isAliasingAllowed(right.lifetime);
}

[[nodiscard]] constexpr bool canAlias(const BufferResourceDesc& left, const BufferResourceDesc& right) noexcept {
    return isAliasingAllowed(left.lifetime) && isAliasingAllowed(right.lifetime);
}

[[nodiscard]] constexpr bool mustPreserveForHistory(ResourceLifetime lifetime) noexcept {
    return lifetime == ResourceLifetime::persistent;
}

[[nodiscard]] inline bool isValidTextureDesc(const TextureResourceDesc& desc) noexcept {
    if (desc.extent.width == 0 || desc.extent.height == 0 || desc.extent.depth == 0)
        return false;
    if (desc.mipLevels == 0 || desc.arrayLayers == 0)
        return false;
    if (desc.dimension == TextureDimension::d3 && desc.arrayLayers != 1)
        return false;
    if (toBits(desc.usage) == 0)
        return false;
    return true;
}

// ---- VMA-friendly allocation hooks ----
// CPU-side size estimates the Vulkan backend feeds into VMA pool selection.
// They stay conservative (mip tail rounded up per level) so aliasing decisions
// made from them never under-allocate physical memory.
[[nodiscard]] constexpr std::size_t pixelFormatByteSize(PixelFormat format) noexcept {
    switch (format) {
    case PixelFormat::rgba8Unorm:
    case PixelFormat::rgba8Srgb:
        return 4;
    case PixelFormat::rgba16Float:
        return 8;
    case PixelFormat::rgba32Float:
        return 16;
    case PixelFormat::depth32Float:
        return 4;
    }
    return 4;
}

[[nodiscard]] inline std::size_t estimateTextureBytes(const TextureResourceDesc& desc) noexcept {
    const std::size_t pixel = pixelFormatByteSize(desc.format);
    std::size_t bytes = 0;
    std::uint32_t width = desc.extent.width;
    std::uint32_t height = desc.extent.height;
    std::uint32_t depth = desc.extent.depth;
    for (std::uint32_t mip = 0; mip < desc.mipLevels; ++mip) {
        bytes += static_cast<std::size_t>(width) * height * depth * pixel;
        width = width > 1 ? width / 2 : 1;
        height = height > 1 ? height / 2 : 1;
        depth = depth > 1 ? depth / 2 : 1;
    }
    return bytes * desc.arrayLayers;
}

[[nodiscard]] constexpr std::size_t estimateBufferBytes(const BufferResourceDesc& desc) noexcept {
    return desc.size;
}

[[nodiscard]] constexpr bool isDepthFormat(PixelFormat format) noexcept {
    return format == PixelFormat::depth32Float;
}

// Frame-indexed retirement queue. Destroyed resources stay alive for
// `keepFrames` frames (default 2, matching double/triple-buffered Vulkan
// flight) so in-flight command buffers never observe freed memory.
// `sweep(completedFrame)` returns handles that are now safe to destroy.
template <typename Handle> class RetirementQueue {
  public:
    explicit RetirementQueue(std::uint64_t keepFrames = 2) noexcept : keepFrames_(keepFrames) {}

    void retire(Handle handle, std::uint64_t currentFrame) {
        if (handle.valid())
            pending_.push_back(Entry{handle, currentFrame});
    }

    [[nodiscard]] std::vector<Handle> sweep(std::uint64_t completedFrame) {
        std::vector<Handle> ready;
        std::vector<Entry> remaining;
        remaining.reserve(pending_.size());
        for (const auto& entry : pending_) {
            if (entry.retireFrame + keepFrames_ <= completedFrame)
                ready.push_back(entry.handle);
            else
                remaining.push_back(entry);
        }
        pending_ = std::move(remaining);
        return ready;
    }

    [[nodiscard]] std::size_t pendingCount() const noexcept {
        return pending_.size();
    }

    [[nodiscard]] std::uint64_t keepFrames() const noexcept {
        return keepFrames_;
    }

    void clear() noexcept {
        pending_.clear();
    }

  private:
    struct Entry {
        Handle handle{};
        std::uint64_t retireFrame{};
    };
    std::vector<Entry> pending_;
    std::uint64_t keepFrames_;
};

using TextureRetirementQueue = RetirementQueue<handles::TextureHandle>;
using BufferRetirementQueue = RetirementQueue<handles::BufferHandle>;

// CPU-side registry for typed resource descriptors. GPU allocation itself
// stays in Device; this registry owns lifetimes, aliasing decisions, and
// generation-checked handles so render-graph compilation can be unit tested
// with a mock device.
class ResourceRegistry {
  public:
    [[nodiscard]] handles::TextureHandle createTexture(const TextureResourceDesc& desc) {
        const auto handle = textures_.create();
        ensureTextureCapacity(handle.index);
        textureDescs_[handle.index] = desc;
        return handle;
    }

    [[nodiscard]] handles::BufferHandle createBuffer(const BufferResourceDesc& desc) {
        const auto handle = buffers_.create();
        ensureBufferCapacity(handle.index);
        bufferDescs_[handle.index] = desc;
        return handle;
    }

    // Returns false for invalid or stale (generation-mismatched) handles.
    bool destroyTexture(handles::TextureHandle handle) {
        return textures_.destroy(handle);
    }

    bool destroyBuffer(handles::BufferHandle handle) {
        return buffers_.destroy(handle);
    }

    [[nodiscard]] bool isTextureAlive(handles::TextureHandle handle) const noexcept {
        return textures_.isAlive(handle);
    }

    [[nodiscard]] bool isBufferAlive(handles::BufferHandle handle) const noexcept {
        return buffers_.isAlive(handle);
    }

    [[nodiscard]] const TextureResourceDesc* findTexture(handles::TextureHandle handle) const noexcept {
        if (!textures_.isAlive(handle) || handle.index >= textureDescs_.size())
            return nullptr;
        return &textureDescs_[handle.index];
    }

    [[nodiscard]] const BufferResourceDesc* findBuffer(handles::BufferHandle handle) const noexcept {
        if (!buffers_.isAlive(handle) || handle.index >= bufferDescs_.size())
            return nullptr;
        return &bufferDescs_[handle.index];
    }

    // Alias query by handle: false when either handle is stale/dead or either
    // endpoint is persistent history.
    [[nodiscard]] bool canAliasTextures(handles::TextureHandle left, handles::TextureHandle right) const noexcept {
        const auto* leftDesc = findTexture(left);
        const auto* rightDesc = findTexture(right);
        if (leftDesc == nullptr || rightDesc == nullptr)
            return false;
        return canAlias(*leftDesc, *rightDesc);
    }

    [[nodiscard]] bool canAliasBuffers(handles::BufferHandle left, handles::BufferHandle right) const noexcept {
        const auto* leftDesc = findBuffer(left);
        const auto* rightDesc = findBuffer(right);
        if (leftDesc == nullptr || rightDesc == nullptr)
            return false;
        return canAlias(*leftDesc, *rightDesc);
    }

    [[nodiscard]] std::size_t aliveTextureCount() const noexcept {
        return textures_.aliveCount();
    }

    [[nodiscard]] std::size_t aliveBufferCount() const noexcept {
        return buffers_.aliveCount();
    }

    void clear() noexcept {
        textures_.clear();
        buffers_.clear();
        textureDescs_.clear();
        bufferDescs_.clear();
    }

  private:
    void ensureTextureCapacity(std::uint32_t index) {
        if (index >= textureDescs_.size())
            textureDescs_.resize(static_cast<std::size_t>(index) + 1U);
    }
    void ensureBufferCapacity(std::uint32_t index) {
        if (index >= bufferDescs_.size())
            bufferDescs_.resize(static_cast<std::size_t>(index) + 1U);
    }

    handles::TexturePool textures_;
    handles::BufferPool buffers_;
    std::vector<TextureResourceDesc> textureDescs_;
    std::vector<BufferResourceDesc> bufferDescs_;
};

} // namespace dayo::graphics
