#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace dayo::test {

// Raw header fields remain available for intentionally incomplete parser fixtures.
struct DdsFixtureDesc {
    std::uint32_t width{1};
    std::uint32_t height{1};
    std::uint32_t depth{};
    std::uint32_t mipLevels{};
    std::uint32_t pixelFlags{4};
    std::uint32_t fourCc{};
    std::uint32_t rgbBits{};
    std::uint32_t redMask{};
    std::uint32_t greenMask{};
    std::uint32_t blueMask{};
    std::uint32_t alphaMask{};
    std::uint32_t caps{};
    std::uint32_t caps2{};
    std::optional<std::uint32_t> dxgiFormat{};
    std::uint32_t dimension{3};
    std::uint32_t miscFlag{};
    std::uint32_t arraySize{1};
};

void putLe32(std::span<std::uint8_t> output, std::size_t offset, std::uint32_t value);
std::vector<std::uint8_t> makeDdsHeader(const DdsFixtureDesc& desc);
void writeDds(const std::filesystem::path& path, const DdsFixtureDesc& desc,
              std::span<const std::uint8_t> payload = {});
void writePpm(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
              std::span<const std::uint8_t> rgb);

} // namespace dayo::test
