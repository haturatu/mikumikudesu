#pragma once

#include "fx/fx_compiler.hpp"
#include "fx/fx_frame.hpp"
#include "graphics/device.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>

namespace dayo::graphics {

// Generic Vulkan FX executor skeleton. Reuses the existing Device and
// CommandList contracts without owning or destroying them:
//   plan (FxFramePlan) -> executor -> backend (CommandList).
// Supported: raster / postprocess / compute / copy / clear / mipmap.
// Raytracing fails explicitly so Preview callers notice the missing path
// instead of silently dropping a pass.
class VulkanFxExecutor {
  public:
    struct Stats {
        std::size_t raster{};
        std::size_t postprocess{};
        std::size_t compute{};
        std::size_t copy{};
        std::size_t clear{};
        std::size_t mipmap{};
    };

    explicit VulkanFxExecutor(Device& device) noexcept : device_(&device) {}

    VulkanFxExecutor(const VulkanFxExecutor&) = delete;
    VulkanFxExecutor& operator=(const VulkanFxExecutor&) = delete;

    [[nodiscard]] Device& device() const noexcept {
        return *device_;
    }

    Stats execute(const dayo::fx::FxFramePlan& plan, CommandList& commands,
                  const dayo::fx::FxFrameContext& context) const;

  private:
    Device* device_;
};

class FxRaytracingUnsupported : public std::logic_error {
  public:
    explicit FxRaytracingUnsupported(const std::string& pass)
        : std::logic_error("VulkanFxExecutor: raytracing pass '" + pass + "' is not supported in the generic path") {}
};

} // namespace dayo::graphics
