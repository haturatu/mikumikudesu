#include "core/model_probe.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <span>
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

void skip(std::istream& input, std::streamoff bytes, std::string_view field) {
    if (bytes < 0) throw std::runtime_error("invalid PMX byte count for " + std::string(field));
    input.seekg(bytes, std::ios::cur);
    if (!input) throw std::runtime_error("truncated PMX while skipping " + std::string(field));
}

std::uint32_t readIndex(std::istream& input, std::uint8_t size) {
    switch (size) {
    case 1: return read<std::uint8_t>(input, "vertex index");
    case 2: return read<std::uint16_t>(input, "vertex index");
    case 4: return read<std::uint32_t>(input, "vertex index");
    default: throw std::runtime_error("invalid PMX vertex index size");
    }
}

struct Header {
    PmxMetadata metadata;
    std::array<std::uint8_t, 64> settings {};
};

Header readHeader(std::istream& input, const std::filesystem::path& path) {
    std::array<char, 4> magic {};
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!input || std::string_view(magic.data(), magic.size()) != "PMX ") {
        throw std::runtime_error("not a PMX file: " + path.string());
    }

    Header result;
    result.metadata.version = read<float>(input, "version");
    if (result.metadata.version < 2.0F || result.metadata.version > 2.2F) {
        throw std::runtime_error("unsupported PMX version");
    }
    const auto headerSize = read<std::uint8_t>(input, "header size");
    if (headerSize < 8 || headerSize > result.settings.size()) {
        throw std::runtime_error("invalid PMX header size");
    }
    input.read(reinterpret_cast<char*>(result.settings.data()), headerSize);
    if (!input) throw std::runtime_error("truncated PMX header");
    result.metadata.textEncoding = result.settings[0];
    result.metadata.additionalUvCount = result.settings[1];
    if (result.metadata.textEncoding > 1 || result.metadata.additionalUvCount > 4) {
        throw std::runtime_error("invalid PMX global settings");
    }
    for (std::size_t i = 2; i < 8; ++i) {
        if (result.settings[i] != 1 && result.settings[i] != 2 && result.settings[i] != 4) {
            throw std::runtime_error("invalid PMX index size");
        }
    }
    result.metadata.modelName = readText(input, result.metadata.textEncoding);
    result.metadata.englishName = readText(input, result.metadata.textEncoding);
    static_cast<void>(readText(input, result.metadata.textEncoding));
    static_cast<void>(readText(input, result.metadata.textEncoding));
    result.metadata.vertexCount = read<std::int32_t>(input, "vertex count");
    if (result.metadata.vertexCount < 0 || result.metadata.vertexCount > 100'000'000) {
        throw std::runtime_error("invalid PMX vertex count");
    }
    return result;
}

} // namespace

PmxMetadata probePmx(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open PMX file: " + path.string());

    return readHeader(input, path).metadata;
}

PmxMesh loadPmxMesh(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open PMX file: " + path.string());
    const auto header = readHeader(input, path);

    PmxMesh mesh;
    mesh.metadata = header.metadata;
    mesh.vertices.resize(static_cast<std::size_t>(mesh.metadata.vertexCount));
    const auto boneIndexSize = header.settings[5];
    for (auto& vertex : mesh.vertices) {
        input.read(reinterpret_cast<char*>(vertex.position), sizeof(vertex.position));
        input.read(reinterpret_cast<char*>(vertex.normal), sizeof(vertex.normal));
        input.read(reinterpret_cast<char*>(vertex.uv), sizeof(vertex.uv));
        if (!input) throw std::runtime_error("truncated PMX vertex data");
        skip(input, static_cast<std::streamoff>(mesh.metadata.additionalUvCount) * 4 * sizeof(float),
             "additional UVs");
        const auto weight = read<std::uint8_t>(input, "weight type");
        switch (weight) {
        case 0: // BDEF1
            skip(input, boneIndexSize, "BDEF1");
            break;
        case 1: // BDEF2
            skip(input, 2 * boneIndexSize + static_cast<std::streamoff>(sizeof(float)), "BDEF2");
            break;
        case 2: // BDEF4
        case 4: // QDEF
            skip(input, 4 * boneIndexSize + 4 * static_cast<std::streamoff>(sizeof(float)), "BDEF4/QDEF");
            break;
        case 3: // SDEF
            skip(input, 2 * boneIndexSize + 10 * static_cast<std::streamoff>(sizeof(float)), "SDEF");
            break;
        default:
            throw std::runtime_error("unsupported PMX weight type");
        }
        skip(input, sizeof(float), "edge scale");
    }

    const auto indexCount = read<std::int32_t>(input, "index count");
    if (indexCount < 0 || indexCount > 300'000'000 || indexCount % 3 != 0) {
        throw std::runtime_error("invalid PMX index count");
    }
    mesh.indices.resize(static_cast<std::size_t>(indexCount));
    const auto vertexIndexSize = header.settings[2];
    for (auto& index : mesh.indices) {
        index = readIndex(input, vertexIndexSize);
        if (index >= mesh.vertices.size()) throw std::runtime_error("PMX vertex index out of range");
    }

    if (!mesh.vertices.empty()) {
        std::array<float, 3> minimum { mesh.vertices[0].position[0], mesh.vertices[0].position[1],
                                      mesh.vertices[0].position[2] };
        auto maximum = minimum;
        for (const auto& vertex : mesh.vertices) {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                minimum[axis] = std::min(minimum[axis], vertex.position[axis]);
                maximum[axis] = std::max(maximum[axis], vertex.position[axis]);
            }
        }
        const std::array center { (minimum[0] + maximum[0]) * 0.5F,
                                  (minimum[1] + maximum[1]) * 0.5F,
                                  (minimum[2] + maximum[2]) * 0.5F };
        const float extent = std::max({ maximum[0] - minimum[0], maximum[1] - minimum[1],
                                        maximum[2] - minimum[2], 0.001F });
        const float scale = 1.8F / extent;
        for (auto& vertex : mesh.vertices) {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                vertex.position[axis] = (vertex.position[axis] - center[axis]) * scale;
            }
        }
    }
    return mesh;
}

} // namespace dayo::core
