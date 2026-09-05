#pragma once

#include <cstdint>

namespace dayo::graphics {

// Returns the modular distance between two Vulkan timestamp values. Vulkan
// exposes only timestampValidBits low bits on a queue, so both operands are
// masked before subtraction to handle counter wrap-around correctly.
[[nodiscard]] inline std::uint64_t timestampDelta(std::uint64_t begin, std::uint64_t end,
                                                  std::uint32_t validBits) noexcept {
    if (validBits == 0)
        return 0;
    if (validBits >= 64)
        return end - begin;
    const auto mask = (std::uint64_t{1} << validBits) - 1U;
    return ((end & mask) - (begin & mask)) & mask;
}

} // namespace dayo::graphics
