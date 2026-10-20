#include "core/image.hpp"
#include "test_assets.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
using Color = std::array<std::uint8_t, 4>;
constexpr std::array<std::array<std::uint8_t, 8>, 2> alphaPalettes{
    {{200, 100, 185, 171, 157, 142, 128, 114}, {20, 120, 40, 60, 80, 100, 0, 255}}};
constexpr std::array<std::array<float, 8>, 2> signedPalettes{
    {{1, -1, 5.0F / 7, 3.0F / 7, 1.0F / 7, -1.0F / 7, -3.0F / 7, -5.0F / 7}, {-1, 1, -0.6F, -0.2F, 0.2F, 0.6F, -1, 1}}};

void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void alphaBlock(std::uint8_t* block, unsigned palette, bool reverse, bool signedValues) {
    block[0] = signedValues ? (palette == 0 ? 127 : 128) : alphaPalettes[palette][0];
    block[1] = signedValues ? (palette == 0 ? 128 : 127) : alphaPalettes[palette][1];
    std::uint64_t bits = 0;
    for (unsigned p = 0; p < 16; ++p)
        bits |= static_cast<std::uint64_t>(reverse ? 7 - p % 8 : p % 8) << (p * 3);
    for (unsigned byte = 0; byte < 6; ++byte)
        block[2 + byte] = static_cast<std::uint8_t>(bits >> (byte * 8));
}

void blockColors(const std::filesystem::path& path) {
    constexpr std::array<std::uint32_t, 7> codes{0x31545844, 0x33545844, 0x35545844, 0x31495441,
                                                 0x32495441, 0x53344342, 0x53354342};
    for (unsigned format = 0; format < codes.size(); ++format)
        for (std::uint32_t width : {1U, 3U, 4U, 7U, 8U})
            for (std::uint32_t height : {1U, 4U, 5U, 9U}) {
                const bool signedValues = format >= 5;
                const auto blockSize = format == 0 || format == 3 || format == 5 ? 8U : 16U;
                const auto blocksWide = (width + 3) / 4;
                const auto blocksHigh = (height + 3) / 4;
                std::vector<std::uint8_t> payload(blocksWide * blocksHigh * blockSize);
                for (unsigned by = 0; by < blocksHigh; ++by)
                    for (unsigned bx = 0; bx < blocksWide; ++bx) {
                        const auto flip = (bx + by) % 2;
                        auto* block = payload.data() + (by * blocksWide + bx) * blockSize;
                        if (format >= 3) {
                            alphaBlock(block, flip, false, signedValues);
                            if (blockSize == 16)
                                alphaBlock(block + 8, 1 - flip, true, signedValues);
                        } else {
                            auto* color = format == 0 ? block : block + 8;
                            const std::array<std::uint8_t, 4> endpoints =
                                flip == 0 ? std::array<std::uint8_t, 4>{0, 248, 31, 0}
                                          : std::array<std::uint8_t, 4>{31, 0, 0, 248};
                            std::copy(endpoints.begin(), endpoints.end(), color);
                            std::fill_n(color + 4, 4, 0xe4);
                            if (format == 1)
                                for (unsigned pixel = 0; pixel < 16; pixel += 2)
                                    block[pixel / 2] = static_cast<std::uint8_t>(pixel | ((pixel + 1) << 4));
                            if (format == 2)
                                alphaBlock(block, flip, false, false);
                        }
                    }
                dayo::test::writeDds(path, {.width = width, .height = height, .fourCc = codes[format]}, payload);
                const auto image = dayo::core::loadTextureImage(path);
                require(image.width == width && image.height == height && image.subresources.size() == 1,
                        "compressed dimensions or subresource count changed");
                const auto& bytes = image.subresource(0).pixels;
                for (unsigned y = 0; y < height; ++y)
                    for (unsigned x = 0; x < width; ++x) {
                        const auto flip = (x / 4 + y / 4) % 2;
                        const auto pixel = (y % 4) * 4 + x % 4;
                        if (signedValues) {
                            const auto channels = format == 5 ? 1U : 2U;
                            require(image.format == (channels == 1 ? "R32_FLOAT" : "R32G32_FLOAT") &&
                                        bytes.size() ==
                                            static_cast<std::size_t>(width) * height * channels * sizeof(float),
                                    "signed BC formats lost their float component layout");
                            for (unsigned channel = 0; channel < channels; ++channel) {
                                float actual{};
                                const auto offset = ((y * width + x) * channels + channel) * sizeof(float);
                                std::memcpy(&actual, bytes.data() + offset, sizeof(float));
                                const auto expected = signedPalettes[channel == 0 ? flip : 1 - flip]
                                                                    [channel == 0 ? pixel % 8 : 7 - pixel % 8];
                                require(std::abs(actual - expected) < 1e-7F,
                                        "signed BC endpoint/selector/interpolation mismatch");
                            }
                            continue;
                        }
                        require(image.format == "R8G8B8A8_UNORM", "BC UNORM output format changed");
                        Color expected;
                        if (format >= 3) {
                            const auto r = alphaPalettes[flip][pixel % 8];
                            expected = format == 3 ? Color{r, r, r, 255}
                                                   : Color{r, alphaPalettes[1 - flip][7 - pixel % 8], 255, 255};
                        } else {
                            const std::array<Color, 4> redFirst{
                                {{255, 0, 0, 255}, {0, 0, 255, 255}, {170, 0, 85, 255}, {85, 0, 170, 255}}};
                            const std::array<Color, 4> blueFirst{
                                {{0, 0, 255, 255}, {255, 0, 0, 255}, {85, 0, 170, 255}, {170, 0, 85, 255}}};
                            expected = (flip == 0 ? redFirst : blueFirst)[pixel % 4];
                            if (format == 0 && flip != 0 && pixel % 4 >= 2)
                                expected = pixel % 4 == 2 ? Color{127, 0, 127, 255} : Color{0, 0, 0, 0};
                            if (format == 1)
                                expected[3] = static_cast<std::uint8_t>(pixel * 17);
                            if (format == 2)
                                expected[3] = alphaPalettes[flip][pixel % 8];
                        }
                        require(std::equal(expected.begin(), expected.end(), bytes.begin() + (y * width + x) * 4),
                                "BC color/alpha mismatch at full or clipped block edge");
                    }
                payload.pop_back();
                dayo::test::writeDds(path, {.width = width, .height = height, .fourCc = codes[format]}, payload);
                bool truncated = false;
                try {
                    static_cast<void>(dayo::core::loadTextureImage(path));
                } catch (const std::runtime_error&) {
                    truncated = true;
                }
                require(truncated, "truncated block payload was accepted");
            }
}
} // namespace
int main() {
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("dayo-dds-blocks-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    bool created = false;
    try {
        created = std::filesystem::create_directory(directory);
        require(created, "cannot create block test directory");
        blockColors(directory / "blocks.dds");
        std::filesystem::remove_all(directory);
    } catch (const std::exception& error) {
        if (created)
            std::filesystem::remove_all(directory);
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
