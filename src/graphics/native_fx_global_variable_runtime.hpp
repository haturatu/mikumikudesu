#pragma once

#include "graphics/device.hpp"

#include <cstdint>
#include <string>

namespace dayo::graphics {

// The pinned MikuMikuDayo 1.30 runtime creates one persistent implicit
// constant buffer per effect (1024 bytes by default) and binds it after the
// effect's controller buffer. The current ABI has no host-side write protocol,
// so this runtime initializes the allocation to zero and keeps its lifetime
// attached to the effect instance.
inline constexpr std::uint32_t kMaxNativeFxGlobalVariableBytes = 64U * 1024U;

class NativeFxGlobalVariableRuntime {
  public:
    NativeFxGlobalVariableRuntime() = default;
    ~NativeFxGlobalVariableRuntime();

    NativeFxGlobalVariableRuntime(const NativeFxGlobalVariableRuntime&) = delete;
    NativeFxGlobalVariableRuntime& operator=(const NativeFxGlobalVariableRuntime&) = delete;

    [[nodiscard]] bool initialize(Device& device, std::uint32_t size, std::string* error = nullptr);
    void reset() noexcept;

    [[nodiscard]] bool ready() const noexcept {
        return device_ != nullptr && (size_ == 0 || buffer_.valid());
    }
    [[nodiscard]] handles::BufferHandle buffer() const noexcept {
        return buffer_;
    }
    [[nodiscard]] std::uint32_t size() const noexcept {
        return size_;
    }

  private:
    Device* device_{};
    handles::BufferHandle buffer_{};
    std::uint32_t size_{};
};

} // namespace dayo::graphics
