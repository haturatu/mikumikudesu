#pragma once

#include "core/fx/fx_size.hpp"
#include "fx/fx_compiler.hpp"
#include "graphics/device.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dayo::graphics {

class FxMaterialGpuRuntime;

struct FxPassDescriptorSet {
    std::uint32_t setIndex{};
    handles::DescriptorSetLayoutHandle layout{};
    handles::DescriptorSetHandle set{};
};

// Owns only physical textures, buffers, and samplers. Descriptor views are
// deliberately absent: the same physical resource can be an SRV in one pass
// and a UAV in the next pass.
class FxResourceStore {
  public:
    enum class Kind : std::uint8_t { texture, buffer, sampler };

    struct Resource {
        std::string name;
        Kind kind{Kind::texture};
        handles::TextureHandle texture{};
        handles::BufferHandle buffer{};
        handles::SamplerHandle sampler{};
        Extent3D extent{};
        PixelFormat format{PixelFormat::rgba8Unorm};
        std::uint32_t dimension{2};
        std::uint64_t allocationBytes{};
        std::uint32_t elementSize{};
        std::string elementType{};
        // Compatibility-only physical slot used by the old single-set API.
        // Per-pass descriptors never consult these fields.
        DescriptorKind legacyDescriptorKind{DescriptorKind::sampledImage};
        std::uint32_t legacyBinding{};
    };

    [[nodiscard]] bool add(Resource resource);
    void clear() noexcept;
    [[nodiscard]] std::size_t size() const noexcept {
        return resources_.size();
    }
    [[nodiscard]] const Resource* find(std::string_view name) const noexcept;
    [[nodiscard]] Resource* find(std::string_view name) noexcept;
    [[nodiscard]] const std::vector<Resource>& resources() const noexcept {
        return resources_;
    }

  private:
    std::vector<Resource> resources_;
    std::unordered_map<std::string, std::size_t> indices_;
};

using FxMaterialRuntimeInitializer = std::function<const FxMaterialGpuRuntime*(const FxResourceStore&, std::string*)>;

// Owns one descriptor layout/set per pass. Its plans are generated from the
// same FxPassBindingPlan used by shader source generation.
class FxPassDescriptorRuntime {
  public:
    FxPassDescriptorRuntime() = default;
    ~FxPassDescriptorRuntime();

    FxPassDescriptorRuntime(const FxPassDescriptorRuntime&) = delete;
    FxPassDescriptorRuntime& operator=(const FxPassDescriptorRuntime&) = delete;

    [[nodiscard]] bool initialize(Device& device, const fx::FxProgram& program, std::uint32_t resourceSet,
                                  const FxResourceStore& store, std::string* error = nullptr,
                                  const FxMaterialGpuRuntime* materialRuntime = nullptr);
    void reset() noexcept;

    [[nodiscard]] std::vector<FxPassDescriptorSet> descriptorSets(const fx::FxDispatch& dispatch) const;
    [[nodiscard]] std::vector<std::pair<std::uint32_t, handles::DescriptorSetLayoutHandle>>
    descriptorLayouts(const fx::FxDispatch& dispatch) const;

    [[nodiscard]] std::optional<handles::DescriptorSetHandle>
    resolveDescriptorSet(const fx::FxDispatch& dispatch) const;
    [[nodiscard]] std::optional<handles::DescriptorSetLayoutHandle>
    resolveDescriptorLayout(const fx::FxDispatch& dispatch) const;
    [[nodiscard]] const fx::FxPassBindingPlan* bindingPlan(const fx::FxDispatch& dispatch) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept {
        return entries_.size();
    }

  private:
    struct Entry {
        fx::FxPassBindingPlan plan;
        struct Set {
            std::uint32_t index{};
            handles::DescriptorSetLayoutHandle layout{};
            std::vector<handles::DescriptorSetHandle> handles;
            std::vector<std::vector<DescriptorBindingEx>> bindings;
        };
        std::vector<Set> sets;
    };

    Device* device_{};
    const FxResourceStore* store_{};
    const FxMaterialGpuRuntime* materialRuntime_{};
    mutable std::unordered_map<std::string, Entry> entries_;
};

// Owns the GPU resources declared by one compiled .fxdayo program. The
// runtime deliberately keeps resource names and descriptor bindings together
// so a hot-reloaded program cannot accidentally resolve a name against the
// previous program's allocation.
class FxResourceRuntime : public core::fx::FxResourceTable {
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
    ~FxResourceRuntime() override;

    FxResourceRuntime(const FxResourceRuntime&) = delete;
    FxResourceRuntime& operator=(const FxResourceRuntime&) = delete;

    [[nodiscard]] bool initialize(Device& device, const fx::FxProgram& program, const fx::FxFrameContext& context,
                                  std::string* error = nullptr, std::uint32_t resourceSet = 0,
                                  const FxMaterialGpuRuntime* materialRuntime = nullptr,
                                  const FxMaterialRuntimeInitializer& initializeMaterialRuntime = {});
    void reset() noexcept;

    [[nodiscard]] bool ready() const noexcept {
        return device_ != nullptr;
    }
    [[nodiscard]] std::optional<handles::TextureHandle> resolveTexture(std::string_view name) const;
    [[nodiscard]] std::optional<handles::BufferHandle> resolveBuffer(std::string_view name) const;
    [[nodiscard]] std::optional<handles::SamplerHandle> resolveSampler(std::string_view name) const;
    [[nodiscard]] std::optional<handles::DescriptorSetHandle> resolveDescriptorSet(const fx::FxDispatch&) const;
    [[nodiscard]] handles::DescriptorSetLayoutHandle descriptorLayout() const noexcept {
        return descriptorLayout_;
    }
    [[nodiscard]] handles::DescriptorSetHandle descriptorSet() const noexcept {
        return descriptorSet_;
    }
    [[nodiscard]] std::optional<handles::DescriptorSetLayoutHandle>
    descriptorLayoutFor(const fx::FxDispatch& dispatch) const {
        return passDescriptors_.resolveDescriptorLayout(dispatch);
    }
    [[nodiscard]] std::vector<FxPassDescriptorSet> descriptorSetsFor(const fx::FxDispatch& dispatch) const {
        return passDescriptors_.descriptorSets(dispatch);
    }
    [[nodiscard]] std::vector<std::pair<std::uint32_t, handles::DescriptorSetLayoutHandle>>
    descriptorLayoutsFor(const fx::FxDispatch& dispatch) const {
        return passDescriptors_.descriptorLayouts(dispatch);
    }
    [[nodiscard]] std::optional<handles::DescriptorSetHandle> descriptorSetFor(const fx::FxDispatch& dispatch) const {
        return passDescriptors_.resolveDescriptorSet(dispatch);
    }
    [[nodiscard]] const fx::FxPassBindingPlan* bindingPlan(const fx::FxDispatch& dispatch) const noexcept {
        return passDescriptors_.bindingPlan(dispatch);
    }
    [[nodiscard]] const DescriptorSetLayoutDesc& descriptorLayoutDesc() const noexcept {
        return descriptorLayoutDesc_;
    }
    [[nodiscard]] std::optional<Extent3D> extent(std::string_view name) const;
    [[nodiscard]] std::optional<core::fx::FxExtent> find(std::string_view name) const override;
    [[nodiscard]] const FxResourceStore& store() const noexcept {
        return store_;
    }
    // Resolves the last texture written by a frame plan. Native presentation
    // uses this explicit final-write rule instead of guessing a resource name
    // such as "screen" or "output" from an effect authoring convention.
    [[nodiscard]] std::optional<ResolvedTexture> resolveOutputTexture(std::span<const fx::FxDispatch> ordered) const;
    [[nodiscard]] std::size_t resourceCount() const noexcept {
        return store_.size();
    }

  private:
    Device* device_{};
    FxResourceStore store_;
    FxPassDescriptorRuntime passDescriptors_;
    DescriptorSetLayoutDesc descriptorLayoutDesc_;
    handles::DescriptorSetLayoutHandle descriptorLayout_{};
    handles::DescriptorSetHandle descriptorSet_{};
};

} // namespace dayo::graphics
