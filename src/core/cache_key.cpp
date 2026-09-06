#include "core/cache_key.hpp"

#include <cstdio>
#include <sstream>
#include <string_view>

namespace dayo::core {

std::uint64_t fnv1a64(std::string_view text) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : text) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(byte));
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string toHex(std::uint64_t value) {
    char buffer[17]{};
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
    return std::string(buffer);
}

namespace {

void mix(std::uint64_t& hash, std::string_view text) noexcept {
    for (const auto byte : text) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(byte));
        hash *= 1099511628211ULL;
    }
    hash ^= 0xFFULL;
    hash *= 1099511628211ULL;
}

} // namespace

std::string ShaderCacheKey::digest() const {
    std::uint64_t hash = 14695981039346656037ULL;
    mix(hash, source);
    mix(hash, entry);
    mix(hash, profile);
    for (const auto& define : defines)
        mix(hash, define);
    return toHex(hash);
}

std::string PipelineCacheKey::digest() const {
    std::uint64_t hash = 14695981039346656037ULL;
    mix(hash, vertex.digest());
    mix(hash, fragment.digest());
    std::ostringstream numbers;
    numbers << width << 'x' << height << " msaa" << sampleCount << ' ' << colorFormat;
    mix(hash, numbers.str());
    return toHex(hash);
}

std::string TextureCacheKey::digest() const {
    std::uint64_t hash = 14695981039346656037ULL;
    mix(hash, logicalPath);
    std::ostringstream numbers;
    numbers << width << 'x' << height << ' ' << format;
    mix(hash, numbers.str());
    return toHex(hash);
}

} // namespace dayo::core
