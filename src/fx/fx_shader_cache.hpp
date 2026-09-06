#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dayo::fx {

struct FxShaderKey {
    std::string sourceHash;
    std::string hlslHash;
    std::string entryPoint{"main"};
    std::string stage;
    std::string dxcVersion;
    std::string spirvTarget;
    std::string compatProfile;
    std::string macros;
    [[nodiscard]] std::string combined() const {
        return sourceHash + "|" + hlslHash + "|" + entryPoint + "|" + stage + "|" + dxcVersion + "|" + spirvTarget +
               "|" + compatProfile + "|" + macros;
    }
};

struct FxPipelineKey {
    FxShaderKey shader;
    std::string renderTargetFormats;
    std::string depthFormat;
    std::string rasterState;
    std::string deviceUuid;
    std::string driver;

    [[nodiscard]] std::string combined() const {
        return shader.combined() + "|" + renderTargetFormats + "|" + depthFormat + "|" + rasterState + "|" +
               deviceUuid + "|" + driver;
    }
};

// CPU-side shader module cache keyed by source hash + entry + macros.
// Vulkan pipeline creation stays in the graphics executor; this cache only
// deduplicates identical compilations across effects.
class FxShaderCache {
  public:
    using Handle = std::uint64_t;
    Handle getOrCompile(const FxShaderKey& key, const std::string& source);
    [[nodiscard]] std::optional<Handle> find(const FxShaderKey& key) const;
    [[nodiscard]] std::optional<Handle> findExact(const FxShaderKey& key, const std::string& source) const;
    void clear() noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Handle> entries_;
    Handle next_{1};
};

class FxPipelineCache {
  public:
    using Handle = std::uint64_t;
    Handle getOrCreate(const FxPipelineKey& key);
    void clear() noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Handle> entries_;
    Handle next_{1};
};

} // namespace dayo::fx
