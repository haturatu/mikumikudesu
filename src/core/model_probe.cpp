#include "core/model_probe.hpp"
#include "core/mapped_file.hpp"
#include "core/parse_budget.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace dayo::core {
namespace {

constexpr std::int32_t maxElements = 300'000'000;

template <typename T> T read(std::istream& input, std::string_view field) {
    static_assert(std::is_trivially_copyable_v<T>);
    T value{};
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!input)
        throw std::runtime_error("truncated PMX while reading " + std::string(field));
    return value;
}

template <std::size_t N> std::array<float, N> readFloatArray(std::istream& input, std::string_view field) {
    std::array<float, N> value{};
    input.read(reinterpret_cast<char*>(value.data()), static_cast<std::streamsize>(sizeof(value)));
    if (!input)
        throw std::runtime_error("truncated PMX while reading " + std::string(field));
    return value;
}

std::int32_t readCount(MappedFileStream& input, ParseBudget& budget, std::string_view field,
                       std::int32_t maximum = maxElements, std::uint64_t minimumRecordBytes = 1,
                       std::uint64_t decodedBytesPerElement = 0) {
    const auto count = read<std::int32_t>(input, field);
    if (count < 0 || count > maximum)
        throw std::runtime_error("invalid PMX " + std::string(field));
    budget.checkCount(static_cast<std::uint64_t>(count), static_cast<std::uint64_t>(maximum), minimumRecordBytes,
                      input.remaining(), decodedBytesPerElement, field);
    return count;
}

std::string utf16LeToUtf8(std::string_view bytes) {
    std::string output;
    output.reserve(bytes.size());
    for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
        const auto lo = static_cast<unsigned char>(bytes[i]);
        const auto hi = static_cast<unsigned char>(bytes[i + 1]);
        std::uint32_t codepoint = static_cast<std::uint32_t>(lo) | (static_cast<std::uint32_t>(hi) << 8U);
        if (codepoint >= 0xD800U && codepoint <= 0xDBFFU && i + 3 < bytes.size()) {
            const auto lo2 = static_cast<unsigned char>(bytes[i + 2]);
            const auto hi2 = static_cast<unsigned char>(bytes[i + 3]);
            const auto trail = static_cast<std::uint32_t>(lo2) | (static_cast<std::uint32_t>(hi2) << 8U);
            if (trail >= 0xDC00U && trail <= 0xDFFFU) {
                codepoint = 0x10000U + ((codepoint - 0xD800U) << 10U) + trail - 0xDC00U;
                i += 2;
            }
        }
        if (codepoint <= 0x7FU)
            output.push_back(static_cast<char>(codepoint));
        else if (codepoint <= 0x7FFU) {
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

std::string readText(MappedFileStream& input, std::uint8_t encoding, ParseBudget& budget) {
    const auto size = readCount(input, budget, "text length", 16 * 1024 * 1024, 1, 1);
    budget.accountInputBytes(static_cast<std::uint64_t>(size), input.remaining(), "text");
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), size);
    if (!input)
        throw std::runtime_error("truncated PMX text");
    return encoding == 0 ? utf16LeToUtf8(bytes) : bytes;
}

std::int32_t readSignedIndex(std::istream& input, std::uint8_t size, std::string_view field) {
    switch (size) {
    case 1:
        return read<std::int8_t>(input, field);
    case 2:
        return read<std::int16_t>(input, field);
    case 4:
        return read<std::int32_t>(input, field);
    default:
        throw std::runtime_error("invalid PMX index size");
    }
}

std::uint32_t readVertexIndex(std::istream& input, std::uint8_t size) {
    switch (size) {
    case 1:
        return read<std::uint8_t>(input, "vertex index");
    case 2:
        return read<std::uint16_t>(input, "vertex index");
    case 4:
        return read<std::uint32_t>(input, "vertex index");
    default:
        throw std::runtime_error("invalid PMX vertex index size");
    }
}

struct Header {
    PmxMetadata metadata;
    std::array<std::uint8_t, 8> settings{};
};

Header readHeader(MappedFileStream& input, ParseBudget& budget, const std::filesystem::path& path) {
    std::array<char, 4> magic{};
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!input || std::string_view(magic.data(), magic.size()) != "PMX ") {
        throw std::runtime_error("not a PMX file: " + path.string());
    }
    Header result;
    result.metadata.version = read<float>(input, "version");
    if (result.metadata.version < 2.0F || result.metadata.version > 2.1F) {
        throw std::runtime_error("unsupported PMX version");
    }
    const auto headerSize = read<std::uint8_t>(input, "header size");
    if (headerSize != result.settings.size())
        throw std::runtime_error("unsupported PMX global header size");
    input.read(reinterpret_cast<char*>(result.settings.data()), static_cast<std::streamsize>(result.settings.size()));
    if (!input)
        throw std::runtime_error("truncated PMX header");
    result.metadata.textEncoding = result.settings[0];
    result.metadata.additionalUvCount = result.settings[1];
    if (result.metadata.textEncoding > 1 || result.metadata.additionalUvCount > 4) {
        throw std::runtime_error("invalid PMX global settings");
    }
    for (std::size_t i = 2; i < result.settings.size(); ++i) {
        if (result.settings[i] != 1 && result.settings[i] != 2 && result.settings[i] != 4) {
            throw std::runtime_error("invalid PMX index size");
        }
    }
    result.metadata.modelName = readText(input, result.metadata.textEncoding, budget);
    result.metadata.englishName = readText(input, result.metadata.textEncoding, budget);
    result.metadata.comment = readText(input, result.metadata.textEncoding, budget);
    result.metadata.englishComment = readText(input, result.metadata.textEncoding, budget);
    result.metadata.vertexCount = readCount(input, budget, "vertex count", 10'000'000, 38, sizeof(PmxVertex));
    return result;
}

void readVertices(std::istream& input, const Header& header, PmxModel& model) {
    model.vertices.resize(static_cast<std::size_t>(header.metadata.vertexCount));
    const auto boneSize = header.settings[5];
    for (auto& vertex : model.vertices) {
        vertex.position = readFloatArray<3>(input, "position");
        vertex.normal = readFloatArray<3>(input, "normal");
        vertex.uv = readFloatArray<2>(input, "uv");
        for (std::uint8_t i = 0; i < header.metadata.additionalUvCount; ++i) {
            vertex.additionalUv[i] = readFloatArray<4>(input, "additional uv");
        }
        const auto type = read<std::uint8_t>(input, "weight type");
        if (type > 4 || (type == 4 && header.metadata.version < 2.1F)) {
            throw std::runtime_error("invalid PMX weight type");
        }
        vertex.weightType = static_cast<PmxWeightType>(type);
        const auto bone = [&] { return readSignedIndex(input, boneSize, "bone index"); };
        switch (vertex.weightType) {
        case PmxWeightType::bdef1:
            vertex.bones[0] = bone();
            break;
        case PmxWeightType::bdef2:
            vertex.bones[0] = bone();
            vertex.bones[1] = bone();
            vertex.weights[0] = read<float>(input, "BDEF2 weight");
            vertex.weights[1] = 1.0F - vertex.weights[0];
            break;
        case PmxWeightType::bdef4:
        case PmxWeightType::qdef:
            for (auto& index : vertex.bones)
                index = bone();
            vertex.weights = readFloatArray<4>(input, "BDEF4/QDEF weights");
            break;
        case PmxWeightType::sdef:
            vertex.bones[0] = bone();
            vertex.bones[1] = bone();
            vertex.weights[0] = read<float>(input, "SDEF weight");
            vertex.weights[1] = 1.0F - vertex.weights[0];
            vertex.sdefC = readFloatArray<3>(input, "SDEF C");
            vertex.sdefR0 = readFloatArray<3>(input, "SDEF R0");
            vertex.sdefR1 = readFloatArray<3>(input, "SDEF R1");
            break;
        }
        vertex.edgeScale = read<float>(input, "edge scale");
    }
}

void readMaterials(MappedFileStream& input, const Header& header, ParseBudget& budget, PmxModel& model) {
    const auto textureCount = readCount(input, budget, "texture count", 1'000'000, 4, sizeof(std::filesystem::path));
    model.textures.reserve(static_cast<std::size_t>(textureCount));
    for (std::int32_t i = 0; i < textureCount; ++i) {
        auto value = readText(input, header.metadata.textEncoding, budget);
        // PMX files authored on Windows commonly store texture paths with
        // backslashes. Normalize them before constructing a native path so
        // the same archive resolves correctly on Linux and other POSIX hosts.
        std::replace(value.begin(), value.end(), '\\', '/');
        const auto* utf8 = reinterpret_cast<const char8_t*>(value.c_str());
        model.textures.push_back((model.sourcePath.parent_path() / std::filesystem::path(utf8)).lexically_normal());
    }
    const auto count = readCount(input, budget, "material count", 1'000'000, 85, sizeof(PmxMaterial));
    model.materials.resize(static_cast<std::size_t>(count));
    std::uint64_t coveredIndices = 0;
    for (auto& material : model.materials) {
        material.name = readText(input, header.metadata.textEncoding, budget);
        material.englishName = readText(input, header.metadata.textEncoding, budget);
        material.diffuse = readFloatArray<4>(input, "material diffuse");
        material.specular = readFloatArray<3>(input, "material specular");
        material.shininess = read<float>(input, "material shininess");
        material.ambient = readFloatArray<3>(input, "material ambient");
        material.drawFlags = read<std::uint8_t>(input, "material draw flags");
        material.edgeColor = readFloatArray<4>(input, "material edge color");
        material.edgeSize = read<float>(input, "material edge size");
        material.textureIndex = readSignedIndex(input, header.settings[3], "texture index");
        material.sphereTextureIndex = readSignedIndex(input, header.settings[3], "sphere texture index");
        material.sphereMode = read<std::uint8_t>(input, "sphere mode");
        material.toonMode = read<std::uint8_t>(input, "toon mode");
        material.toonTextureIndex = material.toonMode == 0
                                        ? readSignedIndex(input, header.settings[3], "toon texture index")
                                        : static_cast<std::int32_t>(read<std::uint8_t>(input, "shared toon index"));
        material.memo = readText(input, header.metadata.textEncoding, budget);
        const auto indexCount =
            readCount(input, budget, "material index count", maxElements, header.settings[2], sizeof(std::uint32_t));
        material.indexCount = static_cast<std::uint32_t>(indexCount);
        coveredIndices += material.indexCount;
    }
    if (coveredIndices != model.indices.size())
        throw std::runtime_error("PMX material ranges do not cover indices");
}

void readBones(MappedFileStream& input, const Header& header, ParseBudget& budget, PmxModel& model) {
    const auto count = readCount(input, budget, "bone count", 100'000, 40, sizeof(PmxBone));
    model.bones.resize(static_cast<std::size_t>(count));
    for (auto& bone : model.bones) {
        bone.name = readText(input, header.metadata.textEncoding, budget);
        bone.englishName = readText(input, header.metadata.textEncoding, budget);
        bone.position = readFloatArray<3>(input, "bone position");
        bone.parent = readSignedIndex(input, header.settings[5], "parent bone");
        bone.deformLayer = read<std::int32_t>(input, "bone layer");
        bone.flags = read<std::uint16_t>(input, "bone flags");
        if ((bone.flags & 0x0001U) != 0)
            bone.tailBone = readSignedIndex(input, header.settings[5], "tail bone");
        else
            bone.tailOffset = readFloatArray<3>(input, "tail offset");
        if ((bone.flags & 0x0300U) != 0) {
            bone.inheritParent = readSignedIndex(input, header.settings[5], "inherit bone");
            bone.inheritRatio = read<float>(input, "inherit ratio");
        }
        if ((bone.flags & 0x0400U) != 0)
            bone.fixedAxis = readFloatArray<3>(input, "fixed axis");
        if ((bone.flags & 0x0800U) != 0) {
            bone.localAxisX = readFloatArray<3>(input, "local axis x");
            bone.localAxisZ = readFloatArray<3>(input, "local axis z");
        }
        if ((bone.flags & 0x2000U) != 0)
            bone.externalParentKey = read<std::int32_t>(input, "external parent key");
        if ((bone.flags & 0x0020U) != 0) {
            bone.ikTarget = readSignedIndex(input, header.settings[5], "IK target");
            bone.ikLoopCount = read<std::int32_t>(input, "IK loop count");
            bone.ikLimitAngle = read<float>(input, "IK angle");
            const auto linkCount = readCount(input, budget, "IK link count", 1'000'000, 1, sizeof(PmxIkLink));
            bone.ikLinks.resize(static_cast<std::size_t>(linkCount));
            for (auto& link : bone.ikLinks) {
                link.bone = readSignedIndex(input, header.settings[5], "IK link bone");
                link.limited = read<std::uint8_t>(input, "IK link limit") != 0;
                if (link.limited) {
                    link.minimum = readFloatArray<3>(input, "IK minimum");
                    link.maximum = readFloatArray<3>(input, "IK maximum");
                }
            }
        }
    }
}

void readMorphs(MappedFileStream& input, const Header& header, ParseBudget& budget, PmxModel& model) {
    const auto count = readCount(input, budget, "morph count", 1'000'000, 14, sizeof(PmxMorph));
    model.morphs.resize(static_cast<std::size_t>(count));
    for (auto& morph : model.morphs) {
        morph.name = readText(input, header.metadata.textEncoding, budget);
        morph.englishName = readText(input, header.metadata.textEncoding, budget);
        morph.panel = read<std::uint8_t>(input, "morph panel");
        morph.type = read<std::uint8_t>(input, "morph type");
        if (morph.type > 10 || (morph.type >= 9 && header.metadata.version < 2.1F)) {
            throw std::runtime_error("invalid PMX morph type");
        }
        const auto offsetCount = readCount(input, budget, "morph offset count", 10'000'000, 1, sizeof(PmxMorphOffset));
        morph.offsets.resize(static_cast<std::size_t>(offsetCount));
        for (auto& offset : morph.offsets) {
            if (morph.type == 0 || morph.type == 9) {
                offset.index = readSignedIndex(input, header.settings[6], "group morph index");
                offset.scalar = read<float>(input, "group morph weight");
            } else if (morph.type == 1) {
                offset.index = static_cast<std::int32_t>(readVertexIndex(input, header.settings[2]));
                offset.vector3 = readFloatArray<3>(input, "vertex morph offset");
            } else if (morph.type == 2) {
                offset.index = readSignedIndex(input, header.settings[5], "bone morph index");
                offset.vector3 = readFloatArray<3>(input, "bone morph translation");
                offset.vector4 = readFloatArray<4>(input, "bone morph rotation");
            } else if (morph.type >= 3 && morph.type <= 7) {
                offset.index = static_cast<std::int32_t>(readVertexIndex(input, header.settings[2]));
                offset.vector4 = readFloatArray<4>(input, "UV morph offset");
            } else if (morph.type == 8) {
                offset.index = readSignedIndex(input, header.settings[4], "material morph index");
                offset.operation = read<std::uint8_t>(input, "material morph operation");
                offset.materialVectors[0] = readFloatArray<4>(input, "morph diffuse");
                const auto specular = readFloatArray<3>(input, "morph specular");
                offset.materialVectors[1] = {specular[0], specular[1], specular[2], read<float>(input, "morph power")};
                const auto ambient = readFloatArray<3>(input, "morph ambient");
                offset.materialVectors[2] = {ambient[0], ambient[1], ambient[2], 0.0F};
                offset.materialVectors[3] = readFloatArray<4>(input, "morph edge color");
                offset.materialVectors[2][3] = read<float>(input, "morph edge size");
                offset.materialVectors[4] = readFloatArray<4>(input, "morph texture");
                offset.materialVectors[5] = readFloatArray<4>(input, "morph sphere");
                offset.materialVectors[6] = readFloatArray<4>(input, "morph toon");
            } else {
                offset.index = readSignedIndex(input, header.settings[7], "impulse rigid body");
                offset.local = read<std::uint8_t>(input, "impulse local flag") != 0;
                offset.vector3 = readFloatArray<3>(input, "impulse velocity");
                offset.tertiaryVector3 = readFloatArray<3>(input, "impulse torque");
            }
        }
    }
}

void readDisplayFrames(MappedFileStream& input, const Header& header, ParseBudget& budget, PmxModel& model) {
    const auto count = readCount(input, budget, "display frame count", 1'000'000, 13, sizeof(PmxDisplayFrame));
    model.displayFrames.resize(static_cast<std::size_t>(count));
    for (auto& frame : model.displayFrames) {
        frame.name = readText(input, header.metadata.textEncoding, budget);
        frame.englishName = readText(input, header.metadata.textEncoding, budget);
        frame.special = read<std::uint8_t>(input, "display frame special") != 0;
        const auto itemCount = readCount(input, budget, "display item count", 10'000'000, 1, sizeof(PmxDisplayItem));
        frame.items.resize(static_cast<std::size_t>(itemCount));
        for (auto& item : frame.items) {
            item.bone = read<std::uint8_t>(input, "display item type") == 0;
            item.index =
                readSignedIndex(input, item.bone ? header.settings[5] : header.settings[6], "display item index");
        }
    }
}

void readPhysics(MappedFileStream& input, const Header& header, ParseBudget& budget, PmxModel& model) {
    const auto bodyCount = readCount(input, budget, "rigid body count", 1'000'000, 70, sizeof(PmxRigidBody));
    model.rigidBodies.resize(static_cast<std::size_t>(bodyCount));
    for (auto& body : model.rigidBodies) {
        body.name = readText(input, header.metadata.textEncoding, budget);
        body.englishName = readText(input, header.metadata.textEncoding, budget);
        body.bone = readSignedIndex(input, header.settings[5], "rigid body bone");
        body.group = read<std::uint8_t>(input, "rigid body group");
        body.collisionMask = read<std::uint16_t>(input, "rigid body mask");
        body.shape = read<std::uint8_t>(input, "rigid body shape");
        body.size = readFloatArray<3>(input, "rigid body size");
        body.position = readFloatArray<3>(input, "rigid body position");
        body.rotation = readFloatArray<3>(input, "rigid body rotation");
        body.mass = read<float>(input, "rigid body mass");
        body.linearDamping = read<float>(input, "rigid body linear damping");
        body.angularDamping = read<float>(input, "rigid body angular damping");
        body.restitution = read<float>(input, "rigid body restitution");
        body.friction = read<float>(input, "rigid body friction");
        body.mode = read<std::uint8_t>(input, "rigid body mode");
        const auto finite = [](const auto& values) {
            return std::ranges::all_of(values, [](const float value) { return std::isfinite(value); });
        };
        if (!finite(body.size) || !finite(body.position) || !finite(body.rotation) || !std::isfinite(body.mass) ||
            !std::isfinite(body.linearDamping) || !std::isfinite(body.angularDamping) ||
            !std::isfinite(body.restitution) || !std::isfinite(body.friction))
            throw std::runtime_error("invalid PMX rigid body numeric value");
    }
    const auto jointCount = readCount(input, budget, "joint count", 1'000'000, 107, sizeof(PmxJoint));
    model.joints.resize(static_cast<std::size_t>(jointCount));
    for (auto& joint : model.joints) {
        joint.name = readText(input, header.metadata.textEncoding, budget);
        joint.englishName = readText(input, header.metadata.textEncoding, budget);
        joint.type = read<std::uint8_t>(input, "joint type");
        joint.bodyA = readSignedIndex(input, header.settings[7], "joint body A");
        joint.bodyB = readSignedIndex(input, header.settings[7], "joint body B");
        joint.position = readFloatArray<3>(input, "joint position");
        joint.rotation = readFloatArray<3>(input, "joint rotation");
        joint.translationMinimum = readFloatArray<3>(input, "joint translation minimum");
        joint.translationMaximum = readFloatArray<3>(input, "joint translation maximum");
        joint.rotationMinimum = readFloatArray<3>(input, "joint rotation minimum");
        joint.rotationMaximum = readFloatArray<3>(input, "joint rotation maximum");
        joint.translationSpring = readFloatArray<3>(input, "joint translation spring");
        joint.rotationSpring = readFloatArray<3>(input, "joint rotation spring");
    }
}

void readSoftBodies(MappedFileStream& input, const Header& header, ParseBudget& budget, PmxModel& model) {
    if (header.metadata.version < 2.1F || input.peek() == std::char_traits<char>::eof())
        return;
    const auto count = readCount(input, budget, "soft body count", 1'000'000, 138, sizeof(PmxSoftBody));
    model.softBodies.resize(static_cast<std::size_t>(count));
    for (auto& body : model.softBodies) {
        body.name = readText(input, header.metadata.textEncoding, budget);
        body.englishName = readText(input, header.metadata.textEncoding, budget);
        body.shape = read<std::uint8_t>(input, "soft body shape");
        body.material = readSignedIndex(input, header.settings[4], "soft body material");
        body.group = read<std::uint8_t>(input, "soft body group");
        body.collisionMask = read<std::uint16_t>(input, "soft body mask");
        body.flags = read<std::uint8_t>(input, "soft body flags");
        body.bendingLinkDistance = read<std::int32_t>(input, "soft body bending distance");
        body.clusterCount = read<std::int32_t>(input, "soft body cluster count");
        body.totalMass = read<float>(input, "soft body mass");
        body.collisionMargin = read<float>(input, "soft body margin");
        body.aeroModel = read<std::int32_t>(input, "soft body aero model");
        body.config = readFloatArray<12>(input, "soft body config");
        body.cluster = readFloatArray<6>(input, "soft body cluster");
        for (auto& value : body.iteration)
            value = read<std::int32_t>(input, "soft body iteration");
        body.materialConfig = readFloatArray<3>(input, "soft body material config");
        const auto anchorCount =
            readCount(input, budget, "soft body anchor count", 10'000'000, 1, sizeof(PmxSoftBodyAnchor));
        body.anchors.resize(static_cast<std::size_t>(anchorCount));
        for (auto& anchor : body.anchors) {
            anchor.rigidBody = readSignedIndex(input, header.settings[7], "soft body anchor rigid body");
            anchor.vertex = static_cast<std::int32_t>(readVertexIndex(input, header.settings[2]));
            anchor.nearMode = read<std::uint8_t>(input, "soft body anchor near mode") != 0;
        }
        const auto pinCount =
            readCount(input, budget, "soft body pin count", 10'000'000, header.settings[2], sizeof(std::int32_t));
        body.pinnedVertices.resize(static_cast<std::size_t>(pinCount));
        for (auto& vertex : body.pinnedVertices)
            vertex = static_cast<std::int32_t>(readVertexIndex(input, header.settings[2]));
    }
}

} // namespace

PmxMetadata probePmx(const std::filesystem::path& path) {
    MappedFileStream input(path);
    ParseBudget budget;
    return readHeader(input, budget, path).metadata;
}

PmxModel loadPmxModel(const std::filesystem::path& path) {
    MappedFileStream input(path);
    ParseBudget budget;
    const auto header = readHeader(input, budget, path);
    PmxModel model;
    model.metadata = header.metadata;
    model.sourcePath = path;
    readVertices(input, header, model);
    const auto indexCount =
        readCount(input, budget, "index count", maxElements, header.settings[2], sizeof(std::uint32_t));
    if (indexCount % 3 != 0)
        throw std::runtime_error("PMX index count is not divisible by three");
    model.indices.resize(static_cast<std::size_t>(indexCount));
    for (auto& index : model.indices) {
        index = readVertexIndex(input, header.settings[2]);
        if (index >= model.vertices.size())
            throw std::runtime_error("PMX vertex index out of range");
    }
    readMaterials(input, header, budget, model);
    readBones(input, header, budget, model);
    readMorphs(input, header, budget, model);
    readDisplayFrames(input, header, budget, model);
    readPhysics(input, header, budget, model);
    readSoftBodies(input, header, budget, model);
    return model;
}

PmxMesh loadPmxMesh(const std::filesystem::path& path) {
    auto model = loadPmxModel(path);
    PmxMesh mesh{.metadata = std::move(model.metadata),
                 .vertices = std::move(model.vertices),
                 .indices = std::move(model.indices)};
    if (mesh.vertices.empty())
        return mesh;
    auto minimum = mesh.vertices.front().position;
    auto maximum = minimum;
    for (const auto& vertex : mesh.vertices) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            minimum[axis] = std::min(minimum[axis], vertex.position[axis]);
            maximum[axis] = std::max(maximum[axis], vertex.position[axis]);
        }
    }
    const Float3 center{(minimum[0] + maximum[0]) * 0.5F, (minimum[1] + maximum[1]) * 0.5F,
                        (minimum[2] + maximum[2]) * 0.5F};
    const float scale =
        1.8F / std::max({maximum[0] - minimum[0], maximum[1] - minimum[1], maximum[2] - minimum[2], 0.001F});
    for (auto& vertex : mesh.vertices) {
        for (std::size_t axis = 0; axis < 3; ++axis)
            vertex.position[axis] = (vertex.position[axis] - center[axis]) * scale;
    }
    return mesh;
}

} // namespace dayo::core
