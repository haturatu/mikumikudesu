#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dayo::core {

// Deterministic cache-key design. Keys hash only declared inputs (source,
// entry, defines, dimensions, format); driver handles and absolute paths are
// excluded so keys are stable across machines.
struct ShaderCacheKey {
    std::string source;
    std::string entry{"main"};
    std::string profile;
    std::vector<std::string> defines;
    [[nodiscard]] std::string digest() const;
};

struct PipelineCacheKey {
    ShaderCacheKey vertex;
    ShaderCacheKey fragment;
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t sampleCount{1};
    std::string colorFormat{"R8G8B8A8_SRGB"};
    [[nodiscard]] std::string digest() const;
};

struct TextureCacheKey {
    std::string logicalPath;
    std::uint32_t width{};
    std::uint32_t height{};
    std::string format{"RGBA8"};
    [[nodiscard]] std::string digest() const;
};

[[nodiscard]] std::uint64_t fnv1a64(std::string_view text) noexcept;
[[nodiscard]] std::string toHex(std::uint64_t value);

} // namespace dayo::core
