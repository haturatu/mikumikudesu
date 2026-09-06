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
    std::string entryPoint{"main"};
    std::string macros;
    [[nodiscard]] std::string combined() const {
        return sourceHash + "|" + entryPoint + "|" + macros;
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
    void clear() noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Handle> entries_;
    Handle next_{1};
};

} // namespace dayo::fx
