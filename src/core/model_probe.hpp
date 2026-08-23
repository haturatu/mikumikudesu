#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dayo::core {

struct PmxMetadata {
    float version {};
    std::string modelName;
    std::string englishName;
    std::int32_t vertexCount {};
    std::uint8_t textEncoding {};
    std::uint8_t additionalUvCount {};
};

struct PmxVertex {
    float position[3] {};
    float normal[3] {};
    float uv[2] {};
};

struct PmxMesh {
    PmxMetadata metadata;
    std::vector<PmxVertex> vertices;
    std::vector<std::uint32_t> indices;
};

[[nodiscard]] PmxMetadata probePmx(const std::filesystem::path& path);
[[nodiscard]] PmxMesh loadPmxMesh(const std::filesystem::path& path);

} // namespace dayo::core
