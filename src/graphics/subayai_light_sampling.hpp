#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dayo::graphics {

// Walker alias table for light sampling. The light count is always taken from
// the caller (controller/builtin contribution); no NUM_LIGHTS constant lives
// here. The table rebuilds only when lighting is dirty.
struct AliasEntry {
    float probability{0.0F};
    std::uint32_t alias{0};
};

class LightSamplingService {
  public:
    LightSamplingService() = default;

    // Rebuilds the alias table only when lightingDirty is true.
    void update(std::span<const float> lightPowers, bool lightingDirty);

    [[nodiscard]] std::span<const AliasEntry> table() const noexcept {
        return {table_.data(), table_.size()};
    }
    [[nodiscard]] std::size_t lightCount() const noexcept {
        return table_.size();
    }
    [[nodiscard]] std::uint64_t buildCount() const noexcept {
        return builds_;
    }
    void clear() noexcept;

  private:
    std::vector<AliasEntry> table_;
    std::uint64_t builds_{0};
};

} // namespace dayo::graphics
