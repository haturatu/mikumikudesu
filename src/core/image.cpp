#include "core/image.hpp"
#include "core/parse_budget.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace dayo::core {
namespace {

std::uint32_t u32(const std::uint8_t* value) {
    return static_cast<std::uint32_t>(value[0]) | (static_cast<std::uint32_t>(value[1]) << 8U) |
           (static_cast<std::uint32_t>(value[2]) << 16U) | (static_cast<std::uint32_t>(value[3]) << 24U);
}

std::uint32_t fourCc(char a, char b, char c, char d) {
    return static_cast<std::uint8_t>(a) | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b)) << 8U) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(c)) << 16U) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(d)) << 24U);
}

std::array<std::uint8_t, 4> rgb565(std::uint16_t value) {
    return {static_cast<std::uint8_t>(((value >> 11U) & 31U) * 255U / 31U),
            static_cast<std::uint8_t>(((value >> 5U) & 63U) * 255U / 63U),
            static_cast<std::uint8_t>((value & 31U) * 255U / 31U), 255};
}

void writePixel(ImageRgba8& image, std::uint32_t x, std::uint32_t y, const std::array<std::uint8_t, 4>& color) {
    if (x >= image.width || y >= image.height)
        return;
    const auto offset = (static_cast<std::size_t>(y) * image.width + x) * 4U;
    std::copy(color.begin(), color.end(), image.pixels.begin() + static_cast<std::ptrdiff_t>(offset));
}

std::array<std::array<std::uint8_t, 4>, 4> colorPalette(const std::uint8_t* block, bool dxt1) {
    const auto color0 = static_cast<std::uint16_t>(block[0] | (static_cast<std::uint16_t>(block[1]) << 8U));
    const auto color1 = static_cast<std::uint16_t>(block[2] | (static_cast<std::uint16_t>(block[3]) << 8U));
    std::array<std::array<std::uint8_t, 4>, 4> colors{rgb565(color0), rgb565(color1)};
    if (!dxt1 || color0 > color1) {
        for (int channel = 0; channel < 3; ++channel) {
            colors[2][channel] = static_cast<std::uint8_t>((2U * colors[0][channel] + colors[1][channel]) / 3U);
            colors[3][channel] = static_cast<std::uint8_t>((colors[0][channel] + 2U * colors[1][channel]) / 3U);
        }
        colors[2][3] = colors[3][3] = 255;
    } else {
        for (int channel = 0; channel < 3; ++channel) {
            colors[2][channel] = static_cast<std::uint8_t>((colors[0][channel] + colors[1][channel]) / 2U);
        }
        colors[2][3] = 255;
        colors[3] = {0, 0, 0, 0};
    }
    return colors;
}

std::array<std::uint8_t, 8> alphaPalette(const std::uint8_t* block) {
    std::array<std::uint8_t, 8> values{block[0], block[1]};
    if (values[0] > values[1]) {
        for (std::uint32_t i = 1; i <= 6; ++i) {
            values[i + 1] = static_cast<std::uint8_t>(((7U - i) * values[0] + i * values[1]) / 7U);
        }
    } else {
        for (std::uint32_t i = 1; i <= 4; ++i) {
            values[i + 1] = static_cast<std::uint8_t>(((5U - i) * values[0] + i * values[1]) / 5U);
        }
        values[6] = 0;
        values[7] = 255;
    }
    return values;
}

std::uint8_t alphaIndex(const std::uint8_t* block, std::uint32_t pixel) {
    std::uint64_t bits = 0;
    for (std::uint32_t i = 0; i < 6; ++i)
        bits |= static_cast<std::uint64_t>(block[2 + i]) << (i * 8U);
    return static_cast<std::uint8_t>((bits >> (pixel * 3U)) & 7U);
}

std::size_t checkedRgbaBytes(std::uint32_t width, std::uint32_t height) {
    const auto pixels = static_cast<std::uint64_t>(width) * height;
    if (width == 0 || height == 0 || pixels > ParseBudget::defaultMaxDecodedBytes / 4U ||
        pixels > std::numeric_limits<std::size_t>::max() / 4U)
        throw std::runtime_error("invalid DDS dimensions");
    return static_cast<std::size_t>(pixels * 4U);
}

enum class BlockFormat { bc1, bc2, bc3, bc4, bc5 };

ImageRgba8 decodeBlocks(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> data,
                        BlockFormat format) {
    ImageRgba8 image{width, height, std::vector<std::uint8_t>(checkedRgbaBytes(width, height))};
    const std::uint32_t blockSize = (format == BlockFormat::bc1 || format == BlockFormat::bc4) ? 8U : 16U;
    const auto blocksWide = (width + 3U) / 4U;
    const auto blocksHigh = (height + 3U) / 4U;
    const auto expectedBytes = static_cast<std::uint64_t>(blocksWide) * blocksHigh * blockSize;
    if (expectedBytes > data.size()) {
        throw std::runtime_error("truncated DDS block data");
    }
    for (std::uint32_t by = 0; by < blocksHigh; ++by)
        for (std::uint32_t bx = 0; bx < blocksWide; ++bx) {
            const auto* block = data.data() + (static_cast<std::size_t>(by) * blocksWide + bx) * blockSize;
            if (format == BlockFormat::bc4 || format == BlockFormat::bc5) {
                const auto red = alphaPalette(block);
                const auto green = format == BlockFormat::bc5 ? alphaPalette(block + 8) : red;
                for (std::uint32_t pixel = 0; pixel < 16; ++pixel) {
                    const auto r = red[alphaIndex(block, pixel)];
                    const auto g = green[alphaIndex(format == BlockFormat::bc5 ? block + 8 : block, pixel)];
                    writePixel(image, bx * 4U + pixel % 4U, by * 4U + pixel / 4U,
                               {r, g, format == BlockFormat::bc5 ? static_cast<std::uint8_t>(255) : r, 255});
                }
                continue;
            }
            const auto* colorBlock = format == BlockFormat::bc1 ? block : block + 8;
            const auto colors = colorPalette(colorBlock, format == BlockFormat::bc1);
            const auto colorBits = u32(colorBlock + 4);
            const auto alphas = format == BlockFormat::bc3 ? alphaPalette(block) : std::array<std::uint8_t, 8>{};
            for (std::uint32_t pixel = 0; pixel < 16; ++pixel) {
                auto color = colors[(colorBits >> (pixel * 2U)) & 3U];
                if (format == BlockFormat::bc2) {
                    const auto nibble = static_cast<std::uint8_t>((block[pixel / 2U] >> ((pixel & 1U) * 4U)) & 15U);
                    color[3] = static_cast<std::uint8_t>(nibble * 17U);
                } else if (format == BlockFormat::bc3) {
                    color[3] = alphas[alphaIndex(block, pixel)];
                }
                writePixel(image, bx * 4U + pixel % 4U, by * 4U + pixel / 4U, color);
            }
        }
    return image;
}

std::uint8_t unpackChannel(std::uint32_t value, std::uint32_t mask, std::uint8_t fallback) {
    if (mask == 0)
        return fallback;
    const auto shift = static_cast<std::uint32_t>(std::countr_zero(mask));
    const auto maximum = mask >> shift;
    return static_cast<std::uint8_t>(((value & mask) >> shift) * 255U / maximum);
}

ImageRgba8 decodeDds(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        throw std::runtime_error("cannot open DDS image: " + path.string());
    const auto end = input.tellg();
    if (end < 128)
        throw std::runtime_error("invalid DDS header: " + path.string());
    const auto fileSize = static_cast<std::uint64_t>(end);
    std::array<std::uint8_t, 128> header{};
    input.seekg(0);
    input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (!input || std::memcmp(header.data(), "DDS ", 4) != 0 || u32(header.data() + 4) != 124U ||
        u32(header.data() + 76) != 32U)
        throw std::runtime_error("invalid DDS file: " + path.string());
    const auto height = u32(header.data() + 12);
    const auto width = u32(header.data() + 16);
    const auto rgbaBytes = checkedRgbaBytes(width, height);
    const auto flags = u32(header.data() + 80);
    auto code = u32(header.data() + 84);
    std::size_t dataOffset = 128;
    if (code == fourCc('D', 'X', '1', '0')) {
        if (fileSize < 148)
            throw std::runtime_error("truncated DDS DX10 header");
        std::array<std::uint8_t, 20> dx10Header{};
        input.read(reinterpret_cast<char*>(dx10Header.data()), static_cast<std::streamsize>(dx10Header.size()));
        if (!input)
            throw std::runtime_error("truncated DDS DX10 header");
        const auto dxgi = u32(dx10Header.data());
        dataOffset = 148;
        if (dxgi == 71 || dxgi == 72)
            code = fourCc('D', 'X', 'T', '1');
        else if (dxgi == 74 || dxgi == 75)
            code = fourCc('D', 'X', 'T', '3');
        else if (dxgi == 77 || dxgi == 78)
            code = fourCc('D', 'X', 'T', '5');
        else if (dxgi == 80 || dxgi == 81)
            code = fourCc('B', 'C', '4', 'U');
        else if (dxgi == 83 || dxgi == 84)
            code = fourCc('B', 'C', '5', 'U');
        else if (dxgi == 28 || dxgi == 29)
            code = fourCc('R', 'G', 'B', 'A');
        else if (dxgi == 87 || dxgi == 91)
            code = fourCc('B', 'G', 'R', 'A');
        else
            throw std::runtime_error("unsupported DDS DXGI format " + std::to_string(dxgi));
    }
    if (fileSize < dataOffset)
        throw std::runtime_error("truncated DDS payload");
    std::uint64_t expectedPayload = rgbaBytes;
    BlockFormat blockFormat{};
    bool compressed = false;
    if (code == fourCc('D', 'X', 'T', '1')) {
        blockFormat = BlockFormat::bc1;
        compressed = true;
    } else if (code == fourCc('D', 'X', 'T', '3')) {
        blockFormat = BlockFormat::bc2;
        compressed = true;
    } else if (code == fourCc('D', 'X', 'T', '5')) {
        blockFormat = BlockFormat::bc3;
        compressed = true;
    } else if (code == fourCc('A', 'T', 'I', '1') || code == fourCc('B', 'C', '4', 'U')) {
        blockFormat = BlockFormat::bc4;
        compressed = true;
    } else if (code == fourCc('A', 'T', 'I', '2') || code == fourCc('B', 'C', '5', 'U')) {
        blockFormat = BlockFormat::bc5;
        compressed = true;
    }
    const auto bits = u32(header.data() + 88);
    if (!compressed && (flags & 0x40U) == 0 && code != fourCc('R', 'G', 'B', 'A') &&
        code != fourCc('B', 'G', 'R', 'A'))
        throw std::runtime_error("unsupported DDS FourCC");
    if (!compressed && bits != 32 && code != fourCc('R', 'G', 'B', 'A') && code != fourCc('B', 'G', 'R', 'A'))
        throw std::runtime_error("unsupported DDS pixel depth");
    if (compressed) {
        const auto blockSize = (blockFormat == BlockFormat::bc1 || blockFormat == BlockFormat::bc4) ? 8U : 16U;
        const auto blocksWide = (static_cast<std::uint64_t>(width) + 3U) / 4U;
        const auto blocksHigh = (static_cast<std::uint64_t>(height) + 3U) / 4U;
        expectedPayload = blocksWide * blocksHigh * blockSize;
    }
    if (expectedPayload > fileSize - dataOffset)
        throw std::runtime_error("truncated DDS payload");
    std::vector<std::uint8_t> payload(static_cast<std::size_t>(expectedPayload));
    input.seekg(static_cast<std::streamoff>(dataOffset));
    input.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    if (!input)
        throw std::runtime_error("truncated DDS payload");
    const auto payloadSpan = std::span<const std::uint8_t>(payload);
    if (compressed)
        return decodeBlocks(width, height, payloadSpan, blockFormat);
    const bool rgba = code == fourCc('R', 'G', 'B', 'A');
    const bool bgra = code == fourCc('B', 'G', 'R', 'A');
    const auto rMask = rgba ? 0x000000FFU : (bgra ? 0x00FF0000U : u32(header.data() + 92));
    const auto gMask = (rgba || bgra) ? 0x0000FF00U : u32(header.data() + 96);
    const auto bMask = rgba ? 0x00FF0000U : (bgra ? 0x000000FFU : u32(header.data() + 100));
    const auto aMask = (rgba || bgra) ? 0xFF000000U : u32(header.data() + 104);
    ImageRgba8 image{width, height, std::vector<std::uint8_t>(rgbaBytes)};
    for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) {
        const auto value = u32(payloadSpan.data() + i * 4U);
        image.pixels[i * 4U] = unpackChannel(value, rMask, 0);
        image.pixels[i * 4U + 1] = unpackChannel(value, gMask, 0);
        image.pixels[i * 4U + 2] = unpackChannel(value, bMask, 0);
        image.pixels[i * 4U + 3] = unpackChannel(value, aMask, 255);
    }
    return image;
}

} // namespace

ImageRgba8 loadImageRgba8(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(),
                           [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (extension == ".dds")
        return decodeDds(path);
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded = stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (decoded == nullptr) {
        throw std::runtime_error("cannot decode image " + path.string() + ": " + stbi_failure_reason());
    }
    if (width <= 0 || height <= 0 ||
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) >
            std::numeric_limits<std::size_t>::max() / 4U) {
        stbi_image_free(decoded);
        throw std::runtime_error("invalid image dimensions: " + path.string());
    }
    ImageRgba8 image;
    image.width = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    const auto size = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
    image.pixels.assign(decoded, decoded + size);
    stbi_image_free(decoded);
    return image;
}

} // namespace dayo::core
