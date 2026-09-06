#include "core/upload_ring.hpp"

#include <limits>
#include <stdexcept>

namespace dayo::core {

UploadRing::UploadRing(std::size_t capacity, std::size_t defaultAlignment)
    : capacity_(capacity), defaultAlignment_(defaultAlignment) {
    if (capacity_ == 0 || defaultAlignment_ == 0 || (defaultAlignment_ & (defaultAlignment_ - 1U)) != 0)
        throw std::invalid_argument("upload ring capacity must be non-zero and alignment must be a power of two");
}

std::optional<std::size_t> UploadRing::alignUp(std::size_t value, std::size_t alignment) const noexcept {
    const auto mask = alignment - 1U;
    if (value > std::numeric_limits<std::size_t>::max() - mask)
        return std::nullopt;
    return (value + mask) & ~mask;
}

std::optional<UploadSlice> UploadRing::tryAllocate(std::size_t size, std::uint64_t retireValue, std::size_t alignment) {
    if (size == 0 || size > capacity_)
        return std::nullopt;
    alignment = alignment == 0 ? defaultAlignment_ : alignment;
    if (alignment == 0 || (alignment & (alignment - 1U)) != 0)
        return std::nullopt;
    if (!allocations_.empty() && retireValue < allocations_.back().retireValue)
        return std::nullopt;

    if (used_ == 0) {
        head_ = tail_ = 0;
    }
    const auto alignedOffset = alignUp(head_, alignment);
    if (!alignedOffset)
        return std::nullopt;
    std::size_t offset = *alignedOffset;
    std::size_t begin = head_;
    const bool exceedsCapacity = offset > capacity_ || size > capacity_ - offset;
    if (head_ >= tail_) {
        if (exceedsCapacity) {
            offset = 0;
            begin = head_;
            if (size > tail_)
                return std::nullopt;
        }
    } else if (exceedsCapacity || offset > tail_ || size > tail_ - offset) {
        return std::nullopt;
    }

    const auto padding = offset >= begin ? offset - begin : capacity_ - begin + offset;
    if (padding > capacity_ || size > capacity_ - padding)
        return std::nullopt;
    const auto occupied = padding + size;
    if (occupied > capacity_ - used_)
        return std::nullopt;
    allocations_.push_back({begin, occupied, retireValue});
    used_ += occupied;
    head_ = (offset + size) % capacity_;
    return UploadSlice{offset, size};
}

void UploadRing::rollback(std::uint64_t retireValue) noexcept {
    while (!allocations_.empty() && allocations_.back().retireValue == retireValue) {
        const auto allocation = allocations_.back();
        allocations_.pop_back();
        used_ -= allocation.occupied;
        head_ = allocation.begin;
    }
    if (used_ == 0)
        head_ = tail_ = 0;
}

void UploadRing::reclaim(std::uint64_t completedValue) {
    while (!allocations_.empty() && allocations_.front().retireValue <= completedValue) {
        const auto allocation = allocations_.front();
        allocations_.pop_front();
        used_ -= allocation.occupied;
        tail_ = (allocation.begin + allocation.occupied) % capacity_;
    }
    if (used_ == 0)
        head_ = tail_ = 0;
}

void UploadRing::rollback(std::uint64_t retireValue) noexcept {
    while (!allocations_.empty() && allocations_.back().retireValue == retireValue) {
        const auto allocation = allocations_.back();
        allocations_.pop_back();
        used_ -= allocation.occupied;
        head_ = allocation.begin;
    }
    if (used_ == 0)
        head_ = tail_ = 0;
}

std::optional<std::uint64_t> UploadRing::oldestRetireValue() const noexcept {
    if (allocations_.empty())
        return std::nullopt;
    return allocations_.front().retireValue;
}

void UploadRing::reset() noexcept {
    allocations_.clear();
    head_ = tail_ = used_ = 0;
}

} // namespace dayo::core
