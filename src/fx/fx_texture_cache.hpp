#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace dayo::fx {

struct FxTextureKey {
    std::filesystem::path path;
    bool srgb{};
    [[nodiscard]] std::string combined() const {
        return path.string() + (srgb ? "|srgb" : "|unorm");
    }
};

struct FxTextureEntry {
    std::uint64_t handle{};
    std::uint32_t width{};
    std::uint32_t height{};
    bool hasTransparency{};
};

// CPU-side texture metadata cache. GPU upload stays with the backend;
// the cache keeps dimensions/transparency so plans can size passes
// without touching the filesystem per frame.
class FxTextureCache {
  public:
    using Handle = std::uint64_t;
    Handle store(const FxTextureKey& key, std::uint32_t width, std::uint32_t height, bool hasTransparency = false);
    [[nodiscard]] std::optional<FxTextureEntry> find(const FxTextureKey& key) const;
    void clear() noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, FxTextureEntry> entries_;
    Handle next_{1};
};

} // namespace dayo::fx
