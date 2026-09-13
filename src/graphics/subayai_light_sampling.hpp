#pragma once

#include "graphics/device.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
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

// Device-side alias table mirror. The CPU service remains authoritative and
// only dirty tables are uploaded, so native lighting can bind a stable storage
// buffer without rebuilding it every frame.
class LightSamplingGpuRuntime {
  public:
    LightSamplingGpuRuntime() = default;
    ~LightSamplingGpuRuntime();

    LightSamplingGpuRuntime(const LightSamplingGpuRuntime&) = delete;
    LightSamplingGpuRuntime& operator=(const LightSamplingGpuRuntime&) = delete;

    [[nodiscard]] bool sync(Device& device, std::span<const AliasEntry> table, std::string* error = nullptr);
    void reset() noexcept;

    [[nodiscard]] bool ready() const noexcept {
        return device_ != nullptr && buffer_.valid() && count_ != 0;
    }
    [[nodiscard]] handles::BufferHandle buffer() const noexcept {
        return buffer_;
    }
    [[nodiscard]] std::size_t count() const noexcept {
        return count_;
    }

  private:
    Device* device_{};
    handles::BufferHandle buffer_{};
    std::size_t count_{};
};

} // namespace dayo::graphics
