#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dayo::core {

struct ImageRgba8 {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> pixels;
};

enum class DdsDimension : std::uint8_t { twoD, threeD, cube };

struct DdsImageMetadata {
    DdsDimension dimension{DdsDimension::twoD};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t depth{1};
    std::uint32_t arrayLayers{1};
    std::uint32_t mipLevels{1};
};

struct ImageRgba8Subresource {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t depth{1};
    std::vector<std::uint8_t> pixels;
};

// DDS data decoded to RGBA8 and kept in DDS subresource order: array layer,
// then mip level. Cubemaps expose their faces as six layers per cube.
struct DdsImageRgba8 {
    DdsDimension dimension{DdsDimension::twoD};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t depth{1};
    std::uint32_t arrayLayers{1};
    std::uint32_t mipLevels{1};
    std::vector<ImageRgba8Subresource> subresources;

    [[nodiscard]] const ImageRgba8Subresource& subresource(std::uint32_t mipLevel, std::uint32_t arrayLayer = 0) const;
};

struct TextureImage : DdsImageRgba8 {
    std::string format{"R8G8B8A8_UNORM"};
};
// Preserves numeric component formats; BC1-5 UNORM decode to RGBA8,
// signed BC4/5 decode to float, HDR to float32.
[[nodiscard]] TextureImage loadTextureImage(const std::filesystem::path& path);

[[nodiscard]] ImageRgba8 loadImageRgba8(const std::filesystem::path& path);
[[nodiscard]] DdsImageRgba8 loadDdsImageRgba8(const std::filesystem::path& path);
[[nodiscard]] DdsImageMetadata inspectDdsImage(const std::filesystem::path& path);

} // namespace dayo::core
