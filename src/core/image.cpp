#include "core/image.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include <stb_image.h>

#include <limits>
#include <stdexcept>

namespace dayo::core {

ImageRgba8 loadImageRgba8(const std::filesystem::path& path) {
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded = stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (decoded == nullptr) {
        throw std::runtime_error("cannot decode image " + path.string() + ": " + stbi_failure_reason());
    }
    if (width <= 0 || height <= 0
        || static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height)
             > std::numeric_limits<std::size_t>::max() / 4U) {
        stbi_image_free(decoded);
        throw std::runtime_error("invalid image dimensions: " + path.string());
    }
    ImageRgba8 image;
    image.width = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    const auto size = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
    image.pixels.assign(decoded, decoded + size);
    stbi_image_free(decoded);
    return image;
}

} // namespace dayo::core
