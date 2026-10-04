#include "test_assets.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace dayo::test {

void putLe32(std::span<std::uint8_t> output, std::size_t offset, std::uint32_t value) {
    if (offset > output.size() || output.size() - offset < 4)
        throw std::out_of_range("fixture header field exceeds output");
    for (std::size_t byte = 0; byte < 4; ++byte)
        output[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8));
}

std::vector<std::uint8_t> makeDdsHeader(const DdsFixtureDesc& desc) {
    std::vector<std::uint8_t> header(desc.dxgiFormat ? 148 : 128);
    std::copy_n("DDS ", 4, header.begin());
    const auto put = [&](std::size_t offset, std::uint32_t value) { putLe32(header, offset, value); };
    put(4, 124);
    put(12, desc.height);
    put(16, desc.width);
    put(24, desc.depth);
    put(28, desc.mipLevels);
    put(76, 32);
    put(80, desc.pixelFlags);
    put(84, desc.dxgiFormat ? 0x30315844U : desc.fourCc);
    put(88, desc.rgbBits);
    put(92, desc.redMask);
    put(96, desc.greenMask);
    put(100, desc.blueMask);
    put(104, desc.alphaMask);
    put(108, desc.caps);
    put(112, desc.caps2);
    if (desc.dxgiFormat) {
        put(128, *desc.dxgiFormat);
        put(132, desc.dimension);
        put(136, desc.miscFlag);
        put(140, desc.arraySize);
    }
    return header;
}

void writeDds(const std::filesystem::path& path, const DdsFixtureDesc& desc, std::span<const std::uint8_t> payload) {
    const auto header = makeDdsHeader(desc);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (!payload.empty())
        output.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
}

void writePpm(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
              std::span<const std::uint8_t> rgb) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output << "P6\n" << width << ' ' << height << "\n255\n";
    output.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
}

} // namespace dayo::test
