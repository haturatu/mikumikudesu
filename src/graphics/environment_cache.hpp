#pragma once

#include "core/cache_key.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace dayo::graphics {

// A bounded, versioned cache of RGBA16F cubemap faces followed by prefiltered mips.
// Keys include linear source pixels, exposure, dimensions and the shader source digest.
inline std::optional<std::filesystem::path> environmentCachePath(std::string_view key) {
    const char* root = std::getenv("XDG_CACHE_HOME");
    std::filesystem::path directory;
    if (root != nullptr && *root != '\0' && std::filesystem::path(root).is_absolute())
        directory = root;
    else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0')
        directory = std::filesystem::path(home) / ".cache";
    else
        return std::nullopt;
    return directory / "mikumikudesu" / "environment" / (std::string(key) + ".bin");
}

inline std::size_t environmentCacheBytes(std::uint32_t faceSize, std::uint32_t mipLevels) {
    std::size_t bytes = static_cast<std::size_t>(faceSize) * faceSize * 8U * 6U;
    for (std::uint32_t mip = 0; mip < mipLevels; ++mip) {
        const auto size = std::max(faceSize >> mip, 1U);
        bytes += static_cast<std::size_t>(size) * size * 8U * 6U;
    }
    return bytes;
}

inline std::uint64_t environmentCacheChecksum(std::span<const std::uint8_t> bytes) {
    return core::fnv1a64({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
}

inline std::vector<std::uint8_t> readEnvironmentCache(const std::filesystem::path& path, std::uint32_t faceSize,
                                                      std::uint32_t mipLevels) {
    if (faceSize == 0 || faceSize > 512 || mipLevels == 0 || mipLevels > 10)
        return {};
    std::ifstream input(path, std::ios::binary);
    std::array<std::uint32_t, 4> header{};
    std::uint64_t checksum{};
    input.read(reinterpret_cast<char*>(header.data()), sizeof(header));
    input.read(reinterpret_cast<char*>(&checksum), sizeof(checksum));
    if (!input || header != std::array<std::uint32_t, 4>{0x454E5631, 1, faceSize, mipLevels})
        return {};
    std::vector<std::uint8_t> bytes(environmentCacheBytes(faceSize, mipLevels));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.peek() != std::char_traits<char>::eof() || environmentCacheChecksum(bytes) != checksum)
        return {};
    return bytes;
}

inline void writeEnvironmentCache(const std::filesystem::path& path, std::uint32_t faceSize, std::uint32_t mipLevels,
                                  std::span<const std::uint8_t> bytes) {
    if (bytes.size() != environmentCacheBytes(faceSize, mipLevels))
        throw std::invalid_argument("environment cache payload has the wrong size");
    std::filesystem::create_directories(path.parent_path());
    const auto temporary = path.string() + ".tmp-" + core::toHex(std::random_device{}()) + "-" +
                           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    const std::array<std::uint32_t, 4> header{0x454E5631, 1, faceSize, mipLevels};
    const auto checksum = environmentCacheChecksum(bytes);
    output.write(reinterpret_cast<const char*>(header.data()), sizeof(header));
    output.write(reinterpret_cast<const char*>(&checksum), sizeof(checksum));
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("could not write environment cache");
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("could not publish environment cache: " + error.message());
    }
}

} // namespace dayo::graphics
