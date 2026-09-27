#pragma once
#include "core/image_hdr.hpp"
#include "graphics/fx_resource_runtime.hpp"

namespace dayo::graphics {
struct FxDebugRequest {
    std::string owner;
    std::string name;
    std::uint64_t generation{};
    std::uint32_t mip{};
    std::uint32_t slice{};
    int mode{};
    float scale{1.0F};
    std::filesystem::path hlslDirectory;
    std::filesystem::path dumpPath;
};
struct FxDebugResult {
    std::string label;
    std::string message;
    core::ImageRgba8 preview;
    std::vector<std::uint8_t> buffer;
};
[[nodiscard]] std::uint64_t fxDebugGeneration(const FxResourceStore::Resource& resource) noexcept;
[[nodiscard]] FxDebugResult readFxDebugResource(Device& device, CommandList& commands,
                                                const FxResourceStore::Resource& resource,
                                                const FxDebugRequest& request);
} // namespace dayo::graphics
