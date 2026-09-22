#include "graphics/native_scene_data.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace dayo::graphics {
namespace {

using Vec3 = std::array<float, 3>;
using Vec2 = std::array<float, 2>;

[[nodiscard]] Vec3 subtract(const Vec3& left, const Vec3& right) noexcept {
    return {left[0] - right[0], left[1] - right[1], left[2] - right[2]};
}

[[nodiscard]] Vec2 subtract(const Vec2& left, const Vec2& right) noexcept {
    return {left[0] - right[0], left[1] - right[1]};
}

[[nodiscard]] Vec3 cross(const Vec3& left, const Vec3& right) noexcept {
    return {left[1] * right[2] - left[2] * right[1], left[2] * right[0] - left[0] * right[2],
            left[0] * right[1] - left[1] * right[0]};
}

[[nodiscard]] float dot(const Vec3& left, const Vec3& right) noexcept {
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

[[nodiscard]] Vec3 normalize(const Vec3& value, Vec3 fallback) noexcept {
    const auto lengthSquared = dot(value, value);
    if (!(lengthSquared > 1e-12F) || !std::isfinite(lengthSquared))
        return fallback;
    const auto scale = 1.0F / std::sqrt(lengthSquared);
    return {value[0] * scale, value[1] * scale, value[2] * scale};
}

template <std::size_t N> void copyArray(float (&destination)[N], const auto& source) noexcept {
    std::copy_n(source.begin(), N, std::begin(destination));
}

void copyArray4(float (&destination)[4], const std::array<float, 4>& source) noexcept {
    std::copy(source.begin(), source.end(), std::begin(destination));
}

} // namespace

NativeSceneVertex makeNativeSceneVertex(const PreviewVertex& vertex) noexcept {
    NativeSceneVertex result;
    std::copy_n(std::begin(vertex.position), 3, std::begin(result.position));
    std::copy_n(std::begin(vertex.normal), 3, std::begin(result.normal));
    std::copy_n(std::begin(vertex.uv), 2, std::begin(result.uv));
    result.edge = vertex.edgeScale;
    return result;
}

NativeSceneMaterial makeNativeSceneMaterial(const mmd::PmxMaterial& material,
                                            const mmd::AnimatedModelFrame::Material* animated) noexcept {
    NativeSceneMaterial result;
    copyArray4(result.diffuse, material.diffuse);
    copyArray(result.specular, material.specular);
    result.shininess = material.shininess;
    copyArray(result.ambient, material.ambient);
    copyArray4(result.edgeColor, material.edgeColor);
    result.edgeSize = material.edgeSize;
    result.drawFlag = material.drawFlags;
    result.tex = material.textureIndex;
    result.spTex = material.sphereTextureIndex;
    result.spmode = material.sphereMode;
    result.toonFlag = material.toonMode;
    result.toonTex = material.toonTextureIndex;
    result.vertexCount = static_cast<std::int32_t>(material.indexCount);
    if (animated == nullptr)
        return result;
    copyArray4(result.diffuse, animated->diffuse);
    copyArray(result.specular, animated->specular);
    result.shininess = animated->shininess;
    copyArray(result.ambient, animated->ambient);
    copyArray4(result.edgeColor, animated->edgeColor);
    result.edgeSize = animated->edgeSize;
    copyArray4(result.textureAddValue, animated->textureAdd);
    copyArray4(result.sphereAddValue, animated->sphereAdd);
    copyArray4(result.toonAddValue, animated->toonAdd);
    copyArray4(result.textureMulValue, animated->textureMultiply);
    copyArray4(result.sphereMulValue, animated->sphereMultiply);
    copyArray4(result.toonMulValue, animated->toonMultiply);
    return result;
}

NativeSceneModelData makeNativeSceneModelData(const mmd::PmxModel& model, std::span<const PreviewVertex> vertices,
                                              std::span<const mmd::AnimatedModelFrame::Material> animatedMaterials,
                                              mmd::PreviewNormalization normalization) {
    if (vertices.size() != model.vertices.size())
        throw std::invalid_argument("native scene vertex count does not match PMX model");
    if (model.indices.size() % 3U != 0)
        throw std::invalid_argument("native scene index count is not a triangle list");
    if (model.materials.size() != animatedMaterials.size() && !animatedMaterials.empty())
        throw std::invalid_argument("native scene animated material count does not match PMX model");

    NativeSceneModelData result;
    result.vertices.reserve(vertices.size());
    result.rawVertices.reserve(model.vertices.size());
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        auto converted = makeNativeSceneVertex(vertices[index]);
        for (std::size_t channel = 0; channel < model.vertices[index].additionalUv.size(); ++channel)
            std::copy(model.vertices[index].additionalUv[channel].begin(),
                      model.vertices[index].additionalUv[channel].end(), converted.exuv + channel * 4U);
        result.vertices.push_back(converted);

        const auto& source = model.vertices[index];
        NativeSceneVertex raw;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            raw.position[axis] = (source.position[axis] - normalization.center[axis]) * normalization.scale;
            raw.normal[axis] = source.normal[axis];
        }
        std::copy(source.uv.begin(), source.uv.end(), std::begin(raw.uv));
        raw.edge = source.edgeScale;
        for (std::size_t channel = 0; channel < source.additionalUv.size(); ++channel)
            std::copy(source.additionalUv[channel].begin(), source.additionalUv[channel].end(), raw.exuv + channel * 4U);
        result.rawVertices.push_back(raw);
    }
    result.indices = model.indices;

    const auto calculateTangents = [&result](std::vector<NativeSceneVertex>& destination) {
        std::vector<Vec3> tangents(destination.size());
        for (std::size_t offset = 0; offset < result.indices.size(); offset += 3U) {
            const auto i0 = result.indices[offset];
            const auto i1 = result.indices[offset + 1U];
            const auto i2 = result.indices[offset + 2U];
            if (i0 >= destination.size() || i1 >= destination.size() || i2 >= destination.size())
                throw std::invalid_argument("native scene face references a vertex outside the PMX model");
            const auto p0 = std::array{destination[i0].position[0], destination[i0].position[1],
                                       destination[i0].position[2]};
            const auto p1 = std::array{destination[i1].position[0], destination[i1].position[1],
                                       destination[i1].position[2]};
            const auto p2 = std::array{destination[i2].position[0], destination[i2].position[1],
                                       destination[i2].position[2]};
            const auto uv0 = std::array{destination[i0].uv[0], destination[i0].uv[1]};
            const auto uv1 = std::array{destination[i1].uv[0], destination[i1].uv[1]};
            const auto uv2 = std::array{destination[i2].uv[0], destination[i2].uv[1]};
            const auto edge1 = subtract(p1, p0);
            const auto edge2 = subtract(p2, p0);
            const auto duv1 = subtract(uv1, uv0);
            const auto duv2 = subtract(uv2, uv0);
            const auto determinant = duv1[0] * duv2[1] - duv1[1] * duv2[0];
            if (std::abs(determinant) <= 1e-8F)
                continue;
            const auto scale = 1.0F / determinant;
            const Vec3 tangent{(edge1[0] * duv2[1] - edge2[0] * duv1[1]) * scale,
                               (edge1[1] * duv2[1] - edge2[1] * duv1[1]) * scale,
                               (edge1[2] * duv2[1] - edge2[2] * duv1[1]) * scale};
            for (const auto index : {i0, i1, i2})
                for (std::size_t axis = 0; axis < 3; ++axis)
                    tangents[index][axis] += tangent[axis];
        }
        for (std::size_t index = 0; index < destination.size(); ++index) {
            const Vec3 fallback{1.0F, 0.0F, 0.0F};
            const auto tangent = normalize(tangents[index], fallback);
            std::copy(tangent.begin(), tangent.end(), std::begin(destination[index].tangent));
        }
    };
    calculateTangents(result.vertices);
    calculateTangents(result.rawVertices);

    result.materials.reserve(model.materials.size());
    for (std::size_t index = 0; index < model.materials.size(); ++index) {
        const auto* animated = animatedMaterials.empty() ? nullptr : &animatedMaterials[index];
        result.materials.push_back(makeNativeSceneMaterial(model.materials[index], animated));
    }

    const auto faceCount = model.indices.size() / 3U;
    result.faces.reserve(faceCount);
    result.faceWalker.resize(faceCount);
    std::size_t faceCursor = 0;
    for (std::size_t materialIndex = 0; materialIndex < model.materials.size(); ++materialIndex) {
        const auto materialFaceCount = model.materials[materialIndex].indexCount / 3U;
        if (materialFaceCount > faceCount - std::min(faceCursor, faceCount))
            throw std::invalid_argument("native scene material index ranges exceed the PMX index buffer");
        const auto start = faceCursor;
        for (std::size_t face = 0; face < materialFaceCount; ++face) {
            result.faces.push_back(static_cast<std::uint32_t>(materialIndex));
            const auto offset = (faceCursor + face) * 3U;
            const auto i0 = model.indices[offset];
            const auto i1 = model.indices[offset + 1U];
            const auto i2 = model.indices[offset + 2U];
            if (i0 >= result.vertices.size() || i1 >= result.vertices.size() || i2 >= result.vertices.size())
                throw std::invalid_argument("native scene face references a vertex outside the PMX model");
            const auto p0 = std::array{result.vertices[i0].position[0], result.vertices[i0].position[1],
                                       result.vertices[i0].position[2]};
            const auto p1 = std::array{result.vertices[i1].position[0], result.vertices[i1].position[1],
                                       result.vertices[i1].position[2]};
            const auto p2 = std::array{result.vertices[i2].position[0], result.vertices[i2].position[1],
                                       result.vertices[i2].position[2]};
            const auto areaVector = cross(subtract(p1, p0), subtract(p2, p0));
            const auto area = 0.5F * std::sqrt(std::max(dot(areaVector, areaVector), 0.0F));
            result.faceWalker[faceCursor + face] = {
                .pair = static_cast<std::uint32_t>(faceCursor + face),
                .probability = 1.0F,
                .pdf = materialFaceCount == 0 ? 0.0F : 1.0F / static_cast<float>(materialFaceCount),
            };
            if (face == 0)
                result.materialFaces.push_back({.start = static_cast<std::uint32_t>(start),
                                                .count = materialFaceCount,
                                                .totalArea = area,
                                                .padding = 0});
            else
                result.materialFaces.back().totalArea += area;
        }
        faceCursor += materialFaceCount;
        if (materialFaceCount == 0)
            result.materialFaces.push_back(
                {.start = static_cast<std::uint32_t>(start), .count = 0, .totalArea = 0.0F, .padding = 0});
    }
    if (faceCursor != faceCount)
        throw std::invalid_argument("native scene material index ranges do not cover the PMX index buffer");
    return result;
}

} // namespace dayo::graphics
