#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace dayo::core {

struct ImageRgba8 {
    std::uint32_t width {};
    std::uint32_t height {};
    std::vector<std::uint8_t> pixels;
};

[[nodiscard]] ImageRgba8 loadImageRgba8(const std::filesystem::path& path);

} // namespace dayo::core
