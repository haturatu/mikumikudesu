#pragma once

#include <cstddef>
#include <memory_resource>
#include <vector>

namespace dayo::core {

class FrameScratch {
  public:
    static constexpr std::size_t initialCapacity = 4U * 1024U * 1024U;

    FrameScratch() : buffer_(initialCapacity), resource_(buffer_.data(), buffer_.size()) {}

    void reset() noexcept {
        resource_.release();
    }

    [[nodiscard]] std::pmr::memory_resource* resource() noexcept {
        return &resource_;
    }

  private:
    std::vector<std::byte> buffer_;
    std::pmr::monotonic_buffer_resource resource_;
};

} // namespace dayo::core
