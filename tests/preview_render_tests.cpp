#include "core/model_probe.hpp"
#include "graphics/vulkan/vulkan_device.hpp"
#include "platform/window.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <numbers>
#include <span>

namespace {

using dayo::core::Float3;
using dayo::core::Float4;
using dayo::graphics::PreviewBoneTransform;
using dayo::graphics::PreviewDraw;
using dayo::graphics::PreviewMaterial;
using dayo::graphics::PreviewSkinningType;
using dayo::graphics::PreviewTexture;
using dayo::graphics::PreviewVertex;

Float3 add(const Float3& left, const Float3& right) {
    return {left[0] + right[0], left[1] + right[1], left[2] + right[2]};
}

Float3 mul(const Float3& value, float scale) {
    return {value[0] * scale, value[1] * scale, value[2] * scale};
}

Float3 rotate(const Float4& quaternion, const Float3& value) {
    const Float3 axis{quaternion[0], quaternion[1], quaternion[2]};
    const Float3 cross1{
        axis[1] * value[2] - axis[2] * value[1],
        axis[2] * value[0] - axis[0] * value[2],
        axis[0] * value[1] - axis[1] * value[0],
    };
    const Float3 cross2{
        axis[1] * cross1[2] - axis[2] * cross1[1],
        axis[2] * cross1[0] - axis[0] * cross1[2],
        axis[0] * cross1[1] - axis[1] * cross1[0],
    };
    return {
        value[0] + 2.0F * (cross2[0] + quaternion[3] * cross1[0]),
        value[1] + 2.0F * (cross2[1] + quaternion[3] * cross1[1]),
        value[2] + 2.0F * (cross2[2] + quaternion[3] * cross1[2]),
    };
}

Float4 slerp(const Float4& left, const Float4& right, float amount) {
    float cosine = left[0] * right[0] + left[1] * right[1] + left[2] * right[2] + left[3] * right[3];
    if (cosine > 0.9995F)
        return left;
    const float angle = std::acos(std::clamp(cosine, -1.0F, 1.0F));
    const float sine = std::sin(angle);
    const float leftWeight = std::sin((1.0F - amount) * angle) / sine;
    const float rightWeight = std::sin(amount * angle) / sine;
    return {
        left[0] * leftWeight + right[0] * rightWeight,
        left[1] * leftWeight + right[1] * rightWeight,
        left[2] * leftWeight + right[2] * rightWeight,
        left[3] * leftWeight + right[3] * rightWeight,
    };
}

std::array<PreviewVertex, 3> makeVertices(PreviewSkinningType type, bool reference) {
    std::array<PreviewVertex, 3> vertices{};
    const std::array positions{
        Float3{-0.45F, -0.35F, 0.0F},
        Float3{0.45F, -0.35F, 0.0F},
        Float3{0.0F, 0.45F, 0.0F},
    };
    const std::array<std::int32_t, 4> bones{0, 1, -1, -1};
    const std::array weights{0.5F, 0.5F, 0.0F, 0.0F};
    const Float3 sdefCenter{0.0F, 0.0F, 0.0F};
    const Float3 sdefHalfDelta{0.4F, 0.0F, 0.0F};
    for (std::size_t index = 0; index < vertices.size(); ++index)
        std::copy(positions[index].begin(), positions[index].end(), vertices[index].position);
    for (auto& vertex : vertices) {
        vertex.normal[2] = 1.0F;
        std::copy(bones.begin(), bones.end(), vertex.bones);
        std::copy(weights.begin(), weights.end(), vertex.weights);
        vertex.skinningType = static_cast<std::uint32_t>(type);
        vertex.gpuSkinning = reference ? 0U : 1U;
        std::copy(sdefCenter.begin(), sdefCenter.end(), vertex.sdefC);
        std::copy(sdefHalfDelta.begin(), sdefHalfDelta.end(), vertex.sdefHalfDelta);
    }
    if (!reference)
        return vertices;

    const Float4 identity{0.0F, 0.0F, 0.0F, 1.0F};
    const Float4 ninetyDegrees{0.0F, 0.0F, std::sin(std::numbers::pi_v<float> * 0.25F),
                               std::cos(std::numbers::pi_v<float> * 0.25F)};
    const Float4 fortyFiveDegrees = slerp(ninetyDegrees, identity, 0.5F);
    for (auto& vertex : vertices) {
        if (type == PreviewSkinningType::sdef) {
            const Float3 halfDelta{vertex.sdefHalfDelta[0], vertex.sdefHalfDelta[1], vertex.sdefHalfDelta[2]};
            const Float3 cr1 = mul(halfDelta, -0.5F);
            const Float3 cr0 = mul(halfDelta, 0.5F);
            const auto position =
                add(rotate(fortyFiveDegrees, {vertex.position[0], vertex.position[1], vertex.position[2]}),
                    mul(add(rotate(ninetyDegrees, cr1), cr0), 0.5F));
            const auto normal = rotate(fortyFiveDegrees, {vertex.normal[0], vertex.normal[1], vertex.normal[2]});
            std::copy(position.begin(), position.end(), vertex.position);
            std::copy(normal.begin(), normal.end(), vertex.normal);
        } else {
            const auto position =
                rotate(fortyFiveDegrees, {vertex.position[0], vertex.position[1], vertex.position[2]});
            const auto normal = rotate(fortyFiveDegrees, {vertex.normal[0], vertex.normal[1], vertex.normal[2]});
            std::copy(position.begin(), position.end(), vertex.position);
            std::copy(normal.begin(), normal.end(), vertex.normal);
        }
    }
    return vertices;
}

std::array<PreviewBoneTransform, 2> makeBones() {
    std::array<PreviewBoneTransform, 2> bones{};
    const std::array rotation{0.0F, 0.0F, std::sin(std::numbers::pi_v<float> * 0.25F),
                              std::cos(std::numbers::pi_v<float> * 0.25F)};
    std::copy(rotation.begin(), rotation.end(), bones[1].rotation);
    return bones;
}

bool imagesMatch(const dayo::core::ImageRgba8& left, const dayo::core::ImageRgba8& right) {
    if (left.width != right.width || left.height != right.height || left.pixels.size() != right.pixels.size())
        return false;
    std::size_t mismatched = 0;
    for (std::size_t index = 0; index < left.pixels.size(); ++index) {
        if (std::abs(static_cast<int>(left.pixels[index]) - static_cast<int>(right.pixels[index])) > 2)
            ++mismatched;
    }
    return mismatched <= left.pixels.size() / 100U;
}

dayo::core::ImageRgba8 renderCase(dayo::graphics::VulkanDevice& device, std::span<const PreviewVertex> vertices,
                                  std::span<const PreviewBoneTransform> bones) {
    const std::array<std::uint32_t, 3> indices{0, 2, 1};
    const std::array<PreviewMaterial, 1> materials{};
    const std::array<dayo::graphics::PreviewDraw, 1> draws{{{0, 3, 0}}};
    device.uploadPreviewMesh(vertices, indices);
    device.updatePreviewBones(bones);
    device.updatePreviewMaterials(materials);
    device.updatePreviewDraws(draws);
    return device.renderToImage({64, 64});
}

std::array<PreviewVertex, 3> makeFlatTriangle() {
    std::array<PreviewVertex, 3> vertices{};
    const std::array positions{
        Float3{-0.8F, -0.8F, 0.0F},
        Float3{0.8F, -0.8F, 0.0F},
        Float3{0.0F, 0.8F, 0.0F},
    };
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        std::copy(positions[index].begin(), positions[index].end(), vertices[index].position);
        vertices[index].normal[2] = 1.0F;
    }
    return vertices;
}

dayo::core::ImageRgba8 renderMaterial(dayo::graphics::VulkanDevice& device, std::span<const PreviewTexture> textures,
                                      PreviewMaterial material) {
    const auto vertices = makeFlatTriangle();
    const std::array<std::uint32_t, 3> indices{0, 2, 1};
    const std::array materials{material};
    const std::array<PreviewDraw, 1> draws{{{0, 3, 0}}};
    device.uploadPreviewTextures(textures);
    device.uploadPreviewMesh(vertices, indices);
    device.updatePreviewMaterials(materials);
    device.updatePreviewDraws(draws);
    return device.renderToImage({64, 64});
}

std::array<std::uint8_t, 4> centerPixel(const dayo::core::ImageRgba8& image) {
    const auto offset =
        (static_cast<std::size_t>(image.height) / 2U * image.width + static_cast<std::size_t>(image.width) / 2U) * 4U;
    if (image.pixels.size() < offset + 4U)
        return {};
    return {image.pixels[offset], image.pixels[offset + 1U], image.pixels[offset + 2U], image.pixels[offset + 3U]};
}

void resetPreviewScene(dayo::graphics::VulkanDevice& device) {
    dayo::graphics::PreviewScene scene;
    scene.cameraDistance = 3.0F;
    scene.backgroundEnabled = false;
    device.updatePreviewScene(scene);
}

bool missingSphereDoesNotAddWhite(dayo::graphics::VulkanDevice& device) {
    const std::array<std::uint8_t, 4> base{64, 32, 16, 255};
    const std::array<PreviewTexture, 1> textures{{
        {1, 1, std::span<const std::uint8_t>(base), false},
    }};
    PreviewMaterial material;
    material.textureSlot = 1;
    material.sphereMode = 0;
    material.sphereTextureSlot = 0;
    const auto image = renderMaterial(device, textures, material);
    const auto pixel = centerPixel(image);
    return pixel[0] < 200U && pixel[0] > pixel[1] && pixel[1] > pixel[2];
}

bool additiveSphereUsesTexture(dayo::graphics::VulkanDevice& device) {
    const std::array<std::uint8_t, 4> base{64, 64, 64, 255};
    const std::array<std::uint8_t, 4> redSphere{64, 0, 0, 255};
    const std::array<std::uint8_t, 4> blackSphere{0, 0, 0, 255};
    const std::array<PreviewTexture, 2> redTextures{{
        {1, 1, std::span<const std::uint8_t>(base), false},
        {1, 1, std::span<const std::uint8_t>(redSphere), false},
    }};
    const std::array<PreviewTexture, 2> blackTextures{{
        {1, 1, std::span<const std::uint8_t>(base), false},
        {1, 1, std::span<const std::uint8_t>(blackSphere), false},
    }};
    PreviewMaterial material;
    material.textureSlot = 1;
    material.sphereTextureSlot = 2;
    material.sphereMode = 2;
    const auto withTexture = centerPixel(renderMaterial(device, redTextures, material));
    const auto withBlackTexture = centerPixel(renderMaterial(device, blackTextures, material));
    return withTexture[0] > withBlackTexture[0] + 10U;
}

bool multiplySphereUsesTexture(dayo::graphics::VulkanDevice& device) {
    const std::array<std::uint8_t, 4> base{128, 128, 128, 255};
    const std::array<std::uint8_t, 4> whiteSphere{255, 255, 255, 255};
    const std::array<std::uint8_t, 4> blackSphere{0, 0, 0, 255};
    const std::array<PreviewTexture, 2> whiteTextures{{
        {1, 1, std::span<const std::uint8_t>(base), false},
        {1, 1, std::span<const std::uint8_t>(whiteSphere), false},
    }};
    const std::array<PreviewTexture, 2> blackTextures{{
        {1, 1, std::span<const std::uint8_t>(base), false},
        {1, 1, std::span<const std::uint8_t>(blackSphere), false},
    }};
    PreviewMaterial material;
    material.textureSlot = 1;
    material.sphereTextureSlot = 2;
    material.sphereMode = 1;
    const auto white = centerPixel(renderMaterial(device, whiteTextures, material));
    const auto black = centerPixel(renderMaterial(device, blackTextures, material));
    return static_cast<unsigned>(black[0]) + black[1] + black[2] + 30U <
           static_cast<unsigned>(white[0]) + white[1] + white[2];
}

bool sharedToonUsesTexture(dayo::graphics::VulkanDevice& device) {
    const std::array<std::uint8_t, 4> base{128, 128, 128, 255};
    const std::array<std::uint8_t, 4> whiteToon{255, 255, 255, 255};
    const std::array<std::uint8_t, 4> blackToon{0, 0, 0, 255};
    const std::array<PreviewTexture, 2> whiteTextures{{
        {1, 1, std::span<const std::uint8_t>(base), false},
        {1, 1, std::span<const std::uint8_t>(whiteToon), false},
    }};
    const std::array<PreviewTexture, 2> blackTextures{{
        {1, 1, std::span<const std::uint8_t>(base), false},
        {1, 1, std::span<const std::uint8_t>(blackToon), false},
    }};
    PreviewMaterial material;
    material.textureSlot = 1;
    material.toonTextureSlot = 2;
    material.toonMode = 1;
    const auto white = centerPixel(renderMaterial(device, whiteTextures, material));
    const auto black = centerPixel(renderMaterial(device, blackTextures, material));
    return static_cast<unsigned>(black[0]) + black[1] + black[2] + 30U <
           static_cast<unsigned>(white[0]) + white[1] + white[2];
}

bool alphaZeroIsDiscarded(dayo::graphics::VulkanDevice& device) {
    const std::array<std::uint8_t, 4> transparent{255, 0, 0, 0};
    const std::array<PreviewTexture, 1> textures{{
        {1, 1, std::span<const std::uint8_t>(transparent), true},
    }};
    PreviewMaterial material;
    material.textureSlot = 1;
    const auto pixel = centerPixel(renderMaterial(device, textures, material));
    return pixel[0] > 240U && pixel[1] > 240U && pixel[2] > 240U;
}

bool alpha098BecomesOpaque(dayo::graphics::VulkanDevice& device) {
    const std::array<std::uint8_t, 4> almostOpaque{255, 0, 0, 250};
    const std::array<std::uint8_t, 4> opaque{255, 0, 0, 255};
    const std::array<PreviewTexture, 1> almostOpaqueTextures{{
        {1, 1, std::span<const std::uint8_t>(almostOpaque), true},
    }};
    const std::array<PreviewTexture, 1> opaqueTextures{{
        {1, 1, std::span<const std::uint8_t>(opaque), false},
    }};
    PreviewMaterial material;
    material.textureSlot = 1;
    return imagesMatch(renderMaterial(device, almostOpaqueTextures, material),
                       renderMaterial(device, opaqueTextures, material));
}

bool lightColorAffectsDiffuse(dayo::graphics::VulkanDevice& device) {
    dayo::graphics::PreviewScene scene;
    scene.cameraDistance = 3.0F;
    scene.backgroundEnabled = false;
    scene.lightColor[0] = 0.0F;
    scene.lightColor[1] = 1.0F;
    scene.lightColor[2] = 0.0F;
    device.updatePreviewScene(scene);
    const std::array<std::uint8_t, 4> white{255, 255, 255, 255};
    const std::array<PreviewTexture, 1> textures{{
        {1, 1, std::span<const std::uint8_t>(white), false},
    }};
    PreviewMaterial material;
    material.textureSlot = 1;
    material.ambient[0] = material.ambient[1] = material.ambient[2] = 0.0F;
    material.specular[0] = material.specular[1] = material.specular[2] = 0.0F;
    material.toonMode = 2;
    const auto pixel = centerPixel(renderMaterial(device, textures, material));
    resetPreviewScene(device);
    return pixel[1] > 200U && pixel[0] < 30U && pixel[2] < 30U;
}

bool coplanarMaterialsUseStrictDepth(dayo::graphics::VulkanDevice& device) {
    std::array<PreviewVertex, 6> vertices{};
    const std::array positions{
        Float3{-0.8F, -0.8F, 0.0F},
        Float3{0.8F, -0.8F, 0.0F},
        Float3{0.0F, 0.8F, 0.0F},
    };
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        auto& vertex = vertices[index];
        const auto& position = positions[index % positions.size()];
        std::copy(position.begin(), position.end(), vertex.position);
        vertex.normal[2] = 1.0F;
    }
    const std::array<std::uint32_t, 6> indices{0, 2, 1, 3, 5, 4};
    const std::array<std::uint8_t, 4> red{255, 0, 0, 255};
    const std::array<std::uint8_t, 4> blue{0, 0, 255, 255};
    const std::array<PreviewTexture, 2> textures{{
        {1, 1, std::span<const std::uint8_t>(red), false},
        {1, 1, std::span<const std::uint8_t>(blue), false},
    }};
    std::array<PreviewMaterial, 2> materials{};
    materials[0].textureSlot = 1;
    materials[1].textureSlot = 2;
    const std::array<dayo::graphics::PreviewDraw, 2> draws{{{0, 3, 0}, {3, 3, 1}}};

    device.uploadPreviewTextures(textures);
    device.uploadPreviewMesh(vertices, indices);
    device.updatePreviewMaterials(materials);
    device.updatePreviewDraws(draws);
    const auto image = device.renderToImage({64, 64});
    const auto center =
        (static_cast<std::size_t>(image.height) / 2U * image.width + static_cast<std::size_t>(image.width) / 2U) * 4U;
    return image.pixels.size() >= center + 4U && image.pixels[center] > 200U && image.pixels[center + 1U] < 80U &&
           image.pixels[center + 2U] < 80U;
}

bool bindlessTextureSlotsSelectTable(dayo::graphics::VulkanDevice& device) {
    const std::array positions{
        Float3{-0.9F, -0.7F, 0.0F}, Float3{-0.05F, -0.7F, 0.0F}, Float3{-0.475F, 0.7F, 0.0F},
        Float3{0.05F, -0.7F, 0.0F}, Float3{0.9F, -0.7F, 0.0F},   Float3{0.475F, 0.7F, 0.0F},
    };
    std::array<PreviewVertex, 6> vertices{};
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        std::copy(positions[index].begin(), positions[index].end(), vertices[index].position);
        vertices[index].normal[2] = 1.0F;
    }
    const std::array<std::uint32_t, 6> indices{0, 2, 1, 3, 5, 4};
    const std::array<std::uint8_t, 4> red{255, 0, 0, 255};
    const std::array<std::uint8_t, 4> blue{0, 0, 255, 255};
    const std::array<PreviewTexture, 2> textures{{
        {1, 1, std::span<const std::uint8_t>(red), false},
        {1, 1, std::span<const std::uint8_t>(blue), false},
    }};
    std::array<PreviewMaterial, 2> materials{};
    materials[0].textureSlot = 1;
    materials[1].textureSlot = 2;
    const std::array<PreviewDraw, 2> draws{{{0, 3, 0}, {3, 3, 1}}};

    device.uploadPreviewTextures(textures);
    device.uploadPreviewMesh(vertices, indices);
    device.updatePreviewMaterials(materials);
    device.updatePreviewDraws(draws);
    const auto image = device.renderToImage({96, 64});
    bool sawRed = false;
    bool sawBlue = false;
    for (std::size_t index = 0; index + 3 < image.pixels.size(); index += 4) {
        sawRed =
            sawRed || (image.pixels[index] > 200U && image.pixels[index + 1U] < 80U && image.pixels[index + 2U] < 80U);
        sawBlue =
            sawBlue || (image.pixels[index] < 80U && image.pixels[index + 1U] < 80U && image.pixels[index + 2U] > 200U);
    }
    return sawRed && sawBlue;
}

bool multiMaterialDrawUsesIndirectMaterialIndex(dayo::graphics::VulkanDevice& device) {
    dayo::graphics::PreviewScene scene;
    scene.cameraDistance = 3.0F;
    scene.backgroundEnabled = false;
    device.updatePreviewScene(scene);

    std::array<PreviewVertex, 6> vertices{};
    const std::array positions{
        Float3{-0.9F, -0.7F, 0.0F}, Float3{-0.05F, -0.7F, 0.0F}, Float3{-0.475F, 0.7F, 0.0F},
        Float3{0.05F, -0.7F, 0.0F}, Float3{0.9F, -0.7F, 0.0F},   Float3{0.475F, 0.7F, 0.0F},
    };
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        std::copy(positions[index].begin(), positions[index].end(), vertices[index].position);
        vertices[index].normal[2] = 1.0F;
    }
    const std::array<std::uint32_t, 6> indices{0, 2, 1, 3, 5, 4};
    const std::array<std::uint8_t, 4> red{255, 0, 0, 255};
    const std::array<std::uint8_t, 4> blue{0, 0, 255, 255};
    const std::array<PreviewTexture, 2> textures{{
        {1, 1, std::span<const std::uint8_t>(red), false},
        {1, 1, std::span<const std::uint8_t>(blue), false},
    }};
    std::array<PreviewMaterial, 2> materials{};
    materials[0].textureSlot = 1;
    materials[1].textureSlot = 2;
    const std::array<dayo::graphics::PreviewDraw, 2> draws{{{0, 3, 0}, {3, 3, 1}}};

    device.uploadPreviewTextures(textures);
    device.uploadPreviewMesh(vertices, indices);
    device.updatePreviewMaterials(materials);
    device.updatePreviewDraws(draws);
    const auto image = device.renderToImage({96, 64});
    bool sawRed = false;
    bool sawBlue = false;
    for (std::size_t index = 0; index + 3 < image.pixels.size(); index += 4) {
        sawRed =
            sawRed || (image.pixels[index] > 200U && image.pixels[index + 1U] < 80U && image.pixels[index + 2U] < 80U);
        sawBlue =
            sawBlue || (image.pixels[index] < 80U && image.pixels[index + 1U] < 80U && image.pixels[index + 2U] > 200U);
    }
    return sawRed && sawBlue;
}

bool singleSidedMaterialsUseClockwiseFrontFaces(dayo::graphics::VulkanDevice& device) {
    std::array<PreviewVertex, 3> vertices{};
    const std::array positions{
        Float3{-0.8F, -0.8F, 0.0F},
        Float3{0.8F, -0.8F, 0.0F},
        Float3{0.0F, 0.8F, 0.0F},
    };
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        std::copy(positions[index].begin(), positions[index].end(), vertices[index].position);
        vertices[index].normal[2] = 1.0F;
    }
    const std::array<std::uint8_t, 4> red{255, 0, 0, 255};
    const std::array<PreviewTexture, 1> textures{{
        {1, 1, std::span<const std::uint8_t>(red), false},
    }};
    std::array<PreviewMaterial, 1> materials{};
    materials[0].textureSlot = 1;
    const std::array<PreviewDraw, 1> draws{{{0, 3, 0}}};
    device.uploadPreviewTextures(textures);
    device.updatePreviewMaterials(materials);
    device.updatePreviewDraws(draws);

    const std::array<std::uint32_t, 3> frontIndices{0, 2, 1};
    device.uploadPreviewMesh(vertices, frontIndices);
    const auto frontImage = device.renderToImage({64, 64});
    const auto center = (static_cast<std::size_t>(frontImage.height) / 2U * frontImage.width +
                         static_cast<std::size_t>(frontImage.width) / 2U) *
                        4U;
    const bool frontVisible = frontImage.pixels.size() >= center + 4U && frontImage.pixels[center] > 200U &&
                              frontImage.pixels[center + 1U] < 80U && frontImage.pixels[center + 2U] < 80U;

    const std::array<std::uint32_t, 3> backIndices{0, 1, 2};
    device.uploadPreviewMesh(vertices, backIndices);
    const auto backImage = device.renderToImage({64, 64});
    const bool backDiscarded = backImage.pixels.size() >= center + 4U && backImage.pixels[center] > 200U &&
                               backImage.pixels[center + 1U] > 200U && backImage.pixels[center + 2U] > 200U;
    materials[0].doubleSided = true;
    device.updatePreviewMaterials(materials);
    const auto doubleSidedImage = device.renderToImage({64, 64});
    const auto doubleSidedPixel = centerPixel(doubleSidedImage);
    const bool backVisibleWhenDoubleSided =
        doubleSidedPixel[0] > 200U && doubleSidedPixel[1] < 80U && doubleSidedPixel[2] < 80U;
    return frontVisible && backDiscarded && backVisibleWhenDoubleSided;
}

bool cloneDrawUsesInstanceCount(dayo::graphics::VulkanDevice& device) {
    dayo::graphics::PreviewScene scene;
    scene.cameraDistance = 5.0F;
    scene.backgroundEnabled = false;
    device.updatePreviewScene(scene);

    std::array<PreviewVertex, 3> vertices{};
    const std::array positions{
        Float3{-0.35F, -0.35F, 0.0F},
        Float3{0.35F, -0.35F, 0.0F},
        Float3{0.0F, 0.35F, 0.0F},
    };
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        std::copy(positions[index].begin(), positions[index].end(), vertices[index].position);
        vertices[index].normal[2] = 1.0F;
    }
    const std::array<std::uint32_t, 3> indices{0, 2, 1};
    const std::array<std::uint8_t, 4> red{255, 0, 0, 255};
    const std::array<PreviewTexture, 1> textures{{
        {1, 1, std::span<const std::uint8_t>(red), false},
    }};
    std::array<PreviewMaterial, 1> materials{};
    materials[0].textureSlot = 1;
    std::array<PreviewDraw, 1> draws{{{0, 3, 0, 1}}};

    device.uploadPreviewTextures(textures);
    device.uploadPreviewMesh(vertices, indices);
    device.updatePreviewMaterials(materials);
    device.updatePreviewDraws(draws);
    const auto singleImage = device.renderToImage({96, 64});
    draws[0].instanceCount = 2;
    device.updatePreviewDraws(draws);
    const auto cloneImage = device.renderToImage({96, 64});
    std::size_t differentPixels = 0;
    for (std::size_t index = 0; index < singleImage.pixels.size(); ++index) {
        if (singleImage.pixels[index] != cloneImage.pixels[index])
            ++differentPixels;
    }
    return differentPixels > 64;
}

bool staticPreviewFallsBackToDynamicVertices(dayo::graphics::VulkanDevice& device) {
    dayo::graphics::PreviewScene scene;
    scene.cameraDistance = 3.0F;
    scene.backgroundEnabled = false;
    device.updatePreviewScene(scene);

    std::array<PreviewVertex, 3> vertices{};
    const std::array positions{
        Float3{-0.45F, -0.35F, 0.0F},
        Float3{0.45F, -0.35F, 0.0F},
        Float3{0.0F, 0.45F, 0.0F},
    };
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        std::copy(positions[index].begin(), positions[index].end(), vertices[index].position);
        vertices[index].normal[2] = 1.0F;
    }
    const std::array<std::uint32_t, 3> indices{0, 2, 1};
    const std::array<std::uint8_t, 4> red{255, 0, 0, 255};
    const std::array<PreviewTexture, 1> textures{{
        {1, 1, std::span<const std::uint8_t>(red), false},
    }};
    std::array<PreviewMaterial, 1> materials{};
    materials[0].textureSlot = 1;
    const std::array<PreviewDraw, 1> draws{{{0, 3, 0}}};

    device.uploadPreviewTextures(textures);
    device.uploadPreviewMesh(vertices, indices);
    device.updatePreviewMaterials(materials);
    device.updatePreviewDraws(draws);
    const auto initialImage = device.renderToImage({64, 64});

    for (auto& vertex : vertices)
        vertex.position[0] += 5.0F;
    device.updatePreviewVertices(vertices);
    const auto movedImage = device.renderToImage({64, 64});
    const auto center =
        (static_cast<std::size_t>(initialImage.height) / 2U * initialImage.width + initialImage.width / 2U) * 4U;
    const bool initialVisible = initialImage.pixels.size() >= center + 4U && initialImage.pixels[center] > 200U &&
                                initialImage.pixels[center + 1U] < 80U && initialImage.pixels[center + 2U] < 80U;
    const bool movedAway = movedImage.pixels.size() >= center + 4U && movedImage.pixels[center] > 200U &&
                           movedImage.pixels[center + 1U] > 200U && movedImage.pixels[center + 2U] > 200U;
    return initialVisible && movedAway;
}

bool runCase(dayo::graphics::VulkanDevice& device, PreviewSkinningType type) {
    const auto bones = makeBones();
    const auto gpuVertices = makeVertices(type, false);
    const auto referenceVertices = makeVertices(type, true);
    const auto gpuImage = renderCase(device, gpuVertices, bones);
    const auto referenceImage = renderCase(device, referenceVertices, bones);
    return imagesMatch(gpuImage, referenceImage);
}

bool orthographicZoom(dayo::graphics::VulkanDevice& device) {
    dayo::graphics::PreviewScene scene;
    scene.backgroundEnabled = false;
    scene.cameraDistance = 3.0F;
    scene.perspective = false;
    device.updatePreviewScene(scene);
    const auto nearImage = device.renderToImage({64, 64});
    scene.cameraDistance = 6.0F;
    device.updatePreviewScene(scene);
    const auto farImage = device.renderToImage({64, 64});
    scene.verticalFovRadians = 0.4F;
    device.updatePreviewScene(scene);
    const auto narrowImage = device.renderToImage({64, 64});
    return !imagesMatch(nearImage, farImage) && !imagesMatch(farImage, narrowImage);
}

bool sphereAlphaDoesNotHideMaterial(dayo::graphics::VulkanDevice& device) {
    std::array<PreviewMaterial, 1> materials{};
    materials[0].sphereTextureSlot = 1;
    materials[0].sphereMode = 1;
    device.updatePreviewMaterials(materials);
    std::array<std::uint8_t, 4> pixel{255, 0, 0, 255};
    const std::array<PreviewTexture, 1> textures{{{1, 1, pixel, false}}};
    device.uploadPreviewTextures(textures);
    const auto opaque = device.renderToImage({64, 64});
    pixel[3] = 0;
    device.uploadPreviewTextures(textures);
    const auto transparent = device.renderToImage({64, 64});
    return imagesMatch(opaque, transparent);
}

} // namespace

int main() {
    try {
        const auto window = dayo::platform::createWindow({"preview shader test", 64, 64, true});
        dayo::graphics::VulkanDevice device(*window, false);
        dayo::graphics::PreviewScene scene;
        scene.cameraDistance = 3.0F;
        scene.backgroundEnabled = false;
        device.updatePreviewScene(scene);
        if (!runCase(device, PreviewSkinningType::sdef)) {
            std::cerr << "FAIL: GPU SDEF output differs from reference rendering\n";
            return 1;
        }
        if (!runCase(device, PreviewSkinningType::qdef)) {
            std::cerr << "FAIL: GPU QDEF output differs from reference rendering\n";
            return 1;
        }
        if (!coplanarMaterialsUseStrictDepth(device)) {
            std::cerr << "FAIL: coplanar preview materials did not preserve the first material\n";
            return 1;
        }
        if (!bindlessTextureSlotsSelectTable(device)) {
            std::cerr << "FAIL: bindless preview texture slots did not select the correct textures\n";
            return 1;
        }
        if (!multiMaterialDrawUsesIndirectMaterialIndex(device)) {
            std::cerr << "FAIL: preview material index was not preserved across indirect draws\n";
            return 1;
        }
        if (!singleSidedMaterialsUseClockwiseFrontFaces(device)) {
            std::cerr << "FAIL: single-sided preview materials used the wrong front-face winding\n";
            return 1;
        }
        if (!cloneDrawUsesInstanceCount(device)) {
            std::cerr << "FAIL: preview clone draw did not use instancing\n";
            return 1;
        }
        if (!staticPreviewFallsBackToDynamicVertices(device)) {
            std::cerr << "FAIL: device-local preview vertices did not switch to dynamic storage\n";
            return 1;
        }
        const std::array<std::uint32_t, 3> front{0, 2, 1};
        const auto vertices = makeVertices(PreviewSkinningType::sdef, true);
        device.uploadPreviewMesh(vertices, front);
        if (!orthographicZoom(device) || !sphereAlphaDoesNotHideMaterial(device)) {
            std::cerr << "FAIL: orthographic zoom or sphere alpha regression\n";
            return 1;
        }
        resetPreviewScene(device);
        if (!missingSphereDoesNotAddWhite(device)) {
            std::cerr << "FAIL: disabled sphere map changed the base material color\n";
            return 1;
        }
        if (!additiveSphereUsesTexture(device)) {
            std::cerr << "FAIL: additive sphere map did not affect the material\n";
            return 1;
        }
        if (!multiplySphereUsesTexture(device)) {
            std::cerr << "FAIL: multiply sphere map did not affect the material\n";
            return 1;
        }
        if (!sharedToonUsesTexture(device)) {
            std::cerr << "FAIL: shared toon texture did not affect the material\n";
            return 1;
        }
        if (!alphaZeroIsDiscarded(device)) {
            std::cerr << "FAIL: alpha-zero texture fragment was not discarded\n";
            return 1;
        }
        if (!alpha098BecomesOpaque(device)) {
            std::cerr << "FAIL: alpha threshold did not make the fragment opaque\n";
            return 1;
        }
        if (!lightColorAffectsDiffuse(device)) {
            std::cerr << "FAIL: VMD light color did not affect diffuse shading\n";
            return 1;
        }
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: preview shader rendering: " << exception.what() << '\n';
        return 1;
    }
    return 0;
}
