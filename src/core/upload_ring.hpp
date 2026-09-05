#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

namespace dayo::core {

struct UploadSlice {
    std::size_t offset{};
    std::size_t size{};
};

// A small CPU-side allocator for persistently mapped upload buffers. Vulkan
// users associate each allocation with a timeline value and call reclaim()
// after that value has completed.
class UploadRing {
  public:
    explicit UploadRing(std::size_t capacity, std::size_t defaultAlignment = 1);

    [[nodiscard]] std::optional<UploadSlice> tryAllocate(std::size_t size, std::uint64_t retireValue,
                                                         std::size_t alignment = 0);
    void reclaim(std::uint64_t completedValue);
    void reset() noexcept;

    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }
    [[nodiscard]] std::size_t used() const noexcept {
        return used_;
    }
    [[nodiscard]] std::size_t available() const noexcept {
        return capacity_ - used_;
    }

  private:
    struct Allocation {
        std::size_t begin{};
        std::size_t occupied{};
        std::uint64_t retireValue{};
    };

    [[nodiscard]] std::size_t alignUp(std::size_t value, std::size_t alignment) const noexcept;

    std::size_t capacity_{};
    std::size_t defaultAlignment_{};
    std::size_t head_{};
    std::size_t tail_{};
    std::size_t used_{};
    std::deque<Allocation> allocations_;
};

} // namespace dayo::core
