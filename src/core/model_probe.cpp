#include "core/model_probe.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace dayo::core {
namespace {

template <typename T>
T read(std::istream& input, std::string_view field) {
    static_assert(std::is_trivially_copyable_v<T>);
    T result {};
    input.read(reinterpret_cast<char*>(&result), sizeof(result));
    if (!input) throw std::runtime_error("truncated PMX while reading " + std::string(field));
    return result;
}

std::string utf16LeToUtf8(std::string_view bytes) {
    std::string output;
    output.reserve(bytes.size());
    for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
        const auto lo = static_cast<unsigned char>(bytes[i]);
        const auto hi = static_cast<unsigned char>(bytes[i + 1]);
        std::uint32_t codepoint = static_cast<std::uint32_t>(lo)
                                | (static_cast<std::uint32_t>(hi) << 8U);
        if (codepoint >= 0xD800U && codepoint <= 0xDBFFU && i + 3 < bytes.size()) {
            const auto lo2 = static_cast<unsigned char>(bytes[i + 2]);
            const auto hi2 = static_cast<unsigned char>(bytes[i + 3]);
            const std::uint32_t trail = static_cast<std::uint32_t>(lo2)
                                      | (static_cast<std::uint32_t>(hi2) << 8U);
            if (trail >= 0xDC00U && trail <= 0xDFFFU) {
                codepoint = 0x10000U + ((codepoint - 0xD800U) << 10U) + (trail - 0xDC00U);
                i += 2;
            }
        }
        if (codepoint <= 0x7FU) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FFU) {
            output.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else if (codepoint <= 0xFFFFU) {
            output.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else {
            output.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        }
    }
    return output;
}

std::string readText(std::istream& input, std::uint8_t encoding) {
    const auto size = read<std::int32_t>(input, "text length");
    constexpr std::int32_t maxTextSize = 16 * 1024 * 1024;
    if (size < 0 || size > maxTextSize) throw std::runtime_error("invalid PMX text length");
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), size);
    if (!input) throw std::runtime_error("truncated PMX text");
    return encoding == 0 ? utf16LeToUtf8(bytes) : bytes;
}

} // namespace

PmxMetadata probePmx(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open PMX file: " + path.string());

    std::array<char, 4> magic {};
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!input || std::string_view(magic.data(), magic.size()) != "PMX ") {
        throw std::runtime_error("not a PMX file: " + path.string());
    }

    PmxMetadata result;
    result.version = read<float>(input, "version");
    if (result.version < 2.0F || result.version > 2.2F) {
        throw std::runtime_error("unsupported PMX version");
    }

    const auto headerSize = read<std::uint8_t>(input, "header size");
    if (headerSize < 8 || headerSize > 64) throw std::runtime_error("invalid PMX header size");
    std::array<std::uint8_t, 64> header {};
    input.read(reinterpret_cast<char*>(header.data()), headerSize);
    if (!input) throw std::runtime_error("truncated PMX header");
    result.textEncoding = header[0];
    result.additionalUvCount = header[1];
    if (result.textEncoding > 1 || result.additionalUvCount > 4) {
        throw std::runtime_error("invalid PMX global settings");
    }

    result.modelName = readText(input, result.textEncoding);
    result.englishName = readText(input, result.textEncoding);
    static_cast<void>(readText(input, result.textEncoding));
    static_cast<void>(readText(input, result.textEncoding));
    result.vertexCount = read<std::int32_t>(input, "vertex count");
    if (result.vertexCount < 0) throw std::runtime_error("invalid PMX vertex count");
    return result;
}

} // namespace dayo::core

