#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dayo::graphics {

struct SbtProperties {
    std::uint32_t handleSize{1};
    std::uint32_t handleAlignment{1};
    std::uint32_t baseAlignment{1};
    std::uint32_t maxStride{0xFFFFFFFFU};
};

// Standalone Shader Binding Table layout builder shared by Subayai and BDPT.
// Ray generation, miss and hit groups share handle size/alignment; only the
// group lists differ. Pure CPU layout math so Preview-only GPUs can still
// construct (and skip) native tables.
class ShaderBindingTableBuilder {
  public:
    using Properties = dayo::graphics::SbtProperties;
    using SbtProperties = dayo::graphics::SbtProperties;

    struct Layout {
        std::uint64_t raygenAddress{0};
        std::uint64_t missAddress{0};
        std::uint64_t hitAddress{0};
        std::uint32_t raygenStride{0};
        std::uint32_t missStride{0};
        std::uint32_t hitStride{0};
        std::uint64_t totalSize{0};
    };

    void setRaygen(std::string name);
    void addMiss(std::string name);
    void addHitGroup(std::string name);
    void clear() noexcept;

    [[nodiscard]] std::size_t raygenCount() const noexcept {
        return raygen_.size();
    }
    [[nodiscard]] std::size_t missCount() const noexcept {
        return miss_.size();
    }
    [[nodiscard]] std::size_t hitCount() const noexcept {
        return hitGroups_.size();
    }
    [[nodiscard]] std::size_t totalGroups() const noexcept {
        return raygen_.size() + miss_.size() + hitGroups_.size();
    }

    [[nodiscard]] const std::vector<std::string>& raygen() const noexcept {
        return raygen_;
    }
    [[nodiscard]] const std::vector<std::string>& miss() const noexcept {
        return miss_;
    }
    [[nodiscard]] const std::vector<std::string>& hitGroups() const noexcept {
        return hitGroups_;
    }

    // Properties correspond to VkPhysicalDeviceRayTracingPipelinePropertiesKHR.
    // Region starts are aligned independently; handleAlignment constrains the
    // stride and maxStride is enforced before returning a layout.
    [[nodiscard]] Layout build(std::uint64_t baseAddress, const Properties& properties) const noexcept;
    // Compatibility overload for callers that have not queried Vulkan yet.
    // Such a layout is only a host-side estimate because baseAlignment is
    // unknown; native Vulkan callers must use Properties.
    [[nodiscard]] Layout build(std::uint64_t baseAddress, std::uint32_t handleSize,
                               std::uint32_t handleAlignment) const noexcept {
        return build(baseAddress, Properties{handleSize, handleAlignment, 1, 0xFFFFFFFFU});
    }

  private:
    std::vector<std::string> raygen_;
    std::vector<std::string> miss_;
    std::vector<std::string> hitGroups_;
};

[[nodiscard]] std::uint32_t alignUp(std::uint32_t value, std::uint32_t alignment) noexcept;

} // namespace dayo::graphics
