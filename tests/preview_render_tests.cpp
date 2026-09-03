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
using dayo::graphics::PreviewMaterial;
using dayo::graphics::PreviewSkinningType;
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
    const std::array<std::uint32_t, 3> indices{0, 1, 2};
    const std::array<PreviewMaterial, 1> materials{};
    const std::array<dayo::graphics::PreviewDraw, 1> draws{{{0, 3, 0, {0.0F, 0.0F, 0.0F}}}};
    device.uploadPreviewMesh(vertices, indices);
    device.updatePreviewBones(bones);
    device.updatePreviewMaterials(materials);
    device.updatePreviewDraws(draws);
    return device.renderToImage({64, 64});
}

bool runCase(dayo::graphics::VulkanDevice& device, PreviewSkinningType type) {
    const auto bones = makeBones();
    const auto gpuVertices = makeVertices(type, false);
    const auto referenceVertices = makeVertices(type, true);
    const auto gpuImage = renderCase(device, gpuVertices, bones);
    const auto referenceImage = renderCase(device, referenceVertices, bones);
    return imagesMatch(gpuImage, referenceImage);
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
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: preview shader rendering: " << exception.what() << '\n';
        return 1;
    }
    return 0;
}
