#pragma once

#include "fx/fx_compiler.hpp"
#include "graphics/device.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dayo::graphics {

// Owns the GPU resources declared by one compiled .fxdayo program. The
// runtime deliberately keeps resource names and descriptor bindings together
// so a hot-reloaded program cannot accidentally resolve a name against the
// previous program's allocation.
class FxResourceRuntime {
  public:
    struct ResolvedTexture {
        handles::TextureHandle handle{};
        Extent3D extent{};
        PixelFormat format{PixelFormat::rgba8Unorm};

        [[nodiscard]] bool valid() const noexcept {
            return handle.valid() && extent.width != 0 && extent.height != 0 && extent.depth == 1;
        }
    };

    FxResourceRuntime() = default;
    ~FxResourceRuntime();

    FxResourceRuntime(const FxResourceRuntime&) = delete;
    FxResourceRuntime& operator=(const FxResourceRuntime&) = delete;

    [[nodiscard]] bool initialize(Device& device, const fx::FxProgram& program, const fx::FxFrameContext& context,
                                   std::string* error = nullptr);
    void reset() noexcept;

    [[nodiscard]] bool ready() const noexcept {
        return device_ != nullptr;
    }
    [[nodiscard]] std::optional<handles::TextureHandle> resolveTexture(std::string_view name) const;
    [[nodiscard]] std::optional<handles::BufferHandle> resolveBuffer(std::string_view name) const;
    [[nodiscard]] std::optional<handles::SamplerHandle> resolveSampler(std::string_view name) const;
    [[nodiscard]] std::optional<handles::DescriptorSetHandle>
    resolveDescriptorSet(const fx::FxDispatch&) const;
    [[nodiscard]] handles::DescriptorSetLayoutHandle descriptorLayout() const noexcept {
        return descriptorLayout_;
    }
    [[nodiscard]] handles::DescriptorSetHandle descriptorSet() const noexcept {
        return descriptorSet_;
    }
    [[nodiscard]] const DescriptorSetLayoutDesc& descriptorLayoutDesc() const noexcept {
        return descriptorLayoutDesc_;
    }
    [[nodiscard]] std::optional<Extent3D> extent(std::string_view name) const;
    // Resolves the last texture written by a frame plan. Native presentation
    // uses this explicit final-write rule instead of guessing a resource name
    // such as "screen" or "output" from an effect authoring convention.
    [[nodiscard]] std::optional<ResolvedTexture>
    resolveOutputTexture(std::span<const fx::FxDispatch> ordered) const;
    [[nodiscard]] std::size_t resourceCount() const noexcept {
        return resources_.size();
    }

  private:
    enum class Kind : std::uint8_t { texture, buffer, sampler };

    struct Resource {
        std::string name;
        Kind kind{Kind::texture};
        DescriptorKind descriptorKind{DescriptorKind::sampledImage};
        std::uint32_t binding{};
        handles::TextureHandle texture{};
        handles::BufferHandle buffer{};
        handles::SamplerHandle sampler{};
        Extent3D extent{};
        PixelFormat format{PixelFormat::rgba8Unorm};
    };

    Device* device_{};
    std::vector<Resource> resources_;
    std::unordered_map<std::string, std::size_t> indices_;
    DescriptorSetLayoutDesc descriptorLayoutDesc_;
    handles::DescriptorSetLayoutHandle descriptorLayout_{};
    handles::DescriptorSetHandle descriptorSet_{};
};

} // namespace dayo::graphics
