#include "core/image.hpp"
#include "core/image_hdr.hpp"

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
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace dayo::core {
namespace {

constexpr std::uint64_t kImageAllocationBudget = 512ULL * 1024ULL * 1024ULL;

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

void writePixel(std::vector<std::uint8_t>& pixels, std::uint32_t width, std::uint32_t height, std::uint32_t depth,
                std::uint32_t z, std::uint32_t x, std::uint32_t y, const std::array<std::uint8_t, 4>& color) {
    if (x >= width || y >= height || z >= depth)
        return;
    const auto offset = ((static_cast<std::size_t>(z) * height + y) * width + x) * 4U;
    std::copy(color.begin(), color.end(), pixels.begin() + static_cast<std::ptrdiff_t>(offset));
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

std::uint64_t checkedMultiply(std::uint64_t left, std::uint64_t right, std::string_view field) {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left)
        throw std::runtime_error("image size overflow for " + std::string(field));
    return left * right;
}

std::uint64_t checkedRgbaBytes(std::uint32_t width, std::uint32_t height, std::string_view field) {
    if (width == 0 || height == 0)
        throw std::runtime_error("invalid " + std::string(field) + " dimensions");
    const auto pixels = checkedMultiply(width, height, field);
    const auto bytes = checkedMultiply(pixels, 4U, field);
    if (bytes > std::numeric_limits<std::size_t>::max())
        throw std::runtime_error("image size exceeds addressable memory for " + std::string(field));
    return bytes;
}

std::uint64_t checkedRgbaVolumeBytes(std::uint32_t width, std::uint32_t height, std::uint32_t depth,
                                     std::string_view field) {
    if (depth == 0)
        throw std::runtime_error("invalid " + std::string(field) + " depth");
    const auto bytes = checkedMultiply(checkedRgbaBytes(width, height, field), depth, field);
    if (bytes > std::numeric_limits<std::size_t>::max())
        throw std::runtime_error("image size exceeds addressable memory for " + std::string(field));
    return bytes;
}

void checkPeakAllocation(std::uint64_t inputBytes, std::uint64_t outputBytes, std::string_view field) {
    if (inputBytes > kImageAllocationBudget || outputBytes > kImageAllocationBudget - inputBytes)
        throw std::runtime_error("image allocation budget exceeded for " + std::string(field));
}

// Windows-authored Dayo assets use case-insensitive paths. Resolve each
// component on case-sensitive filesystems, rejecting ambiguous alternatives.
std::filesystem::path resolveImageFile(const std::filesystem::path& requested) {
    if (std::filesystem::exists(requested))
        return requested;
    const auto folded = [](std::string value) {
        std::ranges::transform(value, value.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    };
    const auto absolute = std::filesystem::absolute(requested).lexically_normal();
    auto resolved = absolute.root_path();
    for (const auto& component : absolute.relative_path()) {
        const auto direct = resolved / component;
        if (std::filesystem::exists(direct)) {
            resolved = direct;
            continue;
        }
        if (!std::filesystem::is_directory(resolved))
            return requested;
        std::filesystem::path match;
        for (const auto& candidate : std::filesystem::directory_iterator(resolved)) {
            if (folded(candidate.path().filename().string()) != folded(component.string()))
                continue;
            if (!match.empty())
                throw std::runtime_error("ambiguous image filename: " + requested.string());
            match = candidate.path();
        }
        if (match.empty())
            return requested;
        resolved = std::move(match);
    }
    return resolved;
}

[[nodiscard]] std::vector<std::uint8_t> readImageSnapshot(const std::filesystem::path& requested) {
    const auto path = resolveImageFile(requested);
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        throw std::runtime_error("cannot open image " + path.string());
    const auto end = input.tellg();
    if (end <= 0)
        throw std::runtime_error("empty image " + path.string());
    const auto size = static_cast<std::uint64_t>(end);
    if (size > kImageAllocationBudget || size > std::numeric_limits<std::size_t>::max() ||
        size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()))
        throw std::runtime_error("encoded image exceeds allocation budget: " + path.string());
    std::vector<std::uint8_t> snapshot(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(snapshot.data()), static_cast<std::streamsize>(snapshot.size()));
    if (!input || input.gcount() != static_cast<std::streamsize>(snapshot.size()))
        throw std::runtime_error("cannot read image " + path.string());
    return snapshot;
}

enum class BlockFormat { bc1, bc2, bc3, bc4, bc5 };

void decodeBlockSlice(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> data, BlockFormat format,
                      std::vector<std::uint8_t>& pixels, std::uint32_t z, std::uint32_t outputDepth) {
    const auto blockSize = (format == BlockFormat::bc1 || format == BlockFormat::bc4) ? 8U : 16U;
    const auto blocksWide = (static_cast<std::uint64_t>(width) + 3U) / 4U;
    const auto blocksHigh = (static_cast<std::uint64_t>(height) + 3U) / 4U;
    const auto expectedBytes =
        checkedMultiply(checkedMultiply(blocksWide, blocksHigh, "DDS blocks"), blockSize, "DDS block payload");
    if (expectedBytes > data.size())
        throw std::runtime_error("truncated DDS block data");
    for (std::uint32_t by = 0; by < blocksHigh; ++by)
        for (std::uint32_t bx = 0; bx < blocksWide; ++bx) {
            const auto* block = data.data() + (static_cast<std::size_t>(by) * blocksWide + bx) * blockSize;
            if (format == BlockFormat::bc4 || format == BlockFormat::bc5) {
                const auto red = alphaPalette(block);
                const auto green = format == BlockFormat::bc5 ? alphaPalette(block + 8) : red;
                for (std::uint32_t pixel = 0; pixel < 16; ++pixel) {
                    const auto r = red[alphaIndex(block, pixel)];
                    const auto g = green[alphaIndex(format == BlockFormat::bc5 ? block + 8 : block, pixel)];
                    writePixel(pixels, width, height, outputDepth, z, bx * 4U + pixel % 4U, by * 4U + pixel / 4U,
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
                writePixel(pixels, width, height, outputDepth, z, bx * 4U + pixel % 4U, by * 4U + pixel / 4U, color);
            }
        }
}

ImageRgba8 decodeBlocks(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> data,
                        BlockFormat format) {
    const auto rgbaBytes = checkedRgbaBytes(width, height, "DDS");
    checkPeakAllocation(data.size(), rgbaBytes, "DDS");
    ImageRgba8 image{width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(rgbaBytes))};
    decodeBlockSlice(width, height, data, format, image.pixels, 0, 1);
    return image;
}

std::uint8_t unpackChannel(std::uint32_t value, std::uint32_t mask, std::uint8_t fallback) {
    if (mask == 0)
        return fallback;
    const auto shift = static_cast<std::uint32_t>(std::countr_zero(mask));
    const auto maximum = mask >> shift;
    return static_cast<std::uint8_t>(((value & mask) >> shift) * 255U / maximum);
}

ImageRgba8 decodeDds(const std::filesystem::path& requested) {
    const auto path = resolveImageFile(requested);
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
    const auto rgbaBytes = checkedRgbaBytes(width, height, "DDS");
    const auto flags = u32(header.data() + 80);
    auto code = u32(header.data() + 84);
    std::uint64_t dataOffset = 128;
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
    if (!compressed && (flags & 0x40U) == 0 && code != fourCc('R', 'G', 'B', 'A') && code != fourCc('B', 'G', 'R', 'A'))
        throw std::runtime_error("unsupported DDS FourCC");
    if (!compressed && bits != 32 && code != fourCc('R', 'G', 'B', 'A') && code != fourCc('B', 'G', 'R', 'A'))
        throw std::runtime_error("unsupported DDS pixel depth");
    auto expectedPayload = rgbaBytes;
    if (compressed) {
        const auto blockSize = (blockFormat == BlockFormat::bc1 || blockFormat == BlockFormat::bc4) ? 8U : 16U;
        const auto blocksWide = (static_cast<std::uint64_t>(width) + 3U) / 4U;
        const auto blocksHigh = (static_cast<std::uint64_t>(height) + 3U) / 4U;
        expectedPayload =
            checkedMultiply(checkedMultiply(blocksWide, blocksHigh, "DDS blocks"), blockSize, "DDS block payload");
    }
    if (compressed) {
        checkPeakAllocation(expectedPayload, rgbaBytes, "DDS");
        if (expectedPayload > fileSize - dataOffset)
            throw std::runtime_error("truncated DDS payload");
        std::vector<std::uint8_t> payload(static_cast<std::size_t>(expectedPayload));
        input.seekg(static_cast<std::streamoff>(dataOffset));
        input.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        if (!input)
            throw std::runtime_error("truncated DDS payload");
        return decodeBlocks(width, height, payload, blockFormat);
    }
    checkPeakAllocation(0, rgbaBytes, "DDS");
    if (expectedPayload > fileSize - dataOffset)
        throw std::runtime_error("truncated DDS payload");
    const bool rgba = code == fourCc('R', 'G', 'B', 'A');
    const bool bgra = code == fourCc('B', 'G', 'R', 'A');
    const auto rMask = rgba ? 0x000000FFU : (bgra ? 0x00FF0000U : u32(header.data() + 92));
    const auto gMask = (rgba || bgra) ? 0x0000FF00U : u32(header.data() + 96);
    const auto bMask = rgba ? 0x00FF0000U : (bgra ? 0x000000FFU : u32(header.data() + 100));
    const auto aMask = (rgba || bgra) ? 0xFF000000U : u32(header.data() + 104);
    ImageRgba8 image{width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(rgbaBytes))};
    input.seekg(static_cast<std::streamoff>(dataOffset));
    input.read(reinterpret_cast<char*>(image.pixels.data()), static_cast<std::streamsize>(rgbaBytes));
    if (!input)
        throw std::runtime_error("truncated DDS payload");
    for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) {
        const auto value = u32(image.pixels.data() + i * 4U);
        image.pixels[i * 4U] = unpackChannel(value, rMask, 0);
        image.pixels[i * 4U + 1] = unpackChannel(value, gMask, 0);
        image.pixels[i * 4U + 2] = unpackChannel(value, bMask, 0);
        image.pixels[i * 4U + 3] = unpackChannel(value, aMask, 255);
    }
    return image;
}

[[nodiscard]] TextureImage decodeDdsTexture(std::span<const std::uint8_t> bytes, const std::filesystem::path& path,
                                            bool typed = false) {
    const auto readU32 = [bytes, &path](std::size_t offset) {
        if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint32_t))
            throw std::runtime_error("truncated DDS header: " + path.string());
        return u32(bytes.data() + offset);
    };
    if (bytes.size() < 128 || std::memcmp(bytes.data(), "DDS ", 4) != 0 || readU32(4) != 124U || readU32(76) != 32U)
        throw std::runtime_error("invalid DDS file: " + path.string());

    TextureImage result;
    std::uint32_t rawPixelBytes = 0;
    std::uint32_t signedBlockChannels = 0;
    result.width = readU32(16);
    result.height = readU32(12);
    result.depth = std::max(readU32(24), 1U);
    result.mipLevels = std::max(readU32(28), 1U);
    result.arrayLayers = 1;
    if (result.width == 0 || result.height == 0)
        throw std::runtime_error("invalid DDS dimensions: " + path.string());

    const auto pixelFlags = readU32(80);
    auto code = readU32(84);
    auto dataOffset = std::uint64_t{128};
    auto arraySize = 1U;
    const auto caps2 = readU32(112);
    if (code == fourCc('D', 'X', '1', '0')) {
        if (bytes.size() < 148)
            throw std::runtime_error("truncated DDS DX10 header: " + path.string());
        const auto dxgi = readU32(128);
        const auto resourceDimension = readU32(132);
        const auto miscFlag = readU32(136);
        arraySize = readU32(140);
        if (arraySize == 0)
            throw std::runtime_error("invalid DDS DX10 array size: " + path.string());
        dataOffset = 148;
        if (typed) {
            switch (dxgi) {
            case 61:
                result.format = "R8_UNORM";
                rawPixelBytes = 1;
                break;
            case 54:
                result.format = "R16_FLOAT";
                rawPixelBytes = 2;
                break;
            case 34:
                result.format = "R16G16_FLOAT";
                rawPixelBytes = 4;
                break;
            case 41:
                result.format = "R32_FLOAT";
                rawPixelBytes = 4;
                break;
            case 16:
                result.format = "R32G32_FLOAT";
                rawPixelBytes = 8;
                break;
            case 17:
                result.format = "R32G32_UINT";
                rawPixelBytes = 8;
                break;
            case 28:
                result.format = "R8G8B8A8_UNORM";
                rawPixelBytes = 4;
                break;
            case 29:
                result.format = "R8G8B8A8_SRGB";
                rawPixelBytes = 4;
                break;
            case 10:
                result.format = "R16G16B16A16_FLOAT";
                rawPixelBytes = 8;
                break;
            case 2:
                result.format = "R32G32B32A32_FLOAT";
                rawPixelBytes = 16;
                break;
            case 62:
                result.format = "R8_UINT";
                rawPixelBytes = 1;
                break;
            case 63:
                result.format = "R8_SNORM";
                rawPixelBytes = 1;
                break;
            case 64:
                result.format = "R8_SINT";
                rawPixelBytes = 1;
                break;
            case 50:
                result.format = "R8G8_UINT";
                rawPixelBytes = 2;
                break;
            case 51:
                result.format = "R8G8_SNORM";
                rawPixelBytes = 2;
                break;
            case 52:
                result.format = "R8G8_SINT";
                rawPixelBytes = 2;
                break;
            case 30:
                result.format = "R8G8B8A8_UINT";
                rawPixelBytes = 4;
                break;
            case 31:
                result.format = "R8G8B8A8_SNORM";
                rawPixelBytes = 4;
                break;
            case 32:
                result.format = "R8G8B8A8_SINT";
                rawPixelBytes = 4;
                break;
            case 57:
                result.format = "R16_UINT";
                rawPixelBytes = 2;
                break;
            case 58:
                result.format = "R16_SNORM";
                rawPixelBytes = 2;
                break;
            case 59:
                result.format = "R16_SINT";
                rawPixelBytes = 2;
                break;
            case 36:
                result.format = "R16G16_UINT";
                rawPixelBytes = 4;
                break;
            case 37:
                result.format = "R16G16_SNORM";
                rawPixelBytes = 4;
                break;
            case 38:
                result.format = "R16G16_SINT";
                rawPixelBytes = 4;
                break;
            case 12:
                result.format = "R16G16B16A16_UINT";
                rawPixelBytes = 8;
                break;
            case 13:
                result.format = "R16G16B16A16_SNORM";
                rawPixelBytes = 8;
                break;
            case 14:
                result.format = "R16G16B16A16_SINT";
                rawPixelBytes = 8;
                break;
            case 42:
                result.format = "R32_UINT";
                rawPixelBytes = 4;
                break;
            case 43:
                result.format = "R32_SINT";
                rawPixelBytes = 4;
                break;
            case 18:
                result.format = "R32G32_SINT";
                rawPixelBytes = 8;
                break;
            case 3:
                result.format = "R32G32B32A32_UINT";
                rawPixelBytes = 16;
                break;
            case 4:
                result.format = "R32G32B32A32_SINT";
                rawPixelBytes = 16;
                break;
            case 49:
                result.format = "R8G8_UNORM";
                rawPixelBytes = 2;
                break;
            case 56:
                result.format = "R16_UNORM";
                rawPixelBytes = 2;
                break;
            case 35:
                result.format = "R16G16_UNORM";
                rawPixelBytes = 4;
                break;
            case 11:
                result.format = "R16G16B16A16_UNORM";
                rawPixelBytes = 8;
                break;
            case 81:
                signedBlockChannels = 1;
                result.format = "R32_FLOAT";
                break;
            case 84:
                signedBlockChannels = 2;
                result.format = "R32G32_FLOAT";
                break;
            case 72:
            case 75:
            case 78:
            case 91:
                result.format = "R8G8B8A8_SRGB";
                break;
            default:
                break;
            }
        }
        if (rawPixelBytes != 0)
            code = fourCc('R', 'A', 'W', ' ');
        else if (dxgi == 71 || dxgi == 72)
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

        constexpr std::uint32_t kTexture2D = 3;
        constexpr std::uint32_t kTexture3D = 4;
        constexpr std::uint32_t kTextureCube = 0x4;
        if (resourceDimension == kTexture3D) {
            if (arraySize != 1 || (miscFlag & kTextureCube) != 0 || readU32(24) == 0)
                throw std::runtime_error("invalid DDS DX10 volume texture metadata: " + path.string());
            result.dimension = DdsDimension::threeD;
        } else if (resourceDimension == kTexture2D) {
            result.depth = 1;
            if ((miscFlag & kTextureCube) != 0) {
                result.dimension = DdsDimension::cube;
                if (arraySize > std::numeric_limits<std::uint32_t>::max() / 6U)
                    throw std::runtime_error("DDS cubemap array is too large: " + path.string());
                result.arrayLayers = arraySize * 6U;
            } else {
                result.dimension = DdsDimension::twoD;
                result.arrayLayers = arraySize;
            }
        } else {
            throw std::runtime_error("unsupported DDS DX10 resource dimension: " + path.string());
        }
    } else if ((caps2 & 0x200000U) != 0U) {
        if ((caps2 & 0x200U) != 0U || readU32(24) == 0)
            throw std::runtime_error("invalid DDS volume texture metadata: " + path.string());
        result.dimension = DdsDimension::threeD;
    } else if ((caps2 & 0x200U) != 0U) {
        constexpr std::uint32_t kAllCubeFaces = 0xFC00U;
        if ((caps2 & kAllCubeFaces) != kAllCubeFaces)
            throw std::runtime_error("incomplete DDS cubemap is unsupported: " + path.string());
        result.dimension = DdsDimension::cube;
        result.depth = 1;
        result.arrayLayers = 6;
    } else {
        result.dimension = DdsDimension::twoD;
        result.depth = 1;
    }

    if (typed && rawPixelBytes == 0) {
        switch (code) {
        case 111:
            result.format = "R16_FLOAT";
            rawPixelBytes = 2;
            break;
        case 112:
            result.format = "R16G16_FLOAT";
            rawPixelBytes = 4;
            break;
        case 113:
            result.format = "R16G16B16A16_FLOAT";
            rawPixelBytes = 8;
            break;
        case 114:
            result.format = "R32_FLOAT";
            rawPixelBytes = 4;
            break;
        case 115:
            result.format = "R32G32_FLOAT";
            rawPixelBytes = 8;
            break;
        case 116:
            result.format = "R32G32B32A32_FLOAT";
            rawPixelBytes = 16;
            break;
        default:
            break;
        }
    }
    if (typed && (code == fourCc('B', 'C', '4', 'S') || code == fourCc('B', 'C', '5', 'S'))) {
        signedBlockChannels = code == fourCc('B', 'C', '4', 'S') ? 1U : 2U;
        result.format = signedBlockChannels == 1 ? "R32_FLOAT" : "R32G32_FLOAT";
        code = signedBlockChannels == 1 ? fourCc('B', 'C', '4', 'U') : fourCc('B', 'C', '5', 'U');
    }
    if (rawPixelBytes != 0 && (readU32(8) & 8U) != 0 &&
        readU32(20) != checkedMultiply(result.width, rawPixelBytes, "DDS row pitch"))
        throw std::runtime_error("padded typed DDS row pitch is unsupported: " + path.string());

    auto largest = std::max({result.width, result.height, result.depth});
    std::uint32_t maximumMipLevels = 1;
    while (largest > 1) {
        largest >>= 1U;
        ++maximumMipLevels;
    }
    if (result.mipLevels > maximumMipLevels)
        throw std::runtime_error("DDS mip count exceeds the texture extent: " + path.string());

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
    const bool rgba = code == fourCc('R', 'G', 'B', 'A');
    const bool bgra = code == fourCc('B', 'G', 'R', 'A');
    const auto bits = readU32(88);
    if (!compressed && rawPixelBytes == 0 && !rgba && !bgra && (pixelFlags & 0x40U) == 0U)
        throw std::runtime_error("unsupported DDS pixel format: " + path.string());
    if (!compressed && rawPixelBytes == 0 && !rgba && !bgra && bits != 32)
        throw std::runtime_error("unsupported DDS pixel depth: " + path.string());
    if (!compressed && rawPixelBytes == 0 && !rgba && !bgra && code != 0)
        throw std::runtime_error("unsupported DDS FourCC: " + path.string());

    const auto rMask = rgba ? 0x000000FFU : (bgra ? 0x00FF0000U : readU32(92));
    const auto gMask = (rgba || bgra) ? 0x0000FF00U : readU32(96);
    const auto bMask = rgba ? 0x00FF0000U : (bgra ? 0x000000FFU : readU32(100));
    const auto aMask = (rgba || bgra) ? 0xFF000000U : readU32(104);
    std::uint64_t decodedBytes = 0;
    std::uint64_t offset = dataOffset;
    const auto subresourceCount = checkedMultiply(result.arrayLayers, result.mipLevels, "DDS subresource count");
    const auto metadataBytes =
        checkedMultiply(subresourceCount, sizeof(ImageRgba8Subresource), "DDS subresource metadata");
    if (subresourceCount > std::numeric_limits<std::size_t>::max() || metadataBytes > kImageAllocationBudget)
        throw std::runtime_error("DDS has too many subresources: " + path.string());
    checkPeakAllocation(bytes.size(), metadataBytes, "DDS");
    result.subresources.reserve(static_cast<std::size_t>(subresourceCount));
    for (std::uint32_t layer = 0; layer < result.arrayLayers; ++layer) {
        for (std::uint32_t mip = 0; mip < result.mipLevels; ++mip) {
            const auto width = std::max(1U, result.width >> mip);
            const auto height = std::max(1U, result.height >> mip);
            const auto depth = result.dimension == DdsDimension::threeD ? std::max(1U, result.depth >> mip) : 1U;
            const auto outputPixelBytes =
                rawPixelBytes != 0 ? rawPixelBytes : (signedBlockChannels != 0 ? signedBlockChannels * 4U : 4U);
            const auto outputBytes = checkedMultiply(checkedRgbaVolumeBytes(width, height, depth, "DDS") / 4U,
                                                     outputPixelBytes, "DDS typed pixels");
            if (outputBytes > std::numeric_limits<std::uint64_t>::max() - decodedBytes)
                throw std::runtime_error("DDS decoded size overflow: " + path.string());
            decodedBytes += outputBytes;
            if (metadataBytes > std::numeric_limits<std::uint64_t>::max() - decodedBytes)
                throw std::runtime_error("DDS allocation size overflow: " + path.string());
            checkPeakAllocation(bytes.size(), metadataBytes + decodedBytes, "DDS");

            std::uint64_t payloadBytes = outputBytes;
            std::uint64_t blockSliceBytes = 0;
            if (compressed) {
                const auto blockSize = (blockFormat == BlockFormat::bc1 || blockFormat == BlockFormat::bc4) ? 8U : 16U;
                const auto blocksWide = (static_cast<std::uint64_t>(width) + 3U) / 4U;
                const auto blocksHigh = (static_cast<std::uint64_t>(height) + 3U) / 4U;
                blockSliceBytes = checkedMultiply(checkedMultiply(blocksWide, blocksHigh, "DDS blocks"), blockSize,
                                                  "DDS block payload");
                payloadBytes = checkedMultiply(blockSliceBytes, depth, "DDS volume block payload");
            }
            if (offset > bytes.size() || payloadBytes > bytes.size() - offset)
                throw std::runtime_error("truncated DDS subresource payload: " + path.string());

            ImageRgba8Subresource subresource{.width = width,
                                              .height = height,
                                              .depth = depth,
                                              .pixels =
                                                  std::vector<std::uint8_t>(static_cast<std::size_t>(outputBytes))};
            const auto payload =
                bytes.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(payloadBytes));
            if (signedBlockChannels != 0) {
                const auto blocksWide = (static_cast<std::uint64_t>(width) + 3U) / 4U;
                for (std::uint32_t z = 0; z < depth; ++z) {
                    for (std::uint32_t y = 0; y < height; ++y) {
                        for (std::uint32_t x = 0; x < width; ++x) {
                            for (std::uint32_t channel = 0; channel < signedBlockChannels; ++channel) {
                                const auto blockOffset = blockSliceBytes * z +
                                                         ((y / 4U) * blocksWide + x / 4U) * (signedBlockChannels * 8U) +
                                                         channel * 8U;
                                const auto* block = payload.data() + blockOffset;
                                const auto endpoint = [](std::uint8_t value) {
                                    const int signedValue = value < 128 ? value : static_cast<int>(value) - 256;
                                    return std::max(-1.0F, static_cast<float>(signedValue) / 127.0F);
                                };
                                const float first = endpoint(block[0]);
                                const float second = endpoint(block[1]);
                                std::array<float, 8> palette{first, second};
                                if (first > second) {
                                    for (std::size_t i = 1; i <= 6; ++i)
                                        palette[i + 1] =
                                            (static_cast<float>(7 - i) * first + static_cast<float>(i) * second) / 7.0F;
                                } else {
                                    for (std::size_t i = 1; i <= 4; ++i)
                                        palette[i + 1] =
                                            (static_cast<float>(5 - i) * first + static_cast<float>(i) * second) / 5.0F;
                                    palette[6] = -1.0F;
                                    palette[7] = 1.0F;
                                }
                                std::uint64_t indices = 0;
                                for (unsigned i = 0; i < 6; ++i)
                                    indices |= static_cast<std::uint64_t>(block[i + 2]) << (i * 8U);
                                const auto index = (indices >> (((y % 4U) * 4U + x % 4U) * 3U)) & 7U;
                                const auto output =
                                    ((static_cast<std::size_t>(z) * height + y) * width + x) * signedBlockChannels +
                                    channel;
                                std::memcpy(subresource.pixels.data() + output * sizeof(float), &palette[index],
                                            sizeof(float));
                            }
                        }
                    }
                }
            } else if (compressed) {
                for (std::uint32_t z = 0; z < depth; ++z) {
                    const auto sliceOffset = static_cast<std::size_t>(blockSliceBytes) * z;
                    const auto slice = payload.subspan(sliceOffset, static_cast<std::size_t>(blockSliceBytes));
                    decodeBlockSlice(width, height, slice, blockFormat, subresource.pixels, z, depth);
                }
            } else if (rawPixelBytes != 0) {
                std::copy(payload.begin(), payload.end(), subresource.pixels.begin());
            } else {
                std::copy(payload.begin(), payload.end(), subresource.pixels.begin());
                const auto pixels = static_cast<std::size_t>(width) * height * depth;
                for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
                    const auto value = u32(subresource.pixels.data() + pixel * 4U);
                    subresource.pixels[pixel * 4U] = unpackChannel(value, rMask, 0);
                    subresource.pixels[pixel * 4U + 1U] = unpackChannel(value, gMask, 0);
                    subresource.pixels[pixel * 4U + 2U] = unpackChannel(value, bMask, 0);
                    subresource.pixels[pixel * 4U + 3U] = unpackChannel(value, aMask, 255);
                }
            }
            result.subresources.push_back(std::move(subresource));
            offset += payloadBytes;
        }
    }
    return result;
}

} // namespace

DdsImageMetadata inspectDdsImage(const std::filesystem::path& requested) {
    const auto path = resolveImageFile(requested);
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        throw std::runtime_error("cannot open DDS image: " + path.string());
    const auto end = input.tellg();
    if (end < 128)
        throw std::runtime_error("invalid DDS header: " + path.string());

    std::array<std::uint8_t, 148> header{};
    input.seekg(0);
    input.read(reinterpret_cast<char*>(header.data()), 128);
    if (!input || std::memcmp(header.data(), "DDS ", 4) != 0 || u32(header.data() + 4) != 124U ||
        u32(header.data() + 76) != 32U)
        throw std::runtime_error("invalid DDS file: " + path.string());

    DdsImageMetadata result;
    result.width = u32(header.data() + 16);
    result.height = u32(header.data() + 12);
    result.depth = std::max(u32(header.data() + 24), 1U);
    result.mipLevels = std::max(u32(header.data() + 28), 1U);
    if (result.width == 0 || result.height == 0)
        throw std::runtime_error("invalid DDS dimensions: " + path.string());

    const auto code = u32(header.data() + 84);
    const auto caps2 = u32(header.data() + 112);
    if (code == fourCc('D', 'X', '1', '0')) {
        if (end < 148)
            throw std::runtime_error("truncated DDS DX10 header: " + path.string());
        input.read(reinterpret_cast<char*>(header.data() + 128), 20);
        if (!input)
            throw std::runtime_error("truncated DDS DX10 header: " + path.string());

        constexpr std::uint32_t kTexture2D = 3;
        constexpr std::uint32_t kTexture3D = 4;
        constexpr std::uint32_t kTextureCube = 0x4;
        const auto resourceDimension = u32(header.data() + 132);
        const auto miscFlag = u32(header.data() + 136);
        const auto arraySize = u32(header.data() + 140);
        if (arraySize == 0)
            throw std::runtime_error("invalid DDS DX10 array size: " + path.string());
        if (resourceDimension == kTexture3D) {
            if (arraySize != 1 || (miscFlag & kTextureCube) != 0 || u32(header.data() + 24) == 0)
                throw std::runtime_error("invalid DDS DX10 volume metadata: " + path.string());
            result.dimension = DdsDimension::threeD;
        } else if (resourceDimension == kTexture2D) {
            result.depth = 1;
            if ((miscFlag & kTextureCube) != 0) {
                result.dimension = DdsDimension::cube;
                if (arraySize > std::numeric_limits<std::uint32_t>::max() / 6U)
                    throw std::runtime_error("DDS cubemap array is too large: " + path.string());
                result.arrayLayers = arraySize * 6U;
            } else {
                result.dimension = DdsDimension::twoD;
                result.arrayLayers = arraySize;
            }
        } else {
            throw std::runtime_error("unsupported DDS DX10 resource dimension: " + path.string());
        }
    } else if ((caps2 & 0x200000U) != 0U) {
        if ((caps2 & 0x200U) != 0U || u32(header.data() + 24) == 0)
            throw std::runtime_error("invalid DDS volume metadata: " + path.string());
        result.dimension = DdsDimension::threeD;
    } else if ((caps2 & 0x200U) != 0U) {
        constexpr std::uint32_t kAllCubeFaces = 0xFC00U;
        if ((caps2 & kAllCubeFaces) != kAllCubeFaces)
            throw std::runtime_error("incomplete DDS cubemap is unsupported: " + path.string());
        result.dimension = DdsDimension::cube;
        result.depth = 1;
        result.arrayLayers = 6;
    } else {
        result.dimension = DdsDimension::twoD;
        result.depth = 1;
    }

    auto largest = std::max({result.width, result.height, result.depth});
    std::uint32_t maximumMipLevels = 1;
    while (largest > 1) {
        largest >>= 1U;
        ++maximumMipLevels;
    }
    if (result.mipLevels > maximumMipLevels)
        throw std::runtime_error("DDS mip count exceeds the texture extent: " + path.string());
    return result;
}

const ImageRgba8Subresource& DdsImageRgba8::subresource(std::uint32_t mipLevel, std::uint32_t arrayLayer) const {
    if (mipLevel >= mipLevels || arrayLayer >= arrayLayers)
        throw std::out_of_range("DDS subresource index is out of range");
    const auto index = static_cast<std::size_t>(arrayLayer) * mipLevels + mipLevel;
    if (index >= subresources.size())
        throw std::logic_error("DDS image subresource table is incomplete");
    return subresources[index];
}

DdsImageRgba8 loadDdsImageRgba8(const std::filesystem::path& path) {
    const auto snapshot = readImageSnapshot(path);
    auto decoded = decodeDdsTexture(snapshot, path);
    return {.dimension = decoded.dimension,
            .width = decoded.width,
            .height = decoded.height,
            .depth = decoded.depth,
            .arrayLayers = decoded.arrayLayers,
            .mipLevels = decoded.mipLevels,
            .subresources = std::move(decoded.subresources)};
}

TextureImage loadTextureImage(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension == ".dds")
        return decodeDdsTexture(readImageSnapshot(path), path, true);
    TextureImage result;
    if (extension == ".hdr") {
        auto hdr = loadImageData(path);
        result.width = hdr.width;
        result.height = hdr.height;
        result.format = "R32G32B32A32_FLOAT";
        result.subresources.push_back({hdr.width, hdr.height, 1, std::move(hdr.bytes)});
    } else {
        auto image = loadImageRgba8(path);
        result.width = image.width;
        result.height = image.height;
        result.subresources.push_back({image.width, image.height, 1, std::move(image.pixels)});
    }
    return result;
}

ImageRgba8 loadImageRgba8(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(),
                           [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (extension == ".dds")
        return decodeDds(path);
    const auto snapshot = readImageSnapshot(path);
    int infoWidth = 0;
    int infoHeight = 0;
    int infoChannels = 0;
    if (!stbi_info_from_memory(snapshot.data(), static_cast<int>(snapshot.size()), &infoWidth, &infoHeight,
                               &infoChannels))
        throw std::runtime_error("cannot inspect image " + path.string() + ": " + stbi_failure_reason());
    if (infoWidth <= 0 || infoHeight <= 0)
        throw std::runtime_error("invalid image dimensions: " + path.string());
    const auto expectedBytes =
        checkedRgbaBytes(static_cast<std::uint32_t>(infoWidth), static_cast<std::uint32_t>(infoHeight), "image");
    const auto decodedAndCopiedBytes = checkedMultiply(expectedBytes, 2U, "image decoded buffers");
    checkPeakAllocation(snapshot.size(), decodedAndCopiedBytes, "image");
    int width = 0;
    int height = 0;
    int channels = 0;
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> decoded(
        stbi_load_from_memory(snapshot.data(), static_cast<int>(snapshot.size()), &width, &height, &channels,
                              STBI_rgb_alpha),
        &stbi_image_free);
    if (decoded == nullptr) {
        throw std::runtime_error("cannot decode image " + path.string() + ": " + stbi_failure_reason());
    }
    if (width <= 0 || height <= 0)
        throw std::runtime_error("invalid image dimensions: " + path.string());
    const auto actualBytes =
        checkedRgbaBytes(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), "image");
    if (actualBytes != expectedBytes)
        throw std::runtime_error("image dimensions changed during decode: " + path.string());
    checkPeakAllocation(snapshot.size(), checkedMultiply(actualBytes, 2U, "image decoded buffers"), "image");
    ImageRgba8 image{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
                     std::vector<std::uint8_t>(decoded.get(), decoded.get() + static_cast<std::size_t>(actualBytes))};
    return image;
}

ImageData loadImageData(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(),
                           [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (extension != ".hdr") {
        const auto ldr = loadImageRgba8(path);
        return rgba8ToHalf(ldr, ColorSpace::srgb);
    }

    const auto snapshot = readImageSnapshot(path);
    int infoWidth = 0;
    int infoHeight = 0;
    int infoChannels = 0;
    if (!stbi_info_from_memory(snapshot.data(), static_cast<int>(snapshot.size()), &infoWidth, &infoHeight,
                               &infoChannels))
        throw std::runtime_error("cannot inspect HDR image " + path.string() + ": " + stbi_failure_reason());
    if (infoWidth <= 0 || infoHeight <= 0)
        throw std::runtime_error("invalid HDR image dimensions: " + path.string());
    const auto pixels = checkedMultiply(static_cast<std::uint64_t>(infoWidth), static_cast<std::uint64_t>(infoHeight),
                                        "HDR image pixels");
    const auto decodedBytes = checkedMultiply(pixels, 4U * sizeof(float), "HDR image bytes");
    if (decodedBytes > std::numeric_limits<std::size_t>::max())
        throw std::runtime_error("HDR image exceeds addressable memory: " + path.string());
    const auto decodedAndCopiedBytes = checkedMultiply(decodedBytes, 2U, "HDR image decoded buffers");
    checkPeakAllocation(snapshot.size(), decodedAndCopiedBytes, "HDR image");
    int width = 0;
    int height = 0;
    int channels = 0;
    std::unique_ptr<float, decltype(&stbi_image_free)> decoded(
        stbi_loadf_from_memory(snapshot.data(), static_cast<int>(snapshot.size()), &width, &height, &channels, 4),
        &stbi_image_free);
    if (decoded == nullptr)
        throw std::runtime_error("cannot decode HDR image " + path.string() + ": " + stbi_failure_reason());
    if (width != infoWidth || height != infoHeight)
        throw std::runtime_error("HDR image dimensions changed during decode: " + path.string());
    checkPeakAllocation(snapshot.size(), checkedMultiply(decodedBytes, 2U, "HDR image decoded buffers"), "HDR image");
    ImageData result{.width = static_cast<std::uint32_t>(width),
                     .height = static_cast<std::uint32_t>(height),
                     .channels = 4,
                     .type = PixelType::float32,
                     .space = ColorSpace::linear,
                     .bytes = std::vector<std::uint8_t>(static_cast<std::size_t>(decodedBytes))};
    std::memcpy(result.bytes.data(), decoded.get(), result.bytes.size());
    return result;
}

} // namespace dayo::core
