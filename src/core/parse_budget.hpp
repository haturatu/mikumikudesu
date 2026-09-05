#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace dayo::core {

class ParseBudget final {
  public:
    static constexpr std::uint64_t defaultMaxDecodedBytes = 512ULL * 1024ULL * 1024ULL;
    static constexpr std::uint64_t defaultMaxTotalElements = 50'000'000ULL;

    explicit ParseBudget(std::uint64_t maxDecodedBytes = defaultMaxDecodedBytes,
                         std::uint64_t maxTotalElements = defaultMaxTotalElements)
        : maxDecodedBytes_(maxDecodedBytes), maxTotalElements_(maxTotalElements) {}

    void checkCount(std::uint64_t count, std::uint64_t maxElementsPerSection, std::uint64_t minimumRecordBytes,
                    std::uint64_t remainingInputBytes, std::uint64_t decodedBytesPerElement, std::string_view field) {
        remainingInputBytes_ = remainingInputBytes;
        if (count > maxElementsPerSection)
            throw std::runtime_error("invalid element count for " + std::string(field));
        if (minimumRecordBytes != 0 && count > remainingInputBytes / minimumRecordBytes)
            throw std::runtime_error("element count exceeds remaining input for " + std::string(field));
        if (count > maxTotalElements_ - std::min(totalElements_, maxTotalElements_))
            throw std::runtime_error("parse element budget exceeded by " + std::string(field));
        totalElements_ += count;
        if (decodedBytesPerElement != 0)
            accountDecodedBytes(checkedMultiply(count, decodedBytesPerElement, field), field);
    }

    void accountDecodedBytes(std::uint64_t bytes, std::string_view field) {
        if (bytes > maxDecodedBytes_ - std::min(decodedBytes_, maxDecodedBytes_))
            throw std::runtime_error("decoded byte budget exceeded by " + std::string(field));
        decodedBytes_ += bytes;
    }

    void accountInputBytes(std::uint64_t bytes, std::uint64_t remainingInputBytes, std::string_view field) {
        remainingInputBytes_ = remainingInputBytes;
        if (bytes > remainingInputBytes_)
            throw std::runtime_error("input is too short for " + std::string(field));
    }

    [[nodiscard]] std::uint64_t remainingInputBytes() const noexcept {
        return remainingInputBytes_;
    }
    [[nodiscard]] std::uint64_t decodedBytes() const noexcept {
        return decodedBytes_;
    }
    [[nodiscard]] std::uint64_t totalElements() const noexcept {
        return totalElements_;
    }

  private:
    static std::uint64_t checkedMultiply(std::uint64_t left, std::uint64_t right, std::string_view field) {
        if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left)
            throw std::runtime_error("decoded size overflow for " + std::string(field));
        return left * right;
    }

    std::uint64_t maxDecodedBytes_;
    std::uint64_t maxTotalElements_;
    std::uint64_t decodedBytes_{};
    std::uint64_t totalElements_{};
    std::uint64_t remainingInputBytes_{};
};

} // namespace dayo::core
