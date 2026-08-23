#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace dayo::core {

struct PmxMetadata {
    float version {};
    std::string modelName;
    std::string englishName;
    std::int32_t vertexCount {};
    std::uint8_t textEncoding {};
    std::uint8_t additionalUvCount {};
};

[[nodiscard]] PmxMetadata probePmx(const std::filesystem::path& path);

} // namespace dayo::core

