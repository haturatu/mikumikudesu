#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dayo::fx {

// Shader-module cache key (TASKS.md section 23). Covers the full
// compilation identity so a stale module is never reused after a source,
// macro, toolchain, or compatibility change:
//   fx source dependency hash + generated HLSL + macro set + entry point +
//   shader stage + DXC version + SPIR-V target + compatibility profile.
// The compatibility profile is a plain string ("upstream130" vs
// "nativeExtended", cf. dayo::core::fx::FxCompatibilityProfile in PR1) to
// keep this library independent of the expr/size PR.
struct FxShaderKey {
    std::string sourceHash;
    std::string hlslHash;
    std::string entryPoint{"main"};
    std::string macros;
    std::string stage{"pixel"};
    std::string dxcVersion;
    std::string spirvTarget{"1.3"};
    std::string compatProfile{"upstream130"};
    [[nodiscard]] std::string combined() const {
        return sourceHash + "|" + hlslHash + "|" + entryPoint + "|" + macros + "|" + stage + "|" + dxcVersion + "|" +
               spirvTarget + "|" + compatProfile;
    }
};

// Pipeline cache key: shader identity plus the target/pipeline state that
// makes a compiled pipeline non-portable across framebuffers, raster
// state, and physical devices.
struct FxPipelineKey {
    FxShaderKey shader;
    std::string renderTargetFormats{"rgba8"};
    std::string depthFormat{"none"};
    std::string rasterState;
    std::string deviceUuid;
    std::string driver;
    [[nodiscard]] std::string combined() const {
        return shader.combined() + "|rt=" + renderTargetFormats + "|depth=" + depthFormat + "|raster=" + rasterState +
               "|dev=" + deviceUuid + "|drv=" + driver;
    }
};

// CPU-side shader module cache keyed by the full compilation identity.
// Vulkan pipeline creation stays in the graphics executor; this cache only
// deduplicates identical compilations across effects.
class FxShaderCache {
  public:
    using Handle = std::uint64_t;
    Handle getOrCompile(const FxShaderKey& key, const std::string& source);
    // Exact lookup including the source payload hash.
    [[nodiscard]] std::optional<Handle> findExact(const FxShaderKey& key, const std::string& source) const;
    // Prefix lookup on the key identity (diagnostics; may alias sources).
    [[nodiscard]] std::optional<Handle> find(const FxShaderKey& key) const;
    void clear() noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Handle> entries_;
    Handle next_{1};
};

// Deduplicated pipeline handles keyed by FxPipelineKey. Creation stays in
// the graphics executor; the cache owns only the identity -> handle map.
class FxPipelineCache {
  public:
    using Handle = std::uint64_t;
    Handle getOrCreate(const FxPipelineKey& key);
    [[nodiscard]] std::optional<Handle> find(const FxPipelineKey& key) const;
    void clear() noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Handle> entries_;
    Handle next_{1};
};

} // namespace dayo::fx
